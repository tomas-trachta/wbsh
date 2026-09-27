/**
 * @file repl.cpp
 * @brief Interactive loop: console setup, prompt expansion, history
 *        expansion, and the read-parse-execute cycle.
 */

#include "repl.h"

#include "interrupt.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>

#  include <dwmapi.h>
#  include <io.h>
#  pragma comment(lib, "dwmapi.lib")
// 20 since Win10 1903; defined in newer SDKs but we don't rely on the SDK version.
#  ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#    define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#  endif
// 19 was the pre-1903 value; attempted as a fallback on older builds.
#  ifndef DWMWA_USE_IMMERSIVE_DARK_MODE_OLD
#    define DWMWA_USE_IMMERSIVE_DARK_MODE_OLD 19
#  endif
#  ifndef DWMWA_CAPTION_COLOR
#    define DWMWA_CAPTION_COLOR 35
#  endif
#  ifndef DWMWA_BORDER_COLOR
#    define DWMWA_BORDER_COLOR 34
#  endif
#endif /* _WIN32 */

#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ast.h"
#include "environment.h"
#include "executor.h"
#include "lexer.h"
#include "lineedit.h"
#include "numparse.h"
#include "parser.h"
#include "pathconv.h"
#include "setup.h"
#include "strscan.h"

#if !defined(WBSH_VERSION_MAJOR) || !defined(WBSH_VERSION_MINOR) || !defined(WBSH_VERSION_PATCH)
#define WBSH_VERSION_MAJOR 0
#define WBSH_VERSION_MINOR 0
#define WBSH_VERSION_PATCH 0
#endif
#define WBSH_VSTR_(x) #x
#define WBSH_VSTR(x)  WBSH_VSTR_(x)
#define WBSH_VERSION_STR \
	WBSH_VSTR(WBSH_VERSION_MAJOR) "." \
	WBSH_VSTR(WBSH_VERSION_MINOR) "." \
	WBSH_VSTR(WBSH_VERSION_PATCH)

namespace wbsh {

	namespace fs = std::filesystem;

	static const int kSigintStatus = 130;
	static const char* const kResetStyle = "\x1b[0m";
	static const char* const kBranchStyle = "\x1b[33;1m";

	struct ReplState {
		std::string buffer;
		bool        waiting_for_more = false;
		bool        color_ok = false;
		std::string histfile;
#ifdef _WIN32
		HANDLE h_in       = INVALID_HANDLE_VALUE;
		HANDLE h_out      = INVALID_HANDLE_VALUE;
		DWORD  good_in_mode  = 0;
		DWORD  good_out_mode = 0;
		int    last_cols  = 0;
		int    last_lines = 0;
#endif
	};

	static bool stderrIsTty() {
#ifdef _WIN32
		return _isatty(_fileno(stderr)) != 0;
#else
		return false;
#endif /* _WIN32 */
	}

	static bool stdoutIsTty() {
#ifdef _WIN32
		return _isatty(_fileno(stdout)) != 0;
#else
		return false;
#endif /* _WIN32 */
	}

#ifdef _WIN32
	static void setupConsoleWindow() {
		::SetConsoleTitleW(L"wbsh");
		const HWND hwnd = ::GetConsoleWindow();
		if (hwnd == nullptr) return;

		BOOL dark = TRUE;
		if (FAILED(::DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
		                                   &dark, sizeof(dark)))) {
			::DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_OLD,
			                        &dark, sizeof(dark));
		}

		// Win11-only: paint the caption + frame to match the shell's dark
		// theme. Older systems return E_INVALIDARG, which we silently ignore.
		COLORREF caption = RGB(0x14, 0x14, 0x18);
		::DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
		COLORREF border = RGB(0x33, 0x33, 0x3a);
		::DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
	}

	static void setupConsoleFont(HANDLE h_out) {
		if (h_out == INVALID_HANDLE_VALUE) return;

		CONSOLE_FONT_INFOEX font{};
		font.cbSize = sizeof(font);
		font.nFont = 0;
		font.dwFontSize.X = 0;
		font.dwFontSize.Y = 16;
		font.FontFamily = FF_DONTCARE;
		font.FontWeight = FW_NORMAL;

		const wchar_t* faces[] = { L"Cascadia Mono", L"Cascadia Code", L"Consolas" };
		for (const wchar_t* face : faces) {
			wcsncpy_s(font.FaceName, face, _TRUNCATE);
			if (::SetCurrentConsoleFontEx(h_out, FALSE, &font)) return;
		}
	}
#endif /* _WIN32 */

	struct GitInfo {
		std::string branch;
		std::string state;
	};

	static const char* gitStateColor(const std::string& state) {
		if (state == "merging" ||
		    state == "rebasing" ||
		    state == "cherry-picking" ||
		    state == "reverting")  return "\x1b[31;1m";
		if (state == "bisecting")  return "\x1b[35;1m";
		if (state == "unstaged")   return "\x1b[31m";
		if (state == "staged")     return "\x1b[32m";
		if (state == "untracked")  return "\x1b[36m";
		return "";
	}

	static std::string detectGitOpState(const fs::path& git_dir) {
		std::error_code ec;
		if (fs::exists(git_dir / "MERGE_HEAD", ec))        return "merging";
		if (fs::exists(git_dir / "rebase-merge", ec) ||
		    fs::exists(git_dir / "rebase-apply", ec))      return "rebasing";
		if (fs::exists(git_dir / "CHERRY_PICK_HEAD", ec))  return "cherry-picking";
		if (fs::exists(git_dir / "REVERT_HEAD", ec))       return "reverting";
		if (fs::exists(git_dir / "BISECT_LOG", ec))        return "bisecting";
		return {};
	}

	static bool gitDirtyCheckDisabled() {
		const char* off = std::getenv("WBSH_GIT_NO_DIRTY");
		return off != nullptr && *off != '\0' && *off != '0';
	}

	static FILE* openGitStatusPipe() {
#ifdef _WIN32
		return _popen("git --no-optional-locks status --porcelain 2>NUL", "r");
#else
		return popen("git --no-optional-locks status --porcelain 2>/dev/null", "r");
#endif
	}

	static void closeGitStatusPipe(FILE* pipe) {
#ifdef _WIN32
		_pclose(pipe);
#else
		pclose(pipe);
#endif
	}

	static std::string detectGitDirtyState() {
		if (gitDirtyCheckDisabled()) return {};

		FILE* pipe = openGitStatusPipe();
		if (pipe == nullptr) return {};

		bool unstaged = false;
		bool staged = false;
		bool untracked = false;
		char line[512];
		while (std::fgets(line, sizeof(line), pipe) != nullptr) {
			if (line[0] == '?' && line[1] == '?') {
				untracked = true;
			} else {
				if (line[0] != ' ' && line[0] != '?') staged = true;
				if (line[1] != ' ' && line[1] != '?') unstaged = true;
			}

			if (unstaged) break;
		}

		closeGitStatusPipe(pipe);
		if (unstaged)  return "unstaged";
		if (staged)    return "staged";
		if (untracked) return "untracked";
		return {};
	}

	static void stripLineEnding(std::string& line) {
		while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
	}

	static std::string branchFromHeadLine(std::string line) {
		const std::string ref_prefix = "ref: ";
		if (line.compare(0, ref_prefix.size(), ref_prefix) != 0) {
			if (line.size() >= 7) line.resize(7);
			return line + " (detached)";
		}

		const std::string ref = line.substr(ref_prefix.size());
		const std::string heads = "refs/heads/";
		if (ref.compare(0, heads.size(), heads) == 0) return ref.substr(heads.size());

		return ref;
	}

	static GitInfo detectGitInfo() {
		GitInfo info;
		const fs::path git_dir = findGitDir();
		if (git_dir.empty()) return info;

		std::ifstream head(git_dir / "HEAD");
		std::string line;
		if (!std::getline(head, line)) return info;

		stripLineEnding(line);
		info.branch = branchFromHeadLine(line);

		info.state = detectGitOpState(git_dir);
		if (info.state.empty()) info.state = detectGitDirtyState();
		return info;
	}

	static std::string currentCwdAsUtf8() {
		std::error_code ec;
		const fs::path here = fs::current_path(ec);
		return ec ? std::string(".") : pathToUtf8(here);
	}

	static std::string promptUser(const Environment& env) {
		std::string user = env.get("USER");
		if (user.empty()) user = env.get("USERNAME");
		return user;
	}

	static std::string promptHost(const Environment& env, char form) {
		std::string host = env.get("HOSTNAME");
		if (host.empty()) host = env.get("COMPUTERNAME");
		if (form == 'h') {
			const std::size_t dot = host.find('.');
			if (dot != std::string::npos) host.resize(dot);
		}

		return host;
	}

	static bool underHome(const std::string& posix, const std::string& home) {
		return !home.empty() && posix.size() >= home.size()
		    && posix.compare(0, home.size(), home) == 0
		    && (posix.size() == home.size() || posix[home.size()] == '/');
	}

	static std::string promptCwdHome(const Environment& env, const PathConv& conv) {
		const std::string posix = conv.toPosix(currentCwdAsUtf8());
		const std::string home = env.get("HOME");
		if (underHome(posix, home)) return "~" + posix.substr(home.size());

		return posix;
	}

	static std::string promptCwdBasename(const PathConv& conv) {
		const std::string posix = conv.toPosix(currentCwdAsUtf8());
		const std::size_t slash = posix.rfind('/');
		return slash == std::string::npos ? posix : posix.substr(slash + 1);
	}

	static std::string plainGitSegment(const GitInfo& info) {
		std::string out = " (" + info.branch;
		if (!info.state.empty()) out += " | " + info.state;
		out += ")";
		return out;
	}

	static std::string coloredGitSegment(const GitInfo& info) {
		std::string out = " ";
		out += kBranchStyle;
		out += "(" + info.branch;
		if (!info.state.empty()) {
			out += " | ";
			const char* state_color = gitStateColor(info.state);
			if (*state_color != '\0') out += state_color;
			out += info.state;
			if (*state_color != '\0') out += kBranchStyle;
		}

		out += ")";
		out += kResetStyle;
		return out;
	}

	static std::string promptGitBranch() {
		const GitInfo info = detectGitInfo();
		if (info.branch.empty()) return {};
		if (!stdoutIsTty()) return plainGitSegment(info);

		return coloredGitSegment(info);
	}

	static std::string promptTimeHms() {
		const std::time_t now = std::time(nullptr);
		char text[16];
		std::strftime(text, sizeof(text), "%H:%M:%S", std::localtime(&now));
		return text;
	}

	static std::string expandPromptEscape(char code, const Environment& env,
	                                      const PathConv& conv) {
		switch (code) {
		case 'n':  return "\n";
		case 'r':  return "\r";
		case 'a':  return "\a";
		case 'e':  return "\x1b";
		case '\\': return "\\";
		case '$':  return "$";
		case 's':  return "wbsh";
		case '[':  return {};
		case ']':  return {};
		case 'u':  return promptUser(env);
		case 'h':
		case 'H':  return promptHost(env, code);
		case 'w':  return promptCwdHome(env, conv);
		case 'W':  return promptCwdBasename(conv);
		case 'g':  return promptGitBranch();
		case 't':  return promptTimeHms();
		default:   return std::string{ '\\', code };
		}
	}

	static std::string expandPrompt(const std::string& ps, const Environment& env,
	                                const PathConv& conv) {
		std::string out;
		for (std::size_t i = 0; i < ps.size(); ++i) {
			if (ps[i] != '\\' || i + 1 >= ps.size()) {
				out.push_back(ps[i]);
				continue;
			}

			out += expandPromptEscape(ps[++i], env, conv);
		}

		return out;
	}

	static bool expandHistoryCarat(const std::string& line,
	                               const std::vector<std::string>& history,
	                               std::string& expanded) {
		StrScan in(line);
		std::string old_text;
		if (history.empty() || !in.consume("^") || !in.readUpTo('^', old_text)) {
			expanded = line;
			return false;
		}

		std::string new_text;
		if (!in.readUpTo('^', new_text)) new_text = in.rest();

		const std::string& base = history.back();
		const std::size_t found = base.find(old_text);
		if (found == std::string::npos) {
			expanded = line;
			return false;
		}

		expanded = base.substr(0, found) + new_text + base.substr(found + old_text.size());
		return true;
	}

	static bool isDigitChar(char letter) {
		return std::isdigit(static_cast<unsigned char>(letter)) != 0;
	}

	static bool isHistoryWordChar(char letter) {
		return std::isalnum(static_cast<unsigned char>(letter)) != 0
		    || letter == '_' || letter == '-';
	}

	static std::size_t scanDigits(const std::string& line, std::size_t from) {
		std::size_t end = from;
		while (end < line.size() && isDigitChar(line[end])) ++end;
		return end;
	}

	static std::size_t scanHistoryWord(const std::string& line, std::size_t from) {
		std::size_t end = from;
		while (end < line.size() && isHistoryWordChar(line[end])) ++end;
		return end;
	}

	// `!n` counts from the first entry, `!-n` from the last.
	static bool historyByNumber(const std::vector<std::string>& history,
	                            const std::string& digits, bool from_end, std::string& sub) {
		int number = 0;
		if (!parseInt(digits, number)) return false;
		if (number <= 0 || static_cast<std::size_t>(number) > history.size()) return false;

		sub = from_end ? history[history.size() - number] : history[number - 1];
		return true;
	}

	static bool historyByPrefix(const std::vector<std::string>& history,
	                            const std::string& prefix, std::string& sub) {
		for (auto it = history.rbegin(); it != history.rend(); ++it) {
			if (it->compare(0, prefix.size(), prefix) != 0) continue;

			sub = *it;
			return true;
		}

		return false;
	}

	static bool resolveHistoryBang(const std::string& line, std::size_t i,
	                               const std::vector<std::string>& history,
	                               std::string& sub, std::size_t& advance) {
		const char next = line[i + 1];
		if (next == '!') {
			if (history.empty()) return false;

			sub = history.back();
			advance = 2;
			return true;
		}

		if (next == '-' && i + 2 < line.size() && isDigitChar(line[i + 2])) {
			const std::size_t end = scanDigits(line, i + 2);
			advance = end - i;
			return historyByNumber(history, line.substr(i + 2, end - i - 2), true, sub);
		}

		if (isDigitChar(next)) {
			const std::size_t end = scanDigits(line, i + 1);
			advance = end - i;
			return historyByNumber(history, line.substr(i + 1, end - i - 1), false, sub);
		}

		if (std::isalpha(static_cast<unsigned char>(next)) != 0 || next == '_') {
			const std::size_t end = scanHistoryWord(line, i + 1);
			advance = end - i;
			return historyByPrefix(history, line.substr(i + 1, end - i - 1), sub);
		}

		return false;
	}

	static bool bangIsLiteral(char next) {
		return next == ' ' || next == '\t' || next == '\n' || next == '='
		    || next == '"' || next == '\\';
	}

	static bool expandHistory(const std::string& line,
	                          const std::vector<std::string>& history,
	                          std::string& expanded) {
		expanded.clear();
		if (line.empty()) {
			expanded = line;
			return false;
		}

		if (line[0] == '^') return expandHistoryCarat(line, history, expanded);

		bool any = false;
		for (std::size_t i = 0; i < line.size(); ++i) {
			const char letter = line[i];
			if (letter != '!' || i + 1 >= line.size() || bangIsLiteral(line[i + 1])) {
				expanded.push_back(letter);
				continue;
			}

			std::string sub;
			std::size_t advance = 0;
			if (!resolveHistoryBang(line, i, history, sub, advance)) {
				expanded.push_back(letter);
				continue;
			}

			expanded += sub;
			i += advance - 1;
			any = true;
		}

		return any;
	}

	static bool parseErrorLooksIncomplete(const std::string& message) {
		return message.find("expected `")        != std::string::npos
		    || message.find("expected pipeline") != std::string::npos
		    || message.find("expected command")  != std::string::npos;
	}

	static bool looksIncomplete(const std::vector<LexError>& lex_errs,
	                            const std::vector<ParseError>& parse_errs) {
		for (const auto& error : lex_errs) {
			if (error.message.find("unterminated") != std::string::npos) return true;
		}

		for (const auto& error : parse_errs) {
			if (parseErrorLooksIncomplete(error.message)) return true;
		}

		return false;
	}

#ifdef _WIN32
	// The captured "good" modes are re-applied before every prompt:
	// externals (pagers, vim) corrupt console state and don't restore it.
	static void captureConsoleModes(ReplState& state) {
		state.h_out = ::GetStdHandle(STD_OUTPUT_HANDLE);
		state.h_in  = ::GetStdHandle(STD_INPUT_HANDLE);

		if (state.h_out != INVALID_HANDLE_VALUE
		    && ::GetConsoleMode(state.h_out, &state.good_out_mode)) {
			state.good_out_mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
			state.good_out_mode |= ENABLE_PROCESSED_OUTPUT;
			state.good_out_mode |= ENABLE_WRAP_AT_EOL_OUTPUT;
			state.color_ok = ::SetConsoleMode(state.h_out, state.good_out_mode) != 0;
		}

		if (state.h_in != INVALID_HANDLE_VALUE
		    && ::GetConsoleMode(state.h_in, &state.good_in_mode)) {
			state.good_in_mode |= ENABLE_LINE_INPUT;
			state.good_in_mode |= ENABLE_ECHO_INPUT;
			state.good_in_mode |= ENABLE_PROCESSED_INPUT;
			state.good_in_mode |= ENABLE_EXTENDED_FLAGS;
			::SetConsoleMode(state.h_in, state.good_in_mode);
		}
	}
#endif /* _WIN32 */

	static void initConsoleAndSignals(ReplState& state) {
#ifdef _WIN32
		installCtrlCHandler();
		::SetConsoleOutputCP(CP_UTF8);
		captureConsoleModes(state);
		setupConsoleWindow();
		setupConsoleFont(state.h_out);
#else
		state.color_ok = true;
#endif /* _WIN32 */
	}

	static void initShellDefaults(Environment& env, const ReplState& state) {
		if (env.get("PS1").empty()) {
			if (state.color_ok) {
				env.set("PS1",
					"\\[\\e[32;1m\\]\\u@\\h\\[\\e[0m\\] "
					"\\[\\e[36;1m\\]\\w\\[\\e[0m\\]\\g\\$ ");
			} else {
				env.set("PS1", "\\u@\\h \\w\\g\\$ ");
			}
		}

		if (env.get("PS2").empty()) env.set("PS2", "> ");
	}

	// Semantic marks (OSC 633, as VS Code and others use) tell a terminal
	// where a prompt starts, where a command's output begins, and how it
	// ended. Terminals that do not know them ignore the sequence, so this
	// costs nothing elsewhere.
	static void emitShellMark(const ReplState& state, const std::string& body) {
		if (!state.color_ok) return;

		std::fputs(("\x1b]633;" + body + "\x07").c_str(), stdout);
		std::fflush(stdout);
	}

	static std::string percentEncodePath(const std::string& path) {
		static const char* kHexDigits = "0123456789ABCDEF";

		std::string encoded;
		for (unsigned char letter : path) {
			const bool plain = std::isalnum(letter) != 0 || letter == '/' || letter == '-'
				|| letter == '_' || letter == '.' || letter == '~' || letter == ':';
			if (plain) {
				encoded.push_back(static_cast<char>(letter));
				continue;
			}

			encoded.push_back('%');
			encoded.push_back(kHexDigits[letter >> 4]);
			encoded.push_back(kHexDigits[letter & 0x0F]);
		}

		return encoded;
	}

	// OSC 7 reports the working directory, so a terminal can show it without
	// parsing the prompt.
	static void emitWorkingDirectory(const ReplState& state) {
		if (!state.color_ok) return;

		std::error_code ec;
		const fs::path here = fs::current_path(ec);
		if (ec) return;

		std::string path = pathToUtf8(here);
		for (char& letter : path) {
			if (letter == '\\') letter = '/';
		}

		std::fputs(("\x1b]7;file:///" + percentEncodePath(path) + "\x07").c_str(), stdout);
		std::fflush(stdout);
	}

	static void printBanner(const ReplState& state) {
		if (state.color_ok) {
			std::fputs(
				"\x1b[36;1m wbsh " WBSH_VERSION_STR " \x1b[0m"
				"\x1b[2m— a Bash-compatible shell for Windows\x1b[0m\n"
				"\x1b[2m   type `\x1b[0;33mexit\x1b[2m` or press "
				"`\x1b[0;33mCtrl-D\x1b[2m` to quit\x1b[0m\n",
				stdout);
		} else {
			std::fputs(
				"wbsh " WBSH_VERSION_STR " -- a Bash-compatible shell for Windows\n"
				"   type `exit` or press Ctrl-D to quit\n",
				stdout);
		}
	}

	static void initHistFile(Environment& env, Executor& exec, ReplState& state) {
		const std::string home_dir = env.get("HOME");
		state.histfile = env.get("HISTFILE");
		if (state.histfile.empty() && !home_dir.empty()) {
			state.histfile = home_dir + "/.wbsh_history";
			env.set("HISTFILE", state.histfile);
		}

		if (!state.histfile.empty()) {
			exec.loadHistoryFromFile(exec.pathConv().toWin32(state.histfile));
		}
	}

	static void sourceWbshrc(const Environment& env, Executor& exec) {
		const std::string home_dir = env.get("HOME");
		if (home_dir.empty()) return;

		const std::string rcfile = home_dir + "/.wbshrc";
		const fs::path native = utf8ToPath(exec.pathConv().toWin32(rcfile));
		std::error_code ec;
		if (!fs::exists(native, ec)) return;

		std::ifstream file(native, std::ios::binary);
		if (!file) return;

		std::stringstream contents;
		contents << file.rdbuf();
		exec.executeText(contents.str(), rcfile);
	}

	// A host terminal can ask for one command to run once the session is
	// up -- wbshterm uses it for its startup panel. Kept separate from
	// .wbshrc so it cannot be lost by editing that file.
	static void runInitCommand(const Environment& env, Executor& exec) {
		const std::string command = env.get("WBSH_INIT_COMMAND");
		if (command.empty()) return;

		exec.executeText(command, "WBSH_INIT_COMMAND");
	}

	static void saveHistory(Executor& exec, const ReplState& state) {
		if (!state.histfile.empty()) {
			exec.saveHistoryToFile(exec.pathConv().toWin32(state.histfile));
		}
	}

#ifdef _WIN32
	static void restoreConsoleModes(const ReplState& state) {
		if (state.h_out != INVALID_HANDLE_VALUE) ::SetConsoleMode(state.h_out, state.good_out_mode);
		if (state.h_in  != INVALID_HANDLE_VALUE) ::SetConsoleMode(state.h_in,  state.good_in_mode);
	}

	static void runTrap(Executor& exec, const char* signal_name, const char* origin) {
		if (!exec.hasTrap(signal_name)) return;

		const std::string action = exec.trapAction(signal_name);
		exec.executeText(action, origin);
	}

	static void handlePendingCtrlC(Executor& exec, ReplState& state) {
		if (!takeCtrlC()) return;

		state.buffer.clear();
		state.waiting_for_more = false;
		std::fputc('\n', stdout);
		runTrap(exec, "INT", "<trap INT>");
		exec.setLastStatus(kSigintStatus);
	}

	static void trackWindowSize(Environment& env, Executor& exec, ReplState& state) {
		if (state.h_out == INVALID_HANDLE_VALUE) return;

		CONSOLE_SCREEN_BUFFER_INFO info{};
		if (!::GetConsoleScreenBufferInfo(state.h_out, &info)) return;

		const int cols  = info.srWindow.Right  - info.srWindow.Left + 1;
		const int lines = info.srWindow.Bottom - info.srWindow.Top  + 1;
		if (cols == state.last_cols && lines == state.last_lines) return;

		env.set("COLUMNS", std::to_string(cols));
		env.set("LINES",   std::to_string(lines));
		if (state.last_cols != 0) runTrap(exec, "WINCH", "<trap WINCH>");

		state.last_cols  = cols;
		state.last_lines = lines;
	}
#endif /* _WIN32 */

	static void pumpAsyncEvents(Environment& env, Executor& exec, ReplState& state) {
#ifdef _WIN32
		restoreConsoleModes(state);
		handlePendingCtrlC(exec, state);
		trackWindowSize(env, exec, state);
#else
		(void)env; (void)exec; (void)state;
#endif /* _WIN32 */
	}

	static std::string buildPrompt(const Environment& env, const Executor& exec,
	                               const ReplState& state) {
		std::string ps_raw = state.waiting_for_more ? env.get("PS2") : env.get("PS1");
		if (ps_raw.empty()) ps_raw = state.waiting_for_more ? "> " : "$ ";
		return expandPrompt(ps_raw, env, exec.pathConv());
	}

	static void maybeExpandHistory(const std::vector<std::string>& history, std::string& line) {
		if (line.empty() || line[0] == ' ') return;

		std::string expanded;
		if (!expandHistory(line, history, expanded)) return;

		std::fprintf(stdout, "%s\n", expanded.c_str());
		std::fflush(stdout);
		line = expanded;
	}

	static void appendToBuffer(ReplState& state, const std::string& line) {
		if (!state.buffer.empty()) state.buffer.push_back('\n');
		state.buffer += line;
	}

	static void printLexParseErrors(const std::vector<LexError>& lex_errs,
	                                const std::vector<ParseError>& parse_errs) {
		const bool err_color = stderrIsTty();
		const char* err_pre = err_color ? "\x1b[31;1m" : "";
		const char* err_loc = err_color ? "\x1b[33m"   : "";
		const char* err_msg = err_color ? "\x1b[0m"    : "";
		for (const auto& error : lex_errs) {
			std::fprintf(stderr, "%swbsh: lex%s %s%zu:%zu:%s %s\n",
				err_pre, err_msg, err_loc, error.loc.line, error.loc.column, err_msg,
				error.message.c_str());
		}

		for (const auto& error : parse_errs) {
			std::fprintf(stderr, "%swbsh: parse%s %s%zu:%zu:%s %s\n",
				err_pre, err_msg, err_loc, error.loc.line, error.loc.column, err_msg,
				error.message.c_str());
		}
	}

	static void parseAndMaybeExecute(Executor& exec, ReplState& state) {
		Lexer lex(state.buffer);
		auto tokens = lex.tokenize();
		Parser parser(std::move(tokens), state.buffer);
		auto root = parser.parseProgram();

		if (looksIncomplete(lex.errors(), parser.errors())) {
			state.waiting_for_more = true;
			return;
		}

		state.waiting_for_more = false;
		printLexParseErrors(lex.errors(), parser.errors());

		if (root && parser.errors().empty() && lex.errors().empty()) {
			exec.adoptArena(parser.takeArena());
			exec.setSourceText(state.buffer);
			exec.execute(*root);
		}

		state.buffer.clear();
	}

	static bool shellExited(Executor& exec, int& exit_status) {
		if (exec.consumeFlow(FlowSignal::Kind::Exit, &exit_status)) return true;

		exec.clearFlow();
		return false;
	}

	// Ctrl-D with a continuation pending only abandons the half-typed
	// command; the shell itself stays up.
	static bool abandonPendingInput(ReplState& state) {
		if (state.buffer.empty()) return false;

		state.buffer.clear();
		state.waiting_for_more = false;
		std::fputc('\n', stdout);
		return true;
	}

	static int exitOnEof(Executor& exec, const ReplState& state) {
		std::fputc('\n', stdout);
		exec.fireExitTrap();
		saveHistory(exec, state);
		return exec.lastStatus();
	}

	static int exitWithStatus(Executor& exec, const ReplState& state, int exit_status) {
		exec.fireExitTrap();
		saveHistory(exec, state);
		return exit_status;
	}

	static void runLine(Executor& exec, ReplState& state, std::string& line) {
		if (!state.waiting_for_more) maybeExpandHistory(exec.history(), line);
		if (!line.empty()) exec.addHistoryEntry(line);
		appendToBuffer(state, line);

		emitShellMark(state, "C");
		parseAndMaybeExecute(exec, state);
		emitShellMark(state, "D;" + std::to_string(exec.lastStatus()));
	}

	int runInteractive() {
		Environment env;
		prepareEnv(env);
		Executor exec(env);
		absorbInheritedState(env, exec);

		ReplState state;
		initConsoleAndSignals(state);
		initShellDefaults(env, state);
		printBanner(state);
		initHistFile(env, exec, state);

		sourceWbshrc(env, exec);
		runInitCommand(env, exec);
		int init_status = 0;
		if (shellExited(exec, init_status)) {
			saveHistory(exec, state);
			return init_status;
		}

		LineEditor editor(env, exec);
		for (;;) {
			pumpAsyncEvents(env, exec, state);
			emitWorkingDirectory(state);
			emitShellMark(state, "A");

			const std::string prompt = buildPrompt(env, exec, state);
			std::string line;
			if (!editor.readLine(prompt, line)) {
				if (abandonPendingInput(state)) continue;

				return exitOnEof(exec, state);
			}

			runLine(exec, state, line);

			int exit_status = 0;
			if (shellExited(exec, exit_status)) return exitWithStatus(exec, state, exit_status);

			if (!state.waiting_for_more && !line.empty()) {
				exec.markLastHistoryStatus(exec.lastStatus());
			}
		}
	}

}  // namespace wbsh

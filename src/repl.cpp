#include "repl.h"

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

#include <atomic>
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

namespace wbsh {

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

#ifdef _WIN32
	std::atomic<bool> g_ctrlc_pending{ false };

	BOOL WINAPI ctrlCHandler(DWORD ctrl) {
		if (ctrl == CTRL_C_EVENT || ctrl == CTRL_BREAK_EVENT) {
			g_ctrlc_pending.store(true);
			return TRUE;
		}

		return FALSE;
	}

	static void setupConsoleWindow() {
		SetConsoleTitleW(L"wbsh");
		HWND hwnd = GetConsoleWindow();
		if (!hwnd) return;
		BOOL dark = TRUE;
		if (FAILED(DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
		                                 &dark, sizeof(dark)))) {
			DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_OLD,
			                      &dark, sizeof(dark));
		}
		// Win11-only: paint the caption + frame to match the shell's dark
		// theme. Older systems return E_INVALIDARG, which we silently ignore.
		COLORREF caption = RGB(0x14, 0x14, 0x18);
		DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
		COLORREF border = RGB(0x33, 0x33, 0x3a);
		DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
	}

	static void setupConsoleFont(HANDLE h_out) {
		if (h_out == INVALID_HANDLE_VALUE) return;
		CONSOLE_FONT_INFOEX cfi{};
		cfi.cbSize = sizeof(cfi);
		cfi.nFont = 0;
		cfi.dwFontSize.X = 0;
		cfi.dwFontSize.Y = 16;
		cfi.FontFamily = FF_DONTCARE;
		cfi.FontWeight = FW_NORMAL;
		const wchar_t* faces[] = { L"Cascadia Mono", L"Cascadia Code", L"Consolas" };
		for (const wchar_t* face : faces) {
			wcsncpy_s(cfi.FaceName, face, _TRUNCATE);
			if (SetCurrentConsoleFontEx(h_out, FALSE, &cfi)) return;
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

	static std::string detectGitOpState(const std::filesystem::path& git_dir) {
		namespace fs = std::filesystem;
		std::error_code ec;
		if (fs::exists(git_dir / "MERGE_HEAD", ec))        return "merging";
		if (fs::exists(git_dir / "rebase-merge", ec) ||
		    fs::exists(git_dir / "rebase-apply", ec))      return "rebasing";
		if (fs::exists(git_dir / "CHERRY_PICK_HEAD", ec))  return "cherry-picking";
		if (fs::exists(git_dir / "REVERT_HEAD", ec))       return "reverting";
		if (fs::exists(git_dir / "BISECT_LOG", ec))        return "bisecting";
		return {};
	}

	static std::string detectGitDirtyState() {
		if (const char* off = std::getenv("WBSH_GIT_NO_DIRTY"); off && *off && *off != '0')
			return {};
#ifdef _WIN32
		FILE* p = _popen("git --no-optional-locks status --porcelain 2>NUL", "r");
#else
		FILE* p = popen("git --no-optional-locks status --porcelain 2>/dev/null", "r");
#endif
		if (!p) return {};
		bool unstaged = false, staged = false, untracked = false;
		char buf[512];
		while (std::fgets(buf, sizeof(buf), p)) {
			if (buf[0] == '?' && buf[1] == '?') {
				untracked = true;
			} else {
				if (buf[0] != ' ' && buf[0] != '?') staged = true;
				if (buf[1] != ' ' && buf[1] != '?') unstaged = true;
			}

			if (unstaged) break;
		}
#ifdef _WIN32
		_pclose(p);
#else
		pclose(p);
#endif
		if (unstaged)  return "unstaged";
		if (staged)    return "staged";
		if (untracked) return "untracked";
		return {};
	}

	static GitInfo detectGitInfo() {
		GitInfo info;
		const std::filesystem::path git_dir = findGitDir();
		if (git_dir.empty()) return info;

		std::ifstream head(git_dir / "HEAD");
		std::string line;
		if (!std::getline(head, line)) return info;
		while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
			line.pop_back();

		const std::string ref_prefix = "ref: ";
		if (line.compare(0, ref_prefix.size(), ref_prefix) == 0) {
			std::string ref = line.substr(ref_prefix.size());
			const std::string heads = "refs/heads/";
			info.branch = (ref.compare(0, heads.size(), heads) == 0)
				? ref.substr(heads.size()) : ref;
		} else {
			if (line.size() >= 7) line.resize(7);
			info.branch = line + " (detached)";
		}

		info.state = detectGitOpState(git_dir);
		if (info.state.empty()) info.state = detectGitDirtyState();
		return info;
	}

	static std::string currentCwdAsUtf8() {
		std::error_code ec;
		const auto p = std::filesystem::current_path(ec);
		return ec ? std::string(".") : pathToUtf8(p);
	}

	static std::string promptUser(const Environment& env) {
		std::string u = env.get("USER");
		if (u.empty()) u = env.get("USERNAME");
		return u;
	}

	static std::string promptHost(const Environment& env, char form) {
		std::string h = env.get("HOSTNAME");
		if (h.empty()) h = env.get("COMPUTERNAME");
		if (form == 'h') {
			const auto dot = h.find('.');
			if (dot != std::string::npos) h.resize(dot);
		}

		return h;
	}

	static std::string promptCwdHome(const Environment& env, const PathConv& pc) {
		std::string posix = pc.toPosix(currentCwdAsUtf8());
		const std::string home = env.get("HOME");
		if (!home.empty() && posix.size() >= home.size()
		    && posix.compare(0, home.size(), home) == 0
		    && (posix.size() == home.size() || posix[home.size()] == '/'))
		{
			posix = "~" + posix.substr(home.size());
		}

		return posix;
	}

	static std::string promptCwdBasename(const PathConv& pc) {
		const std::string posix = pc.toPosix(currentCwdAsUtf8());
		const auto slash = posix.rfind('/');
		return (slash == std::string::npos) ? posix : posix.substr(slash + 1);
	}

	static std::string promptGitBranch() {
		GitInfo gi = detectGitInfo();
		if (gi.branch.empty()) return {};

#ifdef _WIN32
		const bool tty = _isatty(_fileno(stdout)) != 0;
#else
		const bool tty = false;
#endif
		std::string out;
		if (!tty) {
			out += " (";
			out += gi.branch;
			if (!gi.state.empty()) { out += " | "; out += gi.state; }
			out += ")";
			return out;
		}

		const char* yellow = "\x1b[33;1m";
		out += " ";
		out += yellow;
		out += "(";
		out += gi.branch;
		if (!gi.state.empty()) {
			out += " | ";
			const char* sc = gitStateColor(gi.state);
			if (*sc) {
				out += sc;
				out += gi.state;
				out += yellow;
			} else {
				out += gi.state;
			}
		}

		out += ")\x1b[0m";
		return out;
	}

	static std::string promptTimeHms() {
		const std::time_t t = std::time(nullptr);
		char buf[16];
		std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
		return buf;
	}

	static std::string expandPromptEscape(char nx, const Environment& env,
	                                      const PathConv& pc) {
		switch (nx) {
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
		case 'H':  return promptHost(env, nx);
		case 'w':  return promptCwdHome(env, pc);
		case 'W':  return promptCwdBasename(pc);
		case 'g':  return promptGitBranch();
		case 't':  return promptTimeHms();
		default: {
			std::string out;
			out.push_back('\\');
			out.push_back(nx);
			return out;
		}
		}
	}

	static std::string expandPrompt(const std::string& ps, Environment& env, const PathConv& pc) {
		std::string out;
		for (std::size_t i = 0; i < ps.size(); ++i) {
			if (ps[i] != '\\' || i + 1 >= ps.size()) {
				out.push_back(ps[i]);
				continue;
			}

			out += expandPromptEscape(ps[++i], env, pc);
		}

		return out;
	}

	static bool expandHistoryCarat(const std::string& line,
	                               const std::vector<std::string>& history,
	                               std::string& expanded) {
		StrScan in(line);
		std::string oldp;
		if (history.empty() || !in.consume("^") || !in.readUpTo('^', oldp)) {
			expanded = line;
			return false;
		}

		std::string newp;
		if (!in.readUpTo('^', newp)) newp = in.rest();

		const std::string& base = history.back();
		const auto pos = base.find(oldp);
		if (pos == std::string::npos) { expanded = line; return false; }
		expanded = base.substr(0, pos) + newp + base.substr(pos + oldp.size());
		return true;
	}

	static bool resolveHistoryBang(const std::string& line, std::size_t i,
	                               const std::vector<std::string>& history,
	                               std::string& sub, std::size_t& advance) {
		const char nx = line[i + 1];
		if (nx == '!') {
			if (history.empty()) return false;
			sub = history.back();
			advance = 2;
			return true;
		}

		if (nx == '-' && i + 2 < line.size() && std::isdigit((unsigned char)line[i + 2])) {
			std::size_t k = i + 2;
			while (k < line.size() && std::isdigit((unsigned char)line[k])) ++k;
			int n = 0;
			if (!parseInt(line.substr(i + 2, k - i - 2), n)) return false;
			if (n <= 0 || static_cast<std::size_t>(n) > history.size()) return false;
			sub = history[history.size() - n];
			advance = k - i;
			return true;
		}

		if (std::isdigit((unsigned char)nx)) {
			std::size_t k = i + 1;
			while (k < line.size() && std::isdigit((unsigned char)line[k])) ++k;
			int n = 0;
			if (!parseInt(line.substr(i + 1, k - i - 1), n)) return false;
			if (n <= 0 || static_cast<std::size_t>(n) > history.size()) return false;
			sub = history[n - 1];
			advance = k - i;
			return true;
		}

		if (std::isalpha((unsigned char)nx) || nx == '_') {
			std::size_t k = i + 1;
			while (k < line.size()
			    && (std::isalnum((unsigned char)line[k]) || line[k] == '_' || line[k] == '-')) ++k;
			std::string prefix = line.substr(i + 1, k - i - 1);
			for (auto rit = history.rbegin(); rit != history.rend(); ++rit) {
				if (rit->compare(0, prefix.size(), prefix) == 0) {
					sub = *rit;
					advance = k - i;
					return true;
				}
			}
		}

		return false;
	}

	static bool expandHistory(const std::string& line,
	                   const std::vector<std::string>& history,
	                   std::string& expanded) {
		expanded.clear();
		if (line.empty()) { expanded = line; return false; }
		if (line[0] == '^') return expandHistoryCarat(line, history, expanded);

		bool any = false;
		for (std::size_t i = 0; i < line.size(); ++i) {
			char c = line[i];
			if (c != '!' || i + 1 >= line.size()) { expanded.push_back(c); continue; }
			char nx = line[i + 1];
			if (nx == ' ' || nx == '\t' || nx == '\n' || nx == '='
			    || nx == '"' || nx == '\\') { expanded.push_back(c); continue; }
			std::string sub;
			std::size_t advance = 0;
			if (!resolveHistoryBang(line, i, history, sub, advance)) {
				expanded.push_back(c);
				continue;
			}

			expanded += sub;
			i += advance - 1;
			any = true;
		}

		return any;
	}

	static bool looksIncomplete(const std::vector<LexError>& lex_errs,
	                     const std::vector<ParseError>& parse_errs) {
		for (const auto& e : lex_errs) {
			if (e.message.find("unterminated") != std::string::npos) return true;
		}

		for (const auto& e : parse_errs) {
			const auto& m = e.message;
			if (m.find("expected `")        != std::string::npos
			    || m.find("expected pipeline") != std::string::npos
			    || m.find("expected command")  != std::string::npos) {
				return true;
			}
		}

		return false;
	}

	static void initConsoleAndSignals(ReplState& s) {
#ifdef _WIN32
		SetConsoleCtrlHandler(ctrlCHandler, TRUE);
		SetConsoleOutputCP(CP_UTF8);
		// The captured "good" modes are re-applied before every prompt:
		// externals (pagers, vim) corrupt console state and don't restore it.
		s.h_out = GetStdHandle(STD_OUTPUT_HANDLE);
		s.h_in  = GetStdHandle(STD_INPUT_HANDLE);
		if (s.h_out != INVALID_HANDLE_VALUE && GetConsoleMode(s.h_out, &s.good_out_mode)) {
			s.good_out_mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
			s.good_out_mode |= ENABLE_PROCESSED_OUTPUT;
			s.good_out_mode |= ENABLE_WRAP_AT_EOL_OUTPUT;
			s.color_ok = SetConsoleMode(s.h_out, s.good_out_mode) != 0;
		}

		if (s.h_in != INVALID_HANDLE_VALUE && GetConsoleMode(s.h_in, &s.good_in_mode)) {
			s.good_in_mode |= ENABLE_LINE_INPUT;
			s.good_in_mode |= ENABLE_ECHO_INPUT;
			s.good_in_mode |= ENABLE_PROCESSED_INPUT;
			s.good_in_mode |= ENABLE_EXTENDED_FLAGS;
			SetConsoleMode(s.h_in, s.good_in_mode);
		}

		setupConsoleWindow();
		setupConsoleFont(s.h_out);
#else
		s.color_ok = true;
#endif /* _WIN32 */
	}

	static void initShellDefaults(Environment& env, const ReplState& s) {
		if (env.get("PS1").empty()) {
			if (s.color_ok) {
				env.set("PS1",
					"\\[\\e[32;1m\\]\\u@\\h\\[\\e[0m\\] "
					"\\[\\e[36;1m\\]\\w\\[\\e[0m\\]\\g\\$ ");
			} else {
				env.set("PS1", "\\u@\\h \\w\\g\\$ ");
			}
		}

		if (env.get("PS2").empty()) env.set("PS2", "> ");
	}

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

	static void printBanner(const ReplState& s) {
		if (s.color_ok) {
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

	static void initHistFile(Environment& env, Executor& exec, ReplState& s) {
		std::string home_dir = env.get("HOME");
		s.histfile = env.get("HISTFILE");
		if (s.histfile.empty() && !home_dir.empty()) {
			s.histfile = home_dir + "/.wbsh_history";
			env.set("HISTFILE", s.histfile);
		}

		if (!s.histfile.empty()) {
			exec.loadHistoryFromFile(exec.pathConv().toWin32(s.histfile));
		}
	}

	static void sourceWbshrc(const Environment& env, Executor& exec) {
		std::string home_dir = env.get("HOME");
		if (home_dir.empty()) return;
		std::string rcfile = home_dir + "/.wbshrc";
		std::filesystem::path native = utf8ToPath(exec.pathConv().toWin32(rcfile));
		std::error_code ec;
		if (!std::filesystem::exists(native, ec)) return;
		std::ifstream f(native, std::ios::binary);
		if (!f) return;
		std::stringstream ss;
		ss << f.rdbuf();
		exec.executeText(ss.str(), rcfile);
	}

	static void saveHistory(Executor& exec, const ReplState& s) {
		if (!s.histfile.empty()) {
			exec.saveHistoryToFile(exec.pathConv().toWin32(s.histfile));
		}
	}

	static void pumpAsyncEvents(Environment& env, Executor& exec, ReplState& s) {
#ifdef _WIN32
		if (s.h_out != INVALID_HANDLE_VALUE) SetConsoleMode(s.h_out, s.good_out_mode);
		if (s.h_in  != INVALID_HANDLE_VALUE) SetConsoleMode(s.h_in,  s.good_in_mode);
		if (g_ctrlc_pending.exchange(false)) {
			s.buffer.clear();
			s.waiting_for_more = false;
			std::fputc('\n', stdout);
			if (exec.hasTrap("INT")) {
				std::string action = exec.trapAction("INT");
				exec.executeText(action, "<trap INT>");
			}

			exec.setLastStatus(130);   // 128 + SIGINT
		}

		if (s.h_out != INVALID_HANDLE_VALUE) {
			CONSOLE_SCREEN_BUFFER_INFO info{};
			if (GetConsoleScreenBufferInfo(s.h_out, &info)) {
				int cols  = info.srWindow.Right  - info.srWindow.Left + 1;
				int lines = info.srWindow.Bottom - info.srWindow.Top  + 1;
				if (cols != s.last_cols || lines != s.last_lines) {
					env.set("COLUMNS", std::to_string(cols));
					env.set("LINES",   std::to_string(lines));
					if (s.last_cols != 0 && exec.hasTrap("WINCH")) {
						std::string action = exec.trapAction("WINCH");
						exec.executeText(action, "<trap WINCH>");
					}

					s.last_cols  = cols;
					s.last_lines = lines;
				}
			}
		}
#else
		(void)env; (void)exec; (void)s;
#endif /* _WIN32 */
	}

	static std::string buildPrompt(Environment& env, const Executor& exec, const ReplState& s) {
		std::string ps_raw = s.waiting_for_more ? env.get("PS2") : env.get("PS1");
		if (ps_raw.empty()) ps_raw = s.waiting_for_more ? "> " : "$ ";
		return expandPrompt(ps_raw, env, exec.pathConv());
	}

	static void maybeExpandHistory(const std::vector<std::string>& history,
	                        std::string& line) {
		if (line.empty() || line[0] == ' ') return;
		std::string expanded;
		if (expandHistory(line, history, expanded)) {
			std::fprintf(stdout, "%s\n", expanded.c_str());
			std::fflush(stdout);
			line = expanded;
		}
	}

	static void appendToBuffer(ReplState& s, const std::string& line) {
		if (!s.buffer.empty()) s.buffer.push_back('\n');
		s.buffer += line;
	}

	static void printLexParseErrors(const std::vector<LexError>& lex_errs,
	                         const std::vector<ParseError>& parse_errs) {
		bool err_color = stderrIsTty();
		const char* err_pre = err_color ? "\x1b[31;1m" : "";
		const char* err_loc = err_color ? "\x1b[33m"   : "";
		const char* err_msg = err_color ? "\x1b[0m"    : "";
		for (const auto& e : lex_errs) {
			std::fprintf(stderr, "%swbsh: lex%s %s%zu:%zu:%s %s\n",
				err_pre, err_msg, err_loc, e.loc.line, e.loc.column, err_msg,
				e.message.c_str());
		}

		for (const auto& e : parse_errs) {
			std::fprintf(stderr, "%swbsh: parse%s %s%zu:%zu:%s %s\n",
				err_pre, err_msg, err_loc, e.loc.line, e.loc.column, err_msg,
				e.message.c_str());
		}
	}

	static void parseAndMaybeExecute(Executor& exec, ReplState& s) {
		Lexer lex(s.buffer);
		auto tokens = lex.tokenize();
		Parser parser(std::move(tokens), s.buffer);
		auto root = parser.parseProgram();

		if (looksIncomplete(lex.errors(), parser.errors())) {
			s.waiting_for_more = true;
			return;
		}

		s.waiting_for_more = false;

		printLexParseErrors(lex.errors(), parser.errors());

		if (root && parser.errors().empty() && lex.errors().empty()) {
			exec.adoptArena(parser.takeArena());
			exec.setSourceText(s.buffer);
			exec.execute(*root);
		}

		s.buffer.clear();
	}

	static bool shellExited(Executor& exec, int* exit_status) {
		if (exec.consumeFlow(FlowSignal::Kind::Exit, exit_status)) return true;
		exec.clearFlow();
		return false;
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
		int rc_status = 0;
		if (shellExited(exec, &rc_status)) {
			saveHistory(exec, state);
			return rc_status;
		}

		LineEditor editor(env, exec);
		while (true) {
			pumpAsyncEvents(env, exec, state);

			std::string prompt = buildPrompt(env, exec, state);
			std::string line;
			if (!editor.readLine(prompt, line)) {
				if (!state.buffer.empty()) {
					state.buffer.clear();
					state.waiting_for_more = false;
					std::fputc('\n', stdout);
					continue;
				}

				std::fputc('\n', stdout);
				exec.fireExitTrap();
				saveHistory(exec, state);
				return exec.lastStatus();
			}

			if (!state.waiting_for_more) maybeExpandHistory(exec.history(), line);
			if (!line.empty()) exec.addHistoryEntry(line);
			appendToBuffer(state, line);

			parseAndMaybeExecute(exec, state);
			int exit_status = 0;
			if (shellExited(exec, &exit_status)) {
				exec.fireExitTrap();
				saveHistory(exec, state);
				return exit_status;
			}

			if (!state.waiting_for_more && !line.empty()) {
				exec.markLastHistoryStatus(exec.lastStatus());
			}
		}
	}

}  // namespace wbsh

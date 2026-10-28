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
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ast.h"
#include "environment.h"
#include "executor.h"
#include "gitprompt.h"
#include "history.h"
#include "keymap.h"
#include "lexer.h"
#include "lineedit.h"
#include "parser.h"
#include "pathconv.h"
#include "prompt.h"
#include "setup.h"

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
	static const char* const kDefaultHistorySize = "5000";

	struct ReplState {
		std::string buffer;
		std::string history_entry;
		bool        history_recorded = false;
		HistorySubstitution last_substitution;
		bool        waiting_for_more = false;
		bool        color_ok = false;
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

	static bool stdinIsTty() {
#ifdef _WIN32
		return _isatty(_fileno(stdin)) != 0;
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

	static bool parseErrorLooksIncomplete(const std::string& message) {
		return message.find("expected `")        != std::string::npos
		    || message.find("expected pipeline") != std::string::npos
		    || message.find("expected command")  != std::string::npos;
	}

	static bool looksIncomplete(const std::string& text, const std::vector<LexError>& lex_errs,
	                            const std::vector<ParseError>& parse_errs) {
		if (endsWithLineContinuation(text)) return true;

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
		installHistoryCloseHandler();
		::SetConsoleOutputCP(CP_UTF8);
		captureConsoleModes(state);
		if (stdinIsTty() && stdoutIsTty()) {
			setupConsoleWindow();
			setupConsoleFont(state.h_out);
		}
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

	// As bash does for an interactive shell, with wbsh's longer default.
	static void initHistoryVariables(Environment& env) {
		const std::string home_dir = env.get("HOME");
		if (!env.has("HISTFILE") && !home_dir.empty()) {
			env.set("HISTFILE", home_dir + "/.wbsh_history");
		}

		if (!env.has("HISTSIZE")) env.set("HISTSIZE", kDefaultHistorySize);
		if (!env.has("HISTFILESIZE")) env.set("HISTFILESIZE", env.get("HISTSIZE"));
	}

	static std::string historyFilePath(Executor& exec) {
		const std::string histfile = exec.env().get("HISTFILE");
		if (histfile.empty()) return {};

		return exec.pathConv().toWin32(histfile);
	}

	static void trimHistoryFileToSize(Executor& exec, const std::string& path) {
		const long long limit = historyLimit(exec.env().get("HISTFILESIZE"));
		if (path.empty() || limit < 0) return;

		trimHistoryFile(path, static_cast<std::size_t>(limit));
	}

	// Read after the rc file, so a HISTFILE set there is the one used.
	static void loadHistory(Executor& exec) {
		const std::string path = historyFilePath(exec);
		if (path.empty()) return;

		exec.loadHistoryFromFile(path);
		trimHistoryFileToSize(exec, path);
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

	static void saveNewHistory(Executor& exec) {
		commitHistory(exec, historyFilePath(exec));
	}

	static void finishHistory(Executor& exec) {
		const std::string path = historyFilePath(exec);
		commitHistory(exec, path);
		trimHistoryFileToSize(exec, path);
	}

	static void runTrap(Executor& exec, const char* signal_name, const char* origin) {
		if (!exec.hasTrap(signal_name)) return;

		const std::string action = exec.trapAction(signal_name);
		exec.executeText(action, origin);
	}

	static void discardPendingInput(ReplState& state) {
		state.buffer.clear();
		state.history_entry.clear();
		state.waiting_for_more = false;
	}

#ifdef _WIN32
	static void restoreConsoleModes(const ReplState& state) {
		if (state.h_out != INVALID_HANDLE_VALUE) ::SetConsoleMode(state.h_out, state.good_out_mode);
		if (state.h_in  != INVALID_HANDLE_VALUE) ::SetConsoleMode(state.h_in,  state.good_in_mode);
	}

	static void handlePendingCtrlC(Executor& exec, ReplState& state) {
		if (!takeCtrlC()) return;

		discardPendingInput(state);
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

	static std::string buildPrompt(const Environment& env, Executor& exec,
	                               const ReplState& state) {
		std::string ps_raw = state.waiting_for_more ? env.get("PS2") : env.get("PS1");
		if (ps_raw.empty()) ps_raw = state.waiting_for_more ? "> " : "$ ";
		return expandPromptString(exec, ps_raw, stdinIsTty() && stdoutIsTty());
	}

	// PROMPT_COMMAND runs before each primary prompt; as an array, every
	// element runs in turn. `$?` is the same afterwards.
	static std::vector<std::string> promptCommands(const Environment& env) {
		std::vector<std::string> commands;
		const Environment::IndexedArray* array = env.getIndexedArray("PROMPT_COMMAND");
		if (array != nullptr) {
			for (const auto& element : *array) commands.push_back(element.second);
			return commands;
		}

		const std::string command = env.get("PROMPT_COMMAND");
		if (!command.empty()) commands.push_back(command);
		return commands;
	}

	static void runPromptCommand(Executor& exec, const ReplState& state) {
		if (state.waiting_for_more) return;

		const int status = exec.lastStatus();
		for (const std::string& command : promptCommands(exec.env())) {
			if (command.empty()) continue;

			exec.executeText(command, "PROMPT_COMMAND");
			if (exec.hasPendingExit()) return;
		}

		exec.setLastStatus(status);
	}

	// A continuation line that starts inside a quoted string keeps its
	// `!`s as long as the quote is single.
	static char openQuoteOf(const std::string& pending) {
		Lexer lex(pending);
		lex.tokenize();
		for (const LexError& error : lex.errors()) {
			if (error.message.find("single-quoted") != std::string::npos) return '\'';
			if (error.message.find("$'...'") != std::string::npos) return '\'';
			if (error.message.find("double-quoted") != std::string::npos) return '"';
		}

		return '\0';
	}

	static void showExpandedLine(const std::string& text) {
		std::fprintf(stderr, "%s\n", text.c_str());
		std::fflush(stderr);
	}

	// As in bash, the expanded line is shown before it runs; a failed
	// expansion drops the line, and `:p` only shows and saves it.
	static bool expandHistoryInLine(Executor& exec, ReplState& state, std::string& line) {
		if (!exec.env().options().histexpand) return true;

		const char quote = state.waiting_for_more ? openQuoteOf(state.buffer) : '\0';
		const HistoryExpansion expansion =
			expandHistoryLine(line, exec.history(), quote, state.last_substitution);
		switch (expansion.kind) {
		case HistoryExpansion::Kind::kExpanded:
			showExpandedLine(expansion.text);
			line = expansion.text;
			return true;
		case HistoryExpansion::Kind::kPrintOnly:
			showExpandedLine(expansion.text);
			exec.addHistoryEntry(expansion.text);
			discardPendingInput(state);
			return false;
		case HistoryExpansion::Kind::kFailed:
			std::fprintf(stderr, "wbsh: %s\n", expansion.text.c_str());
			discardPendingInput(state);
			return false;
		default:
			return true;
		}
	}

	static void collectHistoryLine(ReplState& state, const std::string& line) {
		appendHistoryLine(state.history_entry, state.buffer, line);
	}

	static bool historyWanted(Executor& exec, const std::string& entry) {
		if (!exec.env().options().history) return false;

		const Environment& env = exec.env();
		const HistoryFilter filter = { env.get("HISTCONTROL"), env.get("HISTIGNORE") };
		const auto& history = exec.history();
		return historyAccepts(filter, entry, history.empty() ? std::string() : history.back());
	}

	static void recordHistoryEntry(Executor& exec, ReplState& state) {
		const std::string entry = std::move(state.history_entry);
		state.history_entry.clear();
		state.history_recorded = historyWanted(exec, entry);
		if (!state.history_recorded) return;

		const HistoryFilter filter = { exec.env().get("HISTCONTROL"), std::string() };
		if (historyErasesDuplicates(filter)) exec.eraseHistoryEntries(entry);

		exec.addHistoryEntry(entry);
		holdUnsavedHistory(historyFilePath(exec), entry);
	}

	// The entry goes to the history file as soon as its status is known,
	// so other shells and a closed window keep it.
	static void finishCommandHistory(Executor& exec, ReplState& state) {
		if (state.history_recorded) exec.markLastHistoryStatus(exec.lastStatus());

		state.history_recorded = false;
		saveNewHistory(exec);
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

	// The text ends in a newline, as bash's input does, so a here-document
	// whose terminator line has not been typed yet reads as unfinished.
	static void parseAndMaybeExecute(Executor& exec, ReplState& state) {
		const std::string text = state.buffer + "\n";
		Lexer lex(text);
		auto tokens = lex.tokenize();
		Parser parser(std::move(tokens), text);
		auto root = parser.parseProgram();

		if (looksIncomplete(state.buffer, lex.errors(), parser.errors())) {
			state.waiting_for_more = true;
			return;
		}

		state.waiting_for_more = false;
		exec.advanceCommandNumber();
		recordHistoryEntry(exec, state);
		printLexParseErrors(lex.errors(), parser.errors());

		if (!parser.errors().empty() || !lex.errors().empty()) {
			exec.setLastStatus(kSyntaxErrorStatus);
		} else if (root) {
			exec.adoptArena(parser.takeArena());
			exec.setSourceText(text);
			exec.executeInput(*root, text);
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

		discardPendingInput(state);
		std::fputc('\n', stdout);
		return true;
	}

	// Ctrl-C at a prompt arrives as a key in raw mode, so the editor has
	// already echoed ^C; the rest is what a SIGINT at the prompt does.
	static void abandonInterruptedInput(Executor& exec, ReplState& state) {
		discardPendingInput(state);
		runTrap(exec, "INT", "<trap INT>");
		exec.setLastStatus(kSigintStatus);
	}

	static int exitOnEof(Executor& exec) {
		std::fputc('\n', stdout);
		const int status = exec.fireExitTrap(exec.lastStatus());
		finishHistory(exec);
		return status;
	}

	static int exitWithStatus(Executor& exec, int exit_status) {
		const int status = exec.fireExitTrap(exit_status);
		finishHistory(exec);
		return status;
	}

	static void runLine(Executor& exec, ReplState& state, std::string& line) {
		if (!expandHistoryInLine(exec, state, line)) return;

		collectHistoryLine(state, line);
		appendToBuffer(state, line);

		emitShellMark(state, "C");
		parseAndMaybeExecute(exec, state);
		emitShellMark(state, "D;" + std::to_string(exec.lastStatus()));
	}

	int runInteractive() {
		Environment env;
		prepareEnv(env);
		env.setInteractive(true);
		Executor exec(env);
		absorbInheritedState(env, exec);

		ReplState state;
		initConsoleAndSignals(state);
		initShellDefaults(env, state);
		printBanner(state);
		initHistoryVariables(env);

		loadInputrc(exec);
		sourceWbshrc(env, exec);
		loadHistory(exec);
		runInitCommand(env, exec);
		int init_status = 0;
		if (shellExited(exec, init_status)) {
			finishHistory(exec);
			return init_status;
		}

		LineEditor editor(env, exec);
		editor.setPromptRefresher([&env, &exec, &state] { return buildPrompt(env, exec, state); });
		setGitStatusListener([] { LineEditor::requestPromptRefresh(); });
		for (;;) {
			pumpAsyncEvents(env, exec, state);
			runPromptCommand(exec, state);

			int prompt_exit = 0;
			if (shellExited(exec, prompt_exit)) return exitWithStatus(exec, prompt_exit);

			emitWorkingDirectory(state);
			emitShellMark(state, "A");

			const std::string prompt = buildPrompt(env, exec, state);
			std::string line;
			const LineStatus status = editor.readLine(prompt, line);
			if (status == LineStatus::kInterrupted) {
				abandonInterruptedInput(exec, state);
				continue;
			}

			if (status == LineStatus::kEof) {
				if (abandonPendingInput(state)) continue;

				return exitOnEof(exec);
			}

			runLine(exec, state, line);

			int exit_status = 0;
			if (shellExited(exec, exit_status)) return exitWithStatus(exec, exit_status);

			finishCommandHistory(exec, state);
		}
	}

}  // namespace wbsh

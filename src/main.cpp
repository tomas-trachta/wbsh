/**
 * @file main.cpp
 * @brief Process entry point: decode the command line, pick an action, run it.
 */

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>

#  include <crtdbg.h>
#  include <cstdint>
#  include <fcntl.h>
#  include <io.h>
#  include <shellapi.h>
#  pragma comment(lib, "shell32.lib")
#endif /* _WIN32 */

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "pathconv.h"
#include "repl.h"
#include "script.h"
#include "source.h"

using namespace wbsh;

// The version arrives from wbsh.vcxproj as bare numeric defines — /D
// with quoted strings gets double-escaped by MSBuild.
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

static void printHelp() {
	std::cout <<
		"wbsh " WBSH_VERSION_STR " -- a Bash-compatible shell for Windows\n"
		"\n"
		"usage:\n"
		"  wbsh                          interactive shell (TTY auto-detect)\n"
		"  wbsh [opts] -c <command>      run / dump the given string\n"
		"  wbsh [opts] <file>            run / dump the file (- for stdin)\n"
		"\n"
		"modes (default for files = dump AST, NOT execute -- see -r):\n"
		"  -i, --interactive             force interactive REPL\n"
		"  -r, --run                     actually execute the script\n"
		"  -e, --expand                  walk the AST and dump expanded words\n"
		"  -t, --tokens                  dump the token stream\n"
		"  --no-ast                      suppress the AST dump\n"
		"  -h, --help                    show this help\n"
		"  -v, --version                 print version and exit\n"
		"  --agent-info                  print a longer brief for scripts/tools/AI agents\n";
}

static void printAgentInfo() {
	std::cout <<
		"wbsh " WBSH_VERSION_STR " -- agent brief\n"
		"\n"
		"WHAT THIS IS\n"
		"  wbsh is a Bash-compatible shell for Windows: a real POSIX shell\n"
		"  grammar (lexer/parser/AST), not a wrapper around cmd.exe. It ships\n"
		"  as a single exe with bundled coreutils (ls, grep, sed, awk, find,\n"
		"  xargs, tar, gzip, curl, hashing tools, etc.), so scripts using those\n"
		"  work without anything else on PATH. System tools (git, vim, less, ...)\n"
		"  are auto-discovered on PATH and invoked as native Windows processes.\n"
		"\n"
		"HOW TO RUN A COMMAND (READ THIS FIRST)\n"
		"  By default, giving wbsh a command or script does NOT execute it --\n"
		"  it parses and dumps the AST instead. You must pass -r/--run to\n"
		"  actually execute:\n"
		"    wbsh -r -c \"echo hello && ls -la\"     run one command line\n"
		"    wbsh -r ./script.sh                   run a script file\n"
		"    wbsh -r -                             run a script from stdin\n"
		"  Without -r, wbsh -c \"...\" only prints a parse tree and exits 0 --\n"
		"  it will NOT run the command and will NOT produce the command's\n"
		"  output. This is the opposite of bash's -c default; do not omit -r.\n"
		"\n"
		"PATH TRANSLATION\n"
		"  POSIX-style paths are converted for native Windows executables:\n"
		"  /c/Users/name <-> C:\\Users\\name. Built-in commands and coreutils\n"
		"  accept either form directly.\n"
		"\n"
		"EXIT STATUS AND OUTPUT\n"
		"  Standard POSIX exit-status conventions apply ($?, &&, ||, pipefail\n"
		"  when set). Output is written LF-only (no CRLF translation), so\n"
		"  `$(...)` command substitution and `read` behave like on Linux.\n"
		"\n"
		"COMPATIBILITY NOTES\n"
		"  Core POSIX shell syntax, pipelines, redirection, functions, [[ ]],\n"
		"  arrays, and common bash builtins are supported. This is an early\n"
		"  but released project (see README) -- some bash edge cases may not\n"
		"  yet match exactly. When in doubt, test the exact command with\n"
		"  `wbsh -r -c \"...\"` first rather than assuming bash parity.\n"
		"\n"
		"MORE\n"
		"  wbsh --help          short flag reference\n"
		"  wbsh --version       version string\n"
		"  README.md            full docs, install, feature list\n";
}

enum class Action {
	RunScript,
	RunInteractive,
	PrintHelp,
	PrintAgentInfo,
	PrintVersion,
};

enum class SourceKind {
	Unset,
	CommandString,
	File,
	Stdin,
};

enum class AstDump {
	Default,
	Shown,
	Hidden,
};

// A source given later on the command line replaces an earlier one; the
// first informational flag (-h, -v, -i, ...) decides the action.
struct Options {
	Action      action = Action::RunScript;
	SourceKind  source = SourceKind::Unset;
	std::string command;
	std::string file_path;
	bool        show_tokens     = false;
	bool        show_expansions = false;
	bool        execute         = false;
	AstDump     ast             = AstDump::Default;
};

static void setAction(Options& options, Action action) {
	if (options.action == Action::RunScript) options.action = action;
}

static bool parseArgs(int argc, char** argv, Options& options, std::string& out_error) {
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];

		const bool help        = arg == "-h" || arg == "--help";
		const bool version     = arg == "-v" || arg == "--version";
		const bool interactive = arg == "-i" || arg == "--interactive";
		if (help)                  { setAction(options, Action::PrintHelp);      continue; }
		if (arg == "--agent-info") { setAction(options, Action::PrintAgentInfo); continue; }
		if (version)               { setAction(options, Action::PrintVersion);   continue; }
		if (interactive)           { setAction(options, Action::RunInteractive); continue; }

		if (arg == "-t" || arg == "--tokens") { options.show_tokens     = true; continue; }
		if (arg == "-e" || arg == "--expand") { options.show_expansions = true; continue; }
		if (arg == "-r" || arg == "--run")    { options.execute         = true; continue; }
		if (arg == "--ast")                   { options.ast = AstDump::Shown;   continue; }
		if (arg == "--no-ast")                { options.ast = AstDump::Hidden;  continue; }

		if (arg == "-c") {
			if (i + 1 >= argc) {
				out_error = "-c requires an argument";
				return false;
			}

			options.source  = SourceKind::CommandString;
			options.command = argv[++i];
			continue;
		}

		if (arg == "-") {
			options.source = SourceKind::Stdin;
			continue;
		}

		options.source    = SourceKind::File;
		options.file_path = arg;
	}

	return true;
}

static std::string readAll(std::istream& in) {
	std::stringstream buffer;
	buffer << in.rdbuf();

	std::string text = buffer.str();
	normalizeCrlf(text);
	return text;
}

static bool stdinIsTerminal() {
#ifdef _WIN32
	return _isatty(_fileno(stdin)) != 0;
#else
	return false;
#endif /* _WIN32 */
}

static bool readScriptFile(const std::string& path, std::string& out_text) {
	std::ifstream file(path, std::ios::binary);
	if (file.fail()) {
		std::cerr << "wbsh: cannot open file: " << path << "\n";
		return false;
	}

	out_text = readAll(file);
	return true;
}

// Without -r a script is only dumped, so the AST is shown unless said
// otherwise; with -r it is hidden unless asked for with --ast.
static bool astShown(const Options& options) {
	if (options.ast == AstDump::Shown)  return true;
	if (options.ast == AstDump::Hidden) return false;
	return !options.execute;
}

static bool loadScriptRun(const Options& options, ScriptRun& run) {
	run.show_tokens     = options.show_tokens;
	run.show_ast        = astShown(options);
	run.show_expansions = options.show_expansions;
	run.execute         = options.execute;

	switch (options.source) {
	case SourceKind::CommandString:
		run.source = options.command;
		return true;
	case SourceKind::File:
		run.script_name = options.file_path;
		return readScriptFile(options.file_path, run.source);
	case SourceKind::Stdin:
	case SourceKind::Unset:
	default:
		run.source = readAll(std::cin);
		return true;
	}
}

static int runScriptAction(const Options& options) {
	ScriptRun run;
	if (!loadScriptRun(options, run)) return 2;

	return runScript(run);
}

#ifdef _WIN32

static void rewriteArgvAsUtf8(int& argc, char**& argv,
		std::vector<std::string>& storage, std::vector<char*>& ptrs) {
	int wide_argc = 0;
	LPWSTR* wide_argv = ::CommandLineToArgvW(::GetCommandLineW(), &wide_argc);
	if (wide_argv == nullptr) return;

	storage.reserve(static_cast<std::size_t>(wide_argc));
	for (int i = 0; i < wide_argc; ++i) {
		storage.push_back(wbsh::wideToUtf8(wide_argv[i]));
	}

	::LocalFree(wide_argv);

	ptrs.reserve(storage.size() + 1);
	for (auto& arg : storage) ptrs.push_back(arg.data());
	ptrs.push_back(nullptr);
	argc = static_cast<int>(storage.size());
	argv = ptrs.data();
}

// By default the CRT's invalid-parameter handler aborts the whole
// process on things a shell must treat as ordinary, recoverable
// failures: `_dup`/`_dup2`/`_close` on a not-yet-open fd (routine
// while juggling `exec 3>&-`-style descriptors), or `strftime` given
// a glibc-only spec like `%s` that MSVC doesn't implement. Install a
// no-op handler so those calls report failure (-1 / errno) instead of
// tearing down the shell.
static void noopInvalidParameterHandler(const wchar_t*, const wchar_t*,
	const wchar_t*, unsigned int, uintptr_t) {
}

static void prepareCrtForShellUse() {
	_set_invalid_parameter_handler(noopInvalidParameterHandler);
	_CrtSetReportMode(_CRT_ASSERT, 0);

	// LF-only output: the CRT's default text mode writes `\r\n` to pipes
	// and files, which corrupts `$(...)` captures and `read` values.
	_setmode(_fileno(stdout), _O_BINARY);
	_setmode(_fileno(stderr), _O_BINARY);
}

#endif /* _WIN32 */

int main(int argc, char** argv) {
#ifdef _WIN32
	prepareCrtForShellUse();

	std::vector<std::string> argv_utf8_storage;
	std::vector<char*>       argv_ptrs;
	rewriteArgvAsUtf8(argc, argv, argv_utf8_storage, argv_ptrs);
#endif /* _WIN32 */

	Options options;
	std::string error;
	if (!parseArgs(argc, argv, options, error)) {
		std::cerr << "wbsh: " << error << "\n";
		return 2;
	}

	const bool nothing_to_read = options.source == SourceKind::Unset && stdinIsTerminal();
	if (nothing_to_read) setAction(options, Action::RunInteractive);

	switch (options.action) {
	case Action::PrintHelp:
		printHelp();
		return 0;
	case Action::PrintAgentInfo:
		printAgentInfo();
		return 0;
	case Action::PrintVersion:
		std::cout << "wbsh " WBSH_VERSION_STR "\n";
		return 0;
	case Action::RunInteractive:
		return runInteractive();
	case Action::RunScript:
	default:
		return runScriptAction(options);
	}
}

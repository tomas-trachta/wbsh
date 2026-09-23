/**
 * @file main.cpp
 * @brief Process entry point: argv decoding and CLI dispatch.
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

static std::string readAll(std::istream& in) {
	std::stringstream ss;
	ss << in.rdbuf();
	std::string s = ss.str();
	normalizeCrlf(s);
	return s;
}

static bool isInteractiveStdin() {
#ifdef _WIN32
	return _isatty(_fileno(stdin)) != 0;
#else
	return false;
#endif /* _WIN32 */
}

#ifdef _WIN32
static void rewriteArgvAsUtf8(int& argc, char**& argv,
		std::vector<std::string>& storage, std::vector<char*>& ptrs) {
	int wargc = 0;
	LPWSTR* wargv = ::CommandLineToArgvW(::GetCommandLineW(), &wargc);
	if (!wargv) return;
	storage.reserve(static_cast<std::size_t>(wargc));
	for (int i = 0; i < wargc; ++i) {
		storage.push_back(wbsh::wideToUtf8(wargv[i]));
	}

	::LocalFree(wargv);
	ptrs.reserve(storage.size() + 1);
	for (auto& s : storage) ptrs.push_back(s.data());
	ptrs.push_back(nullptr);
	argc = static_cast<int>(storage.size());
	argv = ptrs.data();
}
#endif /* _WIN32 */

struct CliOptions {
	bool show_tokens  = false;
	bool show_ast     = true;
	bool do_expand    = false;
	bool do_run       = false;
	bool ast_explicit = false;
	bool from_stdin   = false;
	bool have_src     = false;
	std::string src;
	std::string script_name;
};

struct ParseResult {
	bool exit_now;
	int  exit_code;
};

static ParseResult parseArgs(int argc, char** argv, CliOptions& opts) {
	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		if (a == "-h" || a == "--help") {
			printHelp();
			return { true, 0 };
		}

		if (a == "--agent-info") {
			printAgentInfo();
			return { true, 0 };
		}

		if (a == "-v" || a == "--version") {
			std::cout << "wbsh " WBSH_VERSION_STR "\n";
			return { true, 0 };
		}

		if (a == "-t" || a == "--tokens")      { opts.show_tokens = true;  continue; }
		if (a == "-e" || a == "--expand")      { opts.do_expand   = true;  continue; }
		if (a == "-r" || a == "--run")         { opts.do_run      = true;  continue; }
		if (a == "-i" || a == "--interactive") {
			return { true, runInteractive() };
		}

		if (a == "--ast")    { opts.show_ast = true;  opts.ast_explicit = true; continue; }
		if (a == "--no-ast") { opts.show_ast = false; opts.ast_explicit = true; continue; }
		if (a == "-c") {
			if (i + 1 >= argc) {
				std::cerr << "wbsh: -c requires an argument\n";
				return { true, 2 };
			}

			opts.src = argv[++i];
			opts.have_src = true;
			continue;
		}

		if (a == "-") {
			opts.from_stdin = true;
			continue;
		}

		std::ifstream f(a, std::ios::binary);
		if (!f) {
			std::cerr << "wbsh: cannot open file: " << a << "\n";
			return { true, 2 };
		}

		opts.src = readAll(f);
		opts.have_src = true;
		opts.script_name = a;
	}

	return { false, 0 };
}

static ParseResult resolveSource(CliOptions& opts) {
	if (opts.have_src) return { false, 0 };
	if (opts.from_stdin) {
		opts.src = readAll(std::cin);
		opts.have_src = true;
		return { false, 0 };
	}

	if (isInteractiveStdin()) {
		return { true, runInteractive() };
	}

	if (!std::cin.eof()) {
		opts.src = readAll(std::cin);
		opts.have_src = true;
		return { false, 0 };
	}

	printHelp();
	return { true, 0 };
}

#ifdef _WIN32
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
#endif /* _WIN32 */

int main(int argc, char** argv) {
#ifdef _WIN32
	_set_invalid_parameter_handler(noopInvalidParameterHandler);
	_CrtSetReportMode(_CRT_ASSERT, 0);

	std::vector<std::string> argv_utf8_storage;
	std::vector<char*>       argv_ptrs;
	rewriteArgvAsUtf8(argc, argv, argv_utf8_storage, argv_ptrs);

	// LF-only output: the CRT's default text mode writes `\r\n` to pipes
	// and files, which corrupts `$(...)` captures and `read` values.
	_setmode(_fileno(stdout), _O_BINARY);
	_setmode(_fileno(stderr), _O_BINARY);
#endif /* _WIN32 */

	CliOptions opts;
	if (auto pr = parseArgs(argc, argv, opts); pr.exit_now) return pr.exit_code;
	if (auto pr = resolveSource(opts);          pr.exit_now) return pr.exit_code;

	if (opts.do_run && !opts.ast_explicit) opts.show_ast = false;
	return runOnSource(opts.src, opts.show_tokens, opts.show_ast,
	                   opts.do_expand, opts.do_run, opts.script_name);
}

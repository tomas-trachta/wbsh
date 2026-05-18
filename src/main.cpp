/**
 * @file main.cpp
 * @brief wbsh process entry point.
 *
 * Parses argv, dispatches to the REPL or the script runner, and
 * stays out of the way. All real logic lives in:
 *   - setup.cpp   environment preparation
 *   - repl.cpp    interactive REPL
 *   - script.cpp  non-interactive run-on-source
 */

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>

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

// The major/minor/patch are passed as numeric preprocessor defines from
// wbsh.vcxproj (which derives them from <WbshVersionMajor> et al). The
// stringification dance below turns them into a single string literal
// usable for adjacent-literal concatenation. Quoting the version
// directly via /D would force MSBuild to escape the quotes, which it
// double-escapes — sidestep that entirely by passing bare digits.
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
		"modes (default for files = dump AST):\n"
		"  -i, --interactive             force interactive REPL\n"
		"  -r, --run                     actually execute the script\n"
		"  -e, --expand                  walk the AST and dump expanded words\n"
		"  -t, --tokens                  dump the token stream\n"
		"  --no-ast                      suppress the AST dump\n"
		"  -h, --help                    show this help\n";
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

// On Windows, MSVC's narrow `argv` is decoded through the C locale
// (default "C"), which mangles any non-ASCII argument like
// "/c/Users/Tomáš" into replacement chars. Re-decode from the wide
// command line to UTF-8 and rewrite argv before anyone reads it.
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
	// Path used to invoke a script file, surfaced as `$0`. Empty for
	// `-c <cmd>` and stdin runs, which keep the default `wbsh`.
	std::string script_name;
};

// Result of one of the early phases of main. `exit_now == true` means the
// caller should `return exit_code` immediately (help shown, REPL ran to
// completion, malformed -c, etc.).
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

// Resolve the source-of-input. With no -c / file / `-`, fall through to
// the interactive REPL when stdin is a TTY, or read piped stdin as a
// script otherwise.
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

int main(int argc, char** argv) {
#ifdef _WIN32
	std::vector<std::string> argv_utf8_storage;
	std::vector<char*>       argv_ptrs;
	rewriteArgvAsUtf8(argc, argv, argv_utf8_storage, argv_ptrs);

	// Force LF-only on stdout so command substitutions and pipelines round-
	// trip the way bash does on POSIX. The MSVC CRT defaults to text mode
	// on stdout/stderr, which silently translates `\n` to `\r\n` whenever
	// the destination is a pipe or file. That extra `\r` then sticks to
	// every word read back through `$(...)` or `read` and makes
	// `arr[$key]` and `arr[X]` index different buckets.
	_setmode(_fileno(stdout), _O_BINARY);
	_setmode(_fileno(stderr), _O_BINARY);
#endif /* _WIN32 */

	CliOptions opts;
	if (auto pr = parseArgs(argc, argv, opts); pr.exit_now) return pr.exit_code;
	if (auto pr = resolveSource(opts);          pr.exit_now) return pr.exit_code;

	// In run mode, default to suppressing the AST dump so output is clean.
	if (opts.do_run && !opts.ast_explicit) opts.show_ast = false;
	return runOnSource(opts.src, opts.show_tokens, opts.show_ast,
	                   opts.do_expand, opts.do_run, opts.script_name);
}

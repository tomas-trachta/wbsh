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
#endif /* _WIN32 */

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#if __has_include("build_info.h")
#  include "build_info.h"
#endif

#include "coreutils_internal.h"
#include "executor.h"
#include "invocation.h"
#include "jsonrun.h"
#include "numparse.h"
#include "pathconv.h"
#include "repl.h"
#include "script.h"
#include "signalbox.h"
#include "source.h"
#include "startup.h"

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

static const int kStatusScriptNotFound = 127;

static void printHelp() {
	std::cout <<
		"wbsh " WBSH_VERSION_STR " -- a Bash-compatible shell for Windows\n"
		"\n"
		"usage:\n"
		"  wbsh [options]                         interactive shell (TTY auto-detect)\n"
		"  wbsh [options] -c <command> [name [arg ...]]\n"
		"                                         run the command ($0 = name)\n"
		"  wbsh [options] <file> [arg ...]        run the file ($1.. = args)\n"
		"  wbsh [options] -s [arg ...]            run stdin (also a lone -)\n"
		"  wbsh [options] --ec <base64> [name [arg ...]]\n"
		"                                         run a base64-encoded command\n"
		"  wbsh --json [options] -c <command>     run it, then print one JSON result\n"
		"\n"
		"options (as in bash; letters may be clustered, e.g. -ec):\n"
		"  -c                    the first operand is the command string\n"
		"  -s                    read commands from stdin\n"
		"  -i                    interactive; with -c or a file, read ~/.wbshrc first\n"
		"  -l, --login           login shell: read ~/.wbsh_profile first\n"
		"  -abefhkmnptuvxBCEHPT  the same letters as `set` (+x turns one off)\n"
		"  -o NAME, +o NAME      set / unset an option by name, e.g. -o pipefail\n"
		"  -O NAME, +O NAME      set / unset a shopt option\n"
		"  --posix               POSIX mode (also when started as sh.exe)\n"
		"  --norc, --noprofile   skip ~/.wbshrc / the login profile\n"
		"  --rcfile FILE         read FILE instead of ~/.wbshrc\n"
		"  --ec, --encodedcommand B64\n"
		"                        the command as base64 (UTF-8 or UTF-16LE), so\n"
		"                        no quoting survives any layer in between\n"
		"  --json                capture stdout and stderr apart, then print one line\n"
		"                        {\"status\":N,\"stdout\":\"..\",\"stderr\":\"..\",\n"
		"                        \"duration_ms\":N}\n"
		"\n"
		"debugging (instead of running, unless -r/--run is given too):\n"
		"  --tokens              dump the token stream\n"
		"  --ast                 dump the parse tree\n"
		"  --expand              dump expanded words\n"
		"\n"
		"  -h, --help            show this help\n"
		"  --version             print version and build\n"
		"  --agent-info          print a longer brief for scripts/tools/AI agents\n";
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
		"HOW TO RUN A COMMAND\n"
		"  Exactly as with bash:\n"
		"    wbsh -c \"echo hello && ls -la\"        run one command line\n"
		"    wbsh -eu -o pipefail -c \"...\"         with errexit, nounset, pipefail\n"
		"    wbsh ./script.sh arg1                 run a script file\n"
		"    wbsh -s < script.sh                   run a script from stdin\n"
		"  A copy named bash.exe behaves the same; one named sh.exe starts in\n"
		"  POSIX mode.\n"
		"\n"
		"QUOTE-PROOF INVOCATION\n"
		"  Every layer between an agent and the shell can re-quote or expand a\n"
		"  command. Base64 it instead and nothing is left to escape:\n"
		"    wbsh --ec ZWNobyAiaGkgJFVTRVIi        runs: echo \"hi $USER\"\n"
		"  UTF-8 or UTF-16LE (PowerShell's -EncodedCommand form), padding\n"
		"  optional, line breaks ignored, an optional v1: prefix is dropped.\n"
		"  Options still go first: wbsh -eu --ec <b64>.\n"
		"  Windows caps a command line at 32767 characters (cmd.exe at 8191);\n"
		"  write a longer command to a file and run wbsh FILE, or pipe it to\n"
		"  wbsh -s.\n"
		"\n"
		"STRUCTURED RESULT\n"
		"  --json runs the command as usual (traps, exit N, set -e) with stdout\n"
		"  and stderr captured apart, then prints one line of JSON:\n"
		"    wbsh --json -c 'echo hi; echo oops >&2; exit 3'\n"
		"    {\"status\":3,\"stdout\":\"hi\\n\",\"stderr\":\"oops\\n\",\"duration_ms\":4}\n"
		"  The line is pure ASCII (non-ASCII as \\uXXXX); bytes that are not\n"
		"  UTF-8 become U+FFFD. The exit status is the command's. It combines\n"
		"  with --ec and script files: wbsh --json --ec <b64>.\n"
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
		"  Non-interactive runs print no banner and read no rc file.\n"
		"\n"
		"COMPATIBILITY NOTES\n"
		"  Core POSIX shell syntax, pipelines, redirection, functions, [[ ]],\n"
		"  arrays, and common bash builtins are supported. Some bash edge\n"
		"  cases may not yet match exactly.\n"
		"\n"
		"MORE\n"
		"  wbsh --help          short flag reference\n"
		"  wbsh --version       version and build\n"
		"  README.md            full docs, install, feature list\n";
}

static void printVersion() {
	std::cout << "wbsh " WBSH_VERSION_STR "\n";
#ifdef WBSH_BUILD_COMMIT
	std::cout << "commit " WBSH_BUILD_COMMIT " (" WBSH_BUILD_DATE ")\n";
#endif /* WBSH_BUILD_COMMIT */
}

static std::string readAll(std::istream& in) {
	std::stringstream buffer;
	buffer << in.rdbuf();

	std::string text = buffer.str();
	stripUtf8Bom(text);
	normalizeCrlf(text);
	return text;
}

// NUL is a character device too, which _isatty takes for a terminal; only a
// console answers GetConsoleMode.
static bool stdinIsTerminal() {
#ifdef _WIN32
	if (_isatty(_fileno(stdin)) == 0) return false;

	DWORD mode = 0;
	const HANDLE input = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdin)));
	return ::GetConsoleMode(input, &mode) != 0;
#else
	return false;
#endif /* _WIN32 */
}

static const int kStatusCannotExecute = 126;
static const std::size_t kBinarySample = 80;

// As bash's check_binary_file: a NUL before the first newline.
static bool looksBinary(const std::string& text) {
	const std::size_t sample = (std::min)(text.size(), kBinarySample);
	for (std::size_t i = 0; i < sample; ++i) {
		if (text[i] == '\n') return false;
		if (text[i] == '\0') return true;
	}

	return false;
}

static bool isRegularFile(const std::filesystem::path& native) {
	std::error_code ec;
	return std::filesystem::is_regular_file(native, ec);
}

// bash looks a script name without a slash up in PATH when the current
// directory does not have it.
static std::filesystem::path locateScript(const std::string& path) {
	const std::filesystem::path given = utf8ToPath(PathConv().toWin32(path));
	const char* search = std::getenv("PATH");
	if (isRegularFile(given) || path.find('/') != std::string::npos || search == nullptr) {
		return given;
	}

	std::stringstream dirs(search);
	std::string dir;
	while (std::getline(dirs, dir, ';')) {
		if (dir.empty()) continue;

		const std::filesystem::path candidate = utf8ToPath(dir) / utf8ToPath(path);
		if (isRegularFile(candidate)) return candidate;

		std::filesystem::path program = candidate;
		program += ".exe";
		if (isRegularFile(program)) return program;
	}

	return given;
}

static int reportScriptError(const std::string& prefix, const std::string& path,
		const char* problem, int status) {
	std::cerr << prefix << ": " << path << ": " << problem << "\n";
	return status;
}

// The path may be POSIX (`/d/work/x.sh`), as tools on PATH pass it; `$0`
// keeps the spelling given. Once the file is open bash has made it `$0`,
// so later errors carry its name instead of the shell's.
static int readScriptFile(const Invocation& invocation, std::string& out_text) {
	const std::string& path = invocation.file_path;
	const std::filesystem::path native = locateScript(path);
	std::error_code ec;
	if (std::filesystem::is_directory(native, ec)) {
		return reportScriptError(path, path, "Is a directory", kStatusCannotExecute);
	}

	std::ifstream file(native, std::ios::binary);
	if (file.fail()) {
		return reportScriptError(invocation.shell_name, path, "No such file or directory",
			kStatusScriptNotFound);
	}

	out_text = readAll(file);
	if (!looksBinary(out_text)) return 0;

	return reportScriptError(path, path, "cannot execute binary file", kStatusCannotExecute);
}

// A self-spawned pipeline stage is a slice of its parent's script and
// keeps counting lines from where the slice starts.
static std::size_t inheritedFirstLine() {
	const char* text = std::getenv("WBSH_LINENO");
	int line = 0;
	if (text == nullptr || !parseInt(text, line) || line < 1) return 1;

	return static_cast<std::size_t>(line);
}

// BASH_ARGV0 in the environment names the shell, as assigning it does,
// unless a name follows the command string.
static void adoptInheritedArgv0(ScriptRun& run) {
	const char* argv0 = std::getenv("BASH_ARGV0");
	if (argv0 != nullptr && *argv0 != '\0' && std::getenv("WBSH_SUBSHELL") == nullptr) {
		run.script_name = argv0;
	}
}

// `wbsh -c cmd name a b` runs cmd with $0 = name and $1.. = a b, as bash does.
static void loadCommandString(const Invocation& invocation, ScriptRun& run) {
	run.source = invocation.command;
	run.command_string = true;
	run.first_line = inheritedFirstLine();
	run.positional.clear();
	if (invocation.operands.empty()) {
		adoptInheritedArgv0(run);
		return;
	}

	run.script_name = invocation.operands.front();
	run.positional.assign(invocation.operands.begin() + 1, invocation.operands.end());
}

static void loadRunSettings(const Invocation& invocation, ScriptRun& run) {
	run.show_tokens     = invocation.show_tokens;
	run.show_ast        = invocationShowsAst(invocation);
	run.show_expansions = invocation.show_expansions;
	run.pretty_print    = invocation.pretty_print;
	run.execute         = invocationExecutes(invocation);
	run.read_rc         = invocation.interactive;
	run.startup         = invocation.startup;
	run.positional      = invocation.operands;
	run.script_name     = invocation.shell_name;
}

// bash reads a script from standard input a command at a time, so that a
// command in it that reads standard input gets the lines that follow.
static void loadStdinScript(ScriptRun& run) {
	run.invocation_letter = 's';
	const bool dumps = run.show_tokens || run.show_ast || run.show_expansions;
	run.stream_stdin = run.execute && !dumps;
	if (!run.stream_stdin) run.source = readAll(std::cin);
}

static int loadScriptRun(const Invocation& invocation, ScriptRun& run) {
	loadRunSettings(invocation, run);

	switch (invocation.source) {
	case InvocationSource::CommandString:
		loadCommandString(invocation, run);
		run.invocation_letter = 'c';
		return 0;
	case InvocationSource::File:
		run.script_name = invocation.file_path;
		run.from_file = true;
		return readScriptFile(invocation, run.source);
	case InvocationSource::Stdin:
	case InvocationSource::Unset:
	default:
		loadStdinScript(run);
		return 0;
	}
}

static int loadAndRunScript(const Invocation& invocation) {
	ScriptRun run;
	const int status = loadScriptRun(invocation, run);
	if (status != 0) return status;

	return runScript(run);
}

static const char* const kBashLongOptions =
		"GNU long options:\n"
		"\t--debug\n"
		"\t--debugger\n"
		"\t--dump-po-strings\n"
		"\t--dump-strings\n"
		"\t--help\n"
		"\t--init-file\n"
		"\t--login\n"
		"\t--noediting\n"
		"\t--noprofile\n"
		"\t--norc\n"
		"\t--posix\n"
		"\t--pretty-print\n"
		"\t--rcfile\n"
		"\t--restricted\n"
		"\t--verbose\n"
		"\t--version\n"
		"Shell options:\n"
		"\t-ilrsD or -c command or -O shopt_option\t\t(invocation only)\n"
		"\t-abefhkmnptuvxBCEHPT or -o option\n";

static bool isInvalidOptionError(const std::string& error) {
	static const std::string kSuffix = ": invalid option";
	return error.size() >= kSuffix.size()
		&& error.compare(error.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0;
}

// After an invalid option bash shows the options it takes.
static void printBashUsage(const std::string& shell_name) {
	std::cerr << shell_name << " [GNU long option] [option] ...\n"
		<< shell_name << " [GNU long option] [option] script-file ...\n"
		<< kBashLongOptions;
}

static int runScriptAction(const Invocation& invocation) {
	if (!invocation.json) return loadAndRunScript(invocation);

	return runCapturedAsJson([&invocation] { return loadAndRunScript(invocation); });
}

static bool startsInteractive(const Invocation& invocation) {
	if (invocation.source != InvocationSource::Unset || invocation.json) return false;

	return invocation.interactive || stdinIsTerminal();
}

#ifdef _WIN32

static void pointAtWords(std::vector<std::string>& storage, std::vector<char*>& ptrs) {
	ptrs.clear();
	ptrs.reserve(storage.size() + 1);
	for (auto& arg : storage) ptrs.push_back(arg.data());
	ptrs.push_back(nullptr);
}

static void utf8Argv(int argc, wchar_t** wide_argv, std::vector<std::string>& storage) {
	storage.reserve(static_cast<std::size_t>(argc));
	for (int i = 0; i < argc; ++i) storage.push_back(wbsh::wideToUtf8(wide_argv[i]));
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
	bufferStdoutForNul();

	// Text-mode input drops every `\r` before a `\n` and stops at the first
	// 0x1A byte, so piped archives and binary data arrive corrupted. The
	// console keeps text mode: its line input arrives as `\r\n`.
	if (_isatty(_fileno(stdin)) == 0) _setmode(_fileno(stdin), _O_BINARY);
}

#endif /* _WIN32 */

static std::vector<std::string> readArgvFile(const std::string& path) {
	std::vector<std::string> words;
	std::ifstream file(utf8ToPath(path), std::ios::binary);
	std::string word;
	while (std::getline(file, word, '\0')) words.push_back(word);
	file.close();

	std::error_code ec;
	std::filesystem::remove(utf8ToPath(path), ec);
	return words;
}

// A parent wbsh whose self-spawn command line would pass the Windows
// limit runs `wbsh --argv-file PATH` instead; PATH holds the real words.
static void expandArgvFile(int& argc, char**& argv,
		std::vector<std::string>& storage, std::vector<char*>& ptrs) {
	if (argc != 3 || std::string(argv[1]) != "--argv-file") return;

	std::vector<std::string> words = readArgvFile(argv[2]);
	words.insert(words.begin(), argv[0]);
	storage = std::move(words);

	pointAtWords(storage, ptrs);
	argc = static_cast<int>(storage.size());
	argv = ptrs.data();
}

static int runShell(int argc, char** argv) {
	Invocation invocation;
	std::string error;
	if (!parseInvocation(argc, argv, invocation, error)) {
		std::cerr << invocation.shell_name << ": " << error << "\n";
		if (isInvalidOptionError(error)) printBashUsage(invocation.shell_name);
		return 2;
	}

	if (startsInteractive(invocation) && invocation.action == InvocationAction::RunScript) {
		invocation.action = InvocationAction::RunInteractive;
	}

	switch (invocation.action) {
	case InvocationAction::PrintHelp:
		printHelp();
		return 0;
	case InvocationAction::PrintAgentInfo:
		printAgentInfo();
		return 0;
	case InvocationAction::PrintVersion:
		printVersion();
		return 0;
	case InvocationAction::RunInteractive:
		return runInteractive(invocation.startup, invocation.shell_name);
	case InvocationAction::RunScript:
	default:
		return runScriptAction(invocation);
	}
}

// wmain gets the command line as UTF-16 without shell32's
// CommandLineToArgvW, whose DLL costs every start, child shells included.
#ifdef _WIN32
int wmain(int argc, wchar_t** wide_argv) {
	prepareCrtForShellUse();
	wbsh::openSignalMailbox();

	std::vector<std::string> storage;
	std::vector<char*> ptrs;
	utf8Argv(argc, wide_argv, storage);
	pointAtWords(storage, ptrs);

	char** argv = ptrs.data();
	expandArgvFile(argc, argv, storage, ptrs);
	return wbsh::processExitCode(runShell(argc, argv));
}
#else
int main(int argc, char** argv) {
	std::vector<std::string> storage;
	std::vector<char*> ptrs;
	expandArgvFile(argc, argv, storage, ptrs);
	return runShell(argc, argv);
}
#endif /* _WIN32 */

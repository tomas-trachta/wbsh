/**
 * @file make_shell.cpp
 * @brief The shell a recipe line or `$(shell)` runs in: `SHELL` and
 *        `.SHELLFLAGS` resolved to a program (a POSIX shell name means this
 *        wbsh), the Win32 command line for it, the spawn, and output capture.
 */

#include "make_internal.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>

#  include <fcntl.h>
#  include <io.h>
#endif /* _WIN32 */

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include "coreutils_internal.h"
#include "executor.h"
#include "exitstatus.h"
#include "pathconv.h"

namespace wbsh::make_detail {

	static const int kStatusCannotRun = 127;

	static const char* const kPosixShells[] = { "sh", "bash", "dash", "ash", "ksh", "mksh", "wbsh" };

	static std::string programStem(const std::string& program) {
		std::string stem = notDirectoryOf(program);
		for (char& c : stem) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		const std::string extension = ".exe";
		const bool has_extension = stem.size() > extension.size()
			&& stem.compare(stem.size() - extension.size(), extension.size(), extension) == 0;
		if (has_extension) stem.resize(stem.size() - extension.size());
		return stem;
	}

	static bool isPosixShellName(const std::string& stem) {
		for (const char* name : kPosixShells) {
			if (stem == name) return true;
		}

		return false;
	}

	// `SHELL = /bin/sh` in a makefile asks for a POSIX shell, and this wbsh
	// is the one at hand; only a Windows path to a real other shell wins.
	static bool meansThisShell(Make& make, const std::string& program) {
		if (program.empty()) return true;

		const std::string stem = programStem(program);
		if (!isPosixShellName(stem)) return false;
		if (stem == "wbsh" || !PathConv::isWin32Absolute(program)) return true;

		return !fileExists(make, program);
	}

	static std::string locateProgram(Make& make, const std::string& program) {
		if (program.find_first_of("/\\") == std::string::npos) {
			const std::string found = make.exec.findExecutable(program);
			return found.empty() ? program : found;
		}

		const std::string native = make.exec.pathConv().toWin32(program);
		if (!fileExists(make, native) && fileExists(make, native + ".exe")) return native + ".exe";

		return native;
	}

	static std::string shellFlags(Make& make, const ExpandContext& ctx) {
		const VarRef found = lookupVariable(make, ".SHELLFLAGS", ctx);
		const bool is_default = found.variable == nullptr || found.variable->origin == Origin::Default;
		if (is_default && make.special.posix) return "-ec";

		return expandVariable(make, ".SHELLFLAGS", ctx);
	}

	ShellProgram resolveShell(Make& make, const ExpandContext& ctx) {
		const std::string program = trimBlanks(expandVariable(make, "SHELL", ctx));
		ShellProgram shell;
		shell.flags = splitWords(shellFlags(make, ctx));
		if (meansThisShell(make, program)) {
			shell.exe = make.self_path;
			shell.is_wbsh = true;
			return shell;
		}

		shell.exe = locateProgram(make, program);
		shell.is_cmd = programStem(program) == "cmd";
		if (shell.is_cmd && shell.flags == std::vector<std::string>{ "-c" }) shell.flags = { "/c" };
		return shell;
	}

	static void appendQuoted(std::string& cmdline, const std::string& arg) {
		if (!cmdline.empty()) cmdline.push_back(' ');
		if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) {
			cmdline += arg;
			return;
		}

		cmdline.push_back('"');
		std::size_t backslashes = 0;
		for (const char c : arg) {
			if (c == '\\') {
				++backslashes;
				continue;
			}

			cmdline.append(c == '"' ? backslashes * 2 + 1 : backslashes, '\\');
			cmdline.push_back(c);
			backslashes = 0;
		}

		cmdline.append(backslashes * 2, '\\');
		cmdline.push_back('"');
	}

	// wbsh's own options are not sh's, so `-e`, `-u`, `-o pipefail` and the
	// like from .SHELLFLAGS reach it as a `set` line before the command.
	static std::string wbshScript(const std::vector<std::string>& flags, const std::string& line) {
		std::vector<std::string> options = flags;
		if (!options.empty() && options.back().size() > 1 && options.back()[0] == '-'
			&& options.back().back() == 'c') {
			options.back().pop_back();
			if (options.back() == "-") options.pop_back();
		}

		if (options.empty()) return line;
		return "set " + joinWords(options) + "\n" + line;
	}

	static std::string shellCommandLine(const ShellProgram& shell, const std::string& line) {
		std::string cmdline;
		appendQuoted(cmdline, shell.exe);
		if (shell.is_wbsh) {
			appendQuoted(cmdline, "-r");
			appendQuoted(cmdline, "-c");
			appendQuoted(cmdline, wbshScript(shell.flags, line));
			return cmdline;
		}

		if (shell.is_cmd) return cmdline + " /d /s " + joinWords(shell.flags) + " \"" + line + "\"";

		for (const std::string& flag : shell.flags) appendQuoted(cmdline, flag);
		appendQuoted(cmdline, line);
		return cmdline;
	}

	static DWORD waitForExitCode(HANDLE process) {
		::WaitForSingleObject(process, INFINITE);
		DWORD code = 0;
		::GetExitCodeProcess(process, &code);
		::CloseHandle(process);
		return code;
	}

	ShellResult runShellLine(Make& make, const ShellProgram& shell, const std::string& line,
	                         const NameValueList& environment) {
		ShellResult result;
		const HANDLE process = make.exec.spawnWithEnvironment(shell.exe,
			shellCommandLine(shell, line), environment);
		if (process == INVALID_HANDLE_VALUE) {
			result.status = kStatusCannotRun;
			return result;
		}

		const DWORD code = waitForExitCode(process);
		result.status = shellStatusFromExitCode(code);
		result.control_c = code == STATUS_CONTROL_C_EXIT;
		return result;
	}

	static NameValueList captureEnvironment(Make& make, const ExpandContext& ctx) {
		if (!make.computing_environment) return childEnvironment(make, ctx);

		NameValueList environment = make.base_environment;
		environment.emplace_back("MAKELEVEL", std::to_string(make.level + 1));
		return environment;
	}

	static void readAllFromFd(int fd, std::string& output) {
		char buffer[4096];
		for (;;) {
			const int got = _read(fd, buffer, sizeof(buffer));
			if (got <= 0) return;
			output.append(buffer, static_cast<std::size_t>(got));
		}
	}

	// The child writes into a pipe put in place of our stdout for the length
	// of the spawn; our copy of the write end is gone before we read, so the
	// read sees end-of-file once the child (and anything it left running) is done.
	bool captureShell(Make& make, const std::string& command, const ExpandContext& ctx,
	                  std::string& output, int& status) {
		const ShellProgram shell = resolveShell(make, ctx);
		const NameValueList environment = captureEnvironment(make, ctx);
		int fds[2];
		std::fflush(stdout);
		if (_pipe(fds, 65536, _O_BINARY | _O_NOINHERIT) != 0) return false;

		const int saved = _dup(1);
		_dup2(fds[1], 1);
		_close(fds[1]);
		const HANDLE process = make.exec.spawnWithEnvironment(shell.exe,
			shellCommandLine(shell, command), environment);
		_dup2(saved, 1);
		_close(saved);

		readAllFromFd(fds[0], output);
		_close(fds[0]);
		status = process == INVALID_HANDLE_VALUE
			? kStatusCannotRun
			: shellStatusFromExitCode(waitForExitCode(process));
		return process != INVALID_HANDLE_VALUE;
	}

}  // namespace wbsh::make_detail

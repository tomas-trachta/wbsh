#pragma once

/**
 * @file pty_relay.h
 * @brief Running a command on a pseudoconsole of its own, relayed to the shell's stdio
 *        (script, unbuffer).
 *
 * The command sees a console on all three standard streams wherever the
 * shell's own go, so it behaves as it would interactively. Its screen
 * output, as the VT stream the pseudoconsole renders, is copied to
 * stdout. A console stdin is put in raw VT input mode and its keys go
 * on as a terminal sends them, Ctrl+C among them; any other stdin is
 * read on a thread of its own, its line ends sent as Enter and its end
 * as Ctrl+Z, Enter: a console's end of file. The pseudoconsole takes
 * the size of the shell's console, and follows it.
 */

#include <cstdio>
#include <string>
#include <vector>

namespace wbsh {

	class Executor;

	struct PtyLogs {
		/// Receives the command's output exactly as stdout does.
		std::FILE* output = nullptr;
		/// Receives the bytes read from stdin, before translation.
		std::FILE* input = nullptr;
		bool flush = false;
	};

	struct PtyRun {
		std::vector<std::string> argv;
		bool forward_input = true;
		PtyLogs logs;
	};

	/**
	 * @brief Runs @p run.argv on a new pseudoconsole until it exits.
	 * @param[out] status The command's exit status as the shell reports it.
	 * @return false, already reported, when no command started.
	 */
	bool runOnPseudoConsole(Executor& exec, const PtyRun& run, int& status);

	/// The size of this process's console window, which a pseudoconsole
	/// started now takes; false when the process has no console.
	bool controllingConsoleSize(int& columns, int& rows);

}  // namespace wbsh

#pragma once

/**
 * @file jobcontrol.h
 * @brief Stopping and resuming jobs, and catching Ctrl+Z, on Windows.
 *
 * Windows has no SIGTSTP: the console delivers Ctrl+Z as an ordinary key.
 * While an interactive shell waits for a foreground job it watches the
 * console input for that key, and stops the job by suspending every
 * process of the job object the job was started in.
 */

#ifdef _WIN32

#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>

#include <vector>

namespace wbsh {

	/// A job object whose processes are stopped and resumed together.
	HANDLE createProcessGroup();

	void suspendProcessGroup(HANDLE group);
	void resumeProcessGroup(HANDLE group);

	/// For `kill -STOP` / `kill -CONT` on a bare PID.
	bool suspendProcessById(unsigned long pid);
	bool resumeProcessById(unsigned long pid);

	/// The console this shell reads, or nullptr when stdin is no console.
	HANDLE consoleInput();

	/// Removes Ctrl+Z key presses from the console input buffer and says
	/// whether there was one; all other input stays, in order.
	bool takeCtrlZ(HANDLE console);

	enum class ForegroundEnd { Exited, Stopped, Interrupted };

	/// Waits for @p processes, or until Ctrl+Z is pressed on @p console
	/// (when not null), or for at most @p milliseconds when that is set.
	ForegroundEnd waitForeground(const std::vector<HANDLE>& processes, HANDLE console,
	                             unsigned long milliseconds = INFINITE);

}  // namespace wbsh

#endif /* _WIN32 */

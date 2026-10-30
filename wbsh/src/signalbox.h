#pragma once

/**
 * @file signalbox.h
 * @brief Signals between wbsh processes, which Windows does not deliver.
 *
 * Every wbsh process keeps a mailbox named after its PID in the session's
 * object namespace: a word of pending-signal bits in shared memory and an
 * event set while any bit is. `kill` and its relatives post into it; the
 * shell takes the signals at its safe points (between commands, in `wait`
 * and `sleep`) and runs the trap, ignores them, or ends as the signal's
 * default action would. A PID without a mailbox is no wbsh in this
 * session, and is terminated as before.
 */

namespace wbsh {

	/// This process's mailbox; opened once, before the shell runs anything.
	void openSignalMailbox();

	/// Posts @p signum (1..31) to the wbsh with @p pid; false when it has no mailbox.
	bool postShellSignal(unsigned long pid, int signum);

	/// Bit n set for each signal n posted to this process and not yet taken.
	unsigned long pendingShellSignals();

	/// Takes @p signum off this process's mailbox; false when it was not pending.
	bool takeShellSignal(int signum);

	/// The signals the shell traps or ignores (bit n for signal n). Any other
	/// signal still pending after a short grace, because the shell is busy
	/// in a builtin, ends the process with 128 + n, as an uncaught signal
	/// would.
	void setCaughtShellSignals(unsigned long mask);

	/// Set while any signal is pending (a HANDLE), for waits to wake on;
	/// nullptr when the mailbox could not be opened.
	void* shellSignalEvent();

}  // namespace wbsh

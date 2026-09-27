#pragma once

/**
 * @file interrupt.h
 * @brief The Ctrl+C flag, shared by the interactive loop and the utils.
 *
 * The console handler runs on its own thread and can do nothing but set
 * a flag; whoever is running on the shell's thread looks at it when it
 * gets round to it. The interactive loop consumes it between lines, and
 * a util's command peeks at it while it runs.
 */

namespace wbsh {

	/** Routes Ctrl+C into the flag instead of ending the process. Idempotent. */
	void installCtrlCHandler();

	/** True once Ctrl+C was pressed and nobody has taken it yet. */
	bool ctrlCPending();

	/** Returns whether Ctrl+C was pending, and clears it. */
	bool takeCtrlC();

	/**
	 * @brief Catches Ctrl+C for the life of the object, where the shell
	 *        is not already doing so.
	 *
	 * A script run with -c has no handler, so Ctrl+C ends it; a util's
	 * command wants to be asked instead, and gets that for exactly as
	 * long as it runs. Where the handler was already installed, this
	 * changes nothing.
	 */
	class ScopedCtrlCCapture {
	public:
		ScopedCtrlCCapture();
		~ScopedCtrlCCapture();

		ScopedCtrlCCapture(const ScopedCtrlCCapture&) = delete;
		ScopedCtrlCCapture& operator=(const ScopedCtrlCCapture&) = delete;

	private:
		bool installed_here_ = false;
	};

}  // namespace wbsh

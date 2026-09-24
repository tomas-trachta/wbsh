#pragma once

/**
 * @file pty.h
 * @brief A child process running on a Windows pseudoconsole (ConPTY).
 */

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif /* WIN32_LEAN_AND_MEAN */
#include <windows.h>

#include <string>

namespace wbshterm {

	struct PtySize {
		SHORT columns = 120;
		SHORT rows    = 30;
	};

	/** What to launch and where; an empty directory means "inherit ours". */
	struct ShellCommand {
		std::wstring command_line;
		std::wstring working_directory;
	};

	/**
	 * @brief Owns a pseudoconsole, its pipes, and the child process on it.
	 *
	 * The child is never killed: ending the session only tears down its
	 * console, so a caller that wants an exit status must waitForExit()
	 * first. All methods are callable from any thread except open() and
	 * the two teardown calls, which the owning thread makes alone.
	 */
	class PtySession {
	public:
		PtySession() = default;
		~PtySession();

		PtySession(const PtySession&)            = delete;
		PtySession& operator=(const PtySession&) = delete;

		bool open(const ShellCommand& shell, PtySize size, std::string& out_error);

		bool resize(PtySize size);

		/** Blocks until bytes arrive; returns 0 once the session has ended. */
		DWORD read(char* buffer, DWORD capacity);

		bool write(const char* data, DWORD length);

		bool waitForExit(DWORD timeout_ms, DWORD& out_exit_code);

		bool childAlive() const;

		/** For waiting on the child; the session keeps ownership. */
		HANDLE childHandle() const { return child_; }

		/**
		 * @brief Close the pseudoconsole so pending read() calls return 0.
		 *
		 * Reader threads must be joined between this and close(): read()
		 * only ends once the pseudoconsole is gone, and close() releases
		 * the handle those readers are still blocked on.
		 */
		void endSession();

		void close();

	private:
		bool createPipesAndConsole(PtySize size, std::string& out_error);
		bool spawnChild(const ShellCommand& shell, std::string& out_error);

		HPCON  console_     = nullptr;
		HANDLE input_write_ = INVALID_HANDLE_VALUE;
		HANDLE output_read_ = INVALID_HANDLE_VALUE;
		HANDLE child_       = INVALID_HANDLE_VALUE;
		HANDLE pty_input_read_   = INVALID_HANDLE_VALUE;
		HANDLE pty_output_write_ = INVALID_HANDLE_VALUE;
	};

} /* namespace wbshterm */

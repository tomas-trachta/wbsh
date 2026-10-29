#pragma once

/**
 * @file pseudoconsole.h
 * @brief A Windows pseudoconsole (ConPTY) and the two pipes that feed and drain it.
 *
 * The conpty.dll beside wbsh.exe is preferred to the one built into
 * Windows, as in wbshterm: it is Microsoft's current build, which starts
 * the OpenConsole.exe next to it. Without the file Windows' own serves.
 */

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif /* WIN32_LEAN_AND_MEAN */
#include <windows.h>

#include <string>

namespace wbsh {

	struct PseudoConsoleSize {
		SHORT columns = 80;
		SHORT rows    = 24;
	};

	/**
	 * @brief Owns a pseudoconsole and the shell's ends of its pipes.
	 *
	 * Children are started on it through SpawnPlacement::pseudo_console.
	 * read() and write() may run on threads of their own; open(),
	 * endSession() and close() belong to the owning thread.
	 */
	class PseudoConsole {
	public:
		PseudoConsole() = default;
		~PseudoConsole();

		PseudoConsole(const PseudoConsole&)            = delete;
		PseudoConsole& operator=(const PseudoConsole&) = delete;

		bool open(PseudoConsoleSize size, std::string& out_error);

		bool resize(PseudoConsoleSize size);

		HPCON handle() const { return console_; }

		/** Blocks until output arrives; returns 0 once the session has ended. */
		DWORD read(char* buffer, DWORD capacity);

		/** Keystrokes as a terminal sends them: text, CR for Enter, VT sequences. */
		bool write(const char* data, DWORD length);

		/**
		 * @brief Closes the pseudoconsole, which flushes its last output and
		 *        ends every process still attached to it.
		 *
		 * A reader thread must keep calling read() until it returns 0 and be
		 * joined before close(): the pseudoconsole may wait for its output
		 * to be drained, and close() releases the pipe the reader is on.
		 */
		void endSession();

		void close();

		/** True when the conpty.dll beside wbsh.exe serves, not Windows' own. */
		static bool usesBundledConsole();

	private:
		HPCON  console_      = nullptr;
		HANDLE input_write_  = INVALID_HANDLE_VALUE;
		HANDLE output_read_  = INVALID_HANDLE_VALUE;
	};

}  // namespace wbsh

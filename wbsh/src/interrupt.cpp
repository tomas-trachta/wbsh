/**
 * @file interrupt.cpp
 * @brief The Ctrl+C flag and the console handler that sets it.
 */

#include "interrupt.h"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif /* WIN32_LEAN_AND_MEAN */
#  include <windows.h>
#endif /* _WIN32 */

#include <atomic>

namespace wbsh {

	namespace interrupt_detail {

		static std::atomic<bool> g_pending{ false };
		static bool              g_installed = false;

#ifdef _WIN32
		static BOOL WINAPI ctrlCHandler(DWORD ctrl) {
			if (ctrl == CTRL_C_EVENT || ctrl == CTRL_BREAK_EVENT) {
				g_pending.store(true);
				return TRUE;
			}

			return FALSE;
		}

		static void removeCtrlCHandler() {
			::SetConsoleCtrlHandler(ctrlCHandler, FALSE);
			g_installed = false;
		}
#else
		static void removeCtrlCHandler() {
			g_installed = false;
		}
#endif /* _WIN32 */

	}  // namespace interrupt_detail

	// A parent that ignores Ctrl+C passes that on to its children, and an
	// ignored press never reaches any handler; a shell that means to catch
	// the key has to take that inheritance back first.
	void installCtrlCHandler() {
		if (interrupt_detail::g_installed) return;

#ifdef _WIN32
		::SetConsoleCtrlHandler(nullptr, FALSE);
		::SetConsoleCtrlHandler(interrupt_detail::ctrlCHandler, TRUE);
#endif /* _WIN32 */
		interrupt_detail::g_installed = true;
	}

	bool ctrlCPending() {
		return interrupt_detail::g_pending.load();
	}

	bool takeCtrlC() {
		return interrupt_detail::g_pending.exchange(false);
	}

	ScopedCtrlCCapture::ScopedCtrlCCapture() {
		if (interrupt_detail::g_installed) return;

		installCtrlCHandler();
		installed_here_ = true;
	}

	// A press the command answered is over; one it ignored must not
	// linger to be mistaken for the next.
	ScopedCtrlCCapture::~ScopedCtrlCCapture() {
		if (!installed_here_) return;

		interrupt_detail::removeCtrlCHandler();
		takeCtrlC();
	}

}  // namespace wbsh

#pragma once

/**
 * @file consolekeys.h
 * @brief Characters past the BMP, read whole from the console input queue.
 */

#include <string>

namespace wbsh {

#ifdef _WIN32

	/**
	 * @brief @p first as UTF-8; a high surrogate takes its low half from @p input.
	 *
	 * The low half is the next key-down that carries one. A pseudoconsole
	 * sends each half as a press and a release of its own, so key-ups and
	 * focus or menu events on the way are passed over. Anything else is
	 * left in the queue for the caller, and the high half then stands alone
	 * and comes out as U+FFFD, as it does when nothing follows in time.
	 * @p input is the console input HANDLE.
	 */
	std::string readConsoleCharacter(void* input, wchar_t first);

#endif /* _WIN32 */

} /* namespace wbsh */

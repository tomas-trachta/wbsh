#pragma once

/**
 * @file signals.h
 * @brief Signal names and numbers as Linux has them, which bash scripts and
 *        `kill -l` assume, shared by kill, trap, pkill, killall and timeout.
 */

#include <string>

namespace wbsh {

	/// The highest signal number known by name (31, SIGSYS).
	int signalCount();

	/// "HUP" for 1 and so on; nullptr outside 1..signalCount().
	const char* signalName(int number);

	/**
	 * @brief A signal given as a number (0 included) or a name, with or
	 *        without `SIG` and in any case: `15`, `TERM`, `SIGTERM`, `term`.
	 */
	bool parseSignalSpec(const std::string& text, int& number);

}  // namespace wbsh

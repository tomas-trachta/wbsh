#pragma once

/**
 * @file timefmt.h
 * @brief strftime with the GNU extensions shell scripts use.
 */

#include <ctime>
#include <string>

#include "dateparse.h"

namespace wbsh {

	/**
	 * @brief Format @p epoch as local time with a strftime @p format.
	 *
	 * On top of what the C runtime knows: `%s` (epoch seconds), `%k` / `%l`
	 * (space-padded 24 / 12 hour), `%P` (lowercase am/pm) and the GNU flags
	 * `-` (no padding), `_` (space padding), `0` (zero padding) and `^`
	 * (upper case). A conversion the runtime does not know stays as written.
	 * Empty when @p epoch has no local time (before 1970 on Windows).
	 */
	std::string formatLocalTime(const std::string& format, std::time_t epoch);

	/// `2023-11-14 23:13:20.123456700 +0100`, what `ls --full-time` and `stat` show.
	std::string formatFullIsoTime(const DateValue& value);

}  // namespace wbsh

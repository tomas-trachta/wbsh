#pragma once

/**
 * @file dateparse.h
 * @brief The date strings `touch -d` and `touch -t` take.
 */

#include <ctime>
#include <string>

namespace wbsh {

	/// A point in time: epoch seconds plus nanoseconds.
	struct DateValue {
		long long seconds = 0;
		long nanos = 0;
	};

	/**
	 * @brief Parse a GNU-style date string relative to @p now.
	 *
	 * Understands `@EPOCH[.frac]`, `now` / `today` / `yesterday` /
	 * `tomorrow`, `YYYY-MM-DD`, a time `HH:MM[:SS[.frac]]` (alone, after a
	 * space or after `T`), a zone `Z` / `UTC` / `GMT` / `+HH[:]MM`, and
	 * relative items such as `2 days ago`, `+3 hours` or `1 week`. Without a
	 * zone the time is local.
	 */
	bool parseDateString(const std::string& text, std::time_t now, DateValue& out);

	/// Parse a `touch -t` stamp, `[[CC]YY]MMDDhhmm[.ss]`, as local time.
	bool parseTouchStamp(const std::string& text, std::time_t now, DateValue& out);

}  // namespace wbsh

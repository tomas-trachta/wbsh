#pragma once

/**
 * @file jsonrun.h
 * @brief `wbsh --json`: one run with stdout and stderr captured apart,
 *        reported as a single JSON object on the real stdout.
 *
 * The object is `{"status":N,"stdout":"...","stderr":"...","duration_ms":N}`
 * followed by a newline, in pure ASCII: every non-ASCII character is a
 * `\uXXXX` escape, so the line parses whatever code page its reader
 * decodes it with. Bytes that are not valid UTF-8 become U+FFFD, one per
 * byte. A run ended by a signal or a kill prints nothing.
 */

#include <functional>

namespace wbsh {

	/// Runs @p body with fds 1 and 2 pointed at capture files, prints the
	/// JSON object, and returns @p body's status.
	int runCapturedAsJson(const std::function<int()>& body);

}  // namespace wbsh

#pragma once

/**
 * @file history.h
 * @brief How typed lines become history entries.
 *
 * A command typed over several lines is one history entry, as bash's
 * cmdhist keeps it: each continuation line is joined to the entry with
 * "; ", a space, or -- inside quotes, here-documents and after comments
 * or backslash-newlines -- a literal newline, so that the entry runs the
 * same command when recalled.
 */

#include <string>

namespace wbsh {

	/**
	 * Adds @p line, typed after the not yet complete command @p pending
	 * (its lines joined with newlines), to the history @p entry being
	 * built for that command.
	 */
	void appendHistoryLine(std::string& entry, const std::string& pending, const std::string& line);

	/** True when @p text ends with an unescaped backslash that continues it on the next line. */
	bool endsWithLineContinuation(const std::string& text);

}  // namespace wbsh

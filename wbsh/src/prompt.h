#pragma once

/**
 * @file prompt.h
 * @brief Prompt strings as bash builds them: backslash escapes, then expansion.
 *
 * PS1 and PS2 (and `${x@P}`) first have their escapes decoded -- `\u`,
 * `\w`, `\d`, `\D{fmt}`, `\j`, `\!`, `\#`, `\nnn` and the rest -- and then,
 * with `shopt promptvars` on, go through parameter, command and
 * arithmetic expansion as a double-quoted string. The text the escapes
 * put in is quoted first, so a `$` in a directory name stays a `$`.
 */

#include <cstddef>
#include <string>

namespace wbsh {

	class Environment;
	class Executor;
	class PathConv;

	/** The shell's counters some escapes show. */
	struct PromptCounts {
		std::size_t jobs = 0;             ///< `\j`
		std::size_t history_number = 1;   ///< `\!`: the number the next history entry gets
		std::size_t command_number = 1;   ///< `\#`: commands run in this shell, this one included
	};

	struct PromptRequest {
		PromptCounts counts;
		bool can_redraw = false;      ///< `\g` may show a late git status and be drawn again
		bool for_expansion = false;   ///< quote what escapes insert, for promptvars expansion
	};

	/** @p ps with its prompt escapes decoded. */
	std::string decodePromptEscapes(const std::string& ps, const Environment& env,
	                                const PathConv& conv, const PromptRequest& request);

	/**
	 * A prompt as the shell shows it: escapes decoded, then expanded when
	 * `shopt promptvars` is on. `$?` is the same afterwards.
	 */
	std::string expandPromptString(Executor& exec, const std::string& ps, bool can_redraw);

	/** True when the shell runs elevated, where `\$` is `#` as for root. */
	bool runsElevated();

}  // namespace wbsh

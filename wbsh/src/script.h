#pragma once

/**
 * @file script.h
 * @brief Non-interactive (single-source) execution entry point.
 *
 * Used by the `wbsh -c <cmd>` and `wbsh <file>` CLI paths. Switches
 * between debugging dumps (tokens / AST / expanded words) and the
 * real executor based on the flags passed in.
 */

#include <string>

namespace wbsh {

	/** One non-interactive run: what to read and which of the stages to show. */
	struct ScriptRun {
		std::string source;
		std::string script_name;
		bool show_tokens     = false;
		bool show_ast        = false;
		bool show_expansions = false;
		bool execute         = false;
	};

	/**
	 * Lexes and parses `run.source`, prints whichever dumps were asked
	 * for, then executes when `run.execute` is set. Without execution the
	 * status is 0 unless the lexer or parser reported an error.
	 */
	int runScript(const ScriptRun& run);

}  // namespace wbsh

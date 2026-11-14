#pragma once

/**
 * @file script.h
 * @brief Non-interactive (single-source) execution entry point.
 *
 * Used by the `wbsh -c <cmd>` and `wbsh <file>` CLI paths. Switches
 * between debugging dumps (tokens / AST / expanded words) and the
 * real executor based on the flags passed in.
 */

#include <cstddef>
#include <string>
#include <vector>

#include "startup.h"

namespace wbsh {

	/** One non-interactive run: what to read and which of the stages to show. */
	struct ScriptRun {
		std::string source;
		std::string script_name;              ///< `$0`; empty keeps the shell's name.
		bool from_file = false;               ///< Read from script_name (BASH_SOURCE's `main`).
		bool stream_stdin = false;            ///< Read standard input a command at a time.
		bool command_string = false;          ///< `-c`: syntax errors name the input `-c`.
		std::size_t first_line = 1;           ///< `$LINENO` of the first line of source.
		std::vector<std::string> positional;  ///< `$1`, `$2`, ...
		bool show_tokens     = false;
		bool show_ast        = false;
		bool show_expansions = false;
		bool execute         = false;
		bool read_rc         = false;           ///< `-i` with a script: read the rc file first.
		char invocation_letter = 0;             ///< `c` (-c) or `s` (stdin), last in `$-`.
		ShellStartup startup;
	};

	/**
	 * Lexes and parses `run.source`, prints whichever dumps were asked
	 * for, then executes when `run.execute` is set. Without execution the
	 * status is 0 unless the lexer or parser reported an error.
	 */
	int runScript(const ScriptRun& run);

}  // namespace wbsh

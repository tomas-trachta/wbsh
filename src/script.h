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

	/**
	 * @brief Run wbsh on a single source string in non-interactive mode.
	 *
	 * @param src         Program text to lex / parse / execute.
	 * @param show_tokens Dump the token stream to stderr.
	 * @param show_ast    Pretty-print the AST to stderr.
	 * @param do_expand   Walk the AST and dump fully expanded words.
	 * @param do_run      If true, execute the AST; otherwise stop after
	 *                    the requested dumps.
	 * @param script_name Path used to invoke the script. Surfaced as
	 *                    `$0` inside the program. Empty for `-c`/stdin
	 *                    invocations, which keep the default `wbsh`.
	 *
	 * @return Executor's exit status when @p do_run is true, otherwise
	 *         0 on success or 1 if lex/parse reported errors.
	 */
	int runOnSource(const std::string& src,
	                bool show_tokens,
	                bool show_ast,
	                bool do_expand,
	                bool do_run,
	                const std::string& script_name = "");

}  // namespace wbsh

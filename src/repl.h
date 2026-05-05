#pragma once

/**
 * @file repl.h
 * @brief Interactive read-eval-print loop entry point.
 *
 * Drives the prompt, line editor, lexer, parser, and executor in a
 * loop until the user exits. Console setup (VT mode, immersive dark
 * mode, signal handling) is also performed here.
 */

namespace wbsh {

	/**
	 * @brief Run the interactive REPL.
	 *
	 * Prompts, line-edits, lexes, parses, and executes one logical
	 * command per iteration.
	 *
	 * @return The shell's exit status when the user exits via the
	 *         `exit` builtin, EOF on an empty line, or a `trap EXIT`
	 *         handler that itself calls `exit`.
	 */
	int runInteractive();

}  // namespace wbsh

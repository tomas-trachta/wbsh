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

	int runInteractive();

}  // namespace wbsh

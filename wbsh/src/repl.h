#pragma once

/**
 * @file repl.h
 * @brief Interactive read-eval-print loop entry point.
 *
 * Drives the prompt, line editor, lexer, parser, and executor in a
 * loop until the user exits. Console setup (VT mode, immersive dark
 * mode, signal handling) is also performed here.
 */

#include <string>

namespace wbsh {

	struct ShellStartup;

	/** @p shell_name is `$0` and the error prefix: argv[0]'s base name. */
	int runInteractive(const ShellStartup& startup, const std::string& shell_name);

}  // namespace wbsh

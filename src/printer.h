#pragma once

/**
 * @file printer.h
 * @brief Pretty-printers for the token stream and the AST.
 *
 * Used by the CLI debug flags (`-t`, default-dump-AST) and by the
 * test harness's golden-file mode to produce stable, human-readable
 * dumps of the lexer and parser output.
 */

#include "ast.h"
#include "lexer.h"

#include <ostream>
#include <vector>

namespace wbsh {

	/**
	 * @brief Pretty-print a token vector to @p os.
	 *
	 * One line per token with kind, raw text, and source location.
	 */
	void dumpTokens(std::ostream& os, const std::vector<Token>& tokens);

	/**
	 * @brief Pretty-print an AST subtree rooted at @p node to @p os.
	 *
	 * Recursively walks Pipeline / List / compound-command children;
	 * formats redirections, words, and assignments inline.
	 */
	void dumpAst(std::ostream& os, const Node& node);

}  // namespace wbsh

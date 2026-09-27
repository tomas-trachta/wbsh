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

	void dumpTokens(std::ostream& os, const std::vector<Token>& tokens);

	void dumpAst(std::ostream& os, const Node& node);

}  // namespace wbsh

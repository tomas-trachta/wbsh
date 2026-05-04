#pragma once

#include "ast.h"
#include "lexer.h"

#include <ostream>
#include <vector>

namespace wbsh {

	void dumpTokens(std::ostream& os, const std::vector<Token>& tokens);
	void dumpAst(std::ostream& os, const Node& node);

}  // namespace wbsh

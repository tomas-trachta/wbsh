#pragma once

/**
 * @file funcprint.h
 * @brief A function definition as `declare -f` and `type` print it: bash's
 *        canonical layout, one command per line, four spaces per level.
 */

#include <string>

#include "ast.h"

namespace wbsh {

	/// `name () ` and the body, without a trailing newline.
	std::string functionDefinitionText(const FunctionDef& def);

}  // namespace wbsh

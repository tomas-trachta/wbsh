#pragma once

/**
 * @file funcprint.h
 * @brief A function definition as `declare -f` and `type` print it: bash's
 *        canonical layout, one command per line, four spaces per level.
 */

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "ast.h"

namespace wbsh {

	/// `name () ` and the body, without a trailing newline.
	std::string functionDefinitionText(const FunctionDef& def);

	/// @p node as bash reprints a command, in the layout of a function body.
	std::string commandText(const Node& node);

	using NodeLines = std::vector<std::pair<const Node*, std::size_t>>;

	/// Bash runs a command substitution's body as it reprints it: each node
	/// of @p root with its line in that text, counted from 0.
	NodeLines reprintedSubstitutionLines(const Node& root, const std::string& source);

	/// `bash --pretty-print`: each line's commands as bash reprints them,
	/// a run of blank or comment lines as one empty line.
	std::string prettyPrintedProgram(const Node& root, const std::string& source);

}  // namespace wbsh

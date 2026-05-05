#pragma once

/**
 * @file source.h
 * @brief Source-location helpers shared between the lexer, parser, and
 *        diagnostics layers.
 *
 * Every token, AST node, and error report carries a SourceLoc so the
 * shell can emit messages of the form `file:line:column:` and the
 * executor can slice substrings out of the original program text.
 */

#include <cstddef>
#include <string>

namespace wbsh {

	/**
	 * @brief 1-based source position.
	 *
	 * `line` and `column` are 1-based for human display; `offset` is the
	 * 0-based byte index into the original source string and is what the
	 * parser uses to slice out node text for self-spawned subshells.
	 */
	struct SourceLoc {
		std::size_t line = 1;       ///< 1-based line number.
		std::size_t column = 1;     ///< 1-based column within the line.
		std::size_t offset = 0;     ///< 0-based byte offset into the source.
	};

	/// Format a SourceLoc as `"line:column"` for diagnostic messages.
	inline std::string toString(const SourceLoc& l) {
		return std::to_string(l.line) + ":" + std::to_string(l.column);
	}

}  // namespace wbsh

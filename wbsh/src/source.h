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

	inline std::string toString(const SourceLoc& loc) {
		return std::to_string(loc.line) + ":" + std::to_string(loc.column);
	}

	/**
	 * @brief Strip CR-before-LF from script source, in place.
	 *
	 * Matches MSYS2/Cygwin bash's file-read tolerance for CRLF-saved
	 * scripts. Standalone CRs are preserved so scripts that intentionally
	 * emit CR (e.g. `printf '\r'`) still work. Should be applied to script
	 * bytes that came from disk or a pipe, not to interactive input.
	 */
	inline void normalizeCrlf(std::string& s) {
		std::size_t write = 0;
		for (std::size_t read = 0; read < s.size(); ++read) {
			if (s[read] == '\r' && read + 1 < s.size() && s[read + 1] == '\n') continue;
			s[write++] = s[read];
		}

		s.resize(write);
	}

}  // namespace wbsh

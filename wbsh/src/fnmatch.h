#pragma once

/**
 * @file fnmatch.h
 * @brief fnmatch-like glob pattern matcher: `*`, `?`, `[...]`, `\x`.
 *
 * Shared by pathname expansion, `${var#pat}`-family operators, `case`
 * patterns, and `[[ == ]]`. Quoted pattern text reaches the matcher
 * backslash-escaped via appendFnmatchLiteral(). The range form matches against a substring of the
 * subject without copying it out, which keeps the probe-every-length
 * loops in the expander free of per-probe allocations.
 *
 * Subject and pattern are UTF-8: `?` and a bracket expression match one
 * character, ranges compare code points, and bracket expressions know
 * the POSIX classes (`[[:alpha:]]`), `[=c=]` and `[.c.]`. A `[` without
 * its closing `]` is an ordinary character.
 */

#include <cstddef>
#include <string>

namespace wbsh {

	/// With @p extglob, `?(...)`, `*(...)`, `+(...)`, `@(...)` and `!(...)`
	/// (alternatives separated by `|`) are patterns too.
	bool fnmatchFull(const std::string& p, const std::string& s, bool extglob = false);

	/// fnmatchFull against the substring s[start, start+len), without
	/// the cost of copying it out.
	bool fnmatchRange(const std::string& p, const std::string& s,
	                  std::size_t start, std::size_t len, bool extglob = false);

	inline bool isExtglobOperator(char c) {
		return c == '?' || c == '*' || c == '+' || c == '@' || c == '!';
	}

	// Includes the characters that are only special inside `[...]` or
	// an extglob group, so quoted text spliced into either stays literal.
	inline bool isFnmatchSpecial(char c) {
		return c == '*' || c == '?' || c == '[' || c == ']' || c == '\\'
		    || c == '-' || c == '!' || c == '^'
		    || c == '+' || c == '@' || c == '(' || c == ')' || c == '|';
	}

	inline void appendFnmatchLiteral(std::string& pattern, char c) {
		if (isFnmatchSpecial(c)) pattern.push_back('\\');
		pattern.push_back(c);
	}

	inline void appendFnmatchLiteral(std::string& pattern, const std::string& text) {
		for (const char c : text) appendFnmatchLiteral(pattern, c);
	}

}  // namespace wbsh

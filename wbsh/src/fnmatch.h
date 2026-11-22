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

	/// How a file name's leading `.` matches: bash's FNM_PERIOD (dotglob off)
	/// keeps wildcards off it, FNM_DOTDOT (dotglob on) keeps them off `.` and `..`.
	enum class LeadingDot { Any, Explicit, NotDotDot };

	/// fnmatchFull for a file name in pathname expansion.
	bool fnmatchName(const std::string& p, const std::string& name, bool extglob, LeadingDot dots);

	struct GlobSkipRules {
		bool extglob  = false;
		bool dotglob  = false;
		bool skipdots = false;   ///< globskipdots: `.` and `..` never match
	};

	/// bash's skipname: true when pathname expansion passes over @p name
	/// for @p p before matching, for its leading `.`.
	bool globSkipsName(const std::string& p, const std::string& name, const GlobSkipRules& rules);

	/// The index just past the bracket expression at p[k], or k + 1 when it never closes.
	std::size_t bracketEnd(const std::string& p, std::size_t k);

	/// The index just past the extglob group `X(...)` at p[k], or npos when it never closes.
	std::size_t extglobGroupEnd(const std::string& p, std::size_t k);

	/// What the expander's match loops can skip without calling the matcher.
	struct PatternShape {
		bool literal = false;                      ///< no wildcards at all
		std::string text;                          ///< a literal pattern, unescaped
		std::size_t max_chars = std::string::npos; ///< bound on a match's length, or npos
		int first_byte = -1;                       ///< byte every match starts with, or -1
	};

	PatternShape analyzePattern(const std::string& p, bool extglob = false);

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

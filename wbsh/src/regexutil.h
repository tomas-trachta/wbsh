#pragma once

/**
 * @file regexutil.h
 * @brief Error-as-value adapters around std::regex, which only grep -P
 *        (ECMAScript patterns) still uses; POSIX patterns go through
 *        posixregex.h.
 *
 * std::regex reports invalid patterns (and pathological backtracking
 * during matching) exclusively by throwing std::regex_error — the
 * standard offers no error-code alternative. The two adapters below
 * convert those failures into plain return values so the rest of the
 * codebase stays exception-free; they are the designated exception
 * boundary for the std::regex API and the only try/catch in wbsh.
 */

#include <regex>
#include <string>

namespace wbsh {

	/// False when the pattern is invalid; @p out is then left in an
	/// unspecified valid state.
	inline bool compileRegex(std::regex& out, const std::string& pat,
	                         std::regex::flag_type flags = std::regex::ECMAScript) {
		try {
			out.assign(pat, flags);
			return true;
		} catch (const std::regex_error&) {
			return false;
		}
	}

	enum class RegexOutcome {
		Match,
		NoMatch,
		/// MSVC's recursive matcher gave up (stack or complexity limit).
		Failed,
	};

	/// regex_search over [first, last) with explicit match flags (for
	/// match_prev_avail when the range starts mid-subject).
	inline RegexOutcome searchRegexOutcome(std::string::const_iterator first,
	                                       std::string::const_iterator last, const std::regex& re,
	                                       std::smatch& m,
	                                       std::regex_constants::match_flag_type flags) {
		try {
			return std::regex_search(first, last, m, re, flags) ? RegexOutcome::Match
				: RegexOutcome::NoMatch;
		} catch (const std::regex_error&) {
			return RegexOutcome::Failed;
		}
	}

}  // namespace wbsh

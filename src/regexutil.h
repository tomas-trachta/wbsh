#pragma once

/**
 * @file regexutil.h
 * @brief Error-as-value adapters around std::regex.
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

	/**
	 * @brief regex_search that treats matcher failures as "no match".
	 *
	 * MSVC's matcher throws regex_error on backtracking complexity /
	 * stack limits for adversarial pattern+input pairs; callers here
	 * want "no match" rather than a crash in that case.
	 */
	inline bool searchRegex(const std::string& s, const std::regex& re,
	                        std::smatch* m = nullptr) {
		try {
			if (m) return std::regex_search(s, *m, re);
			return std::regex_search(s, re);
		} catch (const std::regex_error&) {
			return false;
		}
	}

	/**
	 * @brief searchRegex over the [first, last) range of a string.
	 *
	 * Lets scanning loops advance through a subject without copying the
	 * tail on every step. Positions in @p m are relative to @p first.
	 */
	inline bool searchRegex(std::string::const_iterator first,
	                        std::string::const_iterator last,
	                        const std::regex& re, std::smatch* m) {
		try {
			return std::regex_search(first, last, *m, re);
		} catch (const std::regex_error&) {
			return false;
		}
	}

}  // namespace wbsh

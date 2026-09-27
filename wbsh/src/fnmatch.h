#pragma once

/**
 * @file fnmatch.h
 * @brief fnmatch-like glob pattern matcher: `*`, `?`, `[...]`, `\x`.
 *
 * Shared by pathname expansion, `${var#pat}`-family operators, and
 * `case` patterns. The range form matches against a substring of the
 * subject without copying it out, which keeps the probe-every-length
 * loops in the expander free of per-probe allocations.
 */

#include <cstddef>
#include <string>

namespace wbsh {

	namespace fnmatch_detail {

		inline bool matchHere(const std::string& p, std::size_t pi,
		                      const std::string& s, std::size_t si,
		                      std::size_t send);

		// A backslash escapes the next bracket character.
		inline char readBracketChar(const std::string& p, std::size_t& k) {
			char c = p[k++];
			if (c == '\\' && k < p.size()) c = p[k++];
			return c;
		}

		// `-` starts a range unless it sits directly before the closing `]`.
		inline bool atBracketRangeDash(const std::string& p, std::size_t k) {
			return k < p.size() && p[k] == '-' && k + 1 < p.size() && p[k + 1] != ']';
		}

		// Matches `c` against the `[...]` class at p[pi] and moves `pi`
		// past its `]`. A `]` in first position is a member, not the close.
		inline bool matchBracket(const std::string& p, std::size_t& pi, char c) {
			std::size_t k = pi + 1;
			bool negate = false;
			if (k < p.size() && (p[k] == '!' || p[k] == '^')) {
				negate = true;
				++k;
			}

			bool match = false;
			bool first = true;
			while (k < p.size() && (first || p[k] != ']')) {
				const char low = readBracketChar(p, k);
				if (atBracketRangeDash(p, k)) {
					++k;
					const char high = readBracketChar(p, k);
					if (c >= low && c <= high) match = true;
				} else if (c == low) {
					match = true;
				}

				first = false;
			}

			if (k < p.size() && p[k] == ']') ++k;
			pi = k;
			return match != negate;
		}

		// A run of stars is one star; it then tries every split point of
		// the remaining subject against the rest of the pattern.
		inline bool matchStar(const std::string& p, std::size_t pi,
		                      const std::string& s, std::size_t si,
		                      std::size_t send) {
			while (pi < p.size() && p[pi] == '*') ++pi;
			if (pi >= p.size()) return true;

			for (std::size_t k = si; k <= send; ++k) {
				if (matchHere(p, pi, s, k, send)) return true;
			}

			return false;
		}

		inline bool matchHere(const std::string& p, std::size_t pi,
		                      const std::string& s, std::size_t si,
		                      std::size_t send) {
			while (pi < p.size()) {
				const char pc = p[pi];
				if (pc == '*') return matchStar(p, pi, s, si, send);
				if (si >= send) return false;

				if (pc == '[') {
					if (!matchBracket(p, pi, s[si])) return false;
					++si;
					continue;
				}

				if (pc == '\\' && pi + 1 < p.size()) ++pi;
				if (pc != '?' && p[pi] != s[si]) return false;
				++pi;
				++si;
			}

			return si == send;
		}

	}  // namespace fnmatch_detail

	inline bool fnmatchFull(const std::string& p, const std::string& s) {
		return fnmatch_detail::matchHere(p, 0, s, 0, s.size());
	}

	/// fnmatchFull against the substring s[start, start+len), without
	/// the cost of copying it out.
	inline bool fnmatchRange(const std::string& p, const std::string& s,
	                         std::size_t start, std::size_t len) {
		return fnmatch_detail::matchHere(p, 0, s, start, start + len);
	}

}  // namespace wbsh

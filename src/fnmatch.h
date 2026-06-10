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

		inline bool matchBracket(const std::string& p, std::size_t& pi, char c) {
			std::size_t k = pi + 1;
			bool negate = false;
			if (k < p.size() && (p[k] == '!' || p[k] == '^')) { negate = true; ++k; }
			bool match = false;
			bool first = true;
			while (k < p.size() && (first || p[k] != ']')) {
				char a = p[k++];
				if (a == '\\' && k < p.size()) a = p[k++];
				if (k < p.size() && p[k] == '-' && k + 1 < p.size() && p[k + 1] != ']') {
					++k;                        // consume '-'
					char b = p[k++];
					if (b == '\\' && k < p.size()) b = p[k++];
					if (c >= a && c <= b) match = true;
				} else {
					if (c == a) match = true;
				}
				first = false;
			}
			if (k < p.size() && p[k] == ']') ++k;
			pi = k;
			return match != negate;
		}

		inline bool matchHere(const std::string& p, std::size_t pi,
		                      const std::string& s, std::size_t si,
		                      std::size_t send) {
			while (pi < p.size()) {
				char pc = p[pi];
				if (pc == '*') {
					while (pi < p.size() && p[pi] == '*') ++pi;
					if (pi >= p.size()) return true;
					for (std::size_t k = si; k <= send; ++k) {
						if (matchHere(p, pi, s, k, send)) return true;
					}
					return false;
				}
				if (si >= send) return false;
				if (pc == '?') { ++pi; ++si; continue; }
				if (pc == '[') {
					std::size_t newpi = pi;
					if (!matchBracket(p, newpi, s[si])) return false;
					pi = newpi; ++si; continue;
				}
				if (pc == '\\' && pi + 1 < p.size()) {
					++pi;
					if (p[pi] != s[si]) return false;
					++pi; ++si; continue;
				}
				if (pc != s[si]) return false;
				++pi; ++si;
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

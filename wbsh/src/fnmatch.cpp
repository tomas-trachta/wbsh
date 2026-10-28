/**
 * @file fnmatch.cpp
 * @brief The shell's glob pattern matcher over UTF-8 text.
 */

#include "fnmatch.h"

#include "utf8.h"

namespace wbsh {

	namespace fnmatch_detail {

		struct Matcher {
			const std::string& p;
			const std::string& s;
			std::size_t send;
		};

		enum class BracketResult { Literal, Match, NoMatch };

		struct Bracket {
			std::size_t at;
			std::size_t pend;
			char32_t subject;
			bool matched;
		};

		static bool matchHere(const Matcher& m, std::size_t pi, std::size_t pend, std::size_t si);

		static bool isNamedOpen(const std::string& p, std::size_t k, std::size_t pend) {
			if (k + 1 >= pend || p[k] != '[') return false;

			const char kind = p[k + 1];
			return kind == ':' || kind == '=' || kind == '.';
		}

		// A backslash escapes the next bracket character.
		static char32_t readBracketChar(const std::string& p, std::size_t& k, std::size_t pend) {
			if (p[k] == '\\' && k + 1 < pend) ++k;

			const Utf8Char decoded = decodeUtf8(p, k, pend);
			k += decoded.length;
			return decoded.code;
		}

		// `-` starts a range unless it sits directly before the closing `]`.
		static bool atRangeDash(const std::string& p, std::size_t k, std::size_t pend) {
			return k + 1 < pend && p[k] == '-' && p[k + 1] != ']';
		}

		static bool namedItemMatches(char kind, const std::string& name, char32_t c) {
			if (kind == ':') return isInCharClass(name, c);
			if (name.empty()) return false;

			const Utf8Char item = decodeUtf8(name, 0);
			return item.length == name.size() && item.code == c;
		}

		// `[:class:]`, `[=c=]` or `[.c.]` at p[k]; false when it never closes.
		static bool readNamedItem(Bracket& b, const std::string& p) {
			const char kind = p[b.at + 1];
			const char closer[] = { kind, ']', '\0' };
			const std::size_t close = p.find(closer, b.at + 2);
			if (close == std::string::npos || close + 2 > b.pend) return false;

			const std::string name = p.substr(b.at + 2, close - b.at - 2);
			if (namedItemMatches(kind, name, b.subject)) b.matched = true;
			b.at = close + 2;
			return true;
		}

		static void readRangeOrChar(Bracket& b, const std::string& p) {
			const char32_t low = readBracketChar(p, b.at, b.pend);
			if (!atRangeDash(p, b.at, b.pend)) {
				if (b.subject == low) b.matched = true;
				return;
			}

			++b.at;
			const char32_t high = readBracketChar(p, b.at, b.pend);
			if (b.subject >= low && b.subject <= high) b.matched = true;
		}

		// Matches the subject character against the `[...]` at p[pi] and
		// sets `next` past its `]`. A `]` in first position is a member.
		static BracketResult matchBracket(const Matcher& m, std::size_t pi, std::size_t pend,
				char32_t subject, std::size_t& next) {
			Bracket b{ pi + 1, pend, subject, false };
			const bool negate = b.at < pend && (m.p[b.at] == '!' || m.p[b.at] == '^');
			if (negate) ++b.at;

			bool first = true;
			while (b.at < pend) {
				if (m.p[b.at] == ']' && !first) {
					next = b.at + 1;
					return b.matched != negate ? BracketResult::Match : BracketResult::NoMatch;
				}

				first = false;
				if (isNamedOpen(m.p, b.at, pend) && readNamedItem(b, m.p)) continue;
				readRangeOrChar(b, m.p);
			}

			return BracketResult::Literal;
		}

		// The byte a pattern position must match literally, or -1 when
		// it is a wildcard.
		static int literalByteAt(const std::string& p, std::size_t pi, std::size_t pend) {
			const char c = p[pi];
			if (c == '*' || c == '?' || c == '[') return -1;
			if (c == '\\' && pi + 1 < pend) return static_cast<unsigned char>(p[pi + 1]);
			return static_cast<unsigned char>(c);
		}

		// A run of stars is one star; it then tries every character
		// boundary of the remaining subject against the rest of the
		// pattern, skipping those where a literal next byte cannot match.
		static bool matchStar(const Matcher& m, std::size_t pi, std::size_t pend, std::size_t si) {
			while (pi < pend && m.p[pi] == '*') ++pi;
			if (pi >= pend) return true;

			const int next_byte = literalByteAt(m.p, pi, pend);
			for (std::size_t k = si;; k += utf8CharLength(m.s, k, m.send)) {
				const bool viable = next_byte < 0
				    || (k < m.send && static_cast<unsigned char>(m.s[k]) == next_byte);
				if (viable && matchHere(m, pi, pend, k)) return true;
				if (k >= m.send) return false;
			}
		}

		// The bracket at p[pi] consumed one subject character, failed,
		// or turned out to be a literal `[`.
		static BracketResult stepBracket(const Matcher& m, std::size_t& pi, std::size_t pend,
				std::size_t& si) {
			const Utf8Char c = decodeUtf8(m.s, si, m.send);
			std::size_t next = pi;
			const BracketResult result = matchBracket(m, pi, pend, c.code, next);
			if (result != BracketResult::Match) return result;

			pi = next;
			si += c.length;
			return result;
		}

		static bool matchHere(const Matcher& m, std::size_t pi, std::size_t pend, std::size_t si) {
			while (pi < pend) {
				const char pc = m.p[pi];
				if (pc == '*') return matchStar(m, pi, pend, si);
				if (si >= m.send) return false;

				if (pc == '[') {
					const BracketResult result = stepBracket(m, pi, pend, si);
					if (result == BracketResult::NoMatch) return false;
					if (result == BracketResult::Match) continue;
				}

				if (pc == '?') {
					si += utf8CharLength(m.s, si, m.send);
					++pi;
					continue;
				}

				if (pc == '\\' && pi + 1 < pend) ++pi;
				if (m.p[pi] != m.s[si]) return false;
				++pi;
				++si;
			}

			return si == m.send;
		}

	}  // namespace fnmatch_detail

	bool fnmatchFull(const std::string& p, const std::string& s) {
		const fnmatch_detail::Matcher m{ p, s, s.size() };
		return fnmatch_detail::matchHere(m, 0, p.size(), 0);
	}

	bool fnmatchRange(const std::string& p, const std::string& s,
	                  std::size_t start, std::size_t len) {
		const fnmatch_detail::Matcher m{ p, s, start + len };
		return fnmatch_detail::matchHere(m, 0, p.size(), start);
	}

}  // namespace wbsh

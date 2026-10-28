/**
 * @file fnmatch.cpp
 * @brief The shell's glob pattern matcher over UTF-8 text.
 */

#include "fnmatch.h"

#include <vector>

#include "utf8.h"

namespace wbsh {

	namespace fnmatch_detail {

		struct Matcher {
			const std::string& p;
			const std::string& s;
			std::size_t send;
			bool extglob;
		};

		enum class BracketResult { Literal, Match, NoMatch };

		struct Bracket {
			std::size_t at;
			std::size_t pend;
			char32_t subject;
			bool matched;
		};

		struct Span {
			std::size_t begin;
			std::size_t end;
		};

		// An extglob `X(alt|alt...)`: the operator, its alternatives and
		// the pattern index just past the closing `)`.
		struct Group {
			char op;
			std::vector<Span> alts;
			std::size_t after;
		};

		static bool matchHere(const Matcher& m, std::size_t pi, std::size_t pend, std::size_t si);
		static bool matchGroup(const Matcher& m, const Group& g, std::size_t pend, std::size_t si);
		static std::size_t maxMatchChars(const Matcher& m, std::size_t pi, std::size_t pend);

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
		static BracketResult matchBracket(const std::string& p, std::size_t pi, std::size_t pend,
				char32_t subject, std::size_t& next) {
			Bracket b{ pi + 1, pend, subject, false };
			const bool negate = b.at < pend && (p[b.at] == '!' || p[b.at] == '^');
			if (negate) ++b.at;

			bool first = true;
			while (b.at < pend) {
				if (p[b.at] == ']' && !first) {
					next = b.at + 1;
					return b.matched != negate ? BracketResult::Match : BracketResult::NoMatch;
				}

				first = false;
				if (isNamedOpen(p, b.at, pend) && readNamedItem(b, p)) continue;
				readRangeOrChar(b, p);
			}

			return BracketResult::Literal;
		}

		// Index just past the bracket expression at p[k], or past the `[`
		// alone when it never closes.
		static std::size_t skipBracket(const std::string& p, std::size_t k, std::size_t pend) {
			std::size_t next = k + 1;
			if (matchBracket(p, k, pend, 0, next) == BracketResult::Literal) return k + 1;
			return next;
		}

		static bool isExtglobAt(const Matcher& m, std::size_t pi, std::size_t pend) {
			return m.extglob && pi + 1 < pend && isExtglobOperator(m.p[pi]) && m.p[pi + 1] == '(';
		}

		static void closeAlternative(Group& g, std::size_t& alt_start, std::size_t k) {
			g.alts.push_back({ alt_start, k });
			alt_start = k + 1;
		}

		// Splits the `X(...)` at p[pi] into its alternatives; false when
		// the parenthesis never closes, so the operator is literal.
		static bool parseGroup(const std::string& p, std::size_t pi, std::size_t pend, Group& g) {
			g.op = p[pi];
			std::size_t alt_start = pi + 2;
			int depth = 0;
			for (std::size_t k = alt_start; k < pend;) {
				const char c = p[k];
				if (c == '\\') {
					k += 2;
					continue;
				}

				if (c == '[') {
					k = skipBracket(p, k, pend);
					continue;
				}

				if (c == ')' && depth == 0) {
					closeAlternative(g, alt_start, k);
					g.after = k + 1;
					return true;
				}

				if (c == '(') ++depth;
				if (c == ')') --depth;
				if (c == '|' && depth == 0) closeAlternative(g, alt_start, k);
				++k;
			}

			return false;
		}

		static bool anyAlternativeMatches(const Matcher& m, const Group& g, std::size_t from,
				std::size_t to) {
			const Matcher sub{ m.p, m.s, to, m.extglob };
			for (const Span& alt : g.alts) {
				if (matchHere(sub, alt.begin, alt.end, from)) return true;
			}

			return false;
		}

		static std::size_t nextBoundary(const Matcher& m, std::size_t k) {
			return k + utf8CharLength(m.s, k, m.send);
		}

		static bool matchOnce(const Matcher& m, const Group& g, std::size_t pend, std::size_t si) {
			for (std::size_t k = si;; k = nextBoundary(m, k)) {
				const bool matched = anyAlternativeMatches(m, g, si, k);
				if (matched && matchHere(m, g.after, pend, k)) return true;
				if (k >= m.send) return false;
			}
		}

		// One or more alternatives back to back, then the rest; only a
		// non-empty match may repeat, so the recursion always advances.
		// The most characters one alternative of @p g can match, or npos.
		static std::size_t widestAlternative(const Matcher& m, const Group& g) {
			std::size_t widest = 0;
			for (const Span& alt : g.alts) {
				const std::size_t chars = maxMatchChars(m, alt.begin, alt.end);
				if (chars == std::string::npos) return chars;
				if (chars > widest) widest = chars;
			}

			return widest;
		}

		struct Repetition {
			std::vector<bool> reached;
			std::vector<std::size_t> pending;
		};

		// Marks `end` reached by one more alternative; true once the rest
		// of the pattern matches from there.
		static bool reachRepetitionEnd(const Matcher& m, const Group& g, std::size_t pend,
				std::size_t si, std::size_t end, Repetition& rep) {
			if (rep.reached[end - si]) return false;

			rep.reached[end - si] = true;
			rep.pending.push_back(end);
			return matchHere(m, g.after, pend, end);
		}

		// One or more alternatives back to back, then the rest. Explores
		// every reachable end once, without recursing per repetition, so
		// long subjects cannot exhaust the stack.
		static bool matchRepeated(const Matcher& m, const Group& g, std::size_t pend,
				std::size_t si) {
			const std::size_t reach = widestAlternative(m, g);
			Repetition rep{ std::vector<bool>(m.send - si + 1, false), { si } };
			while (!rep.pending.empty()) {
				const std::size_t from = rep.pending.back();
				rep.pending.pop_back();

				std::size_t chars = 0;
				for (std::size_t k = from;; k = nextBoundary(m, k), ++chars) {
					if (reach != std::string::npos && chars > reach) break;

					const bool matched = anyAlternativeMatches(m, g, from, k);
					if (matched && reachRepetitionEnd(m, g, pend, si, k, rep)) return true;
					if (k >= m.send) break;
				}
			}

			return false;
		}

		static bool matchNegated(const Matcher& m, const Group& g, std::size_t pend,
				std::size_t si) {
			for (std::size_t k = si;; k = nextBoundary(m, k)) {
				const bool matched = anyAlternativeMatches(m, g, si, k);
				if (!matched && matchHere(m, g.after, pend, k)) return true;
				if (k >= m.send) return false;
			}
		}

		static bool matchGroup(const Matcher& m, const Group& g, std::size_t pend, std::size_t si) {
			switch (g.op) {
			case '@':
				return matchOnce(m, g, pend, si);
			case '?':
				return matchHere(m, g.after, pend, si) || matchOnce(m, g, pend, si);
			case '*':
				return matchHere(m, g.after, pend, si) || matchRepeated(m, g, pend, si);
			case '+':
				return matchRepeated(m, g, pend, si);
			case '!':
				return matchNegated(m, g, pend, si);
			default:
				return false;
			}
		}

		// The byte a pattern position must match literally, or -1 when
		// it is a wildcard.
		static int literalByteAt(const Matcher& m, std::size_t pi, std::size_t pend) {
			const char c = m.p[pi];
			if (c == '*' || c == '?' || c == '[' || isExtglobAt(m, pi, pend)) return -1;
			if (c == '\\' && pi + 1 < pend) return static_cast<unsigned char>(m.p[pi + 1]);
			return static_cast<unsigned char>(c);
		}

		// A run of stars is one star; it then tries every character
		// boundary of the remaining subject against the rest of the
		// pattern, skipping those where a literal next byte cannot match.
		static bool matchStar(const Matcher& m, std::size_t pi, std::size_t pend, std::size_t si) {
			while (pi < pend && m.p[pi] == '*' && !isExtglobAt(m, pi, pend)) ++pi;
			if (pi >= pend) return true;

			const int next_byte = literalByteAt(m, pi, pend);
			for (std::size_t k = si;; k = nextBoundary(m, k)) {
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
			const BracketResult result = matchBracket(m.p, pi, pend, c.code, next);
			if (result != BracketResult::Match) return result;

			pi = next;
			si += c.length;
			return result;
		}

		static bool matchHere(const Matcher& m, std::size_t pi, std::size_t pend, std::size_t si) {
			while (pi < pend) {
				const char pc = m.p[pi];
				Group group;
				if (isExtglobAt(m, pi, pend) && parseGroup(m.p, pi, pend, group)) {
					return matchGroup(m, group, pend, si);
				}

				if (pc == '*') return matchStar(m, pi, pend, si);
				if (si >= m.send) return false;

				if (pc == '[') {
					const BracketResult result = stepBracket(m, pi, pend, si);
					if (result == BracketResult::NoMatch) return false;
					if (result == BracketResult::Match) continue;
				}

				if (pc == '?') {
					si = nextBoundary(m, si);
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

		static std::size_t maxGroupChars(const Matcher& m, const Group& g) {
			if (g.op == '*' || g.op == '+' || g.op == '!') return std::string::npos;
			return widestAlternative(m, g);
		}

		// An upper bound on the characters p[pi, pend) can match, or npos
		// when a `*` (or a repeating extglob group) makes it unbounded.
		// Literal bytes count one each, which over-counts multibyte ones.
		static std::size_t maxMatchChars(const Matcher& m, std::size_t pi, std::size_t pend) {
			std::size_t total = 0;
			while (pi < pend) {
				Group group;
				if (isExtglobAt(m, pi, pend) && parseGroup(m.p, pi, pend, group)) {
					const std::size_t chars = maxGroupChars(m, group);
					if (chars == std::string::npos) return chars;

					total += chars;
					pi = group.after;
					continue;
				}

				const char c = m.p[pi];
				if (c == '*') return std::string::npos;

				if (c == '[') pi = skipBracket(m.p, pi, pend);
				else pi += (c == '\\' && pi + 1 < pend) ? 2 : 1;
				++total;
			}

			return total;
		}

		static bool literalText(const Matcher& m, std::string& out) {
			for (std::size_t pi = 0; pi < m.p.size(); ++pi) {
				const char c = m.p[pi];
				if (literalByteAt(m, pi, m.p.size()) < 0) return false;
				if (c == '\\' && pi + 1 < m.p.size()) ++pi;
				out.push_back(m.p[pi]);
			}

			return true;
		}

	}  // namespace fnmatch_detail

	PatternShape analyzePattern(const std::string& p, bool extglob) {
		const std::string none;
		const fnmatch_detail::Matcher m{ p, none, 0, extglob };
		PatternShape shape;
		shape.literal = fnmatch_detail::literalText(m, shape.text);
		shape.max_chars = fnmatch_detail::maxMatchChars(m, 0, p.size());
		if (!p.empty()) shape.first_byte = fnmatch_detail::literalByteAt(m, 0, p.size());
		return shape;
	}

	bool fnmatchFull(const std::string& p, const std::string& s, bool extglob) {
		const fnmatch_detail::Matcher m{ p, s, s.size(), extglob };
		return fnmatch_detail::matchHere(m, 0, p.size(), 0);
	}

	bool fnmatchRange(const std::string& p, const std::string& s,
	                  std::size_t start, std::size_t len, bool extglob) {
		const fnmatch_detail::Matcher m{ p, s, start + len, extglob };
		return fnmatch_detail::matchHere(m, 0, p.size(), start);
	}

}  // namespace wbsh

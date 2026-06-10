/**
 * @file expander.cpp
 * @brief Word-expansion pipeline implementation: brace, tilde,
 *        parameter, command, arithmetic, ANSI-C, splitting, globbing,
 *        quote removal.
 */

#include "expander.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif /* _WIN32 */

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "lexer.h"
#include "numparse.h"

namespace wbsh {

	// ---------------------------------------------------------------------
	// fnmatch-like glob pattern matcher. Supports * ? [...].
	// ---------------------------------------------------------------------

	static bool matchHere(const std::string& p, std::size_t pi,
	               const std::string& s, std::size_t si);

	static bool matchBracket(const std::string& p, std::size_t& pi, char c) {
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

	static bool matchHere(const std::string& p, std::size_t pi,
	               const std::string& s, std::size_t si) {
		while (pi < p.size()) {
			char pc = p[pi];
			if (pc == '*') {
				while (pi < p.size() && p[pi] == '*') ++pi;
				if (pi >= p.size()) return true;
				for (std::size_t k = si; k <= s.size(); ++k) {
					if (matchHere(p, pi, s, k)) return true;
				}
				return false;
			}
			if (si >= s.size()) return false;
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
		return si == s.size();
	}

	static bool fnmatchFull(const std::string& p, const std::string& s) {
		return matchHere(p, 0, s, 0);
	}

	// ---------------------------------------------------------------------
	// Inline $-expansion for heredoc bodies and ${param-default} defaults.
	// Performs $name, ${...}, $(...), $((...)), `...`, and backslash
	// escapes (\$ \\ \` \"). Tilde, splitting, globbing are NOT applied.
	// ---------------------------------------------------------------------

	static bool isNameStart(char c) {
		return c == '_' || std::isalpha(static_cast<unsigned char>(c));
	}
	static bool isNameCont(char c) {
		return c == '_' || std::isalnum(static_cast<unsigned char>(c));
	}

	// ---------------------------------------------------------------------
	// Arithmetic evaluator (recursive descent).
	// ---------------------------------------------------------------------

	class ArithEval {
	public:
		ArithEval(const std::string& src, Environment& env, Expander* outer, int depth)
			: src_(src), env_(env), outer_(outer), depth_(depth) {}

		long long run() {
			skipWs();
			if (eof()) return 0;
			return parseComma();
		}

	private:
		long long parseComma() {
			long long r = parseAssign();
			while (consume(',')) r = parseAssign();
			return r;
		}
		long long parseAssign() {
			std::size_t save = pos_;
			skipWs();
			if (!eof() && (isNameStart(peek()))) {
				std::size_t saved2 = pos_;
				std::string name = readIdent();
				skipWs();
				struct Op { const char* s; int len; };
				static const Op ops[] = {
					{"<<=", 3}, {">>=", 3},
					{"+=", 2}, {"-=", 2}, {"*=", 2}, {"/=", 2}, {"%=", 2},
					{"&=", 2}, {"^=", 2}, {"|=", 2},
					{"=", 1},
				};
				for (auto& o : ops) {
					if (pos_ + o.len > src_.size()) continue;
					if (src_.compare(pos_, o.len, o.s) != 0) continue;
					// Don't grab '==' or '<=' etc. when looking for '='.
					if (o.len == 1 && pos_ + 1 < src_.size() && src_[pos_ + 1] == '=') continue;
					pos_ += o.len;
					long long rhs = parseAssign();
					long long lhs = (o.len == 1) ? 0 : env_get(name);
					long long val = rhs;
					switch (o.s[0]) {
					case '=': val = rhs; break;
					case '+': val = lhs + rhs; break;
					case '-': val = lhs - rhs; break;
					case '*': val = lhs * rhs; break;
					case '/': val = rhs ? lhs / rhs : 0; break;
					case '%': val = rhs ? lhs % rhs : 0; break;
					case '&': val = lhs & rhs; break;
					case '^': val = lhs ^ rhs; break;
					case '|': val = lhs | rhs; break;
					case '<': val = lhs << rhs; break;
					case '>': val = lhs >> rhs; break;
					}
					env_.set(name, std::to_string(val));
					return val;
				}
				pos_ = saved2;
			}
			pos_ = save;
			return parseTernary();
		}
		long long parseTernary() {
			long long c = parseLogOr();
			if (consume('?')) {
				long long a = parseAssign();
				consume(':');
				long long b = parseAssign();
				return c ? a : b;
			}
			return c;
		}
		long long parseLogOr() {
			long long l = parseLogAnd();
			while (consume("||")) {
				long long r = parseLogAnd();
				l = (l || r) ? 1 : 0;
			}
			return l;
		}
		long long parseLogAnd() {
			long long l = parseBitOr();
			while (consume("&&")) {
				long long r = parseBitOr();
				l = (l && r) ? 1 : 0;
			}
			return l;
		}
		long long parseBitOr() {
			long long l = parseBitXor();
			while (true) {
				skipWs();
				if (peek() == '|' && peek(1) != '|' && peek(1) != '=') {
					++pos_;
					long long r = parseBitXor();
					l |= r;
				} else break;
			}
			return l;
		}
		long long parseBitXor() {
			long long l = parseBitAnd();
			while (true) {
				skipWs();
				if (peek() == '^' && peek(1) != '=') {
					++pos_;
					long long r = parseBitAnd();
					l ^= r;
				} else break;
			}
			return l;
		}
		long long parseBitAnd() {
			long long l = parseEq();
			while (true) {
				skipWs();
				if (peek() == '&' && peek(1) != '&' && peek(1) != '=') {
					++pos_;
					long long r = parseEq();
					l &= r;
				} else break;
			}
			return l;
		}
		long long parseEq() {
			long long l = parseRel();
			while (true) {
				skipWs();
				if (consume("==")) { long long r = parseRel(); l = (l == r) ? 1 : 0; }
				else if (consume("!=")) { long long r = parseRel(); l = (l != r) ? 1 : 0; }
				else break;
			}
			return l;
		}
		long long parseRel() {
			long long l = parseShift();
			while (true) {
				skipWs();
				if (consume("<=")) { long long r = parseShift(); l = (l <= r) ? 1 : 0; }
				else if (consume(">=")) { long long r = parseShift(); l = (l >= r) ? 1 : 0; }
				else if (peek() == '<' && peek(1) != '<' && peek(1) != '=') {
					++pos_; long long r = parseShift(); l = (l < r) ? 1 : 0;
				} else if (peek() == '>' && peek(1) != '>' && peek(1) != '=') {
					++pos_; long long r = parseShift(); l = (l > r) ? 1 : 0;
				} else break;
			}
			return l;
		}
		long long parseShift() {
			long long l = parseAdd();
			while (true) {
				skipWs();
				if (peek() == '<' && peek(1) == '<' && peek(2) != '=') {
					pos_ += 2;
					long long r = parseAdd(); l = l << r;
				} else if (peek() == '>' && peek(1) == '>' && peek(2) != '=') {
					pos_ += 2;
					long long r = parseAdd(); l = l >> r;
				} else break;
			}
			return l;
		}
		long long parseAdd() {
			long long l = parseMul();
			while (true) {
				skipWs();
				if (peek() == '+' && peek(1) != '+' && peek(1) != '=') {
					++pos_; long long r = parseMul(); l = l + r;
				} else if (peek() == '-' && peek(1) != '-' && peek(1) != '=') {
					++pos_; long long r = parseMul(); l = l - r;
				} else break;
			}
			return l;
		}
		long long parseMul() {
			long long l = parsePow();
			while (true) {
				skipWs();
				if (peek() == '*' && peek(1) != '*' && peek(1) != '=') {
					++pos_; long long r = parsePow(); l = l * r;
				} else if (peek() == '/' && peek(1) != '=') {
					++pos_; long long r = parsePow(); l = r ? l / r : 0;
				} else if (peek() == '%' && peek(1) != '=') {
					++pos_; long long r = parsePow(); l = r ? l % r : 0;
				} else break;
			}
			return l;
		}
		long long parsePow() {
			long long l = parseUnary();
			skipWs();
			if (consume("**")) {
				long long r = parsePow();   // right-assoc
				long long res = 1;
				if (r < 0) return 0;
				for (long long k = 0; k < r; ++k) res *= l;
				return res;
			}
			return l;
		}
		long long parseUnary() {
			skipWs();
			if (consume("++")) {
				std::string n = readIdent();
				long long v = env_get(n) + 1;
				env_.set(n, std::to_string(v));
				return v;
			}
			if (consume("--")) {
				std::string n = readIdent();
				long long v = env_get(n) - 1;
				env_.set(n, std::to_string(v));
				return v;
			}
			if (consume('+')) return parseUnary();
			if (consume('-')) return -parseUnary();
			if (consume('!')) return parseUnary() ? 0 : 1;
			if (consume('~')) return ~parseUnary();
			return parsePrimary();
		}
		long long parsePrimary() {
			skipWs();
			if (eof()) return 0;
			if (consume('(')) {
				long long v = parseComma();
				consume(')');
				return v;
			}
			if (std::isdigit(static_cast<unsigned char>(peek()))) {
				return readNumber();
			}
			if (isNameStart(peek())) {
				std::string n = readIdent();
				skipWs();
				if (consume("++")) {
					long long v = env_get(n);
					env_.set(n, std::to_string(v + 1));
					return v;
				}
				if (consume("--")) {
					long long v = env_get(n);
					env_.set(n, std::to_string(v - 1));
					return v;
				}
				return env_get(n);
			}
			if (consume('$')) {
				if (isNameStart(peek())) {
					std::string n = readIdent();
					return env_get(n);
				}
				if (std::isdigit(static_cast<unsigned char>(peek()))) {
					std::string num;
					while (std::isdigit(static_cast<unsigned char>(peek()))) {
						num.push_back(advance());
					}
					return env_get(num);
				}
				return 0;
			}
			++pos_;   // skip unrecognized char
			return 0;
		}

		long long env_get(const std::string& name) {
			if (name.empty()) return 0;
			std::string v = env_.get(name);
			if (v.empty()) return 0;
			long long iv = 0;
			std::size_t idx = 0;
			if (parseLL(v, iv, 0, &idx) && idx == v.size()) return iv;
			if (depth_ > 16) return 0;   // recursion guard
			ArithEval inner(v, env_, outer_, depth_ + 1);
			return inner.run();
		}

		long long readNumber() {
			if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
				pos_ += 2;
				long long v = 0;
				while (std::isxdigit(static_cast<unsigned char>(peek()))) {
					char c = advance();
					int d = std::isdigit(static_cast<unsigned char>(c))
					        ? c - '0'
					        : std::tolower(static_cast<unsigned char>(c)) - 'a' + 10;
					v = v * 16 + d;
				}
				return v;
			}
			if (peek() == '0' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
				++pos_;   // leading 0 → octal
				long long v = 0;
				while (peek() >= '0' && peek() <= '7') {
					v = v * 8 + (advance() - '0');
				}
				return v;
			}
			long long v = 0;
			while (std::isdigit(static_cast<unsigned char>(peek()))) {
				v = v * 10 + (advance() - '0');
			}
			if (peek() == '#') {
				++pos_;
				int base = static_cast<int>(v);
				if (base < 2) base = 10;
				v = 0;
				while (true) {
					char c = peek();
					int d;
					if (c >= '0' && c <= '9') d = c - '0';
					else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
					else if (c >= 'A' && c <= 'Z') d = c - 'A' + (base > 36 ? 36 : 10);
					else if (c == '@') d = 62;
					else if (c == '_') d = 63;
					else break;
					if (d >= base) break;
					v = v * base + d;
					advance();
				}
			}
			return v;
		}

		std::string readIdent() {
			skipWs();
			std::size_t start = pos_;
			if (!eof() && isNameStart(peek())) {
				advance();
				while (!eof() && isNameCont(peek())) advance();
			}
			return src_.substr(start, pos_ - start);
		}

		void skipWs() {
			while (!eof() && std::isspace(static_cast<unsigned char>(peek()))) ++pos_;
		}
		bool eof() const { return pos_ >= src_.size(); }
		char peek(std::size_t n = 0) const {
			return pos_ + n < src_.size() ? src_[pos_ + n] : '\0';
		}
		char advance() { return src_[pos_++]; }
		bool consume(char c) {
			skipWs();
			if (peek() == c) { ++pos_; return true; }
			return false;
		}
		bool consume(const char* s) {
			skipWs();
			std::size_t L = std::strlen(s);
			if (pos_ + L > src_.size()) return false;
			if (src_.compare(pos_, L, s) != 0) return false;
			pos_ += L;
			return true;
		}

		const std::string& src_;
		std::size_t pos_ = 0;
		Environment& env_;
		Expander* outer_;
		int depth_;
	};

	// ---------------------------------------------------------------------------
	// Expander construction & arithmetic facade
	// ---------------------------------------------------------------------------

	Expander::Expander(Environment& env, CommandSubstitutor* sub)
		: env_(env), sub_(sub) {}

	long long Expander::evalArith(const std::string& body) {
		// Pre-pass: expand $-forms inside the body (bash recurses into $((...)) bodies).
		std::string expanded = expandHeredoc(body, /*quoted=*/false);
		if (aborting()) return 0;   // error / control flow pending — caller checks
		ArithEval ev(expanded, env_, this, 0);
		return ev.run();
	}

	// ---------------------------------------------------------------------------
	// Inline $-substitution (used for heredocs, default-values, arith pre-pass)
	// ---------------------------------------------------------------------------

	// Find the index just past the matching `}` for `${...}` starting at `i+2`.
	// `i` points at the `$`; `body[i+1]` must be `{`. Counts nested braces.
	static std::size_t scanBalancedBraces(const std::string& body, std::size_t after_open) {
		std::size_t k = after_open;
		int depth = 1;
		while (k < body.size() && depth > 0) {
			if (body[k] == '{') ++depth;
			else if (body[k] == '}') --depth;
			if (depth > 0) ++k;
		}
		return k;
	}

	// Find the index of the matching `)` for `$(...)` starting at `after_open`.
	// `start_depth` is the nesting depth on entry (1 for `$(...)`, 2 for `$((...))`).
	static std::size_t scanBalancedParens(const std::string& body, std::size_t after_open,
	                                      int start_depth) {
		std::size_t k = after_open;
		int depth = start_depth;
		while (k < body.size() && depth > 0) {
			if (body[k] == '(') ++depth;
			else if (body[k] == ')') --depth;
			if (depth > 0) ++k;
		}
		return k;
	}

	// Find the index of the matching `]` for `$[...]` starting at `after_open`.
	// Mirror of scanBalancedParens: nested `[` increments the depth.
	static std::size_t scanBalancedBrackets(const std::string& body,
	                                        std::size_t after_open) {
		std::size_t k = after_open;
		int depth = 1;
		while (k < body.size() && depth > 0) {
			if (body[k] == '[') ++depth;
			else if (body[k] == ']') --depth;
			if (depth > 0) ++k;
		}
		return k;
	}

	// Apply backslash-escape rules used in unquoted heredoc bodies (and the
	// inside of double-quoted strings). Returns true if `body[i]` was a
	// recognised escape; on true, `i` is advanced and characters may be
	// appended to `out`.
	static bool consumeHeredocBackslash(const std::string& body, std::size_t& i,
	                                    std::string& out) {
		if (body[i] != '\\' || i + 1 >= body.size()) return false;
		const char nx = body[i + 1];
		if (nx == '\\' || nx == '$' || nx == '`' || nx == '"') {
			out.push_back(nx);
			i += 2;
			return true;
		}
		if (nx == '\n') {
			// Line continuation — drop both bytes.
			i += 2;
			return true;
		}
		// Other backslash sequences pass through literally; consume only
		// the backslash itself so the next character is reprocessed.
		out.push_back('\\');
		++i;
		return true;
	}

	// `$(...)` plain command substitution: capture stdout, strip trailing
	// newlines, append. `i` is at `$`; body[i+1] must be `(` and body[i+2]
	// must NOT be `(` (use the arith-exp branch for `$((...))`).
	void Expander::expandHeredocCmdSubst(const std::string& body, std::size_t& i,
	                                     std::string& out)
	{
		const std::size_t end = body.size();
		const std::size_t k = scanBalancedParens(body, i + 2, /*start_depth=*/1);
		const std::string inner = body.substr(i + 2, k - (i + 2));
		std::string r = runCmdSubst(inner);
		while (!r.empty() && r.back() == '\n') r.pop_back();
		out += r;
		i = (k < end) ? k + 1 : k;
	}

	// `$((...))` arithmetic expansion. Caller has verified body[i+2]=='('.
	void Expander::expandHeredocArith(const std::string& body, std::size_t& i,
	                                  std::string& out)
	{
		const std::size_t end = body.size();
		const std::size_t k = scanBalancedParens(body, i + 3, /*start_depth=*/2);
		// k is at the second ')'. The inner expression ends one before that.
		const std::size_t inner_end = (k > 0) ? k - 1 : 0;
		const std::string inner = body.substr(i + 3, inner_end - (i + 3));
		const long long v = evalArith(inner);
		out += std::to_string(v);
		i = (k < end) ? k + 1 : end;
	}

	// `$[...]` legacy arithmetic expansion. Sibling of expandHeredocArith
	// for the bracketed form. Caller has verified body[i+1]=='['.
	void Expander::expandHeredocArithBracket(const std::string& body, std::size_t& i,
	                                         std::string& out)
	{
		const std::size_t end = body.size();
		const std::size_t k = scanBalancedBrackets(body, i + 2);
		const std::string inner = body.substr(i + 2, k - (i + 2));
		const long long v = evalArith(inner);
		out += std::to_string(v);
		i = (k < end) ? k + 1 : end;
	}

	// `${...}` parameter expansion in a heredoc body.
	void Expander::expandHeredocParamBraces(const std::string& body, std::size_t& i,
	                                        std::string& out)
	{
		const std::size_t end = body.size();
		const std::size_t k = scanBalancedBraces(body, i + 2);
		const std::string inner = body.substr(i + 2, k - (i + 2));
		out += expandParam(inner, true);
		i = (k < end) ? k + 1 : k;
	}

	// `$NAME`, `$1`, `$@`, etc. — bare-name parameter reference.
	void Expander::expandHeredocSimpleParam(const std::string& body, std::size_t& i,
	                                        std::string& out)
	{
		const std::size_t end = body.size();
		const char n1 = body[i + 1];
		std::size_t k = i + 1;
		if (isNameStart(n1)) {
			++k;
			while (k < end && isNameCont(body[k])) ++k;
		} else {
			++k;
		}
		const std::string name = body.substr(i + 1, k - (i + 1));
		out += lookupParam(name);
		i = k;
	}

	// Backquote command substitution. `i` points to the opening `` ` ``.
	void Expander::expandHeredocBackquote(const std::string& body, std::size_t& i,
	                                      std::string& out)
	{
		const std::size_t end = body.size();
		std::size_t k = i + 1;
		std::string inner;
		while (k < end && body[k] != '`') {
			if (body[k] == '\\' && k + 1 < end) {
				const char nx = body[k + 1];
				if (nx == '$' || nx == '`' || nx == '\\') {
					inner.push_back(nx);
					k += 2;
					continue;
				}
			}
			inner.push_back(body[k]);
			++k;
		}
		std::string r = runCmdSubst(inner);
		while (!r.empty() && r.back() == '\n') r.pop_back();
		out += r;
		i = (k < end) ? k + 1 : k;
	}

	std::string Expander::expandHeredoc(const std::string& body, bool quoted) {
		if (quoted) return body;

		std::string out;
		const std::size_t end = body.size();
		std::size_t i = 0;
		while (i < end && !aborting()) {
			const char c = body[i];

			if (c == '\\' && i + 1 < end) {
				consumeHeredocBackslash(body, i, out);
				continue;
			}

			if (c == '$' && i + 1 < end) {
				const char n1 = body[i + 1];
				if (n1 == '{') { expandHeredocParamBraces(body, i, out); continue; }
				if (n1 == '(') {
					if (i + 2 < end && body[i + 2] == '(')
						expandHeredocArith(body, i, out);
					else
						expandHeredocCmdSubst(body, i, out);
					continue;
				}
				if (n1 == '[') { expandHeredocArithBracket(body, i, out); continue; }
				if (isNameStart(n1) || std::isdigit(static_cast<unsigned char>(n1))
				    || isSpecialParam1(n1)) {
					expandHeredocSimpleParam(body, i, out);
					continue;
				}
				out.push_back('$');
				++i;
				continue;
			}

			if (c == '`') {
				expandHeredocBackquote(body, i, out);
				continue;
			}

			out.push_back(c);
			++i;
		}
		return out;
	}

	std::string Expander::runCmdSubst(const std::string& body) {
		if (sub_) return sub_->run(body);
		return {};
	}

	// ---------------------------------------------------------------------------
	// ANSI-C quoting interpretation ($'...')
	// ---------------------------------------------------------------------------

	// `\xHH` — read up to two hex digits at `body[*i]` and emit the byte.
	// On entry, `*i` points just past the `\x`.
	static void ansicHexEscape(const std::string& body, std::size_t* i,
	                           std::string& out) {
		const std::size_t end = body.size();
		int val = 0;
		int cnt = 0;
		while (cnt < 2 && *i < end
		       && std::isxdigit(static_cast<unsigned char>(body[*i])))
		{
			const int d = std::isdigit(static_cast<unsigned char>(body[*i]))
				? body[*i] - '0'
				: std::tolower(static_cast<unsigned char>(body[*i])) - 'a' + 10;
			val = val * 16 + d;
			++(*i);
			++cnt;
		}
		out.push_back(static_cast<char>(val));
	}

	// `\NNN` — read up to three octal digits starting at `body[*i]`.
	// On entry, `*i` already points at the first octal digit.
	static void ansicOctalEscape(const std::string& body, std::size_t* i,
	                             std::string& out) {
		const std::size_t end = body.size();
		int val = 0;
		int cnt = 0;
		while (cnt < 3 && *i < end && body[*i] >= '0' && body[*i] <= '7') {
			val = val * 8 + (body[*i] - '0');
			++(*i);
			++cnt;
		}
		out.push_back(static_cast<char>(val));
	}

	// Returns the literal mapping for simple `\X` escapes (e.g. `\n` -> `\n`),
	// or 0 if `nx` is not a one-character escape. Caller advances by 2 on a
	// non-zero return.
	static char simpleAnsicEscape(char nx) {
		switch (nx) {
		case 'a':  return '\a';
		case 'b':  return '\b';
		case 'e':  case 'E': return '\x1b';
		case 'f':  return '\f';
		case 'n':  return '\n';
		case 'r':  return '\r';
		case 't':  return '\t';
		case 'v':  return '\v';
		case '\\': return '\\';
		case '\'': return '\'';
		case '"':  return '"';
		case '?':  return '?';
		default:   return 0;
		}
	}

	std::string Expander::interpretAnsiC(const std::string& body) {
		std::string out;
		const std::size_t end = body.size();
		std::size_t i = 0;
		while (i < end) {
			const char c = body[i];
			// Bare char (not a backslash, or backslash with nothing after).
			if (c != '\\' || i + 1 >= end) {
				out.push_back(c);
				++i;
				continue;
			}
			const char nx = body[i + 1];

			// Simple one-character escapes (\n, \t, ...).
			if (const char m = simpleAnsicEscape(nx); m != 0) {
				out.push_back(m);
				i += 2;
				continue;
			}
			// `\xHH` hex byte.
			if (nx == 'x') {
				i += 2;
				ansicHexEscape(body, &i, out);
				continue;
			}
			// `\NNN` octal byte.
			if (nx >= '0' && nx <= '7') {
				++i;
				ansicOctalEscape(body, &i, out);
				continue;
			}
			// `\cX` control byte.
			if (nx == 'c') {
				if (i + 2 < end) {
					out.push_back(static_cast<char>(body[i + 2] & 0x1f));
					i += 3;
				} else {
					out.push_back('\\');
					++i;
				}
				continue;
			}
			// Unknown — keep both the backslash and the next char.
			out.push_back('\\');
			out.push_back(nx);
			i += 2;
		}
		return out;
	}

	// ---------------------------------------------------------------------------
	// Tilde expansion
	// ---------------------------------------------------------------------------

	Word Expander::applyTildeExpansion(const Word& w) {
		Word out = w;
		if (out.segments.empty()) return out;
		auto& first = out.segments[0];
		if (first.kind != WordSegment::Kind::Literal) return out;
		if (first.text.empty() || first.text[0] != '~') return out;
		std::size_t end = 1;
		while (end < first.text.size() && first.text[end] != '/' && first.text[end] != ':')
			++end;
		std::string name = first.text.substr(1, end - 1);
		std::string repl;
		if (name.empty())       repl = env_.get("HOME");
		else if (name == "+")   repl = env_.get("PWD");
		else if (name == "-")   repl = env_.get("OLDPWD");
		else                    return out;          // ~user — not implemented
		if (repl.empty()) return out;
		first.text = repl + first.text.substr(end);
		return out;
	}

	// ---------------------------------------------------------------------------
	// Parameter lookup
	// ---------------------------------------------------------------------------

	bool Expander::isSpecialParam1(char c) const {
		return c == '?' || c == '$' || c == '!' || c == '#'
		    || c == '@' || c == '*' || c == '-' || c == '_';
	}

	// Join positional parameters into one string. `*` form uses the first
	// IFS char as separator (per POSIX), `@` form uses a single space.
	std::string Expander::joinPositionals(char form) const {
		const auto& p = env_.positional();
		std::string sep = " ";
		if (form == '*') {
			const std::string ifs = env_.get("IFS");
			sep = ifs.empty() ? std::string() : std::string(1, ifs[0]);
		}
		std::string out;
		for (std::size_t i = 0; i < p.size(); ++i) {
			if (i) out += sep;
			out += p[i];
		}
		return out;
	}

	// Single-character special parameters: `$?`, `$$`, `$@`, `$1`, etc.
	// On miss, returns false and leaves `out` untouched.
	bool Expander::lookupSpecialChar(char c, std::string& out) {
		switch (c) {
		case '?': out = std::to_string(env_.lastStatus());            return true;
		case '$': out = std::to_string(env_.shellPid());              return true;
		case '!': out = std::to_string(env_.lastBgPid());             return true;
		case '#': out = std::to_string(env_.positional().size());     return true;
		case '@':
		case '*': out = joinPositionals(c);                            return true;
		case '0': out = env_.shellName();                             return true;
		case '-': out = env_.shellOptions();                          return true;
		case '_': out = env_.get("_");                                return true;
		default:
			if (c >= '1' && c <= '9') {
				const std::size_t idx = static_cast<std::size_t>(c - '1');
				out = (idx < env_.positional().size())
					? env_.positional()[idx]
					: std::string();
				return true;
			}
			return false;
		}
	}

	// Dynamic vars whose value is computed each read (RANDOM, SECONDS, ...).
	// Returns false if `name` isn't a dynamic special.
	bool Expander::lookupDynamicSpecial(const std::string& name, std::string& out) const {
		if (name == "RANDOM")  { out = std::to_string(env_.randomNext());        return true; }
		if (name == "SECONDS") { out = std::to_string(env_.secondsSinceStart()); return true; }
		if (name == "LINENO")  { out = std::to_string(env_.currentLineno());     return true; }
		if (name == "BASHPID") {
#ifdef _WIN32
			out = std::to_string(static_cast<long long>(::GetCurrentProcessId()));
#else
			out = std::to_string(static_cast<long long>(::getpid()));
#endif
			return true;
		}
		return false;
	}

	std::string Expander::lookupParam(const std::string& name) {
		if (name.empty()) return {};

		if (name.size() == 1) {
			std::string out;
			if (lookupSpecialChar(name[0], out)) return out;
		}

		// All-digit name: positional arg lookup (`$10`, `$11`, ...).
		bool all_digits = true;
		for (char c : name) {
			if (!std::isdigit(static_cast<unsigned char>(c))) {
				all_digits = false;
				break;
			}
		}
		if (all_digits) {
			unsigned long parsed = 0;
			if (!parseUL(name, parsed)) parsed = 0;
			const std::size_t idx = static_cast<std::size_t>(parsed);
			if (idx == 0) return env_.shellName();
			if (idx <= env_.positional().size()) return env_.positional()[idx - 1];
			return {};
		}

		std::string dyn;
		if (lookupDynamicSpecial(name, dyn)) return dyn;

		if (!env_.has(name)) {
			if (env_.nounset()) {
				fail(name + ": unbound variable");
			}
			return {};
		}
		return env_.get(name);
	}

	// Pick the separator for `${arr[*]}` — first IFS char when `star_join_ifs`,
	// otherwise a single space (matching bash's @ behaviour).
	std::string Expander::starSeparator(bool star_join_ifs) const {
		if (!star_join_ifs) return " ";
		const std::string ifs = env_.get("IFS");
		return ifs.empty() ? std::string() : std::string(1, ifs[0]);
	}

	// Concatenate every value in an indexed array, separated by `sep`. The
	// map is sorted by index, so the output is in subscript order.
	static std::string joinAllArrayValues(const Environment::IndexedArray& items,
	                                      const std::string& sep) {
		std::string out;
		bool first = true;
		for (const auto& kv : items) {
			if (!first) out += sep;
			out += kv.second;
			first = false;
		}
		return out;
	}

	// Same as above, for an associative array. Order matches the underlying
	// std::map iteration (lexicographic by key).
	static std::string joinAllArrayValues(const Environment::AssocArray& items,
	                                      const std::string& sep) {
		std::string out;
		bool first = true;
		for (const auto& kv : items) {
			if (!first) out += sep;
			out += kv.second;
			first = false;
		}
		return out;
	}

	std::string Expander::lookupSubscripted(const std::string& name,
	                                        const std::string& subscript_in,
	                                        bool star_join_ifs)
	{
		// Subscripts permit $-substitutions: `${arr[$i]}`, `${m[$key]}`.
		// `@` and `*` are literal markers and never expanded.
		std::string subscript = subscript_in;
		if (subscript != "@" && subscript != "*"
		    && subscript.find('$') != std::string::npos) {
			subscript = expandHeredoc(subscript, false);
		}
		const bool whole_array = (subscript == "@" || subscript == "*");

		if (auto* ia = env_.getIndexedArray(name)) {
			if (whole_array) {
				const std::string sep = (subscript == "*")
					? starSeparator(star_join_ifs)
					: std::string(" ");
				return joinAllArrayValues(*ia, sep);
			}
			long long idx = 0;
			if (!tryEvalArith(subscript, idx)) return {};
			const auto it = ia->find(idx);
			return it == ia->end() ? std::string() : it->second;
		}
		if (auto* aa = env_.getAssocArray(name)) {
			if (whole_array) {
				const std::string sep = (subscript == "*")
					? starSeparator(star_join_ifs)
					: std::string(" ");
				return joinAllArrayValues(*aa, sep);
			}
			const auto it = aa->find(subscript);
			return it == aa->end() ? std::string() : it->second;
		}
		// Scalar treated as a 1-element indexed array at index 0.
		if (whole_array) return lookupParam(name);
		long long idx = 0;
		if (!tryEvalArith(subscript, idx)) return {};
		return idx == 0 ? lookupParam(name) : std::string();
	}

	std::size_t Expander::arrayLength(const std::string& name) const {
		if (auto* ia = env_.getIndexedArray(name)) return ia->size();
		if (auto* aa = env_.getAssocArray(name))   return aa->size();
		// Treat scalar as length 1 if set, 0 otherwise.
		return env_.has(name) ? 1u : 0u;
	}

	std::vector<std::string> Expander::arrayKeys(const std::string& name) const {
		std::vector<std::string> out;
		if (auto* ia = env_.getIndexedArray(name)) {
			for (const auto& kv : *ia) out.push_back(std::to_string(kv.first));
		} else if (auto* aa = env_.getAssocArray(name)) {
			for (const auto& kv : *aa) out.push_back(kv.first);
		} else if (env_.has(name)) {
			out.push_back("0");
		}
		return out;
	}

	std::vector<std::string> Expander::arrayValues(const std::string& name) const {
		std::vector<std::string> out;
		if (auto* ia = env_.getIndexedArray(name)) {
			for (const auto& kv : *ia) out.push_back(kv.second);
		} else if (auto* aa = env_.getAssocArray(name)) {
			for (const auto& kv : *aa) out.push_back(kv.second);
		} else if (env_.has(name)) {
			out.push_back(env_.get(name));
		}
		return out;
	}

	// ---------------------------------------------------------------------------
	// ${...} body expansion
	// ---------------------------------------------------------------------------

	// `${#name}` / `${#name[i]}` / `${#name[@]}`. Returns empty string only
	// if the body doesn't actually start with `#`.
	std::string Expander::expandParamLengthForm(const std::string& body) {
		const std::string rest = body.substr(1);
		if (rest == "@" || rest == "*")
			return std::to_string(env_.positional().size());
		// Subscripted: ${#arr[@]} / ${#arr[i]}.
		const std::size_t lb = rest.find('[');
		if (lb != std::string::npos && rest.back() == ']') {
			const std::string nm  = rest.substr(0, lb);
			const std::string sub = rest.substr(lb + 1, rest.size() - lb - 2);
			if (sub == "@" || sub == "*")
				return std::to_string(arrayLength(nm));
			return std::to_string(lookupSubscripted(nm, sub, false).size());
		}
		return std::to_string(lookupParam(rest).size());
	}

	// `${!name[@]}` / `${!name[*]}` — list of array indices/keys joined
	// by spaces. Returns empty string for any other body shape (caller
	// will fall through to other forms).
	std::string Expander::expandParamIndicesForm(const std::string& body) {
		if (body.size() <= 4) return {};
		const std::string rest = body.substr(1);
		const std::size_t lb = rest.find('[');
		if (lb == std::string::npos || rest.back() != ']') return {};
		const std::string nm  = rest.substr(0, lb);
		const std::string sub = rest.substr(lb + 1, rest.size() - lb - 2);
		if (sub != "@" && sub != "*") return {};

		auto keys = arrayKeys(nm);
		std::string out;
		for (std::size_t k = 0; k < keys.size(); ++k) {
			if (k) out.push_back(' ');
			out += keys[k];
		}
		return out;
	}

	// Identify the leading parameter name span in `body`. On success returns
	// the index just past the name, with `out_name` set to the name; on
	// failure returns std::string::npos.
	static std::size_t scanParamName(const std::string& body, std::string& out_name,
	                                 const Expander& self_for_special) {
		std::size_t i = 0;
		if (i < body.size() && isNameStart(body[i])) {
			++i;
			while (i < body.size() && isNameCont(body[i])) ++i;
		} else if (i < body.size()
		           && std::isdigit(static_cast<unsigned char>(body[i]))) {
			while (i < body.size()
			       && std::isdigit(static_cast<unsigned char>(body[i]))) ++i;
		} else if (i < body.size() && self_for_special.isSpecialParam1(body[i])) {
			++i;
		} else {
			return std::string::npos;
		}
		out_name = body.substr(0, i);
		return i;
	}

	// Default-form operators: ${name:-x}, ${name:+x}, ${name:=x}, ${name:?x}
	// and the colon-less variants. Caller has already classified `op` and
	// `colon` from the body, and computed `cur` (current value) via
	// namedValue().
	std::string Expander::evalParamDefaultOp(char op, bool colon,
	                                         const std::string& name,
	                                         const std::string& cur,
	                                         const std::string& arg)
	{
		const bool unset = !env_.has(name)
		    && !std::isdigit(static_cast<unsigned char>(name[0]))
		    && !isSpecialParam1(name[0]);
		const bool empty_or_unset = colon ? (unset || cur.empty()) : unset;

		switch (op) {
		case '-':
			return empty_or_unset ? expandHeredoc(arg, false) : cur;
		case '+':
			return empty_or_unset ? std::string() : expandHeredoc(arg, false);
		case '=': {
			if (!empty_or_unset) return cur;
			std::string v = expandHeredoc(arg, false);
			env_.set(name, v);
			return v;
		}
		case '?': {
			if (!empty_or_unset) return cur;
			std::string msg = arg.empty()
				? (name + ": parameter null or not set")
				: expandHeredoc(arg, false);
			fail(std::move(msg));
			return {};
		}
		default:
			return cur;
		}
	}

	// `${arr[@]:offset:length}` — slice across the array's elements, joining
	// the survivors with single spaces. Both offset and length are arithmetic
	// expressions (`-1` from end, etc.).
	std::string Expander::evalArraySlice(const std::string& name,
	                                     const std::string& args)
	{
		auto elems = arrayValues(name);
		// Split off:length on the first unparenthesised `:`.
		int depth = 0;
		std::size_t split = std::string::npos;
		for (std::size_t k = 0; k < args.size(); ++k) {
			const char c = args[k];
			if (c == '(') ++depth;
			else if (c == ')' && depth > 0) --depth;
			else if (c == ':' && depth == 0) { split = k; break; }
		}
		const std::string off_s = (split == std::string::npos) ? args : args.substr(0, split);
		const std::string len_s = (split == std::string::npos) ? std::string()
		                                                       : args.substr(split + 1);

		const long long n = static_cast<long long>(elems.size());
		long long off = 0;
		if (!tryEvalArith(off_s, off)) off = 0;
		if (off < 0) off = std::max<long long>(0, n + off);
		if (off > n) off = n;

		long long take = n - off;
		if (!len_s.empty()) {
			if (!tryEvalArith(len_s, take)) take = 0;
			if (take < 0) take = std::max<long long>(0, n + take - off);
			if (take > n - off) take = n - off;
		}

		std::string out;
		for (long long k = 0; k < take; ++k) {
			if (k) out.push_back(' ');
			out += elems[static_cast<std::size_t>(off + k)];
		}
		return out;
	}

	// `${name/pat/rep}` / `${name//pat/rep}` / `${name/#pat/rep}` /
	// `${name/%pat/rep}`. `args` is everything after `/` or `//`.
	std::string Expander::evalParamReplace(const std::string& cur,
	                                       const std::string& args, bool all)
	{
		std::string body = args;
		bool anchor_start = false;
		bool anchor_end   = false;
		if      (!body.empty() && body[0] == '#') { anchor_start = true; body.erase(0, 1); }
		else if (!body.empty() && body[0] == '%') { anchor_end   = true; body.erase(0, 1); }

		const std::size_t slash = body.find('/');
		const std::string pat = (slash == std::string::npos) ? body : body.substr(0, slash);
		const std::string rep = (slash == std::string::npos) ? std::string()
		                                                     : body.substr(slash + 1);
		return replacePattern(cur, expandHeredoc(pat, false),
		                      expandHeredoc(rep, false),
		                      all, anchor_start, anchor_end);
	}

	// Apply the recognised operator. @p op_pos points at the operator char.
	// `colon` indicates the colon variant of the default-form ops (${name:-x}).
	std::string Expander::expandParamApplyOp(char op, bool colon, std::size_t op_pos,
	                                         ParamOpCtx& ctx) {
		if (op == '-' || op == '+' || op == '=' || op == '?') {
			return evalParamDefaultOp(op, colon, ctx.name, ctx.named_value,
			                          ctx.body.substr(op_pos + 1));
		}
		if (op == ':') {
			if (ctx.has_subscript && (ctx.subscript == "@" || ctx.subscript == "*"))
				return evalArraySlice(ctx.name, ctx.body.substr(op_pos + 1));
			return substringExpand(ctx.named_value, ctx.body.substr(op_pos + 1));
		}
		if (op == '#') {
			const bool greedy = (op_pos + 1 < ctx.body.size() && ctx.body[op_pos + 1] == '#');
			const std::string pat = ctx.body.substr(greedy ? op_pos + 2 : op_pos + 1);
			return stripPrefix(ctx.named_value, expandHeredoc(pat, false), greedy);
		}
		if (op == '%') {
			const bool greedy = (op_pos + 1 < ctx.body.size() && ctx.body[op_pos + 1] == '%');
			const std::string pat = ctx.body.substr(greedy ? op_pos + 2 : op_pos + 1);
			return stripSuffix(ctx.named_value, expandHeredoc(pat, false), greedy);
		}
		if (op == '/') {
			const bool all = (op_pos + 1 < ctx.body.size() && ctx.body[op_pos + 1] == '/');
			return evalParamReplace(ctx.named_value,
			                        ctx.body.substr(all ? op_pos + 2 : op_pos + 1), all);
		}
		return ctx.named_value;
	}

	std::string Expander::expandParam(const std::string& body, bool /*quoted_ctx*/) {
		if (body.empty()) return {};

		if (body[0] == '#' && body.size() > 1) return expandParamLengthForm(body);
		if (body[0] == '!' && body.size() > 4) {
			std::string out = expandParamIndicesForm(body);
			if (!out.empty()) return out;
			// Fall through if the !-form didn't match any recognised shape.
		}

		std::string name;
		std::size_t i = scanParamName(body, name, *this);
		if (i == std::string::npos) return {};

		std::string subscript;
		bool has_subscript = false;
		if (i < body.size() && body[i] == '[') {
			const std::size_t close = body.find(']', i + 1);
			if (close != std::string::npos) {
				subscript = body.substr(i + 1, close - i - 1);
				has_subscript = true;
				i = close + 1;
			}
		}
		const std::string named = has_subscript
			? lookupSubscripted(name, subscript, true)
			: lookupParam(name);
		if (i >= body.size()) return named;

		char op = body[i];
		bool colon = false;
		if (op == ':' && i + 1 < body.size()
		    && (body[i + 1] == '-' || body[i + 1] == '+'
		        || body[i + 1] == '=' || body[i + 1] == '?'))
		{
			colon = true;
			++i;
			op = body[i];
		}
		ParamOpCtx ctx{ name, subscript, has_subscript, body, named };
		return expandParamApplyOp(op, colon, i, ctx);
	}

	std::string Expander::substringExpand(const std::string& val, const std::string& args) {
		// Find unbalanced ':' that splits offset from length.
		int depth = 0;
		std::size_t split = std::string::npos;
		for (std::size_t k = 0; k < args.size(); ++k) {
			char c = args[k];
			if (c == '(') ++depth;
			else if (c == ')' && depth > 0) --depth;
			else if (c == ':' && depth == 0) { split = k; break; }
		}
		std::string off_s = (split == std::string::npos) ? args : args.substr(0, split);
		std::string len_s = (split == std::string::npos) ? std::string() : args.substr(split + 1);
		long long off = evalArith(off_s);
		long long n = static_cast<long long>(val.size());
		if (off < 0) off = std::max<long long>(0, n + off);
		if (off > n) off = n;
		if (len_s.empty()) return val.substr(static_cast<std::size_t>(off));
		long long len = evalArith(len_s);
		if (len < 0) {
			long long endpos = n + len;
			if (endpos < off) endpos = off;
			return val.substr(static_cast<std::size_t>(off),
			                  static_cast<std::size_t>(endpos - off));
		}
		if (off + len > n) len = n - off;
		return val.substr(static_cast<std::size_t>(off),
		                  static_cast<std::size_t>(len));
	}

	std::string Expander::stripPrefix(const std::string& val, const std::string& pat, bool greedy) {
		if (pat.empty()) return val;
		if (greedy) {
			for (long long L = static_cast<long long>(val.size()); L >= 0; --L) {
				if (fnmatchFull(pat, val.substr(0, static_cast<std::size_t>(L))))
					return val.substr(static_cast<std::size_t>(L));
			}
		} else {
			for (std::size_t L = 0; L <= val.size(); ++L) {
				if (fnmatchFull(pat, val.substr(0, L)))
					return val.substr(L);
			}
		}
		return val;
	}

	std::string Expander::stripSuffix(const std::string& val, const std::string& pat, bool greedy) {
		if (pat.empty()) return val;
		std::size_t n = val.size();
		if (greedy) {
			for (long long L = static_cast<long long>(n); L >= 0; --L) {
				std::size_t off = n - static_cast<std::size_t>(L);
				if (fnmatchFull(pat, val.substr(off, static_cast<std::size_t>(L))))
					return val.substr(0, off);
			}
		} else {
			for (std::size_t L = 0; L <= n; ++L) {
				std::size_t off = n - L;
				if (fnmatchFull(pat, val.substr(off, L)))
					return val.substr(0, off);
			}
		}
		return val;
	}

	// `${val/#pat/rep}` — replace the longest prefix of `val` matching `pat`.
	static std::string replaceAnchoredPrefix(const std::string& val,
	                                         const std::string& pat,
	                                         const std::string& rep) {
		for (long long L = static_cast<long long>(val.size()); L >= 0; --L) {
			if (fnmatchFull(pat, val.substr(0, static_cast<std::size_t>(L))))
				return rep + val.substr(static_cast<std::size_t>(L));
		}
		return val;
	}

	// `${val/%pat/rep}` — replace the longest suffix of `val` matching `pat`.
	static std::string replaceAnchoredSuffix(const std::string& val,
	                                         const std::string& pat,
	                                         const std::string& rep) {
		const std::size_t n = val.size();
		for (long long L = static_cast<long long>(n); L >= 0; --L) {
			const std::size_t off = n - static_cast<std::size_t>(L);
			if (fnmatchFull(pat, val.substr(off, static_cast<std::size_t>(L))))
				return val.substr(0, off) + rep;
		}
		return val;
	}

	// At position `i`, find the length of the longest substring matching
	// `pat`. Returns -1 if nothing at this position matches.
	static long long longestMatchAt(const std::string& val, std::size_t i,
	                                const std::string& pat) {
		for (long long L = static_cast<long long>(val.size() - i); L >= 0; --L) {
			if (fnmatchFull(pat, val.substr(i, static_cast<std::size_t>(L))))
				return L;
		}
		return -1;
	}

	// `${val/pat/rep}` (single match) and `${val//pat/rep}` (all matches),
	// unanchored. Greedy at each position.
	static std::string replaceUnanchored(const std::string& val,
	                                     const std::string& pat,
	                                     const std::string& rep, bool all) {
		std::string out;
		std::size_t i = 0;
		while (i <= val.size()) {
			const long long best = longestMatchAt(val, i, pat);
			if (best < 0) {
				if (i < val.size()) out.push_back(val[i]);
				++i;
				continue;
			}

			out += rep;
			if (best == 0) {
				// Zero-width match: advance one literal char so we don't
				// loop forever on a pattern that always matches `""`.
				if (i < val.size()) out.push_back(val[i]);
				++i;
			} else {
				i += static_cast<std::size_t>(best);
			}
			if (!all) {
				out += val.substr(i);
				return out;
			}
		}
		return out;
	}

	std::string Expander::replacePattern(const std::string& val, const std::string& pat,
	                                     const std::string& rep, bool all,
	                                     bool anchor_start, bool anchor_end) {
		if (pat.empty())     return val;
		if (anchor_start)    return replaceAnchoredPrefix(val, pat, rep);
		if (anchor_end)      return replaceAnchoredSuffix(val, pat, rep);
		return replaceUnanchored(val, pat, rep, all);
	}

	// ---------------------------------------------------------------------------
	// Word rendering
	// ---------------------------------------------------------------------------

	Expander::Tagged Expander::renderWord(const Word& w) {
		Tagged t;
		t.reserve(w.raw.size());
		for (const auto& s : w.segments) {
			if (s.kind == WordSegment::Kind::SingleQuoted ||
			    s.kind == WordSegment::Kind::DoubleQuoted ||
			    s.kind == WordSegment::Kind::Escaped ||
			    s.kind == WordSegment::Kind::DollarSingle) {
				t.had_quote = true;
			}
		}
		for (const auto& s : w.segments) {
			// Stop rendering once an expansion error or a control-flow
			// signal from a command substitution is pending — the old
			// exceptions aborted mid-word the same way.
			if (aborting()) break;
			renderSegment(s, t, /*inside_dq=*/false);
		}
		return t;
	}

	// `${name[@]}` / `${name[*]}` / `${!name[@]}`: if `body` is one of these
	// shapes, return the bare `name`; otherwise return empty string. The
	// length form `${#name[@]}` and any body with trailing operators are
	// rejected.
	static std::string fieldAwareArrayName(const std::string& body) {
		if (body.size() < 4) return {};
		if (body[0] == '#') return {};   // length form is not field-aware
		const bool indices = body[0] == '!';
		const std::size_t start = indices ? 1 : 0;
		const std::size_t lb = body.find('[', start);
		if (lb == std::string::npos) return {};
		if (lb + 2 >= body.size()) return {};
		if (body[lb + 1] != '@' && body[lb + 1] != '*') return {};
		if (body[lb + 2] != ']') return {};
		if (lb + 3 != body.size()) return {};   // no trailing ops
		return body.substr(start, lb - start);
	}

	// Render a `${name[@]}` / `${name[*]}` / `${!name[@]}` segment whose
	// element list is `elems`. Inside double quotes with `*`, elements are
	// joined with the first IFS char; otherwise each element becomes its
	// own field separated by an unquoted space (so word splitting can
	// break them apart later while preserving internal spaces).
	void Expander::renderArrayFields(const std::vector<std::string>& elems,
	                                 bool star, bool inside_dq, Tagged& out)
	{
		const std::uint8_t mark = inside_dq ? F_QUOTED : 0;
		if (star && inside_dq) {
			const std::string ifs = env_.get("IFS");
			const std::string sep = ifs.empty() ? std::string() : std::string(1, ifs[0]);
			for (std::size_t k = 0; k < elems.size(); ++k) {
				if (k) for (char c : sep) out.push(c, mark);
				for (char c : elems[k]) out.push(c, mark);
			}
		} else {
			for (std::size_t k = 0; k < elems.size(); ++k) {
				if (k) out.push(' ', 0);
				for (char c : elems[k]) out.push(c, mark);
			}
		}
	}

	void Expander::renderParamExpSegment(const WordSegment& s, Tagged& out, bool inside_dq) {
		const std::uint8_t mark = inside_dq ? F_QUOTED : 0;
		// Field-aware fast path for `${name[@]}` / `${name[*]}` / `${!name[@]}`.
		const std::string fname = fieldAwareArrayName(s.text);
		if (!fname.empty()) {
			const bool indices = s.text[0] == '!';
			const bool star    = s.text.find('*') != std::string::npos;
			const std::vector<std::string> elems =
				indices ? arrayKeys(fname) : arrayValues(fname);
			renderArrayFields(elems, star, inside_dq, out);
			if (!elems.empty() || inside_dq) out.had_quote |= inside_dq;
			return;
		}
		// Generic ${...} expansion — already returns a single string.
		const std::string v = expandParam(s.text, inside_dq);
		for (char c : v) out.push(c, mark);
	}

	void Expander::renderProcSubstSegment(const WordSegment& s, Tagged& out) {
		// `<(cmd)` runs the inner command, captures stdout to a temp
		// file, and substitutes that file's path. `>(cmd)` would need
		// reverse-direction streaming (named pipe); not supported yet.
		if (s.proc_dir != '<') {
			std::fprintf(stderr,
				"wbsh: process substitution >(...) not supported\n");
			return;
		}
#ifdef _WIN32
		char tdir[MAX_PATH];
		const DWORD nd = GetTempPathA(MAX_PATH, tdir);
		char tpath[MAX_PATH];
		if (nd == 0 || nd > MAX_PATH
		    || GetTempFileNameA(tdir, "wbsh", 0, tpath) == 0) {
			return;
		}
		// Process substitution preserves the producer's stdout exactly,
		// unlike $(...) which strips trailing newlines.
		const std::string body = sub_ ? sub_->runRaw(s.text) : std::string();
		{
			std::ofstream f(tpath, std::ios::binary | std::ios::trunc);
			f.write(body.data(), body.size());
		}
		pending_temp_files_.emplace_back(tpath);
		const std::string posix = path_conv_.toPosix(tpath);
		for (char c : posix) out.push(c, F_QUOTED);
		out.had_quote = true;
#endif /* _WIN32 */
	}

	// `$@` (and only `$@`): each positional becomes its own field. Emit them
	// joined by an UNQUOTED IFS-space so word splitting will break them
	// apart later while preserving any spaces inside individual positionals.
	void Expander::renderDollarAt(Tagged& out, bool inside_dq) {
		const std::uint8_t mark = inside_dq ? F_QUOTED : 0;
		const auto& p = env_.positional();
		for (std::size_t i = 0; i < p.size(); ++i) {
			if (i) out.push(' ', 0);
			for (char c : p[i]) out.push(c, mark);
		}
	}

	void Expander::renderSegment(const WordSegment& s, Tagged& out, bool inside_dq) {
		using K = WordSegment::Kind;
		const std::uint8_t mark = inside_dq ? F_QUOTED : 0;
		switch (s.kind) {
		case K::Literal:
			for (char c : s.text) out.push(c, mark);
			break;
		case K::Escaped:
		case K::SingleQuoted:
			for (char c : s.text) out.push(c, F_QUOTED);
			break;
		case K::DoubleQuoted:
			for (const auto& n : s.nested) {
				if (aborting()) break;
				renderSegment(n, out, /*inside_dq=*/true);
			}
			break;
		case K::DollarSingle: {
			const std::string ic = interpretAnsiC(s.text);
			for (char c : ic) out.push(c, F_QUOTED);
			break;
		}
		case K::SimpleVar: {
			if (s.text == "@") {
				renderDollarAt(out, inside_dq);
				break;
			}
			const std::string v = lookupParam(s.text);
			for (char c : v) out.push(c, mark);
			break;
		}
		case K::ParamExp:
			renderParamExpSegment(s, out, inside_dq);
			break;
		case K::CmdSubst: {
			std::string v = runCmdSubst(s.text);
			while (!v.empty() && v.back() == '\n') v.pop_back();
			for (char c : v) out.push(c, mark);
			break;
		}
		case K::ArithExp: {
			const long long r = evalArith(s.text);
			const std::string v = std::to_string(r);
			for (char c : v) out.push(c, mark);
			break;
		}
		case K::ProcSubst:
			renderProcSubstSegment(s, out);
			break;
		}
	}

	// ---------------------------------------------------------------------------
	// Word splitting
	// ---------------------------------------------------------------------------

	std::vector<Expander::Tagged> Expander::splitWords(const Tagged& t) {
		std::string ifs = env_.get("IFS");
		std::vector<Tagged> out;
		if (ifs.empty()) {
			if (t.size() > 0 || t.had_quote) out.push_back(t);
			return out;
		}
		auto isIfsWS = [&](char c) {
			if (c != ' ' && c != '\t' && c != '\n') return false;
			return ifs.find(c) != std::string::npos;
		};
		auto isIfs = [&](char c) {
			return ifs.find(c) != std::string::npos;
		};
		std::size_t n = t.size();
		std::size_t i = 0;
		// Trim leading IFS-whitespace.
		while (i < n && (t.flags[i] & F_QUOTED) == 0 && isIfsWS(t.text[i])) ++i;
		while (i < n) {
			Tagged cur;
			while (i < n) {
				char c = t.text[i];
				bool q = (t.flags[i] & F_QUOTED) != 0;
				if (!q && isIfs(c)) break;
				cur.push(c, t.flags[i]);
				cur.had_quote = cur.had_quote || q;
				++i;
			}
			out.push_back(std::move(cur));
			// Consume separator run; allow at most one non-WS-IFS in the run.
			bool saw_nonws = false;
			while (i < n) {
				char c = t.text[i];
				bool q = (t.flags[i] & F_QUOTED) != 0;
				if (q || !isIfs(c)) break;
				if (!isIfsWS(c)) {
					if (saw_nonws) break;
					saw_nonws = true;
				}
				++i;
			}
		}
		return out;
	}

	// ---------------------------------------------------------------------------
	// Pathname expansion
	// ---------------------------------------------------------------------------

	namespace glob_internal {
		// One path component during glob expansion, with per-char quotedness
		// preserved so we can tell `\*` (literal asterisk) from `*` (meta).
		struct GlobComp {
			std::string text;
			std::vector<std::uint8_t> quoted;

			bool hasMeta() const {
				for (std::size_t i = 0; i < text.size(); ++i) {
					if (!quoted[i]) {
						const char c = text[i];
						if (c == '*' || c == '?' || c == '[') return true;
					}
				}
				return false;
			}
			// Render the component as an fnmatch pattern: literal chars in
			// the source escape the matching glob metas in the output.
			std::string asPattern() const {
				std::string p;
				for (std::size_t i = 0; i < text.size(); ++i) {
					const char c = text[i];
					if (quoted[i]
					    && (c == '*' || c == '?' || c == '[' || c == '\\')) {
						p.push_back('\\');
					}
					p.push_back(c);
				}
				return p;
			}
			std::string asLiteral() const { return text; }
		};

		// Join one path component onto a directory result. Handles "." and
		// "/" specially so we don't get "./foo" or "//foo".
		static std::string joinDir(const std::string& dir, const std::string& name) {
			if (dir == "." || dir.empty()) return name;
			if (dir == "/") return "/" + name;
			if (!dir.empty() && dir.back() == '/') return dir + name;
			return dir + "/" + name;
		}
	}  // namespace glob_internal

	// Quick-reject scan: does the tagged text contain any unquoted glob
	// meta-character? Used to short-circuit globExpand for the common case
	// of a fully-quoted or meta-free word.
	static bool taggedHasUnquotedGlobMeta(const Expander::Tagged& t) {
		for (std::size_t i = 0; i < t.size(); ++i) {
			if ((t.flags[i] & Expander::F_QUOTED) == 0) {
				const char c = t.text[i];
				if (c == '*' || c == '?' || c == '[') return true;
			}
		}
		return false;
	}

	// Slice the tagged text into glob components (separated by unquoted `/`).
	// Sets `*absolute` to true if the path begins with an unquoted `/`.
	static std::vector<glob_internal::GlobComp>
	splitTaggedIntoGlobComps(const Expander::Tagged& t, bool* absolute) {
		std::vector<glob_internal::GlobComp> comps;
		glob_internal::GlobComp cur;
		bool first = true;
		*absolute = false;
		for (std::size_t i = 0; i < t.size(); ++i) {
			const char c = t.text[i];
			const bool q = (t.flags[i] & Expander::F_QUOTED) != 0;
			if (!q && c == '/') {
				if (first) *absolute = true;
				if (!cur.text.empty()) {
					comps.push_back(std::move(cur));
					cur = {};
				}
				first = false;
				continue;
			}
			cur.text.push_back(c);
			cur.quoted.push_back(q ? 1 : 0);
			first = false;
		}
		if (!cur.text.empty()) comps.push_back(std::move(cur));
		return comps;
	}

	// `**` step: expand each accumulated dir into itself plus every reachable
	// descendant. If this is the trailing component we list everything;
	// otherwise we keep only sub-directories so the next component continues
	// to descend.
	static std::vector<std::string>
	expandGlobstarStep(const std::vector<std::string>& current, bool last,
	                   const PathConv& pc) {
		namespace fs = std::filesystem;
		std::set<std::string> acc;
		for (auto& d : current) {
			acc.insert(d);
			const std::string posix_dir = d.empty() ? "." : d;
			const fs::path list_dir = utf8ToPath(pc.toWin32(posix_dir));
			std::error_code ec;
			fs::recursive_directory_iterator rit(list_dir,
				fs::directory_options::skip_permission_denied, ec);
			if (ec) continue;

			for (auto cur = rit; cur != fs::recursive_directory_iterator();
			     cur.increment(ec))
			{
				if (ec) break;
				const std::string name = pathToUtf8(cur->path().filename());
				// Skip dotfiles AND don't descend into them — bash behaviour.
				if (!name.empty() && name[0] == '.') {
					cur.disable_recursion_pending();
					continue;
				}
				std::error_code dec;
				std::string rel = pathToUtf8(fs::relative(cur->path(), list_dir, dec));
				std::replace(rel.begin(), rel.end(), '\\', '/');
				if (last || cur->is_directory(dec)) {
					acc.insert(glob_internal::joinDir(d, rel));
				}
			}
		}
		std::vector<std::string> out(acc.begin(), acc.end());
		std::sort(out.begin(), out.end());
		return out;
	}

	// Single-component step: descend each accumulated dir by one level,
	// either matching `comp` as a glob pattern or appending it literally.
	static std::vector<std::string>
	expandGlobOneStep(const std::vector<std::string>& current,
	                  const glob_internal::GlobComp& comp, const PathConv& pc,
	                  bool dotglob, bool nocaseglob) {
		namespace fs = std::filesystem;
		std::vector<std::string> next;

		auto match_one = [&](const std::string& pat, const std::string& name) {
			if (!nocaseglob) return fnmatchFull(pat, name);
			std::string lp = pat;
			std::string ln = name;
			for (auto& c : lp) c = static_cast<char>(std::tolower(
				static_cast<unsigned char>(c)));
			for (auto& c : ln) c = static_cast<char>(std::tolower(
				static_cast<unsigned char>(c)));
			return fnmatchFull(lp, ln);
		};

		if (!comp.hasMeta()) {
			for (auto& d : current) next.push_back(glob_internal::joinDir(d, comp.asLiteral()));
			return next;
		}

		const std::string pat = comp.asPattern();
		const bool pat_starts_dot = !pat.empty() && pat[0] == '.';
		for (auto& d : current) {
			const std::string posix_dir = d.empty() ? "." : d;
			fs::path list_dir = utf8ToPath(pc.toWin32(posix_dir));
			std::error_code ec;
			fs::directory_iterator it(list_dir, ec);
			if (ec) continue;
			std::vector<std::string> matches;
			for (auto& entry : it) {
				const std::string name = pathToUtf8(entry.path().filename());
				if (name.empty()) continue;
				// Hide dotfiles unless the pattern itself starts with `.`
				// or `dotglob` is on.
				if (name[0] == '.' && !pat_starts_dot && !dotglob) continue;
				if (match_one(pat, name)) matches.push_back(name);
			}
			std::sort(matches.begin(), matches.end());
			for (auto& m : matches) next.push_back(glob_internal::joinDir(d, m));
		}
		return next;
	}

	std::vector<std::string> Expander::globExpand(const Tagged& t) {
		if (env_.noglob()) return { quoteRemove(t) };
		if (!taggedHasUnquotedGlobMeta(t)) return { quoteRemove(t) };

		bool absolute = false;
		std::vector<glob_internal::GlobComp> comps = splitTaggedIntoGlobComps(t, &absolute);
		if (comps.empty()) return { quoteRemove(t) };

		std::vector<std::string> current;
		current.push_back(absolute ? std::string("/") : std::string("."));

		const bool dotglob    = env_.dotglob();
		const bool nocaseglob = env_.nocaseglob();
		const bool globstar   = env_.globstar();

		for (std::size_t ci = 0; ci < comps.size(); ++ci) {
			const auto& comp = comps[ci];
			if (globstar && comp.text == "**") {
				current = expandGlobstarStep(current, /*last=*/ci + 1 == comps.size(),
				                              path_conv_);
			} else {
				current = expandGlobOneStep(current, comp, path_conv_,
				                            dotglob, nocaseglob);
			}
		}

		if (current.empty()) {
			// `nullglob`: drop unmatched patterns entirely. Default bash
			// behaviour (and ours when off) is to keep the literal pattern.
			if (env_.nullglob()) return {};
			return { quoteRemove(t) };
		}
		return current;
	}

	std::string Expander::quoteRemove(const Tagged& t) {
		return t.text;
	}

	// ---------------------------------------------------------------------------
	// Brace expansion
	// ---------------------------------------------------------------------------

	static bool isInteger(const std::string& v) {
			if (v.empty()) return false;
			std::size_t i = (v[0] == '-' || v[0] == '+') ? 1 : 0;
		if (i == v.size()) return false;
		for (; i < v.size(); ++i)
			if (!std::isdigit(static_cast<unsigned char>(v[i]))) return false;
		return true;
	}

	// Returns the alternatives generated by the leftmost UNQUOTED brace
	// Skip past a single-quoted or double-quoted run in @p s starting at @p i
	// (which must index the opening quote). Returns the position one past the
	// closing quote (or end of string if unterminated). Used by braceExpandOnce
	// to stay quote-aware while scanning for brace delimiters.
	static std::size_t braceSkipQuotedRun(const std::string& s, std::size_t i) {
		char quote = s[i];
		++i;
		while (i < s.size() && s[i] != quote) {
			if (quote == '"' && s[i] == '\\' && i + 1 < s.size()) i += 2;
			else ++i;
		}
		return (i < s.size()) ? i + 1 : i;
	}

	// Locate the leftmost top-level `{` in @p s. Returns npos when none.
	static std::size_t braceFindOpen(const std::string& s) {
		std::size_t i = 0;
		while (i < s.size()) {
			char c = s[i];
			if (c == '\\' && i + 1 < s.size()) { i += 2; continue; }
			if (c == '\'' || c == '"') { i = braceSkipQuotedRun(s, i); continue; }
			if (c == '{') return i;
			++i;
		}
		return std::string::npos;
	}

	// Walk forward from @p start (which is one past the opening `{`) until the
	// matching `}` is found at the same nesting level. Populates @p commas
	// with the positions of all top-level `,` separators inside the braces.
	// Returns the index of the matching `}`, or npos when unbalanced.
	static std::size_t braceFindClose(const std::string& s, std::size_t start,
	                                  std::vector<std::size_t>& commas) {
		int depth = 1;
		std::size_t j = start;
		while (j < s.size() && depth > 0) {
			char c = s[j];
			if (c == '\\' && j + 1 < s.size()) { j += 2; continue; }
			if (c == '\'' || c == '"') { j = braceSkipQuotedRun(s, j); continue; }
			if (c == '{') { ++depth; ++j; continue; }
			if (c == '}') { --depth; if (depth == 0) return j; ++j; continue; }
			if (c == ',' && depth == 1) commas.push_back(j);
			++j;
		}
		return std::string::npos;
	}

	// Parse a `{X..Y[..Z]}` sequence body. Returns the empty vector for
	// non-sequence (or malformed) input, mirroring bash's pass-through behavior.
	static std::vector<std::string> braceParseSequence(const std::string& body) {
		auto dd = body.find("..");
		if (dd == std::string::npos) return {};
		std::string from = body.substr(0, dd);
		std::string rest = body.substr(dd + 2);
		auto dd2 = rest.find("..");
		std::string to    = (dd2 == std::string::npos) ? rest : rest.substr(0, dd2);
		std::string stepS = (dd2 == std::string::npos) ? std::string() : rest.substr(dd2 + 2);

		std::vector<std::string> alts;
		if (isInteger(from) && isInteger(to)) {
			long long a = 0;
			long long b = 0;
			long long step = 1;
			if (!parseLL(from, a) || !parseLL(to, b)) return alts;
			if (!stepS.empty() && !parseLL(stepS, step)) return alts;
			if (step == 0) step = 1;
			if ((a < b && step < 0) || (a > b && step > 0)) step = -step;
			int width = 0;
			auto needsPad = [](const std::string& v) {
				if (v.size() < 2) return false;
				std::size_t k = (v[0] == '-' || v[0] == '+') ? 1 : 0;
				return k < v.size() && v[k] == '0';
			};
			if (needsPad(from) || needsPad(to)) {
				width = static_cast<int>((std::max)(from.size(), to.size()));
			}
			for (long long v = a; (step > 0) ? (v <= b) : (v >= b); v += step) {
				if (width > 0) {
					char buf[32];
					std::snprintf(buf, sizeof(buf), "%0*lld", width, v);
					alts.push_back(buf);
				} else {
					alts.push_back(std::to_string(v));
				}
			}
			return alts;
		}
		if (from.size() == 1 && to.size() == 1
		    && std::isalpha(static_cast<unsigned char>(from[0]))
		    && std::isalpha(static_cast<unsigned char>(to[0])))
		{
			int a = static_cast<unsigned char>(from[0]);
			int b = static_cast<unsigned char>(to[0]);
			int step = 1;
			if (!stepS.empty() && !parseInt(stepS, step)) return alts;
			if (step == 0) step = 1;
			if ((a < b && step < 0) || (a > b && step > 0)) step = -step;
			for (int v = a; (step > 0) ? (v <= b) : (v >= b); v += step) {
				alts.emplace_back(1, static_cast<char>(v));
			}
		}
		return alts;
	}

	// pattern in `s`. Empty result means "no expandable brace found here";
	// caller treats `s` as a single unchanged result.
	static std::vector<std::string> braceExpandOnce(const std::string& s) {
		const std::size_t open = braceFindOpen(s);
		if (open == std::string::npos) return {};
		std::vector<std::size_t> commas;
		const std::size_t close = braceFindClose(s, open + 1, commas);
		if (close == std::string::npos) return {};

		const std::string body   = s.substr(open + 1, close - open - 1);
		const std::string prefix = s.substr(0, open);
		const std::string suffix = s.substr(close + 1);

		std::vector<std::string> alts;
		if (!commas.empty()) {
			std::size_t start = 0;
			for (auto p : commas) {
				const std::size_t off = p - (open + 1);
				alts.push_back(body.substr(start, off - start));
				start = off + 1;
			}
			alts.push_back(body.substr(start));
		} else {
			alts = braceParseSequence(body);
			if (alts.empty()) return {};
		}

		std::vector<std::string> out;
		out.reserve(alts.size());
		for (const auto& alt : alts) {
			out.push_back(prefix + alt + suffix);
		}
		return out;
	}

	static std::vector<std::string> braceExpandAll(const std::string& s) {
		auto first = braceExpandOnce(s);
		if (first.empty()) return { s };
		std::vector<std::string> out;
		for (const auto& alt : first) {
			auto sub = braceExpandAll(alt);
			for (auto& r : sub) out.push_back(std::move(r));
		}
		return out;
	}

	// ---------------------------------------------------------------------------
	// Public expansion entry points
	// ---------------------------------------------------------------------------

	std::vector<std::string> Expander::expandWord(const Word& w) {
		// Brace expansion is the first stage; each result is re-lexed and
		// fed through the rest of the pipeline as if it had been typed
		// directly. Quoted regions are preserved.
		auto braced = braceExpandAll(w.raw);
		if (braced.size() <= 1) {
			// Common case: no braces. Skip the re-lex round trip.
			return expandWordPostBrace(w);
		}
		std::vector<std::string> out;
		for (const auto& alt : braced) {
			if (aborting()) break;
			Lexer lex(alt);
			auto tokens = lex.tokenize();
			for (auto& t : tokens) {
				if (t.kind != TokKind::Word) continue;
				Word w2;
				// The token vector is local and visited once — steal the
				// segment list instead of deep-copying every segment.
				w2.segments = std::move(t.segments);
				w2.raw = std::move(t.text);
				w2.loc = w.loc;
				auto fields = expandWordPostBrace(w2);
				for (auto& f : fields) out.push_back(std::move(f));
			}
		}
		return out;
	}

	std::vector<std::string> Expander::expandWordPostBrace(const Word& w) {
		Word tw = applyTildeExpansion(w);
		Tagged t = renderWord(tw);
		std::vector<Tagged> fields = splitWords(t);
		if (fields.empty() && t.had_quote) {
			Tagged empty;
			empty.had_quote = true;
			fields.push_back(std::move(empty));
		}
		std::vector<std::string> out;
		for (auto& f : fields) {
			auto results = globExpand(f);
			for (auto& s : results) out.push_back(std::move(s));
		}
		return out;
	}

	std::string Expander::expandStringValue(const Word& w) {
		Word tw = applyTildeExpansion(w);
		Tagged t = renderWord(tw);
		return quoteRemove(t);
	}

}  // namespace wbsh

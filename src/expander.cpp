#include "expander.h"
#include "lexer.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

#include <fstream>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace wbsh {

	namespace {

		// ---------------------------------------------------------------------
		// fnmatch-like glob pattern matcher. Supports * ? [...].
		// ---------------------------------------------------------------------

		bool matchHere(const std::string& p, std::size_t pi,
		               const std::string& s, std::size_t si);

		bool matchBracket(const std::string& p, std::size_t& pi, char c) {
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

		bool matchHere(const std::string& p, std::size_t pi,
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

		bool fnmatchFull(const std::string& p, const std::string& s) {
			return matchHere(p, 0, s, 0);
		}

		// ---------------------------------------------------------------------
		// Inline $-expansion for heredoc bodies and ${param-default} defaults.
		// Performs $name, ${...}, $(...), $((...)), `...`, and backslash
		// escapes (\$ \\ \` \"). Tilde, splitting, globbing are NOT applied.
		// ---------------------------------------------------------------------

		bool isNameStart(char c) {
			return c == '_' || std::isalpha(static_cast<unsigned char>(c));
		}
		bool isNameCont(char c) {
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
						while (std::isdigit(static_cast<unsigned char>(peek()))) num.push_back(advance());
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
				try {
					std::size_t idx = 0;
					long long iv = std::stoll(v, &idx, 0);
					if (idx == v.size()) return iv;
				} catch (...) {}
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

	}  // namespace

	// ---------------------------------------------------------------------------
	// Expander construction & arithmetic facade
	// ---------------------------------------------------------------------------

	Expander::Expander(Environment& env, CommandSubstitutor* sub)
		: env_(env), sub_(sub) {}

	long long Expander::evalArith(const std::string& body) {
		// Pre-pass: expand $-forms inside the body (bash recurses into $((...)) bodies).
		std::string expanded = expandHeredoc(body, /*quoted=*/false);
		ArithEval ev(expanded, env_, this, 0);
		return ev.run();
	}

	// ---------------------------------------------------------------------------
	// Inline $-substitution (used for heredocs, default-values, arith pre-pass)
	// ---------------------------------------------------------------------------

	std::string Expander::expandHeredoc(const std::string& body, bool quoted) {
		if (quoted) return body;
		std::string out;
		std::size_t i = 0;
		auto end = body.size();
		while (i < end) {
			char c = body[i];
			if (c == '\\' && i + 1 < end) {
				char nx = body[i + 1];
				if (nx == '\\' || nx == '$' || nx == '`' || nx == '"') {
					out.push_back(nx);
					i += 2;
					continue;
				}
				if (nx == '\n') {
					i += 2;
					continue;
				}
				out.push_back('\\');
				++i;
				continue;
			}
			if (c == '$' && i + 1 < end) {
				char n1 = body[i + 1];
				if (n1 == '{') {
					std::size_t k = i + 2;
					int depth = 1;
					while (k < end && depth > 0) {
						if (body[k] == '{') ++depth;
						else if (body[k] == '}') --depth;
						if (depth > 0) ++k;
					}
					std::string inner = body.substr(i + 2, k - (i + 2));
					out += expandParam(inner, true);
					i = (k < end) ? k + 1 : k;
					continue;
				}
				if (n1 == '(') {
					if (i + 2 < end && body[i + 2] == '(') {
						// $((...)) — scan for matching )) and capture inner.
						std::size_t k = i + 3;
						int depth = 2;
						while (k < end && depth > 0) {
							if (body[k] == '(') ++depth;
							else if (body[k] == ')') --depth;
							if (depth > 0) ++k;
						}
						// k is at the closing second ')'.
						std::size_t inner_end = (k > 0) ? k - 1 : 0;
						std::string inner = body.substr(i + 3, inner_end - (i + 3));
						long long v = evalArith(inner);
						out += std::to_string(v);
						i = (k < end) ? k + 1 : end;
						continue;
					}
					std::size_t k = i + 2;
					int depth = 1;
					while (k < end && depth > 0) {
						if (body[k] == '(') ++depth;
						else if (body[k] == ')') --depth;
						if (depth > 0) ++k;
					}
					std::string inner = body.substr(i + 2, k - (i + 2));
					std::string r = runCmdSubst(inner);
					while (!r.empty() && r.back() == '\n') r.pop_back();
					out += r;
					i = (k < end) ? k + 1 : k;
					continue;
				}
				if (isNameStart(n1) || std::isdigit(static_cast<unsigned char>(n1))
				    || isSpecialParam1(n1)) {
					std::size_t k = i + 1;
					if (isNameStart(n1)) {
						++k;
						while (k < end && isNameCont(body[k])) ++k;
					} else {
						++k;
					}
					std::string name = body.substr(i + 1, k - (i + 1));
					out += lookupParam(name);
					i = k;
					continue;
				}
				out.push_back('$');
				++i;
				continue;
			}
			if (c == '`') {
				std::size_t k = i + 1;
				std::string inner;
				while (k < end && body[k] != '`') {
					if (body[k] == '\\' && k + 1 < end) {
						char nx = body[k + 1];
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

	std::string Expander::interpretAnsiC(const std::string& body) {
		std::string out;
		std::size_t i = 0;
		auto end = body.size();
		while (i < end) {
			char c = body[i];
			if (c != '\\' || i + 1 >= end) { out.push_back(c); ++i; continue; }
			char nx = body[i + 1];
			switch (nx) {
			case 'a':  out.push_back('\a'); i += 2; break;
			case 'b':  out.push_back('\b'); i += 2; break;
			case 'e': case 'E': out.push_back('\x1b'); i += 2; break;
			case 'f':  out.push_back('\f'); i += 2; break;
			case 'n':  out.push_back('\n'); i += 2; break;
			case 'r':  out.push_back('\r'); i += 2; break;
			case 't':  out.push_back('\t'); i += 2; break;
			case 'v':  out.push_back('\v'); i += 2; break;
			case '\\': out.push_back('\\'); i += 2; break;
			case '\'': out.push_back('\''); i += 2; break;
			case '"':  out.push_back('"');  i += 2; break;
			case '?':  out.push_back('?');  i += 2; break;
			case 'x': {
				i += 2;
				int val = 0, cnt = 0;
				while (cnt < 2 && i < end && std::isxdigit(static_cast<unsigned char>(body[i]))) {
					int d = std::isdigit(static_cast<unsigned char>(body[i]))
					        ? body[i] - '0'
					        : std::tolower(static_cast<unsigned char>(body[i])) - 'a' + 10;
					val = val * 16 + d;
					++i; ++cnt;
				}
				out.push_back(static_cast<char>(val));
				break;
			}
			case '0': case '1': case '2': case '3':
			case '4': case '5': case '6': case '7': {
				++i;
				int val = 0, cnt = 0;
				while (cnt < 3 && i < end && body[i] >= '0' && body[i] <= '7') {
					val = val * 8 + (body[i] - '0');
					++i; ++cnt;
				}
				out.push_back(static_cast<char>(val));
				break;
			}
			case 'c':
				if (i + 2 < end) {
					out.push_back(static_cast<char>(body[i + 2] & 0x1f));
					i += 3;
				} else {
					out.push_back('\\');
					++i;
				}
				break;
			default:
				out.push_back('\\');
				out.push_back(nx);
				i += 2;
				break;
			}
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

	std::string Expander::lookupParam(const std::string& name) {
		if (name.empty()) return {};
		if (name.size() == 1) {
			char c = name[0];
			switch (c) {
			case '?': return std::to_string(env_.lastStatus());
			case '$': return std::to_string(env_.shellPid());
			case '!': return std::to_string(env_.lastBgPid());
			case '#': return std::to_string(env_.positional().size());
			case '@':
			case '*': {
				std::string out;
				const auto& p = env_.positional();
				std::string sep = " ";
				if (c == '*') {
					std::string ifs = env_.get("IFS");
					sep = ifs.empty() ? std::string() : std::string(1, ifs[0]);
				}
				for (std::size_t i = 0; i < p.size(); ++i) {
					if (i) out += sep;
					out += p[i];
				}
				return out;
			}
			case '0': return env_.shellName();
			case '-': return env_.shellOptions();
			case '_': return env_.get("_");
			default:
				if (c >= '1' && c <= '9') {
					std::size_t idx = static_cast<std::size_t>(c - '1');
					if (idx < env_.positional().size()) return env_.positional()[idx];
					return {};
				}
				break;
			}
		}
		bool all_digits = !name.empty();
		for (char c : name) if (!std::isdigit(static_cast<unsigned char>(c))) { all_digits = false; break; }
		if (all_digits) {
			std::size_t idx = 0;
			try { idx = static_cast<std::size_t>(std::stoul(name)); }
			catch (...) { idx = 0; }
			if (idx == 0) return env_.shellName();
			if (idx <= env_.positional().size()) return env_.positional()[idx - 1];
			return {};
		}
		// Dynamic special parameters take precedence over any stored value.
		if (name == "RANDOM")  return std::to_string(env_.randomNext());
		if (name == "SECONDS") return std::to_string(env_.secondsSinceStart());
		if (name == "LINENO")  return std::to_string(env_.currentLineno());
		if (name == "BASHPID") {
#ifdef _WIN32
			return std::to_string(static_cast<long long>(::GetCurrentProcessId()));
#else
			return std::to_string(static_cast<long long>(::getpid()));
#endif
		}
		if (!env_.has(name)) {
			if (env_.nounset()) {
				throw ExpandError(name + ": unbound variable");
			}
			return {};
		}
		return env_.get(name);
	}

	std::string Expander::lookupSubscripted(const std::string& name,
	                                        const std::string& subscript_in,
	                                        bool star_join_ifs) {
		// Subscripts permit $-substitutions: `${arr[$i]}`, `${m[$key]}`.
		// `@` and `*` are literal markers and never expanded.
		std::string subscript = subscript_in;
		if (subscript != "@" && subscript != "*"
		    && subscript.find('$') != std::string::npos) {
			subscript = expandHeredoc(subscript, false);
		}
		// Indexed array?
		if (auto* ia = env_.getIndexedArray(name)) {
			if (subscript == "@" || subscript == "*") {
				std::string sep = " ";
				if (subscript == "*") {
					std::string ifs = env_.get("IFS");
					sep = ifs.empty() ? std::string() : std::string(1, ifs[0]);
					if (!star_join_ifs) sep = " ";
				}
				std::string out;
				bool first = true;
				for (const auto& kv : *ia) {
					if (!first) out += sep;
					out += kv.second;
					first = false;
				}
				return out;
			}
			long long idx = 0;
			try { idx = evalArith(subscript); }
			catch (...) { return {}; }
			auto it = ia->find(idx);
			return it == ia->end() ? std::string() : it->second;
		}
		// Associative array?
		if (auto* aa = env_.getAssocArray(name)) {
			if (subscript == "@" || subscript == "*") {
				std::string sep = " ";
				if (subscript == "*" && star_join_ifs) {
					std::string ifs = env_.get("IFS");
					sep = ifs.empty() ? std::string() : std::string(1, ifs[0]);
				}
				std::string out;
				bool first = true;
				for (const auto& kv : *aa) {
					if (!first) out += sep;
					out += kv.second;
					first = false;
				}
				return out;
			}
			auto it = aa->find(subscript);
			return it == aa->end() ? std::string() : it->second;
		}
		// Scalar treated as a 1-element indexed array at index 0.
		if (subscript == "@" || subscript == "*") return lookupParam(name);
		long long idx = 0;
		try { idx = evalArith(subscript); }
		catch (...) { return {}; }
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

	std::string Expander::expandParam(const std::string& body, bool /*quoted_ctx*/) {
		if (body.empty()) return {};

		// Length form: ${#name} / ${#name[i]} / ${#name[@]} / ${#name[*]}
		if (body[0] == '#' && body.size() > 1) {
			std::string rest = body.substr(1);
			if (rest == "@" || rest == "*")
				return std::to_string(env_.positional().size());
			// Subscripted: ${#arr[@]} / ${#arr[i]}.
			std::size_t lb = rest.find('[');
			if (lb != std::string::npos && rest.back() == ']') {
				std::string nm = rest.substr(0, lb);
				std::string sub = rest.substr(lb + 1, rest.size() - lb - 2);
				if (sub == "@" || sub == "*")
					return std::to_string(arrayLength(nm));
				return std::to_string(lookupSubscripted(nm, sub, false).size());
			}
			return std::to_string(lookupParam(rest).size());
		}

		// Indices/keys form: ${!name[@]} / ${!name[*]}
		if (body[0] == '!' && body.size() > 4) {
			std::string rest = body.substr(1);
			std::size_t lb = rest.find('[');
			if (lb != std::string::npos && rest.back() == ']') {
				std::string nm = rest.substr(0, lb);
				std::string sub = rest.substr(lb + 1, rest.size() - lb - 2);
				if (sub == "@" || sub == "*") {
					auto keys = arrayKeys(nm);
					std::string out;
					for (std::size_t k = 0; k < keys.size(); ++k) {
						if (k) out.push_back(' ');
						out += keys[k];
					}
					return out;
				}
			}
		}

		// Identify the parameter name span.
		std::size_t i = 0;
		if (i < body.size() && isNameStart(body[i])) {
			++i;
			while (i < body.size() && isNameCont(body[i])) ++i;
		} else if (i < body.size() && std::isdigit(static_cast<unsigned char>(body[i]))) {
			while (i < body.size() && std::isdigit(static_cast<unsigned char>(body[i]))) ++i;
		} else if (i < body.size() && isSpecialParam1(body[i])) {
			++i;
		} else {
			return {};
		}
		std::string name = body.substr(0, i);
		// Optional [subscript] before any operator.
		std::string subscript;
		bool has_subscript = false;
		if (i < body.size() && body[i] == '[') {
			std::size_t close = body.find(']', i + 1);
			if (close != std::string::npos) {
				subscript = body.substr(i + 1, close - i - 1);
				has_subscript = true;
				i = close + 1;
			}
		}
		auto namedValue = [&]() -> std::string {
			if (has_subscript) return lookupSubscripted(name, subscript, true);
			return lookupParam(name);
		};
		if (i >= body.size()) return namedValue();

		char op = body[i];
		bool colon = false;
		auto recurse = [&](const std::string& s) -> std::string {
			return expandHeredoc(s, false);
		};

		// Default forms: :- :+ := :? and the no-colon variants
		if (op == ':' && i + 1 < body.size() &&
		    (body[i + 1] == '-' || body[i + 1] == '+' ||
		     body[i + 1] == '=' || body[i + 1] == '?')) {
			colon = true;
			++i;
			op = body[i];
		}
		if (op == '-' || op == '+' || op == '=' || op == '?') {
			std::string arg = body.substr(i + 1);
			bool unset = !env_.has(name)
			          && !std::isdigit(static_cast<unsigned char>(name[0]))
			          && !isSpecialParam1(name[0]);
			std::string cur = namedValue();
			bool empty_or_unset = colon ? (unset || cur.empty()) : unset;
			switch (op) {
			case '-': return empty_or_unset ? recurse(arg) : cur;
			case '+': return empty_or_unset ? std::string() : recurse(arg);
			case '=': {
				if (empty_or_unset) {
					std::string v = recurse(arg);
					env_.set(name, v);
					return v;
				}
				return cur;
			}
			case '?': {
				if (empty_or_unset) {
					std::string msg = arg.empty()
						? (name + ": parameter null or not set")
						: recurse(arg);
					throw ExpandError(msg);
				}
				return cur;
			}
			}
		}

		// Substring: ${name:offset[:length]} or ${name[@]:offset:length}
		if (op == ':') {
			// Array-slice form: slice over elements, then join with space.
			if (has_subscript && (subscript == "@" || subscript == "*")) {
				auto elems = arrayValues(name);
				std::string args = body.substr(i + 1);
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
				long long n = static_cast<long long>(elems.size());
				long long off = 0;
				try { off = evalArith(off_s); } catch (...) { off = 0; }
				if (off < 0) off = std::max<long long>(0, n + off);
				if (off > n) off = n;
				long long take = n - off;
				if (!len_s.empty()) {
					try { take = evalArith(len_s); } catch (...) { take = 0; }
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
			return substringExpand(namedValue(), body.substr(i + 1));
		}

		// Prefix strip: ${name#pat} / ${name##pat}
		if (op == '#') {
			bool greedy = (i + 1 < body.size() && body[i + 1] == '#');
			std::string pat = body.substr(greedy ? i + 2 : i + 1);
			return stripPrefix(namedValue(), recurse(pat), greedy);
		}

		// Suffix strip: ${name%pat} / ${name%%pat}
		if (op == '%') {
			bool greedy = (i + 1 < body.size() && body[i + 1] == '%');
			std::string pat = body.substr(greedy ? i + 2 : i + 1);
			return stripSuffix(namedValue(), recurse(pat), greedy);
		}

		// Replace: ${name/pat/rep} / ${name//pat/rep} / ${name/#pat/rep} / ${name/%pat/rep}
		if (op == '/') {
			bool all = (i + 1 < body.size() && body[i + 1] == '/');
			std::string args = body.substr(all ? i + 2 : i + 1);
			bool anchor_start = false, anchor_end = false;
			if (!args.empty() && args[0] == '#') { anchor_start = true; args.erase(0, 1); }
			else if (!args.empty() && args[0] == '%') { anchor_end = true; args.erase(0, 1); }
			std::size_t slash = args.find('/');
			std::string pat = (slash == std::string::npos) ? args : args.substr(0, slash);
			std::string rep = (slash == std::string::npos) ? std::string() : args.substr(slash + 1);
			return replacePattern(namedValue(), recurse(pat), recurse(rep),
			                      all, anchor_start, anchor_end);
		}

		return namedValue();
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

	std::string Expander::replacePattern(const std::string& val, const std::string& pat,
	                                     const std::string& rep, bool all,
	                                     bool anchor_start, bool anchor_end) {
		if (pat.empty()) return val;

		if (anchor_start) {
			// Match longest prefix.
			for (long long L = static_cast<long long>(val.size()); L >= 0; --L) {
				if (fnmatchFull(pat, val.substr(0, static_cast<std::size_t>(L))))
					return rep + val.substr(static_cast<std::size_t>(L));
			}
			return val;
		}
		if (anchor_end) {
			std::size_t n = val.size();
			for (long long L = static_cast<long long>(n); L >= 0; --L) {
				std::size_t off = n - static_cast<std::size_t>(L);
				if (fnmatchFull(pat, val.substr(off, static_cast<std::size_t>(L))))
					return val.substr(0, off) + rep;
			}
			return val;
		}

		// Unanchored: find longest match starting at each position.
		std::string out;
		std::size_t i = 0;
		while (i <= val.size()) {
			long long best = -1;
			for (long long L = static_cast<long long>(val.size() - i); L >= 0; --L) {
				if (fnmatchFull(pat, val.substr(i, static_cast<std::size_t>(L)))) {
					best = L;
					break;
				}
			}
			if (best >= 0) {
				out += rep;
				if (best == 0) {
					if (i < val.size()) out.push_back(val[i]);
					++i;
				} else {
					i += static_cast<std::size_t>(best);
				}
				if (!all) {
					out += val.substr(i);
					return out;
				}
			} else {
				if (i < val.size()) out.push_back(val[i]);
				++i;
			}
		}
		return out;
	}

	// ---------------------------------------------------------------------------
	// Word rendering
	// ---------------------------------------------------------------------------

	Expander::Tagged Expander::renderWord(const Word& w) {
		Tagged t;
		for (const auto& s : w.segments) {
			if (s.kind == WordSegment::Kind::SingleQuoted ||
			    s.kind == WordSegment::Kind::DoubleQuoted ||
			    s.kind == WordSegment::Kind::Escaped ||
			    s.kind == WordSegment::Kind::DollarSingle) {
				t.had_quote = true;
			}
		}
		for (const auto& s : w.segments) {
			renderSegment(s, t, /*inside_dq=*/false);
		}
		return t;
	}

	void Expander::renderSegment(const WordSegment& s, Tagged& out, bool inside_dq) {
		using K = WordSegment::Kind;
		std::uint8_t mark = inside_dq ? F_QUOTED : 0;
		switch (s.kind) {
		case K::Literal:
			for (char c : s.text) out.push(c, mark);
			break;
		case K::Escaped:
			for (char c : s.text) out.push(c, F_QUOTED);
			break;
		case K::SingleQuoted:
			for (char c : s.text) out.push(c, F_QUOTED);
			break;
		case K::DoubleQuoted:
			for (const auto& n : s.nested) renderSegment(n, out, /*inside_dq=*/true);
			break;
		case K::DollarSingle: {
			std::string ic = interpretAnsiC(s.text);
			for (char c : ic) out.push(c, F_QUOTED);
			break;
		}
		case K::SimpleVar: {
			// "$@" is field-aware: each positional becomes its own field even
			// inside double quotes. We emit the positionals joined by an
			// UNQUOTED IFS-space, with each positional's chars carrying the
			// caller's quoting context. Word splitting then breaks at the
			// unquoted spaces, preserving spaces inside individual positionals.
			if (s.text == "@") {
				const auto& p = env_.positional();
				for (std::size_t i = 0; i < p.size(); ++i) {
					if (i) out.push(' ', 0);
					for (char c : p[i]) out.push(c, mark);
				}
				break;
			}
			std::string v = lookupParam(s.text);
			for (char c : v) out.push(c, mark);
			break;
		}
		case K::ParamExp: {
			// Field-aware emission for `${name[@]}` / `${name[*]}` (and the
			// indices form `${!name[@]}`): each element becomes its own
			// field even inside double quotes, exactly like `$@`.
			auto fieldArrayName = [](const std::string& body) -> std::string {
				if (body.size() < 4) return {};
				// `${#name[@]}` and other length forms are not field-aware.
				if (body[0] == '#') return {};
				bool indices = body[0] == '!';
				std::size_t start = indices ? 1 : 0;
				std::size_t lb = body.find('[', start);
				if (lb == std::string::npos) return {};
				if (lb + 2 >= body.size()) return {};
				if (body[lb + 1] != '@' && body[lb + 1] != '*') return {};
				if (body[lb + 2] != ']') return {};
				if (lb + 3 != body.size()) return {};   // no trailing ops
				return body.substr(start, lb - start);
			};
			std::string fname = fieldArrayName(s.text);
			if (!fname.empty()) {
				bool indices = s.text[0] == '!';
				bool star = (s.text.find('*') != std::string::npos);
				std::vector<std::string> elems = indices
				    ? arrayKeys(fname)
				    : arrayValues(fname);
				if (star && inside_dq) {
					std::string ifs = env_.get("IFS");
					std::string sep = ifs.empty() ? std::string() : std::string(1, ifs[0]);
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
				if (!elems.empty() || inside_dq) out.had_quote |= inside_dq;
				break;
			}
			std::string v = expandParam(s.text, inside_dq);
			for (char c : v) out.push(c, mark);
			break;
		}
		case K::CmdSubst: {
			std::string v = runCmdSubst(s.text);
			while (!v.empty() && v.back() == '\n') v.pop_back();
			for (char c : v) out.push(c, mark);
			break;
		}
		case K::ArithExp: {
			long long r = evalArith(s.text);
			std::string v = std::to_string(r);
			for (char c : v) out.push(c, mark);
			break;
		}
		case K::ProcSubst: {
			// `<(cmd)` runs the inner command, captures stdout to a temp
			// file, and substitutes that file's path. `>(cmd)` would need
			// reverse-direction streaming (named pipe); not supported yet.
			if (s.proc_dir != '<') {
				std::fprintf(stderr,
					"wbsh: process substitution >(...) not supported\n");
				break;
			}
#ifdef _WIN32
			char tdir[MAX_PATH];
			DWORD nd = GetTempPathA(MAX_PATH, tdir);
			char tpath[MAX_PATH];
			if (nd == 0 || nd > MAX_PATH
			    || GetTempFileNameA(tdir, "wbsh", 0, tpath) == 0) break;
			// Process substitution preserves the producer's stdout exactly
			// (unlike $(...) which strips trailing newlines).
			std::string body = sub_ ? sub_->runRaw(s.text) : std::string();
			{
				std::ofstream f(tpath, std::ios::binary | std::ios::trunc);
				f.write(body.data(), body.size());
			}
			pending_temp_files_.emplace_back(tpath);
			std::string posix = path_conv_.toPosix(tpath);
			for (char c : posix) out.push(c, F_QUOTED);
			out.had_quote = true;
#endif
			break;
		}
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

	std::vector<std::string> Expander::globExpand(const Tagged& t) {
		if (env_.noglob()) {
			return { quoteRemove(t) };
		}
		// Quick scan for unquoted glob meta.
		bool has_meta = false;
		for (std::size_t i = 0; i < t.size(); ++i) {
			if ((t.flags[i] & F_QUOTED) == 0) {
				char c = t.text[i];
				if (c == '*' || c == '?' || c == '[') { has_meta = true; break; }
			}
		}
		if (!has_meta) return { quoteRemove(t) };

		// Build path components, recording per-char quotedness for matching.
		struct Comp {
			std::string text;
			std::vector<std::uint8_t> quoted;
			bool hasMeta() const {
				for (std::size_t i = 0; i < text.size(); ++i) {
					if (!quoted[i]) {
						char c = text[i];
						if (c == '*' || c == '?' || c == '[') return true;
					}
				}
				return false;
			}
			std::string asPattern() const {
				std::string p;
				for (std::size_t i = 0; i < text.size(); ++i) {
					char c = text[i];
					if (quoted[i]) {
						if (c == '*' || c == '?' || c == '[' || c == '\\') p.push_back('\\');
						p.push_back(c);
					} else {
						p.push_back(c);
					}
				}
				return p;
			}
			std::string asLiteral() const { return text; }
		};

		std::vector<Comp> comps;
		Comp cur;
		bool absolute = false;
		bool first = true;
		for (std::size_t i = 0; i < t.size(); ++i) {
			char c = t.text[i];
			bool q = (t.flags[i] & F_QUOTED) != 0;
			if (!q && c == '/') {
				if (first) absolute = true;
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

		if (comps.empty()) return { quoteRemove(t) };

		namespace fs = std::filesystem;

		std::vector<std::string> current;
		current.push_back(absolute ? std::string("/") : std::string("."));

		auto join = [](const std::string& dir, const std::string& name) -> std::string {
			if (dir == "." || dir.empty()) return name;
			if (dir == "/") return "/" + name;
			if (!dir.empty() && dir.back() == '/') return dir + name;
			return dir + "/" + name;
		};

		const bool dotglob_on = env_.dotglob();
		const bool nocaseglob_on = env_.nocaseglob();
		const bool globstar_on = env_.globstar();
		auto matchOne = [&](const std::string& pat, const std::string& name) {
			if (!nocaseglob_on) return fnmatchFull(pat, name);
			std::string lp = pat, ln = name;
			for (auto& c : lp) c = (char)std::tolower((unsigned char)c);
			for (auto& c : ln) c = (char)std::tolower((unsigned char)c);
			return fnmatchFull(lp, ln);
		};
		for (std::size_t ci = 0; ci < comps.size(); ++ci) {
			auto& comp = comps[ci];
			std::vector<std::string> next;
			// Globstar `**`: matches the current dir AND every descendant.
			// If `**` is the trailing component we also list everything under
			// each accumulated dir (recursive ls). Otherwise the next
			// component is applied per-dir as usual.
			bool is_globstar = globstar_on && (comp.text == "**");
			if (is_globstar) {
				bool last = (ci + 1 == comps.size());
				std::set<std::string> acc;
				for (auto& d : current) {
					acc.insert(d);
					std::string posix_dir = (d.empty() ? "." : d);
					std::string list_dir = path_conv_.toWin32(posix_dir);
					std::error_code ec;
					fs::recursive_directory_iterator rit(list_dir,
						fs::directory_options::skip_permission_denied, ec);
					if (ec) continue;
					for (auto cur = rit; cur != fs::recursive_directory_iterator(); cur.increment(ec)) {
						if (ec) break;
						std::string name;
						try { name = cur->path().filename().string(); }
						catch (...) { continue; }
						if (!name.empty() && name[0] == '.') {
							cur.disable_recursion_pending();
							continue;
						}
						std::error_code dec;
						std::string rel = fs::relative(cur->path(), list_dir, dec).string();
						std::replace(rel.begin(), rel.end(), '\\', '/');
						if (last) {
							acc.insert(join(d, rel));
						} else if (cur->is_directory(dec)) {
							acc.insert(join(d, rel));
						}
					}
				}
				for (auto& s : acc) next.push_back(s);
				std::sort(next.begin(), next.end());
				current = std::move(next);
				continue;
			}
			if (!comp.hasMeta()) {
				for (auto& d : current) next.push_back(join(d, comp.asLiteral()));
			} else {
				std::string pat = comp.asPattern();
				bool pat_starts_dot = !pat.empty() && pat[0] == '.';
				for (auto& d : current) {
					std::string posix_dir = (d.empty() ? "." : d);
					std::string list_dir = path_conv_.toWin32(posix_dir);
					std::error_code ec;
					fs::directory_iterator it(list_dir, ec);
					if (ec) continue;
					std::vector<std::string> matches;
					for (auto& entry : it) {
						std::string name;
						try { name = entry.path().filename().string(); }
						catch (...) { continue; }
						if (name.empty()) continue;
						if (name[0] == '.' && !pat_starts_dot && !dotglob_on) continue;
						if (matchOne(pat, name)) matches.push_back(name);
					}
					std::sort(matches.begin(), matches.end());
					for (auto& m : matches) next.push_back(join(d, m));
				}
			}
			current = std::move(next);
		}

		if (current.empty()) {
			// nullglob: drop the unmatched pattern entirely. Default bash
			// behaviour (and ours when off) is to leave the literal pattern.
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

	namespace {

		bool _isInteger(const std::string& v) {
			if (v.empty()) return false;
			std::size_t i = (v[0] == '-' || v[0] == '+') ? 1 : 0;
			if (i == v.size()) return false;
			for (; i < v.size(); ++i)
				if (!std::isdigit(static_cast<unsigned char>(v[i]))) return false;
			return true;
		}

		// Returns the alternatives generated by the leftmost UNQUOTED brace
		// pattern in `s`. Empty result means "no expandable brace found here";
		// caller treats `s` as a single unchanged result.
		std::vector<std::string> braceExpandOnce(const std::string& s) {
			std::size_t i = 0;
			std::size_t open = std::string::npos;
			// Quote-aware scan for the leftmost '{'.
			while (i < s.size()) {
				char c = s[i];
				if (c == '\\' && i + 1 < s.size()) { i += 2; continue; }
				if (c == '\'') {
					++i;
					while (i < s.size() && s[i] != '\'') ++i;
					if (i < s.size()) ++i;
					continue;
				}
				if (c == '"') {
					++i;
					while (i < s.size() && s[i] != '"') {
						if (s[i] == '\\' && i + 1 < s.size()) i += 2;
						else ++i;
					}
					if (i < s.size()) ++i;
					continue;
				}
				if (c == '{') { open = i; break; }
				++i;
			}
			if (open == std::string::npos) return {};

			// Find matching '}', tracking nesting and quoted regions, while
			// remembering top-level commas.
			int depth = 1;
			std::size_t j = open + 1;
			std::vector<std::size_t> commas;
			while (j < s.size() && depth > 0) {
				char c = s[j];
				if (c == '\\' && j + 1 < s.size()) { j += 2; continue; }
				if (c == '\'') {
					++j;
					while (j < s.size() && s[j] != '\'') ++j;
					if (j < s.size()) ++j;
					continue;
				}
				if (c == '"') {
					++j;
					while (j < s.size() && s[j] != '"') {
						if (s[j] == '\\' && j + 1 < s.size()) j += 2;
						else ++j;
					}
					if (j < s.size()) ++j;
					continue;
				}
				if (c == '{') { ++depth; ++j; continue; }
				if (c == '}') { --depth; if (depth == 0) break; ++j; continue; }
				if (c == ',' && depth == 1) commas.push_back(j);
				++j;
			}
			if (depth != 0) return {};
			std::size_t close = j;

			std::string body   = s.substr(open + 1, close - open - 1);
			std::string prefix = s.substr(0, open);
			std::string suffix = s.substr(close + 1);

			std::vector<std::string> alts;
			if (!commas.empty()) {
				std::vector<std::size_t> body_commas;
				for (auto p : commas) body_commas.push_back(p - (open + 1));
				std::size_t start = 0;
				for (auto p : body_commas) {
					alts.push_back(body.substr(start, p - start));
					start = p + 1;
				}
				alts.push_back(body.substr(start));
			} else {
				// Sequence form: {X..Y[..Z]}.
				auto dd = body.find("..");
				if (dd == std::string::npos) return {};
				std::string from = body.substr(0, dd);
				std::string rest = body.substr(dd + 2);
				auto dd2 = rest.find("..");
				std::string to    = (dd2 == std::string::npos) ? rest : rest.substr(0, dd2);
				std::string stepS = (dd2 == std::string::npos) ? std::string() : rest.substr(dd2 + 2);
				if (_isInteger(from) && _isInteger(to)) {
					long long a = std::stoll(from);
					long long b = std::stoll(to);
					long long step = stepS.empty() ? 1 : std::stoll(stepS);
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
				} else if (from.size() == 1 && to.size() == 1
				    && std::isalpha(static_cast<unsigned char>(from[0]))
				    && std::isalpha(static_cast<unsigned char>(to[0]))) {
					int a = static_cast<unsigned char>(from[0]);
					int b = static_cast<unsigned char>(to[0]);
					int step = stepS.empty() ? 1 : std::stoi(stepS);
					if (step == 0) step = 1;
					if ((a < b && step < 0) || (a > b && step > 0)) step = -step;
					for (int v = a; (step > 0) ? (v <= b) : (v >= b); v += step) {
						alts.emplace_back(1, static_cast<char>(v));
					}
				} else {
					return {};
				}
			}

			std::vector<std::string> out;
			out.reserve(alts.size());
			for (const auto& alt : alts) {
				out.push_back(prefix + alt + suffix);
			}
			return out;
		}

		std::vector<std::string> braceExpandAll(const std::string& s) {
			auto first = braceExpandOnce(s);
			if (first.empty()) return { s };
			std::vector<std::string> out;
			for (const auto& alt : first) {
				auto sub = braceExpandAll(alt);
				for (auto& r : sub) out.push_back(std::move(r));
			}
			return out;
		}

	}  // namespace

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
			Lexer lex(alt);
			auto tokens = lex.tokenize();
			for (const auto& t : tokens) {
				if (t.kind != TokKind::Word) continue;
				Word w2;
				w2.segments = t.segments;
				w2.raw = t.text;
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

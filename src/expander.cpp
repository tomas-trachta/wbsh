/**
 * @file expander.cpp
 * @brief Word-expansion pipeline: parameters, arithmetic, braces, globs.
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

#include "fnmatch.h"
#include "lexer.h"
#include "numparse.h"
#include "strscan.h"

namespace wbsh {

	// A variable whose value is itself an expression is evaluated
	// recursively; this bounds `a=b; b=a` style reference cycles.
	static const int kArithNestingLimit = 16;

	static const int kAnsicHexDigits   = 2;
	static const int kAnsicOctalDigits = 3;

	static bool isNameStart(char c) {
		return c == '_' || std::isalpha(static_cast<unsigned char>(c));
	}

	static bool isNameCont(char c) {
		return c == '_' || std::isalnum(static_cast<unsigned char>(c));
	}

	static bool isDigit(char c) {
		return std::isdigit(static_cast<unsigned char>(c)) != 0;
	}

	static int hexDigitValue(char c) {
		if (isDigit(c)) return c - '0';
		return std::tolower(static_cast<unsigned char>(c)) - 'a' + 10;
	}

	static std::size_t scanIdentifierEnd(const std::string& text, std::size_t start) {
		std::size_t end = start + 1;
		while (end < text.size() && isNameCont(text[end])) ++end;
		return end;
	}

	static std::size_t scanDigitsEnd(const std::string& text, std::size_t start) {
		std::size_t end = start;
		while (end < text.size() && isDigit(text[end])) ++end;
		return end;
	}

	static bool isPlainName(const std::string& name) {
		if (name.empty() || !isNameStart(name[0])) return false;
		return scanIdentifierEnd(name, 0) == name.size();
	}

	static void stripTrailingNewlines(std::string& text) {
		while (!text.empty() && text.back() == '\n') text.pop_back();
	}

	static std::string joinWith(const std::vector<std::string>& items, const std::string& sep) {
		std::string out;
		for (std::size_t i = 0; i < items.size(); ++i) {
			if (i != 0) out += sep;
			out += items[i];
		}

		return out;
	}

	template <class Map>
	static std::string joinMapValues(const Map& items, const std::string& sep) {
		std::string out;
		bool first = true;
		for (const auto& entry : items) {
			if (!first) out += sep;
			out += entry.second;
			first = false;
		}

		return out;
	}

	namespace arith_detail {

		struct AssignOp {
			const char* spelling;
			std::size_t length;
		};

		// Longest spellings first so `<<=` is not read as `<` `<=`.
		static const AssignOp kAssignOps[] = {
			{ "<<=", 3 }, { ">>=", 3 },
			{ "+=", 2 }, { "-=", 2 }, { "*=", 2 }, { "/=", 2 }, { "%=", 2 },
			{ "&=", 2 }, { "^=", 2 }, { "|=", 2 },
			{ "=", 1 },
		};

		static long long truth(bool condition) {
			return condition ? 1 : 0;
		}

		static long long combineAssign(char op, long long lhs, long long rhs) {
			switch (op) {
			case '=': return rhs;
			case '+': return lhs + rhs;
			case '-': return lhs - rhs;
			case '*': return lhs * rhs;
			case '/': return rhs != 0 ? lhs / rhs : 0;
			case '%': return rhs != 0 ? lhs % rhs : 0;
			case '&': return lhs & rhs;
			case '^': return lhs ^ rhs;
			case '|': return lhs | rhs;
			case '<': return lhs << rhs;
			case '>': return lhs >> rhs;
			default:  return rhs;
			}
		}

		// Digit value of `c` in `base` for the `base#digits` literal form,
		// or -1 when `c` is not a digit at all. Past base 36 upper-case
		// letters continue the sequence after lower-case ones.
		static int basedDigitValue(char c, int base) {
			if (c >= '0' && c <= '9') return c - '0';
			if (c >= 'a' && c <= 'z') return c - 'a' + 10;
			if (c >= 'A' && c <= 'Z') return c - 'A' + (base > 36 ? 36 : 10);
			if (c == '@') return 62;
			if (c == '_') return 63;
			return -1;
		}

		// Recursive-descent evaluator for `$(( ... ))`. Precedence climbs
		// from parseComma() down to parsePrimary(), one method per level.
		class ArithEval {
		public:
			ArithEval(const std::string& src, Environment& env, int depth)
				: src_(src), env_(env), depth_(depth) {}

			long long run() {
				skipWs();
				if (eof()) return 0;
				return parseComma();
			}

		private:
			long long parseComma() {
				long long result = parseAssign();
				while (consume(',')) result = parseAssign();
				return result;
			}

			long long parseAssign() {
				const std::size_t save = pos_;
				skipWs();
				if (!eof() && isNameStart(peek())) {
					const std::string name = readIdent();
					skipWs();
					const AssignOp* op = matchAssignOp();
					if (op != nullptr) return applyAssignment(name, *op);
				}

				pos_ = save;
				return parseTernary();
			}

			const AssignOp* matchAssignOp() {
				for (const AssignOp& op : kAssignOps) {
					if (pos_ + op.length > src_.size()) continue;
					if (src_.compare(pos_, op.length, op.spelling) != 0) continue;
					// A lone `=` must not claim the first half of `==`.
					if (op.length == 1 && pos_ + 1 < src_.size() && src_[pos_ + 1] == '=') continue;

					pos_ += op.length;
					return &op;
				}

				return nullptr;
			}

			long long applyAssignment(const std::string& name, const AssignOp& op) {
				const long long rhs = parseAssign();
				const long long lhs = (op.length == 1) ? 0 : variableValue(name);
				const long long value = combineAssign(op.spelling[0], lhs, rhs);
				env_.set(name, std::to_string(value));
				return value;
			}

			long long parseTernary() {
				const long long condition = parseLogOr();
				if (!consume('?')) return condition;

				const long long when_true = parseAssign();
				consume(':');
				const long long when_false = parseAssign();
				return condition != 0 ? when_true : when_false;
			}

			long long parseLogOr() {
				long long left = parseLogAnd();
				while (consume("||")) {
					const long long right = parseLogAnd();
					left = truth(left != 0 || right != 0);
				}

				return left;
			}

			long long parseLogAnd() {
				long long left = parseBitOr();
				while (consume("&&")) {
					const long long right = parseBitOr();
					left = truth(left != 0 && right != 0);
				}

				return left;
			}

			long long parseBitOr() {
				long long left = parseBitXor();
				while (consumeOperator("|", "|=")) {
					const long long right = parseBitXor();
					left |= right;
				}

				return left;
			}

			long long parseBitXor() {
				long long left = parseBitAnd();
				while (consumeOperator("^", "=")) {
					const long long right = parseBitAnd();
					left ^= right;
				}

				return left;
			}

			long long parseBitAnd() {
				long long left = parseEq();
				while (consumeOperator("&", "&=")) {
					const long long right = parseEq();
					left &= right;
				}

				return left;
			}

			long long parseEq() {
				long long left = parseRel();
				for (;;) {
					if (consume("==")) {
						const long long right = parseRel();
						left = truth(left == right);
					} else if (consume("!=")) {
						const long long right = parseRel();
						left = truth(left != right);
					} else {
						return left;
					}
				}
			}

			long long parseRel() {
				long long left = parseShift();
				for (;;) {
					if (consume("<=")) {
						const long long right = parseShift();
						left = truth(left <= right);
					} else if (consume(">=")) {
						const long long right = parseShift();
						left = truth(left >= right);
					} else if (consumeOperator("<", "<=")) {
						const long long right = parseShift();
						left = truth(left < right);
					} else if (consumeOperator(">", ">=")) {
						const long long right = parseShift();
						left = truth(left > right);
					} else {
						return left;
					}
				}
			}

			long long parseShift() {
				long long left = parseAdd();
				for (;;) {
					if (consumeOperator("<<", "=")) {
						const long long right = parseAdd();
						left = left << right;
					} else if (consumeOperator(">>", "=")) {
						const long long right = parseAdd();
						left = left >> right;
					} else {
						return left;
					}
				}
			}

			long long parseAdd() {
				long long left = parseMul();
				for (;;) {
					if (consumeOperator("+", "+=")) {
						const long long right = parseMul();
						left = left + right;
					} else if (consumeOperator("-", "-=")) {
						const long long right = parseMul();
						left = left - right;
					} else {
						return left;
					}
				}
			}

			long long parseMul() {
				long long left = parsePow();
				for (;;) {
					if (consumeOperator("*", "*=")) {
						const long long right = parsePow();
						left = left * right;
					} else if (consumeOperator("/", "=")) {
						const long long right = parsePow();
						left = right != 0 ? left / right : 0;
					} else if (consumeOperator("%", "=")) {
						const long long right = parsePow();
						left = right != 0 ? left % right : 0;
					} else {
						return left;
					}
				}
			}

			long long parsePow() {
				const long long base = parseUnary();
				skipWs();
				if (!consume("**")) return base;

				const long long exponent = parsePow();
				if (exponent < 0) return 0;

				long long result = 1;
				for (long long k = 0; k < exponent; ++k) result *= base;
				return result;
			}

			long long parseUnary() {
				skipWs();
				if (consume("++")) return prefixStep(+1);
				if (consume("--")) return prefixStep(-1);
				if (consume('+')) return parseUnary();
				if (consume('-')) return -parseUnary();
				if (consume('!')) return truth(parseUnary() == 0);
				if (consume('~')) return ~parseUnary();
				return parsePrimary();
			}

			long long prefixStep(long long delta) {
				const std::string name = readIdent();
				const long long value = variableValue(name) + delta;
				env_.set(name, std::to_string(value));
				return value;
			}

			long long postfixStep(const std::string& name, long long delta) {
				const long long value = variableValue(name);
				env_.set(name, std::to_string(value + delta));
				return value;
			}

			long long parsePrimary() {
				skipWs();
				if (eof()) return 0;
				if (consume('(')) {
					const long long value = parseComma();
					consume(')');
					return value;
				}

				if (isDigit(peek())) return readNumber();
				if (isNameStart(peek())) return parseVariable();
				if (consume('$')) return parseDollar();

				++pos_;
				return 0;
			}

			long long parseVariable() {
				const std::string name = readIdent();
				skipWs();
				if (consume("++")) return postfixStep(name, +1);
				if (consume("--")) return postfixStep(name, -1);
				return variableValue(name);
			}

			long long parseDollar() {
				if (isNameStart(peek())) return variableValue(readIdent());
				if (!isDigit(peek())) return 0;

				std::string number;
				while (isDigit(peek())) number.push_back(advance());
				return variableValue(number);
			}

			long long variableValue(const std::string& name) {
				if (name.empty()) return 0;
				const std::string text = env_.get(name);
				if (text.empty()) return 0;

				long long value = 0;
				std::size_t consumed = 0;
				if (parseLL(text, value, 0, &consumed) && consumed == text.size()) return value;
				if (depth_ > kArithNestingLimit) return 0;

				ArithEval inner(text, env_, depth_ + 1);
				return inner.run();
			}

			long long readNumber() {
				if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
					pos_ += 2;
					return readHexDigits();
				}

				if (peek() == '0' && isDigit(peek(1))) {
					++pos_;
					return readOctalDigits();
				}

				long long value = 0;
				while (isDigit(peek())) value = value * 10 + (advance() - '0');
				if (peek() != '#') return value;

				++pos_;
				int base = static_cast<int>(value);
				if (base < 2) base = 10;
				return readBasedDigits(base);
			}

			long long readHexDigits() {
				long long value = 0;
				while (std::isxdigit(static_cast<unsigned char>(peek()))) {
					value = value * 16 + hexDigitValue(advance());
				}

				return value;
			}

			long long readOctalDigits() {
				long long value = 0;
				while (peek() >= '0' && peek() <= '7') value = value * 8 + (advance() - '0');
				return value;
			}

			long long readBasedDigits(int base) {
				long long value = 0;
				for (;;) {
					const int digit = basedDigitValue(peek(), base);
					if (digit < 0 || digit >= base) return value;

					value = value * base + digit;
					advance();
				}
			}

			std::string readIdent() {
				skipWs();
				const std::size_t start = pos_;
				if (!eof() && isNameStart(peek())) pos_ = scanIdentifierEnd(src_, pos_);
				return src_.substr(start, pos_ - start);
			}

			void skipWs() {
				while (!eof() && std::isspace(static_cast<unsigned char>(peek()))) ++pos_;
			}

			bool eof() const {
				return pos_ >= src_.size();
			}

			char peek(std::size_t offset = 0) const {
				return pos_ + offset < src_.size() ? src_[pos_ + offset] : '\0';
			}

			char advance() {
				return src_[pos_++];
			}

			bool consume(char c) {
				skipWs();
				if (peek() != c) return false;

				++pos_;
				return true;
			}

			bool consume(const char* text) {
				skipWs();
				const std::size_t length = std::strlen(text);
				if (pos_ + length > src_.size()) return false;
				if (src_.compare(pos_, length, text) != 0) return false;

				pos_ += length;
				return true;
			}

			// Takes `op` unless the character after it turns it into a
			// different operator: `|` followed by `|` or `=` is not bit-or.
			bool consumeOperator(const char* op, const char* not_followed_by) {
				skipWs();
				const std::size_t length = std::strlen(op);
				if (pos_ + length > src_.size()) return false;
				if (src_.compare(pos_, length, op) != 0) return false;

				const char after = peek(length);
				if (after != '\0' && std::strchr(not_followed_by, after) != nullptr) return false;

				pos_ += length;
				return true;
			}

			const std::string& src_;
			std::size_t pos_ = 0;
			Environment& env_;
			int depth_;
		};

	}  // namespace arith_detail

	Expander::Expander(Environment& env, CommandSubstitutor* sub)
		: env_(env), sub_(sub) {}

	long long Expander::evalArith(const std::string& body) {
		const std::string expanded = expandHeredoc(body, /*quoted=*/false);
		if (aborting()) return 0;

		arith_detail::ArithEval evaluator(expanded, env_, 0);
		return evaluator.run();
	}

	// Index of the bracket that closes the group already `depth` deep at
	// `after_open`, or body.size() when it is never closed.
	static std::size_t scanBalanced(const std::string& body, std::size_t after_open,
			char open, char close, int depth) {
		std::size_t k = after_open;
		while (k < body.size() && depth > 0) {
			if (body[k] == open) ++depth;
			else if (body[k] == close) --depth;
			if (depth > 0) ++k;
		}

		return k;
	}

	static void expandHeredocBackslash(const std::string& body, std::size_t& i,
			std::string& out) {
		const char next = body[i + 1];
		if (next == '\\' || next == '$' || next == '`' || next == '"') {
			out.push_back(next);
			i += 2;
			return;
		}

		if (next == '\n') {
			i += 2;
			return;
		}

		out.push_back('\\');
		++i;
	}

	void Expander::expandHeredocCmdSubst(const std::string& body, std::size_t& i,
			std::string& out) {
		const std::size_t end = body.size();
		const std::size_t k = scanBalanced(body, i + 2, '(', ')', 1);
		const std::string inner = body.substr(i + 2, k - (i + 2));

		std::string output = runCmdSubst(inner);
		stripTrailingNewlines(output);
		out += output;
		i = (k < end) ? k + 1 : k;
	}

	void Expander::expandHeredocArith(const std::string& body, std::size_t& i,
			std::string& out) {
		const std::size_t end = body.size();
		const std::size_t k = scanBalanced(body, i + 3, '(', ')', 2);
		const std::size_t inner_end = (k > 0) ? k - 1 : 0;
		const std::string inner = body.substr(i + 3, inner_end - (i + 3));

		out += std::to_string(evalArith(inner));
		i = (k < end) ? k + 1 : end;
	}

	void Expander::expandHeredocArithBracket(const std::string& body, std::size_t& i,
			std::string& out) {
		const std::size_t end = body.size();
		const std::size_t k = scanBalanced(body, i + 2, '[', ']', 1);
		const std::string inner = body.substr(i + 2, k - (i + 2));

		out += std::to_string(evalArith(inner));
		i = (k < end) ? k + 1 : end;
	}

	void Expander::expandHeredocParamBraces(const std::string& body, std::size_t& i,
			std::string& out) {
		const std::size_t end = body.size();
		const std::size_t k = scanBalanced(body, i + 2, '{', '}', 1);
		const std::string inner = body.substr(i + 2, k - (i + 2));

		out += expandParam(inner, true);
		i = (k < end) ? k + 1 : k;
	}

	void Expander::expandHeredocSimpleParam(const std::string& body, std::size_t& i,
			std::string& out) {
		const std::size_t start = i + 1;
		const std::size_t k = isNameStart(body[start])
			? scanIdentifierEnd(body, start)
			: start + 1;

		out += lookupParam(body.substr(start, k - start));
		i = k;
	}

	void Expander::expandHeredocBackquote(const std::string& body, std::size_t& i,
			std::string& out) {
		const std::size_t end = body.size();
		std::size_t k = i + 1;
		std::string inner;
		while (k < end && body[k] != '`') {
			if (body[k] == '\\' && k + 1 < end) {
				const char next = body[k + 1];
				if (next == '$' || next == '`' || next == '\\') {
					inner.push_back(next);
					k += 2;
					continue;
				}
			}

			inner.push_back(body[k]);
			++k;
		}

		std::string output = runCmdSubst(inner);
		stripTrailingNewlines(output);
		out += output;
		i = (k < end) ? k + 1 : k;
	}

	// Dispatches the `$X` form starting at body[i]; requires i + 1 < body.size().
	void Expander::expandHeredocDollar(const std::string& body, std::size_t& i,
			std::string& out) {
		const char next = body[i + 1];
		if (next == '{') {
			expandHeredocParamBraces(body, i, out);
			return;
		}

		if (next == '(') {
			if (i + 2 < body.size() && body[i + 2] == '(') expandHeredocArith(body, i, out);
			else expandHeredocCmdSubst(body, i, out);
			return;
		}

		if (next == '[') {
			expandHeredocArithBracket(body, i, out);
			return;
		}

		if (isNameStart(next) || isDigit(next) || isSpecialParam1(next)) {
			expandHeredocSimpleParam(body, i, out);
			return;
		}

		out.push_back('$');
		++i;
	}

	std::string Expander::expandHeredoc(const std::string& body, bool quoted) {
		if (quoted) return body;

		std::string out;
		const std::size_t end = body.size();
		std::size_t i = 0;
		while (i < end && !aborting()) {
			const char c = body[i];
			if (c == '\\' && i + 1 < end) {
				expandHeredocBackslash(body, i, out);
				continue;
			}

			if (c == '$' && i + 1 < end) {
				expandHeredocDollar(body, i, out);
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

	static void copySingleQuotedRun(const std::string& body, std::size_t& i,
			std::string& out) {
		++i;
		while (i < body.size() && body[i] != '\'') out.push_back(body[i++]);
		if (i < body.size()) ++i;
	}

	// Expands `$`/backtick interpolation exactly like expandHeredoc, but
	// also performs quote removal: unescaped `'...'` runs are copied
	// verbatim with the quotes dropped and no further expansion inside,
	// and bare `"`/closing-`"` characters are dropped without ending
	// expansion (their content already expands the same way a heredoc
	// body does). Used for the argument/pattern words embedded in
	// `${name:-word}`, `${name/pat/rep}`, `${name#pat}`, and friends,
	// which — unlike a heredoc body — are ordinary shell words.
	std::string Expander::expandParamWordArg(const std::string& body) {
		std::string out;
		const std::size_t end = body.size();
		std::size_t i = 0;
		while (i < end && !aborting()) {
			const char c = body[i];
			if (c == '\'') {
				copySingleQuotedRun(body, i, out);
				continue;
			}

			if (c == '"') {
				++i;
				continue;
			}

			if (c == '\\' && i + 1 < end) {
				expandHeredocBackslash(body, i, out);
				continue;
			}

			if (c == '$' && i + 1 < end) {
				expandHeredocDollar(body, i, out);
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
		if (sub_ != nullptr) return sub_->run(body);
		return {};
	}

	static void ansicHexEscape(const std::string& body, std::size_t& i, std::string& out) {
		int value = 0;
		int digits = 0;
		while (digits < kAnsicHexDigits && i < body.size()
				&& std::isxdigit(static_cast<unsigned char>(body[i]))) {
			value = value * 16 + hexDigitValue(body[i]);
			++i;
			++digits;
		}

		out.push_back(static_cast<char>(value));
	}

	static void ansicOctalEscape(const std::string& body, std::size_t& i, std::string& out) {
		int value = 0;
		int digits = 0;
		while (digits < kAnsicOctalDigits && i < body.size()
				&& body[i] >= '0' && body[i] <= '7') {
			value = value * 8 + (body[i] - '0');
			++i;
			++digits;
		}

		out.push_back(static_cast<char>(value));
	}

	static char simpleAnsicEscape(char escape) {
		switch (escape) {
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

	// `\cX` is the control character of X; a trailing `\c` with nothing
	// after it is kept as a literal backslash.
	static void ansicControlEscape(const std::string& body, std::size_t& i, std::string& out) {
		if (i + 2 < body.size()) {
			out.push_back(static_cast<char>(body[i + 2] & 0x1f));
			i += 3;
			return;
		}

		out.push_back('\\');
		++i;
	}

	std::string Expander::interpretAnsiC(const std::string& body) {
		std::string out;
		const std::size_t end = body.size();
		std::size_t i = 0;
		while (i < end) {
			const char c = body[i];
			if (c != '\\' || i + 1 >= end) {
				out.push_back(c);
				++i;
				continue;
			}

			const char escape = body[i + 1];
			const char simple = simpleAnsicEscape(escape);
			if (simple != 0) {
				out.push_back(simple);
				i += 2;
				continue;
			}

			if (escape == 'x') {
				i += 2;
				ansicHexEscape(body, i, out);
				continue;
			}

			if (escape >= '0' && escape <= '7') {
				++i;
				ansicOctalEscape(body, i, out);
				continue;
			}

			if (escape == 'c') {
				ansicControlEscape(body, i, out);
				continue;
			}

			out.push_back('\\');
			out.push_back(escape);
			i += 2;
		}

		return out;
	}

	static std::size_t tildePrefixEnd(const std::string& text) {
		std::size_t end = 1;
		while (end < text.size() && text[end] != '/' && text[end] != ':') ++end;
		return end;
	}

	// Empty when the prefix names nothing this shell expands: `~user`
	// is not implemented, and an unset HOME leaves the word alone.
	static std::string tildeReplacement(const Environment& env, const std::string& name) {
		if (name.empty()) return env.get("HOME");
		if (name == "+")   return env.get("PWD");
		if (name == "-")   return env.get("OLDPWD");
		return {};
	}

	Word Expander::applyTildeExpansion(const Word& w) {
		Word out = w;
		if (out.segments.empty()) return out;

		WordSegment& first = out.segments[0];
		if (first.kind != WordSegment::Kind::Literal) return out;
		if (first.text.empty() || first.text[0] != '~') return out;

		const std::size_t end = tildePrefixEnd(first.text);
		const std::string replacement = tildeReplacement(env_, first.text.substr(1, end - 1));
		if (replacement.empty()) return out;

		first.text = replacement + first.text.substr(end);
		return out;
	}

	bool Expander::isSpecialParam1(char c) const {
		return c == '?' || c == '$' || c == '!' || c == '#'
			|| c == '@' || c == '*' || c == '-' || c == '_';
	}

	std::string Expander::joinPositionals(char form) const {
		return joinWith(env_.positional(), starSeparator(form == '*'));
	}

	std::string Expander::lookupPositional(std::size_t index) const {
		if (index == 0) return env_.shellName();
		if (index <= env_.positional().size()) return env_.positional()[index - 1];
		return {};
	}

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
			if (c < '1' || c > '9') return false;

			out = lookupPositional(static_cast<std::size_t>(c - '0'));
			return true;
		}
	}

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

	std::string Expander::lookupParam(const std::string& name, bool suppress_nounset) {
		if (name.empty()) return {};

		if (name.size() == 1) {
			std::string out;
			if (lookupSpecialChar(name[0], out)) return out;
		}

		if (scanDigitsEnd(name, 0) == name.size()) {
			unsigned long index = 0;
			if (!parseUL(name, index)) index = 0;
			return lookupPositional(static_cast<std::size_t>(index));
		}

		std::string dynamic;
		if (lookupDynamicSpecial(name, dynamic)) return dynamic;

		if (env_.has(name)) return env_.get(name);
		if (env_.nounset() && !suppress_nounset) fail(name + ": unbound variable");
		return {};
	}

	std::string Expander::starSeparator(bool star_join_ifs) const {
		if (!star_join_ifs) return " ";

		const std::string ifs = env_.get("IFS");
		return ifs.empty() ? std::string() : std::string(1, ifs[0]);
	}

	std::string Expander::joinWholeArray(const std::string& name, const std::string& sep) {
		if (const auto* indexed = env_.getIndexedArray(name); indexed != nullptr) {
			return joinMapValues(*indexed, sep);
		}

		if (const auto* assoc = env_.getAssocArray(name); assoc != nullptr) {
			return joinMapValues(*assoc, sep);
		}

		return lookupParam(name);
	}

	// `@` and `*` are subscripts in their own right; anything else may
	// carry `$` references that need expanding before evaluation.
	std::string Expander::resolveSubscript(const std::string& subscript) {
		if (subscript == "@" || subscript == "*") return subscript;
		if (subscript.find('$') == std::string::npos) return subscript;
		return expandHeredoc(subscript, false);
	}

	// A negative subscript counts back from the highest index in use.
	static std::string indexedArrayElement(Expander& expander,
			const Environment::IndexedArray& items, const std::string& subscript) {
		long long index = 0;
		if (!expander.tryEvalArith(subscript, index)) return {};
		if (index < 0 && !items.empty()) index = items.rbegin()->first + index + 1;

		const auto it = items.find(index);
		return it == items.end() ? std::string() : it->second;
	}

	std::string Expander::lookupSubscripted(const std::string& name,
			const std::string& subscript_in, bool star_join_ifs) {
		const std::string subscript = resolveSubscript(subscript_in);
		const bool whole_array = (subscript == "@" || subscript == "*");
		if (whole_array) {
			const std::string sep = (subscript == "*") ? starSeparator(star_join_ifs)
				: std::string(" ");
			return joinWholeArray(name, sep);
		}

		if (const auto* indexed = env_.getIndexedArray(name); indexed != nullptr) {
			return indexedArrayElement(*this, *indexed, subscript);
		}

		if (const auto* assoc = env_.getAssocArray(name); assoc != nullptr) {
			const auto it = assoc->find(subscript);
			return it == assoc->end() ? std::string() : it->second;
		}

		long long index = 0;
		if (!tryEvalArith(subscript, index)) return {};
		return index == 0 ? lookupParam(name) : std::string();
	}

	std::size_t Expander::arrayLength(const std::string& name) const {
		if (const auto* indexed = env_.getIndexedArray(name); indexed != nullptr) {
			return indexed->size();
		}

		if (const auto* assoc = env_.getAssocArray(name); assoc != nullptr) return assoc->size();
		return env_.has(name) ? 1u : 0u;
	}

	std::vector<std::string> Expander::arrayKeys(const std::string& name) const {
		std::vector<std::string> out;
		if (const auto* indexed = env_.getIndexedArray(name); indexed != nullptr) {
			for (const auto& entry : *indexed) out.push_back(std::to_string(entry.first));
			return out;
		}

		if (const auto* assoc = env_.getAssocArray(name); assoc != nullptr) {
			for (const auto& entry : *assoc) out.push_back(entry.first);
			return out;
		}

		if (env_.has(name)) out.push_back("0");
		return out;
	}

	std::vector<std::string> Expander::arrayValues(const std::string& name) const {
		std::vector<std::string> out;
		if (const auto* indexed = env_.getIndexedArray(name); indexed != nullptr) {
			for (const auto& entry : *indexed) out.push_back(entry.second);
			return out;
		}

		if (const auto* assoc = env_.getAssocArray(name); assoc != nullptr) {
			for (const auto& entry : *assoc) out.push_back(entry.second);
			return out;
		}

		if (env_.has(name)) out.push_back(env_.get(name));
		return out;
	}

	// Splits `name[subscript]`; false when `text` is not of that shape.
	static bool splitSubscriptForm(const std::string& text, std::string& name,
			std::string& subscript) {
		const std::size_t open = text.find('[');
		if (open == std::string::npos || text.back() != ']') return false;

		name      = text.substr(0, open);
		subscript = text.substr(open + 1, text.size() - open - 2);
		return true;
	}

	std::string Expander::expandParamLengthForm(const std::string& body) {
		const std::string rest = body.substr(1);
		if (rest == "@" || rest == "*") return std::to_string(env_.positional().size());

		std::string name;
		std::string subscript;
		if (!splitSubscriptForm(rest, name, subscript)) {
			return std::to_string(lookupParam(rest).size());
		}

		if (subscript == "@" || subscript == "*") return std::to_string(arrayLength(name));
		return std::to_string(lookupSubscripted(name, subscript, false).size());
	}

	std::string Expander::expandParamIndicesForm(const std::string& body) {
		if (body.size() <= 4) return {};

		std::string name;
		std::string subscript;
		if (!splitSubscriptForm(body.substr(1), name, subscript)) return {};
		if (subscript != "@" && subscript != "*") return {};

		return joinWith(arrayKeys(name), " ");
	}

	// Length of the parameter name at the start of `body`, or npos when
	// it does not begin with one.
	static std::size_t scanParamName(const std::string& body, std::string& out_name,
			const Expander& expander) {
		if (body.empty()) return std::string::npos;

		std::size_t end = 0;
		if (isNameStart(body[0])) end = scanIdentifierEnd(body, 0);
		else if (isDigit(body[0])) end = scanDigitsEnd(body, 0);
		else if (expander.isSpecialParam1(body[0])) end = 1;
		else return std::string::npos;

		out_name = body.substr(0, end);
		return end;
	}

	static bool isDefaultOp(char op) {
		return op == '-' || op == '+' || op == '=' || op == '?';
	}

	std::string Expander::evalParamDefaultOp(char op, bool colon, const std::string& name,
			const std::string& cur, const std::string& arg) {
		const bool unset = !env_.has(name) && !isDigit(name[0]) && !isSpecialParam1(name[0]);
		const bool empty_or_unset = colon ? (unset || cur.empty()) : unset;

		switch (op) {
		case '-':
			return empty_or_unset ? expandParamWordArg(arg) : cur;
		case '+':
			return empty_or_unset ? std::string() : expandParamWordArg(arg);
		case '=': {
			if (!empty_or_unset) return cur;

			const std::string value = expandParamWordArg(arg);
			env_.set(name, value);
			return value;
		}
		case '?': {
			if (!empty_or_unset) return cur;

			std::string message = arg.empty()
				? (name + ": parameter null or not set")
				: expandParamWordArg(arg);
			fail(std::move(message));
			return {};
		}
		default:
			return cur;
		}
	}

	// `OFFSET:LENGTH` split at the first colon outside parentheses, so a
	// ternary inside the offset expression keeps its own colon.
	static void splitSliceArgs(const std::string& args, std::string& offset_text,
			std::string& length_text) {
		int depth = 0;
		for (std::size_t k = 0; k < args.size(); ++k) {
			const char c = args[k];
			if (c == '(') ++depth;
			else if (c == ')' && depth > 0) --depth;
			else if (c == ':' && depth == 0) {
				offset_text = args.substr(0, k);
				length_text = args.substr(k + 1);
				return;
			}
		}

		offset_text = args;
		length_text.clear();
	}

	std::string Expander::evalArraySlice(const std::string& name, const std::string& args) {
		const std::vector<std::string> elems = arrayValues(name);
		std::string offset_text;
		std::string length_text;
		splitSliceArgs(args, offset_text, length_text);

		const long long count = static_cast<long long>(elems.size());
		long long offset = 0;
		if (!tryEvalArith(offset_text, offset)) offset = 0;
		if (offset < 0) offset = std::max<long long>(0, count + offset);
		if (offset > count) offset = count;

		long long take = count - offset;
		if (!length_text.empty()) {
			if (!tryEvalArith(length_text, take)) take = 0;
			if (take < 0) take = std::max<long long>(0, count + take - offset);
			if (take > count - offset) take = count - offset;
		}

		std::string out;
		for (long long k = 0; k < take; ++k) {
			if (k != 0) out.push_back(' ');
			out += elems[static_cast<std::size_t>(offset + k)];
		}

		return out;
	}

	std::string Expander::evalParamReplace(const std::string& cur, const std::string& args,
			bool all) {
		std::string body = args;
		const bool anchor_start = !body.empty() && body[0] == '#';
		const bool anchor_end   = !anchor_start && !body.empty() && body[0] == '%';
		if (anchor_start || anchor_end) body.erase(0, 1);

		const std::size_t slash = body.find('/');
		const std::string pat = (slash == std::string::npos) ? body : body.substr(0, slash);
		const std::string rep = (slash == std::string::npos) ? std::string()
			: body.substr(slash + 1);
		return replacePattern(cur, expandHeredoc(pat, false), expandHeredoc(rep, false),
			all, anchor_start, anchor_end);
	}

	// `##`, `%%`, `//`, `^^`, `,,` double the operator to mean "greedy"
	// or "all"; the other operators never double.
	static bool isDoublingOp(char op) {
		return op == '#' || op == '%' || op == '/' || op == '^' || op == ',';
	}

	std::string Expander::expandParamApplyOp(char op, bool colon, std::size_t op_pos,
			ParamOpCtx& ctx) {
		const bool doubled = isDoublingOp(op)
			&& op_pos + 1 < ctx.body.size() && ctx.body[op_pos + 1] == op;
		const std::string argument = ctx.body.substr(doubled ? op_pos + 2 : op_pos + 1);

		switch (op) {
		case '-':
		case '+':
		case '=':
		case '?':
			return evalParamDefaultOp(op, colon, ctx.name, ctx.named_value, argument);
		case ':':
			if (ctx.has_subscript && (ctx.subscript == "@" || ctx.subscript == "*")) {
				return evalArraySlice(ctx.name, argument);
			}
			return substringExpand(ctx.named_value, argument);
		case '#':
			return stripPrefix(ctx.named_value, expandHeredoc(argument, false), doubled);
		case '%':
			return stripSuffix(ctx.named_value, expandHeredoc(argument, false), doubled);
		case '/':
			return evalParamReplace(ctx.named_value, argument, doubled);
		case '^':
		case ',':
			return applyCaseConv(ctx.named_value, op, doubled, expandHeredoc(argument, false));
		default:
			return ctx.named_value;
		}
	}

	static char convertCase(char c, char op) {
		if (op == '^') return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}

	std::string Expander::applyCaseConv(const std::string& val, char op, bool all,
			const std::string& pat) {
		std::string out = val;
		for (std::size_t k = 0; k < out.size(); ++k) {
			if (!all && k > 0) break;

			const char c = out[k];
			if (!pat.empty() && !fnmatchFull(pat, std::string(1, c))) continue;
			out[k] = convertCase(c, op);
		}

		return out;
	}

	// `${!name}` indirection: resolve `name`'s value, then look up a
	// parameter by that value. Returns false when `body` isn't a plain
	// `!name` form (e.g. it's the `${!name[@]}` indices form instead).
	bool Expander::tryExpandIndirectParam(const std::string& body, std::string& out) {
		const std::string ref_name = body.substr(1);
		if (!isPlainName(ref_name)) return false;

		const std::string target = lookupParam(ref_name);
		out.clear();
		if (!aborting() && !target.empty()) out = lookupParam(target);
		return true;
	}

	// Peeks past `body[i]`'s operator (skipping a leading `:` for the
	// `-`/`+`/`=`/`?` family) to tell whether that operator already
	// handles an unset parameter itself, so the caller's lookup can
	// skip `set -u`'s unbound-variable check on its behalf.
	static bool paramOpHandlesUnset(const std::string& body, std::size_t i) {
		char op = (i < body.size()) ? body[i] : '\0';
		if (op == ':' && i + 1 < body.size() && isDefaultOp(body[i + 1])) op = body[i + 1];
		return isDefaultOp(op);
	}

	// Takes a `[subscript]` at body[i], advancing i past the `]`.
	static bool takeSubscript(const std::string& body, std::size_t& i, std::string& subscript) {
		if (i >= body.size() || body[i] != '[') return false;

		const std::size_t close = body.find(']', i + 1);
		if (close == std::string::npos) return false;

		subscript = body.substr(i + 1, close - i - 1);
		i = close + 1;
		return true;
	}

	std::string Expander::expandParam(const std::string& body, bool /*quoted_ctx*/) {
		if (body.empty()) return {};

		if (body[0] == '#' && body.size() > 1) return expandParamLengthForm(body);
		if (body[0] == '!' && body.size() > 1) {
			if (body.size() > 4) {
				const std::string indices = expandParamIndicesForm(body);
				if (!indices.empty()) return indices;
			}

			std::string indirect;
			if (tryExpandIndirectParam(body, indirect)) return indirect;
		}

		std::string name;
		std::size_t i = scanParamName(body, name, *this);
		if (i == std::string::npos) return {};

		std::string subscript;
		const bool has_subscript = takeSubscript(body, i, subscript);

		// `${name:-…}`, `${name-…}`, `${name:=…}`, `${name:?…}` (and their
		// colon-less siblings) handle an unset `name` themselves, so the
		// lookup here must not raise `set -u`'s unbound-variable error.
		const std::string named = has_subscript
			? lookupSubscripted(name, subscript, true)
			: lookupParam(name, paramOpHandlesUnset(body, i));
		if (i >= body.size()) return named;

		char op = body[i];
		bool colon = false;
		if (op == ':' && i + 1 < body.size() && isDefaultOp(body[i + 1])) {
			colon = true;
			++i;
			op = body[i];
		}

		ParamOpCtx ctx{ name, subscript, has_subscript, body, named };
		return expandParamApplyOp(op, colon, i, ctx);
	}

	std::string Expander::substringExpand(const std::string& val, const std::string& args) {
		std::string offset_text;
		std::string length_text;
		splitSliceArgs(args, offset_text, length_text);

		const long long size = static_cast<long long>(val.size());
		long long offset = evalArith(offset_text);
		if (offset < 0) offset = std::max<long long>(0, size + offset);
		if (offset > size) offset = size;
		if (length_text.empty()) return val.substr(static_cast<std::size_t>(offset));

		long long length = evalArith(length_text);
		if (length < 0) {
			long long end = size + length;
			if (end < offset) end = offset;
			return val.substr(static_cast<std::size_t>(offset),
				static_cast<std::size_t>(end - offset));
		}

		if (offset + length > size) length = size - offset;
		return val.substr(static_cast<std::size_t>(offset), static_cast<std::size_t>(length));
	}

	// Length of the longest span at `start` matching `pat`, or npos.
	static std::size_t longestMatchAt(const std::string& val, std::size_t start,
			const std::string& pat) {
		for (std::size_t length = val.size() - start;; --length) {
			if (fnmatchRange(pat, val, start, length)) return length;
			if (length == 0) return std::string::npos;
		}
	}

	static std::size_t shortestPrefixMatch(const std::string& val, const std::string& pat) {
		for (std::size_t length = 0; length <= val.size(); ++length) {
			if (fnmatchRange(pat, val, 0, length)) return length;
		}

		return std::string::npos;
	}

	// Start offset of the longest suffix matching `pat`, or npos.
	static std::size_t longestSuffixMatch(const std::string& val, const std::string& pat) {
		for (std::size_t start = 0; start <= val.size(); ++start) {
			if (fnmatchRange(pat, val, start, val.size() - start)) return start;
		}

		return std::string::npos;
	}

	static std::size_t shortestSuffixMatch(const std::string& val, const std::string& pat) {
		for (std::size_t start = val.size();; --start) {
			if (fnmatchRange(pat, val, start, val.size() - start)) return start;
			if (start == 0) return std::string::npos;
		}
	}

	std::string Expander::stripPrefix(const std::string& val, const std::string& pat,
			bool greedy) {
		if (pat.empty()) return val;

		const std::size_t length = greedy ? longestMatchAt(val, 0, pat)
			: shortestPrefixMatch(val, pat);
		if (length == std::string::npos) return val;
		return val.substr(length);
	}

	std::string Expander::stripSuffix(const std::string& val, const std::string& pat,
			bool greedy) {
		if (pat.empty()) return val;

		const std::size_t start = greedy ? longestSuffixMatch(val, pat)
			: shortestSuffixMatch(val, pat);
		if (start == std::string::npos) return val;
		return val.substr(0, start);
	}

	static std::string replaceAnchoredPrefix(const std::string& val, const std::string& pat,
			const std::string& rep) {
		const std::size_t length = longestMatchAt(val, 0, pat);
		if (length == std::string::npos) return val;
		return rep + val.substr(length);
	}

	static std::string replaceAnchoredSuffix(const std::string& val, const std::string& pat,
			const std::string& rep) {
		const std::size_t start = longestSuffixMatch(val, pat);
		if (start == std::string::npos) return val;
		return val.substr(0, start) + rep;
	}

	static std::string replaceUnanchored(const std::string& val, const std::string& pat,
			const std::string& rep, bool all) {
		std::string out;
		std::size_t i = 0;
		while (i <= val.size()) {
			const std::size_t best = longestMatchAt(val, i, pat);
			if (best == std::string::npos) {
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
				i += best;
			}

			if (!all) {
				out += val.substr(i);
				return out;
			}
		}

		return out;
	}

	std::string Expander::replacePattern(const std::string& val, const std::string& pat,
			const std::string& rep, bool all, bool anchor_start, bool anchor_end) {
		if (pat.empty())  return val;
		if (anchor_start) return replaceAnchoredPrefix(val, pat, rep);
		if (anchor_end)   return replaceAnchoredSuffix(val, pat, rep);
		return replaceUnanchored(val, pat, rep, all);
	}

	static void pushText(Expander::Tagged& out, const std::string& text, std::uint8_t mark) {
		for (char c : text) out.push(c, mark);
	}

	static bool isQuotingSegment(WordSegment::Kind kind) {
		return kind == WordSegment::Kind::SingleQuoted
			|| kind == WordSegment::Kind::DoubleQuoted
			|| kind == WordSegment::Kind::Escaped
			|| kind == WordSegment::Kind::DollarSingle;
	}

	Expander::Tagged Expander::renderWord(const Word& w) {
		Tagged out;
		out.reserve(w.raw.size());
		for (const WordSegment& segment : w.segments) {
			if (isQuotingSegment(segment.kind)) out.had_quote = true;
		}

		for (const WordSegment& segment : w.segments) {
			if (aborting()) break;
			renderSegment(segment, out, /*inside_dq=*/false);
		}

		return out;
	}

	// The array name of a `name[@]`, `name[*]`, `!name[@]` or `!name[*]`
	// body — the forms that expand to separate fields — else empty.
	static std::string fieldAwareArrayName(const std::string& body) {
		if (body.size() < 4) return {};
		if (body[0] == '#') return {};

		const std::size_t start = (body[0] == '!') ? 1 : 0;
		const std::size_t open = body.find('[', start);
		if (open == std::string::npos) return {};
		if (open + 2 >= body.size()) return {};
		if (body[open + 1] != '@' && body[open + 1] != '*') return {};
		if (body[open + 2] != ']') return {};
		if (open + 3 != body.size()) return {};
		return body.substr(start, open - start);
	}

	// `"${a[*]}"` joins with the first IFS character inside the quotes;
	// every other form separates elements with an unquoted space so
	// word splitting can cut between them.
	void Expander::renderArrayFields(const std::vector<std::string>& elems, bool star,
			bool inside_dq, Tagged& out) {
		const std::uint8_t mark = inside_dq ? F_QUOTED : 0;
		const bool join_with_ifs = star && inside_dq;
		const std::string sep = join_with_ifs ? starSeparator(true) : std::string(" ");
		const std::uint8_t sep_mark = join_with_ifs ? mark : 0;

		for (std::size_t k = 0; k < elems.size(); ++k) {
			if (k != 0) pushText(out, sep, sep_mark);
			pushText(out, elems[k], mark);
		}
	}

	void Expander::renderParamExpSegment(const WordSegment& s, Tagged& out, bool inside_dq) {
		const std::uint8_t mark = inside_dq ? F_QUOTED : 0;
		const std::string array_name = fieldAwareArrayName(s.text);
		if (array_name.empty()) {
			pushText(out, expandParam(s.text, inside_dq), mark);
			return;
		}

		const bool indices = s.text[0] == '!';
		const bool star    = s.text.find('*') != std::string::npos;
		const std::vector<std::string> elems = indices ? arrayKeys(array_name)
			: arrayValues(array_name);
		renderArrayFields(elems, star, inside_dq, out);
		if (inside_dq) out.had_quote = true;
	}

#ifdef _WIN32
	static bool createTempFile(std::string& out_path) {
		char dir[MAX_PATH];
		const DWORD dir_length = ::GetTempPathA(MAX_PATH, dir);
		if (dir_length == 0 || dir_length > MAX_PATH) return false;

		char path[MAX_PATH];
		if (::GetTempFileNameA(dir, "wbsh", 0, path) == 0) return false;

		out_path = path;
		return true;
	}

	static void writeFileBytes(const std::string& path, const std::string& bytes) {
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file.write(bytes.data(), bytes.size());
	}
#endif /* _WIN32 */

	// `<(cmd)` is served from a temp file holding cmd's output; the
	// executor deletes it once the command that used it has finished.
	void Expander::renderProcSubstSegment(const WordSegment& s, Tagged& out) {
		if (s.proc_dir != '<') {
			std::fprintf(stderr, "wbsh: process substitution >(...) not supported\n");
			return;
		}
#ifdef _WIN32
		std::string path;
		if (!createTempFile(path)) return;

		const std::string body = (sub_ != nullptr) ? sub_->runRaw(s.text) : std::string();
		writeFileBytes(path, body);
		pending_temp_files_.push_back(path);

		pushText(out, path_conv_.toPosix(path), F_QUOTED);
		out.had_quote = true;
#endif /* _WIN32 */
	}

	void Expander::renderDollarAt(Tagged& out, bool inside_dq) {
		const std::uint8_t mark = inside_dq ? F_QUOTED : 0;
		const std::vector<std::string>& positional = env_.positional();
		for (std::size_t i = 0; i < positional.size(); ++i) {
			if (i != 0) out.push(' ', 0);
			pushText(out, positional[i], mark);
		}
	}

	void Expander::renderSegment(const WordSegment& s, Tagged& out, bool inside_dq) {
		using K = WordSegment::Kind;
		const std::uint8_t mark = inside_dq ? F_QUOTED : 0;
		switch (s.kind) {
		case K::Literal:
			pushText(out, s.text, mark);
			break;
		case K::Escaped:
		case K::SingleQuoted:
			pushText(out, s.text, F_QUOTED);
			break;
		case K::DoubleQuoted:
			for (const WordSegment& nested : s.nested) {
				if (aborting()) break;
				renderSegment(nested, out, /*inside_dq=*/true);
			}
			break;
		case K::DollarSingle:
			pushText(out, interpretAnsiC(s.text), F_QUOTED);
			break;
		case K::SimpleVar:
			if (s.text == "@") renderDollarAt(out, inside_dq);
			else pushText(out, lookupParam(s.text), mark);
			break;
		case K::ParamExp:
			renderParamExpSegment(s, out, inside_dq);
			break;
		case K::CmdSubst: {
			std::string output = runCmdSubst(s.text);
			stripTrailingNewlines(output);
			pushText(out, output, mark);
			break;
		}
		case K::ArithExp:
			pushText(out, std::to_string(evalArith(s.text)), mark);
			break;
		case K::ProcSubst:
			renderProcSubstSegment(s, out);
			break;
		}
	}

	static bool isIfsChar(const std::string& ifs, char c) {
		return ifs.find(c) != std::string::npos;
	}

	static bool isIfsWhitespace(const std::string& ifs, char c) {
		if (c != ' ' && c != '\t' && c != '\n') return false;
		return isIfsChar(ifs, c);
	}

	static bool quotedAt(const Expander::Tagged& t, std::size_t i) {
		return (t.flags[i] & Expander::F_QUOTED) != 0;
	}

	static Expander::Tagged takeField(const Expander::Tagged& t, const std::string& ifs,
			std::size_t& i) {
		Expander::Tagged field;
		while (i < t.size()) {
			const char c = t.text[i];
			const bool quoted = quotedAt(t, i);
			if (!quoted && isIfsChar(ifs, c)) break;

			field.push(c, t.flags[i]);
			field.had_quote = field.had_quote || quoted;
			++i;
		}

		return field;
	}

	// A run of IFS whitespace plus at most one non-whitespace IFS
	// character ends a field; a second non-whitespace one delimits an
	// empty field, so it is left for the next round.
	static void skipFieldSeparator(const Expander::Tagged& t, const std::string& ifs,
			std::size_t& i) {
		bool saw_nonws = false;
		while (i < t.size()) {
			const char c = t.text[i];
			if (quotedAt(t, i) || !isIfsChar(ifs, c)) break;
			if (!isIfsWhitespace(ifs, c)) {
				if (saw_nonws) break;
				saw_nonws = true;
			}

			++i;
		}
	}

	std::vector<Expander::Tagged> Expander::splitWords(const Tagged& t) {
		const std::string ifs = env_.get("IFS");
		std::vector<Tagged> out;
		if (ifs.empty()) {
			if (t.size() > 0 || t.had_quote) out.push_back(t);
			return out;
		}

		std::size_t i = 0;
		while (i < t.size() && !quotedAt(t, i) && isIfsWhitespace(ifs, t.text[i])) ++i;
		while (i < t.size()) {
			out.push_back(takeField(t, ifs, i));
			skipFieldSeparator(t, ifs, i);
		}

		return out;
	}

	static bool isGlobMeta(char c) {
		return c == '*' || c == '?' || c == '[';
	}

	namespace glob_detail {

		// One `/`-separated path component with per-char quoting so a
		// quoted `*` stays literal.
		struct GlobComp {
			std::string text;
			std::vector<std::uint8_t> quoted;

			bool hasMeta() const {
				for (std::size_t i = 0; i < text.size(); ++i) {
					if (quoted[i] == 0 && isGlobMeta(text[i])) return true;
				}

				return false;
			}

			std::string asPattern() const {
				std::string pattern;
				for (std::size_t i = 0; i < text.size(); ++i) {
					const char c = text[i];
					if (quoted[i] != 0 && (isGlobMeta(c) || c == '\\')) pattern.push_back('\\');
					pattern.push_back(c);
				}

				return pattern;
			}

			std::string asLiteral() const {
				return text;
			}
		};

		struct GlobMatchOptions {
			bool dotglob    = false;
			bool nocaseglob = false;
		};

		static std::string joinDir(const std::string& dir, const std::string& name) {
			if (dir == "." || dir.empty()) return name;
			if (dir == "/") return "/" + name;
			if (dir.back() == '/') return dir + name;
			return dir + "/" + name;
		}

	}  // namespace glob_detail

	static bool taggedHasUnquotedGlobMeta(const Expander::Tagged& t) {
		for (std::size_t i = 0; i < t.size(); ++i) {
			if (!quotedAt(t, i) && isGlobMeta(t.text[i])) return true;
		}

		return false;
	}

	static std::vector<glob_detail::GlobComp> splitTaggedIntoGlobComps(
			const Expander::Tagged& t, bool& absolute) {
		std::vector<glob_detail::GlobComp> comps;
		glob_detail::GlobComp cur;
		bool first = true;
		absolute = false;
		for (std::size_t i = 0; i < t.size(); ++i) {
			const char c = t.text[i];
			const bool quoted = quotedAt(t, i);
			if (!quoted && c == '/') {
				if (first) absolute = true;
				if (!cur.text.empty()) {
					comps.push_back(std::move(cur));
					cur = {};
				}

				first = false;
				continue;
			}

			cur.text.push_back(c);
			cur.quoted.push_back(quoted ? 1 : 0);
			first = false;
		}

		if (!cur.text.empty()) comps.push_back(std::move(cur));
		return comps;
	}

	static void collectGlobstarUnder(const std::string& dir, bool last, const PathConv& pc,
			std::set<std::string>& found) {
		namespace fs = std::filesystem;
		const std::string posix_dir = dir.empty() ? "." : dir;
		const fs::path list_dir = utf8ToPath(pc.toWin32(posix_dir));

		std::error_code ec;
		fs::recursive_directory_iterator walker(list_dir,
			fs::directory_options::skip_permission_denied, ec);
		if (ec) return;

		for (; walker != fs::recursive_directory_iterator(); walker.increment(ec)) {
			if (ec) break;

			const std::string name = pathToUtf8(walker->path().filename());
			// Skip dotfiles AND don't descend into them — bash behaviour.
			if (!name.empty() && name[0] == '.') {
				walker.disable_recursion_pending();
				continue;
			}

			std::error_code entry_ec;
			std::string rel = pathToUtf8(fs::relative(walker->path(), list_dir, entry_ec));
			std::replace(rel.begin(), rel.end(), '\\', '/');
			if (last || walker->is_directory(entry_ec)) {
				found.insert(glob_detail::joinDir(dir, rel));
			}
		}
	}

	// `**` matches the directory itself plus everything below it; only
	// as the last component does it also yield files.
	static std::vector<std::string> expandGlobstarStep(const std::vector<std::string>& current,
			bool last, const PathConv& pc) {
		std::set<std::string> found;
		for (const std::string& dir : current) {
			found.insert(dir);
			collectGlobstarUnder(dir, last, pc, found);
		}

		std::vector<std::string> out(found.begin(), found.end());
		std::sort(out.begin(), out.end());
		return out;
	}

	static std::string lowerAscii(std::string text) {
		for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return text;
	}

	static bool globNameMatches(const std::string& pat, const std::string& name, bool nocaseglob) {
		if (!nocaseglob) return fnmatchFull(pat, name);
		return fnmatchFull(lowerAscii(pat), lowerAscii(name));
	}

	static std::vector<std::string> matchingEntries(const std::string& dir, const std::string& pat,
			const PathConv& pc, const glob_detail::GlobMatchOptions& options) {
		namespace fs = std::filesystem;
		const std::string posix_dir = dir.empty() ? "." : dir;
		const fs::path list_dir = utf8ToPath(pc.toWin32(posix_dir));

		std::error_code ec;
		fs::directory_iterator it(list_dir, ec);
		if (ec) return {};

		const bool pat_starts_dot = !pat.empty() && pat[0] == '.';
		std::vector<std::string> matches;
		for (const fs::directory_entry& entry : it) {
			const std::string name = pathToUtf8(entry.path().filename());
			if (name.empty()) continue;
			if (name[0] == '.' && !pat_starts_dot && !options.dotglob) continue;
			if (globNameMatches(pat, name, options.nocaseglob)) matches.push_back(name);
		}

		std::sort(matches.begin(), matches.end());
		return matches;
	}

	static std::vector<std::string> expandGlobOneStep(const std::vector<std::string>& current,
			const glob_detail::GlobComp& comp, const PathConv& pc,
			const glob_detail::GlobMatchOptions& options) {
		std::vector<std::string> next;
		if (!comp.hasMeta()) {
			for (const std::string& dir : current) {
				next.push_back(glob_detail::joinDir(dir, comp.asLiteral()));
			}

			return next;
		}

		const std::string pat = comp.asPattern();
		for (const std::string& dir : current) {
			for (const std::string& name : matchingEntries(dir, pat, pc, options)) {
				next.push_back(glob_detail::joinDir(dir, name));
			}
		}

		return next;
	}

	std::vector<std::string> Expander::globExpand(const Tagged& t) {
		if (env_.noglob()) return { quoteRemove(t) };
		if (!taggedHasUnquotedGlobMeta(t)) return { quoteRemove(t) };

		bool absolute = false;
		const std::vector<glob_detail::GlobComp> comps = splitTaggedIntoGlobComps(t, absolute);
		if (comps.empty()) return { quoteRemove(t) };

		std::vector<std::string> current;
		current.push_back(absolute ? std::string("/") : std::string("."));

		glob_detail::GlobMatchOptions options;
		options.dotglob    = env_.dotglob();
		options.nocaseglob = env_.nocaseglob();
		const bool globstar = env_.globstar();

		for (std::size_t ci = 0; ci < comps.size(); ++ci) {
			const glob_detail::GlobComp& comp = comps[ci];
			if (globstar && comp.text == "**") {
				current = expandGlobstarStep(current, /*last=*/ci + 1 == comps.size(), path_conv_);
			} else {
				current = expandGlobOneStep(current, comp, path_conv_, options);
			}
		}

		if (current.empty()) {
			if (env_.nullglob()) return {};
			return { quoteRemove(t) };
		}

		return current;
	}

	std::string Expander::quoteRemove(const Tagged& t) {
		return t.text;
	}

	static bool isInteger(const std::string& text) {
		if (text.empty()) return false;

		const std::size_t start = (text[0] == '-' || text[0] == '+') ? 1 : 0;
		if (start == text.size()) return false;
		return scanDigitsEnd(text, start) == text.size();
	}

	static bool hasLeadingZero(const std::string& text) {
		if (text.size() < 2) return false;

		const std::size_t start = (text[0] == '-' || text[0] == '+') ? 1 : 0;
		return start < text.size() && text[start] == '0';
	}

	static std::size_t braceSkipQuotedRun(const std::string& s, std::size_t i) {
		const char quote = s[i];
		++i;
		while (i < s.size() && s[i] != quote) {
			if (quote == '"' && s[i] == '\\' && i + 1 < s.size()) i += 2;
			else ++i;
		}

		return (i < s.size()) ? i + 1 : i;
	}

	static std::size_t braceFindOpen(const std::string& s) {
		std::size_t i = 0;
		while (i < s.size()) {
			const char c = s[i];
			if (c == '\\' && i + 1 < s.size()) {
				i += 2;
				continue;
			}

			if (c == '\'' || c == '"') {
				i = braceSkipQuotedRun(s, i);
				continue;
			}

			if (c == '{') return i;
			++i;
		}

		return std::string::npos;
	}

	static std::size_t braceFindClose(const std::string& s, std::size_t start,
			std::vector<std::size_t>& commas) {
		int depth = 1;
		std::size_t j = start;
		while (j < s.size() && depth > 0) {
			const char c = s[j];
			if (c == '\\' && j + 1 < s.size()) {
				j += 2;
				continue;
			}

			if (c == '\'' || c == '"') {
				j = braceSkipQuotedRun(s, j);
				continue;
			}

			if (c == '{') ++depth;
			if (c == '}') {
				--depth;
				if (depth == 0) return j;
			}

			if (c == ',' && depth == 1) commas.push_back(j);
			++j;
		}

		return std::string::npos;
	}

	// A step pointing away from `to` is turned around, as bash does.
	static long long orientStep(long long from, long long to, long long step) {
		if ((from < to && step < 0) || (from > to && step > 0)) return -step;
		return step;
	}

	static bool sequenceContinues(long long value, long long to, long long step) {
		return (step > 0) ? (value <= to) : (value >= to);
	}

	static std::vector<std::string> braceIntegerSequence(const std::string& from,
			const std::string& to, const std::string& step_text) {
		std::vector<std::string> alts;
		long long first = 0;
		long long last = 0;
		long long step = 1;
		if (!parseLL(from, first) || !parseLL(to, last)) return alts;
		if (!step_text.empty() && !parseLL(step_text, step)) return alts;
		if (step == 0) step = 1;
		step = orientStep(first, last, step);

		int width = 0;
		if (hasLeadingZero(from) || hasLeadingZero(to)) {
			width = static_cast<int>(std::max(from.size(), to.size()));
		}

		for (long long value = first; sequenceContinues(value, last, step); value += step) {
			if (width > 0) {
				char buffer[32];
				std::snprintf(buffer, sizeof(buffer), "%0*lld", width, value);
				alts.push_back(buffer);
			} else {
				alts.push_back(std::to_string(value));
			}
		}

		return alts;
	}

	static std::vector<std::string> braceLetterSequence(char from, char to,
			const std::string& step_text) {
		std::vector<std::string> alts;
		const int first = static_cast<unsigned char>(from);
		const int last = static_cast<unsigned char>(to);
		int step = 1;
		if (!step_text.empty() && !parseInt(step_text, step)) return alts;
		if (step == 0) step = 1;
		step = static_cast<int>(orientStep(first, last, step));

		for (int value = first; sequenceContinues(value, last, step); value += step) {
			alts.emplace_back(1, static_cast<char>(value));
		}

		return alts;
	}

	static bool isAsciiLetter(char c) {
		return std::isalpha(static_cast<unsigned char>(c)) != 0;
	}

	// `{from..to}` and `{from..to..step}`; empty when the body is not a
	// sequence, so the caller treats the braces as literal text.
	static std::vector<std::string> braceParseSequence(const std::string& body) {
		StrScan in(body);
		std::string from;
		if (!in.readUpTo("..", from)) return {};

		std::string to;
		std::string step_text;
		if (in.readUpTo("..", to)) step_text = in.rest();
		else to = in.rest();

		if (isInteger(from) && isInteger(to)) return braceIntegerSequence(from, to, step_text);

		const bool letters = from.size() == 1 && to.size() == 1
			&& isAsciiLetter(from[0]) && isAsciiLetter(to[0]);
		if (letters) return braceLetterSequence(from[0], to[0], step_text);
		return {};
	}

	static std::vector<std::string> splitBraceAlternatives(const std::string& body,
			const std::vector<std::size_t>& commas, std::size_t body_offset) {
		std::vector<std::string> alts;
		std::size_t start = 0;
		for (std::size_t comma : commas) {
			const std::size_t at = comma - body_offset;
			alts.push_back(body.substr(start, at - start));
			start = at + 1;
		}

		alts.push_back(body.substr(start));
		return alts;
	}

	static std::vector<std::string> braceExpandOnce(const std::string& s) {
		const std::size_t open = braceFindOpen(s);
		if (open == std::string::npos) return {};

		std::vector<std::size_t> commas;
		const std::size_t close = braceFindClose(s, open + 1, commas);
		if (close == std::string::npos) return {};

		const std::string body   = s.substr(open + 1, close - open - 1);
		const std::string prefix = s.substr(0, open);
		const std::string suffix = s.substr(close + 1);

		const std::vector<std::string> alts = commas.empty()
			? braceParseSequence(body)
			: splitBraceAlternatives(body, commas, open + 1);
		if (alts.empty()) return {};

		std::vector<std::string> out;
		out.reserve(alts.size());
		for (const std::string& alt : alts) out.push_back(prefix + alt + suffix);
		return out;
	}

	static std::vector<std::string> braceExpandAll(const std::string& s) {
		const std::vector<std::string> first = braceExpandOnce(s);
		if (first.empty()) return { s };

		std::vector<std::string> out;
		for (const std::string& alt : first) {
			std::vector<std::string> expanded = braceExpandAll(alt);
			for (std::string& result : expanded) out.push_back(std::move(result));
		}

		return out;
	}

	std::vector<std::string> Expander::expandWord(const Word& w) {
		const std::vector<std::string> braced = braceExpandAll(w.raw);
		if (braced.size() <= 1) return expandWordPostBrace(w);

		std::vector<std::string> out;
		for (const std::string& alt : braced) {
			if (aborting()) break;

			Lexer lexer(alt);
			std::vector<Token> tokens = lexer.tokenize();
			for (Token& token : tokens) {
				if (token.kind != TokKind::Word) continue;

				Word alt_word;
				// The token vector is local and visited once — steal the
				// segment list instead of deep-copying every segment.
				alt_word.segments = std::move(token.segments);
				alt_word.raw = std::move(token.text);
				alt_word.loc = w.loc;

				std::vector<std::string> fields = expandWordPostBrace(alt_word);
				for (std::string& field : fields) out.push_back(std::move(field));
			}
		}

		return out;
	}

	std::vector<std::string> Expander::expandWordPostBrace(const Word& w) {
		const Word tilded = applyTildeExpansion(w);
		const Tagged rendered = renderWord(tilded);
		std::vector<Tagged> fields = splitWords(rendered);
		if (fields.empty() && rendered.had_quote) {
			Tagged empty;
			empty.had_quote = true;
			fields.push_back(std::move(empty));
		}

		std::vector<std::string> out;
		for (const Tagged& field : fields) {
			std::vector<std::string> results = globExpand(field);
			for (std::string& result : results) out.push_back(std::move(result));
		}

		return out;
	}

	std::string Expander::expandStringValue(const Word& w) {
		const Word tilded = applyTildeExpansion(w);
		return quoteRemove(renderWord(tilded));
	}

}  // namespace wbsh

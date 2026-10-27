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

	static const char* const kDynamicSpecials[] = { "BASHPID", "LINENO", "RANDOM", "SECONDS" };

	static bool isDynamicSpecialName(const std::string& name) {
		for (const char* special : kDynamicSpecials) {
			if (name == special) return true;
		}

		return false;
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

		static const char* const kDivisionByZero = "division by 0";
		static const char* const kTooGreatForBase = "value too great for base";
		static const char* const kInvalidBase = "invalid arithmetic base";
		static const char* const kOperandExpected = "syntax error: operand expected";
		static const int kMaxBase = 64;

		static bool isDivision(char op) {
			return op == '/' || op == '%';
		}

		static long long wrappingNegate(long long value) {
			return static_cast<long long>(0ULL - static_cast<unsigned long long>(value));
		}

		// LLONG_MIN / -1 traps on x86; bash wraps it like negation.
		static long long divide(char op, long long lhs, long long rhs) {
			if (rhs == 0) return 0;
			if (rhs == -1 && op == '%') return 0;
			if (rhs == -1) return wrappingNegate(lhs);
			return op == '/' ? lhs / rhs : lhs % rhs;
		}

		static long long power(long long base, long long exponent) {
			unsigned long long result = 1;
			unsigned long long factor = static_cast<unsigned long long>(base);
			while (exponent > 0) {
				if ((exponent & 1) != 0) result *= factor;
				factor *= factor;
				exponent >>= 1;
			}

			return static_cast<long long>(result);
		}

		static long long combineAssign(char op, long long lhs, long long rhs) {
			switch (op) {
			case '=': return rhs;
			case '+': return lhs + rhs;
			case '-': return lhs - rhs;
			case '*': return lhs * rhs;
			case '/': return divide(op, lhs, rhs);
			case '%': return divide(op, lhs, rhs);
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

		static bool isAlnumChar(char c) {
			return std::isalnum(static_cast<unsigned char>(c)) != 0;
		}

		static bool isSpaceChar(char c) {
			return std::isspace(static_cast<unsigned char>(c)) != 0;
		}

		static bool isConstantChar(char c) {
			return isAlnumChar(c) || c == '#' || c == '@' || c == '_';
		}

		// Overflow wraps, as in bash.
		static const char* parseDigits(const std::string& digits, int base, long long& value) {
			unsigned long long accumulated = 0;
			for (const char c : digits) {
				const int digit = basedDigitValue(c, base);
				if (digit < 0 || digit >= base) return kTooGreatForBase;
				accumulated = accumulated * static_cast<unsigned long long>(base)
					+ static_cast<unsigned long long>(digit);
			}

			value = static_cast<long long>(accumulated);
			return nullptr;
		}

		static const char* parseBasedConstant(const std::string& text, std::size_t hash,
				long long& value) {
			long long base = 0;
			const char* problem = parseDigits(text.substr(0, hash), 10, base);
			if (problem != nullptr) return problem;
			if (base < 2 || base > kMaxBase) return kInvalidBase;

			return parseDigits(text.substr(hash + 1), static_cast<int>(base), value);
		}

		// Returns the diagnostic for a malformed constant, or nullptr.
		static const char* parseConstant(const std::string& text, long long& value) {
			value = 0;
			const std::size_t hash = text.find('#');
			if (hash != std::string::npos) return parseBasedConstant(text, hash, value);

			const bool leading_zero = text.size() >= 2 && text[0] == '0';
			const bool hex = leading_zero && (text[1] == 'x' || text[1] == 'X');
			if (hex) return parseDigits(text.substr(2), 16, value);
			if (leading_zero) return parseDigits(text.substr(1), 8, value);
			return parseDigits(text, 10, value);
		}

		static std::string trimSpace(const std::string& text) {
			std::size_t start = 0;
			std::size_t end = text.size();
			while (start < end && isSpaceChar(text[start])) ++start;
			while (end > start && isSpaceChar(text[end - 1])) --end;
			return text.substr(start, end - start);
		}

		// A variable operand as written: `name` or `name[subscript]`. The
		// subscript is evaluated only when the operand is used.
		struct VarRef {
			std::string name;
			bool has_subscript = false;
			std::string subscript;
		};

		// The storage a VarRef resolved to: the plain variable, an
		// indexed element, or an assoc-array element.
		struct Slot {
			std::string name;
			bool element = false;
			bool assoc = false;
			long long index = 0;
			std::string key;
		};

		// Not an error in bash: the operand reads as 0 or the assignment
		// is dropped, and evaluation goes on.
		static void warnBadSubscript(const std::string& what) {
			std::fprintf(stderr, "wbsh: %s: bad array subscript\n", what.c_str());
		}

		// A scalar has no negative indices in bash, so it counts as empty.
		static long long nextIndexAfterLast(const Environment& env, const std::string& name) {
			const Environment::IndexedArray* indexed = env.getIndexedArray(name);
			if (indexed == nullptr || indexed->empty()) return 0;
			return indexed->rbegin()->first + 1;
		}

		static std::string elementText(const Environment& env, const Slot& slot) {
			if (slot.assoc) {
				const Environment::AssocArray* assoc = env.getAssocArray(slot.name);
				if (assoc == nullptr) return {};

				const auto it = assoc->find(slot.key);
				return it == assoc->end() ? std::string() : it->second;
			}

			const Environment::IndexedArray* indexed = env.getIndexedArray(slot.name);
			if (indexed == nullptr) return slot.index == 0 ? env.get(slot.name) : std::string();

			const auto it = indexed->find(slot.index);
			return it == indexed->end() ? std::string() : it->second;
		}

		static void storeSlot(Environment& env, const Slot& slot, long long value) {
			std::string text = std::to_string(value);
			if (!slot.element) env.set(slot.name, std::move(text));
			else if (slot.assoc) env.setAssocElement(slot.name, slot.key, std::move(text));
			else env.setIndexedElement(slot.name, slot.index, std::move(text));
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

				const long long value = parseComma();
				skipWs();
				if (!eof()) failOnLeftover();
				return failed() ? 0 : value;
			}

			bool failed() const {
				return !error_.empty();
			}

			// The error is `set -u` meeting an unset variable.
			bool unbound() const {
				return unbound_;
			}

			const std::string& error() const {
				return error_;
			}

		private:
			// The first error wins; jumping to the end makes every parse
			// level unwind without consuming anything further.
			void fail(const std::string& what, std::size_t token_at) {
				if (failed()) return;

				error_ = trimSpace(src_) + ": " + what;
				const std::string token = trimSpace(src_.substr(std::min(token_at, src_.size())));
				if (!token.empty()) error_ += " (error token is \"" + token + "\")";
				pos_ = src_.size();
			}

			// Runtime errors (not syntax errors) in an operand that is
			// not evaluated, like the `1/0` in `0 && 1/0`, are ignored.
			void failEvaluation(const std::string& what, std::size_t token_at) {
				if (skipping()) return;
				fail(what, token_at);
			}

			bool skipping() const {
				return skip_depth_ > 0;
			}

			// Parses an operand that `&&`, `||` or `?:` does not need:
			// syntax is still checked, but nothing is assigned or evaluated.
			long long parseOperand(bool skip, long long (ArithEval::*parse)()) {
				if (skip) ++skip_depth_;
				const long long value = (this->*parse)();
				if (skip) --skip_depth_;
				return value;
			}

			void adoptError(const std::string& message) {
				if (failed()) return;

				error_ = message;
				pos_ = src_.size();
			}

			void adoptInnerError(const ArithEval& inner) {
				if (failed()) return;

				adoptError(inner.error());
				unbound_ = inner.unbound();
			}

			void failUnbound(const std::string& name) {
				if (failed()) return;

				adoptError(name + ": unbound variable");
				unbound_ = true;
			}

			void failOnLeftover() {
				const char c = peek();
				if (c == '=') {
					fail("attempted assignment to non-variable", pos_);
					return;
				}

				const bool operand = isAlnumChar(c) || c == '_' || c == '$' || c == '(';
				fail(operand ? "syntax error in expression"
					: "syntax error: invalid arithmetic operator", pos_);
			}

			long long parseComma() {
				long long result = parseAssign();
				while (consume(',')) result = parseAssign();
				return result;
			}

			long long parseAssign() {
				const std::size_t save = pos_;
				skipWs();
				if (!eof() && isNameStart(peek())) {
					const VarRef ref = readVarRef();
					skipWs();
					const AssignOp* op = matchAssignOp();
					if (op != nullptr) return applyAssignment(ref, *op);
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

			// The right side is evaluated before the subscript, as in bash.
			long long applyAssignment(const VarRef& ref, const AssignOp& op) {
				skipWs();
				const std::size_t rhs_at = pos_;
				const long long rhs = parseAssign();

				Slot slot;
				const bool resolved = resolveSlot(ref, /*for_write=*/true, slot);
				const long long lhs = (op.length == 1 || !resolved) ? 0 : slotValue(slot);
				if (isDivision(op.spelling[0]) && rhs == 0) {
					failEvaluation(kDivisionByZero, rhs_at);
				}

				if (failed()) return 0;

				const long long value = combineAssign(op.spelling[0], lhs, rhs);
				if (resolved) storeSlot(env_, slot, value);
				return value;
			}

			long long parseTernary() {
				const long long condition = parseLogOr();
				if (!consume('?')) return condition;

				const long long when_true = parseOperand(condition == 0, &ArithEval::parseAssign);
				if (!consume(':')) fail("`:' expected for conditional expression", pos_);
				const long long when_false = parseOperand(condition != 0, &ArithEval::parseAssign);
				return condition != 0 ? when_true : when_false;
			}

			long long parseLogOr() {
				long long left = parseLogAnd();
				while (consume("||")) {
					const long long right = parseOperand(left != 0, &ArithEval::parseLogAnd);
					left = truth(left != 0 || right != 0);
				}

				return left;
			}

			long long parseLogAnd() {
				long long left = parseBitOr();
				while (consume("&&")) {
					const long long right = parseOperand(left == 0, &ArithEval::parseBitOr);
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
						left = divide('/', left, parseDivisor());
					} else if (consumeOperator("%", "=")) {
						left = divide('%', left, parseDivisor());
					} else {
						return left;
					}
				}
			}

			long long parseDivisor() {
				skipWs();
				const std::size_t divisor_at = pos_;
				const long long divisor = parsePow();
				if (divisor == 0) failEvaluation(kDivisionByZero, divisor_at);
				return divisor;
			}

			long long parsePow() {
				const long long base = parseUnary();
				skipWs();
				if (!consume("**")) return base;

				skipWs();
				const std::size_t exponent_at = pos_;
				const long long exponent = parsePow();
				if (exponent < 0) failEvaluation("exponent less than 0", exponent_at);
				return power(base, exponent);
			}

			long long parseUnary() {
				skipWs();
				if (consumeStep("++")) return prefixStep(+1);
				if (consumeStep("--")) return prefixStep(-1);
				if (consume('+')) return parseUnary();
				if (consume('-')) return -parseUnary();
				if (consume('!')) return truth(parseUnary() == 0);
				if (consume('~')) return ~parseUnary();
				return parsePrimary();
			}

			long long prefixStep(long long delta) {
				const VarRef ref = readVarRef();
				Slot slot;
				if (!resolveSlot(ref, /*for_write=*/true, slot)) return 0;

				const long long value = slotValue(slot) + delta;
				storeSlot(env_, slot, value);
				return value;
			}

			long long postfixStep(const VarRef& ref, long long delta) {
				Slot slot;
				if (!resolveSlot(ref, /*for_write=*/true, slot)) return 0;

				const long long value = slotValue(slot);
				storeSlot(env_, slot, value + delta);
				return value;
			}

			long long parsePrimary() {
				skipWs();
				if (consume('(')) {
					const long long value = parseComma();
					if (!consume(')')) fail("missing `)'", pos_);
					return value;
				}

				if (isDigit(peek())) return readNumber();
				if (isNameStart(peek())) return parseVariable();
				if (consume('$')) return parseDollar();

				fail(kOperandExpected, eof() ? last_operator_at_ : pos_);
				return 0;
			}

			long long parseVariable() {
				const VarRef ref = readVarRef();
				skipWs();
				if (consume("++")) return postfixStep(ref, +1);
				if (consume("--")) return postfixStep(ref, -1);

				Slot slot;
				if (!resolveSlot(ref, /*for_write=*/false, slot)) return 0;
				return slotValue(slot);
			}

			// The subscript must follow the name directly: `a [1]` is a
			// syntax error, as in bash.
			VarRef readVarRef() {
				VarRef ref;
				skipWs();
				const std::size_t start = pos_;
				ref.name = readIdent();
				if (peek() != '[') return ref;

				const std::size_t close = scanBalanced(src_, pos_ + 1, '[', ']', 1);
				if (close >= src_.size()) {
					fail("bad array subscript", start);
					return ref;
				}

				ref.has_subscript = true;
				ref.subscript = src_.substr(pos_ + 1, close - pos_ - 1);
				pos_ = close + 1;
				return ref;
			}

			// False when nothing may be read or written: the operand is
			// skipped, an error is pending, or the subscript is bad.
			bool resolveSlot(const VarRef& ref, bool for_write, Slot& slot) {
				if (skipping() || failed()) return false;

				slot.name = ref.name;
				if (!ref.has_subscript) return true;

				slot.element = true;
				if (env_.isAssocArray(ref.name)) return resolveAssocKey(ref, slot);
				return resolveIndex(ref, for_write, slot);
			}

			bool resolveAssocKey(const VarRef& ref, Slot& slot) {
				if (ref.subscript.empty()) {
					warnBadSubscript(ref.name + "[]");
					return false;
				}

				slot.assoc = true;
				slot.key = ref.subscript;
				return true;
			}

			// A negative index counts back from one past the last element.
			bool resolveIndex(const VarRef& ref, bool for_write, Slot& slot) {
				if (trimSpace(ref.subscript).empty()) {
					warnBadSubscript(ref.name + "[]");
					return false;
				}

				slot.index = evaluateNested(ref.subscript, ref.name);
				if (failed()) return false;
				if (slot.index < 0) slot.index += nextIndexAfterLast(env_, ref.name);
				if (slot.index >= 0) return true;

				warnBadSubscript(for_write ? ref.name + "[" + ref.subscript + "]" : ref.name);
				return false;
			}

			long long slotValue(const Slot& slot) {
				if (!slot.element) return variableValue(slot.name);
				return textValue(elementText(env_, slot), slot.name);
			}

			long long parseDollar() {
				if (isNameStart(peek())) return variableValue(readIdent());
				if (!isDigit(peek())) return 0;

				std::string number;
				while (isDigit(peek())) number.push_back(advance());
				return variableValue(number);
			}

			long long variableValue(const std::string& name) {
				if (name.empty() || skipping()) return 0;
				if (env_.nounset() && !env_.has(name) && !isDynamicSpecialName(name)) {
					failUnbound(name);
					return 0;
				}

				return textValue(env_.get(name), name);
			}

			// A variable's text is an expression in its own right (`x=2+3`).
			long long textValue(const std::string& text, const std::string& name) {
				if (text.empty()) return 0;

				long long value = 0;
				std::size_t consumed = 0;
				if (parseLL(text, value, 0, &consumed) && consumed == text.size()) return value;
				return evaluateNested(text, name);
			}

			long long evaluateNested(const std::string& text, const std::string& name) {
				if (depth_ > kArithNestingLimit) {
					adoptError(name + ": expression recursion level exceeded (error token is \""
						+ name + "\")");
					return 0;
				}

				ArithEval inner(text, env_, depth_ + 1);
				const long long value = inner.run();
				if (inner.failed()) adoptInnerError(inner);
				return value;
			}

			// The constant runs to the end of its alphanumeric spelling,
			// so `08`, `2#2` and `12ab` are rejected whole, not read in part.
			long long readNumber() {
				const std::size_t start = pos_;
				while (!eof() && isConstantChar(peek())) ++pos_;

				long long value = 0;
				const char* problem = parseConstant(src_.substr(start, pos_ - start), value);
				if (problem != nullptr) fail(problem, start);
				return value;
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

				last_operator_at_ = pos_;
				++pos_;
				return true;
			}

			bool consume(const char* text) {
				skipWs();
				const std::size_t length = std::strlen(text);
				if (pos_ + length > src_.size()) return false;
				if (src_.compare(pos_, length, text) != 0) return false;

				last_operator_at_ = pos_;
				pos_ += length;
				return true;
			}

			// `++` / `--` only step a variable; before anything else they
			// are two signs, so `++5` is 5 as in bash.
			bool consumeStep(const char* op) {
				skipWs();
				const std::size_t save = pos_;
				if (!consume(op)) return false;

				skipWs();
				if (!eof() && isNameStart(peek())) return true;

				pos_ = save;
				return false;
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

				last_operator_at_ = pos_;
				pos_ += length;
				return true;
			}

			const std::string& src_;
			std::size_t pos_ = 0;
			std::size_t last_operator_at_ = 0;
			std::string error_;
			bool unbound_ = false;
			int skip_depth_ = 0;
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
		const long long value = evaluator.run();
		if (evaluator.unbound()) failFatal(evaluator.error());
		else if (evaluator.failed()) fail(evaluator.error());
		return value;
	}

	static bool heredocEscapes(char c) {
		return c == '\\' || c == '$' || c == '`';
	}

	static bool doubleQuoteEscapes(char c) {
		return heredocEscapes(c) || c == '"';
	}

	static void expandBackslash(const std::string& body, std::size_t& i,
			bool (*escapes)(char), std::string& out) {
		const char next = body[i + 1];
		if (escapes(next)) {
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
				expandBackslash(body, i, heredocEscapes, out);
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
				expandBackslash(body, i, doubleQuoteEscapes, out);
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

	static void appendPatternSingleQuoted(const std::string& body, std::size_t& i,
			std::string& out) {
		std::string text;
		copySingleQuotedRun(body, i, text);
		appendFnmatchLiteral(out, text);
	}

	// Inside double quotes only `\\`, `\$`, `` \` `` and `\"` escape;
	// any other backslash there is a literal character of its own.
	static void appendPatternBackslash(const std::string& body, std::size_t& i, bool in_dq,
			std::string& out) {
		const char next = body[i + 1];
		if (next == '\n') {
			i += 2;
			return;
		}

		const bool escapes = !in_dq || next == '\\' || next == '$' || next == '`' || next == '"';
		if (!escapes) {
			appendFnmatchLiteral(out, '\\');
			++i;
			return;
		}

		appendFnmatchLiteral(out, next);
		i += 2;
	}

	void Expander::appendPatternExpansion(const std::string& body, std::size_t& i, bool in_dq,
			std::string& out) {
		std::string piece;
		if (body[i] == '$' && i + 1 < body.size()) expandHeredocDollar(body, i, piece);
		else if (body[i] == '`') expandHeredocBackquote(body, i, piece);
		else piece.push_back(body[i++]);

		if (in_dq) appendFnmatchLiteral(out, piece);
		else out += piece;
	}

	// The pattern of `${name#pat}`, `${name/pat/rep}` and friends: like
	// expandParamWordArg, but quoted text comes out escaped, so only
	// unquoted glob characters (also those from unquoted expansions)
	// act as wildcards.
	std::string Expander::expandPatternArg(const std::string& body) {
		std::string out;
		bool in_dq = false;
		std::size_t i = 0;
		while (i < body.size() && !aborting()) {
			const char c = body[i];
			if (c == '"') {
				in_dq = !in_dq;
				++i;
				continue;
			}

			if (c == '\'' && !in_dq) {
				appendPatternSingleQuoted(body, i, out);
				continue;
			}

			if (c == '\\' && i + 1 < body.size()) {
				appendPatternBackslash(body, i, in_dq, out);
				continue;
			}

			appendPatternExpansion(body, i, in_dq, out);
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

	// Where tilde prefixes may start. A command word only expands a
	// leading `~`; an assignment value also expands one after each
	// unquoted `:`, and a word spelled like an assignment does so from
	// the character after its first `=`.
	struct TildeScope {
		std::size_t start = 0;
		bool after_colons = false;
	};

	struct TildeWalk {
		const Environment& env;
		bool after_colons;
		bool eligible = true;
		bool last_segment = false;
		std::vector<WordSegment> out;
		std::string literal;
	};

	static bool hasLiteralTilde(const Word& w) {
		for (const WordSegment& segment : w.segments) {
			if (segment.kind != WordSegment::Kind::Literal) continue;
			if (segment.text.find('~') != std::string::npos) return true;
		}

		return false;
	}

	// npos when the prefix runs on into a quoted or expanded segment:
	// a prefix with any quoted character is not expanded.
	static std::size_t tildePrefixEnd(const std::string& text, std::size_t tilde,
	                                  bool last_segment) {
		std::size_t end = tilde + 1;
		while (end < text.size() && text[end] != '/' && text[end] != ':') ++end;
		if (end == text.size() && !last_segment) return std::string::npos;
		return end;
	}

	// False when the prefix names nothing this shell expands: `~user`
	// is not implemented, and an unset HOME leaves the word alone.
	static bool tildeReplacement(const Environment& env, const std::string& name,
	                             std::string& out) {
		const char* variable = nullptr;
		if (name.empty())    variable = "HOME";
		else if (name == "+") variable = "PWD";
		else if (name == "-") variable = "OLDPWD";
		if (variable == nullptr || !env.has(variable)) return false;

		out = env.get(variable);
		return true;
	}

	static void flushTildeLiteral(TildeWalk& walk) {
		if (walk.literal.empty()) return;

		WordSegment segment;
		segment.kind = WordSegment::Kind::Literal;
		segment.text = std::move(walk.literal);
		walk.out.push_back(std::move(segment));
		walk.literal.clear();
	}

	static bool tryReplaceTilde(TildeWalk& walk, const std::string& text, std::size_t& i) {
		const std::size_t end = tildePrefixEnd(text, i, walk.last_segment);
		if (end == std::string::npos) return false;

		const std::string name = text.substr(i + 1, end - i - 1);
		WordSegment replacement;
		replacement.kind = WordSegment::Kind::SingleQuoted;
		if (!tildeReplacement(walk.env, name, replacement.text)) return false;

		flushTildeLiteral(walk);
		walk.out.push_back(std::move(replacement));
		i = end;
		return true;
	}

	static void walkTildeLiteral(TildeWalk& walk, const std::string& text, std::size_t from) {
		std::size_t i = from;
		while (i < text.size()) {
			if (walk.eligible && text[i] == '~' && tryReplaceTilde(walk, text, i)) {
				walk.eligible = false;
				continue;
			}

			walk.eligible = walk.after_colons && text[i] == ':';
			walk.literal.push_back(text[i]);
			++i;
		}
	}

	static void walkTildeSegment(TildeWalk& walk, const WordSegment& segment,
	                             std::size_t from) {
		if (segment.kind != WordSegment::Kind::Literal) {
			flushTildeLiteral(walk);
			walk.out.push_back(segment);
			walk.eligible = false;
			return;
		}

		walk.literal.append(segment.text, 0, from);
		walkTildeLiteral(walk, segment.text, from);
	}

	static Word expandTildePrefixes(const Environment& env, const Word& w, TildeScope scope) {
		if (!hasLiteralTilde(w)) return w;

		TildeWalk walk{env, scope.after_colons};
		for (std::size_t k = 0; k < w.segments.size(); ++k) {
			walk.last_segment = k + 1 == w.segments.size();
			walkTildeSegment(walk, w.segments[k], k == 0 ? scope.start : 0);
		}

		flushTildeLiteral(walk);

		Word out;
		out.segments = std::move(walk.out);
		out.raw = w.raw;
		out.loc = w.loc;
		return out;
	}

	// Offset just past the first `=` of a word spelled like an
	// assignment (`name=`, `name+=`, `name[sub]=`); 0 for other words.
	static std::size_t assignmentWordValueStart(const Word& w) {
		if (w.segments.empty() || w.segments[0].kind != WordSegment::Kind::Literal) return 0;

		const std::string& text = w.segments[0].text;
		if (text.empty() || !isNameStart(text[0])) return 0;

		std::size_t i = 1;
		while (i < text.size() && isNameCont(text[i])) ++i;
		if (i < text.size() && text[i] == '[') {
			i = text.find(']', i);
			if (i == std::string::npos) return 0;
			++i;
		}

		if (i < text.size() && text[i] == '+') ++i;
		if (i < text.size() && text[i] == '=') return i + 1;
		return 0;
	}

	Word Expander::applyTildeExpansion(const Word& w) {
		TildeScope scope;
		scope.start = assignmentWordValueStart(w);
		scope.after_colons = scope.start != 0;
		return expandTildePrefixes(env_, w, scope);
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

	bool Expander::isParamSet(const std::string& name) const {
		if (isDigit(name[0])) return !isUnsetPositional(name);
		if (name.size() == 1 && isSpecialParam1(name[0])) return true;
		return isVariableSet(name);
	}

	bool Expander::isUnsetPositional(const std::string& name) const {
		if (name.empty() || scanDigitsEnd(name, 0) != name.size()) return false;

		unsigned long index = 0;
		if (!parseUL(name, index)) return true;
		return index > env_.positional().size();
	}

	std::string Expander::lookupParam(const std::string& name, bool suppress_nounset) {
		if (name.empty()) return {};

		if (env_.nounset() && !suppress_nounset && isUnsetPositional(name)) {
			failFatal(name + ": unbound variable");
			return {};
		}

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
		if (env_.nounset() && !suppress_nounset) failFatal(name + ": unbound variable");
		return {};
	}

	std::string Expander::starSeparator(bool star_join_ifs) const {
		if (!star_join_ifs) return " ";

		const std::string ifs = env_.ifs();
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
			const Environment::IndexedArray& items, const std::string& subscript,
			Expander::ElementSlot& slot) {
		if (!expander.tryEvalArith(subscript, slot.index)) return {};
		if (slot.index < 0 && !items.empty()) slot.index += items.rbegin()->first + 1;
		slot.resolved = true;

		const auto it = items.find(slot.index);
		if (it == items.end()) return {};

		slot.is_set = true;
		return it->second;
	}

	static std::string assocArrayElement(const Environment::AssocArray& items,
			const std::string& key, Expander::ElementSlot& slot) {
		slot.assoc = true;
		slot.key = key;
		slot.resolved = true;

		const auto it = items.find(key);
		if (it == items.end()) return {};

		slot.is_set = true;
		return it->second;
	}

	std::string Expander::lookupElement(const std::string& name, const std::string& subscript,
			ElementSlot& slot) {
		if (const auto* indexed = env_.getIndexedArray(name); indexed != nullptr) {
			return indexedArrayElement(*this, *indexed, subscript, slot);
		}

		if (const auto* assoc = env_.getAssocArray(name); assoc != nullptr) {
			return assocArrayElement(*assoc, subscript, slot);
		}

		if (!tryEvalArith(subscript, slot.index)) return {};
		slot.resolved = true;
		slot.is_set = slot.index == 0 && env_.has(name);
		return slot.is_set ? lookupParam(name) : std::string();
	}

	std::string Expander::lookupSubscripted(const std::string& name,
			const std::string& subscript_in, bool star_join_ifs, ElementSlot* slot) {
		ElementSlot local;
		ElementSlot& out = (slot != nullptr) ? *slot : local;
		const std::string subscript = resolveSubscript(subscript_in);
		const bool whole_array = (subscript == "@" || subscript == "*");
		if (!whole_array) return lookupElement(name, subscript, out);

		const std::string sep = (subscript == "*") ? starSeparator(star_join_ifs)
			: std::string(" ");
		out.is_set = arrayLength(name) != 0;
		return joinWholeArray(name, sep);
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

		ElementSlot slot;
		const std::string element = lookupSubscripted(name, subscript, false, &slot);
		if (env_.nounset() && !slot.is_set) failFatal(name + ": unbound variable");
		return std::to_string(element.size());
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

	std::string Expander::evalParamDefaultOp(char op, bool colon, const ParamOpCtx& ctx,
			const std::string& arg) {
		const std::string& cur = ctx.named_value;
		const bool unset = ctx.has_subscript ? !ctx.slot.is_set : !isParamSet(ctx.name);
		const bool empty_or_unset = colon ? (unset || cur.empty()) : unset;

		switch (op) {
		case '-':
			return empty_or_unset ? expandParamWordArg(arg) : cur;
		case '+':
			return empty_or_unset ? std::string() : expandParamWordArg(arg);
		case '=': {
			if (!empty_or_unset) return cur;

			const std::string value = expandParamWordArg(arg);
			assignParamDefault(ctx, value);
			return value;
		}
		case '?': {
			if (!empty_or_unset) return cur;

			const std::string message = arg.empty() ? std::string("parameter null or not set")
				: expandParamWordArg(arg);
			failFatal(ctx.label + ": " + message);
			return {};
		}
		default:
			return cur;
		}
	}

	void Expander::assignParamDefault(const ParamOpCtx& ctx, const std::string& value) {
		if (!ctx.has_subscript) {
			env_.set(ctx.name, value);
			return;
		}

		const ElementSlot& slot = ctx.slot;
		if (!slot.resolved) return;
		if (slot.assoc) env_.setAssocElement(ctx.name, slot.key, value);
		else if (slot.index >= 0) env_.setIndexedElement(ctx.name, slot.index, value);
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

	Expander::ReplaceArgs Expander::parseReplaceArgs(const std::string& args) {
		ReplaceArgs out;
		std::string body = args;
		out.anchor_start = !body.empty() && body[0] == '#';
		out.anchor_end   = !out.anchor_start && !body.empty() && body[0] == '%';
		if (out.anchor_start || out.anchor_end) body.erase(0, 1);

		const std::size_t slash = body.find('/');
		const std::string pat = (slash == std::string::npos) ? body : body.substr(0, slash);
		const std::string rep = (slash == std::string::npos) ? std::string()
			: body.substr(slash + 1);
		out.pat = expandPatternArg(pat);
		out.rep = expandParamWordArg(rep);
		return out;
	}

	std::string Expander::evalParamReplace(const std::string& cur, const std::string& args,
			bool all) {
		const ReplaceArgs r = parseReplaceArgs(args);
		return replacePattern(cur, r.pat, r.rep, all, r.anchor_start, r.anchor_end);
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
			return evalParamDefaultOp(op, colon, ctx, argument);
		case ':':
			return substringExpand(ctx.named_value, argument);
		case '#':
			return stripPrefix(ctx.named_value, expandPatternArg(argument), doubled);
		case '%':
			return stripSuffix(ctx.named_value, expandPatternArg(argument), doubled);
		case '/':
			return evalParamReplace(ctx.named_value, argument, doubled);
		case '^':
		case ',':
			return applyCaseConv(ctx.named_value, op, doubled, expandPatternArg(argument));
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

	// `${@=word}` is an error in bash, so `=` keeps the scalar path.
	static bool isListOp(const std::string& body, std::size_t i) {
		const char op = body[i];
		if (op == ':') return i + 1 >= body.size() || body[i + 1] != '=';
		return op == '-' || op == '+' || op == '?' || op == '#' || op == '%'
			|| op == '/' || op == '^' || op == ',';
	}

	// Bare `@` / `name[@]` forms are not list params: they have no
	// operator, and the caller renders them directly.
	bool Expander::parseListParam(const std::string& body, ListParam& out) const {
		if (body.empty()) return false;

		std::size_t end = 1;
		if (body[0] == '@' || body[0] == '*') {
			out.name.clear();
			out.star = body[0] == '*';
		} else {
			if (!isNameStart(body[0])) return false;

			const std::size_t open = scanIdentifierEnd(body, 0);
			if (open + 3 > body.size() || body[open] != '[' || body[open + 2] != ']') return false;
			if (body[open + 1] != '@' && body[open + 1] != '*') return false;

			out.name = body.substr(0, open);
			out.star = body[open + 1] == '*';
			end = open + 3;
		}

		if (end >= body.size() || !isListOp(body, end)) return false;
		out.op_pos = end;
		return true;
	}

	std::vector<std::string> Expander::listParamValues(const ListParam& p) const {
		if (p.name.empty()) return env_.positional();
		return arrayValues(p.name);
	}

	// The elements a slice picks from, keyed by what its offset counts:
	// `$0` is key 0 of the positionals, an indexed array keeps its own
	// (maybe sparse) indices, and an assoc array counts 0..n-1.
	Expander::KeyedList Expander::sliceCandidates(const ListParam& p) const {
		KeyedList out;
		if (const auto* indexed = env_.getIndexedArray(p.name); indexed != nullptr) {
			for (const auto& entry : *indexed) out.emplace_back(entry.first, entry.second);
			return out;
		}

		std::vector<std::string> values = listParamValues(p);
		if (p.name.empty()) values.insert(values.begin(), env_.shellName());
		for (std::size_t k = 0; k < values.size(); ++k) {
			out.emplace_back(static_cast<long long>(k), std::move(values[k]));
		}

		return out;
	}

	// A negative offset counts back from one past the highest key. A
	// negative length is an error for a list, not a count from the end.
	std::vector<std::string> Expander::sliceListParam(const ListParam& p,
			const std::string& args) {
		const bool scalar = !p.name.empty() && env_.getIndexedArray(p.name) == nullptr
			&& env_.getAssocArray(p.name) == nullptr;
		if (scalar && !env_.has(p.name)) return {};
		if (scalar) return { substringExpand(env_.get(p.name), args) };

		const KeyedList items = sliceCandidates(p);
		std::string offset_text;
		std::string length_text;
		splitSliceArgs(args, offset_text, length_text);

		long long offset = 0;
		if (!tryEvalArith(offset_text, offset)) offset = 0;
		if (offset < 0 && !items.empty()) offset += items.back().first + 1;
		if (offset < 0) return {};

		long long take = static_cast<long long>(items.size());
		if (!length_text.empty() && !tryEvalArith(length_text, take)) take = 0;
		if (take < 0) {
			fail(std::to_string(take) + ": substring expression < 0");
			return {};
		}

		std::vector<std::string> out;
		for (const auto& item : items) {
			if (static_cast<long long>(out.size()) >= take) break;
			if (item.first >= offset) out.push_back(item.second);
		}

		return out;
	}

	// A list is null when it joins to an empty string: no elements, or
	// one empty element. When `+` drops its word, a list that had
	// elements still leaves one empty field.
	std::vector<std::string> Expander::evalListDefaultOp(char op, bool colon,
			const std::string& subject, std::vector<std::string> elems,
			const std::string& arg) {
		const bool unset = elems.empty();
		const bool null = unset || (elems.size() == 1 && elems[0].empty());
		const bool missing = colon ? null : unset;

		switch (op) {
		case '-':
			if (missing) return { expandParamWordArg(arg) };
			return elems;
		case '+':
			if (!missing) return { expandParamWordArg(arg) };
			if (unset) return {};
			return { std::string() };
		case '?':
			if (!missing) return elems;

			failFatal(subject + ": " + (arg.empty() ? std::string("parameter null or not set")
				: expandParamWordArg(arg)));
			return {};
		default:
			return elems;
		}
	}

	std::vector<std::string> Expander::mapListOp(char op, bool doubled,
			const std::string& arg, std::vector<std::string> elems) {
		if (op == '/') {
			const ReplaceArgs r = parseReplaceArgs(arg);
			for (std::string& elem : elems) {
				elem = replacePattern(elem, r.pat, r.rep, doubled, r.anchor_start, r.anchor_end);
			}

			return elems;
		}

		const std::string pat = expandPatternArg(arg);
		for (std::string& elem : elems) {
			if (op == '#') elem = stripPrefix(elem, pat, doubled);
			else if (op == '%') elem = stripSuffix(elem, pat, doubled);
			else elem = applyCaseConv(elem, op, doubled, pat);
		}

		return elems;
	}

	std::vector<std::string> Expander::expandListParam(const ListParam& p,
			const std::string& body) {
		std::size_t i = p.op_pos;
		char op = body[i];
		bool colon = false;
		if (op == ':' && i + 1 < body.size() && isDefaultOp(body[i + 1])) {
			colon = true;
			++i;
			op = body[i];
		}

		const bool doubled = isDoublingOp(op) && i + 1 < body.size() && body[i + 1] == op;
		const std::string argument = body.substr(doubled ? i + 2 : i + 1);
		if (op == ':') return sliceListParam(p, argument);
		if (isDefaultOp(op)) {
			const std::string subject = p.label.empty() ? body.substr(0, p.op_pos) : p.label;
			return evalListDefaultOp(op, colon, subject, listParamValues(p), argument);
		}

		return mapListOp(op, doubled, argument, listParamValues(p));
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

	bool Expander::isVariableSet(const std::string& name) const {
		return env_.has(name) || isDynamicSpecialName(name);
	}

	static void addPrefixedName(std::set<std::string>& names, const std::string& name,
			const std::string& prefix) {
		if (name.compare(0, prefix.size(), prefix) == 0 && isPlainName(name)) names.insert(name);
	}

	std::vector<std::string> Expander::namesWithPrefix(const std::string& prefix) const {
		std::set<std::string> names;
		for (const auto& entry : env_.vars()) addPrefixedName(names, entry.first, prefix);
		for (const auto& entry : env_.indexedArrays()) addPrefixedName(names, entry.first, prefix);
		for (const auto& entry : env_.assocArrays()) addPrefixedName(names, entry.first, prefix);
		for (const char* special : kDynamicSpecials) addPrefixedName(names, special, prefix);
		return std::vector<std::string>(names.begin(), names.end());
	}

	// `${!prefix*}` / `${!prefix@}`: the names of the set variables
	// that start with `prefix`.
	static bool parsePrefixNamesForm(const std::string& body, std::string& prefix, bool& star) {
		if (body.size() < 3 || body[0] != '!') return false;
		if (body.back() != '*' && body.back() != '@') return false;

		prefix = body.substr(1, body.size() - 2);
		star = body.back() == '*';
		return isPlainName(prefix);
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

	static bool isIndirectBody(const std::string& body) {
		return body.size() > 1 && body[0] == '!' && fieldAwareArrayName(body).empty();
	}

	// What an indirect reference may name: a variable, optionally
	// subscripted, a positional parameter, or a special parameter.
	static bool isParamSpec(const std::string& text, const Expander& expander) {
		std::string name;
		const std::size_t end = scanParamName(text, name, expander);
		if (end == std::string::npos) return false;
		if (end == text.size()) return true;
		return isNameStart(text[0]) && text[end] == '[' && text.back() == ']';
	}

	static bool assignsDefault(const std::string& rest) {
		if (!rest.empty() && rest[0] == '=') return true;
		return rest.size() > 1 && rest[0] == ':' && rest[1] == '=';
	}

	// An unset reference leaves its own (unset) spelling as the subject.
	bool Expander::resolveUnsetIndirect(const std::string& spec, const std::string& rest,
			const std::string& label, std::string& out_body) {
		if (env_.nounset() && !paramOpHandlesUnset(rest, 0)) {
			failFatal(label + ": unbound variable");
			return true;
		}

		if (assignsDefault(rest)) {
			fail(label + ": invalid indirect expansion");
			return true;
		}

		out_body = spec;
		return true;
	}

	bool Expander::resolveIndirectBody(const std::string& body, std::string& out_body,
			std::string& out_label) {
		const std::string spec = body.substr(1);
		std::string ref;
		std::size_t i = scanParamName(spec, ref, *this);
		if (i == std::string::npos) return false;

		std::string subscript;
		const bool has_subscript = takeSubscript(spec, i, subscript);
		const std::string rest = spec.substr(i);
		out_label = "!" + spec.substr(0, i);

		ElementSlot slot;
		const std::string target = has_subscript ? lookupSubscripted(ref, subscript, true, &slot)
			: lookupParam(ref, true);
		if (aborting()) return true;

		bool ref_set = !target.empty();
		if (has_subscript) ref_set = slot.is_set;
		else if (isPlainName(ref)) ref_set = isVariableSet(ref);
		if (!ref_set) return resolveUnsetIndirect(spec, rest, out_label, out_body);

		if (!isParamSpec(target, *this)) {
			fail(target + ": bad substitution");
			return true;
		}

		if (env_.nounset() && !paramOpHandlesUnset(rest, 0) && isPlainName(target)
		    && !isVariableSet(target)) {
			failFatal(out_label + ": unbound variable");
			return true;
		}

		out_body = target + rest;
		return true;
	}

	// `${a[@]}` and `${a[*]}` are exempt from `set -u`, as in bash.
	void Expander::checkElementBound(const std::string& subject, const std::string& subscript,
			const ElementSlot& slot) {
		if (!env_.nounset() || slot.is_set) return;
		if (subscript == "@" || subscript == "*") return;

		failFatal(subject + ": unbound variable");
	}

	std::string Expander::expandBangParam(const std::string& body) {
		std::string prefix;
		bool star = false;
		if (parsePrefixNamesForm(body, prefix, star)) {
			return joinWith(namesWithPrefix(prefix), starSeparator(star));
		}

		const std::string keys_of = fieldAwareArrayName(body);
		if (!keys_of.empty()) return joinWith(arrayKeys(keys_of), " ");

		std::string resolved;
		std::string label;
		if (!resolveIndirectBody(body, resolved, label)) return expandDirectParam(body, {});
		if (aborting()) return {};
		return expandDirectParam(resolved, label);
	}

	std::string Expander::expandParam(const std::string& body, bool /*quoted_ctx*/) {
		if (body.empty()) return {};

		if (body[0] == '#' && body.size() > 1) return expandParamLengthForm(body);
		if (body[0] == '!' && body.size() > 1) return expandBangParam(body);
		return expandDirectParam(body, {});
	}

	std::string Expander::expandDirectParam(const std::string& body, const std::string& label) {
		ListParam list;
		if (parseListParam(body, list)) {
			list.label = label;
			return joinWith(expandListParam(list, body), starSeparator(list.star));
		}

		std::string name;
		std::size_t i = scanParamName(body, name, *this);
		if (i == std::string::npos) return {};

		std::string subscript;
		const bool has_subscript = takeSubscript(body, i, subscript);

		// `${name:-…}`, `${name-…}`, `${name:=…}`, `${name:?…}` (and their
		// colon-less siblings) handle an unset `name` themselves, so the
		// lookup here must not raise `set -u`'s unbound-variable error.
		const bool handles_unset = paramOpHandlesUnset(body, i);
		ElementSlot slot;
		const std::string named = has_subscript
			? lookupSubscripted(name, subscript, true, &slot)
			: lookupParam(name, handles_unset);
		const std::string subject = label.empty() ? body.substr(0, i) : label;
		if (has_subscript && !handles_unset) checkElementBound(subject, subscript, slot);
		if (i >= body.size()) return named;

		ParamOpCtx ctx{ name, subscript, has_subscript, body, named, subject, slot };
		char op = body[i];
		bool colon = false;
		if (op == ':' && i + 1 < body.size() && isDefaultOp(body[i + 1])) {
			colon = true;
			++i;
			op = body[i];
		}

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

	Expander::Tagged Expander::renderWord(const Word& w) {
		Tagged out;
		out.reserve(w.raw.size());
		for (const WordSegment& segment : w.segments) {
			if (aborting()) break;
			renderSegment(segment, out, /*inside_dq=*/false);
		}

		return out;
	}

	// The `*` forms join with the first IFS character, as `$*` does.
	// The `@` forms separate elements with an F_FIELD_END marker, so
	// each element stays a field of its own whatever IFS holds.
	bool Expander::renderArrayFields(const std::vector<std::string>& elems, bool star,
			bool inside_dq, Tagged& out) {
		const std::uint8_t mark = inside_dq ? F_QUOTED : 0;
		const std::string sep = starSeparator(true);

		for (std::size_t k = 0; k < elems.size(); ++k) {
			if (k != 0 && star) pushText(out, sep, mark);
			if (k != 0 && !star) out.endField(inside_dq);
			pushText(out, elems[k], mark);
		}

		return !star && elems.empty();
	}

	// An empty `@` list inside the quotes removes the quoted null too,
	// as in bash: with no positionals, `"$@"` and `"$x$@"` (x empty)
	// both give zero fields.
	void Expander::renderDoubleQuoted(const WordSegment& s, Tagged& out) {
		bool quoted_null = true;
		for (const WordSegment& nested : s.nested) {
			if (aborting()) break;
			if (renderSegment(nested, out, /*inside_dq=*/true)) quoted_null = false;
		}

		if (quoted_null) out.had_quote = true;
	}

	bool Expander::renderParamExpSegment(const WordSegment& s, Tagged& out, bool inside_dq) {
		std::string prefix;
		bool star = false;
		if (parsePrefixNamesForm(s.text, prefix, star)) {
			return renderArrayFields(namesWithPrefix(prefix), star, inside_dq, out);
		}

		std::string resolved;
		std::string label;
		if (isIndirectBody(s.text) && resolveIndirectBody(s.text, resolved, label)) {
			if (aborting()) return false;
			return renderParamBody(resolved, label, inside_dq, out);
		}

		return renderParamBody(s.text, {}, inside_dq, out);
	}

	// A body reached through `${!ref}` has a label and is never read as
	// another `!` or `#` form.
	bool Expander::renderParamBody(const std::string& body, const std::string& label,
			bool inside_dq, Tagged& out) {
		if (body == "@") {
			return renderArrayFields(env_.positional(), /*star=*/false, inside_dq, out);
		}

		ListParam list;
		if (parseListParam(body, list)) {
			list.label = label;
			return renderArrayFields(expandListParam(list, body), list.star, inside_dq, out);
		}

		const std::uint8_t mark = inside_dq ? F_QUOTED : 0;
		const std::string array_name = fieldAwareArrayName(body);
		if (array_name.empty()) {
			const std::string value = label.empty() ? expandParam(body, inside_dq)
				: expandDirectParam(body, label);
			pushText(out, value, mark);
			return false;
		}

		const bool indices = body[0] == '!';
		const bool star    = body.find('*') != std::string::npos;
		const std::vector<std::string> elems = indices ? arrayKeys(array_name)
			: arrayValues(array_name);
		return renderArrayFields(elems, star, inside_dq, out);
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

	bool Expander::renderSegment(const WordSegment& s, Tagged& out, bool inside_dq) {
		using K = WordSegment::Kind;
		const std::uint8_t mark = inside_dq ? F_QUOTED : 0;
		switch (s.kind) {
		case K::Literal:
			pushText(out, s.text, mark);
			break;
		case K::Escaped:
		case K::SingleQuoted:
			pushText(out, s.text, F_QUOTED);
			out.had_quote = true;
			break;
		case K::DoubleQuoted:
			renderDoubleQuoted(s, out);
			break;
		case K::DollarSingle:
			pushText(out, interpretAnsiC(s.text), F_QUOTED);
			out.had_quote = true;
			break;
		case K::SimpleVar:
			if (s.text == "@") {
				return renderArrayFields(env_.positional(), /*star=*/false, inside_dq, out);
			}

			if (env_.nounset() && isUnsetPositional(s.text)) {
				failFatal("$" + s.text + ": unbound variable");
				break;
			}

			pushText(out, lookupParam(s.text), mark);
			break;
		case K::ParamExp:
			return renderParamExpSegment(s, out, inside_dq);
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

		return false;
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

	// IFS-split a text that holds no F_FIELD_END marker. A text with a
	// quoted part gives at least one field, even an empty one.
	static void splitOnIfs(const Expander::Tagged& t, const std::string& ifs,
			std::vector<Expander::Tagged>& out) {
		const std::size_t first = out.size();
		std::size_t i = 0;
		while (!ifs.empty() && i < t.size() && !quotedAt(t, i) && isIfsWhitespace(ifs, t.text[i])) {
			++i;
		}

		while (!ifs.empty() && i < t.size()) {
			out.push_back(takeField(t, ifs, i));
			skipFieldSeparator(t, ifs, i);
		}

		if (ifs.empty() && t.size() > 0) out.push_back(t);
		if (out.size() == first && t.had_quote) {
			Expander::Tagged empty;
			empty.had_quote = true;
			out.push_back(std::move(empty));
		}
	}

	static bool hasFieldEnd(const Expander::Tagged& t) {
		for (std::uint8_t f : t.flags) {
			if ((f & Expander::F_FIELD_END) != 0) return true;
		}

		return false;
	}

	// An unquoted `$@` acts as if its elements were joined with the
	// first IFS character, so `IFS=:` keeps empty elements, as in bash.
	// A quoted `"$@"`, or any `$@` when IFS is empty, cuts a hard field
	// boundary instead.
	std::vector<Expander::Tagged> Expander::splitWords(const Tagged& t) {
		const std::string ifs = env_.ifs();
		std::vector<Tagged> out;
		if (!hasFieldEnd(t)) {
			splitOnIfs(t, ifs, out);
			return out;
		}

		Tagged field;
		for (std::size_t i = 0; i < t.size(); ++i) {
			const std::uint8_t f = t.flags[i];
			if ((f & F_FIELD_END) == 0) {
				field.push(t.text[i], f);
				continue;
			}

			field.had_quote = field.had_quote || (f & F_QUOTED_FIELD) != 0;
			if ((f & F_QUOTED) == 0 && !ifs.empty()) {
				field.push(ifs[0], 0);
				continue;
			}

			splitOnIfs(field, ifs, out);
			field = Tagged();
		}

		field.had_quote = field.had_quote || t.had_quote;
		splitOnIfs(field, ifs, out);
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
					if (quoted[i] != 0) appendFnmatchLiteral(pattern, text[i]);
					else pattern.push_back(text[i]);
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
		if (!env_.braceexpand()) return expandWordPostBrace(w);

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
		const std::vector<Tagged> fields = splitWords(renderWord(tilded));
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

	std::string Expander::expandAssignmentValue(const Word& w) {
		TildeScope scope;
		scope.after_colons = true;
		const Word tilded = expandTildePrefixes(env_, w, scope);
		return quoteRemove(renderWord(tilded));
	}

	static std::string taggedAsPattern(const Expander::Tagged& t) {
		std::string pattern;
		pattern.reserve(t.size());
		for (std::size_t i = 0; i < t.size(); ++i) {
			if (quotedAt(t, i)) appendFnmatchLiteral(pattern, t.text[i]);
			else pattern.push_back(t.text[i]);
		}

		return pattern;
	}

	std::string Expander::expandPattern(const Word& w) {
		const Word tilded = applyTildeExpansion(w);
		return taggedAsPattern(renderWord(tilded));
	}

}  // namespace wbsh

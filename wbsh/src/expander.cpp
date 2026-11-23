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
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "diagnostics.h"
#include "fnmatch.h"
#include "globsort.h"
#include "lexer.h"
#include "numparse.h"
#include "procfs.h"
#include "strscan.h"
#include "userhome.h"
#include "executor.h"
#include "utf8.h"

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

	// The F_FIELD_END marker of a `$*` field that a null IFS joins.
	static constexpr char kNoSeparator = '\0';

	static bool isDigit(char c) {
		return std::isdigit(static_cast<unsigned char>(c)) != 0;
	}

	// A character the expression's author escaped (`\$` in `$((...))`)
	// comes through marked with this; one an expansion's quoting escaped,
	// with a backslash.
	static constexpr char kArithQuoted = '\001';

	static bool escapesNext(char c) {
		return c == '\\' || c == kArithQuoted;
	}

	// A key's brackets need not balance: its `]` is the first one that
	// the rest of the expression can follow, so `A[]]=1` sets key `]`.
	// An escaped character hides a bracket.
	static std::size_t assocKeyClose(const std::string& text, std::size_t from) {
		for (std::size_t close = from; close < text.size(); ++close) {
			if (escapesNext(text[close])) {
				++close;
				continue;
			}

			if (text[close] != ']') continue;

			std::size_t next = close + 1;
			while (next < text.size() && (text[next] == ' ' || text[next] == '\t')) ++next;
			if (next >= text.size() || std::strchr("=+-*/%<>&|^!~?:,)", text[next]) != nullptr) {
				return close;
			}
		}

		return text.size();
	}

	static std::size_t indexClose(const std::string& text, std::size_t from) {
		int depth = 1;
		for (std::size_t i = from; i < text.size(); ++i) {
			if (escapesNext(text[i])) ++i;
			else if (text[i] == '[') ++depth;
			else if (text[i] == ']' && --depth == 0) return i;
		}

		return text.size();
	}

	// An associative key loses every escape. An index is expanded as a
	// double-quoted string, so only a backslash before `$`, a backquote,
	// `"` or another backslash goes, and bash's error shows the rest.
	static bool dropsBackslash(const std::string& text, std::size_t i, bool index) {
		if (text[i] != '\\') return false;
		return !index || (i + 1 < text.size() && std::strchr("$`\"\\", text[i + 1]) != nullptr);
	}

	static std::string withoutArithQuoting(const std::string& text, bool index) {
		std::string out;
		for (std::size_t i = 0; i < text.size(); ++i) {
			const bool drops = text[i] == kArithQuoted || dropsBackslash(text, i, index);
			if (drops && i + 1 < text.size()) ++i;
			out.push_back(text[i]);
		}

		return out;
	}

	// Text an expansion still has to work on: an unescaped `$`, backquote
	// or quote.
	static bool hasUnescapedExpansion(const std::string& text) {
		for (std::size_t i = 0; i < text.size(); ++i) {
			if (escapesNext(text[i])) ++i;
			else if (std::strchr("$`'\"", text[i]) != nullptr) return true;
		}

		return false;
	}

	// bash reads the token after a variable before it evaluates the
	// variable, so a stray byte there fails first.
	static bool startsArithToken(char c) {
		return isNameCont(c) || std::strchr("$()=*/%+-<>&|^!~?:,", c) != nullptr;
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

	static const char* const kDynamicSpecials[] = {
		"BASHOPTS", "BASHPID", "BASH_ARGV0", "BASH_COMMAND", "BASH_MONOSECONDS", "EPOCHREALTIME", "EPOCHSECONDS",
		"LINENO", "RANDOM", "SECONDS", "SHELLOPTS", "BASH_SUBSHELL"
	};

	static long long epochMicroseconds() {
		const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
		return std::chrono::duration_cast<std::chrono::microseconds>(since_epoch).count();
	}

	static long long epochSeconds() {
		return epochMicroseconds() / 1000000;
	}

	static long long monotonicSeconds() {
		const auto since_boot = std::chrono::steady_clock::now().time_since_epoch();
		return std::chrono::duration_cast<std::chrono::seconds>(since_boot).count();
	}

	static std::string epochRealtime() {
		const long long micros = epochMicroseconds();
		char text[32];
		std::snprintf(text, sizeof(text), "%lld.%06lld", micros / 1000000, micros % 1000000);
		return text;
	}

	static bool dynamicSpecialValue(Environment& env, const std::string& name, std::string& out) {
		if (name == "RANDOM")  { out = std::to_string(env.randomNext());        return true; }
		if (name == "SECONDS") { out = std::to_string(env.secondsSinceStart()); return true; }
		if (name == "LINENO")  { out = std::to_string(env.currentLineno());     return true; }
		if (name == "BASH_COMMAND") { out = env.currentCommand();             return true; }
		if (name == "EPOCHSECONDS")  { out = std::to_string(epochSeconds()); return true; }
		if (name == "EPOCHREALTIME") { out = epochRealtime();                return true; }
		if (name == "BASH_MONOSECONDS") { out = std::to_string(monotonicSeconds()); return true; }
		if (name == "BASHOPTS")      { out = env.enabledShoptNames();       return true; }
		if (name == "SHELLOPTS")     { out = env.enabledSetOptionNames();   return true; }
		if (name == "BASHPID") { out = std::to_string(env.bashPid());        return true; }
		if (name == "BASH_SUBSHELL") { out = std::to_string(env.subshellLevel()); return true; }
		if (name == "BASH_ARGV0") { out = env.shellName();                   return true; }

		return false;
	}

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

	// The brace closing the parameter expansion whose body starts at
	// @p after_open: only a nested parameter expansion nests, a bare
	// opening brace is an ordinary character (as in bash), and a backslash
	// escapes the character after it.
	static std::size_t scanParamClose(const std::string& body, std::size_t after_open) {
		int depth = 1;
		std::size_t k = after_open;
		while (k < body.size()) {
			if (body[k] == '\\') {
				k += 2;
				continue;
			}

			if (body[k] == '$' && k + 1 < body.size() && body[k + 1] == '{') {
				++depth;
				k += 2;
				continue;
			}

			if (body[k] == '}' && --depth == 0) return k;
			++k;
		}

		return body.size();
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
		static const char* const kInvalidNumber = "invalid number";
		static const char* const kInvalidConstant = "invalid integer constant";
		static const char* const kOperandExpected = "arithmetic syntax error: operand expected";
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

		// bash's strlong reads a constant left to right and stops at the
		// first problem; a leading `0` or `0x` already fixed the base.
		struct ConstantScan {
			unsigned long long value = 0;
			int base = 10;
			bool found_base = false;
		};

		static std::size_t readRadixPrefix(const std::string& text, ConstantScan& scan) {
			if (text.size() < 2 || text[0] != '0') return 0;

			scan.found_base = true;
			if (text[1] == 'x' || text[1] == 'X') {
				scan.base = 16;
				return 2;
			}

			scan.base = 8;
			return 1;
		}

		static const char* takeBase(ConstantScan& scan, char next) {
			if (scan.found_base) return kInvalidNumber;
			if (scan.value < 2 || scan.value > static_cast<unsigned long long>(kMaxBase)) {
				return kInvalidBase;
			}

			scan.base = static_cast<int>(scan.value);
			scan.value = 0;
			scan.found_base = true;
			const bool digit_follows = isAlnumChar(next) || next == '@' || next == '_';
			return digit_follows ? nullptr : kInvalidConstant;
		}

		// Overflow wraps, as in bash.
		static const char* takeDigit(ConstantScan& scan, char c) {
			const int digit = basedDigitValue(c, scan.base);
			if (digit < 0 || digit >= scan.base) return kTooGreatForBase;

			scan.value = scan.value * static_cast<unsigned long long>(scan.base)
				+ static_cast<unsigned long long>(digit);
			return nullptr;
		}

		// Returns the diagnostic for a malformed constant, or nullptr.
		static const char* parseConstant(const std::string& text, long long& value) {
			value = 0;
			ConstantScan scan;
			for (std::size_t k = readRadixPrefix(text, scan); k < text.size(); ++k) {
				const char next = k + 1 < text.size() ? text[k + 1] : '\0';
				const char* problem = text[k] == '#' ? takeBase(scan, next) : takeDigit(scan, text[k]);
				if (problem != nullptr) return problem;
			}

			value = static_cast<long long>(scan.value);
			return nullptr;
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
			std::string raw_subscript;   ///< as written, before its escapes go
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

		static std::string withoutDoubleQuotes(const std::string& text) {
			std::string out;
			for (std::size_t k = 0; k < text.size(); ++k) {
				if (text[k] == '\\' && k + 1 < text.size()) {
					out.push_back(text[k]);
					out.push_back(text[++k]);
				} else if (text[k] != '"') {
					out.push_back(text[k]);
				}
			}

			return out;
		}

		// Not an error in bash: the operand reads as 0 or the assignment
		// is dropped, and evaluation goes on.
		static void warnBadSubscript(const std::string& what) {
			shellErrorf("%s: bad array subscript\n", what.c_str());
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

			// The text was expanded before it got here, as `let` and `[[`
			// operands are: a `$` left in it is not an operand.
			void rejectExpansions() {
				rejects_expansions_ = true;
			}

			// bash expands the subscripts in a variable's value before it
			// evaluates the value; this does that expansion.
			using SubscriptExpander = std::string (*)(void* context, const std::string& text);
			void expandValueSubscripts(SubscriptExpander expander, void* context) {
				subscript_expander_ = expander;
				subscript_context_ = context;
			}

			// `(( a[$k]++ ))` with `a` associative: the key was left as
			// written, for this to expand once into a literal key.
			void expandAssocKeys(SubscriptExpander keys, SubscriptExpander indexes, void* context) {
				key_expander_ = keys;
				index_expander_ = indexes;
				key_context_ = context;
			}

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

			bool readonlyAssignment() const {
				return readonly_;
			}

			// bash reports an error inside an array subscript without the
			// command's own `((:` or `let:` label.
			bool subscriptError() const {
				return subscript_error_;
			}

			// `let` under assoc_expand_once: its operands were expanded
			// as words already, so a `"` left in a subscript is an error.
			void keepSubscriptQuotes() {
				keep_subscript_quotes_ = true;
			}

			// `a[]` assigned: in `$((...))` bash reports it and goes on.
			bool emptySubscriptAssigned() const {
				return empty_subscript_assigned_;
			}

			const std::string& error() const {
				return error_;
			}

		private:
			// The first error wins; jumping to the end makes every parse
			// level unwind without consuming anything further. bash keeps
			// the trailing blanks of the expression and of the token.
			void fail(const std::string& what, std::size_t token_at) {
				failWithin(what, token_at, src_.size());
			}

			// A bad constant cuts the expression and the token off where the
			// constant ends: bash has put a NUL there to convert it.
			void failWithin(const std::string& what, std::size_t token_at, std::size_t end) {
				if (failed()) return;

				const std::string text = src_.substr(0, end);
				error_ = trimLeadingSpace(text) + ": " + what;
				const std::string token = trimLeadingSpace(text.substr(std::min(token_at, text.size())));
				if (!token.empty()) error_ += " (error token is \"" + token + "\")";
				pos_ = src_.size();
			}

			static std::string trimLeadingSpace(const std::string& text) {
				std::size_t start = 0;
				while (start < text.size() && isSpaceChar(text[start])) ++start;
				return text.substr(start);
			}

			// bash names the token it was looking at, or the last one
			// it read when the expression ended.
			std::size_t lookaheadOr(std::size_t fallback) {
				skipWs();
				return eof() ? fallback : pos_;
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
				readonly_ = inner.readonlyAssignment();
				subscript_error_ = inner.subscriptError();
			}

			void failUnbound(const std::string& name) {
				if (failed()) return;

				adoptError(name + ": unbound variable");
				unbound_ = true;
			}

			void failOnLeftover() {
				const char c = peek();
				const std::size_t at = pos_;
				if (matchAssignOp() != nullptr) {
					fail("attempted assignment to non-variable", at);
					return;
				}

				const bool step = (c == '+' || c == '-') && atStepOperand(pos_ + 1, c);
				const bool operand = isAlnumChar(c) || c == '_' || c == '$' || c == '(' || step;
				fail(operand ? "arithmetic syntax error in expression"
					: "arithmetic syntax error: invalid arithmetic operator", pos_);
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

					last_operator_at_ = pos_;
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
				if (resolved) assignSlot(slot, value);
				return value;
			}

			long long parseTernary() {
				const long long condition = parseLogOr();
				if (!consume('?')) return condition;

				failOnMissingBranch();
				const long long when_true = parseOperand(condition == 0, &ArithEval::parseAssign);
				if (!consume(':')) {
					fail("`:' expected for conditional expression", lookaheadOr(last_operand_at_));
				}

				// As in bash the false branch is itself a conditional, so in
				// `1 ? 20 : x+=2` the `+=` assigns to the whole expression.
				failOnMissingBranch();
				const long long when_false = parseOperand(condition != 0, &ArithEval::parseTernary);
				return condition != 0 ? when_true : when_false;
			}

			// `1 ? : 2` and `1 ? 2 :` leave a branch out.
			void failOnMissingBranch() {
				const std::size_t at = lookaheadOr(last_operator_at_);
				if (eof() || peek() == ':') fail("expression expected", at);
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
					if (consumeAdditive("+")) {
						const long long right = parseMul();
						left = left + right;
					} else if (consumeAdditive("-")) {
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

				const long long exponent = parsePow();
				if (exponent < 0) failEvaluation("exponent less than 0", last_operand_at_);
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

			// `--x++`: the result of the prefix step is no variable to step.
			long long prefixStep(long long delta) {
				const VarRef ref = readVarRef();
				skipWs();
				const bool stepped_again = (peek() == '+' || peek() == '-') && peek(1) == peek();
				if (stepped_again) {
					fail(src_.substr(pos_, 2) + ": assignment requires lvalue", pos_);
					return 0;
				}

				Slot slot;
				if (!resolveSlot(ref, /*for_write=*/true, slot)) return 0;

				const long long value = slotValue(slot) + delta;
				assignSlot(slot, value);
				return value;
			}

			long long postfixStep(const VarRef& ref, long long delta) {
				Slot slot;
				if (!resolveSlot(ref, /*for_write=*/true, slot)) return 0;

				const long long value = slotValue(slot);
				assignSlot(slot, value + delta);
				return value;
			}

			long long parsePrimary() {
				skipWs();
				if (consume('(')) {
					const long long value = parseComma();
					if (!consume(')')) fail("missing `)'", eof() ? last_operand_at_ : pos_);
					return value;
				}

				last_operand_at_ = pos_;
				if (isDigit(peek())) return readNumber();
				if (isNameStart(peek())) return parseVariable();
				if (!rejects_expansions_ && consume('$')) return parseDollar();

				fail(kOperandExpected, eof() ? last_operator_at_ : pos_);
				return 0;
			}

			long long parseVariable() {
				const VarRef ref = readVarRef();
				skipWs();
				if (consume("++")) return postfixStep(ref, +1);
				if (consume("--")) return postfixStep(ref, -1);
				if (!eof() && !startsArithToken(peek())) {
					failOnLeftover();
					return 0;
				}

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

				const std::size_t close = env_.isAssocArray(ref.name) ? assocKeyClose(src_, pos_ + 1)
					: indexClose(src_, pos_ + 1);
				if (close >= src_.size()) {
					fail("bad array subscript", start);
					return ref;
				}

				ref.has_subscript = true;
				ref.raw_subscript = src_.substr(pos_ + 1, close - pos_ - 1);
				ref.subscript = withoutArithQuoting(ref.raw_subscript, !env_.isAssocArray(ref.name));
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
				const bool expands = key_expander_ != nullptr && hasUnescapedExpansion(ref.raw_subscript);
				const std::string key = expands ? key_expander_(key_context_, ref.raw_subscript)
					: ref.subscript;
				if (key.empty()) {
					warnBadSubscript(ref.name + "[]");
					return false;
				}

				slot.assoc = true;
				slot.key = key;
				return true;
			}

			// bash reads `a[]` twice, complaining each time, and refuses to
			// assign it.
			bool rejectEmptySubscript(const VarRef& ref, bool for_write) {
				if (for_write) {
					adoptError("`" + ref.name + "[]': not a valid identifier");
					empty_subscript_assigned_ = true;
					return false;
				}

				warnBadSubscript(ref.name + "[]");
				warnBadSubscript(ref.name + "[]");
				return false;
			}

			// The subscript is expanded with quote removal: a["1"] is a[1],
			// and one that comes out blank is a[0]. A negative index counts
			// back from one past the last element.
			// An index that still holds an expansion, as text `$expr`
			// put there does, is expanded once; what that yields is
			// evaluated as it stands.
			std::string indexText(const VarRef& ref) const {
				if (key_expander_ == nullptr || !hasUnescapedExpansion(ref.raw_subscript)) {
					return ref.subscript;
				}

				return index_expander_(key_context_, ref.raw_subscript);
			}

			bool resolveIndex(const VarRef& ref, bool for_write, Slot& slot) {
				if (ref.subscript.empty()) return rejectEmptySubscript(ref, for_write);

				const std::string index_text = keep_subscript_quotes_ ? ref.subscript
					: withoutDoubleQuotes(indexText(ref));
				slot.index = trimSpace(index_text).empty() ? 0 : evaluateNested(index_text, ref.name);
				if (failed()) {
					subscript_error_ = true;
					return false;
				}

				if (slot.index < 0) slot.index += nextIndexAfterLast(env_, ref.name);
				if (slot.index >= 0) return true;

				warnBadSubscript(for_write ? ref.name + "[" + ref.subscript + "]" : ref.name);
				return false;
			}

			// bash abandons the whole expression at a readonly variable.
			void assignSlot(const Slot& slot, long long value) {
				if (env_.isReadonly(slot.name)) {
					adoptError(env_.resolveRef(slot.name) + ": readonly variable");
					readonly_ = !skipping();
					return;
				}

				storeSlot(env_, slot, value);
			}

			// Under `set -u` bash rejects an element only of an array that is
			// unset as a whole: `b=(1); $(( b[3] ))` is 0.
			long long slotValue(const Slot& slot) {
				if (!slot.element) return variableValue(slot.name);
				if (env_.nounset() && !arrayIsSet(slot.name)) {
					failUnbound(slot.name);
					return 0;
				}

				return textValue(elementText(env_, slot), slot.name);
			}

			bool arrayIsSet(const std::string& name) const {
				const Environment::IndexedArray* indexed = env_.getIndexedArray(name);
				if (indexed != nullptr) return !indexed->empty();

				const Environment::AssocArray* assoc = env_.getAssocArray(name);
				if (assoc != nullptr) return !assoc->empty();

				return env_.has(name);
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

				std::string dynamic;
				if (dynamicSpecialValue(env_, name, dynamic)) return textValue(dynamic, name);
				return textValue(env_.get(name), name);
			}

			// A variable's text is an expression in its own right (`x=2+3`).
			long long textValue(const std::string& text, const std::string& name) {
				if (text.empty()) return 0;

				long long value = 0;
				std::size_t consumed = 0;
				if (parseLL(text, value, 0, &consumed) && consumed == text.size()) return value;
				if (subscript_expander_ == nullptr) return evaluateNested(text, name);

				// Each subscript in it is expanded as the evaluation reaches
				// it, and what that yields stays one subscript.
				if (key_expander_ != nullptr && !env_.shopts().array_expand_once) {
					return evaluateNested(text, name);
				}

				return evaluateNested(subscript_expander_(subscript_context_, text), name);
			}

			long long evaluateNested(const std::string& text, const std::string& name) {
				if (depth_ > kArithNestingLimit) {
					adoptError(name + ": expression recursion level exceeded (error token is \""
						+ name + "\")");
					return 0;
				}

				ArithEval inner(text, env_, depth_ + 1);
				inner.rejectExpansions();
				inner.expandValueSubscripts(subscript_expander_, subscript_context_);
				inner.expandAssocKeys(key_expander_, index_expander_, key_context_);
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
				if (problem != nullptr) failWithin(problem, start, pos_);
				return value;
			}

			std::string readIdent() {
				skipWs();
				const std::size_t start = pos_;
				if (!eof() && isNameStart(peek())) pos_ = scanIdentifierEnd(src_, pos_);
				return src_.substr(start, pos_ - start);
			}

			// The shell never sets a CRT locale, so this is isspace() without
			// its per-call locale lookup, on the hottest path of arithmetic.
			static bool isBlank(char c) {
				return c == ' ' || (c >= '\t' && c <= '\r');
			}

			void skipWs() {
				while (!eof() && isBlank(peek())) ++pos_;
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
			// bash's tokenizer reads `++` after an operand as `+ +` unless a
			// name follows to make it an increment.
			bool consumeAdditive(const char* op) {
				if (!consumeOperator(op, "=")) return false;
				if (!atStepOperand(pos_, op[0])) return true;

				pos_ = last_operator_at_;
				return false;
			}

			bool atStepOperand(std::size_t at, char op) const {
				if (at >= src_.size() || src_[at] != op) return false;

				std::size_t k = at + 1;
				while (k < src_.size() && isSpaceChar(src_[k])) ++k;
				return k < src_.size() && isNameStart(src_[k]);
			}

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
			std::size_t last_operand_at_ = 0;
			std::string error_;
			bool unbound_ = false;
			bool readonly_ = false;
			bool subscript_error_ = false;
			bool empty_subscript_assigned_ = false;
			bool keep_subscript_quotes_ = false;
			bool rejects_expansions_ = false;
			SubscriptExpander subscript_expander_ = nullptr;
			void* subscript_context_ = nullptr;
			SubscriptExpander key_expander_ = nullptr;
			SubscriptExpander index_expander_ = nullptr;
			void* key_context_ = nullptr;
			int skip_depth_ = 0;
			Environment& env_;
			int depth_;
		};

	}  // namespace arith_detail

	Expander::Expander(Environment& env, CommandSubstitutor* sub)
		: env_(env), sub_(sub) {}

	long long Expander::evalArith(const std::string& body) {
		const std::string expanded = expandArithmeticText(body);
		if (aborting()) return 0;

		if (env_.xtrace()) last_arith_text_ = expanded;
		arith_detail::ArithEval evaluator(expanded, env_, 0);
		evaluator.expandValueSubscripts(valueSubscriptsExpanded, this);
		evaluator.expandAssocKeys(assocKeyExpanded, indexExpanded, this);
		evaluator.rejectExpansions();
		return finishArith(evaluator);
	}

	// `let` and `[[` evaluate words the shell has expanded already; as in
	// bash only their array subscripts are expanded again, and not even
	// those under array_expand_once.
	long long Expander::evalArithExpanded(const std::string& text) {
		const std::string expanded = env_.shopts().array_expand_once ? text
			: expandArithmeticSubscripts(text);
		if (aborting()) return 0;

		arith_detail::ArithEval evaluator(expanded, env_, 0);
		evaluator.expandValueSubscripts(valueSubscriptsExpanded, this);
		evaluator.rejectExpansions();
		if (env_.shopts().array_expand_once) evaluator.keepSubscriptQuotes();
		else evaluator.expandAssocKeys(assocKeyExpanded, indexExpanded, this);
		return finishArith(evaluator);
	}

	std::string Expander::assocKeyExpanded(void* self, const std::string& text) {
		return static_cast<Expander*>(self)->resolveSubscript(text);
	}

	std::string Expander::indexExpanded(void* self, const std::string& text) {
		return static_cast<Expander*>(self)->expandArithmeticText(text);
	}

	std::string Expander::valueSubscriptsExpanded(void* self, const std::string& text) {
		Expander& expander = *static_cast<Expander*>(self);
		if (expander.env_.shopts().array_expand_once) return text;

		return expander.expandArithmeticSubscripts(text);
	}

	long long Expander::evalArithLiteral(const std::string& text) {
		arith_detail::ArithEval evaluator(text, env_, 0);
		evaluator.expandValueSubscripts(valueSubscriptsExpanded, this);
		evaluator.rejectExpansions();
		return finishArith(evaluator);
	}

	std::string Expander::expandArithmeticSubscripts(const std::string& text) {
		std::string out;
		std::size_t k = 0;
		while (k < text.size() && !aborting()) {
			const std::size_t open = text.find('[', k);
			if (open == std::string::npos) break;

			const std::size_t close = scanBalanced(text, open + 1, '[', ']', 1);
			if (close >= text.size()) break;

			out += text.substr(k, open + 1 - k);
			out += expandArithmeticText(text.substr(open + 1, close - open - 1), true);
			out.push_back(']');
			k = close + 1;
		}

		return out + text.substr(std::min(k, text.size()));
	}

	long long Expander::finishArith(arith_detail::ArithEval& evaluator) {
		const long long value = evaluator.run();
		arith_soft_error_ = evaluator.emptySubscriptAssigned();
		arith_subscript_error_ = evaluator.subscriptError();
		if (evaluator.unbound()) failFatal(evaluator.error());
		else if (evaluator.readonlyAssignment() || evaluator.subscriptError()) {
			failUnlabeled(evaluator.error());
		}
		else if (evaluator.failed()) fail(evaluator.error());
		return value;
	}

	long long Expander::evalArithExpansion(const std::string& body) {
		const long long value = evalArith(Lexer::withoutArithmeticDoubleQuotes(body));
		if (!failed() || !std::exchange(arith_soft_error_, false)) return value;

		shellErrorf("%s\n", takeError().c_str());
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

	std::string Expander::expandHeredocBody(const std::string& body, bool quoted) {
		const bool outer = std::exchange(heredoc_body_, true);
		std::string expanded = expandHeredoc(body, quoted);
		heredoc_body_ = outer;
		return expanded;
	}

	// bash parses such a `$(...)` only now, and its syntax error discards
	// the command. Its line number counts the newlines of the text from
	// the command's line, one more when the `)` is what is missing and one
	// more again outside a here-document.
	void Expander::failUnterminatedSubstitution(const std::string& text) {
		Lexer lexer("$(" + text);
		lexer.tokenize();
		const std::string message = lexer.errors().empty()
			? "unexpected EOF while looking for matching `)'"
			: lexer.errors().front().message;

		const bool missing_paren = message.find("`)'") != std::string::npos;
		const auto newlines = std::count(text.begin(), text.end(), '\n');
		const int lineno = env_.currentLineno();
		const std::size_t past_end = (missing_paren ? 1 : 0) + (heredoc_body_ ? 0 : 1);
		const std::size_t line = static_cast<std::size_t>(lineno > 0 ? lineno : 1)
			+ static_cast<std::size_t>(newlines) + past_end;
		failLocated(syntaxErrorPrefix("command substitution", line) + message);
	}

	void Expander::expandHeredocCmdSubst(const std::string& body, std::size_t& i,
			std::string& out) {
		const std::size_t end = body.size();
		const std::size_t close = Lexer::commandSubstitutionEnd(body, i + 2);
		if (close == std::string::npos) {
			failUnterminatedSubstitution(body.substr(i + 2));
			i = end;
			return;
		}

		const std::size_t k = std::min(close, end);
		const std::string inner = body.substr(i + 2, k - (i + 2));

		std::string output = runCmdSubst(inner);
		stripTrailingNewlines(output);
		out += output;
		i = (k < end) ? k + 1 : k;
	}

	bool Expander::expandHeredocArith(const std::string& body, std::size_t& i,
			std::string& out) {
		if (i + 2 >= body.size() || body[i + 2] != '(') return false;

		const std::size_t k = Lexer::arithmeticEnd(body, i + 3);
		if (k == std::string::npos) return false;

		out += std::to_string(evalArith(body.substr(i + 3, k - (i + 3))));
		i = k + 2;
		return true;
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
		const std::size_t k = scanParamClose(body, i + 2);
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
			if (!expandHeredocArith(body, i, out)) expandHeredocCmdSubst(body, i, out);
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

		const bool outer = in_heredoc_;
		in_heredoc_ = true;
		std::string out = expandInterpolated(body, heredocEscapes);
		in_heredoc_ = outer;
		return out;
	}

	// As bash's promptvars: the decoded prompt is expanded as a
	// double-quoted string, where a backslash also quotes `"`.
	std::string Expander::expandPromptText(const std::string& text) {
		return expandInterpolated(text, doubleQuoteEscapes);
	}

	std::string Expander::expandInterpolated(const std::string& body, bool (*escapes)(char)) {
		std::string out;
		const std::size_t end = body.size();
		std::size_t i = 0;
		while (i < end && !aborting()) {
			const char c = body[i];
			if (c == '\\' && i + 1 < end) {
				expandBackslash(body, i, escapes, out);
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

	// bash expands an arithmetic expression as a double-quoted string:
	// `\"` is a quote, which the expression then rejects unless it is
	// part of a subscript.
	// What an expansion puts into a subscript of an arithmetic expression
	// gets these characters escaped, as bash's Q_ARITH quoting does, so a
	// key such as `x],b[$(...)` stays one subscript and is not read as
	// syntax.
	static void appendArithQuoted(std::string& out, const std::string& expanded,
			char mark = '\\') {
		for (const char c : expanded) {
			if (std::strchr("\\\"'`[]$~", c) != nullptr) out.push_back(mark);
			out.push_back(c);
		}
	}

	// An associative array's subscript is copied as written: the
	// arithmetic expands its key once, as bash does, so `a['$k']` is the
	// key `$k`. Only a name the expression itself spells counts.
	bool Expander::copiesAssocSubscript(const std::string& body, std::size_t& i,
			std::string& out) const {
		std::size_t start = out.size();
		while (start > 0 && isNameCont(out[start - 1])) --start;
		if (start == out.size() || !isNameStart(out[start])) return false;
		if (start > 0 && std::strchr("'\"\\", out[start - 1]) != nullptr) return false;
		if (!env_.isAssocArray(out.substr(start))) return false;

		const std::size_t close = subscriptEnd(body, i, true);
		if (close == std::string::npos) return false;

		out.append(body, i, close + 1 - i);
		i = close + 1;
		return true;
	}

	std::string Expander::expandArithmeticText(const std::string& body, bool in_subscript) {
		const bool outer = in_heredoc_;
		in_heredoc_ = true;
		std::string out;
		int bracket_depth = in_subscript ? 1 : 0;
		std::size_t i = 0;
		while (i < body.size() && !aborting()) {
			const char c = body[i];
			if (c == '\\' && i + 1 < body.size()) {
				std::string escaped;
				expandBackslash(body, i, doubleQuoteEscapes, escaped);
				if (bracket_depth > 0) appendArithQuoted(out, escaped, kArithQuoted);
				else out += escaped;
				continue;
			}

			if (c == '[' && bracket_depth == 0 && copiesAssocSubscript(body, i, out)) continue;

			const bool expands = (c == '$' && i + 1 < body.size()) || c == '`';
			if (!expands) {
				if (c == '[') ++bracket_depth;
				else if (c == ']' && bracket_depth > 0) --bracket_depth;
				out.push_back(body[i++]);
				continue;
			}

			std::string expanded;
			if (c == '$') expandHeredocDollar(body, i, expanded);
			else expandHeredocBackquote(body, i, expanded);
			if (bracket_depth > 0) appendArithQuoted(out, expanded);
			else out += expanded;
		}

		in_heredoc_ = outer;
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
	// Unquoted, a backslash escapes any character, as in bash (`${u:-\z}` is
	// `z`). Where nothing records the quoting of the result, the glob
	// characters keep their backslash rather than be globbed.
	static bool unquotedWordEscapes(char c) {
		return c != '*' && c != '?' && c != '[' && c != ']';
	}

	static bool escapesAnything(char) {
		return true;
	}

	// A ${...} inside an unquoted word: its own quoting carries over.
	void Expander::expandNestedParamQuoted(const std::string& body, std::size_t& i,
			std::string& out, WordQuoting& quoting) {
		const std::size_t body_end = body.size();
		const std::size_t k = scanParamClose(body, i + 2);
		const std::string inner = body.substr(i + 2, k - (i + 2));
		i = (k < body_end) ? k + 1 : k;

		WordQuoting nested;
		const std::size_t base = out.size();
		const std::string value = expandParam(inner, false, &nested);
		out += value;
		if (nested.set && nested.quoted.size() == value.size()) {
			quoting.quoted.insert(quoting.quoted.end(), nested.quoted.begin(), nested.quoted.end());
		}

		for (const std::size_t at : nested.nulls) quoting.nulls.push_back(base + at);

		quoting.quoted.resize(out.size(), 0);
		quoting.had_quote = quoting.had_quote || nested.had_quote;
	}

	// Inside "${name:+word}" a nested "..." pair switches back to unquoted
	// rules (`"${v:+"\p"}"` is `p`), so a backslash follows double-quote
	// rules when exactly one of the two quotings applies.
	static bool quotedParamWordEscapes(char c) {
		return doubleQuoteEscapes(c) || c == '}';
	}

	Expander::EscapeTest Expander::wordArgEscapes(bool records_quoting, bool in_dq) const {
		if (param_quoted_ && !in_dq) return quotedParamWordEscapes;
		if (param_quoted_ != in_dq) return doubleQuoteEscapes;
		if (records_quoting || param_quoted_) return escapesAnything;
		return unquotedWordEscapes;
	}

	// One piece of a word argument; true when the piece is quoted text.
	bool Expander::expandParamWordStep(const std::string& body, std::size_t& i, bool& in_dq,
			WordQuoting* quoting, std::string& out) {
		const char c = body[i];
		if (c == '\'' && !param_quoted_ && !in_dq) {
			copySingleQuotedRun(body, i, out);
			return true;
		}

		if (c == '"') {
			in_dq = !in_dq;
			++i;
			return true;
		}

		const bool more = i + 1 < body.size();
		if (c == '\\' && more) {
			expandBackslash(body, i, wordArgEscapes(quoting != nullptr, in_dq), out);
			return true;
		}

		if (c == '$' && more && body[i + 1] == '{' && quoting != nullptr && !in_dq) {
			expandNestedParamQuoted(body, i, out, *quoting);
			return false;
		}

		if (c == '$' && more && body[i + 1] == '\'' && !in_dq && !in_heredoc_) {
			out += takeAnsiCQuoted(body, i);
			return true;
		}

		if ((c == '<' || c == '>') && more && body[i + 1] == '(' && !in_dq && !param_quoted_) {
			expandParamProcSubst(body, i, out);
			return true;
		}

		if (c == '$' && more) expandHeredocDollar(body, i, out);
		else if (c == '`') expandHeredocBackquote(body, i, out);
		else out.push_back(body[i++]);
		return in_dq;
	}

	static std::size_t skipNested(const std::string& text, std::size_t i);

	void Expander::expandParamProcSubst(const std::string& body, std::size_t& i, std::string& out) {
		const char direction = body[i];
		const std::size_t close = skipNested(body, i);
		const std::string inner = body.substr(i + 2, close - i - 2);
		i = std::min(close + 1, body.size());
		if (sub_ != nullptr) out += sub_->startProcessSubstitution(inner, direction);
	}

	// Index of the quote closing the `$'...'` at body[dollar], or the end.
	static std::size_t ansiCQuoteClose(const std::string& body, std::size_t dollar) {
		std::size_t end = dollar + 2;
		while (end < body.size() && body[end] != '\'') end += body[end] == '\\' ? 2 : 1;
		return std::min(end, body.size());
	}

	// The `$'...'` at body[i], decoded; even inside double quotes bash
	// decodes one in the word or pattern of a ${...}.
	std::string Expander::takeAnsiCQuoted(const std::string& body, std::size_t& i) {
		const std::size_t end = ansiCQuoteClose(body, i);
		const std::string text = body.substr(i + 2, end - i - 2);
		i = std::min(end + 1, body.size());
		return interpretAnsiC(text);
	}

	static bool leadingTildeExpansion(const Environment& env, const PathConv& paths,
	                                  const std::string& body, std::size_t& i, std::string& out);

	// An unquoted `${name:-word}` expands a tilde prefix at the start of
	// its word; the home directory is not split or globbed.
	std::string Expander::expandParamWordArg(const std::string& body, WordQuoting* quoting) {
		std::string out;
		bool in_dq = false;
		std::size_t i = 0;
		const bool tilde = !param_quoted_ && leadingTildeExpansion(env_, path_conv_, body, i, out);
		if (tilde && quoting != nullptr) quoting->quoted.resize(out.size(), 1);

		std::size_t dq_open_at = 0;
		while (i < body.size() && !aborting()) {
			const std::size_t before = out.size();
			const bool was_dq = in_dq;
			const bool single_run = startsSingleQuotedRun(body, i, in_dq);
			const bool quoted = expandParamWordStep(body, i, in_dq, quoting, out);
			if (quoting == nullptr) continue;

			quoting->quoted.resize(out.size(), quoted ? 1 : 0);
			quoting->had_quote = quoting->had_quote || quoted;
			if (!was_dq && in_dq) dq_open_at = out.size();

			const bool empty_dq = was_dq && !in_dq && out.size() == dq_open_at;
			const bool empty_single = single_run && out.size() == before;
			if (empty_dq || empty_single) quoting->nulls.push_back(out.size());
		}

		if (quoting != nullptr) quoting->set = true;
		return out;
	}

	bool Expander::startsSingleQuotedRun(const std::string& body, std::size_t i, bool in_dq) const {
		if (in_dq) return false;
		if (body[i] == '\'' && !param_quoted_) return true;
		return body[i] == '$' && i + 1 < body.size() && body[i + 1] == '\'' && !in_heredoc_;
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
			const bool localized = c == '$' && !in_dq && body.compare(i, 2, "$\"") == 0;
			if (localized) ++i;
			if (c == '"' || localized) {
				in_dq = !in_dq;
				++i;
				continue;
			}

			if (c == '\'' && !in_dq) {
				appendPatternSingleQuoted(body, i, out);
				continue;
			}

			if (c == '$' && i + 1 < body.size() && body[i + 1] == '\'' && !in_dq) {
				appendFnmatchLiteral(out, takeAnsiCQuoted(body, i));
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

	// A shell string cannot hold a NUL byte, so bash drops them from the
	// output, with a warning; a variable then has the same length in both.
	std::string Expander::runCmdSubst(const std::string& body) {
		if (sub_ == nullptr) return {};

		std::string output = sub_->run(body);
		const auto nul = std::remove(output.begin(), output.end(), '\0');
		if (nul == output.end()) return output;

		output.erase(nul, output.end());
		shellErrorf("warning: command substitution: ignored null byte in input\n");
		return output;
	}

	static bool isNoForkSubstBody(const std::string& body) {
		const char first = body[0];
		return first == ' ' || first == '\t' || first == '\n' || first == '|';
	}

	std::string Expander::runNoForkSubst(const std::string& body) {
		if (sub_ == nullptr) return {};
		if (body[0] == '|') return sub_->runForReply(body.substr(1));

		return sub_->runInCurrentShell(body);
	}

	// bash 5.3's `\x{HHH}` takes any number of digits and keeps the low
	// byte; the closing brace is optional.
	static void ansicBracedHexEscape(const std::string& body, std::size_t& i, std::string& out) {
		unsigned value = 0;
		while (i < body.size() && std::isxdigit(static_cast<unsigned char>(body[i]))) {
			value = value * 16 + static_cast<unsigned>(hexDigitValue(body[i]));
			++i;
		}

		if (i < body.size() && body[i] == '}') ++i;
		out.push_back(static_cast<char>(value & 0xff));
	}

	// @p i is at the backslash; `\x` without a hex digit stays literal.
	static bool ansicHexEscape(const std::string& body, std::size_t& i, std::string& out) {
		std::size_t at = i + 2;
		if (at < body.size() && body[at] == '{') {
			i = at + 1;
			ansicBracedHexEscape(body, i, out);
			return true;
		}

		int value = 0;
		int digits = 0;
		while (digits < kAnsicHexDigits && at < body.size()
				&& std::isxdigit(static_cast<unsigned char>(body[at]))) {
			value = value * 16 + hexDigitValue(body[at]);
			++at;
			++digits;
		}

		if (digits == 0) return false;

		out.push_back(static_cast<char>(value));
		i = at;
		return true;
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

	// `\cX` is the control character of X (`\c?` is DEL, and `\c\\` is
	// FS); a trailing `\c` with nothing after it is kept as a literal
	// backslash.
	static void ansicControlEscape(const std::string& body, std::size_t& i, std::string& out) {
		if (i + 2 < body.size()) {
			const char target = body[i + 2];
			const bool doubled = target == '\\' && i + 3 < body.size() && body[i + 3] == '\\';
			out.push_back(target == '?' ? '\x7f' : static_cast<char>(target & 0x1f));
			i += doubled ? 4 : 3;
			return;
		}

		out.push_back('\\');
		++i;
	}

	static bool ansicUnicodeEscape(const std::string& body, std::size_t& i, std::string& out,
			bool utf8_locale) {
		std::size_t at = i + 2;
		char32_t code = 0;
		if (!readUnicodeEscape(body, at, body[i + 1], code)) return false;

		appendUnicodeEscape(out, code, utf8_locale);
		i = at;
		return true;
	}

	// `\x`, octal, `\c`, `\u` and `\U` with @p i at the backslash; false
	// when the escape is none of them.
	static bool ansicCodedEscape(const std::string& body, std::size_t& i, std::string& out,
			bool utf8_locale) {
		const char escape = body[i + 1];
		if (escape == 'x') return ansicHexEscape(body, i, out);

		if (escape >= '0' && escape <= '7') {
			++i;
			ansicOctalEscape(body, i, out);
			return true;
		}

		if (escape == 'c') {
			ansicControlEscape(body, i, out);
			return true;
		}

		if (escape == 'u' || escape == 'U') return ansicUnicodeEscape(body, i, out, utf8_locale);
		return false;
	}

	// bash keeps `$'...'` as a C string: a NUL ends it.
	static std::string truncatedAtNul(std::string text) {
		const std::size_t nul = text.find('\0');
		if (nul != std::string::npos) text.resize(nul);
		return text;
	}

	std::string Expander::interpretAnsiC(const std::string& body) {
		return decodeAnsiC(body, env_.utf8Locale());
	}

	std::string decodeAnsiC(const std::string& body, bool utf8) {
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

			if (ansicCodedEscape(body, i, out, utf8)) continue;

			out.push_back('\\');
			out.push_back(escape);
			i += 2;
		}

		return truncatedAtNul(std::move(out));
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
		const PathConv& paths;
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
	// ~N, ~+N and ~-N name directory-stack entries, never users.
	static bool isDirStackForm(const std::string& name) {
		const std::size_t digits = name[0] == '+' || name[0] == '-' ? 1 : 0;
		return digits < name.size()
			&& name.find_first_not_of("0123456789", digits) == std::string::npos;
	}

	// `dirs +N` and `dirs -N`: counted from the current directory, or from
	// the bottom of the stack; past either end the word stays as it is.
	static bool dirStackReplacement(const Environment& env, const std::string& name,
	                                std::string& out) {
		const Environment::IndexedArray* stack = env.getIndexedArray("DIRSTACK");
		const bool from_bottom = name[0] == '-';
		const std::size_t digits = name[0] == '+' || from_bottom ? 1 : 0;
		unsigned long long n = 0;
		if (stack == nullptr || !parseULL(name.substr(digits), n) || n >= stack->size()) return false;

		auto entry = stack->begin();
		std::advance(entry, static_cast<std::ptrdiff_t>(from_bottom ? stack->size() - 1 - n : n));
		out = entry->second;
		return true;
	}

	static bool userTildeReplacement(const Environment& env, const PathConv& paths,
	                                 const std::string& name, std::string& out) {
		if (isDirStackForm(name)) return dirStackReplacement(env, name, out);

		out = userHomeDirectory(paths, name);
		return !out.empty();
	}

	static bool tildeReplacement(const Environment& env, const PathConv& paths,
	                             const std::string& name, std::string& out) {
		const char* variable = nullptr;
		if (name.empty())    variable = "HOME";
		else if (name == "+") variable = "PWD";
		else if (name == "-") variable = "OLDPWD";
		if (variable == nullptr) return userTildeReplacement(env, paths, name, out);
		if (!env.has(variable)) return false;

		out = env.get(variable);
		return true;
	}

	static bool leadingTildeExpansion(const Environment& env, const PathConv& paths,
	                                  const std::string& body, std::size_t& i, std::string& out) {
		if (body.empty() || body[0] != '~') return false;

		const std::size_t end = std::min(body.find_first_of("/:", 1), body.size());
		const std::string name = body.substr(1, end - 1);
		if (name.find_first_of("'\"\\$`") != std::string::npos) return false;
		if (!tildeReplacement(env, paths, name, out)) return false;

		i = end;
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
		if (!tildeReplacement(walk.env, walk.paths, name, replacement.text)) return false;

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

	static Word expandTildePrefixes(const Environment& env, const PathConv& paths, const Word& w,
	                                TildeScope scope) {
		if (!hasLiteralTilde(w)) return w;

		TildeWalk walk{env, paths, scope.after_colons};
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

	// POSIX mode expands only a leading tilde in a word that merely looks
	// like an assignment (`echo a=~`, `export a=~`).
	Word Expander::applyTildeExpansion(const Word& w) {
		TildeScope scope;
		if (!env_.options().posix) scope.start = assignmentWordValueStart(w);
		scope.after_colons = scope.start != 0;
		return expandTildePrefixes(env_, path_conv_, w, scope);
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

	// `$!` is unset until the shell starts a background job.
	std::string Expander::lastBackgroundPid() const {
		const long long pid = env_.lastBgPid();
		return pid != 0 ? std::to_string(pid) : std::string();
	}

	bool Expander::lookupSpecialChar(char c, std::string& out) {
		switch (c) {
		case '?': out = std::to_string(env_.lastStatus());            return true;
		case '$': out = std::to_string(env_.shellPid());              return true;
		case '!': out = lastBackgroundPid();                          return true;
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
		return dynamicSpecialValue(env_, name, out);
	}

	bool Expander::isParamSet(const std::string& name) const {
		if (isDigit(name[0])) return !isUnsetPositional(name);
		if (name == "!") return env_.lastBgPid() != 0;
		if (name.size() == 1 && isSpecialParam1(name[0])) return true;
		return isVariableSet(name) && !lacksElementZero(name);
	}

	bool Expander::isUnsetPositional(const std::string& name) const {
		if (name.empty() || scanDigitsEnd(name, 0) != name.size()) return false;

		unsigned long index = 0;
		if (!parseUL(name, index)) return true;
		return index > env_.positional().size();
	}

	// A positional parameter past `$#`, or `$!` before any background job.
	bool Expander::isUnboundSpecial(const std::string& name) const {
		if (name == "!") return !isParamSet(name);
		return isUnsetPositional(name);
	}

	// A nameref without a target takes an assigned value as its target.
	bool Expander::isUnboundNameref(const std::string& name) const {
		return env_.isNameref(name) && env_.namerefTarget(name).empty();
	}

	// A nameref to `a[@]` reads the whole array, which set -u lets be empty.
	bool Expander::refersToWholeArray(const std::string& name) const {
		if (!env_.isNameref(name)) return false;

		const std::string& target = env_.resolveRef(name);
		const std::size_t size = target.size();
		return size > 3 && target[size - 1] == ']' && target[size - 3] == '['
			&& (target[size - 2] == '@' || target[size - 2] == '*');
	}

	std::string Expander::lookupParam(const std::string& name, bool suppress_nounset,
			const std::string& label) {
		if (name.empty()) return {};

		const std::string& subject = label.empty() ? name : label;
		if (env_.nounset() && !suppress_nounset && isUnboundSpecial(name)) {
			failFatal(subject + ": unbound variable");
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
		if (env_.isNameref(name) && env_.isCyclicRef(name)) {
			shellErrorf("warning: %s: circular name reference\n", name.c_str());
			std::string global;
			env_.readThroughCycle(name, global);
			return global;
		}

		// Only set -u needs to know whether it is set; a nameref's subscript
		// is evaluated once either way.
		if (!env_.nounset() || suppress_nounset) return env_.get(name);

		const bool unbound = !env_.has(name) || lacksElementZero(name);
		if (!unbound) return env_.get(name);
		const bool exempt = suppress_nounset || refersToWholeArray(name);
		if (env_.nounset() && !exempt) failFatal(subject + ": unbound variable");
		return {};
	}

	// `$a` names element 0 of an array, which may be unset on its own.
	bool Expander::lacksElementZero(const std::string& name) const {
		const Environment::IndexedArray* indexed = env_.getIndexedArray(name);
		if (indexed != nullptr) return indexed->find(0) == indexed->end();

		const Environment::AssocArray* assoc = env_.getAssocArray(name);
		return assoc != nullptr && assoc->find("0") == assoc->end();
	}

	std::string Expander::starSeparator(bool star_join_ifs) const {
		if (!star_join_ifs) return " ";

		const std::string ifs = env_.ifs();
		if (ifs.empty()) return std::string();

		const std::size_t length = env_.utf8Locale() ? utf8CharLength(ifs, 0) : 1;
		return ifs.substr(0, length);
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
	static bool lexSingleWord(const std::string& text, Word& out);

	// bash expands a key as a word without tilde expansion, splitting or
	// globbing: `${a["0"]}` is key 0.
	std::string Expander::resolveSubscript(const std::string& subscript) {
		if (subscript == "@" || subscript == "*") return subscript;
		if (subscript.find_first_of("$`'\"\\") == std::string::npos) return subscript;

		Word word;
		if (!lexSingleWord(subscript, word)) return expandHeredoc(subscript, false);
		return quoteRemove(renderWord(word));
	}

	// A negative subscript counts back from the highest index in use. An
	// error in the subscript stays pending: as in bash, it fails the
	// expansion (and drops the rest of the line).
	// bash warns about an index before the first element and reads it as
	// empty.
	static std::string indexedArrayElement(Expander& expander,
			const Environment::IndexedArray& items, const std::string& name,
			const std::string& subscript, Expander::ElementSlot& slot) {
		slot.index = expander.evalArith(subscript);
		if (expander.failed()) return {};
		if (slot.index < 0 && !items.empty()) slot.index += items.rbegin()->first + 1;
		if (slot.index < 0) {
			arith_detail::warnBadSubscript(name);
			return {};
		}

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

	// An index is expanded once, by the arithmetic that evaluates it, its
	// double quotes dropped as in `(( ))`; only an associative key is
	// expanded on its own.
	std::string Expander::lookupElement(const std::string& name, const std::string& subscript,
			ElementSlot& slot) {
		if (const auto* assoc = env_.getAssocArray(name); assoc != nullptr) {
			return assocArrayElement(*assoc, resolveSubscript(subscript), slot);
		}

		const std::string index_text = Lexer::withoutArithmeticDoubleQuotes(subscript);
		if (const auto* indexed = env_.getIndexedArray(name); indexed != nullptr) {
			return indexedArrayElement(*this, *indexed, name, index_text, slot);
		}

		slot.index = evalArith(index_text);
		if (failed()) return {};
		if (slot.index < 0) {
			arith_detail::warnBadSubscript(name);
			return {};
		}

		slot.resolved = true;
		slot.is_set = slot.index == 0 && env_.has(name);
		return slot.is_set ? lookupParam(name) : std::string();
	}

	std::string Expander::lookupSubscripted(const std::string& name,
			const std::string& subscript_in, bool star_join_ifs, ElementSlot* slot) {
		ElementSlot local;
		ElementSlot& out = (slot != nullptr) ? *slot : local;
		const bool whole_array = (subscript_in == "@" || subscript_in == "*");
		if (!whole_array) return lookupElement(name, subscript_in, out);

		const std::string sep = (subscript_in == "*") ? starSeparator(star_join_ifs)
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
			return std::to_string(utf8Count(lookupParam(rest)));
		}

		if (subscript == "@" || subscript == "*") return std::to_string(arrayLength(name));

		// bash answers 0 for a variable with nothing in it before it looks
		// at the subscript.
		const bool is_array = env_.isIndexedArray(name) || env_.isAssocArray(name);
		const bool holds_nothing = is_array ? arrayLength(name) == 0 : !env_.has(name);
		if (holds_nothing && !env_.nounset()) return "0";

		if (env_.isIndexedArray(name)) return indexedElementLength(name, subscript);

		ElementSlot slot;
		std::string element;
		if (const Environment::AssocArray* assoc = env_.getAssocArray(name)) {
			const std::string key = resolveSubscript(subscript);
			if (key.empty()) {
				fail("[" + subscript + "]: bad array subscript");
				return {};
			}

			element = assocArrayElement(*assoc, key, slot);
		} else {
			element = lookupSubscripted(name, subscript, false, &slot);
		}

		if (env_.nounset() && !slot.is_set) failFatal(name + "[" + subscript + "]: unbound variable");
		return std::to_string(utf8Count(element));
	}

	// An index before the first element is an error here, not a warning.
	std::string Expander::indexedElementLength(const std::string& name,
			const std::string& subscript) {
		const Environment::IndexedArray& items = *env_.getIndexedArray(name);
		long long index = evalArith(Lexer::withoutArithmeticDoubleQuotes(subscript));
		if (failed()) return {};
		if (index < 0 && !items.empty()) index += items.rbegin()->first + 1;
		if (index < 0) {
			fail("[" + subscript + "]: bad array subscript");
			return {};
		}

		const auto it = items.find(index);
		if (it == items.end() && env_.nounset() && items.empty()) {
			failFatal(name + "[" + subscript + "]: unbound variable");
			return {};
		}

		return std::to_string(it == items.end() ? 0 : utf8Count(it->second));
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

	static std::string missingParameterMessage(bool colon) {
		return colon ? "parameter null or not set" : "parameter not set";
	}

	static bool isDefaultOp(char op) {
		return op == '-' || op == '+' || op == '=' || op == '?';
	}

	static void dropAssignedQuotedNulls(const std::string& word, std::string& value);

	std::string Expander::evalParamDefaultOp(char op, bool colon, const ParamOpCtx& ctx,
			const std::string& arg) {
		const std::string& cur = ctx.named_value;
		const bool unset = ctx.has_subscript ? !ctx.slot.is_set : !isParamSet(ctx.name);
		const bool empty_or_unset = colon ? (unset || cur.empty()) : unset;

		switch (op) {
		case '-':
			return empty_or_unset ? expandParamWordArg(arg, param_quoting_) : cur;
		case '+':
			return empty_or_unset ? std::string() : expandParamWordArg(arg, param_quoting_);
		case '=': {
			if (!empty_or_unset) return cur;

			// Quote removal comes before the assignment; the result is the
			// value as stored, after -i or -u had their say, and not quoted.
			WordQuoting removed;
			const bool outer = std::exchange(assignment_value_, true);
			std::string value = expandParamWordArg(arg, param_quoted_ ? nullptr : &removed);
			assignment_value_ = outer;
			if (!param_quoted_) dropAssignedQuotedNulls(arg, value);
			const bool targets_ref = isUnboundNameref(ctx.name);
			assignParamDefault(ctx, value);
			if (failed()) return {};
			if (ctx.has_subscript) return lookupSubscripted(ctx.name, ctx.subscript, false, nullptr);
			return targets_ref ? value : lookupParam(ctx.name, true);
		}
		case '?': {
			if (!empty_or_unset) return cur;

			WordQuoting removed;
			const std::string message = arg.empty() ? missingParameterMessage(colon)
				: expandParamWordArg(arg, param_quoted_ ? nullptr : &removed);
			failFatal(ctx.label + ": " + message);
			return {};
		}
		default:
			return cur;
		}
	}

	// `${*:N}` or `${name[*]:N}`.
	static bool isStarSliceWord(const std::string& word) {
		if (word.rfind("${", 0) != 0) return false;
		if (word.compare(2, 2, "*:") == 0) return true;

		std::size_t end = 2;
		while (end < word.size() && isNameCont(word[end])) ++end;
		return end > 2 && word.compare(end, 4, "[*]:") == 0;
	}

	// bash keeps a quoted null as a DEL byte inside, and assigning an
	// unquoted `*` slice loses every DEL to that.
	static void dropAssignedQuotedNulls(const std::string& word, std::string& value) {
		static const char kDel = '\177';

		if (!isStarSliceWord(word)) return;
		value.erase(std::remove(value.begin(), value.end(), kDel), value.end());
	}

	void Expander::assignParamDefault(const ParamOpCtx& ctx, const std::string& value) {
		if (!isNameStart(ctx.name[0])) {
			fail("$" + ctx.name + ": cannot assign in this way");
			return;
		}

		// As in bash, this ends a script, not just the command.
		if (env_.isReadonly(ctx.name)) {
			failFatal(env_.resolveRef(ctx.name) + ": readonly variable");
			return;
		}

		if (!ctx.has_subscript) {
			env_.set(ctx.name, value);
			return;
		}

		const ElementSlot& slot = ctx.slot;
		if (!slot.resolved) return;
		if (slot.assoc) env_.setAssocElement(ctx.name, slot.key, value);
		else if (slot.index >= 0) env_.setIndexedElement(ctx.name, slot.index, value);
	}

	// `OFFSET:LENGTH` split at the first colon outside parentheses and
	// braces (`${PARAM:${OFFSET:-0}}`) that does not close a ternary of
	// the offset expression.
	// True when there is a length, `${a:2:}`'s empty one (0) included.
	static bool splitSliceArgs(const std::string& args, std::string& offset_text,
			std::string& length_text) {
		int depth = 0;
		int ternaries = 0;
		for (std::size_t k = 0; k < args.size(); ++k) {
			const char c = args[k];
			if (c == '(' || c == '{') ++depth;
			else if ((c == ')' || c == '}') && depth > 0) --depth;
			else if (c == '?' && depth == 0) ++ternaries;
			else if (c == ':' && depth == 0 && ternaries > 0) --ternaries;
			else if (c == ':' && depth == 0) {
				offset_text = args.substr(0, k);
				length_text = args.substr(k + 1);
				return true;
			}
		}

		offset_text = args;
		length_text.clear();
		return false;
	}

	static std::size_t skipQuoted(const std::string& text, std::size_t i) {
		const char quote = text[i];
		for (++i; i < text.size() && text[i] != quote; ++i) {
			if (quote != '\'' && text[i] == '\\') ++i;
		}

		return i;
	}

	static std::size_t skipNested(const std::string& text, std::size_t i) {
		const char open = text[i + 1];
		const char close = open == '{' ? '}' : ')';
		int depth = 0;
		for (++i; i < text.size(); ++i) {
			if (text[i] == '\\') {
				++i;
			} else if (text[i] == '\'' || text[i] == '"' || text[i] == '`') {
				i = skipQuoted(text, i);
			} else if (text[i] == open) {
				++depth;
			} else if (text[i] == close && --depth == 0) {
				return i;
			}
		}

		return i;
	}

	// The `/` between pattern and replacement is the first one that is not
	// escaped, quoted or inside a nested expansion: `${s/\/usr/U}` replaces
	// "/usr", as in bash.
	// A pattern may start with `/`: the delimiter is looked for after it.
	static std::size_t findReplaceSlash(const std::string& body, bool anchored) {
		const std::size_t start = !anchored && !body.empty() && body[0] == '/' ? 1 : 0;
		for (std::size_t i = start; i < body.size(); ++i) {
			const char c = body[i];
			if (c == '/') return i;
			if (c == '\\') {
				++i;
			} else if (c == '\'' || c == '"' || c == '`') {
				i = skipQuoted(body, i);
			} else if (c == '$' && i + 1 < body.size() && body[i + 1] == '\'') {
				i = ansiCQuoteClose(body, i);
			} else if (c == '$' && i + 1 < body.size()
					&& (body[i + 1] == '{' || body[i + 1] == '(')) {
				i = skipNested(body, i);
			}
		}

		return std::string::npos;
	}

	// bash 5.2, patsub_replacement: in the replacement, an `&` stands for the
	// matched text. In its literal text a backslash quotes the character
	// after it (`\&` is a plain `&`, `\z` is `z`, `\\` is `\`), and an `&`
	// inside quotes is plain too. What an expansion puts there keeps its
	// backslashes, and its `&` is the match unless the expansion is quoted.
	// A plain `&` leaves here as kLiteralAmpersand.
	static const char kLiteralAmpersand = '\x01';

	static void appendQuotedReplacement(const std::string& text, std::string& out) {
		for (char c : text) out.push_back(c == '&' ? kLiteralAmpersand : c);
	}

	// An unquoted expansion in the replacement keeps its backslashes,
	// except that `\&` is a literal `&` and `\\` one backslash.
	static void appendUnquotedReplacement(const std::string& text, std::string& out) {
		for (std::size_t i = 0; i < text.size(); ++i) {
			const bool escape = text[i] == '\\' && i + 1 < text.size()
				&& (text[i + 1] == '&' || text[i + 1] == '\\');
			if (escape) ++i;
			out.push_back(escape && text[i] == '&' ? kLiteralAmpersand : text[i]);
		}
	}

	std::string Expander::expandReplacementArg(const std::string& raw) {
		std::string out;
		bool in_dq = false;
		std::size_t i = 0;
		while (i < raw.size() && !aborting()) {
			const char c = raw[i];
			if (c == '\'' && !in_dq) {
				std::string quoted;
				copySingleQuotedRun(raw, i, quoted);
				appendQuotedReplacement(quoted, out);
			} else if (c == '"') {
				in_dq = !in_dq;
				++i;
			} else if (c == '$' && i + 1 < raw.size() && raw[i + 1] == '\'' && !in_dq) {
				appendQuotedReplacement(takeAnsiCQuoted(raw, i), out);
			} else if (c == '$' || c == '`') {
				std::string value;
				if (c == '$') expandHeredocDollar(raw, i, value);
				else expandHeredocBackquote(raw, i, value);
				if (in_dq) appendQuotedReplacement(value, out);
				else appendUnquotedReplacement(value, out);
			} else if (in_dq && c == '\\' && i + 1 < raw.size()) {
				expandBackslash(raw, i, doubleQuoteEscapes, out);
			} else if (c == '\\' && i + 1 < raw.size()) {
				const char next = raw[i + 1];
				if (next != '\n') out.push_back(next == '&' ? kLiteralAmpersand : next);
				i += 2;
			} else {
				out.push_back(in_dq && c == '&' ? kLiteralAmpersand : c);
				++i;
			}
		}

		return out;
	}

	// After `//` bash 5.3 reads a leading `#` or `%` as part of the pattern.
	Expander::ReplaceArgs Expander::parseReplaceArgs(const std::string& args, bool all) {
		ReplaceArgs out;
		std::string body = args;
		out.anchor_start = !all && !body.empty() && body[0] == '#';
		out.anchor_end   = !all && !out.anchor_start && !body.empty() && body[0] == '%';
		if (out.anchor_start || out.anchor_end) body.erase(0, 1);

		const std::size_t slash = findReplaceSlash(body, out.anchor_start || out.anchor_end);
		const std::string pat = (slash == std::string::npos) ? body : body.substr(0, slash);
		const std::string rep = (slash == std::string::npos) ? std::string()
			: body.substr(slash + 1);
		out.pat = expandPatternArg(pat);
		out.rep = expandReplacementWord(rep);
		return out;
	}

	// Even inside double quotes the replacement expands a leading tilde.
	std::string Expander::expandReplacementWord(const std::string& rep) {
		std::string home;
		std::size_t i = 0;
		leadingTildeExpansion(env_, path_conv_, rep, i, home);

		const std::string rest = rep.substr(i);
		if (!env_.shopts().patsub_replacement) return home + expandParamWordArg(rest);

		std::string out;
		appendQuotedReplacement(home, out);
		return out + expandReplacementArg(rest);
	}

	std::string Expander::evalParamReplace(const std::string& cur, const std::string& args,
			bool all) {
		const ReplaceArgs r = parseReplaceArgs(args, all);
		return replacePattern(cur, r.pat, r.rep, all, r.anchor_start, r.anchor_end);
	}

	// `##`, `%%`, `//`, `^^`, `,,`, `~~` double the operator to mean
	// "greedy" or "all"; the other operators never double.
	static bool isDoublingOp(char op) {
		return op == '#' || op == '%' || op == '/' || op == '^' || op == ',' || op == '~';
	}

	bool Expander::isOperandUnset(const ParamOpCtx& ctx) const {
		return ctx.has_subscript ? !ctx.slot.is_set : !isParamSet(ctx.name);
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
			// An unset parameter gives nothing, without the offset checks.
			if (isOperandUnset(ctx)) return std::string();
			return substringExpand(ctx.named_value, argument, ctx.label.empty() ? ctx.name : ctx.label);
		case '#':
			return stripPrefix(ctx.named_value, expandPatternArg(argument), doubled);
		case '%':
			return stripSuffix(ctx.named_value, expandPatternArg(argument), doubled);
		case '/':
			if (isOperandUnset(ctx)) return std::string();
			return evalParamReplace(ctx.named_value, argument, doubled);
		case '^':
		case ',':
		case '~':
			return applyCaseConv(ctx.named_value, op, doubled, expandPatternArg(argument));
		case '@':
			return transformParam(ctx, argument);
		default:
			failBadSubstitution(ctx.body);
			return {};
		}
	}

	static std::string singleQuotedText(const std::string& text) {
		std::string out = "'";
		for (const char c : text) {
			if (c == '\'') out += "'\\''";
			else out.push_back(c);
		}

		return out + "'";
	}

	// bash has turned each `$'...'` of the text into a '...' string by then.
	std::string Expander::withAnsiCStringsQuoted(const std::string& body) {
		std::string out;
		std::size_t i = 0;
		while (i < body.size()) {
			if (body.compare(i, 2, "$'") != 0) {
				out.push_back(body[i++]);
				continue;
			}

			const std::size_t close = ansiCQuoteClose(body, i);
			out += singleQuotedText(interpretAnsiC(body.substr(i + 2, close - i - 2)));
			i = std::min(close + 1, body.size());
		}

		return out;
	}

	void Expander::failBadSubstitution(const std::string& body) {
		fail("${" + withAnsiCStringsQuoted(body) + "}: bad substitution");
	}

	static char32_t convertCase(char32_t c, char op) {
		if (op == '^') return upperCode(c);
		if (op == ',') return lowerCode(c);

		const char32_t upper = upperCode(c);
		return upper != c ? upper : lowerCode(c);
	}

	// `^` uppercases, `,` lowercases and `~` toggles the case of the
	// matching characters: the first one, or every one when doubled.
	std::string Expander::applyCaseConv(const std::string& val, char op, bool all,
			const std::string& pat) {
		std::string out;
		out.reserve(val.size());
		for (std::size_t k = 0; k < val.size();) {
			const Utf8Char c = decodeUtf8(val, k);
			const bool wanted = (all || k == 0)
				&& (pat.empty() || fnmatchRange(pat, val, k, c.length, env_.extglob()));
			if (wanted) appendUtf8(out, convertCase(c.code, op));
			else out.append(val, k, c.length);
			k += c.length;
		}

		return out;
	}

	static bool isTransformLetter(const std::string& letter) {
		return letter.size() == 1 && std::strchr("UuLQEPAKak", letter[0]) != nullptr;
	}

	static std::string doubleQuotedText(const std::string& text) {
		std::string quoted = "\"";
		for (const char c : text) {
			if (c == '"' || c == '\\' || c == '$' || c == '`') quoted.push_back('\\');
			quoted.push_back(c);
		}

		return quoted + "\"";
	}

	static bool hasControlChar(const std::string& text) {
		for (const char c : text) {
			const unsigned char code = static_cast<unsigned char>(c);
			if (code < 0x20 || code == 0x7f) return true;
		}

		return false;
	}

	static std::string keyValueText(const std::string& text) {
		return hasControlChar(text) ? shellQuoteAlways(text) : doubleQuotedText(text);
	}

	// An associative key stays bare unless it needs quoting.
	static std::string keyText(const std::string& key) {
		if (hasControlChar(key)) return shellQuoteAlways(key);
		if (key.empty() || key == "@" || key == "*" || shellQuoteForReuse(key) != key) {
			return doubleQuotedText(key);
		}

		return key;
	}

	// `${x@P}`: the value as a prompt string -- escapes decoded and, with
	// promptvars, expanded.
	std::string Expander::promptTransform(const std::string& value) {
		PromptRequest request;
		if (sub_ != nullptr) request.counts = sub_->promptCounts();
		request.for_expansion = env_.shopts().promptvars;

		const std::string decoded = decodePromptEscapes(value, env_, path_conv_, request);
		return request.for_expansion ? expandPromptText(decoded) : decoded;
	}

	std::string Expander::transformValue(char letter, const std::string& value) {
		switch (letter) {
		case 'U': return applyCaseConv(value, '^', true, {});
		case 'u': return applyCaseConv(value, '^', false, {});
		case 'L': return applyCaseConv(value, ',', true, {});
		case 'E': return interpretAnsiC(value);
		case 'P': return promptTransform(value);
		default:  return shellQuoteAlways(value);
		}
	}

	std::string Expander::assignmentText(const std::string& name, const std::string& value) const {
		const std::string letters = env_.attributeLetters(name);
		const std::string assignment = name + "=" + shellQuoteAlways(value);
		return letters.empty() ? assignment : "declare -" + letters + " " + assignment;
	}

	// A variable declared with attributes but never given a value:
	// `${v@A}` still shows how it was declared.
	bool Expander::isDeclaredOnly(const std::string& name) const {
		if (name.empty() || !env_.isValueless(name)) return false;
		if (const auto* indexed = env_.getIndexedArray(name)) return indexed->empty();
		if (const auto* assoc = env_.getAssocArray(name)) return assoc->empty();
		return !env_.attributeLetters(name).empty();
	}

	std::string Expander::declaredOnlyText(const std::string& name) const {
		return isDeclaredOnly(name) ? declarationOnly(name) : std::string();
	}

	std::string Expander::declarationOnly(const std::string& name) const {
		const std::string letters = env_.attributeLetters(name);
		return "declare -" + (letters.empty() ? std::string("-") : letters) + " " + name;
	}

	// `${x@a}` shows attributes even of a declared but unset variable;
	// the other transformations of an unset one are empty.
	std::string Expander::transformParam(const ParamOpCtx& ctx, const std::string& letter) {
		if (!isTransformLetter(letter)) {
			failBadSubstitution(ctx.body);
			return {};
		}

		if (letter[0] == 'a') return env_.attributeLetters(ctx.name);

		const bool array = env_.isIndexedArray(ctx.name) || env_.isAssocArray(ctx.name);
		const bool bare = isDeclaredOnly(ctx.name) || (array && lacksElementZero(ctx.name));
		if (letter[0] == 'A' && !ctx.has_subscript && bare) return declarationOnly(ctx.name);

		const bool set = ctx.has_subscript ? ctx.slot.is_set : isParamSet(ctx.name);
		if (!set) return {};
		if (letter[0] == 'A') return assignmentText(ctx.name, ctx.named_value);
		return transformValue(letter[0], ctx.named_value);
	}

	// The keys and values of array @p name, alternating.
	std::vector<std::string> Expander::keyValueWords(const std::string& name) const {
		std::vector<std::string> words;
		if (const auto* indexed = env_.getIndexedArray(name)) {
			for (const auto& entry : *indexed) {
				words.push_back(std::to_string(entry.first));
				words.push_back(entry.second);
			}
		}

		if (const auto* assoc = env_.getAssocArray(name)) {
			for (const auto& entry : *assoc) {
				words.push_back(entry.first);
				words.push_back(entry.second);
			}
		}

		return words;
	}

	static std::string keyValuePairs(const std::vector<std::string>& words, bool assoc) {
		std::string text;
		for (std::size_t k = 0; k + 1 < words.size(); k += 2) {
			if (!text.empty()) text.push_back(' ');
			text += assoc ? keyText(words[k]) : words[k];
			text += " " + keyValueText(words[k + 1]);
		}

		return text;
	}

	static std::string setPositionalText(const std::vector<std::string>& elems) {
		std::string text = "set --";
		for (const std::string& elem : elems) text += " " + shellQuoteAlways(elem);
		return text;
	}

	// `@A`, `@K` and `@k` of a whole array describe the array; the other
	// transformations, and all of them on `$@`, apply to each element.
	std::vector<std::string> Expander::transformList(const ListParam& p, const std::string& letter,
			std::vector<std::string> elems) {
		if (!isTransformLetter(letter)) {
			failBadSubstitution(p.name + "[@]@" + letter);
			return {};
		}

		const char op = letter[0];
		const bool array = !p.name.empty()
			&& (env_.isIndexedArray(p.name) || env_.isAssocArray(p.name));
		if (op == 'A' && p.name.empty()) return { setPositionalText(elems) };
		if (elems.empty() && (op == 'A' || op == 'a') && isDeclaredOnly(p.name)) {
			return { op == 'A' ? declaredOnlyText(p.name) : env_.attributeLetters(p.name) };
		}

		if (op == 'A' && array) return { declarationText(env_, p.name) };
		if (op == 'K' && array) {
			return { keyValuePairs(keyValueWords(p.name), env_.isAssocArray(p.name)) };
		}

		if (op == 'k' && array) return keyValueWords(p.name);

		for (std::string& elem : elems) {
			if (op == 'a') elem = env_.attributeLetters(p.name);
			else if (op == 'A') elem = assignmentText(p.name, elem);
			else elem = transformValue(op, elem);
		}

		return elems;
	}

	// `${@=word}` is an error in bash, so `=` keeps the scalar path.
	// Only an array takes `=`; `${@=word}` is the scalar path's error.
	static bool isListOp(const std::string& body, std::size_t i, bool array) {
		const char op = body[i];
		if (op == ':') return array || i + 1 >= body.size() || body[i + 1] != '=';
		if (op == '=') return array;
		return op == '-' || op == '+' || op == '?' || op == '#' || op == '%'
			|| op == '/' || op == '^' || op == ',' || op == '~' || op == '@';
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

		if (end >= body.size() || !isListOp(body, end, !out.name.empty())) return false;
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
		if (scalar) return { substringExpand(env_.get(p.name), args, p.name) };

		const KeyedList items = sliceCandidates(p);
		std::string offset_text;
		std::string length_text;
		const bool has_length = splitSliceArgs(args, offset_text, length_text);

		const std::string label = sliceLabel(p);
		long long offset = evalSliceArith(offset_text, label);
		if (failed()) return {};
		if (offset < 0 && !items.empty()) offset += items.back().first + 1;
		if (offset < 0) return {};

		long long take = static_cast<long long>(items.size());
		if (has_length) take = length_text.empty() ? 0 : evalSliceArith(length_text, label);
		if (failed()) return {};
		if (take < 0) {
			fail(length_text + ": substring expression < 0");
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
	// A `*` list is null when it joins to nothing: `${A[*]:-X}` with
	// A=('' '') and a null IFS takes X, as in bash.
	std::vector<std::string> Expander::nullTestedValues(const ListParam& p) const {
		std::vector<std::string> values = listParamValues(p);
		if (!p.star || values.size() < 2 || !starSeparator(true).empty()) return values;

		const bool all_empty = std::all_of(values.begin(), values.end(),
			[](const std::string& value) { return value.empty(); });
		return all_empty ? std::vector<std::string>{ std::string() } : values;
	}

	std::vector<std::string> Expander::evalListDefaultOp(char op, bool colon,
			const std::string& subject, std::vector<std::string> elems,
			const std::string& arg) {
		const bool unset = elems.empty();
		const bool null = unset || (elems.size() == 1 && elems[0].empty());
		const bool missing = colon ? null : unset;

		switch (op) {
		case '-':
			if (missing) return { expandParamWordArg(arg, param_quoting_) };
			return elems;
		case '+':
			if (!missing) return { expandParamWordArg(arg, param_quoting_) };
			if (unset) return {};
			return { std::string() };
		case '?':
			if (!missing) return elems;

			failFatal(subject + ": " + (arg.empty() ? missingParameterMessage(colon)
				: expandParamWordArg(arg)));
			return {};
		default:
			return elems;
		}
	}

	// bash assigns `${A[@]=v}` to the key `@` of an associative array (`*`
	// for `${A[*]=v}`); an indexed array has no such element.
	std::vector<std::string> Expander::assignWholeArrayDefault(const ListParam& p, bool colon,
			const std::string& subject, const std::string& arg) {
		std::vector<std::string> elems = nullTestedValues(p);
		const bool null = elems.empty() || (elems.size() == 1 && elems[0].empty());
		if (!(colon ? null : elems.empty())) return elems;

		const std::string name = env_.resolveRef(p.name);
		if (!env_.isAssocArray(name)) {
			failFatal(subject + ": bad array subscript");
			return {};
		}

		WordQuoting removed;
		const bool outer = std::exchange(assignment_value_, true);
		const std::string value = expandParamWordArg(arg, param_quoted_ ? nullptr : &removed);
		assignment_value_ = outer;
		if (failed()) return {};

		const std::string key = p.star ? "*" : "@";
		env_.setAssocElement(name, key, value);
		const Environment::AssocArray* array = env_.getAssocArray(name);
		if (array == nullptr) return {};

		const auto stored = array->find(key);
		if (stored == array->end()) return {};
		return { stored->second };
	}

	std::vector<std::string> Expander::mapListOp(char op, bool doubled,
			const std::string& arg, std::vector<std::string> elems) {
		if (op == '/') {
			const ReplaceArgs r = parseReplaceArgs(arg, doubled);
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
		if (op == '@') return transformList(p, argument, listParamValues(p));
		if (isDefaultOp(op)) {
			const std::string subject = p.label.empty() ? body.substr(0, p.op_pos) : p.label;
			if (op == '=' && !p.name.empty()) {
				return assignWholeArrayDefault(p, colon, subject, argument);
			}

			return evalListDefaultOp(op, colon, subject, nullTestedValues(p), argument);
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

	// `${v@a}` and `${v@A}` describe an array that has elements but no
	// element 0 with no unbound-variable error under set -u; an empty one
	// is still unbound.
	bool Expander::showsAttributesOf(const std::string& body, std::size_t i,
			const std::string& name) const {
		if (i + 1 >= body.size() || body[i] != '@') return false;
		if (body[i + 1] != 'a' && body[i + 1] != 'A') return false;

		return arrayLength(name) > 0;
	}

	static std::size_t skipQuotedRun(const std::string& text, std::size_t i) {
		const char quote = text[i];
		for (++i; i < text.size() && text[i] != quote; ++i) {
			if (quote == '"' && text[i] == '\\') ++i;
		}

		return i;
	}

	// The `]` closing the `[` at @p open; brackets nest, as in
	// `${a[ ${#a[@]} - 1 ]}`, and quoted text is skipped.
	static std::size_t subscriptClose(const std::string& text, std::size_t open) {
		int depth = 0;
		for (std::size_t i = open; i < text.size(); ++i) {
			const char c = text[i];
			if (c == '\\') ++i;
			else if (c == '\'' || c == '"') i = skipQuotedRun(text, i);
			else if (c == '[') ++depth;
			else if (c == ']' && --depth == 0) return i;
		}

		return std::string::npos;
	}

	// Takes a `[subscript]` at body[i], advancing i past the `]`.
	static bool takeSubscript(const std::string& body, std::size_t& i, std::string& subscript) {
		if (i >= body.size() || body[i] != '[') return false;

		const std::size_t close = subscriptClose(body, i);
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

	// `${!1*}`, `${!@*}` and `${!_Q* }` look like the prefix form but are
	// not: bash rejects them whole rather than expand them indirectly.
	static bool isMalformedPrefixForm(const std::string& body) {
		if (body.size() < 3 || body[0] != '!') return false;

		std::size_t end = 1;
		if (isNameStart(body[1])) end = scanIdentifierEnd(body, 1);
		else if (isDigit(body[1])) end = scanDigitsEnd(body, 1);
		else end = 2;

		if (end >= body.size() || (body[end] != '*' && body[end] != '@')) return false;

		const bool transform = body[end] == '@' && end + 2 == body.size()
			&& std::isalpha(static_cast<unsigned char>(body[end + 1])) != 0;
		return !transform;
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

	// `${!-word}` is `$!` with a default: only a name, a digit or one of
	// `#?@*` (`@*` in posix mode) after the `!` makes the reference indirect.
	static bool isIndirectBody(const std::string& body, bool posix) {
		if (body.size() < 2 || body[0] != '!' || !fieldAwareArrayName(body).empty()) return false;

		const char c = body[1];
		const char* const specials = posix ? "@*" : "#?@*";
		return isNameStart(c) || isDigit(c) || std::strchr(specials, c) != nullptr;
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

	// bash 5.3 refuses to expand through an unset variable, whatever
	// operator follows it and even under `set -u`. An unset element or
	// special parameter leaves its own (unset) spelling as the subject.
	bool Expander::resolveUnsetIndirect(const std::string& spec, const std::string& rest,
			const std::string& label, bool named, std::string& out_body) {
		if (named) {
			fail(label.substr(1) + ": invalid indirect expansion");
			return true;
		}

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

	// `${#name}` is a length; `${#:-w}`, `${#-w}`, `${#?w}` and `${#:off}`
	// apply an operator to `$#` itself; anything else after `#` is a bad
	// substitution, as in bash.
	std::string Expander::expandHashForm(const std::string& body) {
		const std::string rest = body.substr(1);
		if (isParamSpec(rest, *this)) return expandParamLengthForm(body);

		const bool operator_on_count = (rest[0] == ':' && rest.size() > 1) || rest[0] == '-'
			|| rest[0] == '?';
		if (operator_on_count) return expandDirectParam(body, {});

		failBadSubstitution(body);
		return {};
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
		if (!ref_set) {
			const bool missing = !has_subscript || !(env_.isIndexedArray(ref)
				|| env_.isAssocArray(ref) || isVariableSet(ref));
			const bool named = missing && isPlainName(ref);
			return resolveUnsetIndirect(spec, rest, out_label, named, out_body);
		}

		if (!isParamSpec(target, *this)) {
			fail(target + ": invalid variable name");
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
		const std::string referrer = body.substr(1);
		if (env_.isNameref(referrer)) return env_.namerefTarget(referrer);

		std::string prefix;
		bool star = false;
		if (parsePrefixNamesForm(body, prefix, star)) {
			return joinWith(namesWithPrefix(prefix), starSeparator(star));
		}

		if (isMalformedPrefixForm(body)) {
			failBadSubstitution(body);
			return {};
		}

		const std::string keys_of = fieldAwareArrayName(body);
		if (!keys_of.empty()) return joinWith(arrayKeys(keys_of), " ");

		std::string resolved;
		std::string label;
		if (!resolveIndirectBody(body, resolved, label)) return expandDirectParam(body, {});
		if (aborting()) return {};
		return expandDirectParam(resolved, label);
	}

	// `${!-word}`, `${!:+word}`: an operator on `$!` itself.
	static bool isBangOperatorForm(const std::string& body) {
		return body.size() > 1 && std::strchr("-+=?:%/^,", body[1]) != nullptr;
	}

	std::string Expander::expandParam(const std::string& body, bool quoted_ctx,
			WordQuoting* quoting, const std::string& label) {
		if (body.empty()) return {};
		if (isNoForkSubstBody(body)) return runNoForkSubst(body);

		const bool saved = param_quoted_;
		WordQuoting* const saved_quoting = param_quoting_;
		param_quoted_ = quoted_ctx;
		param_quoting_ = quoted_ctx ? nullptr : quoting;
		const bool bang_form = body[0] == '!' && body.size() > 1 && !isBangOperatorForm(body);
		std::string value;
		if (!label.empty()) value = expandDirectParam(body, label);
		else if (body[0] == '#' && body.size() > 1) value = expandHashForm(body);
		else if (bang_form) value = expandBangParam(body);
		else value = expandDirectParam(body, {});

		param_quoted_ = saved;
		param_quoting_ = saved_quoting;
		return value;
	}

	// With extquote, `${$'x1'%t}` names the parameter by the decoded string.
	static bool startsWithAnsiCName(const std::string& body) {
		return body.size() > 2 && body[0] == '$' && body[1] == '\'';
	}

	std::string Expander::withDecodedName(const std::string& body) {
		const std::size_t close = ansiCQuoteClose(body, 0);
		const std::string rest = close < body.size() ? body.substr(close + 1) : std::string();
		return interpretAnsiC(body.substr(2, close - 2)) + rest;
	}

	std::string Expander::expandDirectParam(const std::string& body, const std::string& label) {
		if (env_.shopts().extquote && startsWithAnsiCName(body)) {
			return expandDirectParam(withDecodedName(body), label);
		}

		ListParam list;
		if (parseListParam(body, list)) {
			list.label = label;
			return joinWith(expandListParam(list, body), starSeparator(list.star));
		}

		std::string name;
		std::size_t i = scanParamName(body, name, *this);
		if (i == std::string::npos) {
			failBadSubstitution(body);
			return {};
		}

		std::string subscript;
		const bool has_subscript = takeSubscript(body, i, subscript);

		// `${name:-…}`, `${name-…}`, `${name:=…}`, `${name:?…}` (and their
		// colon-less siblings) handle an unset `name` themselves, so the
		// lookup here must not raise `set -u`'s unbound-variable error.
		const bool handles_unset = paramOpHandlesUnset(body, i) || showsAttributesOf(body, i, name);
		ElementSlot slot;
		const std::string named = has_subscript
			? lookupSubscripted(name, subscript, true, &slot)
			: lookupParam(name, handles_unset, label);
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

	static std::string charSlice(const std::string& val, std::size_t offset, std::size_t count) {
		const std::size_t begin = utf8Advance(val, 0, offset);
		const std::size_t end = utf8Advance(val, begin, count);
		return val.substr(begin, end - begin);
	}

	// bash names the parameter in front of an offset or length error.
	long long Expander::evalSliceArith(const std::string& text, const std::string& label) {
		const bool had_error = failed();
		const long long value = evalArith(text);
		if (!had_error && failed()) pending_error_ = label + ": " + pending_error_;
		return value;
	}

	std::string Expander::sliceLabel(const ListParam& p) const {
		if (!p.label.empty()) return p.label;

		const std::string all = p.star ? "*" : "@";
		return p.name.empty() ? all : p.name + "[" + all + "]";
	}

	std::string Expander::substringExpand(const std::string& val, const std::string& args,
			const std::string& label) {
		std::string offset_text;
		std::string length_text;
		const bool has_length = splitSliceArgs(args, offset_text, length_text);

		// As in bash: an offset before the start or past the end gives
		// nothing, and a negative length (counted from the end) must not end
		// before the offset.
		const long long size = static_cast<long long>(utf8Count(val));
		long long offset = evalSliceArith(offset_text, label);
		if (offset < 0) offset += size;
		if (failed() || offset < 0 || offset > size) return {};

		long long length = !has_length ? size - offset
			: length_text.empty() ? 0 : evalSliceArith(length_text, label);
		if (failed()) return {};
		if (length < 0) {
			length = size + length - offset;
			if (length < 0) {
				fail(length_text + ": substring expression < 0");
				return {};
			}
		}

		if (offset + length > size) length = size - offset;
		return charSlice(val, static_cast<std::size_t>(offset), static_cast<std::size_t>(length));
	}

	// The byte offset of every character start, plus the end.
	using CharBounds = std::vector<std::size_t>;

	// With nocasematch the pattern is lower-cased once and each stretch of
	// the value as it is matched; ASCII case folding keeps byte offsets.
	struct PatternRef {
		const std::string& text;
		bool extglob;
		PatternShape shape;
		bool nocase = false;
	};

	static std::string asciiLowered(std::string text) {
		for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return text;
	}

	static PatternRef makePatternRef(const std::string& pat, bool extglob) {
		return { pat, extglob, analyzePattern(pat, extglob) };
	}

	// Matching steps through characters, or through bytes when the pattern
	// or the value is not valid UTF-8, as bash does.
	static CharBounds charBounds(const std::string& val, const std::string& pattern) {
		const bool bytes = !isValidUtf8(pattern) || !isValidUtf8(val);
		CharBounds bounds;
		bounds.reserve(val.size() + 1);
		for (std::size_t pos = 0; pos < val.size(); pos += bytes ? 1 : utf8CharLength(val, pos)) {
			bounds.push_back(pos);
		}

		bounds.push_back(val.size());
		return bounds;
	}

	static bool matchesBetween(const std::string& val, const CharBounds& bounds, std::size_t from,
			std::size_t to, const PatternRef& pat) {
		const std::size_t length = bounds[to] - bounds[from];
		if (!pat.nocase) return fnmatchRange(pat.text, val, bounds[from], length, pat.extglob);

		return fnmatchFull(pat.text, asciiLowered(val.substr(bounds[from], length)), pat.extglob);
	}

	// A pattern that starts with a literal byte cannot match where the
	// value has another one.
	static bool canMatchFrom(const std::string& val, const CharBounds& bounds, std::size_t from,
			const PatternRef& pat) {
		if (pat.shape.first_byte < 0) return true;

		const std::size_t at = bounds[from];
		if (at >= val.size()) return false;

		const unsigned char c = static_cast<unsigned char>(val[at]);
		return (pat.nocase ? std::tolower(c) : c) == pat.shape.first_byte;
	}

	// The farthest bound index a match starting at bounds[from] can reach.
	static std::size_t farthestEnd(const CharBounds& bounds, std::size_t from,
			const PatternRef& pat) {
		const std::size_t last = bounds.size() - 1;
		const std::size_t reach = pat.shape.max_chars;
		return (reach == std::string::npos || last - from <= reach) ? last : from + reach;
	}

	// Bound index of the farthest end matching `pat` from bounds[from], or npos.
	static std::size_t longestMatchAt(const std::string& val, const CharBounds& bounds,
			std::size_t from, const PatternRef& pat) {
		if (!canMatchFrom(val, bounds, from, pat)) return std::string::npos;

		for (std::size_t to = farthestEnd(bounds, from, pat);; --to) {
			if (matchesBetween(val, bounds, from, to, pat)) return to;
			if (to == from) return std::string::npos;
		}
	}

	static std::size_t shortestPrefixMatch(const std::string& val, const CharBounds& bounds,
			const PatternRef& pat) {
		if (!canMatchFrom(val, bounds, 0, pat)) return std::string::npos;

		const std::size_t farthest = farthestEnd(bounds, 0, pat);
		for (std::size_t to = 0; to <= farthest; ++to) {
			if (matchesBetween(val, bounds, 0, to, pat)) return to;
		}

		return std::string::npos;
	}

	// The first bound index a suffix match can start at.
	static std::size_t earliestSuffixStart(const CharBounds& bounds, const PatternRef& pat) {
		const std::size_t last = bounds.size() - 1;
		const std::size_t reach = pat.shape.max_chars;
		return (reach == std::string::npos || last <= reach) ? 0 : last - reach;
	}

	// Bound index where the longest suffix matching `pat` starts, or npos.
	static std::size_t longestSuffixMatch(const std::string& val, const CharBounds& bounds,
			const PatternRef& pat) {
		const std::size_t last = bounds.size() - 1;
		for (std::size_t from = earliestSuffixStart(bounds, pat); from <= last; ++from) {
			if (!canMatchFrom(val, bounds, from, pat)) continue;
			if (matchesBetween(val, bounds, from, last, pat)) return from;
		}

		return std::string::npos;
	}

	static std::size_t shortestSuffixMatch(const std::string& val, const CharBounds& bounds,
			const PatternRef& pat) {
		const std::size_t last = bounds.size() - 1;
		const std::size_t earliest = earliestSuffixStart(bounds, pat);
		for (std::size_t from = last;; --from) {
			const bool viable = canMatchFrom(val, bounds, from, pat);
			if (viable && matchesBetween(val, bounds, from, last, pat)) return from;
			if (from == earliest) return std::string::npos;
		}
	}

	static bool startsWith(const std::string& val, const std::string& text) {
		return val.size() >= text.size() && val.compare(0, text.size(), text) == 0;
	}

	static bool endsWith(const std::string& val, const std::string& text) {
		return val.size() >= text.size()
			&& val.compare(val.size() - text.size(), text.size(), text) == 0;
	}

	std::string Expander::stripPrefix(const std::string& val, const std::string& pat,
			bool greedy) {
		if (pat.empty()) return val;

		const PatternRef ref = makePatternRef(pat, env_.extglob());
		if (ref.shape.literal) {
			return startsWith(val, ref.shape.text) ? val.substr(ref.shape.text.size()) : val;
		}

		const CharBounds bounds = charBounds(val, pat);
		const std::size_t to = greedy ? longestMatchAt(val, bounds, 0, ref)
			: shortestPrefixMatch(val, bounds, ref);
		if (to == std::string::npos) return val;
		return val.substr(bounds[to]);
	}

	std::string Expander::stripSuffix(const std::string& val, const std::string& pat,
			bool greedy) {
		if (pat.empty()) return val;

		const PatternRef ref = makePatternRef(pat, env_.extglob());
		if (ref.shape.literal) {
			if (!endsWith(val, ref.shape.text)) return val;
			return val.substr(0, val.size() - ref.shape.text.size());
		}

		const CharBounds bounds = charBounds(val, pat);
		const std::size_t from = greedy ? longestSuffixMatch(val, bounds, ref)
			: shortestSuffixMatch(val, bounds, ref);
		if (from == std::string::npos) return val;
		return val.substr(0, bounds[from]);
	}

	struct Replacement {
		const std::string& text;
		bool ampersands = false;
	};

	static std::string replacementFor(const Replacement& rep, const std::string& match) {
		if (!rep.ampersands) return rep.text;

		std::string out;
		for (char c : rep.text) {
			if (c == '&') out += match;
			else out.push_back(c == kLiteralAmpersand ? '&' : c);
		}

		return out;
	}

	static std::string replaceAnchoredPrefix(const std::string& val, const PatternRef& pat,
			const Replacement& rep) {
		if (pat.shape.literal) {
			if (!startsWith(val, pat.shape.text)) return val;
			return replacementFor(rep, pat.shape.text) + val.substr(pat.shape.text.size());
		}

		const CharBounds bounds = charBounds(val, pat.text);
		const std::size_t to = longestMatchAt(val, bounds, 0, pat);
		if (to == std::string::npos) return val;
		return replacementFor(rep, val.substr(0, bounds[to])) + val.substr(bounds[to]);
	}

	static std::string replaceAnchoredSuffix(const std::string& val, const PatternRef& pat,
			const Replacement& rep) {
		if (pat.shape.literal) {
			if (!endsWith(val, pat.shape.text)) return val;
			const std::size_t at = val.size() - pat.shape.text.size();
			return val.substr(0, at) + replacementFor(rep, pat.shape.text);
		}

		const CharBounds bounds = charBounds(val, pat.text);
		const std::size_t from = longestSuffixMatch(val, bounds, pat);
		if (from == std::string::npos) return val;
		return val.substr(0, bounds[from]) + replacementFor(rep, val.substr(bounds[from]));
	}

	static void appendCharAt(std::string& out, const std::string& val, const CharBounds& bounds,
			std::size_t index) {
		if (index + 1 >= bounds.size()) return;

		out.append(val, bounds[index], bounds[index + 1] - bounds[index]);
	}

	// A literal pattern is found with a plain substring search.
	static std::string replaceLiteral(const std::string& val, const std::string& text,
			const Replacement& rep, bool all) {
		const std::string inserted = replacementFor(rep, text);
		std::string out;
		std::size_t from = 0;
		for (std::size_t at = val.find(text); at != std::string::npos; at = val.find(text, from)) {
			out.append(val, from, at - from);
			out += inserted;
			from = at + text.size();
			if (!all) break;
		}

		out.append(val, from, std::string::npos);
		return out;
	}

	// A zero-width match copies the next character, so a pattern that
	// always matches `""` still makes progress.
	static std::string replaceUnanchored(const std::string& val, const PatternRef& pat,
			const Replacement& rep, bool all) {
		if (pat.shape.literal) return replaceLiteral(val, pat.shape.text, rep, all);

		const CharBounds bounds = charBounds(val, pat.text);
		const std::size_t last = bounds.size() - 1;
		std::string out;
		std::size_t i = 0;
		while (i <= last) {
			const std::size_t to = longestMatchAt(val, bounds, i, pat);
			if (to == std::string::npos) {
				appendCharAt(out, val, bounds, i++);
				continue;
			}

			out += replacementFor(rep, val.substr(bounds[i], bounds[to] - bounds[i]));
			const bool empty_match = to == i;
			if (empty_match) appendCharAt(out, val, bounds, i++);
			else i = to;

			// A match that ran to the end leaves nothing for `*` to match.
			if (!empty_match && i == last) return out;
			if (!all) {
				if (i <= last) out.append(val, bounds[i], std::string::npos);
				return out;
			}
		}

		return out;
	}

	// An empty pattern matches only at an anchor: `${x/#/p}` prefixes p.
	static std::string insertAtAnchor(const std::string& val, const Replacement& rep,
			bool anchor_start, bool anchor_end) {
		if (anchor_start) return replacementFor(rep, std::string()) + val;
		if (anchor_end)   return val + replacementFor(rep, std::string());
		return val;
	}

	std::string Expander::replacePattern(const std::string& val, const std::string& pat,
			const std::string& rep, bool all, bool anchor_start, bool anchor_end) {
		const Replacement replacement{ rep, env_.shopts().patsub_replacement };
		if (pat.empty()) return insertAtAnchor(val, replacement, anchor_start, anchor_end);

		const bool nocase = env_.nocasematch();
		const std::string folded = nocase ? asciiLowered(pat) : std::string();
		PatternRef ref = makePatternRef(nocase ? folded : pat, env_.extglob());
		ref.nocase = nocase;
		if (nocase) ref.shape.literal = false;
		if (anchor_start) return replaceAnchoredPrefix(val, ref, replacement);
		if (anchor_end)   return replaceAnchoredSuffix(val, ref, replacement);
		return replaceUnanchored(val, ref, replacement, all);
	}

	static void pushText(Expander::Tagged& out, const std::string& text, std::uint8_t mark) {
		for (char c : text) out.push(c, mark);
	}

	static void pushQuotedNull(Expander::Tagged& out) {
		out.push(kNoSeparator, Expander::F_QUOTED | Expander::F_QUOTED_NULL);
		out.had_quote = true;
	}

	static void pushQuotedText(Expander::Tagged& out, const std::string& text) {
		if (text.empty()) {
			pushQuotedNull(out);
			return;
		}

		pushText(out, text, Expander::F_QUOTED);
		out.had_quote = true;
	}

	static void pushParamValue(Expander::Tagged& out, const std::string& value,
			const Expander::WordQuoting& quoting, std::uint8_t mark) {
		if (mark != 0 || !quoting.set || quoting.quoted.size() != value.size()) {
			pushText(out, value, mark);
			return;
		}

		std::size_t next_null = 0;
		for (std::size_t k = 0; k <= value.size(); ++k) {
			while (next_null < quoting.nulls.size() && quoting.nulls[next_null] == k) {
				pushQuotedNull(out);
				++next_null;
			}

			if (k < value.size()) out.push(value[k], quoting.quoted[k] != 0 ? Expander::F_QUOTED : 0);
		}

		if (quoting.had_quote) out.had_quote = true;
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
		const bool joins = star && inside_dq;
		const char shown = star ? (sep.empty() ? kNoSeparator : sep[0]) : ' ';
		const bool spaces_split = star && !inside_dq && sep.empty();
		if (spaces_split) out.split_on_spaces = true;

		const std::uint8_t elem_mark = spaces_split ? F_NOSPLIT : mark;
		for (std::size_t k = 0; k < elems.size(); ++k) {
			if (k != 0 && joins) pushText(out, sep, mark);
			if (k != 0 && !joins) out.endField(inside_dq, shown);
			if (inside_dq && !joins && elems[k].empty()) pushQuotedNull(out);
			else pushText(out, elems[k], elem_mark);
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

		if (quoted_null) pushQuotedNull(out);
	}

	// A nameref to `a[@]` or `a[*]` expands as that array does.
	bool Expander::renderWholeArrayRef(const std::string& name, Tagged& out, bool inside_dq) {
		WordSegment as_array;
		as_array.kind = WordSegment::Kind::ParamExp;
		as_array.text = env_.resolveRef(name);
		return renderParamExpSegment(as_array, out, inside_dq);
	}

	bool Expander::renderParamExpSegment(const WordSegment& s, Tagged& out, bool inside_dq) {
		if (isPlainName(s.text) && refersToWholeArray(s.text)) {
			return renderWholeArrayRef(s.text, out, inside_dq);
		}

		std::string prefix;
		bool star = false;
		if (parsePrefixNamesForm(s.text, prefix, star)) {
			return renderArrayFields(namesWithPrefix(prefix), star, inside_dq, out);
		}

		if (isMalformedPrefixForm(s.text)) {
			failBadSubstitution(s.text);
			return false;
		}

		std::string resolved;
		std::string label;
		const bool indirect = isIndirectBody(s.text, env_.options().posix)
			&& !env_.isNameref(s.text.substr(1));
		if (indirect && resolveIndirectBody(s.text, resolved, label)) {
			if (aborting()) return false;
			return renderParamBody(resolved, label, inside_dq, out);
		}

		return renderParamBody(s.text, {}, inside_dq, out);
	}

	// Like expandParam for a list: an unquoted `${a[@]:-word}` records the
	// quoting of its word, as the scalar form does.
	std::vector<std::string> Expander::expandListParamQuoted(const ListParam& list,
			const std::string& body, bool inside_dq, WordQuoting& quoting) {
		const bool saved = param_quoted_;
		WordQuoting* const saved_quoting = param_quoting_;
		param_quoted_ = inside_dq;
		param_quoting_ = inside_dq ? nullptr : &quoting;
		std::vector<std::string> elems = expandListParam(list, body);
		param_quoted_ = saved;
		param_quoting_ = saved_quoting;
		return elems;
	}

	static bool lexSingleWord(const std::string& text, Word& out) {
		Lexer lexer(text);
		std::vector<Token> tokens = lexer.tokenize();
		if (!lexer.errors().empty() || tokens.empty() || tokens[0].kind != TokKind::Word) return false;
		if (tokens.size() > 1 && tokens[1].kind != TokKind::EndOfInput) return false;

		out.segments = std::move(tokens[0].segments);
		out.raw = std::move(tokens[0].text);
		return true;
	}

	// The words of `${x+"$@" "$@"}`'s word and the blanks between them.
	struct WordList {
		std::vector<Word> words;
		std::vector<std::string> gaps;
	};

	static bool lexWordList(const std::string& text, WordList& out) {
		Lexer lexer(text);
		std::vector<Token> tokens = lexer.tokenize();
		if (!lexer.errors().empty()) return false;

		std::size_t previous_end = 0;
		for (Token& token : tokens) {
			if (token.kind == TokKind::EndOfInput) break;
			if (token.kind != TokKind::Word) return false;

			const std::size_t start = token.loc.offset;
			if (!out.words.empty()) out.gaps.push_back(text.substr(previous_end, start - previous_end));
			previous_end = start + token.text.size();

			Word word;
			word.segments = std::move(token.segments);
			word.raw = std::move(token.text);
			out.words.push_back(std::move(word));
		}

		return !out.words.empty();
	}

	struct ListAlternative {
		char op = 0;
		bool colon = false;
		std::string word;
	};

	static bool parseListAlternative(const std::string& body, std::size_t op_pos,
			ListAlternative& out) {
		std::size_t i = op_pos;
		out.colon = i < body.size() && body[i] == ':';
		if (out.colon) ++i;
		if (i >= body.size() || (body[i] != '+' && body[i] != '-')) return false;

		out.op = body[i];
		out.word = body.substr(i + 1);
		return out.word.find('@') != std::string::npos;
	}

	// `${a[@]+"${a[@]}"}`: an unquoted alternative word holding a quoted
	// list expands in place, so the elements stay separate words as in bash.
	bool Expander::renderListAlternative(const ListParam& list, const std::string& body,
			bool inside_dq, Tagged& out, bool& rendered) {
		rendered = false;
		ListAlternative alternative;
		if (inside_dq || !parseListAlternative(body, list.op_pos, alternative)) return false;

		const std::vector<std::string> values = listParamValues(list);
		const bool unset = values.empty();
		const bool missing = alternative.colon ? unset || (values.size() == 1 && values[0].empty())
			: unset;
		const bool uses_word = alternative.op == '+' ? !missing : missing;
		rendered = alternative.op == '+' && !uses_word;
		if (!uses_word) return false;

		Word word;
		if (!lexSingleWord(alternative.word, word)) return false;

		rendered = true;
		bool removed_null = false;
		for (const WordSegment& segment : word.segments) {
			if (renderSegment(segment, out, false)) removed_null = true;
		}

		return removed_null;
	}

	static std::size_t scalarParamNameEnd(const std::string& body) {
		if (body.empty()) return 0;
		if (isDigit(body[0])) return scanDigitsEnd(body, 0);
		if (!isNameStart(body[0])) return 0;

		std::size_t end = 1;
		while (end < body.size() && isNameCont(body[end])) ++end;
		return end;
	}

	static bool mentionsPositionalList(const std::string& word) {
		for (const char* form : { "$@", "$*", "${@", "${*", "[@]}", "[*]}" }) {
			if (word.find(form) != std::string::npos) return true;
		}

		return false;
	}

	static bool hasDoubleQuotedSegment(const Word& word) {
		return std::any_of(word.segments.begin(), word.segments.end(), [](const WordSegment& s) {
			return s.kind == WordSegment::Kind::DoubleQuoted;
		});
	}

	// The blanks around `${1+ "$@" }`'s word are unquoted text of the
	// expansion: a null IFS keeps them, a blank one splits them away.
	struct BlankEdges {
		std::string leading;
		std::string inner;
		std::string trailing;
	};

	static bool isWordBlank(char c) {
		return c == ' ' || c == '\t' || c == '\n';
	}

	static BlankEdges blankEdgesOf(const std::string& word) {
		std::size_t start = 0;
		std::size_t end = word.size();
		while (start < end && isWordBlank(word[start])) ++start;
		while (end > start && isWordBlank(word[end - 1])) --end;
		return { word.substr(0, start), word.substr(start, end - start), word.substr(end) };
	}

	static void pushSplittable(const std::string& text, bool inside_dq, Expander::Tagged& out) {
		for (const char c : text) out.push(c, inside_dq ? Expander::F_QUOTED : 0);
	}

	// `${1+"$@"}` and `"${1+$@}"`: a word holding `$@` (or `$*` with a null
	// IFS) that is used keeps the positional parameters separate words.
	// Inside double quotes an empty `$@` there still leaves the quoted
	// null: `"${1+"$@"}"` is one empty word without positionals, in bash.
	bool Expander::renderScalarListAlternative(const std::string& body, bool inside_dq,
			Tagged& out, bool& rendered) {
		rendered = false;
		const std::size_t name_end = scalarParamNameEnd(body);
		ListAlternative alternative;
		if (name_end == 0) return false;

		parseListAlternative(body, name_end, alternative);
		if (alternative.op == 0 || !mentionsPositionalList(alternative.word)) return false;

		const std::string name = body.substr(0, name_end);
		const bool unset = !isParamSet(name);
		const bool missing = unset || (alternative.colon && lookupParam(name, true).empty());
		const bool uses_word = alternative.op == '+' ? !missing : missing;
		const BlankEdges edges = blankEdgesOf(alternative.word);
		WordList list;
		if (!uses_word || !lexWordList(edges.inner, list)) return false;

		const bool quoted_part = std::any_of(list.words.begin(), list.words.end(),
			hasDoubleQuotedSegment);
		if (!inside_dq && !env_.ifs().empty() && !quoted_part) return false;

		rendered = true;
		const bool split_on_spaces = out.split_on_spaces;
		pushSplittable(edges.leading, inside_dq, out);
		bool removed_null = false;
		for (std::size_t k = 0; k < list.words.size(); ++k) {
			if (k > 0) pushSplittable(list.gaps[k - 1], inside_dq, out);
			for (const WordSegment& segment : list.words[k].segments) {
				if (renderSegment(segment, out, inside_dq)) removed_null = true;
			}
		}

		pushSplittable(edges.trailing, inside_dq, out);
		out.split_on_spaces = split_on_spaces;
		return removed_null && !inside_dq;
	}

	static bool isStarSlice(bool star, std::size_t at, const std::string& body) {
		if (!star || at >= body.size() || body[at] != ':') return false;

		return at + 1 >= body.size() || std::strchr("-=+?", body[at + 1]) == nullptr;
	}

	// bash uses DEL for a quoted null inside, and an unquoted `*` slice
	// that is assigned loses its DEL bytes to that.
	static void dropDelBytes(std::vector<std::string>& elems) {
		static const char kDel = '\177';

		for (std::string& elem : elems) {
			elem.erase(std::remove(elem.begin(), elem.end(), kDel), elem.end());
		}
	}

	// A body reached through `${!ref}` has a label and is never read as
	// another `!` or `#` form.
	bool Expander::renderParamBody(const std::string& body, const std::string& label,
			bool inside_dq, Tagged& out) {
		if (body == "@") {
			return renderArrayFields(env_.positional(), /*star=*/false, inside_dq, out);
		}

		if (body == "*" && !inside_dq) {
			return renderArrayFields(env_.positional(), /*star=*/true, inside_dq, out);
		}

		ListParam list;
		if (parseListParam(body, list)) {
			list.label = label;
			bool rendered = false;
			const bool removed_null = renderListAlternative(list, body, inside_dq, out, rendered);
			if (rendered) return removed_null;

			WordQuoting quoting;
			std::vector<std::string> elems = expandListParamQuoted(list, body, inside_dq, quoting);
			if (assignment_value_ && !inside_dq && isStarSlice(list.star, list.op_pos, body)) {
				dropDelBytes(elems);
			}

			if (!inside_dq && quoting.set && elems.size() == 1) {
				pushParamValue(out, elems[0], quoting, 0);
				return false;
			}

			return renderArrayFields(elems, list.star, inside_dq, out);
		}

		bool rendered = false;
		const bool removed_null = renderScalarListAlternative(body, inside_dq, out, rendered);
		if (rendered) return removed_null;

		const std::uint8_t mark = inside_dq ? F_QUOTED : 0;
		const std::string array_name = fieldAwareArrayName(body);
		if (array_name.empty()) {
			WordQuoting quoting;
			const std::string value = expandParam(body, inside_dq, &quoting, label);
			pushParamValue(out, value, quoting, mark);
			return false;
		}

		const bool indices = body[0] == '!';
		const bool star    = body.find('*') != std::string::npos;
		const std::vector<std::string> elems = indices ? arrayKeys(array_name)
			: arrayValues(array_name);
		return renderArrayFields(elems, star, inside_dq, out);
	}

	// The substituted command runs concurrently, connected through a
	// named pipe whose path takes the place of the word.
	void Expander::renderProcSubstSegment(const WordSegment& s, Tagged& out) {
		if (sub_ == nullptr) return;

		const std::string path = sub_->startProcessSubstitution(s.text, s.proc_dir);
		if (path.empty()) return;

		pushText(out, path, F_QUOTED);
		out.had_quote = true;
	}

	// bash warns as it reads the command that the substitution left a
	// here-document open, whose body then came from the lines after it.
	static void warnCarriedHeredocs(const WordSegment& s) {
		if (s.carried_heredocs == 0) return;

		shellErrorf("warning: command substitution: %zu unterminated here-document%s\n",
			s.carried_heredocs, s.carried_heredocs == 1 ? "" : "s");
	}

	bool Expander::renderSegment(const WordSegment& s, Tagged& out, bool inside_dq) {
		using K = WordSegment::Kind;
		const std::uint8_t mark = inside_dq ? F_QUOTED : 0;
		switch (s.kind) {
		case K::Literal:
			pushText(out, s.text, mark | F_NOSPLIT);
			break;
		case K::Escaped:
		case K::SingleQuoted:
			pushQuotedText(out, s.text);
			break;
		case K::DoubleQuoted:
			renderDoubleQuoted(s, out);
			break;
		case K::DollarSingle:
			pushQuotedText(out, interpretAnsiC(s.text));
			break;
		case K::SimpleVar:
			if (s.text == "@") {
				return renderArrayFields(env_.positional(), /*star=*/false, inside_dq, out);
			}

			// Unquoted, `$*` gives one field per parameter like `$@`; only
			// inside quotes does it join them with IFS.
			if (s.text == "*" && !inside_dq) {
				return renderArrayFields(env_.positional(), /*star=*/true, inside_dq, out);
			}

			if (env_.nounset() && isUnboundSpecial(s.text)) {
				failFatal("$" + s.text + ": unbound variable");
				break;
			}

			if (refersToWholeArray(s.text)) return renderWholeArrayRef(s.text, out, inside_dq);

			pushText(out, lookupParam(s.text), mark);
			break;
		case K::ParamExp:
			return renderParamExpSegment(s, out, inside_dq);
		case K::CmdSubst: {
			warnCarriedHeredocs(s);
			std::string output = runCmdSubst(s.text);
			stripTrailingNewlines(output);
			pushText(out, output, mark);
			break;
		}
		case K::ArithExp:
			pushText(out, std::to_string(evalArithExpansion(s.text)), mark);
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

	// Where no splitting happens a null IFS joins `$*` with nothing; a
	// quoted null is no text either.
	static bool isJoinedWithNothing(const Expander::Tagged& t, std::size_t i) {
		if ((t.flags[i] & Expander::F_QUOTED_NULL) != 0) return true;
		return t.text[i] == kNoSeparator && (t.flags[i] & Expander::F_FIELD_END) != 0;
	}

	static bool quotedAt(const Expander::Tagged& t, std::size_t i) {
		return (t.flags[i] & Expander::F_QUOTED) != 0;
	}

	static bool ifsHoldsChar(const std::string& ifs, const std::string& text, std::size_t at,
			std::size_t length) {
		for (std::size_t k = 0; k < ifs.size(); k += utf8CharLength(ifs, k)) {
			const std::size_t ifs_length = utf8CharLength(ifs, k);
			if (ifs_length == length && ifs.compare(k, length, text, at, length) == 0) return true;
		}

		return false;
	}

	// The byte length of the IFS character at t.text[i], or 0. IFS holds
	// characters: a byte inside a multibyte character never splits.
	static std::size_t ifsCharAt(const Expander::Tagged& t, std::size_t i, const std::string& ifs) {
		const std::uint8_t protection = Expander::F_QUOTED | Expander::F_NOSPLIT;
		if ((t.flags[i] & protection) != 0) return 0;

		const std::size_t length = utf8CharLength(t.text, i);
		return ifsHoldsChar(ifs, t.text, i, length) ? length : 0;
	}

	static bool splitsAt(const Expander::Tagged& t, std::size_t i, const std::string& ifs) {
		return ifsCharAt(t, i, ifs) != 0;
	}

	static Expander::Tagged takeField(const Expander::Tagged& t, const std::string& ifs,
			std::size_t& i) {
		Expander::Tagged field;
		while (i < t.size() && !splitsAt(t, i, ifs)) {
			const std::size_t end = i + utf8CharLength(t.text, i);
			for (; i < end; ++i) {
				field.push(t.text[i], t.flags[i]);
				field.had_quote = field.had_quote || quotedAt(t, i);
			}
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
			const std::size_t length = ifsCharAt(t, i, ifs);
			if (length == 0) break;
			if (!isIfsWhitespace(ifs, t.text[i])) {
				if (saw_nonws) break;
				saw_nonws = true;
			}

			i += length;
		}
	}

	// IFS-split a text that holds no F_FIELD_END marker. A text with a
	// quoted part gives at least one field, even an empty one.
	static void splitOnIfs(const Expander::Tagged& t, const std::string& ifs,
			std::vector<Expander::Tagged>& out) {
		const std::size_t first = out.size();
		std::size_t i = 0;
		while (!ifs.empty() && i < t.size() && splitsAt(t, i, ifs) && isIfsWhitespace(ifs, t.text[i])) {
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
		const std::string ifs = t.split_on_spaces ? std::string(" ") : env_.ifs();
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

	// As bash's unquoted_glob_pattern_p, a plain `/` ends the bracket
	// expression; a backslash-escaped character, `\/` included, does not.
	static bool hasUnquotedCloseBracket(const std::string& text,
			const std::vector<std::uint8_t>& flags, std::size_t from) {
		for (std::size_t k = from; k < text.size(); ++k) {
			if ((flags[k] & Expander::F_QUOTED) != 0) continue;
			if (text[k] == '\\') ++k;
			else if (text[k] == '/') return false;
			else if (text[k] == ']') return true;
		}

		return false;
	}

	// An unquoted wildcard, or with extglob an unquoted `X(` group
	// opener; `flags` marks quoted characters with F_QUOTED. As in bash,
	// a `[` with no unquoted `]` after it is literal, so a bare `[`
	// command survives nullglob.
	static bool isUnquotedMetaAt(const std::string& text, const std::vector<std::uint8_t>& flags,
			std::size_t i, bool extglob) {
		const auto quoted = [&flags](std::size_t k) {
			return (flags[k] & Expander::F_QUOTED) != 0;
		};
		if (quoted(i)) return false;
		if (text[i] == '*' || text[i] == '?') return true;
		if (text[i] == '[') return hasUnquotedCloseBracket(text, flags, i + 1);
		return extglob && isExtglobOperator(text[i]) && i + 1 < text.size()
			&& text[i + 1] == '(' && !quoted(i + 1);
	}

	namespace glob_detail {

		// One `/`-separated path component with per-char quoting so a
		// quoted `*` stays literal.
		struct GlobComp {
			std::string text;
			std::vector<std::uint8_t> quoted;

			bool hasMeta(bool extglob) const {
				for (std::size_t i = 0; i < text.size(); ++i) {
					if (isUnquotedMetaAt(text, quoted, i, extglob)) return true;
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

			// As bash's dequote_pathname: an unquoted backslash, as from
			// `$var`, quotes the character after it.
			std::string asLiteral() const {
				std::string literal;
				for (std::size_t i = 0; i < text.size(); ++i) {
					if (text[i] == '\\' && quoted[i] == 0) ++i;
					if (i < text.size()) literal.push_back(text[i]);
				}

				return literal;
			}
		};

		struct GlobMatchOptions {
			bool dotglob    = false;
			bool nocaseglob = false;
			bool extglob    = false;
			bool globstar   = false;
			bool skipdots   = true;
		};

		struct GlobPlan {
			std::vector<GlobComp> comps;
			bool absolute = false;
			bool trailing_slash = false;
		};

		static std::string joinDir(const std::string& dir, const std::string& name) {
			if (dir.empty()) return name;
			if (dir == "/") return "/" + name;
			if (dir.back() == '/') return dir + name;
			return dir + "/" + name;
		}

	}  // namespace glob_detail

	static bool taggedHasUnquotedGlobMeta(const Expander::Tagged& t, bool extglob) {
		for (std::size_t i = 0; i < t.size(); ++i) {
			if (isUnquotedMetaAt(t.text, t.flags, i, extglob)) return true;
		}

		return false;
	}

	static glob_detail::GlobPlan splitTaggedIntoGlobPlan(const Expander::Tagged& t) {
		glob_detail::GlobPlan plan;
		glob_detail::GlobComp cur;
		for (std::size_t i = 0; i < t.size(); ++i) {
			if (isJoinedWithNothing(t, i)) continue;

			const char c = t.text[i];
			const bool quoted = quotedAt(t, i);
			if (c == '/') {
				if (i == 0) plan.absolute = true;
				if (!cur.text.empty()) {
					plan.comps.push_back(std::move(cur));
					cur = {};
				}

				continue;
			}

			cur.text.push_back(c);
			cur.quoted.push_back(quoted ? Expander::F_QUOTED : 0);
		}

		if (!cur.text.empty()) plan.comps.push_back(std::move(cur));
		else plan.trailing_slash = !plan.comps.empty();
		return plan;
	}

	static bool isGlobstarComp(const glob_detail::GlobComp& comp,
			const glob_detail::GlobMatchOptions& options) {
		return options.globstar && comp.text == "**" && comp.hasMeta(false);
	}

	// The walker's paths all start with the listed directory, so the
	// relative part is a plain suffix; fs::relative would hit the disk.
	static std::string globstarRelative(const std::filesystem::path& full,
			const std::wstring& prefix) {
		const std::wstring& text = full.native();
		std::string rel = pathToUtf8(text.compare(0, prefix.size(), prefix) == 0
			? std::filesystem::path(text.substr(prefix.size())) : full.filename());
		std::replace(rel.begin(), rel.end(), '\\', '/');
		return rel;
	}

	static std::wstring globstarPrefix(const std::filesystem::path& list_dir) {
		std::wstring prefix = list_dir.native();
		if (!prefix.empty() && prefix.back() != L'\\' && prefix.back() != L'/') prefix += L'\\';
		return prefix;
	}

	// What a `**` lists below a directory. Before another component bash
	// goes on only from real directories, not from links to them.
	enum class GlobstarListing { Everything, Directories, WalkableDirectories };

	static bool globstarLists(const std::filesystem::directory_entry& entry,
			GlobstarListing listing) {
		if (listing == GlobstarListing::Everything) return true;

		std::error_code ec;
		if (!entry.is_directory(ec)) return false;
		return listing == GlobstarListing::Directories || !entry.is_symlink(ec);
	}

	// Dot names are neither listed nor entered unless dotglob is on.
	static void collectGlobstarUnder(const std::string& dir, GlobstarListing listing,
			bool dotglob, const PathConv& pc, std::vector<std::string>& found) {
		namespace fs = std::filesystem;
		const fs::path list_dir = utf8ToPath(pc.toWin32(dir.empty() ? "." : dir));
		const std::wstring prefix = globstarPrefix(list_dir);

		std::error_code ec;
		fs::recursive_directory_iterator walker(list_dir,
			fs::directory_options::skip_permission_denied, ec);
		if (ec) return;

		for (; walker != fs::recursive_directory_iterator(); walker.increment(ec)) {
			if (ec) break;

			const std::wstring name = walker->path().filename().native();
			if (!dotglob && !name.empty() && name[0] == L'.') {
				walker.disable_recursion_pending();
				continue;
			}

			if (!globstarLists(*walker, listing)) continue;
			found.push_back(glob_detail::joinDir(dir, globstarRelative(walker->path(), prefix)));
		}
	}

	enum class GlobstarPlace { Inner, Last, LastAfterGlobstar, LastAfterPattern };

	// As the last component after a literal directory `**` yields that
	// directory as `dir/` (none for the implicit current directory) and
	// everything below it; elsewhere it yields the directory and the
	// directories below it. Right after another `**` it yields the
	// directory as it is (`**/**` is `**`); after a pattern, bash lists
	// every matched directory with all that is below it, so a path below
	// two of them comes twice (`**/a/**`).
	static void addGlobstarStart(const std::string& dir, GlobstarPlace place,
			std::vector<std::string>& found) {
		if (place == GlobstarPlace::Inner) {
			found.push_back(dir);
			return;
		}

		if (dir.empty()) return;

		const bool slashed = place == GlobstarPlace::Last && dir.back() != '/';
		found.push_back(slashed ? dir + "/" : dir);
	}

	static void dropDuplicatePaths(std::vector<std::string>& paths) {
		std::sort(paths.begin(), paths.end());
		paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
	}

	static bool globPathExists(const std::string& path, bool want_dir, const PathConv& pc);

	// A literal component before the `**` was joined on blindly, so only
	// the directories that exist are walked.
	static std::vector<std::string> expandGlobstarStep(const std::vector<std::string>& current,
			GlobstarPlace place, bool dirs_only, const glob_detail::GlobMatchOptions& options,
			const PathConv& pc) {
		GlobstarListing listing = GlobstarListing::Everything;
		if (dirs_only) listing = GlobstarListing::Directories;
		if (place == GlobstarPlace::Inner) listing = GlobstarListing::WalkableDirectories;

		std::vector<std::string> found;
		for (const std::string& dir : current) {
			if (!dir.empty() && !globPathExists(dir, true, pc)) continue;

			addGlobstarStart(dir, place, found);
			collectGlobstarUnder(dir, listing, options.dotglob, pc, found);
		}

		if (place != GlobstarPlace::LastAfterPattern) dropDuplicatePaths(found);
		return found;
	}

	static std::string lowerAscii(std::string text) {
		for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return text;
	}

	static bool globNameMatches(const std::string& pat, const std::string& name,
			const glob_detail::GlobMatchOptions& options) {
		const LeadingDot dots = options.dotglob ? LeadingDot::NotDotDot : LeadingDot::Explicit;
		if (!options.nocaseglob) return fnmatchName(pat, name, options.extglob, dots);
		return fnmatchName(lowerAscii(pat), lowerAscii(name), options.extglob, dots);
	}

	static bool globNameWanted(const std::string& name, const std::string& pat,
			const glob_detail::GlobMatchOptions& options) {
		if (name.empty()) return false;

		const GlobSkipRules rules{ options.extglob, options.dotglob, options.skipdots };
		if (globSkipsName(pat, name, rules)) return false;
		return globNameMatches(pat, name, options);
	}

	// A link out of /proc, such as cwd, counts by what it leads to.
	static bool isProcDirectory(const std::string& posix_path, const PathConv& pc) {
		const ProcNode node = lookupProcPath(pc, posix_path, true);
		if (node.type != ProcType::Outside) return node.type == ProcType::Directory;

		std::error_code ec;
		return std::filesystem::is_directory(utf8ToPath(pc.toWin32(node.target)), ec);
	}

	static std::vector<std::string> matchingProcEntries(const std::string& dir,
			const std::string& pat, const PathConv& pc,
			const glob_detail::GlobMatchOptions& options, bool dirs_only) {
		std::vector<std::string> matches;
		for (const std::string& name : listProcDirectory(pc, dir)) {
			if (!globNameWanted(name, pat, options)) continue;
			if (dirs_only && !isProcDirectory(glob_detail::joinDir(dir, name), pc)) continue;
			matches.push_back(name);
		}

		return matches;
	}

	// The entry's type comes from the directory listing itself, so the
	// directory test costs no extra call except on links.
	static std::vector<std::string> matchingDiskEntries(const std::string& dir,
			const std::string& pat, const PathConv& pc,
			const glob_detail::GlobMatchOptions& options, bool dirs_only) {
		namespace fs = std::filesystem;
		std::error_code ec;
		fs::directory_iterator it(utf8ToPath(pc.toWin32(dir)), ec);
		if (ec) return {};

		std::vector<std::string> matches;
		for (const char* dot_name : { ".", ".." }) {
			if (globNameWanted(dot_name, pat, options)) matches.emplace_back(dot_name);
		}

		for (const fs::directory_entry& entry : it) {
			std::string name = pathToUtf8(entry.path().filename());
			if (!globNameWanted(name, pat, options)) continue;

			std::error_code type_ec;
			if (dirs_only && !entry.is_directory(type_ec)) continue;
			matches.push_back(std::move(name));
		}

		return matches;
	}

	static std::vector<std::string> matchingEntries(const std::string& dir, const std::string& pat,
			const PathConv& pc, const glob_detail::GlobMatchOptions& options, bool dirs_only) {
		const std::string posix_dir = dir.empty() ? "." : dir;
		if (isProcPath(posix_dir)) {
			return matchingProcEntries(posix_dir, pat, pc, options, dirs_only);
		}

		return matchingDiskEntries(posix_dir, pat, pc, options, dirs_only);
	}

	static std::vector<std::string> expandGlobOneStep(const std::vector<std::string>& current,
			const glob_detail::GlobComp& comp, const PathConv& pc,
			const glob_detail::GlobMatchOptions& options, bool dirs_only) {
		std::vector<std::string> next;
		if (!comp.hasMeta(options.extglob)) {
			for (const std::string& dir : current) {
				next.push_back(glob_detail::joinDir(dir, comp.asLiteral()));
			}

			return next;
		}

		const std::string pat = comp.asPattern();
		for (const std::string& dir : current) {
			for (const std::string& name : matchingEntries(dir, pat, pc, options, dirs_only)) {
				next.push_back(glob_detail::joinDir(dir, name));
			}
		}

		return next;
	}

	static bool globPathExists(const std::string& path, bool want_dir, const PathConv& pc) {
		namespace fs = std::filesystem;
		if (isProcPath(path)) {
			if (want_dir) return isProcDirectory(path, pc);
			return lookupProcPath(pc, path, true).type != ProcType::Missing;
		}

		std::error_code ec;
		const fs::file_status status = fs::status(utf8ToPath(pc.toWin32(path)), ec);
		return want_dir ? fs::is_directory(status) : fs::exists(status);
	}

	// Literal components after the last pattern were joined on blindly;
	// only paths that exist count as matches.
	static void keepExistingPaths(std::vector<std::string>& paths, bool want_dir,
			const PathConv& pc) {
		const auto missing = [&](const std::string& path) {
			return !globPathExists(path, want_dir, pc);
		};
		paths.erase(std::remove_if(paths.begin(), paths.end(), missing), paths.end());
	}

	static void appendTrailingSlashes(std::vector<std::string>& paths) {
		for (std::string& path : paths) {
			if (path.empty() || path.back() != '/') path.push_back('/');
		}
	}

	static std::size_t globstarRunStart(const glob_detail::GlobPlan& plan, std::size_t ci,
			const glob_detail::GlobMatchOptions& options) {
		std::size_t start = ci;
		while (start > 0 && isGlobstarComp(plan.comps[start - 1], options)) --start;
		return start;
	}

	// A run of `**` is one step, placed by what comes before the run.
	static GlobstarPlace globstarPlace(const glob_detail::GlobPlan& plan, std::size_t ci,
			const glob_detail::GlobMatchOptions& options) {
		if (ci + 1 != plan.comps.size()) return GlobstarPlace::Inner;

		const std::size_t run_start = globstarRunStart(plan, ci, options);
		for (std::size_t k = 0; k < run_start; ++k) {
			if (plan.comps[k].hasMeta(options.extglob)) return GlobstarPlace::LastAfterPattern;
		}

		return run_start < ci ? GlobstarPlace::LastAfterGlobstar : GlobstarPlace::Last;
	}

	static bool repeatsGlobstar(const glob_detail::GlobPlan& plan, std::size_t ci,
			const glob_detail::GlobMatchOptions& options) {
		return ci + 1 < plan.comps.size() && isGlobstarComp(plan.comps[ci], options)
			&& isGlobstarComp(plan.comps[ci + 1], options);
	}

	// A component that is not the last only ever names directories.
	static std::vector<std::string> expandGlobPlan(const glob_detail::GlobPlan& plan,
			const glob_detail::GlobMatchOptions& options, const PathConv& pc) {
		std::vector<std::string> current{ plan.absolute ? std::string("/") : std::string() };
		std::size_t literal_tail = 0;
		for (std::size_t ci = 0; ci < plan.comps.size(); ++ci) {
			const glob_detail::GlobComp& comp = plan.comps[ci];
			if (repeatsGlobstar(plan, ci, options)) continue;

			const bool last = ci + 1 == plan.comps.size();
			const bool dirs_only = !last || plan.trailing_slash;
			if (isGlobstarComp(comp, options)) {
				const GlobstarPlace place = globstarPlace(plan, ci, options);
				current = expandGlobstarStep(current, place, dirs_only, options, pc);
			} else {
				current = expandGlobOneStep(current, comp, pc, options, dirs_only);
			}

			literal_tail = comp.hasMeta(options.extglob) ? 0 : literal_tail + 1;
		}

		if (literal_tail != 0) keepExistingPaths(current, plan.trailing_slash, pc);
		if (plan.trailing_slash) appendTrailingSlashes(current);
		return current;
	}

	// bash's skip_to_delim with SD_GLOB and SD_EXTGLOB: a `:` inside a
	// bracket expression or an extglob group does not end the pattern.
	static std::size_t globIgnoreStepEnd(const std::string& value, std::size_t i) {
		const bool group = i + 1 < value.size() && isExtglobOperator(value[i]) && value[i + 1] == '(';
		const std::size_t group_end = group ? extglobGroupEnd(value, i) : std::string::npos;
		if (group_end != std::string::npos) return group_end;
		if (value[i] == '[') return bracketEnd(value, i);
		return i + (value[i] == '\\' ? 2 : 1);
	}

	static std::size_t globIgnoreItemEnd(const std::string& value, std::size_t from) {
		std::size_t i = from;
		while (i < value.size() && value[i] != ':') i = globIgnoreStepEnd(value, i);
		return std::min(i, value.size());
	}

	static std::vector<std::string> globIgnorePatterns(const Environment& env) {
		std::vector<std::string> patterns;
		if (!env.has("GLOBIGNORE")) return patterns;

		const std::string value = env.get("GLOBIGNORE");
		std::size_t from = 0;
		while (from <= value.size()) {
			const std::size_t colon = globIgnoreItemEnd(value, from);
			if (colon > from) patterns.push_back(value.substr(from, colon - from));
			from = colon + 1;
		}

		return patterns;
	}

	static void dropIgnoredPaths(std::vector<std::string>& paths,
			const std::vector<std::string>& patterns, bool extglob) {
		const auto ignored = [&](const std::string& path) {
			for (const std::string& pattern : patterns) {
				if (fnmatchFull(pattern, path, extglob)) return true;
			}

			return false;
		};
		paths.erase(std::remove_if(paths.begin(), paths.end(), ignored), paths.end());
	}

	static glob_detail::GlobMatchOptions globMatchOptions(const Environment& env) {
		glob_detail::GlobMatchOptions options;
		options.dotglob    = env.dotglob();
		options.nocaseglob = env.nocaseglob();
		options.extglob    = env.extglob();
		options.globstar   = env.globstar();
		options.skipdots   = env.shopts().globskipdots;
		return options;
	}

	// bash sorts the whole result, not each directory's share of it.
	std::vector<std::string> Expander::globExpand(const Tagged& t) {
		if (env_.noglob()) return { quoteRemove(t) };
		if (!taggedHasUnquotedGlobMeta(t, env_.extglob())) return { quoteRemove(t) };

		const glob_detail::GlobPlan plan = splitTaggedIntoGlobPlan(t);
		if (plan.comps.empty()) return { quoteRemove(t) };

		const std::vector<std::string> ignore = globIgnorePatterns(env_);
		std::vector<std::string> matches = expandGlobPlan(plan,
			globMatchOptions(env_), path_conv_);
		if (!ignore.empty()) dropIgnoredPaths(matches, ignore, env_.extglob());

		if (!matches.empty()) {
			sortGlobMatches(matches, env_.get("GLOBSORT"), path_conv_);
			return matches;
		}

		if (env_.shopts().failglob) fail("no match: " + quoteRemove(t));
		if (env_.shopts().failglob || env_.nullglob()) return {};
		return { quoteRemove(t) };
	}

	std::string Expander::quoteRemove(const Tagged& t) {
		std::string out;
		out.reserve(t.size());
		for (std::size_t i = 0; i < t.size(); ++i) {
			if (!isJoinedWithNothing(t, i)) out.push_back(t.text[i]);
		}

		return out;
	}

	// Brace expansion follows bash's braces.c step for step: which `{`
	// starts an expansion, which sequences are valid and how the pieces
	// combine all decide the literal text bash leaves behind.
	enum class BraceType { None, Comma, Sequence };
	enum class SeqType { Bad, Int, PaddedInt, Char };
	enum class BraceStep { Plain, Consumed, Unterminated };

	// One brace_gobbler scan for `satisfy` from `i`.
	struct BraceScan {
		const std::string& text;
		std::size_t i;
		char satisfy;
		int separators;
		int level = 0;
		char quoted = 0;
		BraceType type = BraceType::None;
	};

	static BraceScan braceScanFor(const std::string& text, std::size_t from, char satisfy) {
		return BraceScan{ text, from, satisfy, satisfy == '}' ? 0 : 1 };
	}

	static char braceCharAt(const std::string& text, std::size_t i) {
		return i < text.size() ? text[i] : '\0';
	}

	static bool isBraceBlank(char c) {
		return c == '\0' || c == ' ' || c == '\t' || c == '\n';
	}

	// A `{` between blanks, or before a `}`, is an ordinary word.
	static bool isLoneOpenBrace(const std::string& text, std::size_t i) {
		const bool blank_before = i == 0 || isBraceBlank(text[i - 1]);
		const char next = braceCharAt(text, i + 1);
		return blank_before && (isBraceBlank(next) || next == '}');
	}

	static BraceStep braceSkipSubstitution(BraceScan& scan) {
		const std::size_t close = Lexer::commandSubstitutionEnd(scan.text, scan.i + 2);
		if (close == std::string::npos) {
			scan.i = scan.text.size();
			return BraceStep::Unterminated;
		}

		scan.i = close + 1;
		return BraceStep::Consumed;
	}

	static BraceStep braceSkipQuoted(BraceScan& scan, char c) {
		if (c == scan.quoted) scan.quoted = 0;
		if (scan.quoted == '"' && c == '$' && braceCharAt(scan.text, scan.i + 1) == '(') {
			return braceSkipSubstitution(scan);
		}

		++scan.i;
		return BraceStep::Consumed;
	}

	static bool escapesInBraceScan(char quoted) {
		return quoted == 0 || quoted == '"' || quoted == '`';
	}

	static BraceStep braceSkipQuoting(BraceScan& scan) {
		const char c = scan.text[scan.i];
		const char next = braceCharAt(scan.text, scan.i + 1);
		if (c == '\\' && escapesInBraceScan(scan.quoted)) {
			scan.i = (std::min)(scan.i + 2, scan.text.size());
			return BraceStep::Consumed;
		}

		if (c == '$' && next == '{' && scan.quoted != '\'') {
			scan.i += 2;
			if (scan.quoted == 0) ++scan.level;
			return BraceStep::Consumed;
		}

		if (scan.quoted != 0) return braceSkipQuoted(scan, c);
		if (c == '"' || c == '\'' || c == '`') {
			scan.quoted = c;
			++scan.i;
			return BraceStep::Consumed;
		}

		if ((c == '$' || c == '<' || c == '>') && next == '(') return braceSkipSubstitution(scan);
		return BraceStep::Plain;
	}

	static void braceNoteNesting(BraceScan& scan) {
		const char c = scan.text[scan.i];
		if (c == '{') {
			++scan.level;
			return;
		}

		if (c == '}' && scan.level > 0) {
			--scan.level;
			return;
		}

		if (scan.satisfy != '}' || scan.level != 0) return;

		if (c == ',') {
			scan.type = BraceType::Comma;
			++scan.separators;
			return;
		}

		const bool dots = scan.text.compare(scan.i, 2, "..") == 0
			&& braceCharAt(scan.text, scan.i + 2) != '}';
		if (dots && scan.type == BraceType::None) {
			scan.type = BraceType::Sequence;
			++scan.separators;
		}
	}

	// True with `scan.i` at the character looked for; false with it at the end.
	static bool braceGobble(BraceScan& scan) {
		while (scan.i < scan.text.size()) {
			const BraceStep step = braceSkipQuoting(scan);
			if (step == BraceStep::Unterminated) return false;
			if (step == BraceStep::Consumed) continue;

			const char c = scan.text[scan.i];
			const bool wanted = c == scan.satisfy && scan.level == 0 && scan.separators > 0;
			if (wanted && !(c == '{' && isLoneOpenBrace(scan.text, scan.i))) return true;

			if (!wanted) braceNoteNesting(scan);
			++scan.i;
		}

		return false;
	}

	static SeqType seqTermStart(const std::string& text, std::size_t at, bool is_rhs) {
		const char c = braceCharAt(text, at);
		const char next = braceCharAt(text, at + 1);
		const bool signed_digit = (c == '+' || c == '-') && isDigit(next);
		if (isDigit(c) || signed_digit) return SeqType::Int;

		const bool letter = std::isalpha(static_cast<unsigned char>(c)) != 0;
		const bool ends = next == '.' || (is_rhs && next == '}');
		return letter && ends ? SeqType::Char : SeqType::Bad;
	}

	// The cheap check bash makes before it takes `{x..y}` for a sequence.
	static bool validSeqterm(const std::string& text, std::size_t start, std::size_t end) {
		const std::size_t dots = text.find("..", start);
		if (dots == std::string::npos || dots + 2 > end) return false;

		const std::size_t rhs = dots + 2;
		if (text[start] == '.' || braceCharAt(text, rhs) == '}') return false;

		const SeqType lhs_type = seqTermStart(text, start, false);
		return lhs_type != SeqType::Bad && lhs_type == seqTermStart(text, rhs, true);
	}

	static bool braceOpensExpansion(const std::string& text, std::size_t open) {
		BraceScan scan = braceScanFor(text, open + 1, '}');
		if (!braceGobble(scan)) return false;
		if (scan.type != BraceType::Sequence) return true;

		return validSeqterm(text, open + 1, scan.i);
	}

	static std::size_t braceFindExpansion(const std::string& text) {
		std::size_t from = 0;
		for (;;) {
			BraceScan scan = braceScanFor(text, from, '{');
			if (!braceGobble(scan)) return std::string::npos;
			if (braceOpensExpansion(text, scan.i)) return scan.i;

			from = scan.i + 1;
		}
	}

	// strtoimax: blanks, a sign and at least one digit; false on overflow
	// or no digits, with `pos` past what was read.
	static bool braceParseNumber(const std::string& text, std::size_t& pos, long long& value) {
		std::size_t k = pos;
		while (k < text.size() && std::isspace(static_cast<unsigned char>(text[k])) != 0) ++k;

		const bool negative = k < text.size() && text[k] == '-';
		if (k < text.size() && (text[k] == '-' || text[k] == '+')) ++k;
		if (k >= text.size() || !isDigit(text[k])) return false;

		unsigned long long magnitude = 0;
		const unsigned long long limit = static_cast<unsigned long long>(LLONG_MAX) + (negative ? 1 : 0);
		for (; k < text.size() && isDigit(text[k]); ++k) {
			magnitude = magnitude * 10 + static_cast<unsigned long long>(text[k] - '0');
			if (magnitude > limit) return false;
		}

		pos = k;
		value = negative ? static_cast<long long>(0 - magnitude) : static_cast<long long>(magnitude);
		return true;
	}

	struct SeqTerm {
		SeqType type = SeqType::Bad;
		long long value = 0;
		std::size_t length = 0;
	};

	static SeqTerm braceSeqLhs(const std::string& lhs) {
		SeqTerm term;
		term.length = lhs.size();
		std::size_t pos = 0;
		if (braceParseNumber(lhs, pos, term.value)) {
			while (pos < lhs.size() && (lhs[pos] == ' ' || lhs[pos] == '\t')) ++pos;
			if (pos == lhs.size()) term.type = SeqType::Int;
			return term;
		}

		if (lhs.size() == 1 && std::isalpha(static_cast<unsigned char>(lhs[0])) != 0) {
			term.type = SeqType::Char;
			term.value = static_cast<unsigned char>(lhs[0]);
		}

		return term;
	}

	// The right-hand term up to an optional `..incr`; `pos` ends past it.
	static SeqTerm braceSeqRhsTerm(const std::string& rhs, std::size_t& pos) {
		SeqTerm term;
		const SeqType start = seqTermStart(rhs + '}', 0, true);
		if (start == SeqType::Int) {
			const bool parsed = braceParseNumber(rhs, pos, term.value);
			const bool ends = pos == rhs.size() || rhs[pos] == '.';
			if (parsed && ends) term.type = SeqType::Int;
		} else if (start == SeqType::Char) {
			term.type = SeqType::Char;
			term.value = static_cast<unsigned char>(rhs[0]);
			pos = 1;
		}

		term.length = pos;
		return term;
	}

	static bool braceSeqIncrement(const std::string& rhs, std::size_t pos, long long& incr) {
		incr = 1;
		const bool has_incr = rhs.compare(pos, 2, "..") == 0 && pos + 2 < rhs.size();
		if (!has_incr) return pos == rhs.size();

		pos += 2;
		return braceParseNumber(rhs, pos, incr) && pos == rhs.size();
	}

	static bool hasZeroPadding(const std::string& term, std::size_t length) {
		if (length > 1 && term[0] == '0') return true;
		return length > 2 && term[0] == '-' && term[1] == '0';
	}

	static std::size_t braceSeqWidth(const std::string& lhs, const SeqTerm& from,
			const std::string& rhs, const SeqTerm& to) {
		const bool padded = hasZeroPadding(lhs, from.length) || hasZeroPadding(rhs, to.length);
		return padded ? (std::max)(from.length, to.length) : 0;
	}

	static std::string braceSeqElement(long long n, SeqType type, std::size_t width) {
		if (type == SeqType::Char) return std::string(1, static_cast<char>(n));

		std::string text = std::to_string(n);
		if (type == SeqType::PaddedInt && text.size() < width) {
			text.insert(n < 0 ? 1 : 0, width - text.size(), '0');
		}

		return text;
	}

	// bash's mkseq; empty when the numbers overflow.
	static std::vector<std::string> braceMakeSequence(long long start, long long end,
			long long incr, SeqType type, std::size_t width) {
		if (incr == 0) incr = 1;
		if (incr == LLONG_MIN) return {};

		const unsigned long long step = static_cast<unsigned long long>(incr < 0 ? -incr : incr);
		if ((start < end) == (incr < 0)) incr = -incr;

		const unsigned long long distance = start < end
			? static_cast<unsigned long long>(end) - static_cast<unsigned long long>(start)
			: static_cast<unsigned long long>(start) - static_cast<unsigned long long>(end);
		if (distance > static_cast<unsigned long long>(LLONG_MAX)) return {};

		std::vector<std::string> out;
		const unsigned long long count = distance / step + 1;
		long long n = start;
		for (unsigned long long k = 0; k < count; ++k) {
			out.push_back(braceSeqElement(n, type, width));
			if (k + 1 < count) n += incr;
		}

		return out;
	}

	// bash's expand_seqterm; empty when the body is no valid sequence.
	static std::vector<std::string> braceExpandSequence(const std::string& amble) {
		const std::size_t dots = amble.find("..");
		if (dots == std::string::npos) return {};

		const std::string lhs = amble.substr(0, dots);
		const std::string rhs = amble.substr(dots + 2);
		if (lhs.empty() || rhs.empty()) return {};

		const SeqTerm from = braceSeqLhs(lhs);
		std::size_t pos = 0;
		const SeqTerm to = braceSeqRhsTerm(rhs, pos);
		long long incr = 1;
		if (to.type == SeqType::Bad || !braceSeqIncrement(rhs, pos, incr)) return {};
		if (from.type != to.type || from.type == SeqType::Bad) return {};

		if (from.type == SeqType::Char) {
			return braceMakeSequence(from.value, to.value, incr, SeqType::Char, 1);
		}

		const std::size_t width = braceSeqWidth(lhs, from, rhs, to);
		const SeqType type = width > 0 ? SeqType::PaddedInt : SeqType::Int;
		return braceMakeSequence(from.value, to.value, incr, type, width);
	}

	// bash's array_concat: every left piece followed by every right one.
	static std::vector<std::string> braceConcat(const std::vector<std::string>& left,
			const std::vector<std::string>& right) {
		if (left.size() == 1 && left[0].empty()) return right;
		if (right.size() == 1 && right[0].empty()) return left;

		std::vector<std::string> out;
		out.reserve(left.size() * right.size());
		for (const std::string& head : left) {
			for (const std::string& tail : right) out.push_back(head + tail);
		}

		return out;
	}

	static std::vector<std::string> braceExpandText(const std::string& text);

	static std::vector<std::string> braceExpandAlternatives(const std::string& amble) {
		std::vector<std::string> out;
		std::size_t start = 0;
		for (;;) {
			BraceScan scan = braceScanFor(amble, start, ',');
			const bool found = braceGobble(scan);
			for (std::string& piece : braceExpandText(amble.substr(start, scan.i - start))) {
				out.push_back(std::move(piece));
			}

			if (!found) return out;
			start = scan.i + 1;
		}
	}

	// The expansion of the braces at [open, close]; empty when the whole
	// text stays as it is.
	static std::vector<std::string> braceExpandAmble(const std::string& text, std::size_t open,
			std::size_t close, BraceType type) {
		const std::string amble = text.substr(open + 1, close - open - 1);
		if (type != BraceType::Sequence) return braceExpandAlternatives(amble);

		std::vector<std::string> sequence = braceExpandSequence(amble);
		if (!sequence.empty()) return sequence;
		if (close + 1 < text.size()) return { text.substr(open, close - open + 1) };
		return {};
	}

	static std::vector<std::string> braceExpandText(const std::string& text) {
		const std::size_t open = braceFindExpansion(text);
		if (open == std::string::npos) return { text };

		BraceScan scan = braceScanFor(text, open + 1, '}');
		if (!braceGobble(scan)) return { text };

		const std::vector<std::string> amble = braceExpandAmble(text, open, scan.i, scan.type);
		if (amble.empty()) return { text };

		const std::vector<std::string> result = braceConcat({ text.substr(0, open) }, amble);
		const std::string postamble = text.substr(scan.i + 1);
		if (postamble.empty()) return result;

		return braceConcat(result, braceExpandText(postamble));
	}

	// Unquoted source text with nothing a brace, tilde or pathname
	// expansion could act on, and literal text is never split: the word
	// is its own single field.
	static bool isPlainLiteralWord(const Word& w) {
		if (w.segments.size() != 1 || w.segments[0].kind != WordSegment::Kind::Literal) return false;

		const std::string& text = w.segments[0].text;
		return !text.empty() && text == w.raw && text.find_first_of("*?[{~(") == std::string::npos;
	}

	// Quote removal drops a backslash that ends a brace-expanded word, as
	// in the `\` that `{Z..a}` yields.
	static std::string withoutDanglingBackslash(const std::string& text) {
		std::size_t backslashes = 0;
		while (backslashes < text.size() && text[text.size() - 1 - backslashes] == '\\') {
			++backslashes;
		}

		if (backslashes % 2 == 0) return text;
		return text.substr(0, text.size() - 1);
	}

	void Expander::expandBraceAlternative(const std::string& alt, const Word& w,
			std::vector<std::string>& out) {
		const std::string text = withoutDanglingBackslash(alt);
		if (text.empty() && !alt.empty()) {
			out.emplace_back();
			return;
		}

		Lexer lexer(text);
		std::vector<Token> tokens = lexer.tokenize();
		if (!lexer.errors().empty()) {
			out.push_back(text);
			return;
		}

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

	std::vector<std::string> Expander::expandWord(const Word& w) {
		if (isPlainLiteralWord(w) && !aborting()) return { w.segments[0].text };
		if (!env_.braceexpand()) return expandWordPostBrace(w);

		const std::vector<std::string> braced = braceExpandText(w.raw);
		if (braced.size() == 1 && braced.front() == w.raw) return expandWordPostBrace(w);

		std::vector<std::string> out;
		for (const std::string& alt : braced) {
			if (aborting()) break;

			expandBraceAlternative(alt, w, out);
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
		const Word tilded = expandTildePrefixes(env_, path_conv_, w, scope);

		const bool outer = std::exchange(assignment_value_, true);
		std::string value = quoteRemove(renderWord(tilded));
		assignment_value_ = outer;
		return value;
	}

	static std::string taggedAsPattern(const Expander::Tagged& t) {
		std::string pattern;
		pattern.reserve(t.size());
		for (std::size_t i = 0; i < t.size(); ++i) {
			if (isJoinedWithNothing(t, i)) continue;
			if (quotedAt(t, i)) appendFnmatchLiteral(pattern, t.text[i]);
			else pattern.push_back(t.text[i]);
		}

		return pattern;
	}

	std::string Expander::expandPattern(const Word& w) {
		const Word tilded = applyTildeExpansion(w);
		return taggedAsPattern(renderWord(tilded));
	}

	// bash's ere_char(): what a quoted character needs a backslash for.
	static bool isEreSpecial(char c) {
		return c != '\0' && std::strchr("\\.[()*+?{|^$", c) != nullptr;
	}

	struct RegexBracket {
		const Expander::Tagged& t;
		std::size_t pos;
		std::string text;
		char open_class;
	};

	static bool bracketCharIs(const RegexBracket& b, std::size_t at, char c) {
		return at < b.t.size() && !quotedAt(b.t, at) && b.t.text[at] == c;
	}

	static void takeBracketChar(RegexBracket& b) {
		b.text.push_back(b.t.text[b.pos++]);
	}

	static bool opensBracketClass(const RegexBracket& b) {
		return bracketCharIs(b, b.pos, '[') && (bracketCharIs(b, b.pos + 1, ':')
			|| bracketCharIs(b, b.pos + 1, '=') || bracketCharIs(b, b.pos + 1, '.'));
	}

	// `[:`, `[=` and `[.` open a class that only its own `:]`, `=]` or
	// `.]` closes; a `]` right after `[=` or `[.` belongs to it.
	static void takeBracketUnit(RegexBracket& b) {
		if (isJoinedWithNothing(b.t, b.pos)) {
			++b.pos;
			return;
		}

		if (opensBracketClass(b)) {
			b.open_class = b.t.text[b.pos + 1];
			takeBracketChar(b);
			takeBracketChar(b);
			if (b.open_class != ':' && bracketCharIs(b, b.pos, ']')) takeBracketChar(b);
			return;
		}

		const bool closes_class = b.open_class != 0 && bracketCharIs(b, b.pos, b.open_class)
			&& bracketCharIs(b, b.pos + 1, ']');
		if (closes_class) {
			b.open_class = 0;
			takeBracketChar(b);
		}

		takeBracketChar(b);
	}

	// Inside a bracket expression bash gives a quoted character no
	// backslash, which would be literal there. False when no unquoted `]`
	// closes it: the `[` is then ordinary text.
	static bool copyRegexBracket(const Expander::Tagged& t, std::size_t& i, std::string& regex) {
		RegexBracket b{ t, i + 1, "[", 0 };
		if (bracketCharIs(b, b.pos, '^')) takeBracketChar(b);
		if (bracketCharIs(b, b.pos, ']')) takeBracketChar(b);

		do {
			if (b.pos >= t.size()) return false;
			takeBracketUnit(b);
		} while (b.pos < t.size() && !bracketCharIs(b, b.pos, ']'));

		if (b.pos >= t.size()) return false;

		regex += b.text + "]";
		i = b.pos;
		return true;
	}

	static std::string taggedAsRegex(const Expander::Tagged& t) {
		std::string regex;
		regex.reserve(t.size());
		for (std::size_t i = 0; i < t.size(); ++i) {
			if (isJoinedWithNothing(t, i)) continue;
			const bool opens_bracket = t.text[i] == '[' && !quotedAt(t, i);
			if (opens_bracket && copyRegexBracket(t, i, regex)) continue;

			if (quotedAt(t, i) && isEreSpecial(t.text[i])) regex.push_back('\\');
			regex.push_back(t.text[i]);
		}

		return regex;
	}

	std::string Expander::expandRegex(const Word& w) {
		const Word tilded = applyTildeExpansion(w);
		return taggedAsRegex(renderWord(tilded));
	}

}  // namespace wbsh

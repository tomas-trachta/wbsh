/**
 * @file coreutils_expr.cpp
 * @brief expr: GNU expr's grammar, from `|` down to `:` and the keyword
 *        functions, with its exit statuses and messages.
 */

#include "coreutils_internal.h"

#include <cstdio>
#include <string>
#include <vector>

#include "executor.h"
#include "numparse.h"
#include "posixregex.h"

namespace wbsh {

	namespace expr_detail {

		static const int kStatusTrue = 0;
		static const int kStatusNull = 1;
		static const int kStatusInvalid = 2;

		// A value is an integer or a string; GNU keeps the integers of the
		// arguments as strings until arithmetic needs them.
		struct Value {
			std::string text;
		};

		struct Parse {
			const std::vector<std::string>& args;
			std::size_t pos = 0;
			std::string error;
		};

		static bool failed(const Parse& parse) {
			return !parse.error.empty();
		}

		static void fail(Parse& parse, const std::string& message) {
			if (!failed(parse)) parse.error = message;
		}

		static bool atEnd(const Parse& parse) {
			return parse.pos >= parse.args.size();
		}

		static bool nextIs(const Parse& parse, const char* word) {
			return !atEnd(parse) && parse.args[parse.pos] == word;
		}

		static const std::string& lastArgument(const Parse& parse) {
			return parse.args[parse.pos == 0 ? 0 : parse.pos - 1];
		}

		// An optional `-` and decimal digits, nothing else.
		static bool toInteger(const std::string& text, long long& out) {
			if (text.empty()) return false;

			const std::size_t digits = text[0] == '-' ? 1 : 0;
			if (digits == text.size()) return false;

			for (std::size_t k = digits; k < text.size(); ++k) {
				if (text[k] < '0' || text[k] > '9') return false;
			}

			return parseLL(text, out);
		}

		// GNU's null(): the empty string, or zeros with an optional `-`.
		static bool isNull(const Value& value) {
			const std::string& text = value.text;
			if (text.empty()) return true;

			std::size_t k = text[0] == '-' ? 1 : 0;
			if (k == text.size()) return false;

			for (; k < text.size(); ++k) {
				if (text[k] != '0') return false;
			}

			return true;
		}

		static Value number(long long value) {
			return { std::to_string(value) };
		}

		static Value truth(bool value) {
			return number(value ? 1 : 0);
		}

		static Value parseOr(Parse& parse);

		static Value takeOperand(Parse& parse) {
			if (atEnd(parse)) {
				fail(parse, "syntax error: missing argument after '" + lastArgument(parse) + "'");
				return {};
			}

			return { parse.args[parse.pos++] };
		}

		static Value parseGroup(Parse& parse) {
			Value inner = parseOr(parse);
			if (failed(parse)) return inner;

			if (atEnd(parse)) {
				fail(parse, "syntax error: expecting ')' after '" + lastArgument(parse) + "'");
			} else if (!nextIs(parse, ")")) {
				fail(parse, "syntax error: expecting ')' instead of '" + parse.args[parse.pos] + "'");
			} else {
				++parse.pos;
			}

			return inner;
		}

		// `STRING : REGEXP` is anchored at the start of STRING; with a group
		// it yields what the group matched, else the length of the match.
		static Value matchRegex(Parse& parse, const Value& subject, const Value& pattern) {
			PosixRegex regex;
			RegexOptions options;
			if (!regex.compile(pattern.text, options)) {
				fail(parse, regex.error());
				return {};
			}

			RegexMatch match;
			const bool found = regex.search(subject.text, 0, match) && match.start(0) == 0;
			if (regex.groups() > 1) return { found ? match.str(subject.text, 1) : std::string() };
			return number(found ? static_cast<long long>(match.length(0)) : 0);
		}

		static Value parsePrimary(Parse& parse);

		static Value substrOf(Parse& parse) {
			const Value text = parsePrimary(parse);
			const Value position = parsePrimary(parse);
			const Value length = parsePrimary(parse);
			if (failed(parse)) return {};

			long long from = 0;
			long long count = 0;
			if (!toInteger(position.text, from) || !toInteger(length.text, count)) return {};
			if (from < 1 || count < 1 || static_cast<std::size_t>(from) > text.text.size()) return {};

			return { text.text.substr(static_cast<std::size_t>(from - 1),
				static_cast<std::size_t>(count)) };
		}

		static Value indexOf(Parse& parse) {
			const Value text = parsePrimary(parse);
			const Value chars = parsePrimary(parse);
			if (failed(parse)) return {};

			const std::size_t at = text.text.find_first_of(chars.text);
			return number(at == std::string::npos ? 0 : static_cast<long long>(at + 1));
		}

		static Value keywordCall(Parse& parse, const std::string& word) {
			if (word == "length") return number(static_cast<long long>(parsePrimary(parse).text.size()));
			if (word == "substr") return substrOf(parse);
			if (word == "index") return indexOf(parse);

			const Value subject = parsePrimary(parse);
			const Value pattern = parsePrimary(parse);
			if (failed(parse)) return {};
			return matchRegex(parse, subject, pattern);
		}

		static bool isKeyword(const std::string& word) {
			return word == "match" || word == "substr" || word == "index" || word == "length";
		}

		static Value parsePrimary(Parse& parse) {
			if (failed(parse)) return {};
			if (atEnd(parse)) {
				fail(parse, parse.pos == 0 ? "missing operand"
					: "syntax error: missing argument after '" + lastArgument(parse) + "'");
				return {};
			}

			const std::string& word = parse.args[parse.pos++];
			if (word == "+") return takeOperand(parse);
			if (word == "(") return parseGroup(parse);
			if (isKeyword(word)) return keywordCall(parse, word);
			return { word };
		}

		static Value parseMatch(Parse& parse) {
			Value left = parsePrimary(parse);
			while (!failed(parse) && nextIs(parse, ":")) {
				++parse.pos;
				const Value right = parsePrimary(parse);
				if (failed(parse)) break;
				left = matchRegex(parse, left, right);
			}

			return left;
		}

		static Value arithmetic(Parse& parse, const std::string& op, const Value& left,
				const Value& right) {
			long long a = 0;
			long long b = 0;
			if (!toInteger(left.text, a) || !toInteger(right.text, b)) {
				fail(parse, "non-integer argument");
				return {};
			}

			if ((op == "/" || op == "%") && b == 0) {
				fail(parse, "division by zero");
				return {};
			}

			if (op == "+") return number(a + b);
			if (op == "-") return number(a - b);
			if (op == "*") return number(a * b);
			if (op == "/") return number(a / b);
			return number(a % b);
		}

		static const char* const kMultiplicative[] = { "*", "/", "%", nullptr };
		static const char* const kAdditive[] = { "+", "-", nullptr };
		static const char* const kComparisons[] = { "<", "<=", "=", "==", "!=", ">=", ">", nullptr };

		static const char* matchOperator(const Parse& parse, const char* const* ops) {
			for (int k = 0; ops[k] != nullptr; ++k) {
				if (nextIs(parse, ops[k])) return ops[k];
			}

			return nullptr;
		}

		static Value parseMultiplicative(Parse& parse) {
			Value left = parseMatch(parse);
			while (!failed(parse)) {
				const char* op = matchOperator(parse, kMultiplicative);
				if (op == nullptr) break;

				++parse.pos;
				const Value right = parseMatch(parse);
				if (failed(parse)) break;
				left = arithmetic(parse, op, left, right);
			}

			return left;
		}

		static Value parseAdditive(Parse& parse) {
			Value left = parseMultiplicative(parse);
			while (!failed(parse)) {
				const char* op = matchOperator(parse, kAdditive);
				if (op == nullptr) break;

				++parse.pos;
				const Value right = parseMultiplicative(parse);
				if (failed(parse)) break;
				left = arithmetic(parse, op, left, right);
			}

			return left;
		}

		// Integers compare as numbers, anything else as strings.
		static int compareValues(const Value& left, const Value& right) {
			long long a = 0;
			long long b = 0;
			if (toInteger(left.text, a) && toInteger(right.text, b)) return a < b ? -1 : (a > b ? 1 : 0);

			const int order = left.text.compare(right.text);
			return order < 0 ? -1 : (order > 0 ? 1 : 0);
		}

		static bool comparisonHolds(const std::string& op, int order) {
			if (op == "<") return order < 0;
			if (op == "<=") return order <= 0;
			if (op == "=" || op == "==") return order == 0;
			if (op == "!=") return order != 0;
			if (op == ">=") return order >= 0;
			return order > 0;
		}

		static Value parseComparison(Parse& parse) {
			Value left = parseAdditive(parse);
			while (!failed(parse)) {
				const char* op = matchOperator(parse, kComparisons);
				if (op == nullptr) break;

				++parse.pos;
				const Value right = parseAdditive(parse);
				if (failed(parse)) break;
				left = truth(comparisonHolds(op, compareValues(left, right)));
			}

			return left;
		}

		static Value parseAnd(Parse& parse) {
			Value left = parseComparison(parse);
			while (!failed(parse) && nextIs(parse, "&")) {
				++parse.pos;
				const Value right = parseComparison(parse);
				if (failed(parse)) break;
				if (isNull(left) || isNull(right)) left = number(0);
			}

			return left;
		}

		static Value parseOr(Parse& parse) {
			Value left = parseAnd(parse);
			while (!failed(parse) && nextIs(parse, "|")) {
				++parse.pos;
				const Value right = parseAnd(parse);
				if (failed(parse)) break;
				if (isNull(left)) left = isNull(right) ? number(0) : right;
			}

			return left;
		}

		static std::vector<std::string> withoutEndOfOptions(const std::vector<std::string>& args) {
			if (!args.empty() && args[0] == "--") return { args.begin() + 1, args.end() };
			return args;
		}

		static int reportError(const Parse& parse) {
			perr("expr", parse.error);
			if (parse.error == "missing operand") {
				std::fputs("Try 'expr --help' for more information.\n", stderr);
			}

			return kStatusInvalid;
		}

		static int builtin_expr(Executor&, const std::vector<std::string>& raw_args) {
			const std::vector<std::string> args = withoutEndOfOptions(raw_args);
			Parse parse{ args };
			const Value result = parseOr(parse);
			if (!failed(parse) && !atEnd(parse)) {
				fail(parse, "syntax error: unexpected argument '" + args[parse.pos] + "'");
			}

			if (failed(parse)) return reportError(parse);

			std::printf("%s\n", result.text.c_str());
			return isNull(result) ? kStatusNull : kStatusTrue;
		}

	}  // namespace expr_detail

	void registerExprBuiltin(Executor& exec) {
		exec.registerBuiltin("expr", expr_detail::builtin_expr);
	}

}  // namespace wbsh

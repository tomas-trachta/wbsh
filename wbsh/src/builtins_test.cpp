/**
 * @file builtins_test.cpp
 * @brief `test` and `[`: bash's test.c parser, the POSIX argument-count
 *        rules first, then a recursive descent for `!`, `-a`, `-o` and
 *        `( )`, with its messages and its quirks.
 */

#include <cstdio>
#include <string>
#include <vector>

#include "diagnostics.h"
#include "executor.h"
#include "numparse.h"
#include "testops.h"

namespace wbsh::test_detail {

	struct TestRun {
		Executor& exec;
		const std::vector<std::string>& args;
		std::size_t pos;
		std::size_t end;
		bool failed;
		std::string error;
	};

	static bool fail(TestRun& run, const std::string& message) {
		if (!run.failed) {
			run.failed = true;
			run.error = message;
		}

		return false;
	}

	static bool beyond(TestRun& run) {
		return fail(run, "argument expected");
	}

	static bool isToken(const std::string& word, const char* token) {
		return word == token;
	}

	static bool isAndOr(const std::string& word) {
		return word == "-a" || word == "-o";
	}

	static bool isIntegerOperator(const std::string& op) {
		return op == "-eq" || op == "-ne" || op == "-lt" || op == "-le" || op == "-gt" || op == "-ge";
	}

	static bool isStringOperator(const std::string& op) {
		return op == "=" || op == "==" || op == "!=" || op == "<" || op == ">";
	}

	static bool isBinaryOperator(const std::string& op) {
		return isStringOperator(op) || isIntegerOperator(op) || isFileCompareOperator(op);
	}

	static std::string trimmedBlanks(const std::string& text) {
		const std::size_t first = text.find_first_not_of(" \t\n");
		if (first == std::string::npos) return {};

		const std::size_t last = text.find_last_not_of(" \t\n");
		return text.substr(first, last - first + 1);
	}

	static bool validNumber(const std::string& text, long long& out) {
		const std::string trimmed = trimmedBlanks(text);
		std::size_t used = 0;
		return !trimmed.empty() && parseLL(trimmed, out, 10, &used) && used == trimmed.size();
	}

	static bool integerOperand(TestRun& run, const std::string& text, long long& out) {
		if (validNumber(text, out)) return true;

		return fail(run, text + ": integer expected");
	}

	static bool compareIntegers(const std::string& op, long long left, long long right) {
		if (op == "-eq") return left == right;
		if (op == "-ne") return left != right;
		if (op == "-lt") return left <  right;
		if (op == "-le") return left <= right;
		if (op == "-gt") return left >  right;
		return left >= right;
	}

	static bool compareStrings(const std::string& op, const std::string& left,
			const std::string& right) {
		if (op == "=" || op == "==") return left == right;
		if (op == "!=")              return left != right;
		if (op == "<")               return left <  right;
		return left > right;
	}

	static bool evalBinary(TestRun& run, const std::string& left, const std::string& op,
			const std::string& right) {
		if (isStringOperator(op)) return compareStrings(op, left, right);
		if (isFileCompareOperator(op)) return evalFileCompare(run.exec, op, left, right);

		long long left_value = 0;
		long long right_value = 0;
		if (!integerOperand(run, left, left_value)) return false;
		if (!integerOperand(run, right, right_value)) return false;
		return compareIntegers(op, left_value, right_value);
	}

	static bool unaryTest(TestRun& run, const std::string& op, const std::string& operand) {
		long long fd = 0;
		if (op[1] == 't' && !integerOperand(run, operand, fd)) return false;

		return evalUnaryTestOperator(run.exec, op[1], operand, true);
	}

	static const std::string& current(const TestRun& run) {
		return run.args[run.pos];
	}

	static bool atEnd(const TestRun& run) {
		return run.pos >= run.end;
	}

	static bool advanceRequired(TestRun& run) {
		++run.pos;
		return atEnd(run) ? beyond(run) : true;
	}

	static bool binaryOperator(TestRun& run) {
		const bool value = evalBinary(run, current(run), run.args[run.pos + 1],
			run.args[run.pos + 2]);
		run.pos += 3;
		return value;
	}

	// Outside POSIX mode `-t` alone means `-t 1`, also right before an -a
	// or -o of a longer expression.
	static bool terminalOperator(TestRun& run) {
		++run.pos;
		if (atEnd(run)) return unaryTest(run, "-t", "1");

		long long fd = 0;
		if (validNumber(current(run), fd)) {
			++run.pos;
			return unaryTest(run, "-t", run.args[run.pos - 1]);
		}

		if (run.end >= 4 && isAndOr(current(run))) return unaryTest(run, "-t", "1");
		return fail(run, current(run) + ": integer expected");
	}

	static bool unaryOperator(TestRun& run) {
		const std::string& op = current(run);
		if (op == "-t" && !run.exec.env().options().posix) return terminalOperator(run);
		if (!advanceRequired(run)) return false;

		++run.pos;
		return unaryTest(run, op, run.args[run.pos - 1]);
	}

	static bool parseOr(TestRun& run);
	static bool posixTest(TestRun& run, std::size_t count);

	static bool expression(TestRun& run) {
		if (atEnd(run)) return beyond(run);
		return parseOr(run);
	}

	// The arguments up to the matching `)`; a short group gets the
	// argument-count rules of its own. Like bash, the check for the `)`
	// sees the `]` that closes `[`.
	static std::size_t groupLength(const TestRun& run) {
		std::size_t length = 1;
		int depth = 1;
		for (; run.pos + length < run.end; ++length) {
			const std::string& word = run.args[run.pos + length];
			if (isToken(word, ")")) --depth;
			else if (isToken(word, "(")) ++depth;
			if (depth == 0) break;
		}

		return length;
	}

	static bool parseGroup(TestRun& run) {
		if (!advanceRequired(run)) return false;

		const std::size_t length = groupLength(run);
		const bool short_group = run.pos + length < run.end && length <= 4;
		const bool value = short_group ? posixTest(run, length) : expression(run);
		if (run.failed) return false;
		if (run.pos >= run.args.size()) return fail(run, "`)' expected");
		if (!isToken(current(run), ")")) return fail(run, "`)' expected, found " + current(run));

		++run.pos;
		return value;
	}

	static bool parseNegations(TestRun& run);

	static bool parseTerm(TestRun& run) {
		if (run.failed) return false;
		if (atEnd(run)) return beyond(run);
		if (isToken(current(run), "!")) return parseNegations(run);
		if (isToken(current(run), "(")) return parseGroup(run);

		if (run.pos + 3 <= run.end && isBinaryOperator(run.args[run.pos + 1])) {
			return binaryOperator(run);
		}

		if (run.pos + 2 <= run.end && isUnaryTestOperator(current(run))) return unaryOperator(run);

		return !run.args[run.pos++].empty();
	}

	static bool parseNegations(TestRun& run) {
		bool negate = false;
		while (!atEnd(run) && isToken(current(run), "!")) {
			if (!advanceRequired(run)) return false;
			negate = !negate;
		}

		const bool value = parseTerm(run);
		return negate ? !value : value;
	}

	static bool parseAnd(TestRun& run) {
		const bool value = parseTerm(run);
		if (atEnd(run) || !isToken(current(run), "-a")) return value;

		++run.pos;
		const bool right = parseAnd(run);
		return value && right;
	}

	static bool parseOr(TestRun& run) {
		const bool value = parseAnd(run);
		if (atEnd(run) || !isToken(current(run), "-o")) return value;

		++run.pos;
		const bool right = parseOr(run);
		return value || right;
	}

	static bool twoArguments(TestRun& run) {
		const std::string& first = current(run);
		if (isToken(first, "!")) {
			++run.pos;
			return run.args[run.pos++].empty();
		}

		if (isUnaryTestOperator(first)) return unaryOperator(run);
		return fail(run, first + ": unary operator expected");
	}

	static bool oneArgumentTest(const std::string& word) {
		return !word.empty();
	}

	static bool threeArguments(TestRun& run) {
		const std::string& left = current(run);
		const std::string& op = run.args[run.pos + 1];
		const std::string& right = run.args[run.pos + 2];
		if (isBinaryOperator(op)) return binaryOperator(run);

		if (isAndOr(op)) {
			run.pos += 3;
			if (op == "-a") return oneArgumentTest(left) && oneArgumentTest(right);
			return oneArgumentTest(left) || oneArgumentTest(right);
		}

		if (isToken(left, "!")) {
			if (!advanceRequired(run)) return false;
			return !twoArguments(run);
		}

		if (isToken(left, "(") && isToken(right, ")")) {
			run.pos += 3;
			return oneArgumentTest(op);
		}

		return fail(run, op + ": binary operator expected");
	}

	static bool fourArguments(TestRun& run) {
		if (isToken(current(run), "!")) {
			if (!advanceRequired(run)) return false;
			return !threeArguments(run);
		}

		if (isToken(current(run), "(") && isToken(run.args[run.pos + 3], ")")) {
			if (!advanceRequired(run)) return false;
			const bool value = twoArguments(run);
			++run.pos;
			return value;
		}

		return expression(run);
	}

	// One argument tests the first one of the whole command, also inside
	// a group, as bash does.
	static bool posixTest(TestRun& run, std::size_t count) {
		switch (count) {
			case 0:
				return false;
			case 1:
				++run.pos;
				return oneArgumentTest(run.args[0]);
			case 2:  return twoArguments(run);
			case 3:  return threeArguments(run);
			case 4:  return fourArguments(run);
			default: return expression(run);
		}
	}

	static void checkAllUsed(TestRun& run) {
		if (run.failed || atEnd(run)) return;

		const std::string& word = current(run);
		if (!word.empty() && word[0] == '-') fail(run, "syntax error: `" + word + "' unexpected");
		else                                 fail(run, "too many arguments");
	}

	static int evaluate(Executor& exec, const char* name, const std::vector<std::string>& args,
			std::size_t count) {
		TestRun run{ exec, args, 0, count, false, {} };
		const bool value = posixTest(run, count);
		checkAllUsed(run);
		if (!run.failed) return value ? 0 : 1;

		shellErrorf("%s: %s\n", name, run.error.c_str());
		return 2;
	}

	static int builtin_test(Executor& exec, const std::vector<std::string>& args) {
		return evaluate(exec, "test", args, args.size());
	}

	static int builtin_lbracket(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty() || args.back() != "]") {
			shellErrorf("[: missing `]'\n");
			return 2;
		}

		return evaluate(exec, "[", args, args.size() - 1);
	}

}  // namespace wbsh::test_detail

namespace wbsh {

	void registerTestBuiltins(Executor& exec) {
		exec.registerBuiltin("test", test_detail::builtin_test);
		exec.registerBuiltin("[",    test_detail::builtin_lbracket);
	}

}  // namespace wbsh

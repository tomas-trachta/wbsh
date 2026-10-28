/**
 * @file builtins_test.cpp
 * @brief `test` and `[`: the POSIX argument-count rules, then a
 *        recursive-descent parser for `!`, `-a`, `-o` and `( )`.
 */

#include <cstdio>
#include <string>
#include <vector>

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

	static bool integerOperand(TestRun& run, const std::string& text, long long& out) {
		const std::string trimmed = trimmedBlanks(text);
		std::size_t used = 0;
		if (!trimmed.empty() && parseLL(trimmed, out, 10, &used) && used == trimmed.size()) return true;

		return fail(run, text + ": integer expression expected");
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

	static bool evalUnary(TestRun& run, const std::string& op, const std::string& operand) {
		return evalUnaryTestOperator(run.exec, op[1], operand);
	}

	static const std::string& at(const TestRun& run, std::size_t offset) {
		return run.args[run.pos + offset];
	}

	static bool remaining(const TestRun& run, std::size_t count) {
		return run.pos + count <= run.end;
	}

	static bool parseOr(TestRun& run);

	static bool parseGroup(TestRun& run) {
		++run.pos;
		const bool value = parseOr(run);
		if (!remaining(run, 1) || at(run, 0) != ")") return fail(run, "`)' expected");

		++run.pos;
		return value;
	}

	static bool parsePrimary(TestRun& run) {
		if (!remaining(run, 1)) return fail(run, "argument expected");
		if (at(run, 0) == "(") return parseGroup(run);

		if (remaining(run, 3) && isBinaryOperator(at(run, 1))) {
			const bool value = evalBinary(run, at(run, 0), at(run, 1), at(run, 2));
			run.pos += 3;
			return value;
		}

		if (remaining(run, 2) && isUnaryTestOperator(at(run, 0))) {
			const bool value = evalUnary(run, at(run, 0), at(run, 1));
			run.pos += 2;
			return value;
		}

		return !run.args[run.pos++].empty();
	}

	static bool parseNot(TestRun& run) {
		if (remaining(run, 2) && at(run, 0) == "!") {
			++run.pos;
			return !parseNot(run);
		}

		return parsePrimary(run);
	}

	static bool parseAnd(TestRun& run) {
		bool value = parseNot(run);
		while (!run.failed && remaining(run, 1) && at(run, 0) == "-a") {
			++run.pos;
			const bool right = parseNot(run);
			value = value && right;
		}

		return value;
	}

	static bool parseOr(TestRun& run) {
		bool value = parseAnd(run);
		while (!run.failed && remaining(run, 1) && at(run, 0) == "-o") {
			++run.pos;
			const bool right = parseAnd(run);
			value = value || right;
		}

		return value;
	}

	static bool parseExpression(TestRun& run, std::size_t first, std::size_t count) {
		run.pos = first;
		run.end = first + count;
		const bool value = parseOr(run);
		if (!run.failed && run.pos != run.end) return fail(run, "too many arguments");
		return value;
	}

	static bool evalByCount(TestRun& run, std::size_t first, std::size_t count);

	static bool evalTwo(TestRun& run, std::size_t first) {
		const std::string& op = run.args[first];
		if (op == "!") return run.args[first + 1].empty();
		if (isUnaryTestOperator(op)) return evalUnary(run, op, run.args[first + 1]);
		return fail(run, op + ": unary operator expected");
	}

	static bool evalLogical(const std::string& op, const std::string& left, const std::string& right) {
		if (op == "-a") return !left.empty() && !right.empty();
		return !left.empty() || !right.empty();
	}

	static bool evalThree(TestRun& run, std::size_t first) {
		const std::string& left = run.args[first];
		const std::string& op = run.args[first + 1];
		const std::string& right = run.args[first + 2];
		if (isBinaryOperator(op)) return evalBinary(run, left, op, right);
		if (op == "-a" || op == "-o") return evalLogical(op, left, right);
		if (left == "!") return !evalByCount(run, first + 1, 2);
		if (left == "(" && right == ")") return !op.empty();
		return fail(run, op + ": binary operator expected");
	}

	static bool evalFour(TestRun& run, std::size_t first) {
		if (run.args[first] == "!") return !evalByCount(run, first + 1, 3);
		if (run.args[first] == "(" && run.args[first + 3] == ")") return evalByCount(run, first + 1, 2);
		return parseExpression(run, first, 4);
	}

	static bool evalByCount(TestRun& run, std::size_t first, std::size_t count) {
		switch (count) {
			case 0:  return false;
			case 1:  return !run.args[first].empty();
			case 2:  return evalTwo(run, first);
			case 3:  return evalThree(run, first);
			case 4:  return evalFour(run, first);
			default: return parseExpression(run, first, count);
		}
	}

	static int evaluate(Executor& exec, const char* name, const std::vector<std::string>& args,
			std::size_t count) {
		TestRun run{ exec, args, 0, count, false, {} };
		const bool value = evalByCount(run, 0, count);
		if (!run.failed) return value ? 0 : 1;

		std::fprintf(stderr, "wbsh: %s: %s\n", name, run.error.c_str());
		return 2;
	}

	static int builtin_test(Executor& exec, const std::vector<std::string>& args) {
		return evaluate(exec, "test", args, args.size());
	}

	static int builtin_lbracket(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty() || args.back() != "]") {
			std::fprintf(stderr, "wbsh: [: missing `]'\n");
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

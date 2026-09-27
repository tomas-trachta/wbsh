/**
 * @file coreutils_bc.cpp
 * @brief Pragmatic bc(1) calculator.
 *
 * Parses arithmetic expressions, assignments, and simple control flow.
 * Uses double precision; `scale` controls fraction display. Math
 * library (`-l`) provides sqrt, s, c, e, l, a.
 */

#include "coreutils_internal.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "executor.h"
#include "numparse.h"
#include "pathconv.h"

namespace wbsh {

	namespace bc_detail {

		static const int kMathLibScale = 20;

		struct BcState {
			std::map<std::string, double> vars;
			int scale = 0;
			bool with_lib = false;
			bool quiet = false;
			bool exit_now = false;
		};

		static bool isNameStart(char c) {
			return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
		}

		static bool isNameChar(char c) {
			return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
		}

		static bool isNumberChar(char c) {
			return std::isdigit(static_cast<unsigned char>(c)) || c == '.';
		}

		static bool isOperatorChar(char c) {
			return std::strchr("<>=!+-*/%^", c) != nullptr && c != '\0';
		}

		static bool isDoublingChar(char c) {
			return c == '&' || c == '|' || c == '+' || c == '-';
		}

		struct BcLex {
			const std::string& source;
			std::size_t pos = 0;
			std::vector<std::string> pushback;

			explicit BcLex(const std::string& text) : source(text) {}

			char current() const {
				return pos < source.size() ? source[pos] : '\0';
			}

			char lookahead() const {
				return pos + 1 < source.size() ? source[pos + 1] : '\0';
			}

			void skipBlanks() {
				while (pos < source.size() && (source[pos] == ' ' || source[pos] == '\t')) ++pos;
				while (current() == '\\' && lookahead() == '\n') pos += 2;
				if (current() != '#') return;

				while (pos < source.size() && source[pos] != '\n') ++pos;
			}

			std::string readWhile(bool (*accept)(char)) {
				const std::size_t start = pos;
				while (pos < source.size() && accept(source[pos])) ++pos;
				return source.substr(start, pos - start);
			}

			std::string readTwoChars() {
				std::string text = source.substr(pos, 2);
				pos += 2;
				return text;
			}

			std::string readOperator() {
				const char c = current();
				if (isOperatorChar(c) && lookahead() == '=') return readTwoChars();
				if (isDoublingChar(c) && lookahead() == c) return readTwoChars();
				if (c == '\n') {
					++pos;
					return ";";
				}

				++pos;
				return std::string(1, c);
			}

			std::string next() {
				if (!pushback.empty()) {
					std::string token = pushback.back();
					pushback.pop_back();
					return token;
				}

				skipBlanks();
				if (pos >= source.size()) return "";

				const char c = current();
				if (isNumberChar(c)) return readWhile(isNumberChar);
				if (isNameStart(c)) return readWhile(isNameChar);
				return readOperator();
			}

			std::string peek() {
				if (pushback.empty()) {
					std::string token = next();
					if (token.empty()) return token;
					pushback.push_back(token);
				}

				return pushback.back();
			}

			void unread(std::string token) {
				pushback.push_back(std::move(token));
			}
		};

		static bool isName(const std::string& token) {
			if (token.empty()) return false;
			if (!isNameStart(token[0])) return false;

			for (char c : token) {
				if (!isNameChar(c)) return false;
			}

			return true;
		}

		static bool isNumber(const std::string& token) {
			if (token.empty()) return false;

			for (char c : token) {
				if (!isNumberChar(c)) return false;
			}

			return true;
		}

		static bool isAssignOp(const std::string& op) {
			return op == "=" || op == "+=" || op == "-=" || op == "*="
				|| op == "/=" || op == "%=" || op == "^=";
		}

		static bool isComparisonOp(const std::string& op) {
			return op == "==" || op == "!=" || op == "<" || op == "<="
				|| op == ">" || op == ">=";
		}

		static double applyCompoundOp(const std::string& op, double current, double rhs) {
			if (op == "+=") return current + rhs;
			if (op == "-=") return current - rhs;
			if (op == "*=") return current * rhs;
			if (op == "/=") return rhs == 0 ? 0 : current / rhs;
			if (op == "%=") return rhs == 0 ? 0 : std::fmod(current, rhs);
			if (op == "^=") return std::pow(current, rhs);
			return rhs;
		}

		static bool compareDoubles(const std::string& op, double left, double right) {
			if (op == "==") return left == right;
			if (op == "!=") return left != right;
			if (op == "<")  return left <  right;
			if (op == "<=") return left <= right;
			if (op == ">")  return left >  right;
			return left >= right;
		}

		static double digitCount(double value) {
			char buffer[64];
			std::snprintf(buffer, sizeof(buffer), "%.20g", value);

			int count = 0;
			for (const char* c = buffer; *c != '\0'; ++c) {
				if (std::isdigit(static_cast<unsigned char>(*c))) ++count;
			}

			return static_cast<double>(count);
		}

		struct BcEval {
			BcLex& lex;
			BcState& state;

			BcEval(BcLex& lexer, BcState& bc_state) : lex(lexer), state(bc_state) {}

			double varOrZero(const std::string& name) const {
				const auto it = state.vars.find(name);
				return it == state.vars.end() ? 0.0 : it->second;
			}

			void storeVar(const std::string& name, double value) {
				if (name == "scale") state.scale = static_cast<int>(value);
				else                 state.vars[name] = value;
			}

			double parseExpr() {
				return parseAssign();
			}

			double parseAssignmentTail(const std::string& name, const std::string& op) {
				lex.next();
				const double rhs = parseAssign();
				const double value = applyCompoundOp(op, varOrZero(name), rhs);
				storeVar(name, value);
				return value;
			}

			double parseAssign() {
				if (!isName(lex.peek())) return parseOr();

				std::string name = lex.next();
				const std::string op = lex.peek();
				if (isAssignOp(op)) return parseAssignmentTail(name, op);

				lex.unread(std::move(name));
				return parseOr();
			}

			double parseOr() {
				double left = parseAnd();
				while (lex.peek() == "||") {
					lex.next();
					const double right = parseAnd();
					left = (left != 0 || right != 0) ? 1 : 0;
				}

				return left;
			}

			double parseAnd() {
				double left = parseCmp();
				while (lex.peek() == "&&") {
					lex.next();
					const double right = parseCmp();
					left = (left != 0 && right != 0) ? 1 : 0;
				}

				return left;
			}

			double parseCmp() {
				double left = parseAddSub();
				for (;;) {
					const std::string op = lex.peek();
					if (!isComparisonOp(op)) return left;

					lex.next();
					const double right = parseAddSub();
					left = compareDoubles(op, left, right) ? 1.0 : 0.0;
				}
			}

			double parseAddSub() {
				double left = parseMul();
				for (;;) {
					const std::string op = lex.peek();
					if (op != "+" && op != "-") return left;

					lex.next();
					const double right = parseMul();
					if (op == "+") left += right;
					else           left -= right;
				}
			}

			double parseMul() {
				double left = parseExp();
				for (;;) {
					const std::string op = lex.peek();
					if (op != "*" && op != "/" && op != "%") return left;

					lex.next();
					const double right = parseExp();
					if (op == "*") {
						left *= right;
					} else if (op == "/") {
						left = right == 0 ? 0 : left / right;
					} else {
						left = right == 0 ? 0 : std::fmod(left, right);
					}
				}
			}

			double parseExp() {
				const double base = parseUnary();
				if (lex.peek() != "^") return base;

				lex.next();
				return std::pow(base, parseExp());
			}

			double preIncrement(double delta) {
				const std::string name = lex.next();
				const double value = varOrZero(name) + delta;
				state.vars[name] = value;
				return value;
			}

			double parseUnary() {
				const std::string op = lex.peek();
				if (op == "-")  { lex.next(); return -parseUnary(); }
				if (op == "+")  { lex.next(); return  parseUnary(); }
				if (op == "!")  { lex.next(); return parseUnary() == 0 ? 1 : 0; }
				if (op == "++") { lex.next(); return preIncrement(1); }
				if (op == "--") { lex.next(); return preIncrement(-1); }
				return parsePrimary();
			}

			std::vector<double> parseCallArgs() {
				std::vector<double> args;
				if (lex.peek() != ")") {
					args.push_back(parseExpr());
					while (lex.peek() == ",") {
						lex.next();
						args.push_back(parseExpr());
					}
				}

				if (lex.peek() == ")") lex.next();
				return args;
			}

			double postIncrement(const std::string& name) {
				const double current = varOrZero(name);
				const double updated = (lex.next() == "++") ? current + 1 : current - 1;
				state.vars[name] = updated;
				return current;
			}

			double parseNameRef(const std::string& name) {
				if (lex.peek() == "(") {
					lex.next();
					const std::vector<double> args = parseCallArgs();
					return callFunc(name, args);
				}

				if (lex.peek() == "++" || lex.peek() == "--") return postIncrement(name);
				if (name == "scale") return state.scale;
				return varOrZero(name);
			}

			double parseGroup() {
				const double value = parseExpr();
				if (lex.peek() == ")") lex.next();
				return value;
			}

			double parsePrimary() {
				const std::string token = lex.next();
				if (token.empty()) return 0;
				if (token == "(") return parseGroup();
				if (isNumber(token)) {
					double value = 0;
					parseDouble(token, value);
					return value;
				}

				if (isName(token)) return parseNameRef(token);
				return 0;
			}

			double callFunc(const std::string& name, const std::vector<double>& args) {
				const double arg = args.empty() ? 0 : args[0];
				if (name == "sqrt") return std::sqrt(arg);
				if (state.with_lib) {
					if (name == "s") return std::sin(arg);
					if (name == "c") return std::cos(arg);
					if (name == "e") return std::exp(arg);
					if (name == "l") return std::log(arg);
					if (name == "a") return std::atan(arg);
				}

				if (name == "length") return digitCount(arg);
				return 0;
			}
		};

		// Peeks two tokens ahead: an assignment or pre-increment statement
		// prints nothing, everything else prints its value.
		static bool startsAssign(BcLex& lex) {
			std::string first = lex.next();
			if (first.empty()) return false;

			std::string second = lex.next();
			lex.unread(second);
			lex.unread(first);
			if (first == "++" || first == "--") return true;
			if (!isName(first)) return false;
			return isAssignOp(second);
		}

		static void skipBraceBlock(BcLex& lex) {
			int depth = 1;
			while (depth > 0) {
				const std::string token = lex.next();
				if (token.empty()) return;
				if (token == "{") ++depth;
				else if (token == "}") --depth;
			}
		}

		static void skipControlledStmt(BcLex& lex) {
			std::string token = lex.next();
			if (token == "{") {
				skipBraceBlock(lex);
				return;
			}

			while (!token.empty() && token != ";" && token != "\n") token = lex.next();
		}

		static void evalLine(BcLex& lex, BcState& state);

		static void printBcResult(double value, const BcState& state) {
			if (state.scale == 0) std::printf("%lld\n", static_cast<long long>(value));
			else                  std::printf("%.*f\n", state.scale, value);
		}

		static void evalIf(BcLex& lex, BcState& state) {
			lex.next();
			if (lex.peek() == "(") lex.next();

			BcEval eval(lex, state);
			const double cond = eval.parseExpr();
			if (lex.peek() == ")") lex.next();

			if (cond != 0) evalLine(lex, state);
			else           skipControlledStmt(lex);
		}

		static void evalBraceBlock(BcLex& lex, BcState& state) {
			lex.next();
			while (lex.peek() != "}" && !lex.peek().empty()) evalLine(lex, state);
			if (lex.peek() == "}") lex.next();
		}

		static void evalExprStmt(BcLex& lex, BcState& state) {
			const bool suppress = startsAssign(lex);
			BcEval eval(lex, state);
			const double value = eval.parseExpr();
			if (!suppress) printBcResult(value, state);
		}

		static void evalLine(BcLex& lex, BcState& state) {
			for (;;) {
				const std::string token = lex.peek();
				if (token.empty()) return;
				if (token == "}") return;
				if (token == ";") {
					lex.next();
					continue;
				}

				if (token == "quit" || token == "halt") {
					lex.next();
					state.exit_now = true;
					return;
				}

				if (token == "if") {
					evalIf(lex, state);
					continue;
				}

				if (token == "{") {
					evalBraceBlock(lex, state);
					continue;
				}

				evalExprStmt(lex, state);
				if (lex.peek() == ";") lex.next();
				if (lex.peek().empty() || lex.peek() == "}") return;
			}
		}

		static void evalText(const std::string& text, BcState& state) {
			BcLex lex(text);
			while (!lex.peek().empty() && !state.exit_now) evalLine(lex, state);
		}

		static void runStream(FILE* file, BcState& state) {
			std::string buffer;
			int c = 0;
			while ((c = std::fgetc(file)) != EOF && !state.exit_now) {
				buffer.push_back(static_cast<char>(c));
				if (c != '\n') continue;

				evalText(buffer, state);
				buffer.clear();
			}

			if (!buffer.empty() && !state.exit_now) evalText(buffer, state);
		}

		static void diagnoseFileError(const char* tool, const std::string& path) {
			std::fprintf(stderr, "wbsh: %s: %s: %s\n",
				tool, path.c_str(), std::strerror(errno));
		}

		static bool isIgnoredOption(const std::string& arg) {
			return arg.size() > 1 && arg[0] == '-';
		}

		static void parseBcArgs(const std::vector<std::string>& args, BcState& state,
				std::vector<std::string>& files) {
			for (std::size_t i = 0; i < args.size(); ++i) {
				const std::string& arg = args[i];
				if (arg == "-l" || arg == "--mathlib") {
					state.with_lib = true;
					state.scale = kMathLibScale;
					continue;
				}

				if (arg == "-q" || arg == "--quiet") {
					state.quiet = true;
					continue;
				}

				if (arg == "-e" && i + 1 < args.size()) {
					evalText(args[++i], state);
					continue;
				}

				if (isIgnoredOption(arg)) continue;
				files.push_back(arg);
			}
		}

		static void runFile(Executor& exec, const std::string& name, BcState& state) {
			FILE* file = openUtf8(exec.pathConv().toWin32(name), "rb");
			if (file == nullptr) {
				diagnoseFileError("bc", name);
				return;
			}

			runStream(file, state);
			std::fclose(file);
		}

		static int builtin_bc(Executor& exec, const std::vector<std::string>& args) {
			BcState state;
			std::vector<std::string> files;
			parseBcArgs(args, state, files);

			if (files.empty()) {
				runStream(stdin, state);
				return 0;
			}

			for (const auto& name : files) runFile(exec, name, state);
			return 0;
		}

	}  // namespace bc_detail

	void registerBcBuiltin(Executor& exec) {
		exec.registerBuiltin("bc", bc_detail::builtin_bc);
	}

}  // namespace wbsh

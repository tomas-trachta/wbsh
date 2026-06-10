/**
 * @file coreutils_bc.cpp
 * @brief Pragmatic bc(1) calculator.
 *
 * Parses arithmetic expressions, assignments, and simple control flow.
 * Uses double precision; `scale` controls fraction display. Math
 * library (`-l`) provides sqrt, s, c, e, l, a.
 *
 * Split out of coreutils.cpp for IDE friendliness and to keep the main
 * coreutils file focused on shell-style utilities. The single public
 * entry point is registerBcBuiltin().
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

		struct BcState {
			std::map<std::string, double> vars;
			int scale = 0;
			bool with_lib = false;
			bool quiet = false;
			bool exit_now = false;
		};

		struct BcLex {
			const std::string& src;
			std::size_t i = 0;
			std::vector<std::string> pushback;
			explicit BcLex(const std::string& s) : src(s) {}

			void skip() {
				while (i < src.size() && (src[i] == ' ' || src[i] == '\t')) ++i;
				while (i + 1 < src.size() && src[i] == '\\' && src[i + 1] == '\n') i += 2;
				if (i < src.size() && src[i] == '#') {
					while (i < src.size() && src[i] != '\n') ++i;
				}
			}
			std::string next() {
				if (!pushback.empty()) {
					std::string t = pushback.back();
					pushback.pop_back();
					return t;
				}
				skip();
				if (i >= src.size()) return "";
				char c = src[i];
				if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
					std::size_t s = i;
					while (i < src.size()
					       && (std::isdigit(static_cast<unsigned char>(src[i])) || src[i] == '.'))
						++i;
					return src.substr(s, i - s);
				}
				if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
					std::size_t s = i;
					while (i < src.size()
					       && (std::isalnum(static_cast<unsigned char>(src[i])) || src[i] == '_'))
						++i;
					return src.substr(s, i - s);
				}
				if (c == '<' || c == '>' || c == '=' || c == '!') {
					if (i + 1 < src.size() && src[i + 1] == '=') {
						std::string r = src.substr(i, 2);
						i += 2;
						return r;
					}
				}
				if (c == '\n') { ++i; return ";"; }
				char r = src[i++];
				return std::string(1, r);
			}
			std::string peek() {
				if (pushback.empty()) {
					std::string t = next();
					if (t.empty()) return t;
					pushback.push_back(t);
				}
				return pushback.back();
			}
			void unread(std::string t) { pushback.push_back(std::move(t)); }
		};

		static bool isName(const std::string& t) {
			if (t.empty()) return false;
			const char c = t[0];
			if (!(std::isalpha(static_cast<unsigned char>(c)) || c == '_')) return false;
			for (char x : t) {
				if (!std::isalnum(static_cast<unsigned char>(x)) && x != '_') return false;
			}
			return true;
		}
		static bool isNumber(const std::string& t) {
			if (t.empty()) return false;
			for (char c : t) {
				if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.') return false;
			}
			return true;
		}

		// Recursive-descent expression parser/evaluator. Operates directly
		// on the lexer stream and BcState.
		struct BcEval {
			BcLex& lex;
			BcState& st;
			BcEval(BcLex& l, BcState& s) : lex(l), st(s) {}

			double parseExpr() { return parseAssign(); }

			double parseAssign() {
				// lookahead: NAME = expr (or any compound assignment)
				const std::string t = lex.peek();
				if (isName(t)) {
					std::string n = lex.next();
					const std::string op = lex.peek();
					if (op == "=" || op == "+=" || op == "-=" || op == "*="
					    || op == "/=" || op == "%=" || op == "^=")
					{
						lex.next();
						const double rhs = parseAssign();
						const double cur = st.vars.count(n) ? st.vars[n] : 0.0;
						double v = rhs;
						if      (op == "+=") v = cur + rhs;
						else if (op == "-=") v = cur - rhs;
						else if (op == "*=") v = cur * rhs;
						else if (op == "/=") v = rhs == 0 ? 0 : cur / rhs;
						else if (op == "%=") v = rhs == 0 ? 0 : std::fmod(cur, rhs);
						else if (op == "^=") v = std::pow(cur, rhs);
						if (n == "scale") st.scale = static_cast<int>(v);
						else              st.vars[n] = v;
						return v;
					}
					lex.unread(std::move(n));
				}
				return parseOr();
			}
			double parseOr() {
				double a = parseAnd();
				while (lex.peek() == "||") {
					lex.next();
					const double b = parseAnd();
					a = (a != 0 || b != 0) ? 1 : 0;
				}
				return a;
			}
			double parseAnd() {
				double a = parseCmp();
				while (lex.peek() == "&&") {
					lex.next();
					const double b = parseCmp();
					a = (a != 0 && b != 0) ? 1 : 0;
				}
				return a;
			}
			double parseCmp() {
				double a = parseAddSub();
				while (true) {
					const std::string p = lex.peek();
					if (p == "==" || p == "!=" || p == "<" || p == "<="
					    || p == ">" || p == ">=")
					{
						lex.next();
						const double b = parseAddSub();
						bool r = false;
						if      (p == "==") r = a == b;
						else if (p == "!=") r = a != b;
						else if (p == "<")  r = a <  b;
						else if (p == "<=") r = a <= b;
						else if (p == ">")  r = a >  b;
						else                r = a >= b;
						a = r ? 1.0 : 0.0;
					} else {
						break;
					}
				}
				return a;
			}
			double parseAddSub() {
				double a = parseMul();
				while (true) {
					const std::string p = lex.peek();
					if      (p == "+") { lex.next(); a += parseMul(); }
					else if (p == "-") { lex.next(); a -= parseMul(); }
					else break;
				}
				return a;
			}
			double parseMul() {
				double a = parseExp();
				while (true) {
					const std::string p = lex.peek();
					if (p == "*") { lex.next(); a *= parseExp(); }
					else if (p == "/") {
						lex.next();
						const double b = parseExp();
						a = b == 0 ? 0 : a / b;
					} else if (p == "%") {
						lex.next();
						const double b = parseExp();
						a = b == 0 ? 0 : std::fmod(a, b);
					} else {
						break;
					}
				}
				return a;
			}
			double parseExp() {
				double a = parseUnary();
				if (lex.peek() == "^") {
					lex.next();
					a = std::pow(a, parseExp());
				}
				return a;
			}
			double parseUnary() {
				const std::string p = lex.peek();
				if (p == "-")  { lex.next(); return -parseUnary(); }
				if (p == "+")  { lex.next(); return  parseUnary(); }
				if (p == "!")  { lex.next(); return parseUnary() == 0 ? 1 : 0; }
				if (p == "++") {
					lex.next();
					const std::string n = lex.next();
					const double v = (st.vars.count(n) ? st.vars[n] : 0.0) + 1;
					st.vars[n] = v;
					return v;
				}
				if (p == "--") {
					lex.next();
					const std::string n = lex.next();
					const double v = (st.vars.count(n) ? st.vars[n] : 0.0) - 1;
					st.vars[n] = v;
					return v;
				}
				return parsePrimary();
			}
			double parsePrimary() {
				const std::string t = lex.next();
				if (t.empty()) return 0;
				if (t == "(") {
					const double v = parseExpr();
					if (lex.peek() == ")") lex.next();
					return v;
				}
				if (isNumber(t)) {
					double v = 0;
					parseDouble(t, v);
					return v;
				}
				if (isName(t)) {
					if (lex.peek() == "(") {
						// Function call
						lex.next();
						std::vector<double> args;
						if (lex.peek() != ")") {
							args.push_back(parseExpr());
							while (lex.peek() == ",") {
								lex.next();
								args.push_back(parseExpr());
							}
						}
						if (lex.peek() == ")") lex.next();
						return callFunc(t, args);
					}
					// Postfix ++/-- — captures the value before the bump.
					if (lex.peek() == "++" || lex.peek() == "--") {
						const double cur = st.vars.count(t) ? st.vars[t] : 0.0;
						const double nv = (lex.next() == "++") ? cur + 1 : cur - 1;
						st.vars[t] = nv;
						return cur;
					}
					if (t == "scale") return st.scale;
					return st.vars.count(t) ? st.vars[t] : 0.0;
				}
				return 0;
			}
			double callFunc(const std::string& n, const std::vector<double>& a) {
				auto get0 = [&]() { return a.empty() ? 0 : a[0]; };
				if (n == "sqrt") return std::sqrt(get0());
				if (st.with_lib) {
					if (n == "s") return std::sin(get0());
					if (n == "c") return std::cos(get0());
					if (n == "e") return std::exp(get0());
					if (n == "l") return std::log(get0());
					if (n == "a") return std::atan(get0());
				}
				if (n == "length") {
					char buf[64];
					std::snprintf(buf, sizeof(buf), "%.20g", get0());
					const std::string s = buf;
					int c = 0;
					for (char x : s) {
						if (std::isdigit(static_cast<unsigned char>(x))) ++c;
					}
					return static_cast<double>(c);
				}
				return 0;
			}
		};

		// Suppress-output check: an expression starting with `NAME = ...`
		// (or `NAME op= ...`, or `++NAME` / `--NAME`) is an assignment and
		// bc does not print its value. Cheap two-token lookahead before eval.
		static bool startsAssign(BcLex& lex) {
			std::string t1 = lex.next();
			if (t1.empty()) return false;
			std::string t2 = lex.next();
			lex.unread(t2);
			lex.unread(t1);
			if (t1 == "++" || t1 == "--") return true;
			if (!isName(t1)) return false;
			return t2 == "=" || t2 == "+=" || t2 == "-=" || t2 == "*="
			    || t2 == "/=" || t2 == "%=" || t2 == "^=";
		}

		// Skip the controlled statement of an `if (cond) STMT` whose cond
		// was false. STMT may be a `{ ... }` block (count braces) or a
		// single statement (run to next `;` / `\n`).
		static void skipControlledStmt(BcLex& lex) {
			std::string nx = lex.next();
			if (nx == "{") {
				int d = 1;
				while (d > 0) {
					const std::string t = lex.next();
					if (t.empty()) return;
					if      (t == "{") ++d;
					else if (t == "}") --d;
				}
				return;
			}
			while (!nx.empty() && nx != ";" && nx != "\n") nx = lex.next();
		}

		static void evalLine(BcLex& lex, BcState& st);

		// Print one bc expression result; honours `scale`.
		static void printBcResult(double v, const BcState& st) {
			if (st.scale == 0) std::printf("%lld\n", static_cast<long long>(v));
			else               std::printf("%.*f\n", st.scale, v);
		}

		static void evalLine(BcLex& lex, BcState& st) {
			while (true) {
				const std::string p = lex.peek();
				if (p.empty()) return;
				if (p == ";") { lex.next(); continue; }
				if (p == "quit" || p == "halt") {
					lex.next();
					st.exit_now = true;
					return;
				}
				if (p == "if") {
					lex.next();
					if (lex.peek() == "(") lex.next();
					BcEval e(lex, st);
					const double cond = e.parseExpr();
					if (lex.peek() == ")") lex.next();
					if (cond) evalLine(lex, st);
					else      skipControlledStmt(lex);
					continue;
				}
				if (p == "{") {
					lex.next();
					while (lex.peek() != "}" && !lex.peek().empty()) evalLine(lex, st);
					if (lex.peek() == "}") lex.next();
					continue;
				}
				if (p == "}") return;

				const bool suppress = startsAssign(lex);
				BcEval e(lex, st);
				const double v = e.parseExpr();
				if (!suppress) printBcResult(v, st);
				if (lex.peek() == ";") lex.next();
				if (lex.peek().empty() || lex.peek() == "}") return;
			}
		}

		// Read `f` line-by-line and feed each line through the bc evaluator.
		// Final partial line (no trailing newline) is also evaluated.
		static void runStream(FILE* f, BcState& st) {
			std::string buf;
			int c;
			while ((c = std::fgetc(f)) != EOF && !st.exit_now) {
				buf.push_back(static_cast<char>(c));
				if (c == '\n') {
					BcLex lex(buf);
					while (!lex.peek().empty() && !st.exit_now) evalLine(lex, st);
					buf.clear();
				}
			}
			if (!buf.empty() && !st.exit_now) {
				BcLex lex(buf);
				while (!lex.peek().empty() && !st.exit_now) evalLine(lex, st);
			}
		}

		static void diagnoseFileError(const char* tool, const std::string& path) {
			std::fprintf(stderr, "wbsh: %s: %s: %s\n",
				tool, path.c_str(), std::strerror(errno));
		}

		static int builtin_bc(Executor& exec, const std::vector<std::string>& args) {
			BcState st;
			std::vector<std::string> files;
			for (std::size_t i = 0; i < args.size(); ++i) {
				const std::string& a = args[i];
				if (a == "-l" || a == "--mathlib") {
					st.with_lib = true;
					st.scale = 20;
				} else if (a == "-q" || a == "--quiet") {
					st.quiet = true;
				} else if (a == "-e" && i + 1 < args.size()) {
					BcLex lex(args[++i]);
					while (!lex.peek().empty() && !st.exit_now) evalLine(lex, st);
				} else if (!a.empty() && a[0] == '-' && a != "-") {
					/* ignore unknown opt */
				} else {
					files.push_back(a);
				}
			}

			if (files.empty()) {
				runStream(stdin, st);
				return 0;
			}
			for (const auto& fn : files) {
				FILE* f = openUtf8(exec.pathConv().toWin32(fn), "rb");
				if (!f) {
					diagnoseFileError("bc", fn);
					continue;
				}
				runStream(f, st);
				std::fclose(f);
			}
			return 0;
		}

	}  // namespace bc_detail

	void registerBcBuiltin(Executor& exec) {
		exec.registerBuiltin("bc", bc_detail::builtin_bc);
	}

}  // namespace wbsh

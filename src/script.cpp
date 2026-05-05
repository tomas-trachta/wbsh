/**
 * @file script.cpp
 * @brief Non-interactive run-on-source driver.
 *
 * Implements the `wbsh -c <cmd>` and `wbsh <file>` paths: lexes,
 * parses, optionally dumps tokens / AST / expanded words, and runs
 * the executor.
 */

#include "script.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>

#  include <io.h>
#endif /* _WIN32 */

#include <iostream>
#include <ostream>
#include <string>

#include "ast.h"
#include "environment.h"
#include "executor.h"
#include "expander.h"
#include "lexer.h"
#include "parser.h"
#include "printer.h"
#include "setup.h"

namespace wbsh {

	static bool stdoutIsTty() {
#ifdef _WIN32
		return _isatty(_fileno(stdout)) != 0;
#else
		return false;
#endif /* _WIN32 */
	}

	static bool stderrIsTty() {
#ifdef _WIN32
		return _isatty(_fileno(stderr)) != 0;
#else
		return false;
#endif /* _WIN32 */
	}

	// ----- AST -> expanded-words pretty-printer ---------------------------
	// Used only by `wbsh -e`. Walks the AST and prints, per command, each
	// word's expansion result. Provides a debugging view roughly between
	// "token dump" and "actually run it".

	static void walkAndExpand(const Node& n, Expander& exp, Environment& env,
	                          std::ostream& os, int depth);

	static void indent(std::ostream& os, int n) {
		for (int i = 0; i < n; ++i) os << "  ";
	}

	static void escape(std::ostream& os, const std::string& s) {
			os << '"';
			for (char c : s) {
				switch (c) {
				case '\n': os << "\\n"; break;
				case '\t': os << "\\t"; break;
				case '\r': os << "\\r"; break;
				case '\\': os << "\\\\"; break;
				case '"':  os << "\\\""; break;
				default:   os << c;
				}
			}
			os << '"';
		}

	static void expandSimpleCommand(const SimpleCommand& sc, Expander& exp,
	                                Environment& env,
	                                std::ostream& os, int depth) {
			indent(os, depth);
			os << "SimpleCommand @ " << sc.loc.line << ":" << sc.loc.column << "\n";
			// Commit assignments to env when this is a "pure" assignment command
			// (no command words). Mirrors bash's `a=1` (set) vs `a=1 cmd` (per-call).
			bool commit = sc.words.empty();
			for (const auto& a : sc.assignments) {
				indent(os, depth + 1);
				std::string v;
				try { v = exp.expandStringValue(a.value); }
				catch (const ExpandError& e) { os << "<expand error: " << e.what() << ">"; }
				if (commit) env.set(a.name, v);
				os << "Assign " << a.name << "=";
				escape(os, v);
				os << "\n";
			}
			for (const auto& w : sc.words) {
				indent(os, depth + 1);
				os << "Word raw=";
				escape(os, w.raw);
				os << " ->";
				try {
					auto fields = exp.expandWord(w);
					if (fields.empty()) {
						os << " (vanished)";
					}
					for (const auto& f : fields) {
						os << ' ';
						escape(os, f);
					}
				} catch (const ExpandError& e) {
					os << " <expand error: " << e.what() << ">";
				}
				os << "\n";
			}
			for (const auto& r : sc.redirs) {
				indent(os, depth + 1);
				os << "Redir " << redirOpName(r.op);
				if (r.fd != -1) os << " fd=" << r.fd;
				os << " target=";
				try { escape(os, exp.expandStringValue(r.target)); }
				catch (const ExpandError& e) { os << "<expand error: " << e.what() << ">"; }
				if (r.op == RedirOp::DLess || r.op == RedirOp::DLessDash) {
					os << " body=";
					escape(os, exp.expandHeredoc(r.heredoc_body, r.heredoc_quoted));
				}
				os << "\n";
			}
		}

	static void walkAndExpand(const Node& n, Expander& exp, Environment& env,
	                          std::ostream& os, int depth) {
		switch (n.kind) {
			case Node::Kind::SimpleCommand:
				expandSimpleCommand(static_cast<const SimpleCommand&>(n), exp, env, os, depth);
				break;
			case Node::Kind::Pipeline: {
				const auto& p = static_cast<const Pipeline&>(n);
				indent(os, depth); os << "Pipeline" << (p.bang ? " (bang)" : "") << "\n";
				for (const auto& c : p.commands) walkAndExpand(*c, exp, env, os, depth + 1);
				break;
			}
			case Node::Kind::AndOr: {
				const auto& a = static_cast<const AndOr&>(n);
				indent(os, depth); os << (a.op == AndOr::Op::AndIf ? "&&" : "||") << "\n";
				walkAndExpand(*a.left, exp, env, os, depth + 1);
				walkAndExpand(*a.right, exp, env, os, depth + 1);
				break;
			}
			case Node::Kind::List: {
				const auto& l = static_cast<const List&>(n);
				for (const auto& it : l.items) walkAndExpand(*it.command, exp, env, os, depth);
				break;
			}
			case Node::Kind::BraceGroup: {
				const auto& bg = static_cast<const BraceGroup&>(n);
				indent(os, depth); os << "BraceGroup\n";
				if (bg.body) walkAndExpand(*bg.body, exp, env, os, depth + 1);
				break;
			}
			case Node::Kind::Subshell: {
				const auto& ss = static_cast<const Subshell&>(n);
				indent(os, depth); os << "Subshell\n";
				if (ss.body) walkAndExpand(*ss.body, exp, env, os, depth + 1);
				break;
			}
			case Node::Kind::IfClause: {
				const auto& ic = static_cast<const IfClause&>(n);
				indent(os, depth); os << "If\n";
				for (std::size_t i = 0; i < ic.branches.size(); ++i) {
					indent(os, depth + 1);
					os << (i == 0 ? "if-cond:" : "elif-cond:") << "\n";
					if (ic.branches[i].cond) walkAndExpand(*ic.branches[i].cond, exp, env, os, depth + 2);
					indent(os, depth + 1); os << "then:\n";
					if (ic.branches[i].body) walkAndExpand(*ic.branches[i].body, exp, env, os, depth + 2);
				}
				if (ic.else_body) {
					indent(os, depth + 1); os << "else:\n";
					walkAndExpand(*ic.else_body, exp, env, os, depth + 2);
				}
				break;
			}
			case Node::Kind::WhileClause: {
				const auto& w = static_cast<const WhileClause&>(n);
				indent(os, depth); os << (w.until ? "Until\n" : "While\n");
				indent(os, depth + 1); os << "cond:\n";
				if (w.cond) walkAndExpand(*w.cond, exp, env, os, depth + 2);
				indent(os, depth + 1); os << "do:\n";
				if (w.body) walkAndExpand(*w.body, exp, env, os, depth + 2);
				break;
			}
			case Node::Kind::ForClause: {
				const auto& f = static_cast<const ForClause&>(n);
				indent(os, depth); os << "For " << f.var << "\n";
				if (f.has_in) {
					indent(os, depth + 1); os << "items:";
					for (const auto& w : f.items) {
						try {
							auto fields = exp.expandWord(w);
							for (const auto& s : fields) { os << ' '; escape(os, s); }
						} catch (const ExpandError& e) {
							os << " <error: " << e.what() << ">";
						}
					}
					os << "\n";
				}
				indent(os, depth + 1); os << "do:\n";
				if (f.body) walkAndExpand(*f.body, exp, env, os, depth + 2);
				break;
			}
			case Node::Kind::CaseClause: {
				const auto& c = static_cast<const CaseClause&>(n);
				indent(os, depth); os << "Case subject=";
				try { escape(os, exp.expandStringValue(c.subject)); }
				catch (const ExpandError& e) { os << "<error: " << e.what() << ">"; }
				os << "\n";
				for (const auto& it : c.items) {
					indent(os, depth + 1); os << "patterns:";
					for (const auto& p : it.patterns) {
						try { os << ' '; escape(os, exp.expandStringValue(p)); }
						catch (const ExpandError&) { os << " <err>"; }
					}
					os << "\n";
					if (it.body) {
						indent(os, depth + 2); os << "body:\n";
						walkAndExpand(*it.body, exp, env, os, depth + 3);
					}
				}
				break;
			}
			case Node::Kind::FunctionDef: {
				const auto& f = static_cast<const FunctionDef&>(n);
				indent(os, depth); os << "FunctionDef " << f.name << "\n";
				if (f.body) walkAndExpand(*f.body, exp, env, os, depth + 1);
				break;
			}
			}
	}

	int runOnSource(const std::string& src, bool show_tokens, bool show_ast,
	                bool do_expand, bool do_run) {
		Lexer lex(src);
		auto tokens = lex.tokenize();

		bool out_color = stdoutIsTty();
		bool err_color = stderrIsTty();
		auto header = [&](const char* title) {
			if (out_color) std::cout << "\x1b[35;1m── " << title << " ──\x1b[0m\n";
			else           std::cout << "--- " << title << " ---\n";
		};
		const char* ep = err_color ? "\x1b[31;1m" : "";
		const char* el = err_color ? "\x1b[33m"   : "";
		const char* er = err_color ? "\x1b[0m"    : "";

		if (show_tokens) {
			header("TOKENS");
			dumpTokens(std::cout, tokens);
		}
		for (const auto& e : lex.errors()) {
			std::cerr << ep << "lex error" << er << " "
				<< el << e.loc.line << ":" << e.loc.column << er
				<< ": " << e.message << "\n";
		}

		Parser parser(std::move(tokens), src);
		auto root = parser.parseProgram();
		if (show_ast) {
			header("AST");
			if (root) dumpAst(std::cout, *root);
			else      std::cout << "(empty)\n";
		}
		for (const auto& e : parser.errors()) {
			std::cerr << ep << "parse error" << er << " "
				<< el << e.loc.line << ":" << e.loc.column << er
				<< ": " << e.message << "\n";
		}

		if (do_expand && root) {
			Environment env;
			prepareEnv(env);
			Expander exp(env, /*sub=*/nullptr);
			header("EXPANSIONS");
			walkAndExpand(*root, exp, env, std::cout, 0);
		}

		if (do_run && root) {
			Environment env;
			prepareEnv(env);
			Executor exec(env);
			exec.setSourceText(src);
			absorbInheritedState(env, exec);
			try {
				return exec.execute(*root);
			} catch (ShellExit& e) {
				return e.status;
			}
		}

		return (parser.errors().empty() && lex.errors().empty()) ? 0 : 1;
	}

}  // namespace wbsh

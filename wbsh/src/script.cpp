/**
 * @file script.cpp
 * @brief Non-interactive (single-source) execution entry point.
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
#include <vector>

#include "ast.h"
#include "environment.h"
#include "executor.h"
#include "expander.h"
#include "lexer.h"
#include "parser.h"
#include "printer.h"
#include "setup.h"

namespace wbsh {

	namespace script_detail {

		struct ExpandDump {
			Expander& expander;
			Environment& env;
			std::ostream& os;
		};

		struct ErrorColors {
			const char* kind;
			const char* location;
			const char* reset;
		};

	}  // namespace script_detail

	using script_detail::ErrorColors;
	using script_detail::ExpandDump;

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

	static void dumpExpanded(ExpandDump& dump, const Node& node, int depth);

	static void indent(std::ostream& os, int depth) {
		for (int i = 0; i < depth; ++i) os << "  ";
	}

	static void escape(std::ostream& os, const std::string& text) {
		os << '"';
		for (char c : text) {
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

	static void writeLine(std::ostream& os, int depth, const std::string& text) {
		indent(os, depth);
		os << text << "\n";
	}

	static void writeQuotedFields(std::ostream& os, const std::vector<std::string>& fields) {
		for (const auto& field : fields) {
			os << ' ';
			escape(os, field);
		}
	}

	static void writeExpandedValue(ExpandDump& dump, const Word& word, const char* error_label) {
		const std::string value = dump.expander.expandStringValue(word);
		if (dump.expander.failed()) {
			dump.os << error_label << dump.expander.takeError() << ">";
			return;
		}

		escape(dump.os, value);
	}

	static void dumpExpandedBody(ExpandDump& dump, const char* label, const Node* body, int depth) {
		writeLine(dump.os, depth, label);
		if (body != nullptr) dumpExpanded(dump, *body, depth + 1);
	}

	static void dumpExpandedAssignments(ExpandDump& dump, const SimpleCommand& command, int depth) {
		const bool bare_assignments = command.words.empty();
		for (const auto& assignment : command.assignments) {
			indent(dump.os, depth + 1);
			const std::string value = dump.expander.expandStringValue(assignment.value);
			if (dump.expander.failed()) {
				dump.os << "<expand error: " << dump.expander.takeError() << ">";
			}

			if (bare_assignments) dump.env.set(assignment.name, value);
			dump.os << "Assign " << assignment.name << "=";
			escape(dump.os, value);
			dump.os << "\n";
		}
	}

	static void dumpExpandedWords(ExpandDump& dump, const SimpleCommand& command, int depth) {
		for (const auto& word : command.words) {
			indent(dump.os, depth + 1);
			dump.os << "Word raw=";
			escape(dump.os, word.raw);
			dump.os << " ->";

			const auto fields = dump.expander.expandWord(word);
			if (dump.expander.failed()) {
				dump.os << " <expand error: " << dump.expander.takeError() << ">";
			} else if (fields.empty()) {
				dump.os << " (vanished)";
			}

			writeQuotedFields(dump.os, fields);
			dump.os << "\n";
		}
	}

	static void dumpExpandedRedirs(ExpandDump& dump, const SimpleCommand& command, int depth) {
		for (const auto& redir : command.redirs) {
			indent(dump.os, depth + 1);
			dump.os << "Redir " << redirOpName(redir.op);
			if (redir.fd != -1) dump.os << " fd=" << redir.fd;
			dump.os << " target=";
			writeExpandedValue(dump, redir.target, "<expand error: ");

			if (redir.op == RedirOp::DLess || redir.op == RedirOp::DLessDash) {
				dump.os << " body=";
				const std::string body =
					dump.expander.expandHeredoc(redir.heredoc_body, redir.heredoc_quoted);
				escape(dump.os, body);
				if (dump.expander.failed()) dump.expander.takeError();
			}

			dump.os << "\n";
		}
	}

	static void dumpExpandedSimpleCommand(ExpandDump& dump, const SimpleCommand& command,
	                                      int depth) {
		indent(dump.os, depth);
		dump.os << "SimpleCommand @ " << command.loc.line << ":" << command.loc.column << "\n";
		dumpExpandedAssignments(dump, command, depth);
		dumpExpandedWords(dump, command, depth);
		dumpExpandedRedirs(dump, command, depth);
	}

	static void dumpExpandedPipeline(ExpandDump& dump, const Pipeline& pipeline, int depth) {
		writeLine(dump.os, depth, pipeline.bang ? "Pipeline (bang)" : "Pipeline");
		for (const auto& command : pipeline.commands) dumpExpanded(dump, *command, depth + 1);
	}

	static void dumpExpandedAndOr(ExpandDump& dump, const AndOr& andor, int depth) {
		writeLine(dump.os, depth, andor.op == AndOr::Op::AndIf ? "&&" : "||");
		dumpExpanded(dump, *andor.left, depth + 1);
		dumpExpanded(dump, *andor.right, depth + 1);
	}

	static void dumpExpandedList(ExpandDump& dump, const List& list, int depth) {
		for (const auto& item : list.items) dumpExpanded(dump, *item.command, depth);
	}

	static void dumpExpandedIf(ExpandDump& dump, const IfClause& clause, int depth) {
		writeLine(dump.os, depth, "If");
		for (std::size_t i = 0; i < clause.branches.size(); ++i) {
			const char* label = (i == 0) ? "if-cond:" : "elif-cond:";
			dumpExpandedBody(dump, label, clause.branches[i].cond, depth + 1);
			dumpExpandedBody(dump, "then:", clause.branches[i].body, depth + 1);
		}

		if (clause.else_body == nullptr) return;

		dumpExpandedBody(dump, "else:", clause.else_body, depth + 1);
	}

	static void dumpExpandedWhile(ExpandDump& dump, const WhileClause& loop, int depth) {
		writeLine(dump.os, depth, loop.until ? "Until" : "While");
		dumpExpandedBody(dump, "cond:", loop.cond, depth + 1);
		dumpExpandedBody(dump, "do:", loop.body, depth + 1);
	}

	static void dumpExpandedFunctionDef(ExpandDump& dump, const FunctionDef& function, int depth) {
		writeLine(dump.os, depth, "FunctionDef " + function.name);
		if (function.body != nullptr) dumpExpanded(dump, *function.body, depth + 1);
	}

	static void dumpExpandedForItems(ExpandDump& dump, const ForClause& loop, int depth) {
		indent(dump.os, depth + 1);
		dump.os << "items:";
		for (const auto& word : loop.items) {
			const auto fields = dump.expander.expandWord(word);
			if (dump.expander.failed()) {
				dump.os << " <error: " << dump.expander.takeError() << ">";
				continue;
			}

			writeQuotedFields(dump.os, fields);
		}

		dump.os << "\n";
	}

	static void dumpExpandedFor(ExpandDump& dump, const ForClause& loop, int depth) {
		writeLine(dump.os, depth, "For " + loop.var);
		if (loop.has_in) dumpExpandedForItems(dump, loop, depth);
		dumpExpandedBody(dump, "do:", loop.body, depth + 1);
	}

	static void dumpExpandedCasePatterns(ExpandDump& dump, const CaseClause::Item& item,
	                                     int depth) {
		indent(dump.os, depth + 1);
		dump.os << "patterns:";
		for (const auto& pattern : item.patterns) {
			dump.os << ' ';
			const std::string text = dump.expander.expandStringValue(pattern);
			if (dump.expander.failed()) {
				dump.expander.takeError();
				dump.os << "<err>";
				continue;
			}

			escape(dump.os, text);
		}

		dump.os << "\n";
	}

	static void dumpExpandedCase(ExpandDump& dump, const CaseClause& clause, int depth) {
		indent(dump.os, depth);
		dump.os << "Case subject=";
		writeExpandedValue(dump, clause.subject, "<error: ");
		dump.os << "\n";

		for (const auto& item : clause.items) {
			dumpExpandedCasePatterns(dump, item, depth);
			if (item.body != nullptr) dumpExpandedBody(dump, "body:", item.body, depth + 2);
		}
	}

	static void dumpExpanded(ExpandDump& dump, const Node& node, int depth) {
		switch (node.kind) {
		case Node::Kind::SimpleCommand:
			dumpExpandedSimpleCommand(dump, static_cast<const SimpleCommand&>(node), depth);
			return;
		case Node::Kind::Pipeline:
			dumpExpandedPipeline(dump, static_cast<const Pipeline&>(node), depth);
			return;
		case Node::Kind::AndOr:
			dumpExpandedAndOr(dump, static_cast<const AndOr&>(node), depth);
			return;
		case Node::Kind::List:
			dumpExpandedList(dump, static_cast<const List&>(node), depth);
			return;
		case Node::Kind::BraceGroup:
			dumpExpandedBody(dump, "BraceGroup", static_cast<const BraceGroup&>(node).body, depth);
			return;
		case Node::Kind::Subshell:
			dumpExpandedBody(dump, "Subshell", static_cast<const Subshell&>(node).body, depth);
			return;
		case Node::Kind::IfClause:
			dumpExpandedIf(dump, static_cast<const IfClause&>(node), depth);
			return;
		case Node::Kind::WhileClause:
			dumpExpandedWhile(dump, static_cast<const WhileClause&>(node), depth);
			return;
		case Node::Kind::ForClause:
			dumpExpandedFor(dump, static_cast<const ForClause&>(node), depth);
			return;
		case Node::Kind::CaseClause:
			dumpExpandedCase(dump, static_cast<const CaseClause&>(node), depth);
			return;
		case Node::Kind::FunctionDef:
			dumpExpandedFunctionDef(dump, static_cast<const FunctionDef&>(node), depth);
			return;
		case Node::Kind::DBracket:
			writeLine(dump.os, depth, "DBracket");
			return;
		case Node::Kind::ArithCommand: {
			const auto& command = static_cast<const ArithCommand&>(node);
			writeLine(dump.os, depth, "ArithCommand " + command.expr);
			return;
		}
		}
	}

	static void printHeader(bool color, const char* title) {
		if (color) std::cout << "\x1b[35;1m── " << title << " ──\x1b[0m\n";
		else       std::cout << "--- " << title << " ---\n";
	}

	static ErrorColors errorColors(bool color) {
		if (!color) return { "", "", "" };
		return { "\x1b[31;1m", "\x1b[33m", "\x1b[0m" };
	}

	static void printDiagnostic(const ErrorColors& colors, const char* kind,
	                            const SourceLoc& loc, const std::string& message) {
		std::cerr << colors.kind << kind << colors.reset << " "
			<< colors.location << loc.line << ":" << loc.column << colors.reset
			<< ": " << message << "\n";
	}

	static void dumpExpansions(NodePtr root, bool header_color) {
		Environment env;
		prepareEnv(env);
		Expander expander(env, /*sub=*/nullptr);
		printHeader(header_color, "EXPANSIONS");

		ExpandDump dump{ expander, env, std::cout };
		dumpExpanded(dump, *root, 0);
	}

	static int executeProgram(NodePtr root, const std::string& src,
	                          const std::string& script_name) {
		Environment env;
		prepareEnv(env);
		if (!script_name.empty()) env.setShellName(script_name);

		Executor exec(env);
		exec.setSourceText(src);
		absorbInheritedState(env, exec);

		int status = exec.execute(*root);
		exec.consumeFlow(FlowSignal::Kind::Exit, &status);
		return status;
	}

	int runScript(const ScriptRun& run) {
		Lexer lex(run.source);
		auto tokens = lex.tokenize();
		const bool out_color = stdoutIsTty();
		const ErrorColors colors = errorColors(stderrIsTty());

		if (run.show_tokens) {
			printHeader(out_color, "TOKENS");
			dumpTokens(std::cout, tokens);
		}

		for (const auto& e : lex.errors()) {
			printDiagnostic(colors, "lex error", e.loc, e.message);
		}

		Parser parser(std::move(tokens), run.source);
		auto root = parser.parseProgram();
		if (run.show_ast) {
			printHeader(out_color, "AST");
			if (root != nullptr) dumpAst(std::cout, *root);
			else std::cout << "(empty)\n";
		}

		for (const auto& e : parser.errors()) {
			printDiagnostic(colors, "parse error", e.loc, e.message);
		}

		if (run.show_expansions && root != nullptr) dumpExpansions(root, out_color);
		if (run.execute && root != nullptr) {
			return executeProgram(root, run.source, run.script_name);
		}

		return (parser.errors().empty() && lex.errors().empty()) ? 0 : 1;
	}

}  // namespace wbsh

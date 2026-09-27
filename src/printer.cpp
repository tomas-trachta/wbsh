/**
 * @file printer.cpp
 * @brief Pretty-printers for the token stream and the AST.
 */

#include "printer.h"

#include <ostream>
#include <string>

namespace wbsh {

	static void indent(std::ostream& os, int depth) {
		for (int i = 0; i < depth; ++i) os << "  ";
	}

	static void writeLine(std::ostream& os, int depth, const char* text) {
		indent(os, depth);
		os << text << "\n";
	}

	static void escapeString(std::ostream& os, const std::string& text) {
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
	}

	static void dumpSegments(std::ostream& os, const std::vector<WordSegment>& segments) {
		os << "[";
		for (std::size_t i = 0; i < segments.size(); ++i) {
			if (i != 0) os << " ";

			const auto& segment = segments[i];
			os << segKindName(segment.kind);
			if (segment.kind == WordSegment::Kind::DoubleQuoted) {
				os << "(";
				dumpSegments(os, segment.nested);
				os << ")";
				continue;
			}

			os << "(\"";
			escapeString(os, segment.text);
			os << "\")";
		}

		os << "]";
	}

	static void dumpWord(std::ostream& os, int depth, const char* label, const Word& word) {
		indent(os, depth);
		os << label << " raw=\"";
		escapeString(os, word.raw);
		os << "\" segs=";
		dumpSegments(os, word.segments);
		os << "\n";
	}

	static void dumpRedir(std::ostream& os, int depth, const Redirection& redir) {
		indent(os, depth);
		os << "Redir " << redirOpName(redir.op);
		if (redir.fd != -1) os << " fd=" << redir.fd;
		os << " target=\"";
		escapeString(os, redir.target.raw);
		os << "\"";
		if (redir.op == RedirOp::DLess || redir.op == RedirOp::DLessDash) {
			os << " quoted=" << (redir.heredoc_quoted ? "y" : "n");
			os << " body=\"";
			escapeString(os, redir.heredoc_body);
			os << "\"";
		}

		os << "\n";
	}

	static void dumpRedirs(std::ostream& os, int depth, const std::vector<Redirection>& redirs) {
		for (const auto& redir : redirs) dumpRedir(os, depth, redir);
	}

	static void dumpNode(std::ostream& os, int depth, const Node& node);

	static void dumpListBody(std::ostream& os, int depth, const Node* body) {
		if (body == nullptr) {
			writeLine(os, depth, "(empty)");
			return;
		}

		dumpNode(os, depth, *body);
	}

	static void dumpSimpleCommand(std::ostream& os, int depth, const SimpleCommand& command) {
		for (const auto& assignment : command.assignments) {
			indent(os, depth + 1);
			os << "Assign " << assignment.name << "=";
			dumpSegments(os, assignment.value.segments);
			os << "\n";
		}

		for (const auto& word : command.words) dumpWord(os, depth + 1, "Word", word);
		dumpRedirs(os, depth + 1, command.redirs);
	}

	static void dumpPipeline(std::ostream& os, int depth, const Pipeline& pipeline) {
		if (pipeline.bang) writeLine(os, depth + 1, "(bang)");

		for (std::size_t i = 0; i < pipeline.commands.size(); ++i) {
			if (i > 0) {
				const bool merged = pipeline.stderr_to_stdout[i - 1];
				writeLine(os, depth + 1, merged ? "|& pipe" : "| pipe");
			}

			dumpNode(os, depth + 1, *pipeline.commands[i]);
		}
	}

	static void dumpAndOr(std::ostream& os, int depth, const AndOr& andor) {
		writeLine(os, depth + 1, andor.op == AndOr::Op::AndIf ? "&&" : "||");
		dumpNode(os, depth + 1, *andor.left);
		dumpNode(os, depth + 1, *andor.right);
	}

	static void dumpList(std::ostream& os, int depth, const List& list) {
		for (const auto& item : list.items) {
			writeLine(os, depth + 1, item.background ? "Item (bg)" : "Item");
			dumpNode(os, depth + 2, *item.command);
		}
	}

	static void dumpIfClause(std::ostream& os, int depth, const IfClause& clause) {
		for (std::size_t i = 0; i < clause.branches.size(); ++i) {
			writeLine(os, depth + 1, i == 0 ? "if-cond" : "elif-cond");
			dumpListBody(os, depth + 2, clause.branches[i].cond);
			writeLine(os, depth + 1, "then");
			dumpListBody(os, depth + 2, clause.branches[i].body);
		}

		if (clause.else_body != nullptr) {
			writeLine(os, depth + 1, "else");
			dumpListBody(os, depth + 2, clause.else_body);
		}

		dumpRedirs(os, depth + 1, clause.redirs);
	}

	static void dumpWhileClause(std::ostream& os, int depth, const WhileClause& loop) {
		writeLine(os, depth + 1, loop.until ? "until-cond" : "while-cond");
		dumpListBody(os, depth + 2, loop.cond);
		writeLine(os, depth + 1, "do");
		dumpListBody(os, depth + 2, loop.body);
		dumpRedirs(os, depth + 1, loop.redirs);
	}

	static void dumpForClause(std::ostream& os, int depth, const ForClause& loop) {
		if (loop.is_arith) {
			indent(os, depth + 1);
			os << "arith init=\"" << loop.arith_init << "\" cond=\"" << loop.arith_cond
				<< "\" update=\"" << loop.arith_update << "\"\n";
			writeLine(os, depth + 1, "do");
			dumpListBody(os, depth + 2, loop.body);
			dumpRedirs(os, depth + 1, loop.redirs);
			return;
		}

		indent(os, depth + 1);
		os << "var " << loop.var << "\n";
		if (loop.has_in) {
			writeLine(os, depth + 1, "in");
			for (const auto& word : loop.items) dumpWord(os, depth + 2, "Word", word);
		}

		writeLine(os, depth + 1, "do");
		dumpListBody(os, depth + 2, loop.body);
		dumpRedirs(os, depth + 1, loop.redirs);
	}

	static const char* caseTermName(CaseClause::Term term) {
		switch (term) {
		case CaseClause::Term::DSemi:    return ";;";
		case CaseClause::Term::SemiAmp:  return ";&";
		case CaseClause::Term::DSemiAmp: return ";;&";
		}

		return "?";
	}

	static void dumpCaseClause(std::ostream& os, int depth, const CaseClause& clause) {
		dumpWord(os, depth + 1, "subject", clause.subject);
		for (const auto& item : clause.items) {
			indent(os, depth + 1);
			os << "Case-Item term=" << caseTermName(item.term) << "\n";
			writeLine(os, depth + 2, "patterns:");
			for (const auto& pattern : item.patterns) dumpWord(os, depth + 3, "Word", pattern);
			writeLine(os, depth + 2, "body:");
			dumpListBody(os, depth + 3, item.body);
		}

		dumpRedirs(os, depth + 1, clause.redirs);
	}

	static void dumpFunctionDef(std::ostream& os, int depth, const FunctionDef& function) {
		indent(os, depth + 1);
		os << "name " << function.name << "\n";
		writeLine(os, depth + 1, "body:");
		dumpNode(os, depth + 2, *function.body);
	}

	static void dumpDBracketPrimary(std::ostream& os, const DBracketCond::Expr& expr) {
		if (expr.op.empty()) {
			os << "test \"" << expr.lhs.raw << "\"\n";
			return;
		}

		if (expr.rhs.raw.empty()) {
			os << "test " << expr.op << " \"" << expr.lhs.raw << "\"\n";
			return;
		}

		os << "test \"" << expr.lhs.raw << "\" " << expr.op << " \"" << expr.rhs.raw << "\"\n";
	}

	static void dumpDBracketExpr(std::ostream& os, int depth, const DBracketCond::Expr& expr) {
		using K = DBracketCond::Expr::K;
		indent(os, depth);
		switch (expr.k) {
		case K::And:
			os << "&&\n";
			dumpDBracketExpr(os, depth + 1, *expr.a);
			dumpDBracketExpr(os, depth + 1, *expr.b);
			return;
		case K::Or:
			os << "||\n";
			dumpDBracketExpr(os, depth + 1, *expr.a);
			dumpDBracketExpr(os, depth + 1, *expr.b);
			return;
		case K::Not:
			os << "!\n";
			dumpDBracketExpr(os, depth + 1, *expr.a);
			return;
		case K::Prim:
			dumpDBracketPrimary(os, expr);
			return;
		}
	}

	static void dumpDBracket(std::ostream& os, int depth, const DBracketCond& cond) {
		if (cond.root != nullptr) {
			writeLine(os, depth + 1, "expr:");
			dumpDBracketExpr(os, depth + 2, *cond.root);
		}

		dumpRedirs(os, depth + 1, cond.redirs);
	}

	static void dumpArithCommand(std::ostream& os, int depth, const ArithCommand& command) {
		indent(os, depth + 1);
		os << "expr=\"" << command.expr << "\"\n";
		dumpRedirs(os, depth + 1, command.redirs);
	}

	static void dumpCompoundWithRedirs(std::ostream& os, int depth, const Node* body,
	                                   const std::vector<Redirection>& redirs) {
		dumpListBody(os, depth + 1, body);
		dumpRedirs(os, depth + 1, redirs);
	}

	static void dumpNode(std::ostream& os, int depth, const Node& node) {
		writeLine(os, depth, nodeKindName(node.kind));
		switch (node.kind) {
		case Node::Kind::SimpleCommand:
			dumpSimpleCommand(os, depth, static_cast<const SimpleCommand&>(node));
			return;
		case Node::Kind::Pipeline:
			dumpPipeline(os, depth, static_cast<const Pipeline&>(node));
			return;
		case Node::Kind::AndOr:
			dumpAndOr(os, depth, static_cast<const AndOr&>(node));
			return;
		case Node::Kind::List:
			dumpList(os, depth, static_cast<const List&>(node));
			return;
		case Node::Kind::BraceGroup: {
			const auto& group = static_cast<const BraceGroup&>(node);
			dumpCompoundWithRedirs(os, depth, group.body, group.redirs);
			return;
		}
		case Node::Kind::Subshell: {
			const auto& subshell = static_cast<const Subshell&>(node);
			dumpCompoundWithRedirs(os, depth, subshell.body, subshell.redirs);
			return;
		}
		case Node::Kind::IfClause:
			dumpIfClause(os, depth, static_cast<const IfClause&>(node));
			return;
		case Node::Kind::WhileClause:
			dumpWhileClause(os, depth, static_cast<const WhileClause&>(node));
			return;
		case Node::Kind::ForClause:
			dumpForClause(os, depth, static_cast<const ForClause&>(node));
			return;
		case Node::Kind::CaseClause:
			dumpCaseClause(os, depth, static_cast<const CaseClause&>(node));
			return;
		case Node::Kind::FunctionDef:
			dumpFunctionDef(os, depth, static_cast<const FunctionDef&>(node));
			return;
		case Node::Kind::DBracket:
			dumpDBracket(os, depth, static_cast<const DBracketCond&>(node));
			return;
		case Node::Kind::ArithCommand:
			dumpArithCommand(os, depth, static_cast<const ArithCommand&>(node));
			return;
		}
	}

	static void dumpHeredocInfo(std::ostream& os, const Token& token) {
		os << "  (heredoc-delim, quoted=" << (token.heredoc_quoted ? "y" : "n")
			<< ", strip_tabs=" << (token.heredoc_strip_tabs ? "y" : "n")
			<< ", body=\"";
		escapeString(os, token.heredoc_body);
		os << "\")";
	}

	void dumpTokens(std::ostream& os, const std::vector<Token>& tokens) {
		for (const auto& token : tokens) {
			os << token.loc.line << ":" << token.loc.column << "  " << tokKindName(token.kind);
			if (token.kind == TokKind::Word || token.kind == TokKind::IoNumber) {
				os << " text=\"";
				escapeString(os, token.text);
				os << "\" segs=";
				dumpSegments(os, token.segments);
			}

			if (token.is_heredoc_delim) dumpHeredocInfo(os, token);
			os << "\n";
		}
	}

	void dumpAst(std::ostream& os, const Node& node) {
		dumpNode(os, 0, node);
	}

}  // namespace wbsh

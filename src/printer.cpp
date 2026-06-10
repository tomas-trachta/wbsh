/**
 * @file printer.cpp
 * @brief Pretty-printers for the token stream and the AST.
 */

#include "printer.h"

#include <functional>
#include <ostream>
#include <string>

namespace wbsh {

	static void indent(std::ostream& os, int n) {
		for (int i = 0; i < n; ++i) os << "  ";
	}

	static void escapeString(std::ostream& os, const std::string& s) {
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
	}

	static void dumpSegments(std::ostream& os, const std::vector<WordSegment>& segs) {
		os << "[";
		for (std::size_t i = 0; i < segs.size(); ++i) {
			if (i) os << " ";
			const auto& s = segs[i];
			os << segKindName(s.kind);
			if (s.kind == WordSegment::Kind::DoubleQuoted) {
				os << "(";
				dumpSegments(os, s.nested);
				os << ")";
			}
			else {
				os << "(\"";
				escapeString(os, s.text);
				os << "\")";
			}
		}
		os << "]";
	}

	static void dumpWord(std::ostream& os, int depth, const char* label, const Word& w) {
		indent(os, depth);
		os << label << " raw=\"";
		escapeString(os, w.raw);
		os << "\" segs=";
		dumpSegments(os, w.segments);
		os << "\n";
	}

	static void dumpRedir(std::ostream& os, int depth, const Redirection& r) {
		indent(os, depth);
		os << "Redir " << redirOpName(r.op);
		if (r.fd != -1) os << " fd=" << r.fd;
		os << " target=\"";
		escapeString(os, r.target.raw);
		os << "\"";
		if (r.op == RedirOp::DLess || r.op == RedirOp::DLessDash) {
			os << " quoted=" << (r.heredoc_quoted ? "y" : "n");
			os << " body=\"";
			escapeString(os, r.heredoc_body);
			os << "\"";
		}
		os << "\n";
	}

	static void dumpNode(std::ostream& os, int depth, const Node& n);

	static void dumpListBody(std::ostream& os, int depth, const Node* maybe_list) {
		if (!maybe_list) {
			indent(os, depth);
			os << "(empty)\n";
			return;
		}
		dumpNode(os, depth, *maybe_list);
	}

	static void dumpSimpleCommand(std::ostream& os, int depth, const SimpleCommand& sc) {
		for (const auto& a : sc.assignments) {
			indent(os, depth + 1);
			os << "Assign " << a.name << "=";
			dumpSegments(os, a.value.segments);
			os << "\n";
		}
		for (const auto& w : sc.words) dumpWord(os, depth + 1, "Word", w);
		for (const auto& r : sc.redirs) dumpRedir(os, depth + 1, r);
	}

	static void dumpPipeline(std::ostream& os, int depth, const Pipeline& p) {
		if (p.bang) { indent(os, depth + 1); os << "(bang)\n"; }
		for (std::size_t i = 0; i < p.commands.size(); ++i) {
			if (i > 0) {
				indent(os, depth + 1);
				os << (p.stderr_to_stdout[i - 1] ? "|& pipe\n" : "| pipe\n");
			}
			dumpNode(os, depth + 1, *p.commands[i]);
		}
	}

	static void dumpAndOr(std::ostream& os, int depth, const AndOr& ao) {
		indent(os, depth + 1);
		os << (ao.op == AndOr::Op::AndIf ? "&&" : "||") << "\n";
		dumpNode(os, depth + 1, *ao.left);
		dumpNode(os, depth + 1, *ao.right);
	}

	static void dumpList(std::ostream& os, int depth, const List& l) {
		for (const auto& it : l.items) {
			indent(os, depth + 1);
			os << "Item" << (it.background ? " (bg)" : "") << "\n";
			dumpNode(os, depth + 2, *it.command);
		}
	}

	static void dumpIfClause(std::ostream& os, int depth, const IfClause& ic) {
		for (std::size_t i = 0; i < ic.branches.size(); ++i) {
			indent(os, depth + 1);
			os << (i == 0 ? "if-cond\n" : "elif-cond\n");
			dumpListBody(os, depth + 2, ic.branches[i].cond);
			indent(os, depth + 1);
			os << "then\n";
			dumpListBody(os, depth + 2, ic.branches[i].body);
		}
		if (ic.else_body) {
			indent(os, depth + 1); os << "else\n";
			dumpListBody(os, depth + 2, ic.else_body);
		}
		for (const auto& r : ic.redirs) dumpRedir(os, depth + 1, r);
	}

	static void dumpWhileClause(std::ostream& os, int depth, const WhileClause& w) {
		indent(os, depth + 1);
		os << (w.until ? "until-cond\n" : "while-cond\n");
		dumpListBody(os, depth + 2, w.cond);
		indent(os, depth + 1); os << "do\n";
		dumpListBody(os, depth + 2, w.body);
		for (const auto& r : w.redirs) dumpRedir(os, depth + 1, r);
	}

	static void dumpForClause(std::ostream& os, int depth, const ForClause& f) {
		indent(os, depth + 1);
		os << "var " << f.var << "\n";
		if (f.has_in) {
			indent(os, depth + 1); os << "in\n";
			for (const auto& w : f.items) dumpWord(os, depth + 2, "Word", w);
		}
		indent(os, depth + 1); os << "do\n";
		dumpListBody(os, depth + 2, f.body);
		for (const auto& r : f.redirs) dumpRedir(os, depth + 1, r);
	}

	static const char* caseTermName(CaseClause::Term t) {
		switch (t) {
		case CaseClause::Term::DSemi:    return ";;";
		case CaseClause::Term::SemiAmp:  return ";&";
		case CaseClause::Term::DSemiAmp: return ";;&";
		}
		return "?";
	}

	static void dumpCaseClause(std::ostream& os, int depth, const CaseClause& c) {
		dumpWord(os, depth + 1, "subject", c.subject);
		for (const auto& it : c.items) {
			indent(os, depth + 1);
			os << "Case-Item term=" << caseTermName(it.term) << "\n";
			indent(os, depth + 2); os << "patterns:\n";
			for (const auto& p : it.patterns) dumpWord(os, depth + 3, "Word", p);
			indent(os, depth + 2); os << "body:\n";
			dumpListBody(os, depth + 3, it.body);
		}
		for (const auto& r : c.redirs) dumpRedir(os, depth + 1, r);
	}

	static void dumpFunctionDef(std::ostream& os, int depth, const FunctionDef& f) {
		indent(os, depth + 1); os << "name " << f.name << "\n";
		indent(os, depth + 1); os << "body:\n";
		dumpNode(os, depth + 2, *f.body);
	}

	static void dumpDBracketExpr(std::ostream& os, int depth,
	                             const DBracketCond::Expr& e) {
		using K = DBracketCond::Expr::K;
		indent(os, depth);
		switch (e.k) {
		case K::And:
			os << "&&\n";
			dumpDBracketExpr(os, depth + 1, *e.a);
			dumpDBracketExpr(os, depth + 1, *e.b);
			return;
		case K::Or:
			os << "||\n";
			dumpDBracketExpr(os, depth + 1, *e.a);
			dumpDBracketExpr(os, depth + 1, *e.b);
			return;
		case K::Not:
			os << "!\n";
			dumpDBracketExpr(os, depth + 1, *e.a);
			return;
		case K::Prim:
			if (e.op.empty()) {
				os << "test \"" << e.lhs.raw << "\"\n";
			} else if (e.rhs.raw.empty()) {
				os << "test " << e.op << " \"" << e.lhs.raw << "\"\n";
			} else {
				os << "test \"" << e.lhs.raw << "\" "
				   << e.op << " \"" << e.rhs.raw << "\"\n";
			}
			return;
		}
	}

	static void dumpDBracket(std::ostream& os, int depth, const DBracketCond& d) {
		if (d.root) {
			indent(os, depth + 1); os << "expr:\n";
			dumpDBracketExpr(os, depth + 2, *d.root);
		}
		for (const auto& r : d.redirs) dumpRedir(os, depth + 1, r);
	}

	static void dumpCompoundWithRedirs(std::ostream& os, int depth,
	                                   const Node* body,
	                                   const std::vector<Redirection>& redirs) {
		dumpListBody(os, depth + 1, body);
		for (const auto& r : redirs) dumpRedir(os, depth + 1, r);
	}

	static void dumpNode(std::ostream& os, int depth, const Node& n) {
		indent(os, depth);
		os << nodeKindName(n.kind) << "\n";
		switch (n.kind) {
		case Node::Kind::SimpleCommand:
			dumpSimpleCommand(os, depth, static_cast<const SimpleCommand&>(n));
			return;
		case Node::Kind::Pipeline:
			dumpPipeline(os, depth, static_cast<const Pipeline&>(n));
			return;
		case Node::Kind::AndOr:
			dumpAndOr(os, depth, static_cast<const AndOr&>(n));
			return;
		case Node::Kind::List:
			dumpList(os, depth, static_cast<const List&>(n));
			return;
		case Node::Kind::BraceGroup: {
			const auto& bg = static_cast<const BraceGroup&>(n);
			dumpCompoundWithRedirs(os, depth, bg.body, bg.redirs);
			return;
		}
		case Node::Kind::Subshell: {
			const auto& ss = static_cast<const Subshell&>(n);
			dumpCompoundWithRedirs(os, depth, ss.body, ss.redirs);
			return;
		}
		case Node::Kind::IfClause:
			dumpIfClause(os, depth, static_cast<const IfClause&>(n));
			return;
		case Node::Kind::WhileClause:
			dumpWhileClause(os, depth, static_cast<const WhileClause&>(n));
			return;
		case Node::Kind::ForClause:
			dumpForClause(os, depth, static_cast<const ForClause&>(n));
			return;
		case Node::Kind::CaseClause:
			dumpCaseClause(os, depth, static_cast<const CaseClause&>(n));
			return;
		case Node::Kind::FunctionDef:
			dumpFunctionDef(os, depth, static_cast<const FunctionDef&>(n));
			return;
		case Node::Kind::DBracket:
			dumpDBracket(os, depth, static_cast<const DBracketCond&>(n));
			return;
		}
	}

	void dumpTokens(std::ostream& os, const std::vector<Token>& tokens) {
		for (const auto& t : tokens) {
			os << t.loc.line << ":" << t.loc.column << "  "
				<< tokKindName(t.kind);
			if (t.kind == TokKind::Word || t.kind == TokKind::IoNumber) {
				os << " text=\"";
				escapeString(os, t.text);
				os << "\" segs=";
				dumpSegments(os, t.segments);
			}
			if (t.is_heredoc_delim) {
				os << "  (heredoc-delim, quoted=" << (t.heredoc_quoted ? "y" : "n")
					<< ", strip_tabs=" << (t.heredoc_strip_tabs ? "y" : "n")
					<< ", body=\"";
				escapeString(os, t.heredoc_body);
				os << "\")";
			}
			os << "\n";
		}
	}

	void dumpAst(std::ostream& os, const Node& node) {
		dumpNode(os, 0, node);
	}

}  // namespace wbsh

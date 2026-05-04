#include "printer.h"

#include <functional>
#include <ostream>
#include <string>

namespace wbsh {

	namespace {

		void indent(std::ostream& os, int n) {
			for (int i = 0; i < n; ++i) os << "  ";
		}

		void escapeString(std::ostream& os, const std::string& s) {
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

		void dumpSegments(std::ostream& os, const std::vector<WordSegment>& segs) {
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

		void dumpWord(std::ostream& os, int depth, const char* label, const Word& w) {
			indent(os, depth);
			os << label << " raw=\"";
			escapeString(os, w.raw);
			os << "\" segs=";
			dumpSegments(os, w.segments);
			os << "\n";
		}

		void dumpRedir(std::ostream& os, int depth, const Redirection& r) {
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

		void dumpNode(std::ostream& os, int depth, const Node& n);

		void dumpListBody(std::ostream& os, int depth, const Node* maybe_list) {
			if (!maybe_list) {
				indent(os, depth);
				os << "(empty)\n";
				return;
			}
			dumpNode(os, depth, *maybe_list);
		}

		void dumpNode(std::ostream& os, int depth, const Node& n) {
			indent(os, depth);
			os << nodeKindName(n.kind) << "\n";
			switch (n.kind) {
			case Node::Kind::SimpleCommand: {
				const auto& sc = static_cast<const SimpleCommand&>(n);
				for (const auto& a : sc.assignments) {
					indent(os, depth + 1);
					os << "Assign " << a.name << "=";
					dumpSegments(os, a.value.segments);
					os << "\n";
				}
				for (const auto& w : sc.words) dumpWord(os, depth + 1, "Word", w);
				for (const auto& r : sc.redirs) dumpRedir(os, depth + 1, r);
				break;
			}
			case Node::Kind::Pipeline: {
				const auto& p = static_cast<const Pipeline&>(n);
				if (p.bang) { indent(os, depth + 1); os << "(bang)\n"; }
				for (std::size_t i = 0; i < p.commands.size(); ++i) {
					if (i > 0) {
						indent(os, depth + 1);
						os << (p.stderr_to_stdout[i - 1] ? "|& pipe\n" : "| pipe\n");
					}
					dumpNode(os, depth + 1, *p.commands[i]);
				}
				break;
			}
			case Node::Kind::AndOr: {
				const auto& ao = static_cast<const AndOr&>(n);
				indent(os, depth + 1);
				os << (ao.op == AndOr::Op::AndIf ? "&&" : "||") << "\n";
				dumpNode(os, depth + 1, *ao.left);
				dumpNode(os, depth + 1, *ao.right);
				break;
			}
			case Node::Kind::List: {
				const auto& l = static_cast<const List&>(n);
				for (const auto& it : l.items) {
					indent(os, depth + 1);
					os << "Item" << (it.background ? " (bg)" : "") << "\n";
					dumpNode(os, depth + 2, *it.command);
				}
				break;
			}
			case Node::Kind::BraceGroup: {
				const auto& bg = static_cast<const BraceGroup&>(n);
				dumpListBody(os, depth + 1, bg.body.get());
				for (const auto& r : bg.redirs) dumpRedir(os, depth + 1, r);
				break;
			}
			case Node::Kind::Subshell: {
				const auto& ss = static_cast<const Subshell&>(n);
				dumpListBody(os, depth + 1, ss.body.get());
				for (const auto& r : ss.redirs) dumpRedir(os, depth + 1, r);
				break;
			}
			case Node::Kind::IfClause: {
				const auto& ic = static_cast<const IfClause&>(n);
				for (std::size_t i = 0; i < ic.branches.size(); ++i) {
					indent(os, depth + 1);
					os << (i == 0 ? "if-cond\n" : "elif-cond\n");
					dumpListBody(os, depth + 2, ic.branches[i].cond.get());
					indent(os, depth + 1);
					os << "then\n";
					dumpListBody(os, depth + 2, ic.branches[i].body.get());
				}
				if (ic.else_body) {
					indent(os, depth + 1); os << "else\n";
					dumpListBody(os, depth + 2, ic.else_body.get());
				}
				for (const auto& r : ic.redirs) dumpRedir(os, depth + 1, r);
				break;
			}
			case Node::Kind::WhileClause: {
				const auto& w = static_cast<const WhileClause&>(n);
				indent(os, depth + 1);
				os << (w.until ? "until-cond\n" : "while-cond\n");
				dumpListBody(os, depth + 2, w.cond.get());
				indent(os, depth + 1); os << "do\n";
				dumpListBody(os, depth + 2, w.body.get());
				for (const auto& r : w.redirs) dumpRedir(os, depth + 1, r);
				break;
			}
			case Node::Kind::ForClause: {
				const auto& f = static_cast<const ForClause&>(n);
				indent(os, depth + 1);
				os << "var " << f.var << "\n";
				if (f.has_in) {
					indent(os, depth + 1); os << "in\n";
					for (const auto& w : f.items) dumpWord(os, depth + 2, "Word", w);
				}
				indent(os, depth + 1); os << "do\n";
				dumpListBody(os, depth + 2, f.body.get());
				for (const auto& r : f.redirs) dumpRedir(os, depth + 1, r);
				break;
			}
			case Node::Kind::CaseClause: {
				const auto& c = static_cast<const CaseClause&>(n);
				dumpWord(os, depth + 1, "subject", c.subject);
				for (const auto& it : c.items) {
					indent(os, depth + 1);
					os << "Case-Item term=";
					switch (it.term) {
					case CaseClause::Term::DSemi:    os << ";;"; break;
					case CaseClause::Term::SemiAmp:  os << ";&"; break;
					case CaseClause::Term::DSemiAmp: os << ";;&"; break;
					}
					os << "\n";
					indent(os, depth + 2); os << "patterns:\n";
					for (const auto& p : it.patterns) dumpWord(os, depth + 3, "Word", p);
					indent(os, depth + 2); os << "body:\n";
					dumpListBody(os, depth + 3, it.body.get());
				}
				for (const auto& r : c.redirs) dumpRedir(os, depth + 1, r);
				break;
			}
			case Node::Kind::FunctionDef: {
				const auto& f = static_cast<const FunctionDef&>(n);
				indent(os, depth + 1); os << "name " << f.name << "\n";
				indent(os, depth + 1); os << "body:\n";
				dumpNode(os, depth + 2, *f.body);
				break;
			}
			case Node::Kind::DBracket: {
				const auto& d = static_cast<const DBracketCond&>(n);
				if (d.root) {
					indent(os, depth + 1); os << "expr:\n";
					std::function<void(const DBracketCond::Expr&, int)> dump =
						[&](const DBracketCond::Expr& e, int dd) {
						using K = DBracketCond::Expr::K;
						indent(os, dd);
						switch (e.k) {
						case K::And: os << "&&\n"; dump(*e.a, dd + 1); dump(*e.b, dd + 1); break;
						case K::Or:  os << "||\n"; dump(*e.a, dd + 1); dump(*e.b, dd + 1); break;
						case K::Not: os << "!\n";  dump(*e.a, dd + 1); break;
						case K::Prim:
							if (e.op.empty()) os << "test \"" << e.lhs.raw << "\"\n";
							else if (e.rhs.raw.empty())
								os << "test " << e.op << " \"" << e.lhs.raw << "\"\n";
							else
								os << "test \"" << e.lhs.raw << "\" "
								   << e.op << " \"" << e.rhs.raw << "\"\n";
							break;
						}
					};
					dump(*d.root, depth + 2);
				}
				for (const auto& r : d.redirs) dumpRedir(os, depth + 1, r);
				break;
			}
			}
		}

	}  // namespace

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

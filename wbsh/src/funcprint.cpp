/**
 * @file funcprint.cpp
 * @brief A function definition as `declare -f` and `type` print it: bash's
 *        canonical layout, one command per line, four spaces per level.
 */

#include "funcprint.h"

#include <string>
#include <vector>

namespace wbsh {

	static const std::size_t kIndentWidth = 4;

	static std::string indentOf(int level) {
		return std::string(static_cast<std::size_t>(level) * kIndentWidth, ' ');
	}

	static std::string sourceSlice(const Node& node) {
		if (node.source_text == nullptr || node.src_end <= node.src_start) return std::string();
		return node.source_text->substr(node.src_start, node.src_end - node.src_start);
	}

	static std::string trimmed(const std::string& text) {
		const std::size_t first = text.find_first_not_of(" \t\n");
		if (first == std::string::npos) return std::string();

		const std::size_t last = text.find_last_not_of(" \t\n");
		return text.substr(first, last - first + 1);
	}

	static int defaultRedirFd(RedirOp op) {
		switch (op) {
		case RedirOp::Less:
		case RedirOp::LessAnd:
		case RedirOp::LessGreat:
		case RedirOp::DLess:
		case RedirOp::DLessDash:
		case RedirOp::TLess:
			return 0;
		case RedirOp::AmpGreat:
		case RedirOp::AmpDGreat:
			return -1;
		default:
			return 1;
		}
	}

	static bool isHeredoc(RedirOp op) {
		return op == RedirOp::DLess || op == RedirOp::DLessDash;
	}

	static std::string redirFdText(const Redirection& redir) {
		if (!redir.fd_variable.empty()) return "{" + redir.fd_variable + "}";
		if (redir.fd < 0 || redir.fd == defaultRedirFd(redir.op)) return std::string();
		return std::to_string(redir.fd);
	}

	static std::string redirText(const Redirection& redir) {
		const bool joined = redir.op == RedirOp::LessAnd || redir.op == RedirOp::GreatAnd
			|| isHeredoc(redir.op);
		return redirFdText(redir) + redirOpName(redir.op) + (joined ? "" : " ") + redir.target.raw;
	}

	static std::string unquotedDelimiter(const std::string& raw) {
		std::string delimiter;
		for (const char c : raw) {
			if (c != '\'' && c != '"' && c != '\\') delimiter.push_back(c);
		}

		return delimiter;
	}

	static void skipQuotedChar(const std::string& raw, std::size_t& i, char& quote) {
		if (raw[i] == '\\' && quote != '\'') ++i;
		else if (raw[i] == quote) quote = 0;
	}

	static std::size_t commandSubstClose(const std::string& raw, std::size_t open) {
		int depth = 1;
		char quote = 0;
		for (std::size_t i = open + 1; i < raw.size(); ++i) {
			const char c = raw[i];
			if (quote != 0 || c == '\\') {
				skipQuotedChar(raw, i, quote);
				continue;
			}

			if (c == '\'' || c == '"') quote = c;
			else if (c == '(') ++depth;
			else if (c == ')' && --depth == 0) return i;
		}

		return std::string::npos;
	}

	// bash prints the command it parsed out of a one-line `$( ... )`,
	// which loses the blanks just inside the parentheses.
	static bool appendReprintedSubst(const std::string& raw, std::size_t& i, std::string& out) {
		if (raw.compare(i, 2, "$(") != 0 || raw.compare(i, 3, "$((") == 0) return false;

		const std::size_t close = commandSubstClose(raw, i + 1);
		if (close == std::string::npos) return false;

		const std::string body = trimmed(raw.substr(i + 2, close - i - 2));
		if (body.empty() || body.front() == '(' || body.find('\n') != std::string::npos) return false;

		out += "$(" + body + ")";
		i = close + 1;
		return true;
	}

	static void copyQuotedRun(const std::string& raw, std::size_t& i, std::string& out) {
		const std::size_t end = raw[i] == '\\' ? i + 2 : raw.find('\'', i + 1) + 1;
		const std::size_t stop = (end == 0 || end > raw.size()) ? raw.size() : end;
		out.append(raw, i, stop - i);
		i = stop;
	}

	static std::string printedWord(const Word& word) {
		const std::string& raw = word.raw;
		if (raw.find("$(") == std::string::npos) return raw;

		std::string out;
		bool in_dq = false;
		std::size_t i = 0;
		while (i < raw.size()) {
			if (raw[i] == '\\' || (raw[i] == '\'' && !in_dq)) {
				copyQuotedRun(raw, i, out);
				continue;
			}

			if (appendReprintedSubst(raw, i, out)) continue;
			if (raw[i] == '"') in_dq = !in_dq;
			out.push_back(raw[i++]);
		}

		return out;
	}

	static std::string joinedWords(const std::vector<Word>& words, const char* separator) {
		std::string text;
		for (const Word& word : words) {
			if (!text.empty()) text += separator;
			text += printedWord(word);
		}

		return text;
	}

	static std::string arrayItemsText(const Assignment& assignment) {
		if (assignment.keyed_items.empty()) return joinedWords(assignment.array_items, " ");

		std::string text;
		for (const Assignment::Keyed& item : assignment.keyed_items) {
			if (!text.empty()) text.push_back(' ');
			if (item.has_key) text += "[" + item.key.raw + (item.append ? "]+=" : "]=");
			text += item.value.raw;
		}

		return text;
	}

	static std::string assignmentText(const Assignment& assignment) {
		std::string text = assignment.name;
		if (assignment.has_subscript) text += "[" + assignment.subscript.raw + "]";
		text += assignment.append ? "+=" : "=";
		if (assignment.is_array) return text + "(" + arrayItemsText(assignment) + ")";
		return text + printedWord(assignment.value);
	}

	static std::string declarationWordText(const SimpleCommand& command, std::size_t index) {
		for (const DeclarationArg& arg : command.declaration_args) {
			if (arg.word_index == index && !arg.compound_text.empty()) return arg.compound_text;
		}

		return printedWord(command.words[index]);
	}

	namespace funcprint_detail {

		// Walks a function body the way bash's print_cmd.c lays it out.
		// Here-document bodies wait until the line with their operator ends.
		class FunctionPrinter {
		public:
			std::string print(const FunctionDef& def) {
				out_ = def.name + " () \n";
				printBodyAsGroup(def, 0);
				return out_;
			}

		private:
			// A body that is not a group, `f() ( ... )`, is shown inside one.
			void printBodyAsGroup(const FunctionDef& def, int level) {
				const Node* body = def.body;
				if (body != nullptr && body->kind == Node::Kind::BraceGroup) {
					printGroup(static_cast<const BraceGroup&>(*body), level);
				} else {
					out_ += "{ \n" + indentOf(level + 1);
					if (body != nullptr) printNode(*body, level + 1);
					out_ += "\n" + indentOf(level) + "}";
				}

				appendRedirs(def.redirs);
			}

			// @p terminate_last puts `;` after the last command too, as bash
			// does in the bodies of `if` and the loops.
			void printList(const Node* body, int level, bool terminate_last) {
				if (body == nullptr) return;
				if (body->kind != Node::Kind::List) {
					printNode(*body, level);
					finishItem(level, true, terminate_last);
					return;
				}

				const std::vector<ListItem>& items = static_cast<const List*>(body)->items;
				for (std::size_t i = 0; i < items.size(); ++i) {
					const bool last = i + 1 == items.size();
					printNode(*items[i].command, level);
					if (items[i].background) {
						out_ += last ? " &" : " & ";
						continue;
					}

					finishItem(level, last, terminate_last);
				}
			}

			void finishItem(int level, bool last, bool terminate_last) {
				if (!heredocs_.empty()) {
					flushHeredocs();
					if (!last) out_ += "\n" + indentOf(level);
					return;
				}

				if (!last) out_ += ";\n" + indentOf(level);
				else if (terminate_last) out_ += ";";
			}

			void flushHeredocs() {
				for (const Redirection* redir : heredocs_) {
					out_ += "\n" + redir->heredoc_body + unquotedDelimiter(redir->target.raw) + "\n";
				}

				heredocs_.clear();
			}

			void appendRedirs(const std::vector<Redirection>& redirs) {
				for (const Redirection& redir : redirs) {
					out_ += " " + redirText(redir);
					if (isHeredoc(redir.op)) heredocs_.push_back(&redir);
				}
			}

			void printNode(const Node& node, int level) {
				switch (node.kind) {
					case Node::Kind::SimpleCommand:
						printSimple(static_cast<const SimpleCommand&>(node));
						return;
					case Node::Kind::Pipeline:
						printPipeline(static_cast<const Pipeline&>(node), level);
						return;
					case Node::Kind::AndOr:
						printAndOr(static_cast<const AndOr&>(node), level);
						return;
					case Node::Kind::List:
						printList(&node, level, false);
						return;
					default:
						printCompound(node, level);
						return;
				}
			}

			void printCompound(const Node& node, int level) {
				switch (node.kind) {
					case Node::Kind::BraceGroup:
						printGroup(static_cast<const BraceGroup&>(node), level);
						return;
					case Node::Kind::Subshell:
						printSubshell(static_cast<const Subshell&>(node), level);
						return;
					case Node::Kind::IfClause:
						printIf(static_cast<const IfClause&>(node), 0, level);
						appendRedirs(static_cast<const IfClause&>(node).redirs);
						return;
					case Node::Kind::WhileClause:
						printWhile(static_cast<const WhileClause&>(node), level);
						return;
					default:
						printOtherCompound(node, level);
						return;
				}
			}

			void printOtherCompound(const Node& node, int level) {
				switch (node.kind) {
					case Node::Kind::ForClause:
						printFor(static_cast<const ForClause&>(node), level);
						return;
					case Node::Kind::CaseClause:
						printCase(static_cast<const CaseClause&>(node), level);
						return;
					case Node::Kind::ArithCommand:
						out_ += "(( " + trimmed(static_cast<const ArithCommand&>(node).expr) + " ))";
						appendRedirs(static_cast<const ArithCommand&>(node).redirs);
						return;
					case Node::Kind::FunctionDef:
						printNestedFunction(static_cast<const FunctionDef&>(node), level);
						return;
					default:
						out_ += sourceSlice(node);
						return;
				}
			}

			void printSimple(const SimpleCommand& command) {
				std::string text;
				for (const Assignment& assignment : command.assignments) {
					if (!text.empty()) text.push_back(' ');
					text += assignmentText(assignment);
				}

				for (std::size_t i = 0; i < command.words.size(); ++i) {
					if (!text.empty()) text.push_back(' ');
					text += declarationWordText(command, i);
				}

				out_ += text;
				appendRedirs(command.redirs);
			}

			void printPipeline(const Pipeline& pipeline, int level) {
				if (pipeline.timed) out_ += pipeline.time_posix ? "time -p " : "time ";
				if (pipeline.bang) out_ += "! ";

				for (std::size_t i = 0; i < pipeline.commands.size(); ++i) {
					if (i > 0) {
						const bool both = i - 1 < pipeline.stderr_to_stdout.size()
							&& pipeline.stderr_to_stdout[i - 1];
						out_ += both ? " |& " : " | ";
					}

					printNode(*pipeline.commands[i], level);
				}
			}

			void printAndOr(const AndOr& and_or, int level) {
				printNode(*and_or.left, level);
				out_ += and_or.op == AndOr::Op::AndIf ? " && " : " || ";
				printNode(*and_or.right, level);
			}

			void printGroup(const BraceGroup& group, int level) {
				out_ += "{ \n" + indentOf(level + 1);
				printList(group.body, level + 1, false);
				out_ += "\n" + indentOf(level) + "}";
				appendRedirs(group.redirs);
			}

			void printSubshell(const Subshell& subshell, int level) {
				out_ += "( ";
				printList(subshell.body, level, false);
				out_ += " )";
				appendRedirs(subshell.redirs);
			}

			// bash prints `elif` as an `if` nested in the `else`.
			void printIf(const IfClause& clause, std::size_t branch, int level) {
				out_ += "if ";
				printList(clause.branches[branch].cond, level, false);
				out_ += "; then\n" + indentOf(level + 1);
				printList(clause.branches[branch].body, level + 1, true);
				out_ += "\n" + indentOf(level);

				if (branch + 1 < clause.branches.size()) {
					out_ += "else\n" + indentOf(level + 1);
					printIf(clause, branch + 1, level + 1);
					out_ += ";\n" + indentOf(level);
				} else if (clause.else_body != nullptr) {
					out_ += "else\n" + indentOf(level + 1);
					printList(clause.else_body, level + 1, true);
					out_ += "\n" + indentOf(level);
				}

				out_ += "fi";
			}

			void printLoopBody(const Node* body, int level) {
				out_ += "do\n" + indentOf(level + 1);
				printList(body, level + 1, true);
				out_ += "\n" + indentOf(level) + "done";
			}

			void printWhile(const WhileClause& clause, int level) {
				out_ += clause.until ? "until " : "while ";
				printList(clause.cond, level, false);
				out_ += "; ";
				printLoopBody(clause.body, level);
				appendRedirs(clause.redirs);
			}

			void printFor(const ForClause& clause, int level) {
				if (clause.is_arith) {
					out_ += "for ((" + trimmed(clause.arith_init) + "; " + trimmed(clause.arith_cond)
						+ "; " + trimmed(clause.arith_update) + "))\n";
				} else {
					const std::string items = clause.has_in ? joinedWords(clause.items, " ") : "\"$@\"";
					out_ += (clause.is_select ? "select " : "for ") + clause.var + " in " + items + ";\n";
				}

				out_ += indentOf(level);
				printLoopBody(clause.body, level);
				appendRedirs(clause.redirs);
			}

			static const char* caseTerminator(CaseClause::Term term) {
				switch (term) {
					case CaseClause::Term::SemiAmp:  return ";&";
					case CaseClause::Term::DSemiAmp: return ";;&";
					default:                         return ";;";
				}
			}

			void printCase(const CaseClause& clause, int level) {
				out_ += "case " + clause.subject.raw + " in \n";
				for (const CaseClause::Item& item : clause.items) {
					out_ += indentOf(level + 1) + joinedWords(item.patterns, " | ") + ")\n";
					if (item.body != nullptr) {
						out_ += indentOf(level + 2);
						printList(item.body, level + 2, false);
					}

					out_ += "\n" + indentOf(level + 1) + caseTerminator(item.term) + "\n";
				}

				out_ += indentOf(level) + "esac";
				appendRedirs(clause.redirs);
			}

			void printNestedFunction(const FunctionDef& def, int level) {
				out_ += "function " + def.name + " () \n" + indentOf(level);
				printBodyAsGroup(def, level);
			}

			std::string out_;
			std::vector<const Redirection*> heredocs_;
		};

	}  // namespace funcprint_detail

	std::string functionDefinitionText(const FunctionDef& def) {
		funcprint_detail::FunctionPrinter printer;
		return printer.print(def);
	}

}  // namespace wbsh

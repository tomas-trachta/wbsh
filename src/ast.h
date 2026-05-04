#pragma once

#include "lexer.h"

#include <memory>
#include <string>
#include <vector>

namespace wbsh {

	// ----- Words & redirections -------------------------------------------------

	struct Word {
		std::vector<WordSegment> segments;
		std::string raw;             // original spelling (for diagnostics / debug)
		SourceLoc loc;
	};

	enum class RedirOp {
		Less,        // <
		Great,       // >
		DGreat,      // >>
		LessAnd,     // <&
		GreatAnd,    // >&
		LessGreat,   // <>
		Clobber,     // >|
		AmpGreat,    // &>
		AmpDGreat,   // &>>
		DLess,       // <<   heredoc
		DLessDash,   // <<-  heredoc with leading-tab strip
		TLess,       // <<<  here-string
	};

	const char* redirOpName(RedirOp o);

	struct Redirection {
		RedirOp op = RedirOp::Less;
		int fd = -1;                 // fd specified before the operator; -1 if default
		Word target;                 // operand word (delimiter for heredocs)
		std::string heredoc_body;    // populated by lexer for DLess / DLessDash
		bool heredoc_quoted = false; // delimiter was quoted -> no expansion at run time
	};

	struct Assignment {
		std::string name;
		Word value;                  // may be empty (foo=)
		// Indexed-element form: foo[2]=val. has_subscript false means scalar.
		bool has_subscript = false;
		Word subscript;              // expanded at exec time
		// Array-literal form: foo=(a b c) or foo=([2]=x [4]=y).
		bool is_array = false;
		std::vector<Word> array_items;     // unkeyed items for [n]=
		// Keyed items capture optional `[key]=` prefixes from the literal,
		// preserving order and letting the executor decide whether they're
		// indexed arithmetic subscripts or assoc-array string keys.
		struct Keyed {
			bool has_key = false;
			Word key;     // arbitrary word; expanded at exec time
			Word value;
		};
		std::vector<Keyed> keyed_items;
		SourceLoc loc;
	};

	// ----- AST ------------------------------------------------------------------

	struct Node {
		enum class Kind {
			SimpleCommand,
			Pipeline,
			AndOr,
			List,
			BraceGroup,
			Subshell,
			IfClause,
			WhileClause,
			ForClause,
			CaseClause,
			FunctionDef,
			DBracket,
		};
		Kind kind;
		SourceLoc loc;
		// Byte range in the original source [src_start, src_end). Used by the
		// pipeline executor to slice a node's text for self-spawn.
		std::size_t src_start = 0;
		std::size_t src_end   = 0;
		// Reference to the source string this node was parsed from. Multiple
		// ASTs can coexist (main script, inherited functions, sourced files);
		// each node carries its own source so slice extraction always uses
		// the right text.
		std::shared_ptr<const std::string> source_text;
		explicit Node(Kind k) : kind(k) {}
		virtual ~Node() = default;
	};
	using NodePtr = std::unique_ptr<Node>;

	const char* nodeKindName(Node::Kind k);

	struct SimpleCommand : Node {
		SimpleCommand() : Node(Kind::SimpleCommand) {}
		std::vector<Assignment> assignments;
		std::vector<Word> words;
		std::vector<Redirection> redirs;
	};

	struct Pipeline : Node {
		Pipeline() : Node(Kind::Pipeline) {}
		bool bang = false;
		// `time` reserved-word prefix: when set, the executor wraps the
		// whole pipeline in a steady_clock measurement and prints the
		// elapsed real time on stderr after it finishes.
		bool timed = false;
		std::vector<NodePtr> commands;
		// Per-pipe flag. stderr_to_stdout[i] applies to the pipe between
		// commands[i] and commands[i+1] (true for `|&`).
		std::vector<bool> stderr_to_stdout;
	};

	struct AndOr : Node {
		enum class Op { AndIf, OrIf };
		AndOr() : Node(Kind::AndOr) {}
		NodePtr left;
		NodePtr right;
		Op op = Op::AndIf;
	};

	struct ListItem {
		NodePtr command;        // Pipeline, AndOr, or compound command directly
		bool background = false;
	};

	struct List : Node {
		List() : Node(Kind::List) {}
		std::vector<ListItem> items;
	};

	struct BraceGroup : Node {
		BraceGroup() : Node(Kind::BraceGroup) {}
		NodePtr body;           // List
		std::vector<Redirection> redirs;
	};

	struct Subshell : Node {
		Subshell() : Node(Kind::Subshell) {}
		NodePtr body;           // List
		std::vector<Redirection> redirs;
	};

	struct IfClause : Node {
		IfClause() : Node(Kind::IfClause) {}
		struct Branch { NodePtr cond; NodePtr body; };
		std::vector<Branch> branches;     // [0] = if, [1..] = elif
		NodePtr else_body;                // optional
		std::vector<Redirection> redirs;
	};

	struct WhileClause : Node {
		WhileClause() : Node(Kind::WhileClause) {}
		bool until = false;
		NodePtr cond;
		NodePtr body;
		std::vector<Redirection> redirs;
	};

	struct ForClause : Node {
		ForClause() : Node(Kind::ForClause) {}
		std::string var;
		bool has_in = false;          // false => iterates "$@"
		std::vector<Word> items;      // word list after `in`
		NodePtr body;
		std::vector<Redirection> redirs;
	};

	struct CaseClause : Node {
		enum class Term { DSemi, SemiAmp, DSemiAmp };
		CaseClause() : Node(Kind::CaseClause) {}
		Word subject;
		struct Item {
			std::vector<Word> patterns;
			NodePtr body;             // List or null for empty body
			Term term = Term::DSemi;
		};
		std::vector<Item> items;
		std::vector<Redirection> redirs;
	};

	// `[[ ... ]]` — modern bash conditional expression. Evaluated without
	// word splitting or pathname expansion on its operand words.
	struct DBracketCond : Node {
		DBracketCond() : Node(Kind::DBracket) {}
		struct Expr {
			enum class K { And, Or, Not, Prim };
			K k = K::Prim;
			std::unique_ptr<Expr> a;     // And/Or: left;  Not: only operand
			std::unique_ptr<Expr> b;     // And/Or: right
			// For Prim:
			//   op==""  ->  truthiness of lhs (non-empty string is true)
			//   op like "-f","-d","-z",...  ->  unary
			//   op like "==","!=","=","<",">","=~","-eq",...  ->  binary
			std::string op;
			Word lhs;
			Word rhs;
		};
		std::unique_ptr<Expr> root;
		std::vector<Redirection> redirs;
	};

	struct FunctionDef : Node {
		FunctionDef() : Node(Kind::FunctionDef) {}
		std::string name;
		NodePtr body;                 // typically BraceGroup or Subshell
		// Pre-sliced source text for the body. Captured at parse time so that
		// the executor can serialise functions for self-spawned subshells
		// (and so we keep working even if the original source string is
		// later moved or freed).
		std::string body_text;
		std::vector<Redirection> redirs;
	};

}  // namespace wbsh

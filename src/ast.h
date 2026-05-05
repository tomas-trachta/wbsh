#pragma once

/**
 * @file ast.h
 * @brief Abstract Syntax Tree node definitions for the wbsh shell.
 *
 * Defines Word, Redirection, Assignment, and the Node hierarchy used
 * by the parser and executor. Each Node carries its location and the
 * shared-ownership source string it was parsed from, so the executor
 * can slice the original text for self-spawned subshells and for
 * `declare -f` output.
 *
 * The Node hierarchy uses tagged unions (Node::Kind) rather than RTTI
 * because the executor dispatches with a switch on `kind`.
 */

#include "lexer.h"

#include <memory>
#include <string>
#include <vector>

namespace wbsh {

	// ----- Words & redirections -------------------------------------------------

	/**
	 * @brief A shell WORD: a sequence of segments with location info.
	 *
	 * `raw` is the original spelling, kept for diagnostics. The
	 * actual semantics live in `segments`; the expander walks those
	 * to produce the final string(s).
	 */
	struct Word {
		std::vector<WordSegment> segments;  ///< Structured segments produced by the lexer.
		std::string raw;                    ///< Original spelling (diagnostics / debug).
		SourceLoc loc;                      ///< Position in the source.
	};

	/// Redirection operator kind.
	enum class RedirOp {
		Less,        ///< `<`
		Great,       ///< `>`
		DGreat,      ///< `>>`
		LessAnd,     ///< `<&`
		GreatAnd,    ///< `>&`
		LessGreat,   ///< `<>`
		Clobber,     ///< `>|`
		AmpGreat,    ///< `&>`
		AmpDGreat,   ///< `&>>`
		DLess,       ///< `<<` heredoc.
		DLessDash,   ///< `<<-` heredoc with leading-tab strip.
		TLess,       ///< `<<<` here-string.
	};

	/// Human-readable operator spelling for a RedirOp.
	const char* redirOpName(RedirOp o);

	/**
	 * @brief One redirection attached to a command or compound command.
	 *
	 * For heredocs the `target` word is the delimiter; the actual
	 * body is captured by the lexer into `heredoc_body`, with
	 * `heredoc_quoted` recording whether expansion should run at
	 * execution time.
	 */
	struct Redirection {
		RedirOp op = RedirOp::Less;     ///< Operator.
		int fd = -1;                    ///< fd before the operator; -1 for default.
		Word target;                    ///< Operand word (delimiter for heredocs).
		std::string heredoc_body;       ///< Filled by the lexer for `<<` / `<<-`.
		bool heredoc_quoted = false;    ///< Delimiter was quoted -> body kept verbatim.
	};

	/**
	 * @brief A `name=value` assignment in a SimpleCommand prefix.
	 *
	 * Supports scalar, indexed-element (`foo[2]=val`), and array
	 * literal (`foo=(a b c)` or `foo=([2]=x)`) forms.
	 */
	struct Assignment {
		std::string name;               ///< Variable name.
		Word value;                     ///< Scalar value; may be empty (`foo=`).
		// Indexed-element form: foo[2]=val. has_subscript false means scalar.
		bool has_subscript = false;     ///< True when written as `name[expr]=value`.
		Word subscript;                 ///< Subscript text; expanded at exec time.
		// Array-literal form: foo=(a b c) or foo=([2]=x [4]=y).
		bool is_array = false;          ///< True for `name=(...)` form.
		std::vector<Word> array_items;  ///< Unkeyed array items.
		/**
		 * @brief Keyed array literal item.
		 *
		 * Captures optional `[key]=` prefixes from the literal,
		 * preserving order and letting the executor decide whether
		 * they're indexed arithmetic subscripts or assoc-array
		 * string keys.
		 */
		struct Keyed {
			bool has_key = false;       ///< True iff `[key]=` was present.
			Word key;                   ///< Key text; expanded at exec time.
			Word value;                 ///< RHS value.
		};
		std::vector<Keyed> keyed_items; ///< Keyed array items in source order.
		SourceLoc loc;                  ///< Position of the assignment.
	};

	// ----- AST ------------------------------------------------------------------

	/**
	 * @brief Base class for every AST node.
	 *
	 * Carries the discriminating `kind`, source location, byte-range
	 * span into the original source, and a shared-ownership pointer
	 * to that source string. The executor downcasts on `kind`; there
	 * is no virtual dispatch beyond the destructor.
	 */
	struct Node {
		/// Concrete subclass tag.
		enum class Kind {
			SimpleCommand,  ///< `simple_command` — words + assignments + redirs.
			Pipeline,       ///< `cmd | cmd | cmd` (with optional `!` and `time`).
			AndOr,          ///< `cmd && cmd` / `cmd || cmd`.
			List,           ///< Sequence of `cmd ;` or `cmd &` items.
			BraceGroup,     ///< `{ list; }`.
			Subshell,       ///< `( list )`.
			IfClause,       ///< `if … then … [elif …] [else …] fi`.
			WhileClause,    ///< `while …` / `until …`.
			ForClause,      ///< `for var [in words]; do …; done`.
			CaseClause,     ///< `case … in …; esac`.
			FunctionDef,    ///< `name() { … }` definition.
			DBracket,       ///< `[[ … ]]` conditional.
		};
		Kind kind;                  ///< Concrete subclass tag.
		SourceLoc loc;              ///< Position in the source.
		std::size_t src_start = 0;  ///< Inclusive byte offset in source.
		std::size_t src_end   = 0;  ///< Exclusive byte offset in source.
		/**
		 * @brief Source string this node was parsed from.
		 *
		 * Multiple ASTs can coexist (main script, inherited functions,
		 * sourced files); each node carries its own source so slice
		 * extraction always uses the right text.
		 */
		std::shared_ptr<const std::string> source_text;
		explicit Node(Kind k) : kind(k) {}
		virtual ~Node() = default;
	};
	/// Owning pointer used everywhere a child node is held.
	using NodePtr = std::unique_ptr<Node>;

	/// Human-readable name for a Node::Kind (debug dumps, errors).
	const char* nodeKindName(Node::Kind k);

	/// `name=val ... cmd arg1 ... [redirs]` — a leaf command node.
	struct SimpleCommand : Node {
		SimpleCommand() : Node(Kind::SimpleCommand) {}
		std::vector<Assignment> assignments;    ///< Pre-command assignments.
		std::vector<Word> words;                ///< argv[0..N], pre-expansion.
		std::vector<Redirection> redirs;        ///< Attached redirections.
	};

	/// `[!] [time] cmd | cmd | cmd` — pipeline of N commands (N >= 1).
	struct Pipeline : Node {
		Pipeline() : Node(Kind::Pipeline) {}
		bool bang = false;                  ///< `!`-prefixed: invert exit status.
		/**
		 * @brief `time` reserved-word prefix.
		 *
		 * When set, the executor wraps the whole pipeline in a
		 * `steady_clock` measurement and prints the elapsed real
		 * time on stderr after it finishes.
		 */
		bool timed = false;
		std::vector<NodePtr> commands;      ///< N pipeline elements.
		/**
		 * @brief Per-pipe stderr-merge flag.
		 *
		 * `stderr_to_stdout[i]` applies to the pipe between
		 * `commands[i]` and `commands[i+1]` (true for `|&`).
		 */
		std::vector<bool> stderr_to_stdout;
	};

	/// `lhs && rhs` or `lhs || rhs`.
	struct AndOr : Node {
		/// Connective operator.
		enum class Op { AndIf, OrIf };
		AndOr() : Node(Kind::AndOr) {}
		NodePtr left;               ///< Left operand.
		NodePtr right;              ///< Right operand.
		Op op = Op::AndIf;          ///< `&&` (default) or `||`.
	};

	/// One entry in a List node — a command plus its terminator semantics.
	struct ListItem {
		NodePtr command;        ///< Pipeline, AndOr, or compound command directly.
		bool background = false;///< True if terminated with `&` instead of `;`.
	};

	/// Sequence of commands separated by `;`, `&`, or newlines.
	struct List : Node {
		List() : Node(Kind::List) {}
		std::vector<ListItem> items;    ///< Items in source order.
	};

	/// `{ list; }` — runs in the current shell.
	struct BraceGroup : Node {
		BraceGroup() : Node(Kind::BraceGroup) {}
		NodePtr body;                       ///< Inner List.
		std::vector<Redirection> redirs;    ///< Redirections applied to the group.
	};

	/// `( list )` — runs in a subshell.
	struct Subshell : Node {
		Subshell() : Node(Kind::Subshell) {}
		NodePtr body;                       ///< Inner List.
		std::vector<Redirection> redirs;    ///< Redirections applied to the subshell.
	};

	/// `if … then … [elif …]* [else …] fi`.
	struct IfClause : Node {
		IfClause() : Node(Kind::IfClause) {}
		/// One `if` / `elif` arm (cond + body).
		struct Branch { NodePtr cond; NodePtr body; };
		std::vector<Branch> branches;       ///< `[0]` = if, `[1..]` = elif.
		NodePtr else_body;                  ///< Optional `else` body.
		std::vector<Redirection> redirs;    ///< Redirections on the whole clause.
	};

	/// `while … do … done` and `until … do … done` (distinguished by `until`).
	struct WhileClause : Node {
		WhileClause() : Node(Kind::WhileClause) {}
		bool until = false;                 ///< True iff this is `until`, not `while`.
		NodePtr cond;                       ///< Loop guard.
		NodePtr body;                       ///< Loop body.
		std::vector<Redirection> redirs;    ///< Redirections on the loop.
	};

	/// `for var [in words]; do … done`.
	struct ForClause : Node {
		ForClause() : Node(Kind::ForClause) {}
		std::string var;                    ///< Loop variable.
		bool has_in = false;                ///< False => iterates `"$@"`.
		std::vector<Word> items;            ///< Word list after `in`.
		NodePtr body;                       ///< Loop body.
		std::vector<Redirection> redirs;    ///< Redirections on the loop.
	};

	/// `case subject in pat) body;; … esac`.
	struct CaseClause : Node {
		/// How the previous case item terminated.
		enum class Term {
			DSemi,      ///< `;;`
			SemiAmp,    ///< `;&`  (fall through unconditionally to next)
			DSemiAmp,   ///< `;;&` (re-evaluate from next pattern)
		};
		CaseClause() : Node(Kind::CaseClause) {}
		Word subject;                       ///< Word being matched against.
		/// One pattern arm.
		struct Item {
			std::vector<Word> patterns;     ///< Pipe-separated pattern alternatives.
			NodePtr body;                   ///< List, or null for an empty body.
			Term term = Term::DSemi;        ///< Terminator after this arm.
		};
		std::vector<Item> items;            ///< Pattern arms in source order.
		std::vector<Redirection> redirs;    ///< Redirections on the whole `case`.
	};

	/**
	 * @brief `[[ … ]]` — bash conditional expression.
	 *
	 * Evaluated without word splitting or pathname expansion on its
	 * operand words; supports unary file tests, binary string and
	 * arithmetic operators, regex match (`=~`), and Boolean
	 * `&&` / `||` / `!`.
	 */
	struct DBracketCond : Node {
		DBracketCond() : Node(Kind::DBracket) {}
		/// One node in the `[[ … ]]` expression tree.
		struct Expr {
			/// Expression node kind.
			enum class K { And, Or, Not, Prim };
			K k = K::Prim;                  ///< Node kind.
			std::unique_ptr<Expr> a;        ///< And/Or: left; Not: only operand.
			std::unique_ptr<Expr> b;        ///< And/Or: right (else null).
			/**
			 * @brief Operator name for `Prim` nodes.
			 *
			 * - `""`: truthiness of `lhs` (non-empty string is true).
			 * - `"-f"`, `"-d"`, `"-z"`, …: unary tests.
			 * - `"=="`, `"!="`, `"="`, `"<"`, `">"`, `"=~"`,
			 *   `"-eq"`, …: binary tests.
			 */
			std::string op;
			Word lhs;                       ///< Left / only operand.
			Word rhs;                       ///< Right operand (binary ops only).
		};
		std::unique_ptr<Expr> root;         ///< Expression tree root.
		std::vector<Redirection> redirs;    ///< Redirections (rare; legal in bash).
	};

	/// `name() { body; }` function definition.
	struct FunctionDef : Node {
		FunctionDef() : Node(Kind::FunctionDef) {}
		std::string name;                   ///< Function name.
		NodePtr body;                       ///< Typically a BraceGroup or Subshell.
		/**
		 * @brief Pre-sliced source text for the body.
		 *
		 * Captured at parse time so the executor can serialise
		 * functions for self-spawned subshells (and so things keep
		 * working even if the original source buffer is later
		 * moved or freed).
		 */
		std::string body_text;
		std::vector<Redirection> redirs;    ///< Redirections on the function.
	};

}  // namespace wbsh

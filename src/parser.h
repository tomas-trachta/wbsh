#pragma once

/**
 * @file parser.h
 * @brief Recursive-descent parser from tokens to AST.
 *
 * Consumes a token vector produced by Lexer::tokenize() and builds a
 * top-level List node tree (see ast.h). Best-effort: errors are
 * accumulated and parsing continues so that a single bad command in a
 * script doesn't suppress diagnostics for everything after it.
 *
 * Each Node carries a byte-range `[src_start, src_end)` into the
 * shared source string so the executor can re-extract a node's
 * original text — needed for spawning child wbsh processes and for
 * faithful `declare -f` output.
 */

#include "ast.h"
#include "lexer.h"

#include <string>
#include <vector>

namespace wbsh {

	/// Parser diagnostic.
	struct ParseError {
		SourceLoc loc;          ///< Where in the source the problem was detected.
		std::string message;    ///< Human-readable description.
	};

	/**
	 * @brief Parses a token stream into the wbsh AST.
	 *
	 * Construct with a token vector (terminated by an EndOfInput
	 * token, as produced by Lexer::tokenize). Optionally pass the
	 * original source string so AST nodes can later be sliced out of
	 * it. Errors are accumulated; parsing continues best-effort.
	 */
	class Parser {
	public:
		/// Parse without a source-text reference (no node slicing).
		explicit Parser(std::vector<Token> tokens);
		/// Parse with an owned source-text copy.
		Parser(std::vector<Token> tokens, std::string source_text);
		/// Parse with a shared, pre-existing source-text buffer.
		Parser(std::vector<Token> tokens, std::shared_ptr<const std::string> source);

		/// Parse a complete program; returns the top-level List node.
		NodePtr parseProgram();

		/// Diagnostics accumulated during parsing.
		const std::vector<ParseError>& errors() const { return errors_; }
		/// True iff parsing completed with no errors.
		bool ok() const { return errors_.empty(); }

	private:
		// ---- Token cursor ----
		const Token& peek(std::size_t n = 0) const;
		const Token& advance();
		bool atEnd() const;
		bool check(TokKind k) const;
		bool match(TokKind k);
		bool checkReserved(const char* word) const;
		bool matchReserved(const char* word);
		bool isReserved(const Token& t, const char* word) const;
		bool checkAnyReserved(std::initializer_list<const char*> words) const;

		void error(const Token& at, std::string msg);
		void expect(TokKind k, const char* msg);
		void expectReserved(const char* word, const char* msg);

		// ---- Common helpers ----
		void skipNewlines();
		bool atRedirOp() const;
		bool atListSeparator() const;        // ; & or newline
		bool atCommandStart() const;
		Word tokenToWord(const Token& t) const;
		bool tokenIsReservedTerminator(const Token& t) const;

		// Byte offset just before the next-to-be-consumed token. Used to
		// stamp src_start at the entry of a parse production.
		std::size_t srcOffsetHere() const;
		// Byte offset just past the last consumed token.
		std::size_t srcOffsetEnd() const;
		// Stamp `node` with the span [start_offset, current_end_offset()).
		void stampSpan(Node& node, std::size_t start_offset);

		// ---- Grammar productions ----
		NodePtr parseList(bool top_level);
		NodePtr parseAndOr();
		NodePtr parsePipeline();
		NodePtr parseCommand();
		NodePtr parseSimpleCommand();
		NodePtr parseBraceGroup();
		NodePtr parseSubshell();
		NodePtr parseIf();
		NodePtr parseWhileUntil(bool until);
		NodePtr parseFor();
		NodePtr parseCase();
		NodePtr parseFunctionRest(std::string name, SourceLoc loc);
		NodePtr parseDoGroup();
		NodePtr parseDBracket();
		std::unique_ptr<DBracketCond::Expr> parseDBracketExpr();
		std::unique_ptr<DBracketCond::Expr> parseDBracketAnd();
		std::unique_ptr<DBracketCond::Expr> parseDBracketUnary();
		std::unique_ptr<DBracketCond::Expr> parseDBracketPrimary();
		bool atDBracketEnd() const;
		NodePtr parseCompoundListUntilReserved(std::initializer_list<const char*> stops);

		// Redirections are syntactically interleaved through compound commands.
		bool tryParseRedirection(Redirection& out);

		// Try to interpret a Word token as `name=value`. Returns false if the
		// token does not have the assignment shape.
		bool extractAssignment(const Token& t, Assignment& out) const;

		// ---- State ----
		std::vector<Token> toks_;
		// Shared-ownership source text. Lives at least as long as any AST
		// node the parser produces; each node receives this same shared_ptr.
		std::shared_ptr<const std::string> source_;
		std::size_t pos_ = 0;
		std::vector<ParseError> errors_;
	};

}  // namespace wbsh

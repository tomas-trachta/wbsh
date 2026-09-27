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

#include "arena.h"
#include "ast.h"
#include "lexer.h"

#include <string>
#include <vector>

namespace wbsh {

	struct ParseError {
		SourceLoc loc;
		std::string message;
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
		/// Parse with an owned source-text copy (interned in the arena).
		Parser(std::vector<Token> tokens, std::string source_text);

		/// The node tree is owned by this parser's Arena — see takeArena().
		NodePtr parseProgram();

		/**
		 * @brief Transfer ownership of the AST's backing memory.
		 *
		 * Everything parseProgram() produced — nodes and the interned
		 * source text — lives in this arena. Callers that keep the AST
		 * beyond the parser's lifetime (the executor adopting function
		 * definitions, for example) must take and hold it. The parser
		 * must not be used again afterwards.
		 */
		Arena takeArena() { return std::move(arena_); }

		const std::vector<ParseError>& errors() const { return errors_; }
		bool ok() const { return errors_.empty(); }

	private:
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

		void skipNewlines();
		bool atRedirOp() const;
		bool atListSeparator() const;
		bool atCommandStart() const;
		bool atFunctionNameParens() const;
		Word tokenToWord(const Token& t) const;
		bool tokenIsReservedTerminator(const Token& t) const;

		// Byte offset just before the next-to-be-consumed token; stamps
		// src_start at the entry of a parse production.
		std::size_t srcOffsetHere() const;
		// Byte offset just past the last consumed token.
		std::size_t srcOffsetEnd() const;
		// Stamps `node` with the span [start_offset, srcOffsetEnd()).
		void stampSpan(Node& node, std::size_t start_offset);

		NodePtr parseList(bool top_level);
		bool matchListSeparator(ListItem& item);
		NodePtr parseAndOr();
		NodePtr parsePipeline();
		bool matchTimePrefix();
		NodePtr parseCommand();
		NodePtr parseFunctionKeyword();
		NodePtr parseSimpleCommand();
		NodePtr parseBraceGroup();
		NodePtr parseSubshell();
		NodePtr parseArithCommand();
		NodePtr parseIf();
		NodePtr parseWhileUntil(bool until);
		NodePtr parseFor();
		NodePtr parseForArith(std::size_t start, SourceLoc loc);
		NodePtr parseSelect();
		NodePtr parseWordListLoop(std::size_t start, SourceLoc loc,
		                          const char* keyword, bool is_select);
		bool parseInWordListHeader(std::string& var, bool& has_in,
		                          std::vector<Word>& items, const char* keyword);
		void parseInWordList(std::vector<Word>& items);
		NodePtr parseCase();
		void parseCaseItem(CaseClause::Item& item);
		CaseClause::Term matchCaseTerminator();
		NodePtr parseFunctionRest(std::string name, SourceLoc loc);
		void captureFunctionBodyText(FunctionDef& function) const;
		NodePtr parseDoGroup();
		NodePtr parseDBracket();
		DBracketCond::Expr* parseDBracketExpr();
		DBracketCond::Expr* parseDBracketAnd();
		DBracketCond::Expr* parseDBracketUnary();
		DBracketCond::Expr* parseDBracketPrimary();
		bool tryParseDBracketUnary(DBracketCond::Expr& e);
		bool atDBracketUnaryTest() const;
		bool atDBracketEnd() const;
		NodePtr parseCompoundListUntilReserved(std::initializer_list<const char*> stops);

		// Redirections are syntactically interleaved through compound commands.
		bool tryParseRedirection(Redirection& out);
		void parseTrailingRedirections(std::vector<Redirection>& redirs);

		bool tryExtractAssignment(const Token& t, Assignment& out) const;
		bool tryConsumeLeadingAssignment(SimpleCommand& cmd);
		void parseArrayLiteralBody(Assignment& a);
		void parseArrayLiteralItem(Assignment& a);

		std::vector<Token> toks_;
		// Owns every node this parser produces plus the interned source
		// text; transferred out via takeArena() when the AST outlives us.
		Arena arena_;
		// Borrowed pointer to the arena-interned source text; stamped
		// onto every node so slice extraction uses the right buffer.
		const std::string* source_ = nullptr;
		std::size_t pos_ = 0;
		std::vector<ParseError> errors_;
	};

}  // namespace wbsh

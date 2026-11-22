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
		bool discards_command = false;
		/// bash reports this one too, right after the error before it.
		bool follows_previous = false;
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

		/// How many leading top-level commands of parseProgram's result sit
		/// on complete lines that end before `offset`: what bash, reading
		/// line by line, has already run when it meets a syntax error there.
		std::size_t completeCommandsBefore(std::size_t offset) const;

		/// Parse the whole input as one `name=(...)` assignment.
		bool parseLoneArrayAssignment(Assignment& out);

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

		// bash names the innermost compound command still open when the
		// input ends: "unexpected end of file from `if' command on line 3".
		struct Opener {
			const char* keyword;
			std::size_t line;
		};

		struct OpenerScope {
			OpenerScope(Parser& parser, const char* keyword, std::size_t line);
			~OpenerScope();
			OpenerScope(const OpenerScope&) = delete;
			OpenerScope& operator=(const OpenerScope&) = delete;

			Parser& owner;
		};

		void expectClosing(const char* word);
		void expectClosingParen();
		void reportUnexpected();
		void reportEndOfFileAfter();
		void reportUnexpectedAtLineEnd();
		void reportUnclosedArrayLiteral();
		std::size_t endOfInputLine() const;

		void skipNewlines();
		bool atRedirOp() const;
		bool atListSeparator() const;
		bool atCommandStart() const;
		bool atFunctionNameParens() const;
		Word tokenToWord(const Token& t);
		void checkSubstitutions(const Token& t, const std::vector<WordSegment>& segments);
		void checkSubstitutionBody(const Token& t, const std::string& body);
		void reportSubstitutionError(const Token& t, const std::string& body, const ParseError& inner);
		bool tokenIsReservedTerminator(const Token& t) const;

		// Byte offset just before the next-to-be-consumed token; stamps
		// src_start at the entry of a parse production.
		std::size_t srcOffsetHere() const;
		// Byte offset just past the last consumed token.
		std::size_t srcOffsetEnd() const;
		// Stamps `node` with the span [start_offset, srcOffsetEnd()).
		void stampSpan(Node& node, std::size_t start_offset);

		NodePtr parseList(bool top_level);
		NodePtr parseCompoundList();
		void recordLineEnd(std::size_t complete_items);
		std::string tokenSpelling(const Token& token) const;
		bool matchListSeparator(ListItem& item);
		NodePtr parseAndOr();
		NodePtr parsePipeline();
		bool matchTimePrefix();
		NodePtr makeNullCommand();
		struct PipelinePrefixes {
			bool timed = false;
			bool time_posix = false;
			bool bang = false;
			bool saw_bang = false;
		};

		void matchPipelinePrefixes(PipelinePrefixes& out);
		bool atBareListTerminator() const;
		bool matchTimePosixOption();
		bool matchTimeDashDash();
		NodePtr parseCommand();
		NodePtr parseFunctionKeyword();
		NodePtr parseCoproc();
		bool atCoprocName() const;
		NodePtr parseSimpleCommand();
		NodePtr parseBraceGroup();
		NodePtr parseSubshell();
		NodePtr parseArithCommand();
		NodePtr parseIf();
		NodePtr parseWhileUntil(bool until);
		NodePtr parseFor();
		NodePtr parseForArith(std::size_t start, SourceLoc loc);
		void reportArithForHeader(const Token& header, std::size_t parts);
		NodePtr parseSelect();
		NodePtr parseWordListLoop(std::size_t start, SourceLoc loc, bool is_select);
		bool parseInWordListHeader(std::string& var, bool& has_in, std::vector<Word>& items);
		void parseInWordList(std::vector<Word>& items);
		NodePtr parseCase();
		void parseCaseItem(CaseClause::Item& item);
		CaseClause::Term matchCaseTerminator();
		bool atCompoundCommand() const;
		NodePtr parseFunctionRest(std::string name, SourceLoc loc);
		void captureFunctionBodyText(FunctionDef& function) const;
		NodePtr parseDoGroup();
		NodePtr parseDBracket();
		DBracketCond::Expr* parseDBracketExpr();
		DBracketCond::Expr* parseDBracketAnd();
		DBracketCond::Expr* parseDBracketUnary();
		DBracketCond::Expr* parseDBracketPrimary();
		DBracketCond::Expr* parseDBracketGroup();
		bool atDBracketOperand() const;
		bool atDBracketTermEnd() const;
		void reportDBracketLeftover();
		void parseDBracketBinaryRest(DBracketCond::Expr& expr);
		void condError(const std::string& detail, const Token& at);
		bool tryParseDBracketUnary(DBracketCond::Expr& e);
		bool atDBracketUnaryTest() const;
		bool atDBracketEnd() const;
		NodePtr parseCompoundListUntilReserved(std::initializer_list<const char*> stops);

		// Redirections are syntactically interleaved through compound commands.
		bool tryParseRedirection(Redirection& out);
		void parseTrailingRedirections(std::vector<Redirection>& redirs);

		bool tryExtractAssignment(const Token& t, Assignment& out) const;
		bool tryConsumeLeadingAssignment(SimpleCommand& cmd);
		bool tryConsumeDeclarationArg(SimpleCommand& cmd);
		bool tryConsumeCompoundWord(SimpleCommand& cmd);
		void takeParenthesizedValue(Assignment& assignment, std::size_t open);
		std::string compoundTextFrom(std::size_t first_token) const;
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
		std::vector<Opener> openers_;

		// Top-level newlines that end a complete line, with the number of
		// top-level commands parsed by then, in source order.
		struct LineEnd {
			std::size_t complete_items;
			std::size_t offset;
		};
		std::vector<LineEnd> line_ends_;
	};

	/// Status of a script, `eval` or `source` that stopped at a syntax error.
	constexpr int kSyntaxErrorStatus = 2;

	/// Offset of the earliest lexer or parser error, or npos when there is none.
	std::size_t firstSyntaxErrorOffset(const std::vector<LexError>& lex_errors,
	                                   const std::vector<ParseError>& parse_errors);

	/// The earliest lexer or parser error, the only one bash reports; false
	/// when there is none.
	bool firstSyntaxError(const std::vector<LexError>& lex_errors,
	                      const std::vector<ParseError>& parse_errors, ParseError& out_error);

	/// The error bash reports right after @p first, if there is one.
	bool followUpSyntaxError(const std::vector<ParseError>& parse_errors, const ParseError& first,
	                         ParseError& out_error);

	/// Where bash reads on after the first syntax error when that error
	/// only discards the command it sits in: the start of the next line.
	/// npos when there is no such error.
	std::size_t discardResumeOffset(const std::string& source,
	                                const std::vector<LexError>& lex_errors,
	                                const std::vector<ParseError>& parse_errors);

	/// Keeps only the top-level commands bash runs before it reports a
	/// syntax error at `error_offset` (all of them when it is npos).
	void truncateAtSyntaxError(Node& program, const Parser& parser, std::size_t error_offset);

}  // namespace wbsh

#pragma once

/**
 * @file lexer.h
 * @brief POSIX shell tokenizer.
 *
 * Scans raw program text into a vector of Token values, capturing the
 * structural boundaries of quoting and `$`-expansion as a sequence of
 * WordSegment values. The bodies of expansions (`$(...)`, `${...}`,
 * `$((...))`, etc.) are stored as raw text and re-parsed by later
 * stages — the lexer's job is to find their extents, not to interpret
 * them.
 *
 * Heredoc bodies are also captured here: when a `<<` operator and its
 * delimiter word are seen, the lexer remembers the delimiter and, on
 * the next newline, slurps lines until the delimiter is matched.
 */

#include "source.h"

#include <string>
#include <vector>

namespace wbsh {

	// Reserved words (`if`, `for`, …) are not token kinds; the parser
	// recognises them from the raw text of a Word token.
	enum class TokKind {
		Word,
		IoNumber,        // digits immediately followed by '<' or '>' (no space)
		Newline,
		EndOfInput,

		AndIf,           // &&
		OrIf,            // ||
		DSemi,           // ;;
		SemiAmp,         // ;&     (bash, fall-through case terminator)
		DSemiAmp,        // ;;&    (bash)
		Semi,            // ;
		Amp,             // &
		Pipe,            // |
		PipeAmp,         // |&     (bash, equivalent to 2>&1 |)
		LParen,          // (
		RParen,          // )
		DArithCmd,       // ((...)) arithmetic command; text holds the body

		Less,            // <
		Great,           // >
		DLess,           // <<
		DLessDash,       // <<-
		TLess,           // <<<
		DGreat,          // >>
		LessAnd,         // <&
		GreatAnd,        // >&
		LessGreat,       // <>
		Clobber,         // >|
		AmpGreat,        // &>
		AmpDGreat,       // &>>
	};

	const char* tokKindName(TokKind k);

	// One segment of a Word token. Expansion bodies are kept as raw
	// text for the expander to re-parse.
	struct WordSegment {
		enum class Kind {
			Literal,         ///< Raw literal characters.
			Escaped,         ///< Single backslash-escaped char; text holds the char.
			SingleQuoted,    ///< Body between `'...'`.
			DoubleQuoted,    ///< `"..."` — `nested` holds the inner segments; text empty.
			DollarSingle,    ///< `$'...'` — text is the raw body (interpreted later).
			SimpleVar,       ///< `$name` — text is the name.
			ParamExp,        ///< `${...}` — text is the body, no outer braces.
			CmdSubst,        ///< `$(...)` or backquotes — text is the body.
			ArithExp,        ///< `$((...))` — text is the body.
			ProcSubst,       ///< `<(...)` or `>(...)` — text is body, `proc_dir` is `<`/`>`.
		};
		Kind kind = Kind::Literal;
		std::string text;
		std::vector<WordSegment> nested;
		char proc_dir = 0;
	};

	const char* segKindName(WordSegment::Kind k);

	struct Token {
		TokKind kind = TokKind::EndOfInput;
		std::string text;                   ///< Raw spelling from the source.
		std::vector<WordSegment> segments;  ///< Populated for TokKind::Word.
		bool first_on_line = false;         ///< First non-whitespace token on its line.
		SourceLoc loc;                      ///< Position of the token's first character.

		// Filled in when this token is the delimiter WORD that
		// immediately follows a DLess / DLessDash operator. The body
		// is collected after the next newline.
		bool is_heredoc_delim = false;
		bool heredoc_strip_tabs = false;    ///< `<<-` form: strip leading tabs.
		bool heredoc_quoted = false;        ///< Delimiter quoted -> body kept verbatim.
		std::string heredoc_body;           ///< Slurped body (excludes terminator line).
	};

	struct LexError {
		SourceLoc loc;
		std::string message;
	};

	// Errors are accumulated rather than thrown so the parser can still
	// try to make progress.
	class Lexer {
	public:
		explicit Lexer(std::string input);
		/// Number lines from @p line: the text was cut out of a larger script.
		void startAtLine(std::size_t line) { loc_.line = line; }

		/// The result is always terminated with a TokKind::EndOfInput token.
		std::vector<Token> tokenize();

		const std::vector<LexError>& errors() const { return errors_; }

		// For callers that scan raw text (heredoc bodies) for the same
		// forms the lexer finds in words. `body_start` is the offset just
		// past `$(` resp. `$((`; the result is the offset of the closing
		// `)` (the first of `))`), or npos when the body is unterminated
		// resp. turns out not to be arithmetic.
		static std::size_t commandSubstitutionEnd(const std::string& text, std::size_t body_start);
		static std::size_t arithmeticEnd(const std::string& text, std::size_t body_start);

	private:
		char peek(std::size_t n = 0) const;
		char advance();
		bool eof() const;
		bool match(char c);
		void skipLineContinuations();
		void error(const SourceLoc& at, std::string msg);

		void scanToken();
		void scanProcessSubstitution();
		void scanOperator();
		void scanArithCommand(SourceLoc start);
		void emitOperator(SourceLoc start, TokKind kind, const char* text);
		void scanAmpRun(SourceLoc start);
		void scanSemiRun(SourceLoc start);
		void scanLessRun(SourceLoc start);
		void scanGreatRun(SourceLoc start);
		void scanWord();
		void scanWordSegments(Token& word);
		bool readWordPart(std::vector<WordSegment>& out, int& group_depth);
		void scanRegexSegments(Token& word);
		bool readRegexPart(std::vector<WordSegment>& out, int& paren_depth);
		bool atRegexOperand() const;
		bool atCommandWordStart() const;
		void trackDBracket(const Token& word, bool command_start);
		bool atExtglobOpen(bool word_start) const;
		bool inCasePattern() const;
		void trackCaseWord(const Token& word, bool command_start);
		void trackCaseOperator(TokKind kind);
		void markHeredocDelimiter(Token& word) const;
		void skipWhitespace();
		void skipComment();

		// Each reader starts at the current position and consumes one
		// segment; readWordSegment returns false once no more segments
		// belong to the current word.
		bool readWordSegment(std::vector<WordSegment>& out);
		void readEscapedChar(std::vector<WordSegment>& out);
		bool readLiteralRun(std::vector<WordSegment>& out);
		WordSegment readSingleQuoted();
		WordSegment readDoubleQuoted();
		void readDoubleQuotedEscape(WordSegment& seg);
		void readDollar(std::vector<WordSegment>& out);
		void readLocalizedString(std::vector<WordSegment>& out);
		void readDollarSingleQuoted(SourceLoc start, std::vector<WordSegment>& out);
		void readSimpleDollarVar(char n1, std::vector<WordSegment>& out);
		WordSegment readBacktick();
		void readDollarParen(std::vector<WordSegment>& out);

		// `$((` and `((` are arithmetic only when the parenthesis that
		// closes the second `(` is directly followed by `)`; otherwise,
		// as in bash, they are a subshell nested in `$(` resp. `(`.
		enum class ArithScan { Closed, NotArithmetic, Unterminated };
		ArithScan readArithmeticBody(std::string& out);
		ArithScan closeArithmeticBody();
		ArithScan tryArithmeticAfterParen(std::string& out);
		void restorePosition(std::size_t pos, SourceLoc loc, std::size_t error_count);

		// The readBalanced* family starts just past the opening
		// delimiter and consumes through the matching closer, which
		// is not part of the result. Quoted strings and nested forms
		// of the same construct are respected.
		std::string readBalancedBraces();
		// `$[...]` (legacy bash arithmetic); nested `[...]` is respected
		// so array subscripts inside the expression don't end the form.
		std::string readBalancedBrackets();

		// Verbatim-copy helpers shared by the readBalanced* family. Each
		// consumes exactly one syntactic unit (escape, quoted string,
		// substitution) and appends the matched span to `out`.
		void copyBackslashEscape(std::string& out);
		void copySingleQuotedRun(std::string& out);
		void copyDoubleQuotedRun(std::string& out);
		void copyBackquotedRun  (std::string& out);
		void copyDollarParenRun (std::string& out);

		void collectHeredocBodies();
		std::string readHeredocBody(const Token& delim);
		std::string readLineText();

		std::string src_;
		std::size_t pos_ = 0;
		SourceLoc loc_{};
		std::vector<Token> tokens_;
		std::vector<LexError> errors_;

		bool at_line_start_ = true;
		// Inside `[[ ... ]]` the word after `=~` is a regex in which
		// `(`, `)` and `|` (and blanks inside parentheses) are word text.
		bool in_dbracket_ = false;

		// Where the words of each open `case` stand, innermost last: a
		// `!(` that leads a pattern is an extglob group, while one that
		// leads a command (bang_is_keyword_) is `!` before a subshell.
		enum class CaseState { Subject, In, Pattern, Body };
		std::vector<CaseState> case_states_;
		bool bang_is_keyword_ = false;

		// Heredoc delimiters waiting for their body after the next
		// newline, as indices into `tokens_` so the body can be filled in.
		struct PendingHeredoc {
			std::size_t delim_token_index;
		};
		std::vector<PendingHeredoc> pending_heredocs_;

		// Open `(` of the command being scanned inside a `$(` body.
		int nested_paren_depth_ = 0;

		// A `$(` body is lexed as a command, so a `)` closing a case
		// pattern or sitting in a comment or heredoc does not end it. The
		// nested scan reuses this lexer with the outer word's state set
		// aside.
		struct OuterScanState {
			std::vector<Token> tokens;
			bool at_line_start = false;
			bool in_dbracket = false;
			std::vector<CaseState> case_states;
			bool bang_is_keyword = false;
			std::vector<PendingHeredoc> pending_heredocs;
			int paren_depth = 0;
		};
		std::string readCommandSubstitutionBody();
		bool scanToUnmatchedParen();
		bool atUnmatchedCloseParen() const;
		OuterScanState enterNestedScan();
		void leaveNestedScan(OuterScanState& outer);
		void trackNestedParen(TokKind kind);
	};

}  // namespace wbsh

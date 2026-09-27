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

		/// The result is always terminated with a TokKind::EndOfInput token.
		std::vector<Token> tokenize();

		const std::vector<LexError>& errors() const { return errors_; }

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
		void readDollarSingleQuoted(SourceLoc start, std::vector<WordSegment>& out);
		void readSimpleDollarVar(char n1, std::vector<WordSegment>& out);
		WordSegment readBacktick();
		void readDollarParen(std::vector<WordSegment>& out);

		// The readBalanced* family starts just past the opening
		// delimiter and consumes through the matching closer, which
		// is not part of the result. Quoted strings and nested forms
		// of the same construct are respected.
		std::string readBalancedParens();
		std::string readBalancedBraces();
		std::string readBalancedDoubleParens();
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
		void copyDollarParenRun (std::string& out, int& paren_depth);

		void collectHeredocBodies();
		std::string readHeredocBody(const Token& delim);
		std::string readLineText();

		std::string src_;
		std::size_t pos_ = 0;
		SourceLoc loc_{};
		std::vector<Token> tokens_;
		std::vector<LexError> errors_;

		bool at_line_start_ = true;

		// Heredoc delimiters waiting for their body after the next
		// newline, as indices into `tokens_` so the body can be filled in.
		struct PendingHeredoc {
			std::size_t delim_token_index;
		};
		std::vector<PendingHeredoc> pending_heredocs_;
	};

}  // namespace wbsh

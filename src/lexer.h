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

	// ----- Tokens ---------------------------------------------------------------

	/**
	 * @brief Lexical token kind.
	 *
	 * Includes the shell's word/operator/punctuation tokens plus the
	 * synthetic terminators `Newline` and `EndOfInput`. Reserved
	 * words (`if`, `for`, …) are not separate token kinds; they're
	 * detected at parse time from the raw text of a `Word` token.
	 */
	enum class TokKind {
		Word,            // generic word; reserved words detected at parse time
		IoNumber,        // digits immediately followed by '<' or '>' (no space)
		Newline,
		EndOfInput,

		// Control / list operators
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

		// Redirection operators
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

	/// Human-readable name for a TokKind (for diagnostics / debug dumps).
	const char* tokKindName(TokKind k);

	/**
	 * @brief One segment of a Word token.
	 *
	 * A WORD is a sequence of segments. The lexer captures the
	 * structural boundaries of quotes and `$`-expansion; the contents
	 * of expansion bodies are kept as raw text and re-parsed by the
	 * expander stage later.
	 */
	struct WordSegment {
		/// Segment kind — describes what produced this slice of the word.
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
		Kind kind = Kind::Literal;          ///< What this segment is.
		std::string text;                   ///< Raw text payload (see Kind for meaning).
		std::vector<WordSegment> nested;    ///< Used by DoubleQuoted to hold inner segments.
		char proc_dir = 0;                  ///< `'<'` or `'>'` for ProcSubst, 0 otherwise.
	};

	/// Human-readable name for a WordSegment::Kind.
	const char* segKindName(WordSegment::Kind k);

	/**
	 * @brief A single lexical token.
	 *
	 * Word tokens carry a structured `segments` vector; operator and
	 * punctuation tokens use only `kind`, `text`, and `loc`. Heredoc
	 * delimiter tokens additionally carry the slurped body and its
	 * quoting/strip flags.
	 */
	struct Token {
		TokKind kind = TokKind::EndOfInput; ///< Token kind.
		std::string text;                   ///< Raw spelling from the source.
		std::vector<WordSegment> segments;  ///< Populated for TokKind::Word.
		bool first_on_line = false;         ///< First non-whitespace token on its line.
		SourceLoc loc;                      ///< Position of the token's first character.

		// ---- Heredoc state ----
		// Filled in when this token is the delimiter WORD that
		// immediately follows a DLess / DLessDash operator. The body
		// is collected after the next newline.
		bool is_heredoc_delim = false;      ///< This token is a heredoc delimiter.
		bool heredoc_strip_tabs = false;    ///< `<<-` form: strip leading tabs.
		bool heredoc_quoted = false;        ///< Delimiter quoted -> body kept verbatim.
		std::string heredoc_body;           ///< Slurped body (excludes terminator line).
	};

	// ----- Lexer ----------------------------------------------------------------

	/// Lexer diagnostic.
	struct LexError {
		SourceLoc loc;          ///< Where in the source the problem was detected.
		std::string message;    ///< Human-readable description.
	};

	/**
	 * @brief POSIX shell tokenizer.
	 *
	 * Constructed with a source string; call tokenize() to get the
	 * full token vector. Errors are accumulated rather than thrown so
	 * the parser can still try to make progress.
	 */
	class Lexer {
	public:
		/// Construct a Lexer over @p input (takes ownership of the buffer).
		explicit Lexer(std::string input);

		/**
		 * @brief Tokenize the entire input.
		 *
		 * @return Vector of tokens. The vector is always terminated
		 *         with a TokKind::EndOfInput token.
		 *
		 * @note Errors are accumulated, not thrown. Inspect errors()
		 *       afterwards.
		 */
		std::vector<Token> tokenize();

		/// Diagnostics accumulated during the last tokenize() call.
		const std::vector<LexError>& errors() const { return errors_; }

	private:
		// ---- Character stream helpers ----
		char peek(std::size_t n = 0) const;
		char advance();                       // consume + return current char
		bool eof() const;
		bool match(char c);                   // advance if peek()==c
		bool startsWith(const char* s) const;
		void skipLineContinuations();         // collapse "\\\n"
		void error(const SourceLoc& at, std::string msg);

		// ---- Top-level dispatch ----
		void scanToken();
		void scanOperator();                  // assumes current char starts an operator
		void scanWord();                      // builds a Word token from current position
		void skipWhitespace();                // spaces / tabs (no newlines)
		void skipComment();                   // from '#' to end of line (excl newline)

		// ---- Word internals ----
		// Read a single segment into `out`, starting at the current position.
		// Returns false if no more segments belong to the current word.
		bool readWordSegment(std::vector<WordSegment>& out);
		void readSingleQuoted(WordSegment& seg);
		void readDoubleQuoted(WordSegment& seg);
		void readDollar(std::vector<WordSegment>& out);   // dispatch on $-form
		void readBacktick(WordSegment& seg);
		void readDollarBrace(WordSegment& seg);           // ${...}
		void readDollarParen(std::vector<WordSegment>& out); // $(...) or $((...))
		void readDollarSingle(WordSegment& seg);          // $'...'

		// Capture the body of a balanced ()-delimited construct starting just
		// past the opening '('. Stops at the matching ')'. Respects strings
		// and nested parens. Result excludes the closing ')'.
		std::string readBalancedParens();

		// Capture the body of a ${...} construct, starting past the '{'.
		// Stops at the matching '}'. Respects nested ${...} and strings.
		std::string readBalancedBraces();

		// Capture a $((...)) body, starting past the leading "((".
		// Stops at the matching "))". Respects strings and nested ((..)).
		std::string readBalancedDoubleParens();

		// Verbatim-copy helpers shared by the readBalanced* family. Each
		// consumes exactly one syntactic unit (escape, quoted string,
		// substitution) and appends the matched span to `out`.
		void copyBackslashEscape(std::string& out);
		void copySingleQuotedRun(std::string& out);
		void copyDoubleQuotedRun(std::string& out);
		void copyBackquotedRun  (std::string& out);
		void copyDollarParenRun (std::string& out, int* paren_depth);

		// ---- Heredoc handling ----
		// Examine `delimiter_token` (the WORD that followed <<). Stash a flag for
		// when we hit a newline; afterwards the body is collected until a line
		// matches the delimiter.
		void recordHeredoc(Token& delim, bool strip_tabs);
		void collectHeredocBodies();          // run after each "logical" newline

		// ---- State ----
		std::string src_;
		std::size_t pos_ = 0;
		SourceLoc loc_{};
		std::vector<Token> tokens_;
		std::vector<LexError> errors_;

		// Per-line state.
		bool at_line_start_ = true;

		// Pending heredoc delimiters waiting for their body to be read after the
		// next newline. We store indices into `tokens_` so we can fill them in.
		struct PendingHeredoc {
			std::size_t delim_token_index;
		};
		std::vector<PendingHeredoc> pending_heredocs_;
	};

}  // namespace wbsh

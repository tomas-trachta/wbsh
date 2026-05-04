#pragma once

#include "source.h"

#include <string>
#include <vector>

namespace wbsh {

	// ----- Tokens ---------------------------------------------------------------

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

	const char* tokKindName(TokKind k);

	// A WORD is a sequence of segments. The lexer captures the structural
	// boundaries of quotes / expansions; the *contents* of expansion bodies
	// are stored as raw text and re-parsed by the expander stage later.
	struct WordSegment {
		enum class Kind {
			Literal,         // raw literal characters
			Escaped,         // single character escaped with backslash; text holds the char
			SingleQuoted,    // text is the body between '...'
			DoubleQuoted,    // nested holds the inner segments; text is empty
			DollarSingle,    // $'...'  text is the raw body (interpreted later)
			SimpleVar,       // $name   text is the name
			ParamExp,        // ${...}  text is the body, no outer braces
			CmdSubst,        // $(...) or `...`   text is the body
			ArithExp,        // $((...))   text is the body
			ProcSubst,       // <(...) or >(...) — text is the body, proc_dir holds < or >
		};
		// Process substitution: text is the inner command body, proc_dir
		// holds '<' (read-end) or '>' (write-end).
		Kind kind = Kind::Literal;
		std::string text;
		std::vector<WordSegment> nested;   // used by DoubleQuoted
		char proc_dir = 0;                 // '<' or '>' for ProcSubst
	};

	const char* segKindName(WordSegment::Kind k);

	struct Token {
		TokKind kind = TokKind::EndOfInput;
		std::string text;                   // raw spelling
		std::vector<WordSegment> segments;  // populated for TokKind::Word
		bool first_on_line = false;         // useful for assignment / reserved-word detection
		SourceLoc loc;

		// Heredoc state. When this token is the delimiter WORD that immediately
		// follows a DLess or DLessDash operator token, the body collected after
		// the next newline is stored here.
		bool is_heredoc_delim = false;
		bool heredoc_strip_tabs = false;
		bool heredoc_quoted = false;        // delimiter contained quoting -> body verbatim
		std::string heredoc_body;
	};

	// ----- Lexer ----------------------------------------------------------------

	struct LexError {
		SourceLoc loc;
		std::string message;
	};

	class Lexer {
	public:
		explicit Lexer(std::string input);

		// Lex the entire input into a vector of tokens. Always terminates with
		// EndOfInput. Errors are accumulated and returned via errors().
		std::vector<Token> tokenize();

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

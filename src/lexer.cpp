/**
 * @file lexer.cpp
 * @brief POSIX shell tokenizer implementation.
 */

#include "lexer.h"

#include <cctype>
#include <cstring>
#include <utility>

namespace wbsh {

	// ---------------------------------------------------------------------------
	// Static name tables
	// ---------------------------------------------------------------------------

	const char* tokKindName(TokKind k) {
		switch (k) {
		case TokKind::Word: return "Word";
		case TokKind::IoNumber: return "IoNumber";
		case TokKind::Newline: return "Newline";
		case TokKind::EndOfInput: return "EndOfInput";
		case TokKind::AndIf: return "&&";
		case TokKind::OrIf: return "||";
		case TokKind::DSemi: return ";;";
		case TokKind::SemiAmp: return ";&";
		case TokKind::DSemiAmp: return ";;&";
		case TokKind::Semi: return ";";
		case TokKind::Amp: return "&";
		case TokKind::Pipe: return "|";
		case TokKind::PipeAmp: return "|&";
		case TokKind::LParen: return "(";
		case TokKind::RParen: return ")";
		case TokKind::Less: return "<";
		case TokKind::Great: return ">";
		case TokKind::DLess: return "<<";
		case TokKind::DLessDash: return "<<-";
		case TokKind::TLess: return "<<<";
		case TokKind::DGreat: return ">>";
		case TokKind::LessAnd: return "<&";
		case TokKind::GreatAnd: return ">&";
		case TokKind::LessGreat: return "<>";
		case TokKind::Clobber: return ">|";
		case TokKind::AmpGreat: return "&>";
		case TokKind::AmpDGreat: return "&>>";
		}
		return "?";
	}

	const char* segKindName(WordSegment::Kind k) {
		switch (k) {
		case WordSegment::Kind::Literal: return "Lit";
		case WordSegment::Kind::Escaped: return "Esc";
		case WordSegment::Kind::SingleQuoted: return "SQ";
		case WordSegment::Kind::DoubleQuoted: return "DQ";
		case WordSegment::Kind::DollarSingle: return "DSQ";
		case WordSegment::Kind::SimpleVar: return "Var";
		case WordSegment::Kind::ParamExp: return "Param";
		case WordSegment::Kind::CmdSubst: return "CmdSub";
		case WordSegment::Kind::ArithExp: return "Arith";
		}
		return "?";
	}

	// ---------------------------------------------------------------------------
	// Helpers
	// ---------------------------------------------------------------------------

	static bool isBlank(char c) { return c == ' ' || c == '\t'; }

	static bool isOperatorStart(char c) {
		switch (c) {
		case '&': case '|': case ';':
		case '<': case '>':
		case '(': case ')':
			return true;
		default:
			return false;
		}
	}

	static bool isNameStart(char c) {
		return c == '_' || std::isalpha(static_cast<unsigned char>(c));
	}
	static bool isNameCont(char c) {
		return c == '_' || std::isalnum(static_cast<unsigned char>(c));
	}

	// Concatenate a heredoc delimiter into a plain string. Delimiter quoting is
	// preserved by the caller; this just produces the comparison text.
	static std::string flattenDelim(const Token& delim) {
		std::string out;
		for (const auto& seg : delim.segments) {
			switch (seg.kind) {
			case WordSegment::Kind::DoubleQuoted:
				for (const auto& n : seg.nested) out += n.text;
				break;
			default:
				out += seg.text;
				break;
			}
		}
		return out;
	}

	static bool delimQuoted(const Token& delim) {
		for (const auto& seg : delim.segments) {
			switch (seg.kind) {
			case WordSegment::Kind::SingleQuoted:
			case WordSegment::Kind::DoubleQuoted:
			case WordSegment::Kind::Escaped:
			case WordSegment::Kind::DollarSingle:
				return true;
			default:
				break;
			}
		}
		return false;
	}

	// ---------------------------------------------------------------------------
	// Construction & top-level
	// ---------------------------------------------------------------------------

	Lexer::Lexer(std::string input) : src_(std::move(input)) {}

	std::vector<Token> Lexer::tokenize() {
		while (!eof()) {
			scanToken();
		}
		Token eofTok;
		eofTok.kind = TokKind::EndOfInput;
		eofTok.loc = loc_;
		tokens_.push_back(std::move(eofTok));
		return std::move(tokens_);
	}

	// ---------------------------------------------------------------------------
	// Stream helpers
	// ---------------------------------------------------------------------------

	bool Lexer::eof() const { return pos_ >= src_.size(); }

	char Lexer::peek(std::size_t n) const {
		if (pos_ + n >= src_.size()) return '\0';
		return src_[pos_ + n];
	}

	char Lexer::advance() {
		char c = src_[pos_++];
		loc_.offset++;
		if (c == '\n') {
			loc_.line++;
			loc_.column = 1;
		}
		else {
			loc_.column++;
		}
		return c;
	}

	bool Lexer::match(char c) {
		if (peek() != c) return false;
		advance();
		return true;
	}

	bool Lexer::startsWith(const char* s) const {
		std::size_t n = std::strlen(s);
		if (pos_ + n > src_.size()) return false;
		return std::memcmp(src_.data() + pos_, s, n) == 0;
	}

	void Lexer::skipLineContinuations() {
		while (peek() == '\\' && peek(1) == '\n') {
			advance();   // backslash
			advance();   // newline (line counter advances)
		}
	}

	void Lexer::error(const SourceLoc& at, std::string msg) {
		errors_.push_back({ at, std::move(msg) });
	}

	// ---------------------------------------------------------------------------
	// Whitespace / comments
	// ---------------------------------------------------------------------------

	void Lexer::skipWhitespace() {
		while (!eof()) {
			skipLineContinuations();
			char c = peek();
			if (isBlank(c)) {
				advance();
			}
			else {
				break;
			}
		}
	}

	void Lexer::skipComment() {
		// assumes current char is '#'
		while (!eof() && peek() != '\n') advance();
	}

	// ---------------------------------------------------------------------------
	// Top-level token dispatch
	// ---------------------------------------------------------------------------

	void Lexer::scanToken() {
		skipWhitespace();
		if (eof()) return;
		char c = peek();
		if (c == '#') {
			skipComment();
			return;
		}
		// Process substitution: `<(` or `>(` (no space between) starts a
		// Word that contains exactly one ProcSubst segment.
		if ((c == '<' || c == '>') && peek(1) == '(') {
			SourceLoc start = loc_;
			std::size_t startPos = pos_;
			char dir = advance();   // < or >
			advance();              // (
			std::string body = readBalancedParens();
			Token tok;
			tok.kind = TokKind::Word;
			tok.loc = start;
			tok.first_on_line = at_line_start_;
			at_line_start_ = false;
			WordSegment seg;
			seg.kind = WordSegment::Kind::ProcSubst;
			seg.text = body;
			seg.proc_dir = dir;
			tok.segments.push_back(std::move(seg));
			tok.text = src_.substr(startPos, pos_ - startPos);
			tokens_.push_back(std::move(tok));
			return;
		}
		if (c == '\n' || isOperatorStart(c)) {
			scanOperator();
			return;
		}
		scanWord();
	}

	// ---------------------------------------------------------------------------
	// Operator scanning
	// ---------------------------------------------------------------------------

	// Push a finished operator token, run heredoc collection on a Newline,
	// and update at_line_start_.
	void Lexer::emitOperator(SourceLoc start, TokKind kind, const char* text) {
		Token tok;
		tok.loc = start;
		tok.kind = kind;
		tok.text = text;
		tokens_.push_back(std::move(tok));
		if (kind == TokKind::Newline) {
			collectHeredocBodies();
			at_line_start_ = true;
		} else {
			at_line_start_ = false;
		}
	}

	// `&` / `&&` / `&>` / `&>>`.
	void Lexer::scanAmpRun(SourceLoc start) {
		advance();
		if (match('&'))      { emitOperator(start, TokKind::AndIf, "&&");     return; }
		if (match('>')) {
			if (match('>'))  { emitOperator(start, TokKind::AmpDGreat, "&>>"); return; }
			emitOperator(start, TokKind::AmpGreat, "&>");
			return;
		}
		emitOperator(start, TokKind::Amp, "&");
	}

	// `;` / `;;` / `;;&` / `;&`.
	void Lexer::scanSemiRun(SourceLoc start) {
		advance();
		if (match(';')) {
			if (match('&')) { emitOperator(start, TokKind::DSemiAmp, ";;&"); return; }
			emitOperator(start, TokKind::DSemi, ";;");
			return;
		}
		if (match('&')) { emitOperator(start, TokKind::SemiAmp, ";&"); return; }
		emitOperator(start, TokKind::Semi, ";");
	}

	// `<` / `<<` / `<<-` / `<<<` / `<&` / `<>`.
	void Lexer::scanLessRun(SourceLoc start) {
		advance();
		if (match('<')) {
			if (match('-'))     { emitOperator(start, TokKind::DLessDash, "<<-"); return; }
			if (match('<'))     { emitOperator(start, TokKind::TLess, "<<<");     return; }
			emitOperator(start, TokKind::DLess, "<<");
			return;
		}
		if (match('&')) { emitOperator(start, TokKind::LessAnd, "<&"); return; }
		if (match('>')) { emitOperator(start, TokKind::LessGreat, "<>"); return; }
		emitOperator(start, TokKind::Less, "<");
	}

	// `>` / `>>` / `>&` / `>|`.
	void Lexer::scanGreatRun(SourceLoc start) {
		advance();
		if (match('>')) { emitOperator(start, TokKind::DGreat, ">>");   return; }
		if (match('&')) { emitOperator(start, TokKind::GreatAnd, ">&"); return; }
		if (match('|')) { emitOperator(start, TokKind::Clobber, ">|");  return; }
		emitOperator(start, TokKind::Great, ">");
	}

	void Lexer::scanOperator() {
		const SourceLoc start = loc_;
		const char c = peek();
		switch (c) {
		case '\n': advance(); emitOperator(start, TokKind::Newline, "\n"); return;
		case '&':  scanAmpRun(start);   return;
		case '|':
			advance();
			if (match('|'))      emitOperator(start, TokKind::OrIf, "||");
			else if (match('&')) emitOperator(start, TokKind::PipeAmp, "|&");
			else                 emitOperator(start, TokKind::Pipe, "|");
			return;
		case ';': scanSemiRun(start);   return;
		case '<': scanLessRun(start);   return;
		case '>': scanGreatRun(start);  return;
		case '(': advance(); emitOperator(start, TokKind::LParen, "("); return;
		case ')': advance(); emitOperator(start, TokKind::RParen, ")"); return;
		default:
			error(start, std::string("unexpected character '") + c + "'");
			advance();
			return;
		}
	}

	// ---------------------------------------------------------------------------
	// Word scanning
	// ---------------------------------------------------------------------------

	void Lexer::scanWord() {
		SourceLoc start = loc_;
		std::size_t startPos = pos_;
		Token tok;
		tok.kind = TokKind::Word;
		tok.loc = start;
		tok.first_on_line = at_line_start_;
		at_line_start_ = false;

		while (!eof()) {
			skipLineContinuations();
			if (eof()) break;
			char c = peek();
			// Word terminators (when not inside a quoted segment).
			if (isBlank(c) || c == '\n' || isOperatorStart(c)) break;
			if (!readWordSegment(tok.segments)) break;
		}

		tok.text = src_.substr(startPos, pos_ - startPos);

		// IO_NUMBER: word is purely digit literal, immediately followed by < or >.
		bool digits_only = !tok.segments.empty();
		for (const auto& s : tok.segments) {
			if (s.kind != WordSegment::Kind::Literal) { digits_only = false; break; }
			for (char ch : s.text) {
				if (!std::isdigit(static_cast<unsigned char>(ch))) { digits_only = false; break; }
			}
			if (!digits_only) break;
		}
		if (digits_only && (peek() == '<' || peek() == '>')) {
			tok.kind = TokKind::IoNumber;
		}

		// Heredoc delimiter detection: previous token is DLess/DLessDash AND
		// there's no intervening non-delimiter (we just emitted that op last).
		if (!tokens_.empty()) {
			TokKind prev = tokens_.back().kind;
			if (prev == TokKind::DLess || prev == TokKind::DLessDash) {
				tok.is_heredoc_delim = true;
				tok.heredoc_strip_tabs = (prev == TokKind::DLessDash);
				tok.heredoc_quoted = delimQuoted(tok);
			}
		}

		tokens_.push_back(std::move(tok));

		// Register pending heredoc using the index we just pushed.
		Token& just_pushed = tokens_.back();
		if (just_pushed.is_heredoc_delim) {
			pending_heredocs_.push_back({ tokens_.size() - 1 });
		}
	}

	// Read one segment. Returns false if no segment was read (word ends).
	bool Lexer::readWordSegment(std::vector<WordSegment>& out) {
		char c = peek();
		switch (c) {
		case '\\': {
			advance();
			if (eof()) {
				// trailing backslash: treat as literal
				WordSegment s; s.kind = WordSegment::Kind::Literal; s.text = "\\";
				out.push_back(std::move(s));
				return true;
			}
			WordSegment s; s.kind = WordSegment::Kind::Escaped;
			s.text = std::string(1, advance());
			out.push_back(std::move(s));
			return true;
		}
		case '\'': {
			WordSegment s; readSingleQuoted(s);
			out.push_back(std::move(s));
			return true;
		}
		case '"': {
			WordSegment s; readDoubleQuoted(s);
			out.push_back(std::move(s));
			return true;
		}
		case '$':
			readDollar(out);
			return true;
		case '`': {
			WordSegment s; readBacktick(s);
			out.push_back(std::move(s));
			return true;
		}
		default:
			break;
		}
		// Plain literal run.
		WordSegment s; s.kind = WordSegment::Kind::Literal;
		while (!eof()) {
			skipLineContinuations();
			if (eof()) break;
			char ch = peek();
			if (ch == '\\' || ch == '\'' || ch == '"' || ch == '$' || ch == '`')
				break;
			if (isBlank(ch) || ch == '\n' || isOperatorStart(ch))
				break;
			s.text.push_back(advance());
		}
		if (s.text.empty()) return false;
		out.push_back(std::move(s));
		return true;
	}

	void Lexer::readSingleQuoted(WordSegment& seg) {
		SourceLoc start = loc_;
		advance();   // opening '
		seg.kind = WordSegment::Kind::SingleQuoted;
		while (!eof() && peek() != '\'') {
			seg.text.push_back(advance());
		}
		if (eof()) {
			error(start, "unterminated single-quoted string");
			return;
		}
		advance();   // closing '
	}

	void Lexer::readDoubleQuoted(WordSegment& seg) {
		SourceLoc start = loc_;
		advance();   // opening "
		seg.kind = WordSegment::Kind::DoubleQuoted;
		while (!eof() && peek() != '"') {
			char c = peek();
			if (c == '\\') {
				advance();
				if (eof()) break;
				char nx = peek();
				// Inside double quotes, backslash escapes only $ ` " \ <newline>.
				if (nx == '$' || nx == '`' || nx == '"' || nx == '\\' || nx == '\n') {
					if (nx == '\n') {
						advance();   // line continuation: drop both
					}
					else {
						WordSegment s; s.kind = WordSegment::Kind::Escaped;
						s.text = std::string(1, advance());
						seg.nested.push_back(std::move(s));
					}
				}
				else {
					// Backslash retained literally.
					WordSegment s; s.kind = WordSegment::Kind::Literal;
					s.text = "\\";
					seg.nested.push_back(std::move(s));
				}
				continue;
			}
			if (c == '$') {
				readDollar(seg.nested);
				continue;
			}
			if (c == '`') {
				WordSegment s; readBacktick(s);
				seg.nested.push_back(std::move(s));
				continue;
			}
			// Coalesce a run of literal characters.
			if (seg.nested.empty() || seg.nested.back().kind != WordSegment::Kind::Literal) {
				WordSegment lit; lit.kind = WordSegment::Kind::Literal;
				seg.nested.push_back(std::move(lit));
			}
			seg.nested.back().text.push_back(advance());
		}
		if (eof()) {
			error(start, "unterminated double-quoted string");
			return;
		}
		advance();   // closing "
	}

	// Read a `$'...'` ANSI-C-style string body. @p start is the `$` location,
	// used only for the unterminated-string error message.
	void Lexer::readDollarSingleQuoted(SourceLoc start, std::vector<WordSegment>& out) {
		advance(); advance();   // consume "$'"
		WordSegment s;
		s.kind = WordSegment::Kind::DollarSingle;
		// Like single-quoted but allow escape of the closing quote with backslash.
		while (!eof() && peek() != '\'') {
			if (peek() == '\\' && peek(1) != '\0') {
				s.text.push_back(advance());
				s.text.push_back(advance());
				continue;
			}
			s.text.push_back(advance());
		}
		if (eof()) error(start, "unterminated $'...' string");
		else advance();   // closing '
		out.push_back(std::move(s));
	}

	// Is @p n1 a valid first character of a simple-variable name like $x / $1 / $? ?
	static bool isSimpleVarStart(char n1) {
		return isNameStart(n1)
		    || n1 == '?' || n1 == '#' || n1 == '@' || n1 == '*'
		    || n1 == '$' || n1 == '!' || n1 == '-' || n1 == '_'
		    || std::isdigit(static_cast<unsigned char>(n1));
	}

	// Read the body of a `$name`, `$?`, `$1` etc. simple variable. Caller has
	// validated @p n1 with isSimpleVarStart and not yet consumed the `$`.
	void Lexer::readSimpleDollarVar(char n1, std::vector<WordSegment>& out) {
		advance();   // consume $
		WordSegment s;
		s.kind = WordSegment::Kind::SimpleVar;
		if (isNameStart(n1)) {
			while (!eof() && isNameCont(peek())) s.text.push_back(advance());
		} else if (std::isdigit(static_cast<unsigned char>(n1))) {
			// $0..$9 — single digit only.
			s.text.push_back(advance());
		} else {
			s.text.push_back(advance());   // single special parameter
		}
		out.push_back(std::move(s));
	}

	void Lexer::readDollar(std::vector<WordSegment>& out) {
		// assumes peek() == '$'
		const SourceLoc start = loc_;
		const char n1 = peek(1);
		switch (n1) {
		case '{': {
			advance(); advance();
			WordSegment s;
			s.kind = WordSegment::Kind::ParamExp;
			s.text = readBalancedBraces();
			out.push_back(std::move(s));
			return;
		}
		case '(':
			advance();
			readDollarParen(out);
			return;
		case '[': {
			// Legacy bash arithmetic `$[expr]`. Equivalent to `$((expr))`
			// for our purposes — re-use the same WordSegment kind so the
			// expander evaluates it as arithmetic.
			advance(); advance();
			WordSegment s;
			s.kind = WordSegment::Kind::ArithExp;
			s.text = readBalancedBrackets();
			out.push_back(std::move(s));
			return;
		}
		case '\'':
			readDollarSingleQuoted(start, out);
			return;
		case '"': {
			// $" is bash's localized string. Treat the $ as literal for now.
			WordSegment lit; lit.kind = WordSegment::Kind::Literal; lit.text = "$";
			advance();
			out.push_back(std::move(lit));
			return;
		}
		default:
			break;
		}
		if (isSimpleVarStart(n1)) {
			readSimpleDollarVar(n1, out);
			return;
		}
		// Lone $ followed by nothing useful — emit literal '$'.
		advance();
		WordSegment lit; lit.kind = WordSegment::Kind::Literal; lit.text = "$";
		out.push_back(std::move(lit));
	}

	void Lexer::readBacktick(WordSegment& seg) {
		SourceLoc start = loc_;
		advance();   // opening `
		seg.kind = WordSegment::Kind::CmdSubst;
		while (!eof() && peek() != '`') {
			if (peek() == '\\') {
				advance();
				if (eof()) break;
				char nx = peek();
				// Within backticks, \ escapes only $ ` \.
				if (nx == '$' || nx == '`' || nx == '\\') {
					seg.text.push_back(advance());
				}
				else {
					seg.text.push_back('\\');
				}
				continue;
			}
			seg.text.push_back(advance());
		}
		if (eof()) {
			error(start, "unterminated backquote command substitution");
			return;
		}
		advance();   // closing `
	}

	void Lexer::readDollarParen(std::vector<WordSegment>& out) {
		// current char is '('. Could be $(...) or $((...)).
		SourceLoc start = loc_;
		advance();   // consume '('
		if (peek() == '(') {
			advance();   // second '('
			WordSegment s;
			s.kind = WordSegment::Kind::ArithExp;
			s.text = readBalancedDoubleParens();
			out.push_back(std::move(s));
			return;
		}
		WordSegment s;
		s.kind = WordSegment::Kind::CmdSubst;
		s.text = readBalancedParens();
		out.push_back(std::move(s));
		(void)start;
	}

	// Copy a `\X` sequence (the backslash and the next char) verbatim.
	// Used by every "balanced ..." reader to preserve escapes inside strings.
	void Lexer::copyBackslashEscape(std::string& out) {
		out.push_back(advance());                  // backslash
		if (!eof()) out.push_back(advance());      // escaped char
	}

	// Copy a `'...'` single-quoted run, including the surrounding quotes.
	// Single quotes don't interpret backslashes, so anything up to the next
	// `'` is data.
	void Lexer::copySingleQuotedRun(std::string& out) {
		out.push_back(advance());                  // opening '
		while (!eof() && peek() != '\'') out.push_back(advance());
		if (!eof()) out.push_back(advance());      // closing '
	}

	// Copy a `"..."` double-quoted run, honouring `\X` escapes inside.
	void Lexer::copyDoubleQuotedRun(std::string& out) {
		out.push_back(advance());                  // opening "
		while (!eof() && peek() != '"') {
			if (peek() == '\\') { copyBackslashEscape(out); continue; }
			out.push_back(advance());
		}
		if (!eof()) out.push_back(advance());      // closing "
	}

	// Copy a backquoted run, honouring `\X` escapes inside.
	void Lexer::copyBackquotedRun(std::string& out) {
		out.push_back(advance());                  // opening `
		while (!eof() && peek() != '`') {
			if (peek() == '\\') { copyBackslashEscape(out); continue; }
			out.push_back(advance());
		}
		if (!eof()) out.push_back(advance());      // closing `
	}

	// Copy a `$(...)` or `$((...))` substitution, leaving the cursor just
	// past the closing `)` / `))`. Adjusts `*paren_depth` for `$(...)`
	// since that contributes to the outer depth.
	void Lexer::copyDollarParenRun(std::string& out, int* paren_depth) {
		out.push_back(advance());                       // $
		out.push_back(advance());                       // first (
		if (peek() != '(') {
			++*paren_depth;
			return;
		}
		// $((...)) — independent depth counter.
		out.push_back(advance());                       // second (
		int dep2 = 1;
		while (!eof() && dep2 > 0) {
			if (peek() == '(') ++dep2;
			else if (peek() == ')') --dep2;
			out.push_back(advance());
			if (dep2 == 0 && peek() == ')') {
				out.push_back(advance());
				break;
			}
		}
	}

	// Read until matching ')'. Respects single-/double-quoted strings,
	// escapes, and nested $(...) / `...` constructs. Excludes the closing ')'.
	std::string Lexer::readBalancedParens() {
		std::string out;
		int depth = 1;
		while (!eof() && depth > 0) {
			const char c = peek();
			switch (c) {
			case '\\': copyBackslashEscape(out); continue;
			case '\'': copySingleQuotedRun(out); continue;
			case '"':  copyDoubleQuotedRun(out); continue;
			case '`':  copyBackquotedRun(out); continue;
			case '$':
				if (peek(1) == '(') {
					copyDollarParenRun(out, &depth);
					continue;
				}
				out.push_back(advance());   // literal $
				continue;
			case '(':
				++depth;
				out.push_back(advance());
				continue;
			case ')':
				--depth;
				if (depth == 0) {
					advance();   // consume final )
					return out;
				}
				out.push_back(advance());
				continue;
			default:
				out.push_back(advance());
				continue;
			}
		}
		error(loc_, "unterminated $(...) substitution");
		return out;
	}

	// Read until matching '}'. Respects strings, escapes, and nested ${...}.
	std::string Lexer::readBalancedBraces() {
		std::string out;
		int depth = 1;
		while (!eof() && depth > 0) {
			char c = peek();
			switch (c) {
			case '\\': copyBackslashEscape(out); continue;
			case '\'': copySingleQuotedRun(out); continue;
			case '"':  copyDoubleQuotedRun(out); continue;
			case '$':
				if (peek(1) == '{') {
					out.push_back(advance());
					out.push_back(advance());
					++depth;
					continue;
				}
				out.push_back(advance());   // literal $
				continue;
			case '{':
				++depth;
				out.push_back(advance());
				continue;
			case '}':
				--depth;
				if (depth == 0) {
					advance();   // consume final }
					return out;
				}
				out.push_back(advance());
				continue;
			default:
				out.push_back(advance());
				continue;
			}
		}
		error(loc_, "unterminated ${...} expansion");
		return out;
	}

	// Read body of $[...]. We have already consumed the leading "[".
	// Stops at the matching "]". Tracks bracket nesting so an array
	// subscript inside the expression doesn't close the form early.
	std::string Lexer::readBalancedBrackets() {
		std::string out;
		int depth = 1;
		while (!eof() && depth > 0) {
			const char c = peek();
			switch (c) {
			case '\\': copyBackslashEscape(out); continue;
			case '\'': copySingleQuotedRun(out); continue;
			case '"':  copyDoubleQuotedRun(out); continue;
			case '[':
				++depth;
				out.push_back(advance());
				continue;
			case ']':
				--depth;
				if (depth == 0) {
					advance();   // consume final ]
					return out;
				}
				out.push_back(advance());
				continue;
			default:
				out.push_back(advance());
				continue;
			}
		}
		error(loc_, "unterminated $[...] expansion");
		return out;
	}

	// Read body of $((...)). We have already consumed the leading "((".
	// Stops at the matching "))". Allows nested "((..))" for grouping.
	std::string Lexer::readBalancedDoubleParens() {
		std::string out;
		int depth = 1;
		while (!eof() && depth > 0) {
			const char c = peek();
			switch (c) {
			case '(':
				++depth;
				out.push_back(advance());
				continue;
			case ')':
				if (peek(1) == ')' && depth == 1) {
					advance(); advance();   // consume "))"
					return out;
				}
				--depth;
				out.push_back(advance());
				continue;
			case '\\': copyBackslashEscape(out); continue;
			case '\'': copySingleQuotedRun(out); continue;
			case '"':  copyDoubleQuotedRun(out); continue;
			default:
				out.push_back(advance());
				continue;
			}
		}
		error(loc_, "unterminated $((...)) expansion");
		return out;
	}

	// ---------------------------------------------------------------------------
	// Heredocs
	// ---------------------------------------------------------------------------

	void Lexer::collectHeredocBodies() {
		// pending_heredocs_ is FIFO. Each iteration consumes lines from src_
		// up to and including the line equal to the delimiter.
		while (!pending_heredocs_.empty()) {
			auto ph = pending_heredocs_.front();
			pending_heredocs_.erase(pending_heredocs_.begin());
			Token& delim = tokens_[ph.delim_token_index];
			std::string delim_str = flattenDelim(delim);
			std::string body;
			while (true) {
				// Read one line up to (but not including) newline / EOF.
				std::string line;
				while (!eof() && peek() != '\n') {
					line.push_back(advance());
				}
				// Apply <<- tab stripping for delimiter check and body.
				std::string check = line;
				std::string emitted = line;
				if (delim.heredoc_strip_tabs) {
					std::size_t p = 0;
					while (p < check.size() && check[p] == '\t') ++p;
					check.erase(0, p);
					std::size_t p2 = 0;
					while (p2 < emitted.size() && emitted[p2] == '\t') ++p2;
					emitted.erase(0, p2);
				}
				if (check == delim_str) {
					if (!eof()) advance();   // consume newline after delimiter
					break;
				}
				if (eof()) {
					error(delim.loc, "unterminated heredoc body");
					break;
				}
				body += emitted;
				body.push_back('\n');
				advance();   // consume newline
			}
			delim.heredoc_body = std::move(body);
		}
	}

	void Lexer::recordHeredoc(Token& /*delim*/, bool /*strip_tabs*/) {
		// Currently inlined into scanWord. Kept for future refactor.
	}

}  // namespace wbsh

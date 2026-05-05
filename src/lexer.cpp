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

	static bool isNameStart(char c) { return c == '_' || std::isalpha(static_cast<unsigned char>(c)); }
	static bool isNameCont(char c) { return c == '_' || std::isalnum(static_cast<unsigned char>(c)); }

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

	void Lexer::scanOperator() {
		SourceLoc start = loc_;
		char c = peek();
		Token tok;
		tok.loc = start;

		auto finish = [&](TokKind k, const char* text) {
			tok.kind = k;
			tok.text = text;
			tokens_.push_back(std::move(tok));
			if (k == TokKind::Newline) {
				// Heredoc bodies (if any pending) immediately follow the newline.
				collectHeredocBodies();
				at_line_start_ = true;
			}
			else {
				at_line_start_ = false;
			}
			};

		switch (c) {
		case '\n':
			advance();
			finish(TokKind::Newline, "\n");
			return;
		case '&':
			advance();
			if (match('&'))      finish(TokKind::AndIf, "&&");
			else if (match('>')) {
				if (match('>'))  finish(TokKind::AmpDGreat, "&>>");
				else             finish(TokKind::AmpGreat, "&>");
			}
			else                 finish(TokKind::Amp, "&");
			return;
		case '|':
			advance();
			if (match('|'))      finish(TokKind::OrIf, "||");
			else if (match('&')) finish(TokKind::PipeAmp, "|&");
			else                 finish(TokKind::Pipe, "|");
			return;
		case ';':
			advance();
			if (match(';')) {
				if (match('&')) finish(TokKind::DSemiAmp, ";;&");
				else            finish(TokKind::DSemi, ";;");
			}
			else if (match('&')) finish(TokKind::SemiAmp, ";&");
			else                   finish(TokKind::Semi, ";");
			return;
		case '<':
			advance();
			if (match('<')) {
				if (match('-'))      finish(TokKind::DLessDash, "<<-");
				else if (match('<')) finish(TokKind::TLess, "<<<");
				else                 finish(TokKind::DLess, "<<");
			}
			else if (match('&')) finish(TokKind::LessAnd, "<&");
			else if (match('>')) finish(TokKind::LessGreat, "<>");
			else                 finish(TokKind::Less, "<");
			return;
		case '>':
			advance();
			if (match('>'))      finish(TokKind::DGreat, ">>");
			else if (match('&')) finish(TokKind::GreatAnd, ">&");
			else if (match('|')) finish(TokKind::Clobber, ">|");
			else                 finish(TokKind::Great, ">");
			return;
		case '(':
			advance();
			finish(TokKind::LParen, "(");
			return;
		case ')':
			advance();
			finish(TokKind::RParen, ")");
			return;
		default:
			// Should not get here.
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
		if (c == '\\') {
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
		if (c == '\'') {
			WordSegment s; readSingleQuoted(s);
			out.push_back(std::move(s));
			return true;
		}
		if (c == '"') {
			WordSegment s; readDoubleQuoted(s);
			out.push_back(std::move(s));
			return true;
		}
		if (c == '$') {
			readDollar(out);
			return true;
		}
		if (c == '`') {
			WordSegment s; readBacktick(s);
			out.push_back(std::move(s));
			return true;
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

	void Lexer::readDollar(std::vector<WordSegment>& out) {
		// assumes peek() == '$'
		SourceLoc start = loc_;
		char n1 = peek(1);
		if (n1 == '{') {
			advance(); advance();   // consume "${"
			WordSegment s;
			s.kind = WordSegment::Kind::ParamExp;
			s.text = readBalancedBraces();
			out.push_back(std::move(s));
			return;
		}
		if (n1 == '(') {
			advance();              // consume "$"
			readDollarParen(out);
			return;
		}
		if (n1 == '\'') {
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
			if (eof()) {
				error(start, "unterminated $'...' string");
			}
			else {
				advance();   // closing '
			}
			out.push_back(std::move(s));
			return;
		}
		if (n1 == '"') {
			// $" is bash's localized string. Treat the $ as literal for now.
			WordSegment lit; lit.kind = WordSegment::Kind::Literal; lit.text = "$";
			advance();   // consume $
			out.push_back(std::move(lit));
			return;
		}
		if (isNameStart(n1) || n1 == '?' || n1 == '#' || n1 == '@' || n1 == '*' ||
			n1 == '$' || n1 == '!' || n1 == '-' || n1 == '_' ||
			std::isdigit(static_cast<unsigned char>(n1))) {
			advance();   // consume $
			WordSegment s; s.kind = WordSegment::Kind::SimpleVar;
			if (isNameStart(n1)) {
				while (!eof() && isNameCont(peek())) s.text.push_back(advance());
			}
			else if (std::isdigit(static_cast<unsigned char>(n1))) {
				// $0..$9 — single digit only.
				s.text.push_back(advance());
			}
			else {
				// single special parameter
				s.text.push_back(advance());
			}
			out.push_back(std::move(s));
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

	// Read until matching ')'. Respects single-/double-quoted strings, escapes,
	// and nested $(...) / `...` constructs. Excludes the closing ')'.
	std::string Lexer::readBalancedParens() {
		std::string out;
		int depth = 1;
		while (!eof() && depth > 0) {
			char c = peek();
			if (c == '\\') {
				out.push_back(advance());
				if (!eof()) out.push_back(advance());
				continue;
			}
			if (c == '\'') {
				out.push_back(advance());
				while (!eof() && peek() != '\'') out.push_back(advance());
				if (!eof()) out.push_back(advance());
				continue;
			}
			if (c == '"') {
				out.push_back(advance());
				while (!eof() && peek() != '"') {
					if (peek() == '\\') {
						out.push_back(advance());
						if (!eof()) out.push_back(advance());
						continue;
					}
					out.push_back(advance());
				}
				if (!eof()) out.push_back(advance());
				continue;
			}
			if (c == '`') {
				out.push_back(advance());
				while (!eof() && peek() != '`') {
					if (peek() == '\\') {
						out.push_back(advance());
						if (!eof()) out.push_back(advance());
						continue;
					}
					out.push_back(advance());
				}
				if (!eof()) out.push_back(advance());
				continue;
			}
			if (c == '$' && peek(1) == '(') {
				// Nested $(...) or $((...))
				out.push_back(advance());   // $
				out.push_back(advance());   // (
				if (peek() == '(') {
					out.push_back(advance());   // second (
					int dep2 = 1;
					while (!eof() && dep2 > 0) {
						if (peek() == '(') dep2++;
						else if (peek() == ')') dep2--;
						out.push_back(advance());
						if (dep2 == 0 && peek() == ')') {
							out.push_back(advance());
							break;
						}
					}
				}
				else {
					depth++;
				}
				continue;
			}
			if (c == '(') {
				depth++;
				out.push_back(advance());
				continue;
			}
			if (c == ')') {
				depth--;
				if (depth == 0) {
					advance();   // consume final )
					return out;
				}
				out.push_back(advance());
				continue;
			}
			out.push_back(advance());
		}
		// Unterminated.
		error(loc_, "unterminated $(...) substitution");
		return out;
	}

	// Read until matching '}'. Respects strings, escapes, and nested ${...}.
	std::string Lexer::readBalancedBraces() {
		std::string out;
		int depth = 1;
		while (!eof() && depth > 0) {
			char c = peek();
			if (c == '\\') {
				out.push_back(advance());
				if (!eof()) out.push_back(advance());
				continue;
			}
			if (c == '\'') {
				out.push_back(advance());
				while (!eof() && peek() != '\'') out.push_back(advance());
				if (!eof()) out.push_back(advance());
				continue;
			}
			if (c == '"') {
				out.push_back(advance());
				while (!eof() && peek() != '"') {
					if (peek() == '\\') {
						out.push_back(advance());
						if (!eof()) out.push_back(advance());
						continue;
					}
					out.push_back(advance());
				}
				if (!eof()) out.push_back(advance());
				continue;
			}
			if (c == '$' && peek(1) == '{') {
				out.push_back(advance());
				out.push_back(advance());
				depth++;
				continue;
			}
			if (c == '{') {
				depth++;
				out.push_back(advance());
				continue;
			}
			if (c == '}') {
				depth--;
				if (depth == 0) {
					advance();   // consume final }
					return out;
				}
				out.push_back(advance());
				continue;
			}
			out.push_back(advance());
		}
		error(loc_, "unterminated ${...} expansion");
		return out;
	}

	// Read body of $((...)). We have already consumed the leading "((".
	// Stops at the matching "))". Allows nested "((..))" for grouping.
	std::string Lexer::readBalancedDoubleParens() {
		std::string out;
		int depth = 1;
		while (!eof() && depth > 0) {
			char c = peek();
			if (c == '(') {
				depth++;
				out.push_back(advance());
				continue;
			}
			if (c == ')') {
				if (peek(1) == ')' && depth == 1) {
					advance(); advance();   // consume "))"
					return out;
				}
				depth--;
				out.push_back(advance());
				continue;
			}
			if (c == '\\') {
				out.push_back(advance());
				if (!eof()) out.push_back(advance());
				continue;
			}
			if (c == '\'') {
				out.push_back(advance());
				while (!eof() && peek() != '\'') out.push_back(advance());
				if (!eof()) out.push_back(advance());
				continue;
			}
			if (c == '"') {
				out.push_back(advance());
				while (!eof() && peek() != '"') {
					if (peek() == '\\') {
						out.push_back(advance());
						if (!eof()) out.push_back(advance());
						continue;
					}
					out.push_back(advance());
				}
				if (!eof()) out.push_back(advance());
				continue;
			}
			out.push_back(advance());
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

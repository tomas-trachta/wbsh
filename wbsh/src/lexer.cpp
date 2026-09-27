/**
 * @file lexer.cpp
 * @brief POSIX shell tokenizer.
 */

#include "lexer.h"

#include <cctype>
#include <utility>

namespace wbsh {

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
		case TokKind::DArithCmd: return "((";
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

	static bool isBlank(char c) {
		return c == ' ' || c == '\t';
	}

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

	static bool isDigit(char c) {
		return std::isdigit(static_cast<unsigned char>(c)) != 0;
	}

	static bool isSegmentStart(char c) {
		return c == '\\' || c == '\'' || c == '"' || c == '$' || c == '`';
	}

	static bool endsWord(char c) {
		return isBlank(c) || c == '\n' || isOperatorStart(c);
	}

	static WordSegment segmentOf(WordSegment::Kind kind, std::string text) {
		WordSegment segment;
		segment.kind = kind;
		segment.text = std::move(text);
		return segment;
	}

	static WordSegment literalSegment(std::string text) {
		return segmentOf(WordSegment::Kind::Literal, std::move(text));
	}

	static std::string flattenDelim(const Token& delim) {
		std::string out;
		for (const auto& segment : delim.segments) {
			if (segment.kind != WordSegment::Kind::DoubleQuoted) {
				out += segment.text;
				continue;
			}

			for (const auto& inner : segment.nested) out += inner.text;
		}

		return out;
	}

	static bool delimQuoted(const Token& delim) {
		for (const auto& segment : delim.segments) {
			switch (segment.kind) {
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

	static bool isDigitsOnly(const std::vector<WordSegment>& segments) {
		if (segments.empty()) return false;

		for (const auto& segment : segments) {
			if (segment.kind != WordSegment::Kind::Literal) return false;
			for (char c : segment.text) {
				if (!isDigit(c)) return false;
			}
		}

		return true;
	}

	static void stripLeadingTabs(std::string& line) {
		std::size_t count = 0;
		while (count < line.size() && line[count] == '\t') ++count;
		line.erase(0, count);
	}

	Lexer::Lexer(std::string input) : src_(std::move(input)) {}

	std::vector<Token> Lexer::tokenize() {
		while (!eof()) scanToken();

		Token end;
		end.kind = TokKind::EndOfInput;
		end.loc = loc_;
		tokens_.push_back(std::move(end));
		return std::move(tokens_);
	}

	bool Lexer::eof() const {
		return pos_ >= src_.size();
	}

	char Lexer::peek(std::size_t n) const {
		if (pos_ + n >= src_.size()) return '\0';
		return src_[pos_ + n];
	}

	char Lexer::advance() {
		const char c = src_[pos_++];
		loc_.offset++;
		if (c == '\n') {
			loc_.line++;
			loc_.column = 1;
			return c;
		}

		loc_.column++;
		return c;
	}

	bool Lexer::match(char c) {
		if (peek() != c) return false;

		advance();
		return true;
	}

	void Lexer::skipLineContinuations() {
		while (peek() == '\\' && peek(1) == '\n') {
			advance();
			advance();
		}
	}

	void Lexer::error(const SourceLoc& at, std::string msg) {
		errors_.push_back({ at, std::move(msg) });
	}

	void Lexer::skipWhitespace() {
		while (!eof()) {
			skipLineContinuations();
			if (!isBlank(peek())) break;

			advance();
		}
	}

	void Lexer::skipComment() {
		while (!eof() && peek() != '\n') advance();
	}

	void Lexer::scanToken() {
		skipWhitespace();
		if (eof()) return;

		const char c = peek();
		if (c == '#') {
			skipComment();
			return;
		}

		if ((c == '<' || c == '>') && peek(1) == '(') {
			scanProcessSubstitution();
			return;
		}

		if (c == '\n' || isOperatorStart(c)) {
			scanOperator();
			return;
		}

		scanWord();
	}

	void Lexer::scanProcessSubstitution() {
		Token word;
		word.kind = TokKind::Word;
		word.loc = loc_;
		word.first_on_line = at_line_start_;
		at_line_start_ = false;

		const std::size_t start_pos = pos_;
		const char direction = advance();
		advance();

		WordSegment segment;
		segment.kind = WordSegment::Kind::ProcSubst;
		segment.text = readBalancedParens();
		segment.proc_dir = direction;
		word.segments.push_back(std::move(segment));

		word.text = src_.substr(start_pos, pos_ - start_pos);
		tokens_.push_back(std::move(word));
	}

	void Lexer::emitOperator(SourceLoc start, TokKind kind, const char* text) {
		Token token;
		token.loc = start;
		token.kind = kind;
		token.text = text;
		tokens_.push_back(std::move(token));

		if (kind != TokKind::Newline) {
			at_line_start_ = false;
			return;
		}

		collectHeredocBodies();
		at_line_start_ = true;
	}

	void Lexer::scanAmpRun(SourceLoc start) {
		advance();
		if (match('&')) {
			emitOperator(start, TokKind::AndIf, "&&");
			return;
		}

		if (!match('>')) {
			emitOperator(start, TokKind::Amp, "&");
			return;
		}

		if (match('>')) {
			emitOperator(start, TokKind::AmpDGreat, "&>>");
			return;
		}

		emitOperator(start, TokKind::AmpGreat, "&>");
	}

	void Lexer::scanSemiRun(SourceLoc start) {
		advance();
		if (match(';')) {
			if (match('&')) {
				emitOperator(start, TokKind::DSemiAmp, ";;&");
				return;
			}

			emitOperator(start, TokKind::DSemi, ";;");
			return;
		}

		if (match('&')) {
			emitOperator(start, TokKind::SemiAmp, ";&");
			return;
		}

		emitOperator(start, TokKind::Semi, ";");
	}

	void Lexer::scanLessRun(SourceLoc start) {
		advance();
		if (match('<')) {
			if (match('-')) {
				emitOperator(start, TokKind::DLessDash, "<<-");
				return;
			}

			if (match('<')) {
				emitOperator(start, TokKind::TLess, "<<<");
				return;
			}

			emitOperator(start, TokKind::DLess, "<<");
			return;
		}

		if (match('&')) {
			emitOperator(start, TokKind::LessAnd, "<&");
			return;
		}

		if (match('>')) {
			emitOperator(start, TokKind::LessGreat, "<>");
			return;
		}

		emitOperator(start, TokKind::Less, "<");
	}

	void Lexer::scanGreatRun(SourceLoc start) {
		advance();
		if (match('>')) { emitOperator(start, TokKind::DGreat, ">>");   return; }
		if (match('&')) { emitOperator(start, TokKind::GreatAnd, ">&"); return; }
		if (match('|')) { emitOperator(start, TokKind::Clobber, ">|");  return; }
		emitOperator(start, TokKind::Great, ">");
	}

	void Lexer::scanArithCommand(SourceLoc start) {
		advance();
		advance();

		Token token;
		token.kind = TokKind::DArithCmd;
		token.loc = start;
		token.first_on_line = at_line_start_;
		at_line_start_ = false;
		token.text = readBalancedDoubleParens();
		tokens_.push_back(std::move(token));
	}

	void Lexer::scanOperator() {
		const SourceLoc start = loc_;
		const char c = peek();
		switch (c) {
		case '\n':
			advance();
			emitOperator(start, TokKind::Newline, "\n");
			return;
		case '&':
			scanAmpRun(start);
			return;
		case '|':
			advance();
			if (match('|'))      emitOperator(start, TokKind::OrIf, "||");
			else if (match('&')) emitOperator(start, TokKind::PipeAmp, "|&");
			else                 emitOperator(start, TokKind::Pipe, "|");
			return;
		case ';':
			scanSemiRun(start);
			return;
		case '<':
			scanLessRun(start);
			return;
		case '>':
			scanGreatRun(start);
			return;
		case '(':
			if (peek(1) == '(') {
				scanArithCommand(start);
				return;
			}

			advance();
			emitOperator(start, TokKind::LParen, "(");
			return;
		case ')':
			advance();
			emitOperator(start, TokKind::RParen, ")");
			return;
		default:
			error(start, std::string("unexpected character '") + c + "'");
			advance();
			return;
		}
	}

	void Lexer::scanWord() {
		const std::size_t start_pos = pos_;
		Token word;
		word.kind = TokKind::Word;
		word.loc = loc_;
		word.first_on_line = at_line_start_;
		at_line_start_ = false;

		while (!eof()) {
			skipLineContinuations();
			if (eof()) break;
			if (endsWord(peek())) break;
			if (!readWordSegment(word.segments)) break;
		}

		word.text = src_.substr(start_pos, pos_ - start_pos);
		if (isDigitsOnly(word.segments) && (peek() == '<' || peek() == '>')) {
			word.kind = TokKind::IoNumber;
		}

		markHeredocDelimiter(word);
		tokens_.push_back(std::move(word));
		if (tokens_.back().is_heredoc_delim) pending_heredocs_.push_back({ tokens_.size() - 1 });
	}

	void Lexer::markHeredocDelimiter(Token& word) const {
		if (tokens_.empty()) return;

		const TokKind previous = tokens_.back().kind;
		if (previous != TokKind::DLess && previous != TokKind::DLessDash) return;

		word.is_heredoc_delim = true;
		word.heredoc_strip_tabs = (previous == TokKind::DLessDash);
		word.heredoc_quoted = delimQuoted(word);
	}

	bool Lexer::readWordSegment(std::vector<WordSegment>& out) {
		switch (peek()) {
		case '\\':
			readEscapedChar(out);
			return true;
		case '\'':
			out.push_back(readSingleQuoted());
			return true;
		case '"':
			out.push_back(readDoubleQuoted());
			return true;
		case '$':
			readDollar(out);
			return true;
		case '`':
			out.push_back(readBacktick());
			return true;
		default:
			return readLiteralRun(out);
		}
	}

	void Lexer::readEscapedChar(std::vector<WordSegment>& out) {
		advance();
		if (eof()) {
			out.push_back(literalSegment("\\"));
			return;
		}

		out.push_back(segmentOf(WordSegment::Kind::Escaped, std::string(1, advance())));
	}

	bool Lexer::readLiteralRun(std::vector<WordSegment>& out) {
		std::string text;
		while (!eof()) {
			skipLineContinuations();
			if (eof()) break;

			const char c = peek();
			if (isSegmentStart(c) || endsWord(c)) break;

			text.push_back(advance());
		}

		if (text.empty()) return false;

		out.push_back(literalSegment(std::move(text)));
		return true;
	}

	WordSegment Lexer::readSingleQuoted() {
		const SourceLoc start = loc_;
		advance();

		WordSegment segment;
		segment.kind = WordSegment::Kind::SingleQuoted;
		while (!eof() && peek() != '\'') segment.text.push_back(advance());

		if (eof()) {
			error(start, "unterminated single-quoted string");
			return segment;
		}

		advance();
		return segment;
	}

	WordSegment Lexer::readDoubleQuoted() {
		const SourceLoc start = loc_;
		advance();

		WordSegment segment;
		segment.kind = WordSegment::Kind::DoubleQuoted;
		while (!eof() && peek() != '"') {
			const char c = peek();
			if (c == '\\') {
				advance();
				if (eof()) break;

				readDoubleQuotedEscape(segment);
				continue;
			}

			if (c == '$') {
				readDollar(segment.nested);
				continue;
			}

			if (c == '`') {
				segment.nested.push_back(readBacktick());
				continue;
			}

			const bool literal_open = !segment.nested.empty()
				&& segment.nested.back().kind == WordSegment::Kind::Literal;
			if (!literal_open) segment.nested.push_back(literalSegment(""));

			segment.nested.back().text.push_back(advance());
		}

		if (eof()) {
			error(start, "unterminated double-quoted string");
			return segment;
		}

		advance();
		return segment;
	}

	void Lexer::readDoubleQuotedEscape(WordSegment& seg) {
		const char next = peek();
		if (next == '\n') {
			advance();
			return;
		}

		if (next == '$' || next == '`' || next == '"' || next == '\\') {
			const std::string escaped(1, advance());
			seg.nested.push_back(segmentOf(WordSegment::Kind::Escaped, escaped));
			return;
		}

		seg.nested.push_back(literalSegment("\\"));
	}

	void Lexer::readDollarSingleQuoted(SourceLoc start, std::vector<WordSegment>& out) {
		advance();
		advance();

		WordSegment segment;
		segment.kind = WordSegment::Kind::DollarSingle;
		while (!eof() && peek() != '\'') {
			if (peek() == '\\' && peek(1) != '\0') {
				segment.text.push_back(advance());
				segment.text.push_back(advance());
				continue;
			}

			segment.text.push_back(advance());
		}

		if (eof()) error(start, "unterminated $'...' string");
		else advance();

		out.push_back(std::move(segment));
	}

	static bool isSimpleVarStart(char n1) {
		return isNameStart(n1)
		    || n1 == '?' || n1 == '#' || n1 == '@' || n1 == '*'
		    || n1 == '$' || n1 == '!' || n1 == '-' || n1 == '_'
		    || isDigit(n1);
	}

	void Lexer::readSimpleDollarVar(char n1, std::vector<WordSegment>& out) {
		advance();

		WordSegment segment;
		segment.kind = WordSegment::Kind::SimpleVar;
		if (isNameStart(n1)) {
			while (!eof() && isNameCont(peek())) segment.text.push_back(advance());
		} else {
			segment.text.push_back(advance());
		}

		out.push_back(std::move(segment));
	}

	void Lexer::readDollar(std::vector<WordSegment>& out) {
		const SourceLoc start = loc_;
		const char n1 = peek(1);
		switch (n1) {
		case '{':
			advance();
			advance();
			out.push_back(segmentOf(WordSegment::Kind::ParamExp, readBalancedBraces()));
			return;
		case '(':
			advance();
			readDollarParen(out);
			return;
		case '[':
			advance();
			advance();
			out.push_back(segmentOf(WordSegment::Kind::ArithExp, readBalancedBrackets()));
			return;
		case '\'':
			readDollarSingleQuoted(start, out);
			return;
		case '"':
			// $" is bash's localized string. Treat the $ as literal for now.
			advance();
			out.push_back(literalSegment("$"));
			return;
		default:
			break;
		}

		if (isSimpleVarStart(n1)) {
			readSimpleDollarVar(n1, out);
			return;
		}

		advance();
		out.push_back(literalSegment("$"));
	}

	WordSegment Lexer::readBacktick() {
		const SourceLoc start = loc_;
		advance();

		WordSegment segment;
		segment.kind = WordSegment::Kind::CmdSubst;
		while (!eof() && peek() != '`') {
			if (peek() != '\\') {
				segment.text.push_back(advance());
				continue;
			}

			advance();
			if (eof()) break;

			const char next = peek();
			if (next == '$' || next == '`' || next == '\\') {
				segment.text.push_back(advance());
			} else {
				segment.text.push_back('\\');
			}
		}

		if (eof()) {
			error(start, "unterminated backquote command substitution");
			return segment;
		}

		advance();
		return segment;
	}

	void Lexer::readDollarParen(std::vector<WordSegment>& out) {
		advance();
		if (peek() == '(') {
			advance();
			out.push_back(segmentOf(WordSegment::Kind::ArithExp, readBalancedDoubleParens()));
			return;
		}

		out.push_back(segmentOf(WordSegment::Kind::CmdSubst, readBalancedParens()));
	}

	void Lexer::copyBackslashEscape(std::string& out) {
		out.push_back(advance());
		if (!eof()) out.push_back(advance());
	}

	void Lexer::copySingleQuotedRun(std::string& out) {
		out.push_back(advance());
		while (!eof() && peek() != '\'') out.push_back(advance());
		if (!eof()) out.push_back(advance());
	}

	void Lexer::copyDoubleQuotedRun(std::string& out) {
		out.push_back(advance());
		while (!eof() && peek() != '"') {
			if (peek() == '\\') {
				copyBackslashEscape(out);
				continue;
			}

			out.push_back(advance());
		}

		if (!eof()) out.push_back(advance());
	}

	void Lexer::copyBackquotedRun(std::string& out) {
		out.push_back(advance());
		while (!eof() && peek() != '`') {
			if (peek() == '\\') {
				copyBackslashEscape(out);
				continue;
			}

			out.push_back(advance());
		}

		if (!eof()) out.push_back(advance());
	}

	void Lexer::copyDollarParenRun(std::string& out, int& paren_depth) {
		out.push_back(advance());
		out.push_back(advance());
		if (peek() != '(') {
			++paren_depth;
			return;
		}

		out.push_back(advance());
		int inner_depth = 1;
		while (!eof() && inner_depth > 0) {
			if (peek() == '(') ++inner_depth;
			else if (peek() == ')') --inner_depth;

			out.push_back(advance());
			if (inner_depth == 0 && peek() == ')') {
				out.push_back(advance());
				break;
			}
		}
	}

	std::string Lexer::readBalancedParens() {
		std::string out;
		int depth = 1;
		while (!eof() && depth > 0) {
			switch (peek()) {
			case '\\': copyBackslashEscape(out); continue;
			case '\'': copySingleQuotedRun(out); continue;
			case '"':  copyDoubleQuotedRun(out); continue;
			case '`':  copyBackquotedRun(out); continue;
			case '$':
				if (peek(1) == '(') {
					copyDollarParenRun(out, depth);
					continue;
				}

				out.push_back(advance());
				continue;
			case '(':
				++depth;
				out.push_back(advance());
				continue;
			case ')':
				--depth;
				if (depth == 0) {
					advance();
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

	std::string Lexer::readBalancedBraces() {
		std::string out;
		int depth = 1;
		while (!eof() && depth > 0) {
			switch (peek()) {
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

				out.push_back(advance());
				continue;
			case '{':
				++depth;
				out.push_back(advance());
				continue;
			case '}':
				--depth;
				if (depth == 0) {
					advance();
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

	std::string Lexer::readBalancedBrackets() {
		std::string out;
		int depth = 1;
		while (!eof() && depth > 0) {
			switch (peek()) {
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
					advance();
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

	std::string Lexer::readBalancedDoubleParens() {
		std::string out;
		int depth = 1;
		while (!eof() && depth > 0) {
			switch (peek()) {
			case '(':
				++depth;
				out.push_back(advance());
				continue;
			case ')':
				if (peek(1) == ')' && depth == 1) {
					advance();
					advance();
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

	void Lexer::collectHeredocBodies() {
		while (!pending_heredocs_.empty()) {
			const PendingHeredoc pending = pending_heredocs_.front();
			pending_heredocs_.erase(pending_heredocs_.begin());

			Token& delim = tokens_[pending.delim_token_index];
			delim.heredoc_body = readHeredocBody(delim);
		}
	}

	std::string Lexer::readHeredocBody(const Token& delim) {
		const std::string terminator = flattenDelim(delim);
		std::string body;
		for (;;) {
			std::string line = readLineText();
			if (delim.heredoc_strip_tabs) stripLeadingTabs(line);

			if (line == terminator) {
				if (!eof()) advance();
				return body;
			}

			if (eof()) {
				error(delim.loc, "unterminated heredoc body");
				return body;
			}

			body += line;
			body.push_back('\n');
			advance();
		}
	}

	std::string Lexer::readLineText() {
		std::string line;
		while (!eof() && peek() != '\n') line.push_back(advance());
		return line;
	}

}  // namespace wbsh

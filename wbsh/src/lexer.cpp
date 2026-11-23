/**
 * @file lexer.cpp
 * @brief POSIX shell tokenizer.
 */

#include "lexer.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <utility>

namespace wbsh {

	static const char* const kEofLookingForParen = "unexpected EOF while looking for matching `)'";

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

	// The lexer cannot see `shopt -s extglob`, so `?(`, `*(`, `+(`, `@(`
	// and `!(` always open a group (bash rejects them as syntax errors
	// when extglob is off), except a command-leading `!(`, which stays
	// the `!` keyword before a subshell.
	static bool isExtglobOpen(char c, char next) {
		const bool op = c == '?' || c == '*' || c == '+' || c == '@' || c == '!';
		return op && next == '(';
	}

	static bool endsRegexWord(char c) {
		return isBlank(c) || c == '\n' || c == ')' || c == '&' || c == ';' || c == '<' || c == '>';
	}

	static bool isWordSpelled(const Token& token, const char* text) {
		return token.kind == TokKind::Word && token.text == text;
	}

	static bool isCommandSeparator(TokKind kind) {
		switch (kind) {
		case TokKind::Newline:
		case TokKind::Semi:
		case TokKind::Amp:
		case TokKind::AndIf:
		case TokKind::OrIf:
		case TokKind::Pipe:
		case TokKind::PipeAmp:
		case TokKind::LParen:
		case TokKind::RParen:
		case TokKind::DSemi:
		case TokKind::SemiAmp:
		case TokKind::DSemiAmp:
			return true;
		default:
			return false;
		}
	}

	static bool isCommandPrefixWord(const Token& token) {
		static const char* const kPrefixes[] = {
			"!", "if", "then", "else", "elif", "do", "while", "until", "{", "time", "coproc",
		};
		for (const char* prefix : kPrefixes) {
			if (isWordSpelled(token, prefix)) return true;
		}

		return false;
	}

	static void appendLiteralChar(std::vector<WordSegment>& out, char c) {
		if (out.empty() || out.back().kind != WordSegment::Kind::Literal) {
			out.push_back(WordSegment{});
		}

		out.back().text.push_back(c);
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

	static bool escapesInDelimQuotes(char c) {
		return c == '$' || c == '`' || c == '"' || c == '\\' || c == '\n';
	}

	static bool atLineContinuation(const std::string& raw, std::size_t k) {
		return raw[k] == '\\' && k + 1 < raw.size() && raw[k + 1] == '\n';
	}

	static void copyQuotedDelimRun(const std::string& raw, std::size_t& k, std::string& out) {
		const char quote = raw[k++];
		while (k < raw.size() && raw[k] != quote) {
			if (quote == '"' && atLineContinuation(raw, k)) {
				k += 2;
				continue;
			}

			const bool escape = quote == '"' && raw[k] == '\\' && k + 1 < raw.size()
				&& escapesInDelimQuotes(raw[k + 1]);
			if (escape) ++k;
			out.push_back(raw[k++]);
		}
	}

	// The delimiter is the word after quote removal and nothing else: bash
	// expands nothing in it, so `` EO`true`F `` is matched as written.
	static std::string flattenDelim(const Token& delim) {
		const std::string& raw = delim.text;
		std::string out;
		for (std::size_t k = 0; k < raw.size(); ++k) {
			const char c = raw[k];
			const bool quote_follows = k + 1 < raw.size() && (raw[k + 1] == '\'' || raw[k + 1] == '"');
			if (atLineContinuation(raw, k)) {
				++k;
			} else if (c == '\\' && k + 1 < raw.size()) {
				out.push_back(raw[++k]);
			} else if (c == '\'' || c == '"') {
				copyQuotedDelimRun(raw, k, out);
			} else if (c != '$' || !quote_follows) {
				out.push_back(c);
			}
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

	// bash takes a number that overflows an int for an ordinary word.
	static bool fitsInInt(const std::string& digits) {
		static const long long kIntMax = 2147483647;

		long long value = 0;
		for (const char c : digits) {
			value = value * 10 + (c - '0');
			if (value > kIntMax) return false;
		}

		return true;
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

	// `{name}` or `{name[subscript]}` right before a redirection operator:
	// the shell picks the descriptor and stores it in the variable.
	static bool isFdVariableWord(const std::string& text) {
		if (text.size() < 3 || text.front() != '{' || text.back() != '}') return false;
		if (!isNameStart(text[1])) return false;

		std::size_t i = 2;
		while (i + 1 < text.size() && isNameCont(text[i])) ++i;
		if (i + 1 == text.size()) return true;

		return text[i] == '[' && text[text.size() - 2] == ']';
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
			if (pos_ > same_line_end_ && !insideAlias(pos_ - 1)) loc_.line++;
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

		if (atProcessSubstitution()) {
			scanWord();
			return;
		}

		const bool regex_word = atRegexOperand() && !endsRegexWord(c);
		if (!regex_word && (c == '\n' || isOperatorStart(c))) {
			scanOperator();
			return;
		}

		scanWord();
	}

	// `<(cmd)` and `>(cmd)` are part of the word they are in, as in bash:
	// `a<(x)b`, `x=<(cmd)` and `--file=<(cmd)` are each one word.
	bool Lexer::atProcessSubstitution() const {
		return (peek() == '<' || peek() == '>') && peek(1) == '(';
	}

	void Lexer::readProcessSubstitution(std::vector<WordSegment>& out) {
		WordSegment segment;
		segment.kind = WordSegment::Kind::ProcSubst;
		segment.proc_dir = advance();
		advance();
		segment.text = readCommandSubstitutionBody();
		out.push_back(std::move(segment));
	}

	void Lexer::emitOperator(SourceLoc start, TokKind kind, const char* text) {
		Token token;
		token.loc = start;
		token.kind = kind;
		token.text = text;
		tokens_.push_back(std::move(token));
		trackNestedParen(kind);
		trackCaseOperator(kind);

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

		std::string body;
		const ArithScan scan = tryArithmeticAfterParen(body);
		if (scan == ArithScan::NotArithmetic) {
			emitOperator(start, TokKind::LParen, "(");
			return;
		}

		if (scan == ArithScan::Unterminated) error(start, kEofLookingForParen);

		Token token;
		token.kind = TokKind::DArithCmd;
		token.loc = start;
		token.first_on_line = at_line_start_;
		at_line_start_ = false;
		token.text = std::move(body);
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
		const bool command_start = atCommandWordStart();
		bang_is_keyword_ = command_start && !inCasePattern();
		Token word;
		word.kind = TokKind::Word;
		word.loc = loc_;
		word.first_on_line = at_line_start_;
		at_line_start_ = false;

		if (atRegexOperand()) scanRegexSegments(word);
		else scanWordSegments(word);

		word.text = src_.substr(start_pos, pos_ - start_pos);
		if (expandsAlias(word, start_pos, command_start)) return;

		const bool fd_number = isDigitsOnly(word.segments) && fitsInInt(word.text);
		const bool names_fd = fd_number || isFdVariableWord(word.text);
		if (names_fd && (peek() == '<' || peek() == '>')) word.kind = TokKind::IoNumber;

		markHeredocDelimiter(word);
		trackDBracket(word, command_start);
		trackCaseWord(word, command_start);
		tokens_.push_back(std::move(word));
		if (tokens_.back().is_heredoc_delim) queueHeredoc();
	}

	// POSIX has reserved words recognized before aliases.
	static bool isReservedWordText(const std::string& text) {
		static const char* const kReserved[] = {
			"!", "case", "coproc", "do", "done", "elif", "else", "esac", "fi", "for", "function",
			"if", "in", "select", "then", "time", "until", "while", "{", "}", "[[", "]]",
		};

		for (const char* reserved : kReserved) {
			if (text == reserved) return true;
		}

		return false;
	}

	// A plain word, quoted nowhere, in command position, after an alias
	// whose text ends in a blank, or first in an alias's text. An alias
	// never expands inside its own text.
	bool Lexer::expandsAlias(const Token& word, std::size_t start_pos, bool command_start) {
		const bool after_blank_alias = blank_alias_end_ != std::string::npos
			&& start_pos >= blank_alias_end_;
		if (after_blank_alias) blank_alias_end_ = std::string::npos;
		const bool first_in_alias = start_pos == alias_restart_;
		alias_restart_ = std::string::npos;
		if (aliases_ == nullptr || inCasePattern()) return false;

		const bool candidate = command_start || after_blank_alias || first_in_alias;
		if (!candidate && !atAliasPosition()) return false;
		if (word.segments.size() != 1 || word.segments[0].kind != WordSegment::Kind::Literal) {
			return false;
		}

		const auto alias = aliases_->find(word.text);
		if (word.segments[0].text != word.text || alias == aliases_->end()) return false;
		if (insideAliasOf(word.text, start_pos)) return false;
		if (posix_mode_ && command_start && isReservedWordText(word.text)) return false;

		spliceAlias(word, start_pos, alias->second);
		return true;
	}

	bool Lexer::insideAliasOf(const std::string& name, std::size_t pos) const {
		for (const AliasSplice& splice : splices_) {
			if (splice.name == name && pos >= splice.at && pos < splice.at + splice.length) return true;
		}

		return false;
	}

	bool Lexer::insideAlias(std::size_t pos) const {
		for (const AliasSplice& splice : splices_) {
			if (pos >= splice.at && pos < splice.at + splice.length) return true;
		}

		return false;
	}

	static bool endsInsideQuotes(const std::string& text) {
		char quote = '\0';
		for (std::size_t i = 0; i < text.size(); ++i) {
			const char c = text[i];
			if (quote == '\'') {
				if (c == '\'') quote = '\0';
			} else if (c == '\\') {
				++i;
			} else if (c == '"' || (c == '\'' && quote == '\0')) {
				quote = quote == c ? '\0' : c;
			}
		}

		return quote != '\0';
	}

	// bash's shell_getc ends an alias's text with a blank, so that it is
	// a token of its own, unless the text is empty, already ends one, ends
	// in a backslash that quotes what follows, or ends inside quotes. Text
	// of more than one line may end inside a here-document, which a blank
	// would change.
	static bool needsClosingBlank(const std::string& text) {
		if (text.empty() || text.find('\n') != std::string::npos) return false;
		if (std::strchr(" \t\n\\|&;()<>", text.back()) != nullptr) return false;

		return !endsInsideQuotes(text);
	}

	// The alias text takes the name's place and is read from the start of
	// the name again; an alias it sits in grows by the difference.
	void Lexer::spliceAlias(const Token& word, std::size_t start_pos, const std::string& alias_text) {
		const std::string value = needsClosingBlank(alias_text) ? alias_text + " " : alias_text;
		const bool checks_next = !alias_text.empty() && isBlank(alias_text.back());
		const std::size_t replaced = pos_ - start_pos;
		for (AliasSplice& outer : splices_) {
			if (start_pos >= outer.at && start_pos < outer.at + outer.length) {
				outer.length = outer.length + value.size() - replaced;
			}
		}

		src_.replace(start_pos, replaced, value);
		splices_.push_back({ start_pos, value.size(), replaced, word.text });
		blank_alias_end_ = checks_next ? start_pos + value.size() : std::string::npos;
		alias_restart_ = start_pos;

		pos_ = start_pos;
		loc_ = word.loc;
		at_line_start_ = word.first_on_line;
	}

	// The aliases expanded since @p first_splice, from @p from on, go back to
	// their names.
	void Lexer::restoreAliasNames(std::size_t from, std::size_t first_splice) {
		if (splices_.size() == first_splice || closedInsideAlias(first_splice)) return;

		std::string original;
		std::size_t cursor = from;
		for (std::size_t k = first_splice; k < splices_.size(); ++k) {
			const AliasSplice& splice = splices_[k];
			if (splice.at < cursor) continue;

			original += src_.substr(cursor, splice.at - cursor) + splice.name;
			cursor = splice.at + splice.length;
		}

		original += src_.substr(cursor, pos_ - cursor);
		const std::size_t expanded = pos_ - from;
		for (std::size_t k = 0; k < first_splice; ++k) {
			AliasSplice& outer = splices_[k];
			if (from >= outer.at && from < outer.at + outer.length) {
				outer.length = outer.length + original.size() - expanded;
			}
		}

		src_.replace(from, expanded, original);
		pos_ = from + original.size();
		loc_.offset = loc_.offset + original.size() - expanded;
		splices_.resize(first_splice);
	}

	// The `)` just read came from an alias's text: the body has no text of
	// its own to go back to.
	bool Lexer::closedInsideAlias(std::size_t first_splice) const {
		for (std::size_t k = first_splice; k < splices_.size(); ++k) {
			const AliasSplice& splice = splices_[k];
			if (pos_ > splice.at && pos_ <= splice.at + splice.length) return true;
		}

		return false;
	}

	std::size_t offsetAfterAliases(const std::vector<AliasSplice>& splices, std::size_t offset) {
		std::size_t shift = 0;
		std::size_t covered_to = 0;
		for (const AliasSplice& splice : splices) {
			if (splice.at < covered_to) continue;
			if (splice.at - shift >= offset) break;

			covered_to = splice.at + splice.length;
			shift += splice.length - splice.replaced;
		}

		return offset + shift;
	}

	std::size_t offsetBeforeAliases(const std::vector<AliasSplice>& splices, std::size_t offset) {
		std::size_t shift = 0;
		std::size_t covered_to = 0;
		for (const AliasSplice& splice : splices) {
			if (splice.at < covered_to) continue;
			if (offset <= splice.at) break;

			covered_to = splice.at + splice.length;
			if (offset < covered_to) return splice.at + splice.replaced - shift;
			shift += splice.length - splice.replaced;
		}

		return offset - shift;
	}

	static const std::size_t kMaxPendingHeredocs = 16;

	// bash's HEREDOC_MAX: a 17th here-document waiting for its body is an
	// error that ends the shell.
	void Lexer::queueHeredoc() {
		if (pending_heredocs_.size() == kMaxPendingHeredocs) {
			error(tokens_.back().loc, "maximum here-document count exceeded");
		}

		pending_heredocs_.push_back({ tokens_.size() - 1 });
	}

	void Lexer::scanWordSegments(Token& word) {
		int group_depth = 0;
		while (!eof()) {
			skipLineContinuations();
			if (eof()) break;
			if (atAssignmentSubscript(word.segments) && readAssignmentSubscript(word.segments)) {
				continue;
			}

			if (!readWordPart(word.segments, group_depth)) break;
		}
	}

	// Inside an extglob group `X(...)` the operator characters, `|`
	// and blanks are word text, up to the group's closing parenthesis.
	bool Lexer::readWordPart(std::vector<WordSegment>& out, int& group_depth) {
		const char c = peek();
		if (atExtglobOpen(out.empty())) {
			appendLiteralChar(out, advance());
			appendLiteralChar(out, advance());
			++group_depth;
			return true;
		}

		if (group_depth == 0) {
			if (atProcessSubstitution()) {
				readProcessSubstitution(out);
				return true;
			}

			if (endsWord(c)) return false;
			return readWordSegment(out);
		}

		if (c == '\n') return false;
		if (isSegmentStart(c)) return readWordSegment(out);

		if (c == '(') ++group_depth;
		else if (c == ')') --group_depth;
		appendLiteralChar(out, advance());
		return true;
	}

	void Lexer::scanRegexSegments(Token& word) {
		int paren_depth = 0;
		while (!eof()) {
			skipLineContinuations();
			if (eof()) break;
			if (!readRegexPart(word.segments, paren_depth)) break;
		}
	}

	bool Lexer::readRegexPart(std::vector<WordSegment>& out, int& paren_depth) {
		const char c = peek();
		if (c == '\n') return false;
		if (paren_depth == 0 && endsRegexWord(c)) return false;
		if (isSegmentStart(c)) return readWordSegment(out);

		if (c == '(') ++paren_depth;
		else if (c == ')') --paren_depth;
		appendLiteralChar(out, advance());
		return true;
	}

	bool Lexer::atRegexOperand() const {
		return in_dbracket_ && !tokens_.empty() && isWordSpelled(tokens_.back(), "=~");
	}

	static bool isRedirectionKind(TokKind kind) {
		return kind >= TokKind::Less && kind <= TokKind::AmpDGreat;
	}

	// `name=value` or `name[sub]=value`, as a word in front of a command.
	static bool isAssignmentText(const std::string& text) {
		if (text.empty() || !(std::isalpha(static_cast<unsigned char>(text[0])) || text[0] == '_')) {
			return false;
		}

		std::size_t i = 1;
		while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_')) {
			++i;
		}

		if (i < text.size() && text[i] == '[') i = text.find(']', i);
		if (i == std::string::npos || i >= text.size()) return false;
		if (text[i] == ']') ++i;
		return text.compare(i, 1, "=") == 0 || text.compare(i, 2, "+=") == 0;
	}

	// bash checks for an alias wherever a command name may come: after
	// the redirections and assignments in front of it too.
	bool Lexer::atAliasPosition() const {
		std::size_t i = tokens_.size();
		while (i > 0) {
			const Token& previous = tokens_[i - 1];
			if (previous.kind == TokKind::Word && i >= 2 && isRedirectionKind(tokens_[i - 2].kind)) {
				i -= 2;
				if (i > 0 && tokens_[i - 1].kind == TokKind::IoNumber) --i;
				continue;
			}

			if (previous.kind != TokKind::Word || !isAssignmentText(previous.text)) break;
			--i;
		}

		if (i == 0) return true;
		return isCommandSeparator(tokens_[i - 1].kind) || isCommandPrefixWord(tokens_[i - 1]);
	}

	bool Lexer::atCommandWordStart() const {
		if (tokens_.empty()) return true;

		const Token& previous = tokens_.back();
		return isCommandSeparator(previous.kind) || isCommandPrefixWord(previous);
	}

	bool Lexer::atExtglobOpen(bool word_start) const {
		const char c = peek();
		if (!isExtglobOpen(c, peek(1))) return false;
		return c != '!' || !word_start || !bang_is_keyword_;
	}

	bool Lexer::inCasePattern() const {
		return !case_states_.empty() && case_states_.back() == CaseState::Pattern;
	}

	// In `in|esac)` and `(esac)` the word is a pattern; only an `esac`
	// where a pattern list could start ends the case.
	bool Lexer::afterPatternBar() const {
		if (tokens_.empty()) return false;

		const TokKind previous = tokens_.back().kind;
		return previous == TokKind::Pipe || previous == TokKind::LParen;
	}

	void Lexer::trackCaseWord(const Token& word, bool command_start) {
		const CaseState state = case_states_.empty() ? CaseState::Body : case_states_.back();
		switch (state) {
		case CaseState::Subject:
			case_states_.back() = CaseState::In;
			return;
		case CaseState::In:
			if (isWordSpelled(word, "in")) case_states_.back() = CaseState::Pattern;
			return;
		case CaseState::Pattern:
			if (isWordSpelled(word, "esac") && !afterPatternBar()) case_states_.pop_back();
			return;
		case CaseState::Body:
			if (!command_start) return;
			if (isWordSpelled(word, "case")) case_states_.push_back(CaseState::Subject);
			else if (isWordSpelled(word, "esac") && !case_states_.empty()) case_states_.pop_back();
			return;
		default:
			return;
		}
	}

	void Lexer::trackCaseOperator(TokKind kind) {
		if (case_states_.empty()) return;

		CaseState& state = case_states_.back();
		const bool ends_item = kind == TokKind::DSemi || kind == TokKind::SemiAmp
			|| kind == TokKind::DSemiAmp;
		if (state == CaseState::Pattern && kind == TokKind::RParen) state = CaseState::Body;
		else if (state == CaseState::Body && ends_item) state = CaseState::Pattern;
	}

	void Lexer::trackDBracket(const Token& word, bool command_start) {
		if (command_start && isWordSpelled(word, "[[")) in_dbracket_ = true;
		else if (isWordSpelled(word, "]]")) in_dbracket_ = false;
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
			if (peek(1) == '"') {
				readLocalizedString(out);
				return true;
			}

			readDollar(out);
			return true;
		case '`':
			out.push_back(readBacktick(false));
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
			const bool word_start = out.empty() && text.empty();
			if (isSegmentStart(c) || endsWord(c) || atExtglobOpen(word_start)) break;
			if (c == '[' && out.empty() && assignmentNameBeforeSubscript(text)) break;

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
			error(start, "unexpected EOF while looking for matching `''");
			return segment;
		}

		advance();
		return segment;
	}

	WordSegment Lexer::readDoubleQuoted() {
		const bool outer = in_double_quotes_;
		in_double_quotes_ = true;
		WordSegment segment = readDoubleQuotedBody();
		in_double_quotes_ = outer;
		return segment;
	}

	WordSegment Lexer::readDoubleQuotedBody() {
		const SourceLoc start = loc_;
		const std::size_t errors_before = errors_.size();
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

			if (c == '$' && peek(1) != '\'') {
				readDollar(segment.nested);
				continue;
			}

			if (c == '`') {
				segment.nested.push_back(readBacktick(true));
				continue;
			}

			const bool literal_open = !segment.nested.empty()
				&& segment.nested.back().kind == WordSegment::Kind::Literal;
			if (!literal_open) segment.nested.push_back(literalSegment(""));

			segment.nested.back().text.push_back(advance());
		}

		if (eof()) {
			if (errors_.size() == errors_before) {
				error(start, "unexpected EOF while looking for matching `\"'");
			}

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

		if (eof()) error(start, "unexpected EOF while looking for matching `''");
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

	// `$"..."` asks for a translation from the locale's message catalog;
	// with no catalogs it is the plain double-quoted string, as in bash
	// without gettext data.
	void Lexer::readLocalizedString(std::vector<WordSegment>& out) {
		advance();
		out.push_back(readDoubleQuoted());
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

	// Within backquotes a backslash quotes only `$`, a backquote and itself;
	// inside double quotes it quotes `"` as well.
	static bool backquoteEscapes(char c, bool in_double_quotes) {
		return c == '$' || c == '`' || c == '\\' || (in_double_quotes && c == '"');
	}

	// A backquote does not cut a here-document off the way `)` ends one
	// in `$(...)`: a delimiter right before it still ends it cleanly.
	static void endHeredocLine(std::string& body) {
		if (body.find("<<") == std::string::npos || body.empty() || body.back() == '\n') return;

		body.push_back('\n');
	}

	WordSegment Lexer::readBacktick(bool in_double_quotes) {
		const SourceLoc start = loc_;
		advance();

		WordSegment segment;
		segment.kind = WordSegment::Kind::CmdSubst;
		segment.backquoted = true;
		while (!eof() && peek() != '`') {
			if (peek() != '\\') {
				segment.text.push_back(advance());
				continue;
			}

			advance();
			if (eof()) break;

			if (peek() == '\n') {
				advance();
			} else if (backquoteEscapes(peek(), in_double_quotes)) {
				segment.text.push_back(advance());
			} else {
				segment.text.push_back('\\');
			}
		}

		if (eof()) {
			error(start, "unexpected EOF while looking for matching ``'");
			return segment;
		}

		endHeredocLine(segment.text);
		advance();
		return segment;
	}

	void Lexer::readDollarParen(std::vector<WordSegment>& out) {
		const SourceLoc start = loc_;
		advance();

		std::string body;
		const ArithScan scan = tryArithmeticAfterParen(body);
		if (scan == ArithScan::NotArithmetic) {
			const std::size_t errors_before = errors_.size();
			out.push_back(segmentOf(WordSegment::Kind::CmdSubst, readCommandSubstitutionBody()));
			out.back().unterminated = errors_.size() > errors_before;
			out.back().aliased = body_had_aliases_;
			return;
		}

		if (scan == ArithScan::Unterminated) error(start, kEofLookingForParen);
		out.push_back(segmentOf(WordSegment::Kind::ArithExp, std::move(body)));
	}

	Lexer::ArithScan Lexer::tryArithmeticAfterParen(std::string& out) {
		if (peek() != '(') return ArithScan::NotArithmetic;

		const std::size_t pos = pos_;
		const SourceLoc loc = loc_;
		const std::size_t error_count = errors_.size();
		advance();

		const ArithScan scan = readArithmeticBody(out);
		if (scan == ArithScan::NotArithmetic) restorePosition(pos, loc, error_count);
		return scan;
	}

	void Lexer::restorePosition(std::size_t pos, SourceLoc loc, std::size_t error_count) {
		pos_ = pos;
		loc_ = loc;
		errors_.resize(error_count);
	}

	Lexer::ArithScan Lexer::readArithmeticBody(std::string& out) {
		int depth = 1;
		while (!eof()) {
			switch (peek()) {
			case '(':
				++depth;
				out.push_back(advance());
				continue;
			case ')':
				if (depth > 1) {
					--depth;
					out.push_back(advance());
					continue;
				}

				return closeArithmeticBody();
			case '\\': copyBackslashEscape(out); continue;
			case '\'': copySingleQuotedRun(out); continue;
			case '"':  copyDoubleQuotedRun(out); continue;
			case '`':  copyBackquotedRun(out); continue;
			case '$':  copyArithmeticDollar(out); continue;
			default:
				out.push_back(advance());
				continue;
			}
		}

		return ArithScan::Unterminated;
	}

	Lexer::ArithScan Lexer::closeArithmeticBody() {
		if (peek(1) != ')') return ArithScan::NotArithmetic;

		advance();
		advance();
		return ArithScan::Closed;
	}

	// Aliases in the body count for finding its end, as in bash; the body
	// keeps its own text, for it is read again, aliases and all, as it runs.
	std::string Lexer::readCommandSubstitutionBody() {
		const SourceLoc start = loc_;
		const std::size_t body_start = pos_;
		const std::size_t splices_before = splices_.size();

		OuterScanState outer = enterNestedScan();
		++nested_scan_depth_;
		const bool closed = scanToUnmatchedParen();
		--nested_scan_depth_;
		if (!closed) warnCutOffHeredocs();
		std::vector<Token> unread;
		for (const PendingHeredoc& pending : pending_heredocs_) {
			unread.push_back(tokens_[pending.delim_token_index]);
		}

		leaveNestedScan(outer);
		if (closed && !unread.empty()) carryHeredocs(unread, src_.substr(body_start, pos_ - 1 - body_start));
		body_had_aliases_ = splices_.size() > splices_before;
		restoreAliasNames(body_start, splices_before);

		if (!closed) {
			SourceLoc past_end = loc_;
			past_end.line = lastLineReached() + 1;
			error(past_end, "unexpected EOF while looking for matching `)'");
			return src_.substr(body_start);
		}

		return src_.substr(body_start, pos_ - 1 - body_start);
	}

	void Lexer::warnCutOffHeredocs() {
		for (const Token& token : tokens_) {
			if (token.heredoc_eof_line != 0) warnHeredocAtEof(token);
		}
	}

	// Only a syntax error report shows these; a document whose command runs
	// gets its warning as it is read in.
	void Lexer::warnHeredocAtEof(const Token& delim) {
		SourceLoc at = delim.loc;
		at.line = delim.heredoc_eof_line;
		const std::size_t start = delim.heredoc_start_line != 0 ? delim.heredoc_start_line
			: delim.loc.line;
		warnings_.push_back({ at, "here-document at line " + std::to_string(start)
			+ " delimited by end-of-file (wanted `" + flattenDelim(delim) + "')" });
	}

	bool Lexer::scanToUnmatchedParen() {
		while (!eof()) {
			skipWhitespace();
			if (atUnmatchedCloseParen()) {
				advance();
				return true;
			}

			if (!eof()) scanToken();
		}

		return false;
	}

	bool Lexer::atUnmatchedCloseParen() const {
		return peek() == ')' && nested_paren_depth_ == 0 && !inCasePattern();
	}

	Lexer::OuterScanState Lexer::enterNestedScan() {
		OuterScanState outer;
		outer.tokens = std::move(tokens_);
		outer.at_line_start = at_line_start_;
		outer.in_dbracket = in_dbracket_;
		outer.case_states = std::move(case_states_);
		outer.bang_is_keyword = bang_is_keyword_;
		outer.pending_heredocs = std::move(pending_heredocs_);
		outer.paren_depth = nested_paren_depth_;
		outer.in_double_quotes = in_double_quotes_;

		tokens_.clear();
		at_line_start_ = true;
		in_dbracket_ = false;
		case_states_.clear();
		bang_is_keyword_ = false;
		pending_heredocs_.clear();
		nested_paren_depth_ = 0;
		in_double_quotes_ = false;
		return outer;
	}

	void Lexer::leaveNestedScan(OuterScanState& outer) {
		tokens_ = std::move(outer.tokens);
		at_line_start_ = outer.at_line_start;
		in_dbracket_ = outer.in_dbracket;
		case_states_ = std::move(outer.case_states);
		bang_is_keyword_ = outer.bang_is_keyword;
		pending_heredocs_ = std::move(outer.pending_heredocs);
		nested_paren_depth_ = outer.paren_depth;
		in_double_quotes_ = outer.in_double_quotes;
	}

	// A case pattern's optional `(` and its closing `)` are not grouping.
	void Lexer::trackNestedParen(TokKind kind) {
		if (inCasePattern()) return;

		if (kind == TokKind::LParen) ++nested_paren_depth_;
		else if (kind == TokKind::RParen) --nested_paren_depth_;
	}

	std::size_t Lexer::commandSubstitutionEnd(const std::string& text, std::size_t body_start) {
		Lexer lexer(text);
		lexer.pos_ = body_start;
		lexer.readCommandSubstitutionBody();
		if (!lexer.errors_.empty()) return std::string::npos;

		return lexer.pos_ - 1;
	}

	std::size_t Lexer::arithmeticEnd(const std::string& text, std::size_t body_start) {
		Lexer lexer(text);
		lexer.pos_ = body_start;

		std::string body;
		if (lexer.readArithmeticBody(body) != ArithScan::Closed) return std::string::npos;

		return lexer.pos_ - 2;
	}

	std::string Lexer::withoutArithmeticDoubleQuotes(const std::string& text) {
		Lexer lexer(text);
		std::string out;
		while (!lexer.eof()) lexer.copyArithmeticUnit(out);
		return out;
	}

	void Lexer::copyArithmeticUnit(std::string& out) {
		switch (peek()) {
		case '"':
			advance();
			return;
		case '\\':
			copyBackslashEscape(out);
			return;
		case '\'':
			copySingleQuotedRun(out);
			return;
		case '`':
			copyBackquotedRun(out);
			return;
		case '$':
			copyArithmeticDollar(out);
			return;
		default:
			out.push_back(advance());
			return;
		}
	}

	void Lexer::copyArithmeticDollar(std::string& out) {
		if (peek(1) == '(') {
			copyDollarParenRun(out);
			return;
		}

		out.push_back(advance());
		if (peek() != '{') return;

		out.push_back(advance());
		out += readBalancedBraces();
		out.push_back('}');
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

	// `$'...'`, where a backslash escapes the quote.
	void Lexer::copyDollarSingleQuotedRun(std::string& out) {
		out.push_back(advance());
		out.push_back(advance());
		while (!eof() && peek() != '\'') {
			if (peek() == '\\') copyBackslashEscape(out);
			else out.push_back(advance());
		}

		if (!eof()) out.push_back(advance());
	}

	void Lexer::copyDoubleQuotedRun(std::string& out) {
		out.push_back(advance());
		while (!eof() && peek() != '"') {
			if (peek() == '\\' && peek(1) == '\n') {
				advance();
				advance();
				continue;
			}

			if (peek() == '\\') {
				copyBackslashEscape(out);
				continue;
			}

			if (peek() == '$' && peek(1) == '(') {
				copyDollarParenRun(out);
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

	void Lexer::copyDollarParenRun(std::string& out) {
		const std::size_t start = pos_;
		std::vector<WordSegment> ignored;
		advance();
		readDollarParen(ignored);
		out += src_.substr(start, pos_ - start);
	}

	static bool endsBracketScan(char c) {
		return c == '\0' || c == '\n' || c == '}' || c == '\'' || c == '"'
		    || c == '\\' || c == '$' || c == '`';
	}

	// The length of the bracket expression that starts here, or 0 when a
	// quote, an expansion or the closing brace comes before its `]` and the
	// `[` is a plain character.
	std::size_t Lexer::bracketExpressionLength() const {
		std::size_t n = 1;
		if (peek(n) == '!' || peek(n) == '^') ++n;
		if (peek(n) == ']') ++n;

		for (; !endsBracketScan(peek(n)); ++n) {
			if (peek(n) == ']') return n + 1;
		}

		return 0;
	}

	// A `{` in a bracket expression of a pattern, as in `${o%%[<{(]*}`, is a
	// character to match, not a brace that needs closing.
	void Lexer::copyBracketExpression(std::string& out) {
		std::size_t length = bracketExpressionLength();
		if (length == 0) length = 1;

		for (std::size_t i = 0; i < length; ++i) out.push_back(advance());
	}

	// Past the parameter (`name`, `name[sub]`, a special one) of a ${...}
	// body, at a `-`, `+`, `=` or `?` operator, with or without `:`.
	static std::size_t wordOperatorAt(const std::string& body) {
		std::size_t k = (!body.empty() && (body[0] == '!' || body[0] == '#')) ? 1 : 0;
		const bool named = k < body.size() && (isNameStart(body[k]) || isDigit(body[k]));
		if (named) {
			while (k < body.size() && isNameCont(body[k])) ++k;
		} else {
			++k;
		}

		if (k < body.size() && body[k] == '[') k = std::min(body.find(']', k), body.size() - 1) + 1;
		if (k < body.size() && body[k] == ':') ++k;
		const bool found = k < body.size() && std::strchr("-+=?", body[k]) != nullptr;
		return found ? k : std::string::npos;
	}

	static bool atWordOperator(const std::string& body) {
		return wordOperatorAt(body) != std::string::npos;
	}

	// After an odd number of the literal single quotes of such a word.
	static bool insideLiteralQuote(const std::string& body) {
		const std::size_t op = wordOperatorAt(body);
		if (op == std::string::npos) return false;

		std::size_t quotes = 0;
		for (std::size_t k = op + 1; k < body.size(); ++k) {
			if (body[k] == '\\') ++k;
			else if (body[k] == '\'') ++quotes;
		}

		return quotes % 2 == 1;
	}

	bool Lexer::literalBraceQuotes() const {
		return posix_mode_ && in_double_quotes_;
	}

	// A `$(` between such literal quotes is plain text while the brace is
	// looked for.
	void Lexer::copyBraceDollarParen(std::string& out) {
		if (literalBraceQuotes() && insideLiteralQuote(out)) out.push_back(advance());
		else copyDollarParenRun(out);
	}

	// In POSIX mode a single quote in the word of a double-quoted
	// "${name-word}" (and +, =, ?) is literal and cannot hide a closing
	// brace; in a pattern ("${name#'x'}") it still quotes. Outside POSIX
	// mode it hides the brace, though bash 5.3 keeps it in the value.
	void Lexer::copyBraceSingleQuote(std::string& out) {
		if (literalBraceQuotes() && atWordOperator(out)) out.push_back(advance());
		else copySingleQuotedRun(out);
	}

	// bash 5.3's `${ list; }` and `${| list; }` hold commands: the brace
	// that ends them is a `}` where a command could start.
	bool Lexer::atFunsubStart() const {
		const char c = peek();
		return c == ' ' || c == '\t' || c == '\n' || c == '|';
	}

	// Like bash, a `}` where a command word could start ends the list,
	// whatever follows it: `${ echo a;}b` is `ab`.
	bool Lexer::atClosingBraceWord() const {
		return peek() == '}' && atCommandWordStart();
	}

	// A `{` opens a group where a command could start and as the body of
	// `name()` or `function name`.
	bool Lexer::atBraceGroupOpen() const {
		if (atCommandWordStart()) return true;
		if (tokens_.empty()) return false;
		if (tokens_.back().kind == TokKind::RParen) return true;

		return tokens_.size() >= 2 && isWordSpelled(tokens_[tokens_.size() - 2], "function");
	}

	static int braceGroupDelta(const Token& token, bool group_open, bool command_word) {
		if (token.kind != TokKind::Word) return 0;
		if (group_open && isWordSpelled(token, "{")) return 1;
		if (command_word && isWordSpelled(token, "}")) return -1;
		return 0;
	}

	bool Lexer::scanToUnmatchedBrace() {
		int depth = 0;
		while (!eof()) {
			skipWhitespace();
			if (depth == 0 && atClosingBraceWord()) {
				advance();
				return true;
			}

			if (eof()) break;

			const bool group_open = atBraceGroupOpen();
			const bool command_word = atCommandWordStart();
			const std::size_t before = tokens_.size();
			scanToken();
			if (tokens_.size() > before) {
				depth += braceGroupDelta(tokens_.back(), group_open, command_word);
			}
		}

		return false;
	}

	std::string Lexer::readFunsubBody() {
		const SourceLoc start = loc_;
		const std::size_t body_start = pos_;
		if (peek() == '|') advance();

		const std::size_t splices_before = splices_.size();
		OuterScanState outer = enterNestedScan();
		++nested_scan_depth_;
		const bool closed = scanToUnmatchedBrace();
		--nested_scan_depth_;
		leaveNestedScan(outer);
		restoreAliasNames(body_start, splices_before);

		if (!closed) {
			SourceLoc past_end = loc_;
			past_end.line = lastLineReached() + 1;
			error(past_end, "unexpected EOF while looking for matching `}'");
			return src_.substr(body_start);
		}

		return src_.substr(body_start, pos_ - 1 - body_start);
	}

	std::string Lexer::readBalancedBraces() {
		if (atFunsubStart()) return readFunsubBody();

		const SourceLoc start = loc_;
		std::string out;
		int depth = 1;
		while (!eof() && depth > 0) {
			switch (peek()) {
			case '\\': copyBackslashEscape(out); continue;
			case '\'': copyBraceSingleQuote(out); continue;
			case '"':  copyDoubleQuotedRun(out); continue;
			case '`':  copyBackquotedRun(out); continue;
			case '$':
				if (peek(1) == '{') {
					out.push_back(advance());
					out.push_back(advance());
					++depth;
					continue;
				}

				if (peek(1) == '(') {
					copyBraceDollarParen(out);
					continue;
				}

				if (peek(1) == '\'') {
					copyDollarSingleQuotedRun(out);
					continue;
				}

				out.push_back(advance());
				continue;
			case '[':
				copyBracketExpression(out);
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

		error(start, "unexpected EOF while looking for matching `}'");
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

		error(loc_, "unexpected EOF while looking for matching `]'");
		return out;
	}

	// The body follows the end of the line the command ends on, which bash
	// names in its end-of-file warning.
	void Lexer::collectHeredocBodies() {
		const std::size_t start_line = tokens_.back().loc.line;
		while (!pending_heredocs_.empty()) {
			const PendingHeredoc pending = pending_heredocs_.front();
			pending_heredocs_.erase(pending_heredocs_.begin());

			Token& delim = tokens_[pending.delim_token_index];
			delim.heredoc_start_line = start_line;
			delim.heredoc_body = readHeredocBody(delim);
		}

		collectCarriedHeredocs(start_line);
	}

	void Lexer::carryHeredocs(const std::vector<Token>& unread, const std::string& body) {
		for (const Token& delim : unread) carried_heredocs_.push_back({ delim, tokens_.size(), body });
	}

	static WordSegment* carriedSubstitution(std::vector<WordSegment>& segments,
			const std::string& body) {
		for (WordSegment& segment : segments) {
			if (segment.kind == WordSegment::Kind::CmdSubst && segment.text == body) return &segment;

			WordSegment* inner = carriedSubstitution(segment.nested, body);
			if (inner != nullptr) return inner;
		}

		return nullptr;
	}

	void Lexer::collectCarriedHeredocs(std::size_t start_line) {
		std::vector<CarriedHeredoc> carried = std::move(carried_heredocs_);
		carried_heredocs_.clear();
		for (std::size_t k = 0; k < carried.size(); ++k) {
			CarriedHeredoc& entry = carried[k];
			entry.delim.heredoc_start_line = start_line;
			const std::string text = readHeredocBody(entry.delim);
			if (entry.word_token_index >= tokens_.size()) continue;

			WordSegment* segment = carriedSubstitution(tokens_[entry.word_token_index].segments,
				entry.body);
			if (segment == nullptr) continue;

			segment->text += "\n" + text + flattenDelim(entry.delim) + "\n";
			++segment->carried_heredocs;
			for (std::size_t later = k + 1; later < carried.size(); ++later) {
				if (carried[later].body == entry.body) carried[later].body = segment->text;
			}
		}
	}

	// In an unquoted body a backslash-newline joins the next line onto
	// this one, so that next line can never be the delimiter.
	static bool endsInLineContinuation(const std::string& line) {
		std::size_t backslashes = 0;
		while (backslashes < line.size() && line[line.size() - 1 - backslashes] == '\\') {
			++backslashes;
		}

		return backslashes % 2 == 1;
	}

	// In an unquoted document bash joins a line ending in a backslash with
	// the next before it compares the result with the delimiter, so `EO\`
	// and `F` end a document delimited by EOF. The lines of a joined line
	// wait in `pending` until it is known not to be the delimiter.
	std::string Lexer::readHeredocBody(Token& delim) {
		std::string terminator = flattenDelim(delim);
		if (delim.heredoc_strip_tabs) stripLeadingTabs(terminator);

		HeredocLine joined;
		std::string body;
		for (;;) {
			std::string line = readLineText();
			if (delim.heredoc_strip_tabs && !joined.continued) stripLeadingTabs(line);
			if (!joined.continued && endsAtSubstitutionClose(line, terminator)) {
				unreadLineTail(line.size() - terminator.size());
				return body;
			}

			if (!joined.continued && cutOffBySubstitutionClose(line, terminator)) {
				delim.heredoc_eof_line = loc_.line;
				return body;
			}

			joinHeredocLine(joined, line, delim.heredoc_quoted);
			const bool is_line = !eof() || !line.empty();
			if (!joined.continued && is_line && joined.logical == terminator) {
				if (!eof()) advance();
				return body;
			}

			if (eof()) return finishHeredocAtEof(delim, joined, line, body);

			takeHeredocLine(joined, line, body);
			advance();
		}
	}

	void Lexer::joinHeredocLine(HeredocLine& joined, const std::string& line, bool quoted) {
		const bool continues = !quoted && endsInLineContinuation(line);
		joined.logical += continues ? line.substr(0, line.size() - 1) : line;
		joined.continued = continues;
	}

	void Lexer::takeHeredocLine(HeredocLine& joined, const std::string& line, std::string& body) {
		joined.pending += line + "\n";
		if (joined.continued) return;

		body += joined.pending;
		joined.pending.clear();
		joined.logical.clear();
	}

	std::string Lexer::finishHeredocAtEof(Token& delim, const HeredocLine& joined,
			const std::string& line, std::string& body) {
		if (!heredoc_at_eof_ok_) {
			error(delim.loc, "unterminated heredoc body");
			return body;
		}

		body += joined.pending;
		if (!line.empty()) body += line + "\n";
		delim.heredoc_eof_line = lastLineReached();
		if (nested_scan_depth_ == 0 && errors_.empty()) warnHeredocAtEof(delim);
		return body;
	}

	static bool startsWith(const std::string& line, const std::string& prefix) {
		return line.compare(0, prefix.size(), prefix) == 0;
	}

	// Inside `$(...)` bash 5.3 also ends a here-document at a line that
	// starts with the delimiter and has a `)` after it, and reads the rest
	// of that line again.
	bool Lexer::endsAtSubstitutionClose(const std::string& line,
			const std::string& terminator) const {
		if (nested_scan_depth_ == 0 || line.size() <= terminator.size()) return false;
		if (!startsWith(line, terminator)) return false;

		return line.find(')', terminator.size()) != std::string::npos;
	}

	bool Lexer::cutOffBySubstitutionClose(const std::string& line,
			const std::string& terminator) const {
		return substitution_body_ && nested_scan_depth_ == 0 && eof() && startsWith(line, terminator);
	}

	void Lexer::unreadLineTail(std::size_t length) {
		pos_ -= length;
		loc_.offset -= length;
		loc_.column -= length;
	}

	// The line of the last character read: a final newline does not
	// start a line of its own.
	std::size_t Lexer::lastLineReached() const {
		if (loc_.column == 1 && loc_.line > 1) return loc_.line - 1;
		return loc_.line;
	}

	std::string Lexer::readLineText() {
		std::string line;
		while (!eof() && peek() != '\n') line.push_back(advance());
		return line;
	}

}  // namespace wbsh

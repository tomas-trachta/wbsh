#include "parser.h"

#include <cctype>
#include <cstring>
#include <utility>

#include "numparse.h"

namespace wbsh {

	const char* redirOpName(RedirOp o) {
		switch (o) {
		case RedirOp::Less: return "<";
		case RedirOp::Great: return ">";
		case RedirOp::DGreat: return ">>";
		case RedirOp::LessAnd: return "<&";
		case RedirOp::GreatAnd: return ">&";
		case RedirOp::LessGreat: return "<>";
		case RedirOp::Clobber: return ">|";
		case RedirOp::AmpGreat: return "&>";
		case RedirOp::AmpDGreat: return "&>>";
		case RedirOp::DLess: return "<<";
		case RedirOp::DLessDash: return "<<-";
		case RedirOp::TLess: return "<<<";
		}

		return "?";
	}

	const char* nodeKindName(Node::Kind k) {
		switch (k) {
		case Node::Kind::SimpleCommand: return "SimpleCommand";
		case Node::Kind::Pipeline: return "Pipeline";
		case Node::Kind::AndOr: return "AndOr";
		case Node::Kind::List: return "List";
		case Node::Kind::BraceGroup: return "BraceGroup";
		case Node::Kind::Subshell: return "Subshell";
		case Node::Kind::IfClause: return "IfClause";
		case Node::Kind::WhileClause: return "WhileClause";
		case Node::Kind::ForClause: return "ForClause";
		case Node::Kind::CaseClause: return "CaseClause";
		case Node::Kind::FunctionDef: return "FunctionDef";
		case Node::Kind::DBracket: return "DBracket";
		}

		return "?";
	}

	Parser::Parser(std::vector<Token> tokens) : toks_(std::move(tokens)) {}

	Parser::Parser(std::vector<Token> tokens, std::string source_text)
		: toks_(std::move(tokens))
		, source_(arena_.make<std::string>(std::move(source_text))) {}

	const Token& Parser::peek(std::size_t n) const {
		if (pos_ + n >= toks_.size()) return toks_.back();
		return toks_[pos_ + n];
	}

	const Token& Parser::advance() {
		const Token& t = toks_[pos_];
		if (pos_ + 1 < toks_.size()) ++pos_;
		return t;
	}

	bool Parser::atEnd() const {
		return peek().kind == TokKind::EndOfInput;
	}

	bool Parser::check(TokKind k) const { return peek().kind == k; }

	bool Parser::match(TokKind k) {
		if (!check(k)) return false;
		advance();
		return true;
	}

	bool Parser::isReserved(const Token& t, const char* word) const {
		if (t.kind != TokKind::Word) return false;
		if (t.segments.size() != 1) return false;
		const auto& s = t.segments[0];
		if (s.kind != WordSegment::Kind::Literal) return false;
		return s.text == word;
	}

	bool Parser::checkReserved(const char* word) const {
		return isReserved(peek(), word);
	}

	bool Parser::matchReserved(const char* word) {
		if (!checkReserved(word)) return false;
		advance();
		return true;
	}

	bool Parser::checkAnyReserved(std::initializer_list<const char*> words) const {
		for (auto w : words) if (checkReserved(w)) return true;
		return false;
	}

	bool Parser::tokenIsReservedTerminator(const Token& t) const {
		static const char* terms[] = {
			"then","elif","else","fi","do","done","esac","in","}"
		};
		for (auto w : terms) if (isReserved(t, w)) return true;
		return false;
	}

	void Parser::error(const Token& at, std::string msg) {
		errors_.push_back({ at.loc, std::move(msg) });
	}

	void Parser::expect(TokKind k, const char* msg) {
		if (!match(k)) error(peek(), msg);
	}

	void Parser::expectReserved(const char* word, const char* msg) {
		if (!matchReserved(word)) error(peek(), msg);
	}

	void Parser::skipNewlines() {
		while (match(TokKind::Newline)) {}
	}

	bool Parser::atRedirOp() const {
		switch (peek().kind) {
		case TokKind::Less: case TokKind::Great: case TokKind::DGreat:
		case TokKind::LessAnd: case TokKind::GreatAnd: case TokKind::LessGreat:
		case TokKind::Clobber: case TokKind::AmpGreat: case TokKind::AmpDGreat:
		case TokKind::DLess: case TokKind::DLessDash: case TokKind::TLess:
			return true;
		default:
			return false;
		}
	}

	bool Parser::atListSeparator() const {
		return check(TokKind::Semi) || check(TokKind::Amp) || check(TokKind::Newline);
	}

	std::size_t Parser::srcOffsetHere() const {
		return peek().loc.offset;
	}

	std::size_t Parser::srcOffsetEnd() const {
		if (pos_ == 0) return peek().loc.offset;
		const Token& prev = toks_[pos_ - 1];
		return prev.loc.offset + prev.text.size();
	}

	void Parser::stampSpan(Node& node, std::size_t start_offset) {
		node.src_start = start_offset;
		node.src_end = srcOffsetEnd();
		if (node.src_end < node.src_start) node.src_end = node.src_start;
		node.source_text = source_;
	}

	bool Parser::atCommandStart() const {
		if (atEnd()) return false;
		if (tokenIsReservedTerminator(peek())) return false;
		if (peek().kind == TokKind::Newline) return false;
		if (peek().kind == TokKind::Semi || peek().kind == TokKind::Amp) return false;
		if (peek().kind == TokKind::DSemi || peek().kind == TokKind::SemiAmp ||
			peek().kind == TokKind::DSemiAmp) return false;
		if (peek().kind == TokKind::RParen) return false;
		return true;
	}

	Word Parser::tokenToWord(const Token& t) const {
		Word w;
		w.segments = t.segments;
		w.raw = t.text;
		w.loc = t.loc;
		return w;
	}

	NodePtr Parser::parseProgram() {
		return parseList(/*top_level=*/true);
	}

	NodePtr Parser::parseList(bool /*top_level*/) {
		std::size_t start = srcOffsetHere();
		auto list = arena_.make<List>();
		list->loc = peek().loc;
		skipNewlines();
		while (!atEnd()) {
			auto andor = parseAndOr();
			if (!andor) break;
			ListItem it;
			it.command = andor;
			bool had_sep = false;
			if (match(TokKind::Amp)) { it.background = true; had_sep = true; }
			else if (match(TokKind::Semi)) { had_sep = true; }
			else if (match(TokKind::Newline)) { had_sep = true; }
			list->items.push_back(std::move(it));
			if (!had_sep) break;
			skipNewlines();
		}

		stampSpan(*list, start);
		return list;
	}

	NodePtr Parser::parseAndOr() {
		std::size_t start = srcOffsetHere();
		NodePtr left = parsePipeline();
		if (!left) return nullptr;
		while (check(TokKind::AndIf) || check(TokKind::OrIf)) {
			auto op = check(TokKind::AndIf) ? AndOr::Op::AndIf : AndOr::Op::OrIf;
			advance();
			skipNewlines();
			auto right = parsePipeline();
			if (!right) {
				error(peek(), "expected pipeline after && / ||");
				break;
			}

			auto ao = arena_.make<AndOr>();
			ao->op = op;
			ao->loc = left->loc;
			ao->left = left;
			ao->right = right;
			ao->src_start = start;
			ao->src_end = srcOffsetEnd();
			left = ao;
		}

		if (left && left->src_end == 0) left->src_end = srcOffsetEnd();
		return left;
	}

	NodePtr Parser::parsePipeline() {
		std::size_t start = srcOffsetHere();
		// `time` is the keyword only when a command follows; `time` alone
		// (or before a redirection / `;`) is an ordinary argv[0].
		bool timed = false;
		if (checkReserved("time")) {
			std::size_t saved = pos_;
			advance();
			if (atCommandStart() || checkReserved("!")) {
				timed = true;
			} else {
				pos_ = saved;
			}
		}

		bool bang = matchReserved("!");
		auto first = parseCommand();
		if (!first) {
			if (bang) error(peek(), "expected command after `!`");
			if (timed) error(peek(), "expected command after `time`");
			return nullptr;
		}

		if (!bang && !timed && !check(TokKind::Pipe) && !check(TokKind::PipeAmp)) {
			return first;
		}

		auto pipe = arena_.make<Pipeline>();
		pipe->bang = bang;
		pipe->timed = timed;
		pipe->loc = first->loc;
		pipe->commands.push_back(first);
		while (check(TokKind::Pipe) || check(TokKind::PipeAmp)) {
			bool amp = check(TokKind::PipeAmp);
			advance();
			skipNewlines();
			auto next = parseCommand();
			if (!next) {
				error(peek(), "expected command after pipe");
				break;
			}

			pipe->stderr_to_stdout.push_back(amp);
			pipe->commands.push_back(next);
		}

		stampSpan(*pipe, start);
		return pipe;
	}

	NodePtr Parser::parseCommand() {
		if (!atCommandStart()) return nullptr;

		if (checkReserved("{"))     return parseBraceGroup();
		if (checkReserved("if"))    return parseIf();
		if (checkReserved("while")) return parseWhileUntil(false);
		if (checkReserved("until")) return parseWhileUntil(true);
		if (checkReserved("for"))   return parseFor();
		if (checkReserved("case"))  return parseCase();
		if (checkReserved("[["))    return parseDBracket();

		if (checkReserved("function")) {
			SourceLoc loc = peek().loc;
			advance();
			if (peek().kind != TokKind::Word) {
				error(peek(), "expected function name after `function`");
				return nullptr;
			}

			std::string name = peek().text;
			advance();
			if (match(TokKind::LParen)) {
				if (!match(TokKind::RParen)) error(peek(), "expected `)`");
			}

			return parseFunctionRest(std::move(name), loc);
		}

		if (check(TokKind::LParen)) return parseSubshell();

		if (peek().kind == TokKind::Word
			&& peek(1).kind == TokKind::LParen
			&& peek(2).kind == TokKind::RParen)
		{
			std::string name = peek().text;
			SourceLoc loc = peek().loc;
			advance(); advance(); advance();
			return parseFunctionRest(std::move(name), loc);
		}

		return parseSimpleCommand();
	}

	NodePtr Parser::parseBraceGroup() {
		std::size_t start = srcOffsetHere();
		SourceLoc loc = peek().loc;
		advance();
		auto body = parseList(false);
		expectReserved("}", "expected `}`");
		auto bg = arena_.make<BraceGroup>();
		bg->loc = loc;
		bg->body = body;
		Redirection r;
		while (tryParseRedirection(r)) bg->redirs.push_back(std::move(r));
		stampSpan(*bg, start);
		return bg;
	}

	NodePtr Parser::parseSubshell() {
		std::size_t start = srcOffsetHere();
		SourceLoc loc = peek().loc;
		advance();
		auto body = parseList(false);
		expect(TokKind::RParen, "expected `)`");
		auto ss = arena_.make<Subshell>();
		ss->loc = loc;
		ss->body = body;
		Redirection r;
		while (tryParseRedirection(r)) ss->redirs.push_back(std::move(r));
		stampSpan(*ss, start);
		return ss;
	}

	NodePtr Parser::parseIf() {
		std::size_t start = srcOffsetHere();
		SourceLoc loc = peek().loc;
		advance();
		auto cond = parseList(false);
		expectReserved("then", "expected `then`");
		auto then_body = parseList(false);

		auto node = arena_.make<IfClause>();
		node->loc = loc;
		node->branches.push_back({ cond, then_body });

		while (matchReserved("elif")) {
			auto c = parseList(false);
			expectReserved("then", "expected `then` after elif condition");
			auto b = parseList(false);
			node->branches.push_back({ c, b });
		}

		if (matchReserved("else")) {
			node->else_body = parseList(false);
		}

		expectReserved("fi", "expected `fi`");
		Redirection r;
		while (tryParseRedirection(r)) node->redirs.push_back(std::move(r));
		stampSpan(*node, start);
		return node;
	}

	NodePtr Parser::parseDoGroup() {
		if (!matchReserved("do")) {
			error(peek(), "expected `do`");
			return nullptr;
		}

		auto body = parseList(false);
		expectReserved("done", "expected `done`");
		return body;
	}

	NodePtr Parser::parseWhileUntil(bool until) {
		std::size_t start = srcOffsetHere();
		SourceLoc loc = peek().loc;
		advance();
		auto cond = parseList(false);
		auto body = parseDoGroup();
		auto node = arena_.make<WhileClause>();
		node->loc = loc;
		node->until = until;
		node->cond = cond;
		node->body = body;
		Redirection r;
		while (tryParseRedirection(r)) node->redirs.push_back(std::move(r));
		stampSpan(*node, start);
		return node;
	}

	NodePtr Parser::parseFor() {
		std::size_t start = srcOffsetHere();
		SourceLoc loc = peek().loc;
		advance();
		if (peek().kind != TokKind::Word) {
			error(peek(), "expected variable name after `for`");
			return nullptr;
		}

		std::string var = peek().text;
		advance();
		skipNewlines();

		bool has_in = false;
		std::vector<Word> items;
		if (matchReserved("in")) {
			has_in = true;
			while (peek().kind == TokKind::Word
				&& !tokenIsReservedTerminator(peek()))
			{
				items.push_back(tokenToWord(advance()));
			}

			if (!match(TokKind::Semi) && !match(TokKind::Newline)) {
				if (!checkReserved("do"))
					error(peek(), "expected `;` or newline after for-in word list");
			}

			skipNewlines();
		}
		else {
			if (match(TokKind::Semi) || match(TokKind::Newline)) {
				skipNewlines();
			}
		}

		auto body = parseDoGroup();
		auto node = arena_.make<ForClause>();
		node->loc = loc;
		node->var = std::move(var);
		node->has_in = has_in;
		node->items = std::move(items);
		node->body = body;
		Redirection r;
		while (tryParseRedirection(r)) node->redirs.push_back(std::move(r));
		stampSpan(*node, start);
		return node;
	}

	NodePtr Parser::parseCase() {
		std::size_t start = srcOffsetHere();
		SourceLoc loc = peek().loc;
		advance();
		if (peek().kind != TokKind::Word) {
			error(peek(), "expected word after `case`");
			return nullptr;
		}

		Word subject = tokenToWord(advance());
		skipNewlines();
		expectReserved("in", "expected `in`");
		skipNewlines();

		auto node = arena_.make<CaseClause>();
		node->loc = loc;
		node->subject = std::move(subject);

		while (!atEnd() && !checkReserved("esac")) {
			CaseClause::Item item;
			match(TokKind::LParen);
			while (peek().kind == TokKind::Word) {
				item.patterns.push_back(tokenToWord(advance()));
				if (!match(TokKind::Pipe)) break;
			}

			expect(TokKind::RParen, "expected `)` after case pattern(s)");
			skipNewlines();
			if (!checkReserved("esac")
				&& !check(TokKind::DSemi)
				&& !check(TokKind::SemiAmp)
				&& !check(TokKind::DSemiAmp))
			{
				item.body = parseList(false);
			}

			if (match(TokKind::DSemi))         item.term = CaseClause::Term::DSemi;
			else if (match(TokKind::SemiAmp))  item.term = CaseClause::Term::SemiAmp;
			else if (match(TokKind::DSemiAmp)) item.term = CaseClause::Term::DSemiAmp;
			else                               item.term = CaseClause::Term::DSemi;
			skipNewlines();
			node->items.push_back(std::move(item));
		}

		expectReserved("esac", "expected `esac`");
		Redirection r;
		while (tryParseRedirection(r)) node->redirs.push_back(std::move(r));
		stampSpan(*node, start);
		return node;
	}

	bool Parser::atDBracketEnd() const {
		return isReserved(peek(), "]]");
	}

	DBracketCond::Expr* Parser::parseDBracketExpr() {
		auto left = parseDBracketAnd();
		while (!atDBracketEnd() && check(TokKind::OrIf)) {
			advance();
			auto right = parseDBracketAnd();
			auto e = arena_.make<DBracketCond::Expr>();
			e->k = DBracketCond::Expr::K::Or;
			e->a = left;
			e->b = right;
			left = e;
		}

		return left;
	}

	DBracketCond::Expr* Parser::parseDBracketAnd() {
		auto left = parseDBracketUnary();
		while (!atDBracketEnd() && check(TokKind::AndIf)) {
			advance();
			auto right = parseDBracketUnary();
			auto e = arena_.make<DBracketCond::Expr>();
			e->k = DBracketCond::Expr::K::And;
			e->a = left;
			e->b = right;
			left = e;
		}

		return left;
	}

	DBracketCond::Expr* Parser::parseDBracketUnary() {
		if (matchReserved("!")) {
			auto inner = parseDBracketUnary();
			auto e = arena_.make<DBracketCond::Expr>();
			e->k = DBracketCond::Expr::K::Not;
			e->a = inner;
			return e;
		}

		return parseDBracketPrimary();
	}

	static bool isDBracketUnaryOp(const std::string& s) {
		if (s.size() != 2 || s[0] != '-') return false;
		static const char ops[] = "abcdefghknoprstuwxzGLNOSU";
		for (char c : ops) if (c == s[1]) return true;
		return false;
	}

	static bool isDBracketBinaryOp(const std::string& s) {
		return s == "==" || s == "!=" || s == "=" || s == "=~"
		    || s == "-eq" || s == "-ne" || s == "-lt" || s == "-le"
		    || s == "-gt" || s == "-ge"
		    || s == "-ef" || s == "-nt" || s == "-ot";
	}

	static std::string dBracketOpAsString(const Token& t) {
		if (t.kind == TokKind::Less)  return "<";
		if (t.kind == TokKind::Great) return ">";
		if (t.kind == TokKind::Word
		    && t.segments.size() == 1
		    && t.segments[0].kind == WordSegment::Kind::Literal) {
			return t.segments[0].text;
		}

		return {};
	}

	bool Parser::tryParseDBracketUnary(DBracketCond::Expr& e) {
		if (peek().kind != TokKind::Word
		    || peek().segments.size() != 1
		    || peek().segments[0].kind != WordSegment::Kind::Literal
		    || !isDBracketUnaryOp(peek().segments[0].text)
		    || peek(1).kind != TokKind::Word
		    || isReserved(peek(1), "]]"))
		{
			return false;
		}
		// `-f && X` — only treat as unary if the operand is followed by a
		// connective / closer; otherwise fall back to single-operand truthiness.
		const Token& after = peek(2);
		bool ok = isReserved(after, "]]")
		    || after.kind == TokKind::AndIf
		    || after.kind == TokKind::OrIf
		    || after.kind == TokKind::RParen;
		if (!ok) return false;
		e.op = peek().segments[0].text;
		advance();
		e.lhs = tokenToWord(advance());
		return true;
	}

	DBracketCond::Expr* Parser::parseDBracketPrimary() {
		if (check(TokKind::LParen)) {
			advance();
			auto inner = parseDBracketExpr();
			if (!match(TokKind::RParen)) {
				error(peek(), "expected `)` inside [[ ... ]]");
			}

			return inner;
		}

		auto e = arena_.make<DBracketCond::Expr>();
		e->k = DBracketCond::Expr::K::Prim;

		if (tryParseDBracketUnary(*e)) return e;

		if (peek().kind != TokKind::Word) {
			error(peek(), "expected operand in [[ ... ]]");
			return e;
		}

		e->lhs = tokenToWord(advance());

		const std::string opstr = dBracketOpAsString(peek());
		if (!opstr.empty()
		    && (opstr == "<" || opstr == ">" || isDBracketBinaryOp(opstr))
		    && !isReserved(peek(), "]]"))
		{
			e->op = opstr;
			advance();
			if (peek().kind != TokKind::Word) {
				error(peek(), "expected right operand in [[ ... ]]");
				return e;
			}

			e->rhs = tokenToWord(advance());
		}

		return e;
	}

	NodePtr Parser::parseDBracket() {
		std::size_t start = srcOffsetHere();
		SourceLoc loc = peek().loc;
		advance();
		auto node = arena_.make<DBracketCond>();
		node->loc = loc;
		if (atDBracketEnd()) {
			error(peek(), "[[: empty conditional expression");
		} else {
			node->root = parseDBracketExpr();
		}

		if (!matchReserved("]]")) {
			error(peek(), "expected `]]`");
		}

		Redirection r;
		while (tryParseRedirection(r)) node->redirs.push_back(std::move(r));
		stampSpan(*node, start);
		return node;
	}

	NodePtr Parser::parseFunctionRest(std::string name, SourceLoc loc) {
		std::size_t start = loc.offset;
		skipNewlines();
		auto body = parseCommand();
		if (!body) {
			error(peek(), "expected function body");
			return nullptr;
		}

		auto fn = arena_.make<FunctionDef>();
		fn->loc = loc;
		fn->name = std::move(name);
		fn->body = body;
		stampSpan(*fn, start);
		if (fn->body && source_ && !source_->empty()
		    && fn->body->src_end > fn->body->src_start
		    && fn->body->src_end <= source_->size()) {
			fn->body_text = source_->substr(fn->body->src_start,
				fn->body->src_end - fn->body->src_start);
		}

		return fn;
	}

	void Parser::parseArrayLiteralItem(Assignment& a) {
		Word w = tokenToWord(advance());
		Assignment::Keyed item;

		const bool has_key_form =
			!w.raw.empty() && w.raw[0] == '['
			&& !w.segments.empty()
			&& w.segments[0].kind == WordSegment::Kind::Literal;
		if (has_key_form) {
			const std::string& lit = w.segments[0].text;
			const std::size_t close = lit.find(']');
			if (close != std::string::npos
			    && close + 1 < lit.size()
			    && lit[close + 1] == '=')
			{
				WordSegment ks;
				ks.kind = WordSegment::Kind::Literal;
				ks.text = lit.substr(1, close - 1);
				item.key.segments.push_back(std::move(ks));
				item.has_key = true;

				const std::string val_text = lit.substr(close + 2);
				if (!val_text.empty()) {
					WordSegment vs;
					vs.kind = WordSegment::Kind::Literal;
					vs.text = val_text;
					item.value.segments.push_back(std::move(vs));
				}

				for (std::size_t k = 1; k < w.segments.size(); ++k) {
					item.value.segments.push_back(w.segments[k]);
				}

				item.value.raw = w.raw;
				a.keyed_items.push_back(std::move(item));
				return;
			}
		}

		Assignment::Keyed unkeyed;
		unkeyed.value = std::move(w);
		a.keyed_items.push_back(std::move(unkeyed));
	}

	void Parser::parseArrayLiteralBody(Assignment& a) {
		a.is_array = true;
		skipNewlines();
		while (!atEnd() && peek().kind != TokKind::RParen) {
			if (peek().kind == TokKind::Newline) {
				advance();
				continue;
			}

			if (peek().kind != TokKind::Word) {
				error(peek(), "unexpected token in array literal");
				break;
			}

			parseArrayLiteralItem(a);
		}

		if (!match(TokKind::RParen)) {
			error(peek(), "expected `)` to close array literal");
		}
	}

	static bool isEmptyScalarAssignmentSlot(const Assignment& a) {
		return a.value.segments.empty() && !a.has_subscript;
	}

	bool Parser::tryConsumeLeadingAssignment(SimpleCommand& cmd) {
		Assignment a;
		if (!tryExtractAssignment(peek(), a)) return false;
		advance();
		if (isEmptyScalarAssignmentSlot(a) && peek().kind == TokKind::LParen) {
			advance();
			parseArrayLiteralBody(a);
		}

		cmd.assignments.push_back(std::move(a));
		return true;
	}

	NodePtr Parser::parseSimpleCommand() {
		const std::size_t start = srcOffsetHere();
		auto cmd = arena_.make<SimpleCommand>();
		cmd->loc = peek().loc;
		bool seen_word = false;

		while (!atEnd()) {
			if (atRedirOp() || peek().kind == TokKind::IoNumber) {
				Redirection r;
				if (!tryParseRedirection(r)) break;
				cmd->redirs.push_back(std::move(r));
				continue;
			}

			if (peek().kind != TokKind::Word) break;

			if (!seen_word && tryConsumeLeadingAssignment(*cmd)) continue;

			cmd->words.push_back(tokenToWord(advance()));
			seen_word = true;
		}

		if (cmd->words.empty() && cmd->assignments.empty() && cmd->redirs.empty()) {
			return nullptr;
		}

		stampSpan(*cmd, start);
		return cmd;
	}

	static std::size_t scanAssignmentNameLength(const std::string& s0) {
		if (s0.empty()) return 0;
		const unsigned char c0 = static_cast<unsigned char>(s0[0]);
		if (!(std::isalpha(c0) || c0 == '_')) return 0;
		std::size_t i = 1;
		while (i < s0.size()
		       && (std::isalnum(static_cast<unsigned char>(s0[i])) || s0[i] == '_'))
		{
			++i;
		}

		return i;
	}

	static bool findSubscriptCloseBracket(const Token& t,
	                                      std::size_t cur_seg, std::size_t cur_pos,
	                                      std::size_t* out_close_seg,
	                                      std::size_t* out_close_pos) {
		for (std::size_t k = 0; k < t.segments.size(); ++k) {
			const auto& seg = t.segments[k];
			if (seg.kind != WordSegment::Kind::Literal) continue;
			const std::size_t start = (k == cur_seg) ? cur_pos : 0;
			const auto rb = seg.text.find(']', start);
			if (rb == std::string::npos) continue;
			*out_close_seg = k;
			*out_close_pos = rb;
			return true;
		}

		return false;
	}

	static std::size_t subscriptAssignOpLen(const Token& t,
	                                        std::size_t close_seg,
	                                        std::size_t close_pos) {
		const auto& close_text = t.segments[close_seg].text;
		if (close_pos + 1 < close_text.size()) {
			if (close_text[close_pos + 1] == '=') return 1;
			if (close_text[close_pos + 1] == '+'
			    && close_pos + 2 < close_text.size()
			    && close_text[close_pos + 2] == '=') {
				return 2;
			}

			return 0;
		}

		if (close_seg + 1 >= t.segments.size()) return 0;
		const auto& nxt = t.segments[close_seg + 1];
		if (nxt.kind != WordSegment::Kind::Literal || nxt.text.empty()) return 0;
		if (nxt.text[0] == '=') return 1;
		if (nxt.text.size() >= 2 && nxt.text[0] == '+' && nxt.text[1] == '=') {
			return 2;
		}

		return 0;
	}

	static void buildSubscriptWord(const Token& t,
	                               std::size_t cur_seg, std::size_t cur_pos,
	                               std::size_t close_seg, std::size_t close_pos,
	                               Word& out_subscript) {
		auto push_literal = [&](std::string text) {
			if (text.empty()) return;
			WordSegment w;
			w.kind = WordSegment::Kind::Literal;
			w.text = std::move(text);
			out_subscript.segments.push_back(std::move(w));
		};
		for (std::size_t k = cur_seg; k <= close_seg; ++k) {
			const auto& seg = t.segments[k];
			if (k == cur_seg && seg.kind == WordSegment::Kind::Literal) {
				std::string slice = (k == close_seg)
					? seg.text.substr(cur_pos, close_pos - cur_pos)
					: seg.text.substr(cur_pos);
				push_literal(std::move(slice));
			} else if (k == close_seg && seg.kind == WordSegment::Kind::Literal) {
				push_literal(seg.text.substr(0, close_pos));
			} else {
				out_subscript.segments.push_back(seg);
			}
		}
	}

	static void buildValueWordAfterSubscript(const Token& t,
	                                         std::size_t close_seg,
	                                         std::size_t close_pos,
	                                         std::size_t op_len,
	                                         Word& out_value) {
		auto push_literal = [&](std::string text) {
			if (text.empty()) return;
			WordSegment w;
			w.kind = WordSegment::Kind::Literal;
			w.text = std::move(text);
			out_value.segments.push_back(std::move(w));
		};
		const auto& close_text = t.segments[close_seg].text;
		if (close_pos + 1 < close_text.size()) {
			push_literal(close_text.substr(close_pos + 1 + op_len));
			for (std::size_t k = close_seg + 1; k < t.segments.size(); ++k)
				out_value.segments.push_back(t.segments[k]);
			return;
		}

		const auto& nxt = t.segments[close_seg + 1];
		push_literal(nxt.text.substr(op_len));
		for (std::size_t k = close_seg + 2; k < t.segments.size(); ++k)
			out_value.segments.push_back(t.segments[k]);
	}

	static void buildSimpleAssignmentValue(const Token& t, std::size_t name_end,
	                                       Assignment& out) {
		const std::string& s0 = t.segments[0].text;
		if (name_end + 1 < s0.size()) {
			WordSegment seg;
			seg.kind = WordSegment::Kind::Literal;
			seg.text = s0.substr(name_end + 1);
			out.value.segments.push_back(std::move(seg));
		}

		for (std::size_t k = 1; k < t.segments.size(); ++k)
			out.value.segments.push_back(t.segments[k]);

		const auto eqpos = t.text.find('=');
		out.value.raw = (eqpos == std::string::npos)
			? std::string()
			: t.text.substr(eqpos + 1);
	}

	bool Parser::tryExtractAssignment(const Token& t, Assignment& out) const {
		if (t.kind != TokKind::Word || t.segments.empty()) return false;
		const auto& first = t.segments[0];
		if (first.kind != WordSegment::Kind::Literal) return false;

		const std::string& s0 = first.text;
		const std::size_t name_end = scanAssignmentNameLength(s0);
		if (name_end == 0) return false;

		out.name = s0.substr(0, name_end);
		out.loc = t.loc;
		out.value.loc = t.loc;

		// `name+=...` append form. Must be checked before `name=...` since
		// `+=` is the longer operator.
		if (name_end + 1 < s0.size()
		    && s0[name_end] == '+' && s0[name_end + 1] == '=') {
			out.append = true;
			// Pretend `=` lives at name_end + 1 so the existing builder
			// peels the value starting at name_end + 2.
			buildSimpleAssignmentValue(t, name_end + 1, out);
			return true;
		}

		if (name_end < s0.size() && s0[name_end] == '=') {
			buildSimpleAssignmentValue(t, name_end, out);
			return true;
		}

		if (name_end >= s0.size() || s0[name_end] != '[') return false;

		const std::size_t cur_seg = 0;
		const std::size_t cur_pos = name_end + 1;
		std::size_t close_seg = 0;
		std::size_t close_pos = 0;
		if (!findSubscriptCloseBracket(t, cur_seg, cur_pos, &close_seg, &close_pos))
			return false;
		const std::size_t op_len = subscriptAssignOpLen(t, close_seg, close_pos);
		if (op_len == 0) return false;

		out.has_subscript = true;
		out.append = (op_len == 2);
		out.subscript.loc = t.loc;
		buildSubscriptWord(t, cur_seg, cur_pos, close_seg, close_pos, out.subscript);
		buildValueWordAfterSubscript(t, close_seg, close_pos, op_len, out.value);

		const std::string marker = (op_len == 2) ? "]+=" : "]=";
		const auto eqpos = t.text.find(marker);
		out.value.raw = (eqpos == std::string::npos)
			? std::string()
			: t.text.substr(eqpos + marker.size());
		return true;
	}

	bool Parser::tryParseRedirection(Redirection& out) {
		std::size_t saved = pos_;
		int fd = -1;
		if (peek().kind == TokKind::IoNumber) {
			if (!parseInt(peek().text, fd)) fd = -1;
			advance();
		}

		if (!atRedirOp()) {
			pos_ = saved;
			return false;
		}

		auto kind = peek().kind;
		auto map_op = [](TokKind k) {
			switch (k) {
			case TokKind::Less:       return RedirOp::Less;
			case TokKind::Great:      return RedirOp::Great;
			case TokKind::DGreat:     return RedirOp::DGreat;
			case TokKind::LessAnd:    return RedirOp::LessAnd;
			case TokKind::GreatAnd:   return RedirOp::GreatAnd;
			case TokKind::LessGreat:  return RedirOp::LessGreat;
			case TokKind::Clobber:    return RedirOp::Clobber;
			case TokKind::AmpGreat:   return RedirOp::AmpGreat;
			case TokKind::AmpDGreat:  return RedirOp::AmpDGreat;
			case TokKind::DLess:      return RedirOp::DLess;
			case TokKind::DLessDash:  return RedirOp::DLessDash;
			case TokKind::TLess:      return RedirOp::TLess;
			default:                  return RedirOp::Less;
			}
			};
		out.op = map_op(kind);
		out.fd = fd;
		advance();
		if (peek().kind != TokKind::Word) {
			error(peek(), "expected word after redirection operator");
			return false;
		}

		const Token& t = peek();
		out.target = tokenToWord(t);
		if (kind == TokKind::DLess || kind == TokKind::DLessDash) {
			out.heredoc_body = t.heredoc_body;
			out.heredoc_quoted = t.heredoc_quoted;
		}

		advance();
		return true;
	}

}  // namespace wbsh

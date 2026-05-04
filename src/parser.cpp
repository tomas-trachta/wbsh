#include "parser.h"

#include <cctype>
#include <cstring>
#include <stdexcept>
#include <utility>

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

	// ---------------------------------------------------------------------------
	// Construction & cursor helpers
	// ---------------------------------------------------------------------------

	Parser::Parser(std::vector<Token> tokens) : toks_(std::move(tokens)) {}

	Parser::Parser(std::vector<Token> tokens, std::string source_text)
		: toks_(std::move(tokens))
		, source_(std::make_shared<const std::string>(std::move(source_text))) {}

	Parser::Parser(std::vector<Token> tokens, std::shared_ptr<const std::string> source)
		: toks_(std::move(tokens)), source_(std::move(source)) {}

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
		// End of the previous token in the stream. If we haven't consumed
		// anything yet, fall back to the current token's offset.
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

	// ---------------------------------------------------------------------------
	// Top-level program
	// ---------------------------------------------------------------------------

	NodePtr Parser::parseProgram() {
		return parseList(/*top_level=*/true);
	}

	NodePtr Parser::parseList(bool /*top_level*/) {
		std::size_t start = srcOffsetHere();
		auto list = std::make_unique<List>();
		list->loc = peek().loc;
		skipNewlines();
		while (!atEnd()) {
			auto andor = parseAndOr();
			if (!andor) break;
			ListItem it;
			it.command = std::move(andor);
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

	// ---------------------------------------------------------------------------
	// And-or
	// ---------------------------------------------------------------------------

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
			auto ao = std::make_unique<AndOr>();
			ao->op = op;
			ao->loc = left->loc;
			ao->left = std::move(left);
			ao->right = std::move(right);
			ao->src_start = start;
			ao->src_end = srcOffsetEnd();
			left = std::move(ao);
		}
		// If no &&/|| chain, left already has its own span.
		if (left && left->src_end == 0) left->src_end = srcOffsetEnd();
		return left;
	}

	// ---------------------------------------------------------------------------
	// Pipeline
	// ---------------------------------------------------------------------------

	NodePtr Parser::parsePipeline() {
		std::size_t start = srcOffsetHere();
		// `time` is a reserved word that prefixes a pipeline (not a command),
		// so it must be parsed here. Only treat it as the keyword when the
		// next token actually begins a command — otherwise `time` alone (or
		// followed by a redirection / `;`) is just an argv[0].
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
		auto pipe = std::make_unique<Pipeline>();
		pipe->bang = bang;
		pipe->timed = timed;
		pipe->loc = first->loc;
		pipe->commands.push_back(std::move(first));
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
			pipe->commands.push_back(std::move(next));
		}
		stampSpan(*pipe, start);
		return pipe;
	}

	// ---------------------------------------------------------------------------
	// Command dispatch
	// ---------------------------------------------------------------------------

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

		// Function definition: name '(' ')' compound_command
		if (peek().kind == TokKind::Word
			&& peek(1).kind == TokKind::LParen
			&& peek(2).kind == TokKind::RParen)
		{
			std::string name = peek().text;
			SourceLoc loc = peek().loc;
			advance(); advance(); advance();   // consume name ( )
			return parseFunctionRest(std::move(name), loc);
		}

		return parseSimpleCommand();
	}

	// ---------------------------------------------------------------------------
	// Compound commands
	// ---------------------------------------------------------------------------

	NodePtr Parser::parseBraceGroup() {
		std::size_t start = srcOffsetHere();
		SourceLoc loc = peek().loc;
		advance();   // consume `{`
		auto body = parseList(false);
		expectReserved("}", "expected `}`");
		auto bg = std::make_unique<BraceGroup>();
		bg->loc = loc;
		bg->body = std::move(body);
		Redirection r;
		while (tryParseRedirection(r)) bg->redirs.push_back(std::move(r));
		stampSpan(*bg, start);
		return bg;
	}

	NodePtr Parser::parseSubshell() {
		std::size_t start = srcOffsetHere();
		SourceLoc loc = peek().loc;
		advance();   // consume `(`
		auto body = parseList(false);
		expect(TokKind::RParen, "expected `)`");
		auto ss = std::make_unique<Subshell>();
		ss->loc = loc;
		ss->body = std::move(body);
		Redirection r;
		while (tryParseRedirection(r)) ss->redirs.push_back(std::move(r));
		stampSpan(*ss, start);
		return ss;
	}

	NodePtr Parser::parseIf() {
		std::size_t start = srcOffsetHere();
		SourceLoc loc = peek().loc;
		advance();   // consume `if`
		auto cond = parseList(false);
		expectReserved("then", "expected `then`");
		auto then_body = parseList(false);

		auto node = std::make_unique<IfClause>();
		node->loc = loc;
		node->branches.push_back({ std::move(cond), std::move(then_body) });

		while (matchReserved("elif")) {
			auto c = parseList(false);
			expectReserved("then", "expected `then` after elif condition");
			auto b = parseList(false);
			node->branches.push_back({ std::move(c), std::move(b) });
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
		advance();   // consume while/until
		auto cond = parseList(false);
		auto body = parseDoGroup();
		auto node = std::make_unique<WhileClause>();
		node->loc = loc;
		node->until = until;
		node->cond = std::move(cond);
		node->body = std::move(body);
		Redirection r;
		while (tryParseRedirection(r)) node->redirs.push_back(std::move(r));
		stampSpan(*node, start);
		return node;
	}

	NodePtr Parser::parseFor() {
		std::size_t start = srcOffsetHere();
		SourceLoc loc = peek().loc;
		advance();   // consume `for`
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
			// Allow optional separator before `do`.
			if (match(TokKind::Semi) || match(TokKind::Newline)) {
				skipNewlines();
			}
		}
		auto body = parseDoGroup();
		auto node = std::make_unique<ForClause>();
		node->loc = loc;
		node->var = std::move(var);
		node->has_in = has_in;
		node->items = std::move(items);
		node->body = std::move(body);
		Redirection r;
		while (tryParseRedirection(r)) node->redirs.push_back(std::move(r));
		stampSpan(*node, start);
		return node;
	}

	NodePtr Parser::parseCase() {
		std::size_t start = srcOffsetHere();
		SourceLoc loc = peek().loc;
		advance();   // consume `case`
		if (peek().kind != TokKind::Word) {
			error(peek(), "expected word after `case`");
			return nullptr;
		}
		Word subject = tokenToWord(advance());
		skipNewlines();
		expectReserved("in", "expected `in`");
		skipNewlines();

		auto node = std::make_unique<CaseClause>();
		node->loc = loc;
		node->subject = std::move(subject);

		while (!atEnd() && !checkReserved("esac")) {
			CaseClause::Item item;
			// Optional opening (
			match(TokKind::LParen);
			// Patterns separated by `|`
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
			else                               item.term = CaseClause::Term::DSemi;   // implicit at esac
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

	std::unique_ptr<DBracketCond::Expr> Parser::parseDBracketExpr() {
		// `||`-separated terms.
		auto left = parseDBracketAnd();
		while (!atDBracketEnd() && check(TokKind::OrIf)) {
			advance();
			auto right = parseDBracketAnd();
			auto e = std::make_unique<DBracketCond::Expr>();
			e->k = DBracketCond::Expr::K::Or;
			e->a = std::move(left);
			e->b = std::move(right);
			left = std::move(e);
		}
		return left;
	}

	std::unique_ptr<DBracketCond::Expr> Parser::parseDBracketAnd() {
		// `&&`-separated factors.
		auto left = parseDBracketUnary();
		while (!atDBracketEnd() && check(TokKind::AndIf)) {
			advance();
			auto right = parseDBracketUnary();
			auto e = std::make_unique<DBracketCond::Expr>();
			e->k = DBracketCond::Expr::K::And;
			e->a = std::move(left);
			e->b = std::move(right);
			left = std::move(e);
		}
		return left;
	}

	std::unique_ptr<DBracketCond::Expr> Parser::parseDBracketUnary() {
		if (matchReserved("!")) {
			auto inner = parseDBracketUnary();
			auto e = std::make_unique<DBracketCond::Expr>();
			e->k = DBracketCond::Expr::K::Not;
			e->a = std::move(inner);
			return e;
		}
		return parseDBracketPrimary();
	}

	std::unique_ptr<DBracketCond::Expr> Parser::parseDBracketPrimary() {
		// Parenthesised sub-expression.
		if (check(TokKind::LParen)) {
			advance();
			auto inner = parseDBracketExpr();
			if (!match(TokKind::RParen)) {
				error(peek(), "expected `)` inside [[ ... ]]");
			}
			return inner;
		}
		auto e = std::make_unique<DBracketCond::Expr>();
		e->k = DBracketCond::Expr::K::Prim;

		// Helper: lookahead to detect a unary operator like -f / -d / ...
		auto isUnaryOp = [](const std::string& s) {
			if (s.size() != 2 || s[0] != '-') return false;
			static const char ops[] = "abcdefghknoprstuwxzGLNOSU";
			for (char c : ops) if (c == s[1]) return true;
			return false;
		};
		auto isBinaryOp = [](const std::string& s) {
			return s == "==" || s == "!=" || s == "=" || s == "=~"
			    || s == "-eq" || s == "-ne" || s == "-lt" || s == "-le"
			    || s == "-gt" || s == "-ge"
			    || s == "-ef" || s == "-nt" || s == "-ot";
		};

		// Unary prefix?
		if (peek().kind == TokKind::Word
		    && peek().segments.size() == 1
		    && peek().segments[0].kind == WordSegment::Kind::Literal
		    && isUnaryOp(peek().segments[0].text)
		    && peek(1).kind == TokKind::Word
		    && !isReserved(peek(1), "]]"))
		{
			// But beware: `-f && something` — the next token is the operand,
			// only treat as unary if the token AFTER the operand is a
			// connective / closer.
			const Token& after = peek(2);
			bool ok = isReserved(after, "]]")
			    || after.kind == TokKind::AndIf
			    || after.kind == TokKind::OrIf
			    || after.kind == TokKind::RParen;
			if (ok) {
				e->op = peek().segments[0].text;
				advance();
				e->lhs = tokenToWord(advance());
				return e;
			}
		}

		if (peek().kind != TokKind::Word) {
			error(peek(), "expected operand in [[ ... ]]");
			return e;
		}
		e->lhs = tokenToWord(advance());

		// Binary operator?
		auto opAsString = [](const Token& t) -> std::string {
			if (t.kind == TokKind::Less)  return "<";
			if (t.kind == TokKind::Great) return ">";
			if (t.kind == TokKind::Word
			    && t.segments.size() == 1
			    && t.segments[0].kind == WordSegment::Kind::Literal) {
				return t.segments[0].text;
			}
			return {};
		};
		std::string opstr = opAsString(peek());
		if (!opstr.empty() && (opstr == "<" || opstr == ">" || isBinaryOp(opstr))
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
		// else: single-word truthiness test (op stays "")
		return e;
	}

	NodePtr Parser::parseDBracket() {
		std::size_t start = srcOffsetHere();
		SourceLoc loc = peek().loc;
		advance();   // consume `[[`
		auto node = std::make_unique<DBracketCond>();
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
		// `loc` already points at the start of the function definition. Use
		// the span up to the current end-of-stream position.
		std::size_t start = loc.offset;
		skipNewlines();
		auto body = parseCommand();
		if (!body) {
			error(peek(), "expected function body");
			return nullptr;
		}
		auto fn = std::make_unique<FunctionDef>();
		fn->loc = loc;
		fn->name = std::move(name);
		fn->body = std::move(body);
		stampSpan(*fn, start);
		// Snapshot the body slice from source so the executor can serialise
		// it later, even after the AST is moved or the source is freed.
		if (fn->body && source_ && !source_->empty()
		    && fn->body->src_end > fn->body->src_start
		    && fn->body->src_end <= source_->size()) {
			fn->body_text = source_->substr(fn->body->src_start,
				fn->body->src_end - fn->body->src_start);
		}
		return fn;
	}

	// ---------------------------------------------------------------------------
	// Simple command
	// ---------------------------------------------------------------------------

	NodePtr Parser::parseSimpleCommand() {
		std::size_t start = srcOffsetHere();
		auto cmd = std::make_unique<SimpleCommand>();
		cmd->loc = peek().loc;
		bool seen_word = false;

		while (true) {
			if (atEnd()) break;
			// Redirection (with or without leading IO_NUMBER).
			if (atRedirOp() || peek().kind == TokKind::IoNumber) {
				Redirection r;
				if (tryParseRedirection(r)) {
					cmd->redirs.push_back(std::move(r));
					continue;
				}
				break;
			}
			if (peek().kind == TokKind::Word) {
				if (!seen_word) {
					Assignment a;
					if (extractAssignment(peek(), a)) {
						advance();
						// `name=(...)` array literal: the empty-value
						// scalar form was just consumed; if the next
						// token is `(`, switch to array mode.
						if (a.value.segments.empty() && !a.has_subscript
						    && peek().kind == TokKind::LParen)
						{
							advance();   // consume `(`
							a.is_array = true;
							skipNewlines();
							while (!atEnd() && peek().kind != TokKind::RParen) {
								if (peek().kind == TokKind::Newline) {
									advance(); continue;
								}
								if (peek().kind != TokKind::Word) {
									error(peek(), "unexpected token in array literal");
									break;
								}
								// Detect [key]=value form by inspecting the
								// raw token text. Bracketed key with `=`
								// somewhere after `]`.
								Word w = tokenToWord(advance());
								Assignment::Keyed item;
								if (!w.raw.empty() && w.raw[0] == '['
								    && !w.segments.empty()
								    && w.segments[0].kind == WordSegment::Kind::Literal)
								{
									const std::string& lit = w.segments[0].text;
									std::size_t close = lit.find(']');
									if (close != std::string::npos
									    && close + 1 < lit.size()
									    && lit[close + 1] == '=')
									{
										std::string key_text = lit.substr(1, close - 1);
										std::string val_text = lit.substr(close + 2);
										WordSegment ks;
										ks.kind = WordSegment::Kind::Literal;
										ks.text = std::move(key_text);
										item.key.segments.push_back(std::move(ks));
										item.has_key = true;
										// Value is the rest of the first
										// segment plus any following segments.
										if (!val_text.empty()) {
											WordSegment vs;
											vs.kind = WordSegment::Kind::Literal;
											vs.text = std::move(val_text);
											item.value.segments.push_back(std::move(vs));
										}
										for (std::size_t k = 1; k < w.segments.size(); ++k) {
											item.value.segments.push_back(w.segments[k]);
										}
										item.value.raw = w.raw;
										a.keyed_items.push_back(std::move(item));
										continue;
									}
								}
								Assignment::Keyed unkeyed;
								unkeyed.value = std::move(w);
								a.keyed_items.push_back(std::move(unkeyed));
							}
							if (!match(TokKind::RParen)) {
								error(peek(), "expected `)` to close array literal");
							}
						}
						cmd->assignments.push_back(std::move(a));
						continue;
					}
				}
				cmd->words.push_back(tokenToWord(advance()));
				seen_word = true;
				continue;
			}
			// Anything else terminates the simple command.
			break;
		}

		if (cmd->words.empty() && cmd->assignments.empty() && cmd->redirs.empty()) {
			return nullptr;
		}
		stampSpan(*cmd, start);
		return cmd;
	}

	bool Parser::extractAssignment(const Token& t, Assignment& out) const {
		if (t.kind != TokKind::Word || t.segments.empty()) return false;
		const auto& first = t.segments[0];
		if (first.kind != WordSegment::Kind::Literal) return false;
		const std::string& s0 = first.text;
		std::size_t i = 0;
		if (i >= s0.size()) return false;
		char c0 = s0[i];
		if (!(std::isalpha(static_cast<unsigned char>(c0)) || c0 == '_')) return false;
		++i;
		while (i < s0.size()
			&& (std::isalnum(static_cast<unsigned char>(s0[i])) || s0[i] == '_'))
			++i;
		std::size_t name_end = i;

		out.name = s0.substr(0, name_end);
		out.loc = t.loc;
		out.value.loc = t.loc;

		auto append_value_seg = [&](WordSegment seg) {
			out.value.segments.push_back(std::move(seg));
		};

		// Simple case: `name=...` with `=` in the first literal segment.
		if (i < s0.size() && s0[i] == '=') {
			if (i + 1 < s0.size()) {
				WordSegment seg;
				seg.kind = WordSegment::Kind::Literal;
				seg.text = s0.substr(i + 1);
				append_value_seg(std::move(seg));
			}
			for (std::size_t k = 1; k < t.segments.size(); ++k)
				append_value_seg(t.segments[k]);
			auto eqpos = t.text.find('=');
			out.value.raw = (eqpos == std::string::npos)
				? std::string()
				: t.text.substr(eqpos + 1);
			return true;
		}

		// Subscripted: `name[...]=...`. The subscript may span multiple
		// segments (`m[$key]=v` lexes as Lit("m["), SimpleVar("key"),
		// Lit("]=v")). Walk segments collecting subscript content until we
		// hit a literal segment containing `]=` (or `]` then `=` in the
		// next literal).
		if (i >= s0.size() || s0[i] != '[') return false;
		std::size_t cur_seg = 0;
		std::size_t cur_pos = i + 1;   // skip the `[`
		out.has_subscript = true;
		out.subscript.loc = t.loc;
		// First literal slice: from cur_pos onward (or until `]`).
		bool found_close = false;
		std::size_t close_seg = 0;
		std::size_t close_pos = 0;
		// Scan literal segments to find the `]`.
		for (std::size_t k = 0; k < t.segments.size(); ++k) {
			const auto& seg = t.segments[k];
			if (seg.kind != WordSegment::Kind::Literal) continue;
			std::size_t start = (k == cur_seg) ? cur_pos : 0;
			auto rb = seg.text.find(']', start);
			if (rb != std::string::npos) {
				close_seg = k;
				close_pos = rb;
				found_close = true;
				break;
			}
		}
		if (!found_close) return false;
		// Verify `]=` (or `]` immediately followed by `=` next).
		const auto& close_text = t.segments[close_seg].text;
		if (close_pos + 1 < close_text.size()) {
			if (close_text[close_pos + 1] != '=') return false;
		} else {
			// `]` is the last char of this literal; next segment must start
			// with `=` (only meaningful if the next segment is literal).
			if (close_seg + 1 >= t.segments.size()) return false;
			const auto& nxt = t.segments[close_seg + 1];
			if (nxt.kind != WordSegment::Kind::Literal) return false;
			if (nxt.text.empty() || nxt.text[0] != '=') return false;
		}

		// Build subscript Word from segments[cur_seg..close_seg], trimming
		// the leading `[` and trailing `]`.
		for (std::size_t k = cur_seg; k <= close_seg; ++k) {
			const auto& seg = t.segments[k];
			if (k == cur_seg && seg.kind == WordSegment::Kind::Literal) {
				std::string slice = (k == close_seg)
				    ? seg.text.substr(cur_pos, close_pos - cur_pos)
				    : seg.text.substr(cur_pos);
				if (!slice.empty()) {
					WordSegment w;
					w.kind = WordSegment::Kind::Literal;
					w.text = std::move(slice);
					out.subscript.segments.push_back(std::move(w));
				}
			} else if (k == close_seg && seg.kind == WordSegment::Kind::Literal) {
				std::string slice = seg.text.substr(0, close_pos);
				if (!slice.empty()) {
					WordSegment w;
					w.kind = WordSegment::Kind::Literal;
					w.text = std::move(slice);
					out.subscript.segments.push_back(std::move(w));
				}
			} else {
				out.subscript.segments.push_back(seg);
			}
		}

		// Build value Word from after `=`.
		if (close_pos + 1 < close_text.size()) {
			// `]=` in same segment; value starts at close_pos + 2.
			std::string slice = close_text.substr(close_pos + 2);
			if (!slice.empty()) {
				WordSegment w;
				w.kind = WordSegment::Kind::Literal;
				w.text = std::move(slice);
				append_value_seg(std::move(w));
			}
			for (std::size_t k = close_seg + 1; k < t.segments.size(); ++k)
				append_value_seg(t.segments[k]);
		} else {
			// `]` ends close_seg; next literal segment starts with `=`.
			const auto& nxt = t.segments[close_seg + 1];
			std::string slice = nxt.text.substr(1);
			if (!slice.empty()) {
				WordSegment w;
				w.kind = WordSegment::Kind::Literal;
				w.text = std::move(slice);
				append_value_seg(std::move(w));
			}
			for (std::size_t k = close_seg + 2; k < t.segments.size(); ++k)
				append_value_seg(t.segments[k]);
		}
		auto eqpos = t.text.find("]=");
		out.value.raw = (eqpos == std::string::npos)
			? std::string()
			: t.text.substr(eqpos + 2);
		return true;
	}

	// ---------------------------------------------------------------------------
	// Redirections
	// ---------------------------------------------------------------------------

	bool Parser::tryParseRedirection(Redirection& out) {
		std::size_t saved = pos_;
		int fd = -1;
		if (peek().kind == TokKind::IoNumber) {
			try { fd = std::stoi(peek().text); }
			catch (...) { fd = -1; }
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

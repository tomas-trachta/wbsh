/**
 * @file parser.cpp
 * @brief Recursive-descent parser from tokens to AST.
 */

#include "parser.h"

#include <cctype>
#include <cstring>
#include <utility>

#include "numparse.h"

namespace wbsh {

	static const char* const kReservedTerminators[] = {
		"then", "elif", "else", "fi", "do", "done", "esac", "in", "}"
	};

	static const char kDBracketUnaryOps[] = "abcdefghknoprstuvwxzGLNOSU";

	namespace parser_detail {

		// Where a `name[subscript]=value` token's subscript starts and
		// ends, as (segment index, offset within that segment) pairs.
		struct SubscriptSpan {
			std::size_t open_seg = 0;
			std::size_t open_pos = 0;
			std::size_t close_seg = 0;
			std::size_t close_pos = 0;
		};

	}  // namespace parser_detail

	using parser_detail::SubscriptSpan;

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
		case Node::Kind::ArithCommand: return "ArithCommand";
		}

		return "?";
	}

	static RedirOp redirOpForToken(TokKind kind) {
		switch (kind) {
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
	}

	static bool isLiteralOnlyWord(const Token& token) {
		return token.kind == TokKind::Word
			&& token.segments.size() == 1
			&& token.segments[0].kind == WordSegment::Kind::Literal;
	}

	static void appendLiteralSegment(Word& word, std::string text) {
		if (text.empty()) return;

		WordSegment segment;
		segment.kind = WordSegment::Kind::Literal;
		segment.text = std::move(text);
		word.segments.push_back(std::move(segment));
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
		const Token& token = toks_[pos_];
		if (pos_ + 1 < toks_.size()) ++pos_;
		return token;
	}

	bool Parser::atEnd() const {
		return peek().kind == TokKind::EndOfInput;
	}

	bool Parser::check(TokKind k) const {
		return peek().kind == k;
	}

	bool Parser::match(TokKind k) {
		if (!check(k)) return false;

		advance();
		return true;
	}

	bool Parser::isReserved(const Token& t, const char* word) const {
		if (!isLiteralOnlyWord(t)) return false;
		return t.segments[0].text == word;
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
		for (const char* word : words) {
			if (checkReserved(word)) return true;
		}

		return false;
	}

	bool Parser::tokenIsReservedTerminator(const Token& t) const {
		for (const char* word : kReservedTerminators) {
			if (isReserved(t, word)) return true;
		}

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

		const Token& previous = toks_[pos_ - 1];
		return previous.loc.offset + previous.text.size();
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

		switch (peek().kind) {
		case TokKind::Newline:
		case TokKind::Semi:
		case TokKind::Amp:
		case TokKind::DSemi:
		case TokKind::SemiAmp:
		case TokKind::DSemiAmp:
		case TokKind::RParen:
			return false;
		default:
			return true;
		}
	}

	bool Parser::atFunctionNameParens() const {
		if (peek().kind != TokKind::Word) return false;
		if (peek(1).kind != TokKind::LParen) return false;
		if (peek(2).kind != TokKind::RParen) return false;

		Assignment probe;
		return !tryExtractAssignment(peek(), probe);
	}

	Word Parser::tokenToWord(const Token& t) const {
		Word word;
		word.segments = t.segments;
		word.raw = t.text;
		word.loc = t.loc;
		return word;
	}

	void Parser::parseTrailingRedirections(std::vector<Redirection>& redirs) {
		Redirection redir;
		while (tryParseRedirection(redir)) redirs.push_back(std::move(redir));
	}

	NodePtr Parser::parseProgram() {
		return parseList(true);
	}

	NodePtr Parser::parseList(bool /*top_level*/) {
		const std::size_t start = srcOffsetHere();
		auto list = arena_.make<List>();
		list->loc = peek().loc;
		skipNewlines();

		while (!atEnd()) {
			auto andor = parseAndOr();
			if (andor == nullptr) break;

			ListItem item;
			item.command = andor;
			const bool had_separator = matchListSeparator(item);
			list->items.push_back(std::move(item));
			if (!had_separator) break;

			skipNewlines();
		}

		stampSpan(*list, start);
		return list;
	}

	bool Parser::matchListSeparator(ListItem& item) {
		if (match(TokKind::Amp)) {
			item.background = true;
			return true;
		}

		return match(TokKind::Semi) || match(TokKind::Newline);
	}

	NodePtr Parser::parseAndOr() {
		const std::size_t start = srcOffsetHere();
		NodePtr left = parsePipeline();
		if (left == nullptr) return nullptr;

		while (check(TokKind::AndIf) || check(TokKind::OrIf)) {
			const auto op = check(TokKind::AndIf) ? AndOr::Op::AndIf : AndOr::Op::OrIf;
			advance();
			skipNewlines();

			auto right = parsePipeline();
			if (right == nullptr) {
				error(peek(), "expected pipeline after && / ||");
				break;
			}

			auto andor = arena_.make<AndOr>();
			andor->op = op;
			andor->loc = left->loc;
			andor->left = left;
			andor->right = right;
			andor->src_start = start;
			andor->src_end = srcOffsetEnd();
			left = andor;
		}

		if (left != nullptr && left->src_end == 0) left->src_end = srcOffsetEnd();
		return left;
	}

	// `time` is the keyword only when a command follows; `time` alone
	// (or before a redirection / `;`) is an ordinary argv[0].
	bool Parser::matchTimePrefix() {
		if (!checkReserved("time")) return false;

		const std::size_t saved = pos_;
		advance();
		if (atCommandStart() || checkReserved("!")) return true;

		pos_ = saved;
		return false;
	}

	NodePtr Parser::parsePipeline() {
		const std::size_t start = srcOffsetHere();
		const bool timed = matchTimePrefix();
		const bool bang = matchReserved("!");

		auto first = parseCommand();
		if (first == nullptr) {
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
			const bool merge_stderr = check(TokKind::PipeAmp);
			advance();
			skipNewlines();

			auto next = parseCommand();
			if (next == nullptr) {
				error(peek(), "expected command after pipe");
				break;
			}

			pipe->stderr_to_stdout.push_back(merge_stderr);
			pipe->commands.push_back(next);
		}

		stampSpan(*pipe, start);
		return pipe;
	}

	NodePtr Parser::parseCommand() {
		if (!atCommandStart()) return nullptr;

		if (checkReserved("{"))        return parseBraceGroup();
		if (checkReserved("if"))       return parseIf();
		if (checkReserved("while"))    return parseWhileUntil(false);
		if (checkReserved("until"))    return parseWhileUntil(true);
		if (checkReserved("for"))      return parseFor();
		if (checkReserved("select"))   return parseSelect();
		if (checkReserved("case"))     return parseCase();
		if (checkReserved("[["))       return parseDBracket();
		if (check(TokKind::DArithCmd)) return parseArithCommand();
		if (checkReserved("function")) return parseFunctionKeyword();
		if (check(TokKind::LParen))    return parseSubshell();

		if (atFunctionNameParens()) {
			std::string name = peek().text;
			const SourceLoc loc = peek().loc;
			advance();
			advance();
			advance();
			return parseFunctionRest(std::move(name), loc);
		}

		return parseSimpleCommand();
	}

	NodePtr Parser::parseFunctionKeyword() {
		const SourceLoc loc = peek().loc;
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

	NodePtr Parser::parseBraceGroup() {
		const std::size_t start = srcOffsetHere();
		const SourceLoc loc = peek().loc;
		advance();

		auto body = parseList(false);
		expectReserved("}", "expected `}`");

		auto group = arena_.make<BraceGroup>();
		group->loc = loc;
		group->body = body;
		parseTrailingRedirections(group->redirs);
		stampSpan(*group, start);
		return group;
	}

	NodePtr Parser::parseSubshell() {
		const std::size_t start = srcOffsetHere();
		const SourceLoc loc = peek().loc;
		advance();

		auto body = parseList(false);
		expect(TokKind::RParen, "expected `)`");

		auto subshell = arena_.make<Subshell>();
		subshell->loc = loc;
		subshell->body = body;
		parseTrailingRedirections(subshell->redirs);
		stampSpan(*subshell, start);
		return subshell;
	}

	NodePtr Parser::parseArithCommand() {
		const std::size_t start = srcOffsetHere();
		auto node = arena_.make<ArithCommand>();
		node->loc = peek().loc;
		node->expr = peek().text;
		advance();

		parseTrailingRedirections(node->redirs);
		stampSpan(*node, start);
		return node;
	}

	NodePtr Parser::parseIf() {
		const std::size_t start = srcOffsetHere();
		const SourceLoc loc = peek().loc;
		advance();

		auto cond = parseList(false);
		expectReserved("then", "expected `then`");
		auto then_body = parseList(false);

		auto node = arena_.make<IfClause>();
		node->loc = loc;
		node->branches.push_back({ cond, then_body });

		while (matchReserved("elif")) {
			auto elif_cond = parseList(false);
			expectReserved("then", "expected `then` after elif condition");
			auto elif_body = parseList(false);
			node->branches.push_back({ elif_cond, elif_body });
		}

		if (matchReserved("else")) node->else_body = parseList(false);

		expectReserved("fi", "expected `fi`");
		parseTrailingRedirections(node->redirs);
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
		const std::size_t start = srcOffsetHere();
		const SourceLoc loc = peek().loc;
		advance();

		auto cond = parseList(false);
		auto body = parseDoGroup();

		auto node = arena_.make<WhileClause>();
		node->loc = loc;
		node->until = until;
		node->cond = cond;
		node->body = body;
		parseTrailingRedirections(node->redirs);
		stampSpan(*node, start);
		return node;
	}

	// Splits the raw body of a `for (( init; cond; update ))` header on its
	// top-level semicolons, respecting nested parens so that command
	// substitutions or subexpressions containing `;` aren't split.
	static std::vector<std::string> splitArithForHeader(const std::string& body) {
		std::vector<std::string> parts;
		std::string part;
		int depth = 0;

		for (char c : body) {
			if (c == '(') ++depth;
			else if (c == ')') --depth;

			if (c == ';' && depth == 0) {
				parts.push_back(std::move(part));
				part.clear();
				continue;
			}

			part.push_back(c);
		}

		parts.push_back(std::move(part));
		return parts;
	}

	static std::string trimArith(const std::string& text) {
		const std::size_t begin = text.find_first_not_of(" \t");
		if (begin == std::string::npos) return "";

		const std::size_t end = text.find_last_not_of(" \t");
		return text.substr(begin, end - begin + 1);
	}

	NodePtr Parser::parseForArith(std::size_t start, SourceLoc loc) {
		const std::vector<std::string> parts = splitArithForHeader(peek().text);
		advance();
		skipNewlines();
		match(TokKind::Semi);
		skipNewlines();

		auto node = arena_.make<ForClause>();
		node->loc = loc;
		node->is_arith = true;
		if (parts.size() != 3) {
			error(peek(), "expected `init; cond; update` inside `for ((...))`");
		} else {
			node->arith_init = trimArith(parts[0]);
			node->arith_cond = trimArith(parts[1]);
			node->arith_update = trimArith(parts[2]);
		}

		node->body = parseDoGroup();
		parseTrailingRedirections(node->redirs);
		stampSpan(*node, start);
		return node;
	}

	// Parses the `var [in word...]` header shared by `for var ...` and
	// `select var ...`, up through (but not including) the `do` group.
	// Returns false (with an error already recorded) if there's no
	// variable name to read.
	bool Parser::parseInWordListHeader(std::string& var, bool& has_in,
	                                   std::vector<Word>& items,
	                                   const char* keyword) {
		if (peek().kind != TokKind::Word) {
			error(peek(), std::string("expected variable name after `") + keyword + "`");
			return false;
		}

		var = peek().text;
		advance();
		skipNewlines();

		has_in = matchReserved("in");
		if (has_in) {
			parseInWordList(items);
			return true;
		}

		if (match(TokKind::Semi) || match(TokKind::Newline)) skipNewlines();
		return true;
	}

	void Parser::parseInWordList(std::vector<Word>& items) {
		while (peek().kind == TokKind::Word && !tokenIsReservedTerminator(peek())) {
			items.push_back(tokenToWord(advance()));
		}

		if (!match(TokKind::Semi) && !match(TokKind::Newline)) {
			if (!checkReserved("do")) error(peek(), "expected `;` or newline after word list");
		}

		skipNewlines();
	}

	NodePtr Parser::parseWordListLoop(std::size_t start, SourceLoc loc,
	                                  const char* keyword, bool is_select) {
		std::string var;
		bool has_in = false;
		std::vector<Word> items;
		if (!parseInWordListHeader(var, has_in, items, keyword)) return nullptr;

		auto body = parseDoGroup();

		auto node = arena_.make<ForClause>();
		node->loc = loc;
		node->var = std::move(var);
		node->has_in = has_in;
		node->items = std::move(items);
		node->body = body;
		node->is_select = is_select;
		parseTrailingRedirections(node->redirs);
		stampSpan(*node, start);
		return node;
	}

	NodePtr Parser::parseFor() {
		const std::size_t start = srcOffsetHere();
		const SourceLoc loc = peek().loc;
		advance();

		if (check(TokKind::DArithCmd)) return parseForArith(start, loc);
		return parseWordListLoop(start, loc, "for", false);
	}

	NodePtr Parser::parseSelect() {
		const std::size_t start = srcOffsetHere();
		const SourceLoc loc = peek().loc;
		advance();

		return parseWordListLoop(start, loc, "select", true);
	}

	NodePtr Parser::parseCase() {
		const std::size_t start = srcOffsetHere();
		const SourceLoc loc = peek().loc;
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
			parseCaseItem(item);
			node->items.push_back(std::move(item));
		}

		expectReserved("esac", "expected `esac`");
		parseTrailingRedirections(node->redirs);
		stampSpan(*node, start);
		return node;
	}

	void Parser::parseCaseItem(CaseClause::Item& item) {
		match(TokKind::LParen);
		while (peek().kind == TokKind::Word) {
			item.patterns.push_back(tokenToWord(advance()));
			if (!match(TokKind::Pipe)) break;
		}

		expect(TokKind::RParen, "expected `)` after case pattern(s)");
		skipNewlines();

		const bool has_body = !checkReserved("esac")
			&& !check(TokKind::DSemi)
			&& !check(TokKind::SemiAmp)
			&& !check(TokKind::DSemiAmp);
		if (has_body) item.body = parseList(false);

		item.term = matchCaseTerminator();
		skipNewlines();
	}

	CaseClause::Term Parser::matchCaseTerminator() {
		if (match(TokKind::DSemi))    return CaseClause::Term::DSemi;
		if (match(TokKind::SemiAmp))  return CaseClause::Term::SemiAmp;
		if (match(TokKind::DSemiAmp)) return CaseClause::Term::DSemiAmp;
		return CaseClause::Term::DSemi;
	}

	bool Parser::atDBracketEnd() const {
		return isReserved(peek(), "]]");
	}

	DBracketCond::Expr* Parser::parseDBracketExpr() {
		auto left = parseDBracketAnd();
		while (!atDBracketEnd() && check(TokKind::OrIf)) {
			advance();
			auto right = parseDBracketAnd();

			auto expr = arena_.make<DBracketCond::Expr>();
			expr->k = DBracketCond::Expr::K::Or;
			expr->a = left;
			expr->b = right;
			left = expr;
		}

		return left;
	}

	DBracketCond::Expr* Parser::parseDBracketAnd() {
		auto left = parseDBracketUnary();
		while (!atDBracketEnd() && check(TokKind::AndIf)) {
			advance();
			auto right = parseDBracketUnary();

			auto expr = arena_.make<DBracketCond::Expr>();
			expr->k = DBracketCond::Expr::K::And;
			expr->a = left;
			expr->b = right;
			left = expr;
		}

		return left;
	}

	DBracketCond::Expr* Parser::parseDBracketUnary() {
		if (!matchReserved("!")) return parseDBracketPrimary();

		auto inner = parseDBracketUnary();
		auto expr = arena_.make<DBracketCond::Expr>();
		expr->k = DBracketCond::Expr::K::Not;
		expr->a = inner;
		return expr;
	}

	static bool isDBracketUnaryOp(const std::string& text) {
		if (text.size() != 2 || text[0] != '-') return false;

		for (char op : kDBracketUnaryOps) {
			if (op == text[1]) return true;
		}

		return false;
	}

	static bool isDBracketBinaryOp(const std::string& text) {
		return text == "==" || text == "!=" || text == "=" || text == "=~"
		    || text == "-eq" || text == "-ne" || text == "-lt" || text == "-le"
		    || text == "-gt" || text == "-ge"
		    || text == "-ef" || text == "-nt" || text == "-ot";
	}

	static std::string dBracketOpAsString(const Token& token) {
		if (token.kind == TokKind::Less)  return "<";
		if (token.kind == TokKind::Great) return ">";
		if (isLiteralOnlyWord(token)) return token.segments[0].text;
		return {};
	}

	bool Parser::atDBracketUnaryTest() const {
		if (!isLiteralOnlyWord(peek())) return false;
		if (!isDBracketUnaryOp(peek().segments[0].text)) return false;
		if (peek(1).kind != TokKind::Word) return false;
		return !isReserved(peek(1), "]]");
	}

	bool Parser::tryParseDBracketUnary(DBracketCond::Expr& e) {
		if (!atDBracketUnaryTest()) return false;

		// `-f && X` — only treat as unary if the operand is followed by a
		// connective / closer; otherwise fall back to single-operand truthiness.
		const Token& after = peek(2);
		const bool operand_closed = isReserved(after, "]]")
		    || after.kind == TokKind::AndIf
		    || after.kind == TokKind::OrIf
		    || after.kind == TokKind::RParen;
		if (!operand_closed) return false;

		e.op = peek().segments[0].text;
		advance();
		e.lhs = tokenToWord(advance());
		return true;
	}

	DBracketCond::Expr* Parser::parseDBracketPrimary() {
		if (check(TokKind::LParen)) {
			advance();
			auto inner = parseDBracketExpr();
			if (!match(TokKind::RParen)) error(peek(), "expected `)` inside [[ ... ]]");
			return inner;
		}

		auto expr = arena_.make<DBracketCond::Expr>();
		expr->k = DBracketCond::Expr::K::Prim;
		if (tryParseDBracketUnary(*expr)) return expr;

		if (peek().kind != TokKind::Word) {
			error(peek(), "expected operand in [[ ... ]]");
			return expr;
		}

		expr->lhs = tokenToWord(advance());

		const std::string op_text = dBracketOpAsString(peek());
		const bool is_binary_op = !op_text.empty()
		    && (op_text == "<" || op_text == ">" || isDBracketBinaryOp(op_text))
		    && !isReserved(peek(), "]]");
		if (!is_binary_op) return expr;

		expr->op = op_text;
		advance();
		if (peek().kind != TokKind::Word) {
			error(peek(), "expected right operand in [[ ... ]]");
			return expr;
		}

		expr->rhs = tokenToWord(advance());
		return expr;
	}

	NodePtr Parser::parseDBracket() {
		const std::size_t start = srcOffsetHere();
		auto node = arena_.make<DBracketCond>();
		node->loc = peek().loc;
		advance();

		if (atDBracketEnd()) {
			error(peek(), "[[: empty conditional expression");
		} else {
			node->root = parseDBracketExpr();
		}

		if (!matchReserved("]]")) error(peek(), "expected `]]`");

		parseTrailingRedirections(node->redirs);
		stampSpan(*node, start);
		return node;
	}

	NodePtr Parser::parseFunctionRest(std::string name, SourceLoc loc) {
		const std::size_t start = loc.offset;
		skipNewlines();

		auto body = parseCommand();
		if (body == nullptr) {
			error(peek(), "expected function body");
			return nullptr;
		}

		auto function = arena_.make<FunctionDef>();
		function->loc = loc;
		function->name = std::move(name);
		function->body = body;
		stampSpan(*function, start);
		captureFunctionBodyText(*function);
		return function;
	}

	void Parser::captureFunctionBodyText(FunctionDef& function) const {
		if (function.body == nullptr) return;
		if (source_ == nullptr || source_->empty()) return;

		const std::size_t body_start = function.body->src_start;
		const std::size_t body_end = function.body->src_end;
		if (body_end <= body_start || body_end > source_->size()) return;

		function.body_text = source_->substr(body_start, body_end - body_start);
	}

	static bool tryParseKeyedArrayItem(const Word& word, Assignment::Keyed& item) {
		if (word.raw.empty() || word.raw[0] != '[') return false;
		if (word.segments.empty()) return false;
		if (word.segments[0].kind != WordSegment::Kind::Literal) return false;

		const std::string& literal = word.segments[0].text;
		const std::size_t close = literal.find(']');
		if (close == std::string::npos) return false;
		if (close + 1 >= literal.size() || literal[close + 1] != '=') return false;

		WordSegment key;
		key.kind = WordSegment::Kind::Literal;
		key.text = literal.substr(1, close - 1);
		item.key.segments.push_back(std::move(key));
		item.has_key = true;

		appendLiteralSegment(item.value, literal.substr(close + 2));
		for (std::size_t k = 1; k < word.segments.size(); ++k) {
			item.value.segments.push_back(word.segments[k]);
		}

		item.value.raw = word.raw;
		return true;
	}

	void Parser::parseArrayLiteralItem(Assignment& a) {
		Word word = tokenToWord(advance());

		Assignment::Keyed keyed;
		if (tryParseKeyedArrayItem(word, keyed)) {
			a.keyed_items.push_back(std::move(keyed));
			return;
		}

		Assignment::Keyed unkeyed;
		unkeyed.value = std::move(word);
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

		if (!match(TokKind::RParen)) error(peek(), "expected `)` to close array literal");
	}

	static bool isEmptyScalarAssignmentSlot(const Assignment& a) {
		return a.value.segments.empty() && !a.has_subscript;
	}

	bool Parser::tryConsumeLeadingAssignment(SimpleCommand& cmd) {
		Assignment assignment;
		if (!tryExtractAssignment(peek(), assignment)) return false;

		advance();
		if (isEmptyScalarAssignmentSlot(assignment) && peek().kind == TokKind::LParen) {
			advance();
			parseArrayLiteralBody(assignment);
		}

		cmd.assignments.push_back(std::move(assignment));
		return true;
	}

	NodePtr Parser::parseSimpleCommand() {
		const std::size_t start = srcOffsetHere();
		auto cmd = arena_.make<SimpleCommand>();
		cmd->loc = peek().loc;
		bool seen_word = false;

		while (!atEnd()) {
			if (atRedirOp() || peek().kind == TokKind::IoNumber) {
				Redirection redir;
				if (!tryParseRedirection(redir)) break;

				cmd->redirs.push_back(std::move(redir));
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

	static std::size_t scanAssignmentNameLength(const std::string& text) {
		if (text.empty()) return 0;

		const unsigned char first = static_cast<unsigned char>(text[0]);
		if (!(std::isalpha(first) || first == '_')) return 0;

		std::size_t length = 1;
		while (length < text.size()
		       && (std::isalnum(static_cast<unsigned char>(text[length])) || text[length] == '_'))
		{
			++length;
		}

		return length;
	}

	static bool findSubscriptCloseBracket(const Token& token, SubscriptSpan& span) {
		for (std::size_t k = 0; k < token.segments.size(); ++k) {
			const auto& segment = token.segments[k];
			if (segment.kind != WordSegment::Kind::Literal) continue;

			const std::size_t from = (k == span.open_seg) ? span.open_pos : 0;
			const std::size_t close = segment.text.find(']', from);
			if (close == std::string::npos) continue;

			span.close_seg = k;
			span.close_pos = close;
			return true;
		}

		return false;
	}

	static std::size_t assignOpLengthAt(const std::string& text, std::size_t at) {
		if (at >= text.size()) return 0;
		if (text[at] == '=') return 1;
		if (text[at] == '+' && at + 1 < text.size() && text[at + 1] == '=') return 2;
		return 0;
	}

	static std::size_t subscriptAssignOpLen(const Token& token, const SubscriptSpan& span) {
		const std::string& close_text = token.segments[span.close_seg].text;
		if (span.close_pos + 1 < close_text.size()) {
			return assignOpLengthAt(close_text, span.close_pos + 1);
		}

		if (span.close_seg + 1 >= token.segments.size()) return 0;

		const auto& next = token.segments[span.close_seg + 1];
		if (next.kind != WordSegment::Kind::Literal) return 0;
		return assignOpLengthAt(next.text, 0);
	}

	static void buildSubscriptWord(const Token& token, const SubscriptSpan& span,
	                               Word& out_subscript) {
		for (std::size_t k = span.open_seg; k <= span.close_seg; ++k) {
			const auto& segment = token.segments[k];
			const bool literal = segment.kind == WordSegment::Kind::Literal;

			if (k == span.open_seg && literal) {
				const std::size_t length = (k == span.close_seg)
					? span.close_pos - span.open_pos
					: std::string::npos;
				appendLiteralSegment(out_subscript, segment.text.substr(span.open_pos, length));
				continue;
			}

			if (k == span.close_seg && literal) {
				appendLiteralSegment(out_subscript, segment.text.substr(0, span.close_pos));
				continue;
			}

			out_subscript.segments.push_back(segment);
		}
	}

	static void appendSegmentsFrom(const Token& token, std::size_t first, Word& out) {
		for (std::size_t k = first; k < token.segments.size(); ++k) {
			out.segments.push_back(token.segments[k]);
		}
	}

	static void buildValueWordAfterSubscript(const Token& token, const SubscriptSpan& span,
	                                         std::size_t op_len, Word& out_value) {
		const std::string& close_text = token.segments[span.close_seg].text;
		if (span.close_pos + 1 < close_text.size()) {
			appendLiteralSegment(out_value, close_text.substr(span.close_pos + 1 + op_len));
			appendSegmentsFrom(token, span.close_seg + 1, out_value);
			return;
		}

		const auto& next = token.segments[span.close_seg + 1];
		appendLiteralSegment(out_value, next.text.substr(op_len));
		appendSegmentsFrom(token, span.close_seg + 2, out_value);
	}

	static void buildSimpleAssignmentValue(const Token& token, std::size_t name_end,
	                                       Assignment& out) {
		const std::string& text = token.segments[0].text;
		if (name_end + 1 < text.size()) appendLiteralSegment(out.value, text.substr(name_end + 1));
		appendSegmentsFrom(token, 1, out.value);

		const std::size_t equals = token.text.find('=');
		out.value.raw = (equals == std::string::npos)
			? std::string()
			: token.text.substr(equals + 1);
	}

	static bool tryExtractSubscriptAssignment(const Token& token, std::size_t name_end,
	                                          Assignment& out) {
		SubscriptSpan span;
		span.open_seg = 0;
		span.open_pos = name_end + 1;
		if (!findSubscriptCloseBracket(token, span)) return false;

		const std::size_t op_len = subscriptAssignOpLen(token, span);
		if (op_len == 0) return false;

		out.has_subscript = true;
		out.append = (op_len == 2);
		out.subscript.loc = token.loc;
		buildSubscriptWord(token, span, out.subscript);
		buildValueWordAfterSubscript(token, span, op_len, out.value);

		const std::string marker = (op_len == 2) ? "]+=" : "]=";
		const std::size_t equals = token.text.find(marker);
		out.value.raw = (equals == std::string::npos)
			? std::string()
			: token.text.substr(equals + marker.size());
		return true;
	}

	bool Parser::tryExtractAssignment(const Token& t, Assignment& out) const {
		if (t.kind != TokKind::Word || t.segments.empty()) return false;

		const auto& first = t.segments[0];
		if (first.kind != WordSegment::Kind::Literal) return false;

		const std::string& text = first.text;
		const std::size_t name_end = scanAssignmentNameLength(text);
		if (name_end == 0) return false;

		out.name = text.substr(0, name_end);
		out.loc = t.loc;
		out.value.loc = t.loc;

		// `+=` is the longer operator, so the append form is tried first.
		if (name_end + 1 < text.size() && text[name_end] == '+' && text[name_end + 1] == '=') {
			out.append = true;
			// The value builder peels everything past its `=` position, so
			// pointing it one past the `+` yields the text after `+=`.
			buildSimpleAssignmentValue(t, name_end + 1, out);
			return true;
		}

		if (name_end < text.size() && text[name_end] == '=') {
			buildSimpleAssignmentValue(t, name_end, out);
			return true;
		}

		if (name_end >= text.size() || text[name_end] != '[') return false;
		return tryExtractSubscriptAssignment(t, name_end, out);
	}

	bool Parser::tryParseRedirection(Redirection& out) {
		const std::size_t saved = pos_;
		int fd = -1;
		if (peek().kind == TokKind::IoNumber) {
			if (!parseInt(peek().text, fd)) fd = -1;
			advance();
		}

		if (!atRedirOp()) {
			pos_ = saved;
			return false;
		}

		const TokKind kind = peek().kind;
		out.op = redirOpForToken(kind);
		out.fd = fd;
		advance();
		if (peek().kind != TokKind::Word) {
			error(peek(), "expected word after redirection operator");
			return false;
		}

		const Token& target = peek();
		out.target = tokenToWord(target);
		if (kind == TokKind::DLess || kind == TokKind::DLessDash) {
			out.heredoc_body = target.heredoc_body;
			out.heredoc_quoted = target.heredoc_quoted;
		}

		advance();
		return true;
	}

}  // namespace wbsh

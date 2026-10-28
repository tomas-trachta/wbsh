#include "bc_internal.h"

#include <cstring>
#include <utility>

#include "bc_lexer.h"

namespace wbsh {

	namespace bc_detail {

		static const char* const kKeywords[] = {
			"auto", "break", "continue", "define", "else", "for", "halt", "ibase", "if", "last",
			"length", "limits", "obase", "print", "quit", "read", "return", "scale", "sqrt",
			"warranty", "while",
		};

		static bool isKeyword(const std::string& word) {
			for (const char* keyword : kKeywords) {
				if (word == keyword) return true;
			}

			return word == ".";
		}

		static ExprPtr makeExpr(ExprKind kind) {
			auto expr = std::make_unique<Expr>();
			expr->kind = kind;
			return expr;
		}

		static ExprPtr makeUnary(ExprKind kind, ExprPtr operand) {
			if (!operand) return nullptr;

			ExprPtr expr = makeExpr(kind);
			expr->left = std::move(operand);
			return expr;
		}

		static ExprPtr makeBinary(ExprKind kind, BinaryOp op, ExprPtr left, ExprPtr right) {
			if (!right) return nullptr;

			ExprPtr expr = makeExpr(kind);
			expr->op = op;
			expr->left = std::move(left);
			expr->right = std::move(right);
			return expr;
		}

		static StmtPtr makeStmt(StmtKind kind) {
			auto stmt = std::make_unique<Stmt>();
			stmt->kind = kind;
			return stmt;
		}

		static BinaryOp assignOpOf(const std::string& text) {
			if (text == "+=") return BinaryOp::Add;
			if (text == "-=") return BinaryOp::Sub;
			if (text == "*=") return BinaryOp::Mul;
			if (text == "/=") return BinaryOp::Div;
			if (text == "%=") return BinaryOp::Mod;
			if (text == "^=") return BinaryOp::Pow;
			return BinaryOp::None;
		}

		static bool isAssignOp(const Token& token) {
			if (token.kind != TokKind::Op) return false;
			return token.text == "=" || assignOpOf(token.text) != BinaryOp::None;
		}

		static bool relationOf(const Token& token, RelOp& out) {
			if (token.kind != TokKind::Op) return false;

			static const std::pair<const char*, RelOp> kRelations[] = {
				{ "==", RelOp::Eq }, { "!=", RelOp::Ne }, { "<", RelOp::Lt },
				{ "<=", RelOp::Le }, { ">", RelOp::Gt }, { ">=", RelOp::Ge },
			};
			for (const auto& entry : kRelations) {
				if (token.text != entry.first) continue;
				out = entry.second;
				return true;
			}

			return false;
		}

		struct ParserMark {
			BcLexer::Mark lex;
			Token look;
			bool have_look;
		};

		struct ParserImpl {
			BcState& state;
			BcSource& source;
			BcLexer lex;
			Token look;
			bool have_look = false;
			bool failed = false;
			bool quit = false;
			int loop_depth = 0;
			int brace_depth = 0;
			bool in_function = false;

			ParserImpl(BcState& bc_state, BcSource& src) : state(bc_state), source(src), lex(src) {}

			const Token& peek() {
				if (!have_look) {
					look = lex.next();
					have_look = true;
				}

				return look;
			}

			Token take() {
				peek();
				have_look = false;
				return look;
			}

			bool isOp(const char* text) {
				const Token& token = peek();
				return token.kind == TokKind::Op && token.text == text;
			}

			bool isWord(const char* text) {
				const Token& token = peek();
				return token.kind == TokKind::Name && token.text == text;
			}

			bool isKind(TokKind kind) {
				return peek().kind == kind;
			}

			bool accept(const char* text) {
				if (!isOp(text)) return false;
				take();
				return true;
			}

			bool fail(const std::string& message = "syntax error") {
				if (failed) return false;

				failed = true;
				const Token& token = peek();
				const std::string text = token.kind == TokKind::Error ? token.text : message;
				reportError(state, source.name + " " + std::to_string(token.line) + ": " + text);
				return false;
			}

			bool expect(const char* text) {
				return accept(text) || fail();
			}

			bool expectSemicolon() {
				if (!isKind(TokKind::Semicolon)) return fail();
				take();
				return true;
			}

			ParserMark mark() {
				return { lex.mark(), look, have_look };
			}

			void reset(const ParserMark& saved) {
				lex.reset(saved.lex);
				look = saved.look;
				have_look = saved.have_look;
			}

			void skipNewlines() {
				while (isKind(TokKind::Newline)) take();
			}

			void skipSeparators() {
				while (isKind(TokKind::Newline) || isKind(TokKind::Semicolon)) take();
			}

			bool atSeparator() {
				return isKind(TokKind::Newline) || isKind(TokKind::Semicolon) || isKind(TokKind::End);
			}

			bool atStatementEnd() {
				return atSeparator() || isOp("}") || isWord("else");
			}

			ExprPtr parseExpression();
			ExprPtr parseOr();
			ExprPtr parseAnd();
			ExprPtr parseRelation();
			ExprPtr parseAdditive();
			ExprPtr parseMultiplicative();
			ExprPtr parsePower();
			ExprPtr parseUnary();
			ExprPtr parsePrimary();
			ExprPtr parseGroup();
			ExprPtr parseNameExpr();
			ExprPtr parseBuiltinCall(ExprKind kind);
			ExprPtr parseRead();
			ExprPtr parseCall(int id);
			bool parseCallArg(CallArg& arg);
			ExprPtr parseElement(int id);
			ExprPtr finishNamed(ExprPtr target);
			ExprPtr parseNamedOperand();

			StmtPtr parseStatement();
			StmtPtr parseKeywordStatement(const std::string& word);
			StmtPtr parseExpressionStatement();
			StmtPtr parseLoopControl(StmtKind kind);
			StmtPtr parseReturn();
			StmtPtr parseQuit();
			StmtPtr parseSimple(StmtKind kind);
			StmtPtr parsePrint();
			StmtPtr parseBlock();
			bool parseStatementsUntilBrace(StmtList& list);
			StmtPtr parseIf();
			void parseElse(Stmt& stmt);
			StmtPtr parseWhile();
			StmtPtr parseFor();
			ExprPtr parseOptional(bool absent);
			StmtPtr parseControlledBody();

			bool parseDefine();
			bool parseDefineHeader(Function& fn);
			bool parseParams(std::vector<Param>& params, bool allow_ref);
			bool parseAutos(Function& fn);
			bool parseFunctionBody(Function& fn);
			void installFunction(const std::shared_ptr<Function>& fn);

			ParseStatus parseItem(StmtList& item);
			ParseStatus parseStatementLine(StmtList& item);
			ParseStatus failureStatus();
			void recover();
		};

		ExprPtr ParserImpl::parseExpression() {
			return parseOr();
		}

		ExprPtr ParserImpl::parseOr() {
			ExprPtr left = parseAnd();
			while (left && accept("||")) left = makeBinary(ExprKind::Or, BinaryOp::None, std::move(left), parseAnd());
			return left;
		}

		ExprPtr ParserImpl::parseAnd() {
			ExprPtr left = parseRelation();
			while (left && accept("&&")) {
				left = makeBinary(ExprKind::And, BinaryOp::None, std::move(left), parseRelation());
			}

			return left;
		}

		ExprPtr ParserImpl::parseRelation() {
			ExprPtr left = parseAdditive();
			RelOp relation = RelOp::Eq;
			while (left && relationOf(peek(), relation)) {
				take();
				left = makeBinary(ExprKind::Relation, BinaryOp::None, std::move(left), parseAdditive());
				if (left) left->relation = relation;
			}

			return left;
		}

		ExprPtr ParserImpl::parseAdditive() {
			ExprPtr left = parseMultiplicative();
			while (left && (isOp("+") || isOp("-"))) {
				const BinaryOp op = take().text == "+" ? BinaryOp::Add : BinaryOp::Sub;
				left = makeBinary(ExprKind::Binary, op, std::move(left), parseMultiplicative());
			}

			return left;
		}

		ExprPtr ParserImpl::parseMultiplicative() {
			ExprPtr left = parsePower();
			while (left && (isOp("*") || isOp("/") || isOp("%"))) {
				const std::string text = take().text;
				const BinaryOp op = text == "*" ? BinaryOp::Mul : text == "/" ? BinaryOp::Div : BinaryOp::Mod;
				left = makeBinary(ExprKind::Binary, op, std::move(left), parsePower());
			}

			return left;
		}

		ExprPtr ParserImpl::parsePower() {
			ExprPtr base = parseUnary();
			if (!base || !accept("^")) return base;
			return makeBinary(ExprKind::Binary, BinaryOp::Pow, std::move(base), parsePower());
		}

		ExprPtr ParserImpl::parseUnary() {
			if (accept("-")) return makeUnary(ExprKind::Negate, parseUnary());
			if (accept("!")) return makeUnary(ExprKind::Not, parseRelation());
			if (accept("++")) return makeUnary(ExprKind::PreIncrement, parseNamedOperand());
			if (accept("--")) return makeUnary(ExprKind::PreDecrement, parseNamedOperand());
			return parsePrimary();
		}

		ExprPtr ParserImpl::parseNamedOperand() {
			const Token& token = peek();
			if (token.kind != TokKind::Name || token.text == "length" || token.text == "sqrt" || token.text == "read") {
				fail();
				return nullptr;
			}

			ExprPtr target = parseNameExpr();
			if (!target) return nullptr;

			const ExprKind kind = target->kind;
			const bool named = kind == ExprKind::Variable || kind == ExprKind::Element || kind == ExprKind::Scale
				|| kind == ExprKind::Ibase || kind == ExprKind::Obase || kind == ExprKind::Last;
			if (named) return target;

			fail();
			return nullptr;
		}

		ExprPtr ParserImpl::parsePrimary() {
			const Token& token = peek();
			if (token.kind == TokKind::Number) {
				ExprPtr expr = makeExpr(ExprKind::Number);
				expr->literal = take().text;
				return expr;
			}

			if (token.kind == TokKind::Name) return parseNameExpr();
			if (isOp("(")) return parseGroup();

			fail();
			return nullptr;
		}

		ExprPtr ParserImpl::parseGroup() {
			take();
			ExprPtr expr = parseExpression();
			if (!expr || !expect(")")) return nullptr;

			expr->parenthesized = true;
			return expr;
		}

		ExprPtr ParserImpl::parseBuiltinCall(ExprKind kind) {
			if (!expect("(")) return nullptr;

			ExprPtr argument = parseExpression();
			if (!argument || !expect(")")) return nullptr;
			return makeUnary(kind, std::move(argument));
		}

		ExprPtr ParserImpl::parseRead() {
			if (!expect("(") || !expect(")")) return nullptr;
			return makeExpr(ExprKind::Read);
		}

		ExprPtr ParserImpl::parseNameExpr() {
			const std::string word = take().text;
			if (word == "length") return parseBuiltinCall(ExprKind::Length);
			if (word == "sqrt") return parseBuiltinCall(ExprKind::Sqrt);
			if (word == "read") return parseRead();
			if (word == "scale" && isOp("(")) return parseBuiltinCall(ExprKind::ScaleOf);
			if (word == "scale") return finishNamed(makeExpr(ExprKind::Scale));
			if (word == "ibase") return finishNamed(makeExpr(ExprKind::Ibase));
			if (word == "obase") return finishNamed(makeExpr(ExprKind::Obase));
			if (word == "last" || word == ".") return finishNamed(makeExpr(ExprKind::Last));
			if (isKeyword(word)) {
				fail();
				return nullptr;
			}

			const int id = state.intern(word);
			if (accept("(")) return parseCall(id);
			if (accept("[")) return parseElement(id);

			ExprPtr variable = makeExpr(ExprKind::Variable);
			variable->name_id = id;
			return finishNamed(std::move(variable));
		}

		ExprPtr ParserImpl::parseElement(int id) {
			ExprPtr index = parseExpression();
			if (!index || !expect("]")) return nullptr;

			ExprPtr element = makeUnary(ExprKind::Element, std::move(index));
			element->name_id = id;
			return finishNamed(std::move(element));
		}

		ExprPtr ParserImpl::finishNamed(ExprPtr target) {
			if (isAssignOp(peek())) {
				const BinaryOp op = assignOpOf(take().text);
				return makeBinary(ExprKind::Assign, op, std::move(target), parseAdditive());
			}

			if (accept("++")) return makeUnary(ExprKind::PostIncrement, std::move(target));
			if (accept("--")) return makeUnary(ExprKind::PostDecrement, std::move(target));
			return target;
		}

		bool ParserImpl::parseCallArg(CallArg& arg) {
			const Token& token = peek();
			if (token.kind == TokKind::Name && !isKeyword(token.text)) {
				const ParserMark saved = mark();
				const std::string name = take().text;
				if (accept("[") && accept("]")) {
					arg.array_id = state.intern(name);
					return true;
				}

				reset(saved);
			}

			arg.value = parseExpression();
			return arg.value != nullptr;
		}

		ExprPtr ParserImpl::parseCall(int id) {
			ExprPtr call = makeExpr(ExprKind::Call);
			call->name_id = id;
			if (accept(")")) return call;

			for (;;) {
				CallArg arg;
				if (!parseCallArg(arg)) return nullptr;
				call->args.push_back(std::move(arg));
				if (accept(",")) continue;
				if (!expect(")")) return nullptr;
				return call;
			}
		}

		StmtPtr ParserImpl::parseStatement() {
			const Token& token = peek();
			switch (token.kind) {
				case TokKind::Newline:
				case TokKind::Semicolon:
				case TokKind::End:
					return makeStmt(StmtKind::Empty);
				case TokKind::String: {
					StmtPtr stmt = makeStmt(StmtKind::String);
					stmt->text = take().text;
					return stmt;
				}
				case TokKind::Name:
					return parseKeywordStatement(token.text);
				case TokKind::Error:
					fail();
					return nullptr;
				default:
					if (isOp("{")) return parseBlock();
					return parseExpressionStatement();
			}
		}

		StmtPtr ParserImpl::parseKeywordStatement(const std::string& word) {
			if (word == "if") return parseIf();
			if (word == "while") return parseWhile();
			if (word == "for") return parseFor();
			if (word == "break") return parseLoopControl(StmtKind::Break);
			if (word == "continue") return parseLoopControl(StmtKind::Continue);
			if (word == "return") return parseReturn();
			if (word == "quit") return parseQuit();
			if (word == "halt") return parseSimple(StmtKind::Halt);
			if (word == "limits") return parseSimple(StmtKind::Limits);
			if (word == "warranty") return parseSimple(StmtKind::Warranty);
			if (word == "print") return parsePrint();
			if (word == "define" || word == "auto" || word == "else") {
				fail();
				return nullptr;
			}

			return parseExpressionStatement();
		}

		StmtPtr ParserImpl::parseExpressionStatement() {
			ExprPtr expr = parseExpression();
			if (!expr) return nullptr;

			StmtPtr stmt = makeStmt(StmtKind::Expression);
			stmt->expr = std::move(expr);
			return stmt;
		}

		StmtPtr ParserImpl::parseLoopControl(StmtKind kind) {
			if (loop_depth == 0) {
				fail(kind == StmtKind::Break ? "break outside a for/while" : "continue outside a for/while");
				return nullptr;
			}

			return parseSimple(kind);
		}

		StmtPtr ParserImpl::parseReturn() {
			if (!in_function) {
				fail("return outside a function");
				return nullptr;
			}

			take();
			StmtPtr stmt = makeStmt(StmtKind::Return);
			if (atStatementEnd()) return stmt;

			stmt->expr = parseExpression();
			return stmt->expr ? std::move(stmt) : nullptr;
		}

		// POSIX: quit leaves bc as soon as it is read, even inside an
		// untaken branch or a function body.
		StmtPtr ParserImpl::parseQuit() {
			quit = true;
			failed = true;
			return nullptr;
		}

		StmtPtr ParserImpl::parseSimple(StmtKind kind) {
			take();
			return makeStmt(kind);
		}

		StmtPtr ParserImpl::parsePrint() {
			take();
			StmtPtr stmt = makeStmt(StmtKind::Print);
			for (;;) {
				PrintItem item;
				if (isKind(TokKind::String)) {
					item.text = take().text;
				} else {
					item.value = parseExpression();
					if (!item.value) return nullptr;
				}

				stmt->items.push_back(std::move(item));
				if (!accept(",")) return stmt;
			}
		}

		bool ParserImpl::parseStatementsUntilBrace(StmtList& list) {
			for (;;) {
				skipSeparators();
				if (isOp("}")) return true;
				if (isKind(TokKind::End)) return fail();

				StmtPtr stmt = parseStatement();
				if (!stmt) return false;
				list.push_back(std::move(stmt));
				if (!atSeparator() && !isOp("}")) return fail();
			}
		}

		StmtPtr ParserImpl::parseBlock() {
			take();
			++brace_depth;
			StmtPtr stmt = makeStmt(StmtKind::Block);
			const bool ok = parseStatementsUntilBrace(stmt->list) && expect("}");
			--brace_depth;
			return ok ? std::move(stmt) : nullptr;
		}

		StmtPtr ParserImpl::parseControlledBody() {
			skipNewlines();
			if (isOp("}")) {
				fail();
				return nullptr;
			}

			return parseStatement();
		}

		// Inside braces an `else` may also start the next line; at top level
		// a newline ends the statement, as in GNU bc.
		void ParserImpl::parseElse(Stmt& stmt) {
			if (!isWord("else") && brace_depth > 0) {
				const ParserMark saved = mark();
				skipNewlines();
				if (!isWord("else")) reset(saved);
			}

			if (!isWord("else")) return;

			take();
			stmt.else_body = parseControlledBody();
		}

		StmtPtr ParserImpl::parseIf() {
			take();
			StmtPtr stmt = makeStmt(StmtKind::If);
			if (!expect("(")) return nullptr;

			stmt->expr = parseExpression();
			if (!stmt->expr || !expect(")")) return nullptr;

			stmt->body = parseControlledBody();
			if (!stmt->body) return nullptr;

			parseElse(*stmt);
			return failed ? nullptr : std::move(stmt);
		}

		StmtPtr ParserImpl::parseWhile() {
			take();
			StmtPtr stmt = makeStmt(StmtKind::While);
			if (!expect("(")) return nullptr;

			stmt->expr = parseExpression();
			if (!stmt->expr || !expect(")")) return nullptr;

			++loop_depth;
			stmt->body = parseControlledBody();
			--loop_depth;
			return stmt->body ? std::move(stmt) : nullptr;
		}

		ExprPtr ParserImpl::parseOptional(bool absent) {
			if (absent) return nullptr;
			return parseExpression();
		}

		StmtPtr ParserImpl::parseFor() {
			take();
			StmtPtr stmt = makeStmt(StmtKind::For);
			if (!expect("(")) return nullptr;

			stmt->init = parseOptional(isKind(TokKind::Semicolon));
			if (failed || !expectSemicolon()) return nullptr;

			stmt->expr = parseOptional(isKind(TokKind::Semicolon));
			if (failed || !expectSemicolon()) return nullptr;

			stmt->step = parseOptional(isOp(")"));
			if (failed || !expect(")")) return nullptr;

			++loop_depth;
			stmt->body = parseControlledBody();
			--loop_depth;
			return stmt->body ? std::move(stmt) : nullptr;
		}

		bool ParserImpl::parseParams(std::vector<Param>& params, bool allow_ref) {
			for (;;) {
				Param param;
				param.by_ref = allow_ref && accept("*");

				const Token& token = peek();
				if (token.kind != TokKind::Name || isKeyword(token.text)) return fail();

				param.id = state.intern(take().text);
				param.is_array = accept("[");
				if (param.is_array && !expect("]")) return false;
				if (param.by_ref && !param.is_array) return fail();

				params.push_back(param);
				if (!accept(",")) return true;
			}
		}

		bool ParserImpl::parseDefineHeader(Function& fn) {
			take();
			if (isWord("void")) {
				const ParserMark saved = mark();
				take();
				fn.is_void = isKind(TokKind::Name);
				if (!fn.is_void) reset(saved);
			}

			const Token& token = peek();
			if (token.kind != TokKind::Name || isKeyword(token.text)) return fail();

			fn.id = state.intern(take().text);
			if (!expect("(")) return false;
			if (!isOp(")") && !parseParams(fn.params, true)) return false;
			if (!expect(")")) return false;

			skipNewlines();
			return expect("{");
		}

		bool ParserImpl::parseAutos(Function& fn) {
			skipSeparators();
			while (isWord("auto")) {
				take();
				if (!parseParams(fn.autos, false)) return false;
				if (!isKind(TokKind::Newline) && !isKind(TokKind::Semicolon)) return fail();
				skipSeparators();
			}

			return true;
		}

		bool ParserImpl::parseFunctionBody(Function& fn) {
			in_function = true;
			++brace_depth;
			const bool ok = parseAutos(fn) && parseStatementsUntilBrace(fn.body) && expect("}");
			--brace_depth;
			in_function = false;
			return ok;
		}

		void ParserImpl::installFunction(const std::shared_ptr<Function>& fn) {
			const auto index = static_cast<std::size_t>(fn->id);
			if (state.functions.size() <= index) state.functions.resize(index + 1);
			state.functions[index] = fn;
		}

		bool ParserImpl::parseDefine() {
			auto fn = std::make_shared<Function>();
			if (!parseDefineHeader(*fn) || !parseFunctionBody(*fn)) return false;

			installFunction(fn);
			return true;
		}

		void ParserImpl::recover() {
			for (;;) {
				const Token token = take();
				if (token.kind == TokKind::Newline || token.kind == TokKind::End) return;
			}
		}

		ParseStatus ParserImpl::failureStatus() {
			if (quit) return ParseStatus::Quit;

			state.had_error = true;
			recover();
			loop_depth = 0;
			brace_depth = 0;
			in_function = false;
			return ParseStatus::Error;
		}

		ParseStatus ParserImpl::parseStatementLine(StmtList& item) {
			for (;;) {
				if (isKind(TokKind::Newline)) {
					take();
					return ParseStatus::Item;
				}

				if (isKind(TokKind::End)) return ParseStatus::Item;
				if (isKind(TokKind::Semicolon)) {
					take();
					continue;
				}

				StmtPtr stmt = parseStatement();
				if (!stmt) return failureStatus();
				item.push_back(std::move(stmt));
				if (atSeparator()) continue;

				fail();
				return failureStatus();
			}
		}

		ParseStatus ParserImpl::parseItem(StmtList& item) {
			item.clear();
			failed = false;
			if (!have_look) lex.compact();
			if (isKind(TokKind::End)) return ParseStatus::End;
			if (!isWord("define")) return parseStatementLine(item);

			return parseDefine() ? ParseStatus::Defined : failureStatus();
		}

		BcParser::BcParser(BcState& state, BcSource& source)
			: impl_(std::make_unique<ParserImpl>(state, source)) {}

		BcParser::~BcParser() = default;

		ParseStatus BcParser::next(StmtList& item) {
			return impl_->parseItem(item);
		}

		bool parseNumberText(BcState& state, const std::string& text, ExprPtr& out) {
			BcSource source;
			source.text = text;
			source.name = "(read)";

			ParserImpl parser(state, source);
			out = parser.parseExpression();
			if (!out) return false;

			parser.skipNewlines();
			return parser.isKind(TokKind::End) || parser.fail();
		}

	}  // namespace bc_detail

}  // namespace wbsh

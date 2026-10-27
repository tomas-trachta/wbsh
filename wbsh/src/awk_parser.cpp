/**
 * @file awk_parser.cpp
 * @brief Recursive-descent parser from awk tokens to the syntax tree in awk_internal.h.
 */

#include <string>
#include <unordered_map>
#include <vector>

#include "awk_internal.h"
#include "awk_lexer.h"

namespace wbsh {

	namespace awk_detail {

		static const char* const kSpecialVarNames[kSpecialCount] = {
			"NF", "NR", "FNR", "FS", "OFS", "ORS", "RS", "FILENAME", "SUBSEP", "CONVFMT", "OFMT",
			"RSTART", "RLENGTH", "ENVIRON", "ARGC", "ARGV",
		};

		const char* specialVarName(int slot) {
			return kSpecialVarNames[slot];
		}

		Builtin builtinFunction(const std::string& name) {
			static const std::unordered_map<std::string, Builtin> kBuiltins = {
				{ "length", Builtin::Length }, { "substr", Builtin::Substr },
				{ "index", Builtin::Index }, { "split", Builtin::Split },
				{ "sub", Builtin::Sub }, { "gsub", Builtin::Gsub },
				{ "match", Builtin::Match }, { "sprintf", Builtin::Sprintf },
				{ "sin", Builtin::Sin }, { "cos", Builtin::Cos }, { "atan2", Builtin::Atan2 },
				{ "exp", Builtin::Exp }, { "log", Builtin::Log }, { "sqrt", Builtin::Sqrt },
				{ "int", Builtin::Int }, { "rand", Builtin::Rand }, { "srand", Builtin::Srand },
				{ "tolower", Builtin::Tolower }, { "toupper", Builtin::Toupper },
				{ "system", Builtin::System }, { "close", Builtin::Close },
				{ "fflush", Builtin::Fflush },
			};

			const auto it = kBuiltins.find(name);
			return it == kBuiltins.end() ? Builtin::None : it->second;
		}

		bool isBuiltinFunction(const std::string& name) {
			return builtinFunction(name) != Builtin::None;
		}

		static Op assignOp(TokenKind kind) {
			switch (kind) {
				case TokenKind::Assign:        return Op::Assign;
				case TokenKind::PlusAssign:    return Op::AddAssign;
				case TokenKind::MinusAssign:   return Op::SubAssign;
				case TokenKind::StarAssign:    return Op::MulAssign;
				case TokenKind::SlashAssign:   return Op::DivAssign;
				case TokenKind::PercentAssign: return Op::ModAssign;
				case TokenKind::CaretAssign:   return Op::PowAssign;
				default:                       return Op::None;
			}
		}

		static Op relationalOp(TokenKind kind) {
			switch (kind) {
				case TokenKind::Lt: return Op::Lt;
				case TokenKind::Le: return Op::Le;
				case TokenKind::Gt: return Op::Gt;
				case TokenKind::Ge: return Op::Ge;
				case TokenKind::Eq: return Op::Eq;
				case TokenKind::Ne: return Op::Ne;
				default:            return Op::None;
			}
		}

		static Op multiplicativeOp(TokenKind kind) {
			switch (kind) {
				case TokenKind::Star:    return Op::Mul;
				case TokenKind::Slash:   return Op::Div;
				case TokenKind::Percent: return Op::Mod;
				default:                 return Op::None;
			}
		}

		static Op unaryOp(TokenKind kind) {
			switch (kind) {
				case TokenKind::Not:   return Op::Not;
				case TokenKind::Minus: return Op::Negate;
				case TokenKind::Plus:  return Op::UnaryPlus;
				case TokenKind::Inc:   return Op::PreIncrement;
				case TokenKind::Dec:   return Op::PreDecrement;
				default:               return Op::None;
			}
		}

		static bool startsConcatOperand(TokenKind kind) {
			switch (kind) {
				case TokenKind::Number: case TokenKind::String: case TokenKind::Identifier:
				case TokenKind::FuncName: case TokenKind::Dollar: case TokenKind::LParen:
				case TokenKind::Not:    case TokenKind::Minus:  case TokenKind::Plus:
				case TokenKind::Inc:    case TokenKind::Dec:    case TokenKind::Regex:
					return true;
				default:
					return false;
			}
		}

		static bool isSpecialPattern(const AwkPattern& pattern) {
			return pattern.kind == AwkPattern::Begin || pattern.kind == AwkPattern::End;
		}

		static bool isLvalueKind(ExprKind kind) {
			return kind == ExprKind::Var || kind == ExprKind::ArrayRef || kind == ExprKind::Field;
		}

		struct Parser {
			Lexer lex;
			std::string error_msg;
			AwkProgram* program = nullptr;
			Arena* arena = nullptr;
			std::unordered_map<std::string, int> global_slots;
			const std::vector<std::string>* params = nullptr;
			std::vector<Expr*> user_calls;
			// In a print argument list an unparenthesised '>' is a redirection.
			bool gt_ends_expr = false;

			explicit Parser(const std::string& text) : lex(text) {}

			bool failed() const {
				return !error_msg.empty();
			}

			void err(const std::string& message) {
				if (error_msg.empty()) {
					error_msg = "awk: parse error: " + message
						+ " (line " + std::to_string(lex.peek().line) + ")";
				}

				lex.abortToEof();
			}

			bool peekIs(TokenKind kind) {
				return lex.peek().kind == kind;
			}

			bool acceptIf(TokenKind kind) {
				if (!peekIs(kind)) return false;

				lex.next();
				return true;
			}

			void expect(TokenKind kind, const char* message) {
				if (!peekIs(kind)) err(message);
				lex.next();
			}

			void skipTerminators() {
				while (peekIs(TokenKind::Semi) || peekIs(TokenKind::Newline)) lex.next();
			}

			void skipNewlines() {
				while (peekIs(TokenKind::Newline)) lex.next();
			}

			bool atStatementEnd() {
				return peekIs(TokenKind::Semi) || peekIs(TokenKind::Newline)
					|| peekIs(TokenKind::RBrace) || peekIs(TokenKind::End);
			}

			bool atPrintEnd() {
				return atStatementEnd() || peekIs(TokenKind::Gt)
					|| peekIs(TokenKind::Append) || peekIs(TokenKind::Pipe);
			}

			Stmt* newStmt(StmtKind kind) {
				Stmt* stmt = arena->make<Stmt>();
				stmt->kind = kind;
				return stmt;
			}

			Expr* newExpr(ExprKind kind) {
				Expr* expr = arena->make<Expr>();
				expr->kind = kind;
				return expr;
			}

			Expr* newBinary(Op op, Expr* left, Expr* right) {
				Expr* expr = newExpr(ExprKind::Binary);
				expr->op = op;
				expr->first = left;
				expr->second = right;
				return expr;
			}

			int globalSlot(const std::string& name) {
				const auto it = global_slots.find(name);
				if (it != global_slots.end()) return it->second;

				const int slot = static_cast<int>(program->global_names.size());
				program->global_names.push_back(name);
				global_slots[name] = slot;
				return slot;
			}

			void registerSpecialVars() {
				for (int slot = 0; slot < kSpecialCount; ++slot) globalSlot(specialVarName(slot));
			}

			void resolveVariable(Expr& expr) {
				if (params != nullptr) {
					for (std::size_t i = 0; i < params->size(); ++i) {
						if ((*params)[i] != expr.name) continue;

						expr.local = true;
						expr.slot = static_cast<int>(i);
						return;
					}
				}

				expr.slot = globalSlot(expr.name);
			}

			Expr* newVariable(ExprKind kind, const std::string& name) {
				Expr* expr = newExpr(kind);
				expr->name = name;
				resolveVariable(*expr);
				return expr;
			}

			Stmt* implicitPrintAction() {
				Stmt* block = newStmt(StmtKind::Block);
				block->children.push_back(newStmt(StmtKind::Print));
				return block;
			}

			void parseProgram(AwkProgram& target) {
				program = &target;
				arena = &target.arena;
				registerSpecialVars();

				skipTerminators();
				while (!peekIs(TokenKind::End) && !failed()) {
					parseItem();
					skipTerminators();
				}

				if (!failed()) linkUserCalls();
			}

			void parseItem() {
				if (peekIs(TokenKind::KwFunction)) {
					parseFunction();
					return;
				}

				AwkRule rule;
				parsePattern(rule.pattern);
				if (failed()) return;

				if (peekIs(TokenKind::LBrace)) {
					rule.action = parseBlock();
				} else if (isSpecialPattern(rule.pattern)) {
					err("BEGIN and END need an action");
					return;
				} else {
					rule.action = implicitPrintAction();
				}

				program->rules.push_back(std::move(rule));
			}

			void parseParamList(std::vector<std::string>& out) {
				skipNewlines();
				while (peekIs(TokenKind::Identifier)) {
					out.push_back(lex.next().text);
					skipNewlines();
					if (!acceptIf(TokenKind::Comma)) break;
					skipNewlines();
				}

				expect(TokenKind::RParen, "expected ')' after the parameter list");
			}

			void parseFunction() {
				lex.next();
				const Token name = lex.next();
				if (name.kind != TokenKind::Identifier && name.kind != TokenKind::FuncName) {
					err("expected a function name");
					return;
				}

				if (program->functions.count(name.text) != 0 || isBuiltinFunction(name.text)) {
					err("function '" + name.text + "' defined twice");
					return;
				}

				FunctionDef* def = arena->make<FunctionDef>();
				def->name = name.text;
				program->functions[name.text] = def;

				expect(TokenKind::LParen, "expected '(' after the function name");
				parseParamList(def->params);
				skipNewlines();

				params = &def->params;
				def->body = parseBlock();
				params = nullptr;
			}

			void linkUserCalls() {
				for (Expr* call : user_calls) {
					const auto it = program->functions.find(call->name);
					if (it == program->functions.end()) {
						err("function '" + call->name + "' is not defined");
						return;
					}

					call->function = it->second;
				}
			}

			void parsePattern(AwkPattern& pattern) {
				if (acceptIf(TokenKind::KwBegin)) {
					pattern.kind = AwkPattern::Begin;
					return;
				}

				if (acceptIf(TokenKind::KwEnd)) {
					pattern.kind = AwkPattern::End;
					return;
				}

				if (peekIs(TokenKind::LBrace)) {
					pattern.kind = AwkPattern::Always;
					return;
				}

				pattern.first = parseExpr();
				if (!acceptIf(TokenKind::Comma)) {
					pattern.kind = AwkPattern::Condition;
					return;
				}

				skipNewlines();
				pattern.second = parseExpr();
				pattern.kind = AwkPattern::Range;
			}

			Stmt* parseBlock() {
				expect(TokenKind::LBrace, "expected '{'");
				Stmt* block = newStmt(StmtKind::Block);

				skipTerminators();
				while (!peekIs(TokenKind::RBrace) && !peekIs(TokenKind::End)) {
					block->children.push_back(parseStmt());
					skipTerminators();
				}

				expect(TokenKind::RBrace, "expected '}'");
				return block;
			}

			void parseExprList(std::vector<Expr*>& out) {
				out.push_back(parseExpr());
				while (acceptIf(TokenKind::Comma)) {
					skipNewlines();
					out.push_back(parseExpr());
				}
			}

			void parseNestedExprList(std::vector<Expr*>& out) {
				const bool outer_gt_ends_expr = gt_ends_expr;
				gt_ends_expr = false;
				parseExprList(out);
				gt_ends_expr = outer_gt_ends_expr;
			}

			Expr* parseRedirectTarget() {
				gt_ends_expr = true;
				Expr* target = parseConcat();
				gt_ends_expr = false;
				return target;
			}

			void parsePrintRedirect(Stmt& stmt) {
				if (acceptIf(TokenKind::Gt)) {
					stmt.redirect = Redirect::Truncate;
				} else if (acceptIf(TokenKind::Append)) {
					stmt.redirect = Redirect::Append;
				} else if (acceptIf(TokenKind::Pipe)) {
					stmt.redirect = Redirect::Pipe;
				} else {
					return;
				}

				stmt.redirect_target = parseRedirectTarget();
			}

			// `print (a, b) > f` lists its arguments in parentheses; `print (a)(b)`
			// merely starts with a group, so the list form is only a guess
			// that is undone unless the print ends right after the ')'.
			bool tryParenthesisedPrintArgs(std::vector<Expr*>& out) {
				const Lexer::Mark start = lex.mark();
				lex.next();

				std::vector<Expr*> args;
				parseNestedExprList(args);
				if (!failed() && acceptIf(TokenKind::RParen) && atPrintEnd()) {
					out = std::move(args);
					return true;
				}

				lex.rewind(start);
				error_msg.clear();
				return false;
			}

			void parsePrintArgs(std::vector<Expr*>& out) {
				if (peekIs(TokenKind::LParen) && tryParenthesisedPrintArgs(out)) return;

				gt_ends_expr = true;
				parseExprList(out);
				gt_ends_expr = false;
			}

			Stmt* parsePrintStmt(TokenKind keyword) {
				lex.next();
				const bool formatted = keyword == TokenKind::KwPrintf;
				Stmt* stmt = newStmt(formatted ? StmtKind::Printf : StmtKind::Print);
				if (!atPrintEnd()) parsePrintArgs(stmt->exprs);
				parsePrintRedirect(*stmt);
				return stmt;
			}

			Stmt* parseKeywordStmt(StmtKind kind) {
				lex.next();
				return newStmt(kind);
			}

			Stmt* parseStmtWithOptionalValue(StmtKind kind) {
				lex.next();
				Stmt* stmt = newStmt(kind);
				if (!atStatementEnd()) stmt->exprs.push_back(parseExpr());
				return stmt;
			}

			Stmt* parseDeleteStmt() {
				lex.next();
				if (!peekIs(TokenKind::Identifier)) {
					err("expected an array name after 'delete'");
					return newStmt(StmtKind::Empty);
				}

				Stmt* stmt = newStmt(StmtKind::Delete);
				stmt->exprs.push_back(parseVariableOrElement(lex.next().text));
				return stmt;
			}

			Stmt* parseExprStmt() {
				Stmt* stmt = newStmt(StmtKind::ExprStmt);
				stmt->exprs.push_back(parseExpr());
				return stmt;
			}

			Stmt* parseStmt() {
				switch (lex.peek().kind) {
					case TokenKind::Semi:       return parseKeywordStmt(StmtKind::Empty);
					case TokenKind::LBrace:     return parseBlock();
					case TokenKind::KwIf:       return parseIf();
					case TokenKind::KwWhile:    return parseWhile();
					case TokenKind::KwDo:       return parseDoWhile();
					case TokenKind::KwFor:      return parseFor();
					case TokenKind::KwBreak:    return parseKeywordStmt(StmtKind::Break);
					case TokenKind::KwContinue: return parseKeywordStmt(StmtKind::Continue);
					case TokenKind::KwNext:     return parseKeywordStmt(StmtKind::Next);
					case TokenKind::KwExit:     return parseStmtWithOptionalValue(StmtKind::Exit);
					case TokenKind::KwReturn:   return parseReturnStmt();
					case TokenKind::KwDelete:   return parseDeleteStmt();
					case TokenKind::KwPrint:    return parsePrintStmt(TokenKind::KwPrint);
					case TokenKind::KwPrintf:   return parsePrintStmt(TokenKind::KwPrintf);
					default:                    return parseExprStmt();
				}
			}

			Stmt* parseReturnStmt() {
				if (params == nullptr) {
					err("'return' outside a function");
					return newStmt(StmtKind::Empty);
				}

				return parseStmtWithOptionalValue(StmtKind::Return);
			}

			Expr* parseParenCondition(const char* open_message) {
				expect(TokenKind::LParen, open_message);
				Expr* cond = parseExpr();
				expect(TokenKind::RParen, "expected ')'");
				return cond;
			}

			Stmt* parseBody() {
				skipNewlines();
				return parseStmt();
			}

			// `if (c) stmt; else ...` may put a terminator before the else.
			bool acceptElse() {
				const Lexer::Mark start = lex.mark();
				skipTerminators();
				if (acceptIf(TokenKind::KwElse)) return true;

				lex.rewind(start);
				return false;
			}

			Stmt* parseIf() {
				lex.next();
				Stmt* stmt = newStmt(StmtKind::If);
				stmt->exprs.push_back(parseParenCondition("expected '(' after if"));
				stmt->children.push_back(parseBody());
				if (acceptElse()) stmt->children.push_back(parseBody());
				return stmt;
			}

			Stmt* parseWhile() {
				lex.next();
				Stmt* stmt = newStmt(StmtKind::While);
				stmt->exprs.push_back(parseParenCondition("expected '('"));
				if (acceptIf(TokenKind::Semi)) {
					stmt->children.push_back(newStmt(StmtKind::Empty));
					return stmt;
				}

				stmt->children.push_back(parseBody());
				return stmt;
			}

			Stmt* parseDoWhile() {
				lex.next();
				Stmt* body = parseBody();

				skipTerminators();
				expect(TokenKind::KwWhile, "expected 'while' after do-body");
				Expr* cond = parseParenCondition("expected '('");

				Stmt* stmt = newStmt(StmtKind::DoWhile);
				stmt->exprs.push_back(cond);
				stmt->children.push_back(body);
				return stmt;
			}

			Stmt* parseForIn(const Token& loop_var) {
				if (!peekIs(TokenKind::Identifier)) err("expected array name in 'for in'");
				const Token array_name = lex.next();
				expect(TokenKind::RParen, "expected ')'");

				Stmt* stmt = newStmt(StmtKind::ForIn);
				stmt->exprs.push_back(newVariable(ExprKind::Var, loop_var.text));
				stmt->exprs.push_back(newVariable(ExprKind::Var, array_name.text));
				stmt->children.push_back(parseBody());
				return stmt;
			}

			Expr* parseOptionalExpr(TokenKind terminator) {
				skipNewlines();
				if (peekIs(terminator)) return nullptr;
				return parseExpr();
			}

			Stmt* parseFor() {
				lex.next();
				expect(TokenKind::LParen, "expected '('");
				if (peekIs(TokenKind::Identifier)) {
					Token loop_var = lex.next();
					if (acceptIf(TokenKind::KwIn)) return parseForIn(loop_var);
					lex.unread(std::move(loop_var));
				}

				Stmt* stmt = newStmt(StmtKind::For);
				stmt->init = parseOptionalExpr(TokenKind::Semi);
				expect(TokenKind::Semi, "expected ';'");
				stmt->cond = parseOptionalExpr(TokenKind::Semi);
				expect(TokenKind::Semi, "expected ';'");
				stmt->step = parseOptionalExpr(TokenKind::RParen);
				expect(TokenKind::RParen, "expected ')'");
				stmt->children.push_back(parseBody());
				return stmt;
			}

			Expr* parseExpr() {
				return parseTernary();
			}

			Expr* parseTernaryTail(Expr* cond) {
				skipNewlines();
				Expr* then_value = parseTernary();
				skipNewlines();
				expect(TokenKind::Colon, "expected ':'");
				skipNewlines();
				Expr* else_value = parseTernary();

				Expr* expr = newExpr(ExprKind::Ternary);
				expr->first = cond;
				expr->second = then_value;
				expr->third = else_value;
				return expr;
			}

			Expr* parseAssignmentTail(Expr* target, Op op) {
				if (!isLvalueKind(target->kind)) {
					err("assignment to something that is not a variable");
					return target;
				}

				lex.next();
				skipNewlines();
				Expr* expr = newExpr(ExprKind::Assign);
				expr->op = op;
				expr->first = target;
				expr->second = parseTernary();
				return expr;
			}

			Expr* parseTernary() {
				Expr* left = parseLogicOr();
				if (acceptIf(TokenKind::Question)) return parseTernaryTail(left);

				const Op assign_op = assignOp(lex.peek().kind);
				if (assign_op != Op::None) return parseAssignmentTail(left, assign_op);
				return left;
			}

			Expr* parseLogicOr() {
				Expr* left = parseLogicAnd();
				while (acceptIf(TokenKind::Or)) {
					skipNewlines();
					left = newBinary(Op::Or, left, parseLogicAnd());
				}

				return left;
			}

			Expr* parseLogicAnd() {
				Expr* left = parseInTest();
				while (acceptIf(TokenKind::And)) {
					skipNewlines();
					left = newBinary(Op::And, left, parseInTest());
				}

				return left;
			}

			Expr* parseInTest() {
				Expr* subscript = parseMatch();
				while (acceptIf(TokenKind::KwIn)) {
					if (!peekIs(TokenKind::Identifier)) err("expected array name after 'in'");

					Expr* expr = newVariable(ExprKind::ArrayInTest, lex.next().text);
					expr->first = subscript;
					subscript = expr;
				}

				return subscript;
			}

			Expr* parseMatch() {
				Expr* left = parseRel();
				while (peekIs(TokenKind::Match) || peekIs(TokenKind::NoMatch)) {
					const Op op = peekIs(TokenKind::Match) ? Op::Match : Op::NoMatch;
					lex.next();
					left = newBinary(op, left, parseRel());
				}

				return left;
			}

			Expr* parseRel() {
				Expr* left = parsePipeGetlines(parseConcat());
				for (;;) {
					const Op op = relationalOp(lex.peek().kind);
					if (op == Op::None) return left;
					if (gt_ends_expr && peekIs(TokenKind::Gt)) return left;

					lex.next();
					left = newBinary(op, left, parseConcat());
				}
			}

			// `cmd | getline [var]`: the '|' is only a getline pipe when getline follows.
			Expr* parsePipeGetlines(Expr* left) {
				while (peekIs(TokenKind::Pipe)) {
					const Lexer::Mark start = lex.mark();
					lex.next();
					if (!acceptIf(TokenKind::KwGetline)) {
						lex.rewind(start);
						return left;
					}

					left = parseGetlineTail(GetlineSource::Command, left);
				}

				return left;
			}

			// Juxtaposition is string concatenation: `a b` means a "" b.
			Expr* parseConcat() {
				Expr* left = parseAdd();
				while (startsConcatOperand(lex.peek().kind)) {
					left = newBinary(Op::Concat, left, parseAdd());
				}

				return left;
			}

			Expr* parseAdd() {
				Expr* left = parseMul();
				while (peekIs(TokenKind::Plus) || peekIs(TokenKind::Minus)) {
					const Op op = peekIs(TokenKind::Plus) ? Op::Add : Op::Sub;
					lex.next();
					left = newBinary(op, left, parseMul());
				}

				return left;
			}

			Expr* parseMul() {
				Expr* left = parseUnary();
				for (;;) {
					const Op op = multiplicativeOp(lex.peek().kind);
					if (op == Op::None) return left;

					lex.next();
					left = newBinary(op, left, parseUnary());
				}
			}

			Expr* newUnary(Op op, Expr* operand) {
				Expr* expr = newExpr(ExprKind::Unary);
				expr->op = op;
				expr->first = operand;
				return expr;
			}

			// Unary minus binds looser than '^': -2^2 is -4.
			Expr* parseUnary() {
				const TokenKind kind = lex.peek().kind;
				const Op op = unaryOp(kind);
				if (op == Op::None) return parseExp();

				lex.next();
				if (kind == TokenKind::Inc || kind == TokenKind::Dec) {
					Expr* target = parsePostfix();
					if (!isLvalueKind(target->kind)) err("++ and -- need a variable");
					return newUnary(op, target);
				}

				return newUnary(op, parseUnary());
			}

			Expr* parseExp() {
				Expr* base = parsePostfix();
				if (!acceptIf(TokenKind::Caret)) return base;

				Expr* exponent = peekIs(TokenKind::Minus) || peekIs(TokenKind::Plus)
					|| peekIs(TokenKind::Not) ? parseUnary() : parseExp();
				return newBinary(Op::Pow, base, exponent);
			}

			Expr* parsePostfix() {
				Expr* operand = parsePrimary();
				if (operand == nullptr || !isLvalueKind(operand->kind)) return operand;

				while (peekIs(TokenKind::Inc) || peekIs(TokenKind::Dec)) {
					const Op op = peekIs(TokenKind::Inc) ? Op::PostIncrement : Op::PostDecrement;
					lex.next();

					Expr* expr = newExpr(ExprKind::PostIncDec);
					expr->op = op;
					expr->first = operand;
					operand = expr;
				}

				return operand;
			}

			bool startsGetlineTarget() {
				if (peekIs(TokenKind::Dollar)) return true;
				return peekIs(TokenKind::Identifier) && !isBuiltinFunction(lex.peek().text);
			}

			Expr* parseGetlineTarget() {
				if (acceptIf(TokenKind::Dollar)) return parseFieldRef();
				return parseVariableOrElement(lex.next().text);
			}

			Expr* parseGetlineTail(GetlineSource source, Expr* command) {
				Expr* expr = newExpr(ExprKind::Getline);
				expr->source = source;
				expr->third = command;
				if (startsGetlineTarget()) expr->first = parseGetlineTarget();
				if (source == GetlineSource::Main && acceptIf(TokenKind::Lt)) {
					expr->source = GetlineSource::File;
					expr->second = parsePostfix();
				}

				return expr;
			}

			Expr* newBuiltinCall(const std::string& name) {
				Expr* expr = newExpr(ExprKind::Call);
				expr->name = name;
				expr->builtin = builtinFunction(name);
				return expr;
			}

			Expr* parseBuiltinCall(const std::string& name) {
				Expr* expr = newBuiltinCall(name);
				if (!peekIs(TokenKind::RParen)) parseNestedExprList(expr->args);
				expect(TokenKind::RParen, "expected ')'");
				return expr;
			}

			Expr* parseUserCall(const std::string& name) {
				expect(TokenKind::LParen, "expected '('");
				Expr* expr = newExpr(ExprKind::UserCall);
				expr->name = name;
				skipNewlines();
				if (!peekIs(TokenKind::RParen)) parseNestedExprList(expr->args);
				skipNewlines();
				expect(TokenKind::RParen, "expected ')'");
				user_calls.push_back(expr);
				return expr;
			}

			Expr* parseFunctionName(const std::string& name) {
				if (!isBuiltinFunction(name)) return parseUserCall(name);

				lex.next();
				return parseBuiltinCall(name);
			}

			Expr* parseArrayRef(const std::string& name) {
				Expr* expr = newVariable(ExprKind::ArrayRef, name);
				parseNestedExprList(expr->args);
				expect(TokenKind::RBracket, "expected ']'");
				return expr;
			}

			Expr* parseVariableOrElement(const std::string& name) {
				if (acceptIf(TokenKind::LBracket)) return parseArrayRef(name);
				return newVariable(ExprKind::Var, name);
			}

			Expr* parseIdentifier(const std::string& name) {
				if (!isBuiltinFunction(name)) return parseVariableOrElement(name);
				if (acceptIf(TokenKind::LParen)) return parseBuiltinCall(name);

				if (name != "length") err("'" + name + "' needs an argument list");
				return newBuiltinCall(name);
			}

			Expr* parseLiteral(ExprKind kind, const Token& token) {
				Expr* expr = newExpr(kind);
				if (kind == ExprKind::Number) expr->number = token.number;
				else expr->text = token.text;
				return expr;
			}

			// `$i++` is ($i)++, while `$++i` and `$-1` take a unary operand.
			Expr* parseFieldRef() {
				Expr* expr = newExpr(ExprKind::Field);
				const bool unary_index = unaryOp(lex.peek().kind) != Op::None;
				expr->first = unary_index ? parseUnary() : parsePrimary();
				return expr;
			}

			Expr* parseMultiSubscriptInTest(std::vector<Expr*> subscripts) {
				expect(TokenKind::KwIn, "expected 'in' after subscript list");
				if (!peekIs(TokenKind::Identifier)) err("expected array name after 'in'");

				Expr* expr = newVariable(ExprKind::ArrayInTest, lex.next().text);
				expr->args = std::move(subscripts);
				return expr;
			}

			Expr* parseGroup() {
				std::vector<Expr*> inner;
				parseNestedExprList(inner);
				expect(TokenKind::RParen, "expected ')'");
				if (inner.size() > 1) return parseMultiSubscriptInTest(std::move(inner));

				Expr* expr = newExpr(ExprKind::Group);
				expr->first = inner[0];
				return expr;
			}

			Expr* parsePrimary() {
				const Token token = lex.peek();
				switch (token.kind) {
					case TokenKind::Number:
						lex.next();
						return parseLiteral(ExprKind::Number, token);
					case TokenKind::String:
						lex.next();
						return parseLiteral(ExprKind::String, token);
					case TokenKind::Regex:
						lex.next();
						return parseLiteral(ExprKind::Regex, token);
					case TokenKind::Dollar:
						lex.next();
						return parseFieldRef();
					case TokenKind::LParen:
						lex.next();
						return parseGroup();
					case TokenKind::KwGetline:
						lex.next();
						return parseGetlineTail(GetlineSource::Main, nullptr);
					case TokenKind::FuncName:
						lex.next();
						return parseFunctionName(token.text);
					case TokenKind::Identifier:
						lex.next();
						return parseIdentifier(token.text);
					default:
						err("unexpected token");
						return newExpr(ExprKind::String);
				}
			}
		};

		bool parseAwkProgram(const std::string& text, AwkProgram& program, std::string& error) {
			Parser parser(text);
			parser.parseProgram(program);
			error = parser.error_msg;
			return !parser.failed();
		}

	}  // namespace awk_detail

}  // namespace wbsh

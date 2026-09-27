/**
 * @file awk.cpp
 * @brief Built-in awk(1): lexer, recursive-descent parser, and tree-walking interpreter.
 */

#include "awk.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "arena.h"
#include "executor.h"
#include "numparse.h"
#include "pathconv.h"
#include "regexutil.h"

namespace wbsh {

	namespace awk_detail {

		// A single space as FS is special in awk: fields are split on runs
		// of blanks and leading/trailing blanks are ignored.
		static const char* const kWhitespaceSeparator = " ";
		static const char kDefaultSubsep = '\x1c';
		static const double kIntegerPrintLimit = 1e16;
		static const int kUsageError = 2;

		static bool isWhitespaceSeparator(const std::string& separator) {
			return separator == kWhitespaceSeparator;
		}

		static bool isBlank(char c) {
			return c == ' ' || c == '\t';
		}

		struct AwkValue {
			double number = 0.0;
			std::string text;
			bool is_number = false;
			bool is_string = false;

			AwkValue() = default;

			static AwkValue num(double value) {
				AwkValue result;
				result.number = value;
				result.is_number = true;
				return result;
			}

			static AwkValue str(std::string value) {
				AwkValue result;
				result.text = std::move(value);
				result.is_string = true;
				return result;
			}

			double asNumber() const {
				if (is_number) return number;
				if (!is_string) return 0.0;

				double parsed = 0.0;
				parseDouble(text, parsed);
				return parsed;
			}

			std::string asString() const {
				if (is_string) return text;
				if (!is_number) return std::string();

				if (number == std::floor(number) && std::abs(number) < kIntegerPrintLimit) {
					char buffer[32];
					std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(number));
					return buffer;
				}

				char buffer[64];
				std::snprintf(buffer, sizeof(buffer), "%.6g", number);
				return buffer;
			}

			bool truthy() const {
				if (!is_string) return number != 0.0;
				if (text.empty()) return false;

				double parsed = 0.0;
				if (parseDouble(text, parsed)) return parsed != 0.0;
				return true;
			}
		};

		static AwkValue boolValue(bool value) {
			return AwkValue::num(value ? 1.0 : 0.0);
		}

		struct Expr;
		struct Stmt;

		enum class ExprKind {
			Number, String, Regex, Var,
			Field,
			ArrayRef,
			ArrayInTest,
			Unary,
			PostIncDec,
			Binary,
			Ternary,
			Assign,
			FieldAssign,
			Call,
			Getline,
			Group,
		};

		struct Expr {
			ExprKind kind;
			double number = 0;
			std::string text;
			std::string name;
			std::string op;
			Expr* first = nullptr;
			Expr* second = nullptr;
			Expr* third = nullptr;
			std::vector<Expr*> args;
			mutable std::shared_ptr<std::regex> compiled;
			mutable bool compile_failed = false;
		};

		enum class StmtKind {
			Empty, Print, Printf, ExprStmt, Block, If, While, DoWhile, For, ForIn,
			Break, Continue, Next, Exit, Delete, Return,
		};

		enum class Redirect { None, Truncate, Append, Pipe };

		struct Stmt {
			StmtKind kind;
			std::vector<Stmt*> children;
			std::vector<Expr*> exprs;
			std::string loop_var;
			std::string array_name;
			Expr* init = nullptr;
			Expr* cond = nullptr;
			Expr* step = nullptr;
			Expr* redirect_target = nullptr;
			Redirect redirect = Redirect::None;
		};

		struct AwkPattern {
			enum Kind { Begin, End, Always, Condition, Range };
			Kind kind = Always;
			Expr* first = nullptr;
			Expr* second = nullptr;
		};

		struct AwkRule {
			AwkPattern pattern;
			Stmt* action = nullptr;
			bool in_range = false;
		};

		struct AwkProgram {
			std::vector<AwkRule> rules;
			Arena arena;
		};

		enum class TokenKind {
			End, Number, String, Regex, Identifier,
			Plus, Minus, Star, Slash, Percent, Caret,
			Assign, PlusAssign, MinusAssign, StarAssign, SlashAssign, PercentAssign, CaretAssign,
			Eq, Ne, Lt, Le, Gt, Ge,
			And, Or, Not, Match, NoMatch,
			Inc, Dec,
			LParen, RParen, LBrace, RBrace, LBracket, RBracket,
			Semi, Comma, Newline, Question, Colon,
			Dollar,
			Append,
			Pipe,
			KwBegin, KwEnd, KwIf, KwElse, KwWhile, KwDo, KwFor, KwIn,
			KwBreak, KwContinue, KwNext, KwExit, KwDelete, KwPrint, KwPrintf,
			KwFunction, KwReturn, KwGetline,
		};

		struct Token {
			TokenKind kind = TokenKind::End;
			std::string text;
			double number = 0.0;
			int line = 1;
		};

		static TokenKind identifierKind(const std::string& word) {
			static const std::unordered_map<std::string, TokenKind> kKeywords = {
				{ "BEGIN", TokenKind::KwBegin }, { "END", TokenKind::KwEnd },
				{ "if", TokenKind::KwIf }, { "else", TokenKind::KwElse },
				{ "while", TokenKind::KwWhile }, { "do", TokenKind::KwDo },
				{ "for", TokenKind::KwFor }, { "in", TokenKind::KwIn },
				{ "break", TokenKind::KwBreak }, { "continue", TokenKind::KwContinue },
				{ "next", TokenKind::KwNext }, { "exit", TokenKind::KwExit },
				{ "delete", TokenKind::KwDelete }, { "print", TokenKind::KwPrint },
				{ "printf", TokenKind::KwPrintf }, { "function", TokenKind::KwFunction },
				{ "return", TokenKind::KwReturn }, { "getline", TokenKind::KwGetline },
			};

			const auto it = kKeywords.find(word);
			if (it == kKeywords.end()) return TokenKind::Identifier;
			return it->second;
		}

		static char unescapeStringChar(char escaped, std::string& out) {
			switch (escaped) {
			case 'n':  return '\n';
			case 't':  return '\t';
			case 'r':  return '\r';
			case '\\': return '\\';
			case '"':  return '"';
			case '/':  return '/';
			default:
				out.push_back('\\');
				return escaped;
			}
		}

		struct Lexer {
			const std::string& source;
			std::size_t pos = 0;
			int line = 1;
			std::vector<Token> pushback;
			// A '/' after a value is division; anywhere else it opens a regex.
			bool prev_was_value = false;

			explicit Lexer(const std::string& text) : source(text) {}

			void unread(Token token) {
				pushback.push_back(std::move(token));
			}

			bool atEnd() const {
				return pos >= source.size();
			}

			char current() const {
				return pos < source.size() ? source[pos] : '\0';
			}

			char lookahead() const {
				return pos + 1 < source.size() ? source[pos + 1] : '\0';
			}

			void advance() {
				if (pos >= source.size()) return;

				if (source[pos] == '\n') ++line;
				++pos;
			}

			bool consumeIf(char c) {
				if (pos >= source.size() || source[pos] != c) return false;

				advance();
				return true;
			}

			Token startToken() const {
				Token token;
				token.line = line;
				return token;
			}

			Token finish(Token token, TokenKind kind, bool value_ends) {
				token.kind = kind;
				prev_was_value = value_ends;
				return token;
			}

			void skipBlanks() {
				while (!atEnd()) {
					const char c = current();
					if (isBlank(c)) {
						advance();
						continue;
					}

					if (c == '\\' && lookahead() == '\n') {
						advance();
						advance();
						continue;
					}

					if (c != '#') break;
					while (!atEnd() && current() != '\n') advance();
				}
			}

			Token readIdentifier() {
				Token token = startToken();

				const std::size_t start = pos;
				while (!atEnd() && isIdentifierChar(current())) advance();

				token.text = source.substr(start, pos - start);
				token.kind = identifierKind(token.text);
				return token;
			}

			void skipDigits() {
				while (!atEnd() && std::isdigit(static_cast<unsigned char>(current()))) advance();
			}

			Token readNumber() {
				Token token = startToken();

				const std::size_t start = pos;
				skipDigits();
				if (consumeIf('.')) skipDigits();
				if (current() == 'e' || current() == 'E') {
					advance();
					if (current() == '+' || current() == '-') advance();
					skipDigits();
				}

				token.text = source.substr(start, pos - start);
				parseDouble(token.text, token.number);
				token.kind = TokenKind::Number;
				return token;
			}

			Token readString() {
				Token token = startToken();
				advance();

				std::string text;
				while (!atEnd() && current() != '"') {
					if (current() == '\\' && pos + 1 < source.size()) {
						text.push_back(unescapeStringChar(lookahead(), text));
						advance();
						advance();
						continue;
					}

					text.push_back(current());
					advance();
				}

				if (!atEnd()) advance();
				token.kind = TokenKind::String;
				token.text = std::move(text);
				return token;
			}

			Token readRegex() {
				Token token = startToken();
				advance();

				std::string pattern;
				while (!atEnd() && current() != '/') {
					if (current() == '\\' && pos + 1 < source.size()) {
						pattern.push_back(current());
						pattern.push_back(lookahead());
						advance();
						advance();
						continue;
					}

					pattern.push_back(current());
					advance();
				}

				if (!atEnd()) advance();
				token.kind = TokenKind::Regex;
				token.text = std::move(pattern);
				return token;
			}

			Token readSimplePunct(char c) {
				Token token = startToken();
				advance();
				switch (c) {
				case '\n': return finish(token, TokenKind::Newline,  false);
				case ';':  return finish(token, TokenKind::Semi,     false);
				case ',':  return finish(token, TokenKind::Comma,    false);
				case '(':  return finish(token, TokenKind::LParen,   false);
				case ')':  return finish(token, TokenKind::RParen,   true);
				case '{':  return finish(token, TokenKind::LBrace,   false);
				case '}':  return finish(token, TokenKind::RBrace,   false);
				case '[':  return finish(token, TokenKind::LBracket, false);
				case ']':  return finish(token, TokenKind::RBracket, true);
				case '?':  return finish(token, TokenKind::Question, false);
				case ':':  return finish(token, TokenKind::Colon,    false);
				case '$':  return finish(token, TokenKind::Dollar,   false);
				case '~':  return finish(token, TokenKind::Match,    false);
				default:   token.kind = TokenKind::End; return token;
				}
			}

			Token readSlashOp() {
				if (!prev_was_value) {
					Token token = readRegex();
					prev_was_value = false;
					return token;
				}

				Token token = startToken();
				advance();
				if (consumeIf('=')) return finish(token, TokenKind::SlashAssign, false);
				return finish(token, TokenKind::Slash, false);
			}

			Token opOrAssign(Token token, TokenKind simple, TokenKind compound) {
				if (consumeIf('=')) return finish(token, compound, false);
				return finish(token, simple, false);
			}

			Token readArithOp(char c) {
				Token token = startToken();
				advance();
				switch (c) {
				case '+':
					if (consumeIf('+')) return finish(token, TokenKind::Inc, true);
					return opOrAssign(token, TokenKind::Plus, TokenKind::PlusAssign);
				case '-':
					if (consumeIf('-')) return finish(token, TokenKind::Dec, true);
					return opOrAssign(token, TokenKind::Minus, TokenKind::MinusAssign);
				case '*': return opOrAssign(token, TokenKind::Star,    TokenKind::StarAssign);
				case '%': return opOrAssign(token, TokenKind::Percent, TokenKind::PercentAssign);
				case '^': return opOrAssign(token, TokenKind::Caret,   TokenKind::CaretAssign);
				default:  token.kind = TokenKind::End; return token;
				}
			}

			Token readCompareOp(char c) {
				Token token = startToken();
				advance();
				switch (c) {
				case '=':
					if (consumeIf('=')) return finish(token, TokenKind::Eq, false);
					return finish(token, TokenKind::Assign, false);
				case '!':
					if (consumeIf('=')) return finish(token, TokenKind::Ne, false);
					if (consumeIf('~')) return finish(token, TokenKind::NoMatch, false);
					return finish(token, TokenKind::Not, false);
				case '<':
					if (consumeIf('=')) return finish(token, TokenKind::Le, false);
					return finish(token, TokenKind::Lt, false);
				case '>':
					if (consumeIf('>')) return finish(token, TokenKind::Append, false);
					if (consumeIf('=')) return finish(token, TokenKind::Ge, false);
					return finish(token, TokenKind::Gt, false);
				default:
					token.kind = TokenKind::End;
					return token;
				}
			}

			Token readLogicalOp(char c) {
				Token token = startToken();
				advance();
				if (c == '&') {
					if (consumeIf('&')) return finish(token, TokenKind::And, false);

					token.kind = TokenKind::End;
					return token;
				}

				if (consumeIf('|')) return finish(token, TokenKind::Or, false);
				return finish(token, TokenKind::Pipe, false);
			}

			bool startsNumber() const {
				const char c = current();
				if (std::isdigit(static_cast<unsigned char>(c))) return true;
				return c == '.' && std::isdigit(static_cast<unsigned char>(lookahead()));
			}

			bool startsIdentifier() const {
				const char c = current();
				return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
			}

			static bool isIdentifierChar(char c) {
				return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
			}

			Token next() {
				if (!pushback.empty()) {
					Token token = std::move(pushback.back());
					pushback.pop_back();
					return token;
				}

				skipBlanks();
				if (atEnd()) return finishEnd();
				if (startsNumber()) return finish(readNumber(), TokenKind::Number, true);
				if (startsIdentifier()) {
					Token token = readIdentifier();
					prev_was_value = token.kind == TokenKind::Identifier;
					return token;
				}

				const char c = current();
				switch (c) {
				case '\n': case ';': case ',': case '(': case ')':
				case '{':  case '}': case '[': case ']':
				case '?':  case ':': case '$': case '~':
					return readSimplePunct(c);
				case '"':
					return finish(readString(), TokenKind::String, true);
				case '/':
					return readSlashOp();
				case '+': case '-': case '*': case '%': case '^':
					return readArithOp(c);
				case '=': case '!': case '<': case '>':
					return readCompareOp(c);
				case '&': case '|':
					return readLogicalOp(c);
				default:
					advance();
					return next();
				}
			}

			Token finishEnd() {
				Token token = startToken();
				token.kind = TokenKind::End;
				return token;
			}

			Token& peek() {
				if (pushback.empty()) pushback.push_back(next());
				return pushback.back();
			}

			void abortToEof() {
				pushback.clear();
				pos = source.size();
			}
		};

		static const char* assignOpText(TokenKind kind) {
			switch (kind) {
			case TokenKind::Assign:        return "=";
			case TokenKind::PlusAssign:    return "+=";
			case TokenKind::MinusAssign:   return "-=";
			case TokenKind::StarAssign:    return "*=";
			case TokenKind::SlashAssign:   return "/=";
			case TokenKind::PercentAssign: return "%=";
			case TokenKind::CaretAssign:   return "^=";
			default:                       return nullptr;
			}
		}

		static const char* relationalOpText(TokenKind kind) {
			switch (kind) {
			case TokenKind::Lt: return "<";
			case TokenKind::Le: return "<=";
			case TokenKind::Gt: return ">";
			case TokenKind::Ge: return ">=";
			case TokenKind::Eq: return "==";
			case TokenKind::Ne: return "!=";
			default:            return nullptr;
			}
		}

		static const char* multiplicativeOpText(TokenKind kind) {
			switch (kind) {
			case TokenKind::Star:    return "*";
			case TokenKind::Slash:   return "/";
			case TokenKind::Percent: return "%";
			default:                 return nullptr;
			}
		}

		static const char* unaryOpText(TokenKind kind) {
			switch (kind) {
			case TokenKind::Not:   return "!";
			case TokenKind::Minus: return "-";
			case TokenKind::Plus:  return "+";
			case TokenKind::Inc:   return "++";
			case TokenKind::Dec:   return "--";
			default:               return nullptr;
			}
		}

		static bool startsConcatOperand(TokenKind kind) {
			switch (kind) {
			case TokenKind::Number: case TokenKind::String: case TokenKind::Identifier:
			case TokenKind::Dollar: case TokenKind::LParen: case TokenKind::Not:
			case TokenKind::Minus:  case TokenKind::Plus:   case TokenKind::Inc:
			case TokenKind::Dec:    case TokenKind::Regex:
				return true;
			default:
				return false;
			}
		}

		struct Parser {
			Lexer lex;
			std::string error_msg;
			Arena* arena = nullptr;

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

			Expr* newBinary(const char* op, Expr* left, Expr* right) {
				Expr* expr = newExpr(ExprKind::Binary);
				expr->op = op;
				expr->first = left;
				expr->second = right;
				return expr;
			}

			Stmt* implicitPrintAction() {
				Stmt* block = newStmt(StmtKind::Block);
				block->children.push_back(newStmt(StmtKind::Print));
				return block;
			}

			AwkProgram parseProgram() {
				AwkProgram program;
				arena = &program.arena;

				skipTerminators();
				while (!peekIs(TokenKind::End)) {
					AwkRule rule;
					parsePattern(rule.pattern);
					if (failed()) break;

					if (peekIs(TokenKind::LBrace)) {
						rule.action = parseBlock();
					} else {
						rule.action = implicitPrintAction();
					}

					program.rules.push_back(std::move(rule));
					skipTerminators();
				}

				return program;
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

				acceptIf(TokenKind::RBrace);
				return block;
			}

			void parseExprList(std::vector<Expr*>& out) {
				out.push_back(parseExpr());
				while (acceptIf(TokenKind::Comma)) out.push_back(parseExpr());
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

				stmt.redirect_target = parseExpr();
			}

			Stmt* parsePrintStmt(TokenKind keyword) {
				lex.next();
				const bool formatted = keyword == TokenKind::KwPrintf;
				Stmt* stmt = newStmt(formatted ? StmtKind::Printf : StmtKind::Print);
				if (!atPrintEnd()) parseExprList(stmt->exprs);
				parsePrintRedirect(*stmt);
				return stmt;
			}

			Stmt* parseKeywordStmt(StmtKind kind) {
				lex.next();
				return newStmt(kind);
			}

			Stmt* parseExitStmt() {
				lex.next();
				Stmt* stmt = newStmt(StmtKind::Exit);
				if (!atStatementEnd()) stmt->exprs.push_back(parseExpr());
				return stmt;
			}

			Stmt* parseDeleteStmt() {
				lex.next();
				Stmt* stmt = newStmt(StmtKind::Delete);
				stmt->exprs.push_back(parseUnary());
				return stmt;
			}

			Stmt* parseExprStmt() {
				Stmt* stmt = newStmt(StmtKind::ExprStmt);
				stmt->exprs.push_back(parseExpr());
				return stmt;
			}

			Stmt* parseStmt() {
				switch (lex.peek().kind) {
				case TokenKind::LBrace:     return parseBlock();
				case TokenKind::KwIf:       return parseIf();
				case TokenKind::KwWhile:    return parseWhile();
				case TokenKind::KwDo:       return parseDoWhile();
				case TokenKind::KwFor:      return parseFor();
				case TokenKind::KwBreak:    return parseKeywordStmt(StmtKind::Break);
				case TokenKind::KwContinue: return parseKeywordStmt(StmtKind::Continue);
				case TokenKind::KwNext:     return parseKeywordStmt(StmtKind::Next);
				case TokenKind::KwExit:     return parseExitStmt();
				case TokenKind::KwDelete:   return parseDeleteStmt();
				case TokenKind::KwPrint:    return parsePrintStmt(TokenKind::KwPrint);
				case TokenKind::KwPrintf:   return parsePrintStmt(TokenKind::KwPrintf);
				default:                    return parseExprStmt();
				}
			}

			Expr* parseParenCondition(const char* open_message) {
				expect(TokenKind::LParen, open_message);
				Expr* cond = parseExpr();
				expect(TokenKind::RParen, "expected ')'");
				return cond;
			}

			Stmt* parseIf() {
				lex.next();
				Expr* cond = parseParenCondition("expected '(' after if");

				skipTerminators();
				Stmt* then_branch = parseStmt();

				skipTerminators();
				Stmt* else_branch = nullptr;
				if (acceptIf(TokenKind::KwElse)) {
					skipTerminators();
					else_branch = parseStmt();
				}

				Stmt* stmt = newStmt(StmtKind::If);
				stmt->exprs.push_back(cond);
				stmt->children.push_back(then_branch);
				if (else_branch != nullptr) stmt->children.push_back(else_branch);
				return stmt;
			}

			Stmt* parseWhile() {
				lex.next();
				Expr* cond = parseParenCondition("expected '('");

				skipTerminators();
				Stmt* body = parseStmt();

				Stmt* stmt = newStmt(StmtKind::While);
				stmt->exprs.push_back(cond);
				stmt->children.push_back(body);
				return stmt;
			}

			Stmt* parseDoWhile() {
				lex.next();
				skipTerminators();
				Stmt* body = parseStmt();

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

				skipTerminators();
				Stmt* body = parseStmt();

				Stmt* stmt = newStmt(StmtKind::ForIn);
				stmt->loop_var = loop_var.text;
				stmt->array_name = array_name.text;
				stmt->children.push_back(body);
				return stmt;
			}

			Stmt* parseFor() {
				lex.next();
				expect(TokenKind::LParen, "expected '('");
				if (peekIs(TokenKind::Identifier)) {
					Token loop_var = lex.next();
					if (acceptIf(TokenKind::KwIn)) return parseForIn(loop_var);
					lex.unread(std::move(loop_var));
				}

				Expr* init = nullptr;
				Expr* cond = nullptr;
				Expr* step = nullptr;
				if (!peekIs(TokenKind::Semi)) init = parseExpr();
				expect(TokenKind::Semi, "expected ';'");
				if (!peekIs(TokenKind::Semi)) cond = parseExpr();
				expect(TokenKind::Semi, "expected ';'");
				if (!peekIs(TokenKind::RParen)) step = parseExpr();
				expect(TokenKind::RParen, "expected ')'");

				skipTerminators();
				Stmt* body = parseStmt();

				Stmt* stmt = newStmt(StmtKind::For);
				stmt->init = init;
				stmt->cond = cond;
				stmt->step = step;
				stmt->children.push_back(body);
				return stmt;
			}

			Expr* parseExpr() {
				return parseTernary();
			}

			Expr* parseTernaryTail(Expr* cond) {
				Expr* then_value = parseTernary();
				expect(TokenKind::Colon, "expected ':'");
				Expr* else_value = parseTernary();

				Expr* expr = newExpr(ExprKind::Ternary);
				expr->first = cond;
				expr->second = then_value;
				expr->third = else_value;
				return expr;
			}

			Expr* parseAssignmentTail(Expr* target, const char* op) {
				lex.next();
				Expr* rhs = parseTernary();

				if (target->kind == ExprKind::Field) {
					Expr* expr = newExpr(ExprKind::FieldAssign);
					expr->op = op;
					expr->first = target->first;
					expr->second = rhs;
					return expr;
				}

				Expr* expr = newExpr(ExprKind::Assign);
				expr->op = op;
				expr->first = target;
				expr->second = rhs;
				return expr;
			}

			Expr* parseTernary() {
				Expr* left = parseLogicOr();
				if (acceptIf(TokenKind::Question)) return parseTernaryTail(left);

				const char* assign_op = assignOpText(lex.peek().kind);
				if (assign_op != nullptr) return parseAssignmentTail(left, assign_op);
				return left;
			}

			Expr* parseLogicOr() {
				Expr* left = parseLogicAnd();
				while (acceptIf(TokenKind::Or)) left = newBinary("||", left, parseLogicAnd());
				return left;
			}

			Expr* parseLogicAnd() {
				Expr* left = parseInTest();
				while (acceptIf(TokenKind::And)) left = newBinary("&&", left, parseInTest());
				return left;
			}

			Expr* parseInTest() {
				Expr* subscript = parseMatch();
				if (!acceptIf(TokenKind::KwIn)) return subscript;

				if (!peekIs(TokenKind::Identifier)) err("expected array name after 'in'");
				const Token array_name = lex.next();

				Expr* expr = newExpr(ExprKind::ArrayInTest);
				expr->first = subscript;
				expr->name = array_name.text;
				return expr;
			}

			Expr* parseMatch() {
				Expr* left = parseRel();
				while (peekIs(TokenKind::Match) || peekIs(TokenKind::NoMatch)) {
					const char* op = peekIs(TokenKind::Match) ? "~" : "!~";
					lex.next();
					left = newBinary(op, left, parseRel());
				}

				return left;
			}

			Expr* parseRel() {
				Expr* left = parseConcat();
				for (;;) {
					const char* op = relationalOpText(lex.peek().kind);
					if (op == nullptr) return left;

					lex.next();
					left = newBinary(op, left, parseConcat());
				}
			}

			// Juxtaposition is string concatenation: `a b` means a "" b.
			Expr* parseConcat() {
				Expr* left = parseAdd();
				while (startsConcatOperand(lex.peek().kind)) {
					left = newBinary(" ", left, parseAdd());
				}
				return left;
			}

			Expr* parseAdd() {
				Expr* left = parseMul();
				while (peekIs(TokenKind::Plus) || peekIs(TokenKind::Minus)) {
					const char* op = peekIs(TokenKind::Plus) ? "+" : "-";
					lex.next();
					left = newBinary(op, left, parseMul());
				}

				return left;
			}

			Expr* parseMul() {
				Expr* left = parseExp();
				for (;;) {
					const char* op = multiplicativeOpText(lex.peek().kind);
					if (op == nullptr) return left;

					lex.next();
					left = newBinary(op, left, parseExp());
				}
			}

			Expr* parseExp() {
				Expr* base = parseUnary();
				if (!acceptIf(TokenKind::Caret)) return base;
				return newBinary("^", base, parseExp());
			}

			Expr* parseUnary() {
				const char* op = unaryOpText(lex.peek().kind);
				if (op == nullptr) return parsePostfix();

				lex.next();
				Expr* operand = parseUnary();

				Expr* expr = newExpr(ExprKind::Unary);
				expr->op = op;
				expr->first = operand;
				return expr;
			}

			Expr* parsePostfix() {
				Expr* operand = parsePrimary();
				while (peekIs(TokenKind::Inc) || peekIs(TokenKind::Dec)) {
					const char* op = peekIs(TokenKind::Inc) ? "++" : "--";
					lex.next();

					Expr* expr = newExpr(ExprKind::PostIncDec);
					expr->op = op;
					expr->first = operand;
					operand = expr;
				}

				return operand;
			}

			Expr* parseGetline() {
				Expr* expr = newExpr(ExprKind::Getline);
				if (peekIs(TokenKind::Identifier)) expr->name = lex.next().text;
				if (acceptIf(TokenKind::Lt)) expr->second = parseUnary();
				return expr;
			}

			Expr* parseCall(const std::string& name) {
				Expr* expr = newExpr(ExprKind::Call);
				expr->name = name;
				if (!peekIs(TokenKind::RParen)) parseExprList(expr->args);
				expect(TokenKind::RParen, "expected ')'");
				return expr;
			}

			Expr* parseArrayRef(const std::string& name) {
				std::vector<Expr*> subscripts;
				parseExprList(subscripts);
				expect(TokenKind::RBracket, "expected ']'");

				Expr* expr = newExpr(ExprKind::ArrayRef);
				expr->name = name;
				expr->args = std::move(subscripts);
				return expr;
			}

			Expr* parseIdentifierSuffix(const std::string& name) {
				if (acceptIf(TokenKind::LParen)) return parseCall(name);
				if (acceptIf(TokenKind::LBracket)) return parseArrayRef(name);

				Expr* expr = newExpr(ExprKind::Var);
				expr->name = name;
				return expr;
			}

			Expr* parseLiteral(ExprKind kind, const Token& token) {
				Expr* expr = newExpr(kind);
				if (kind == ExprKind::Number) expr->number = token.number;
				else expr->text = token.text;
				return expr;
			}

			Expr* parseFieldRef() {
				Expr* index = parseUnary();
				Expr* expr = newExpr(ExprKind::Field);
				expr->first = index;
				return expr;
			}

			Expr* parseGroup() {
				Expr* inner = parseExpr();
				expect(TokenKind::RParen, "expected ')'");

				Expr* expr = newExpr(ExprKind::Group);
				expr->first = inner;
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
					return parseGetline();
				case TokenKind::Identifier:
					lex.next();
					return parseIdentifierSuffix(token.text);
				default:
					err("unexpected token");
					return nullptr;
				}
			}
		};

		static std::vector<std::string> splitByRegex(const std::string& text,
				const std::regex& re) {
			std::vector<std::string> parts;
			std::size_t pos = 0;
			for (;;) {
				std::smatch match;
				if (!searchRegex(text.cbegin() + pos, text.cend(), re, &match)) break;

				const auto offset = static_cast<std::size_t>(match.position(0));
				const auto length = static_cast<std::size_t>(match.length(0));
				parts.push_back(text.substr(pos, offset));
				pos += offset + length;
				// A zero-width separator would re-match in place forever; stop and
				// let the tail below become the final field.
				if (length == 0) break;
			}

			if (pos < text.size() || parts.empty()) parts.push_back(text.substr(pos));
			return parts;
		}

		static std::vector<std::string> splitOnBlanks(const std::string& text) {
			std::vector<std::string> parts;
			const std::size_t size = text.size();
			std::size_t i = 0;
			while (i < size) {
				while (i < size && isBlank(text[i])) ++i;
				if (i >= size) break;

				const std::size_t start = i;
				while (i < size && !isBlank(text[i])) ++i;
				parts.push_back(text.substr(start, i - start));
			}

			return parts;
		}

		static std::vector<std::string> splitOnChar(const std::string& text, char separator) {
			std::vector<std::string> parts;
			std::string current;
			for (char c : text) {
				if (c != separator) {
					current.push_back(c);
					continue;
				}

				parts.push_back(std::move(current));
				current.clear();
			}

			parts.push_back(std::move(current));
			return parts;
		}

		static std::vector<std::string> splitOnSeparator(const std::string& text,
				const std::string& separator) {
			if (isWhitespaceSeparator(separator)) return splitOnBlanks(text);
			if (separator.size() == 1) return splitOnChar(text, separator[0]);

			std::regex re;
			if (compileRegex(re, separator)) return splitByRegex(text, re);
			return { text };
		}

		static bool readLine(FILE* file, std::string& line) {
			line.clear();

			bool any = false;
			int c = 0;
			while ((c = std::fgetc(file)) != EOF) {
				any = true;
				if (c == '\n') break;
				line.push_back(static_cast<char>(c));
			}

			return any;
		}

		template <typename T>
		static bool compareWith(const T& left, const T& right, const std::string& op) {
			if (op == "==") return left == right;
			if (op == "!=") return left != right;
			if (op == "<")  return left < right;
			if (op == "<=") return left <= right;
			if (op == ">")  return left > right;
			return left >= right;
		}

		static double randomFraction() {
			return static_cast<double>(std::rand()) / RAND_MAX;
		}

		static bool namesArray(const Expr& expr) {
			return expr.kind == ExprKind::Var || expr.kind == ExprKind::ArrayRef;
		}

		static bool isSpecialBlock(const AwkPattern& pattern) {
			return pattern.kind == AwkPattern::Begin || pattern.kind == AwkPattern::End;
		}

		static bool isRelationalOp(const std::string& op) {
			return op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=";
		}

		static AwkValue compareValues(const AwkValue& left, const AwkValue& right,
				const std::string& op) {
			const bool numeric = left.is_number || right.is_number;
			if (numeric) return boolValue(compareWith(left.asNumber(), right.asNumber(), op));
			return boolValue(compareWith(left.asString(), right.asString(), op));
		}

		static const std::regex* literalRegex(const Expr& expr) {
			if (expr.compiled) return expr.compiled.get();
			if (expr.compile_failed) return nullptr;

			auto re = std::make_shared<std::regex>();
			if (!compileRegex(*re, expr.text)) {
				expr.compile_failed = true;
				return nullptr;
			}

			expr.compiled = std::move(re);
			return expr.compiled.get();
		}

		static int gsubAll(std::string& subject, const std::regex& re,
				const std::string& replacement) {
			std::string out;
			std::size_t pos = 0;
			int count = 0;
			while (pos <= subject.size()) {
				std::smatch match;
				if (!searchRegex(subject.cbegin() + pos, subject.cend(), re, &match)) break;

				const auto offset = static_cast<std::size_t>(match.position(0));
				const auto length = static_cast<std::size_t>(match.length(0));
				out.append(subject, pos, offset);
				out.append(match.format(replacement));
				++count;
				pos += offset + length;
				if (length != 0) continue;

				// Zero-width match: copy one char through so the scan advances.
				if (pos >= subject.size()) break;
				out.push_back(subject[pos]);
				++pos;
			}

			out.append(subject, pos, std::string::npos);
			subject = std::move(out);
			return count;
		}

		static int subFirst(std::string& subject, const std::regex& re,
				const std::string& replacement) {
			std::smatch match;
			if (!searchRegex(subject, re, &match)) return 0;

			subject = match.prefix().str() + match.format(replacement) + match.suffix().str();
			return 1;
		}

		static void appendPrintfEscape(std::string& out, char escaped) {
			switch (escaped) {
			case 'n':  out.push_back('\n'); return;
			case 't':  out.push_back('\t'); return;
			case 'r':  out.push_back('\r'); return;
			case '\\': out.push_back('\\'); return;
			case '"':  out.push_back('"');  return;
			default:
				out.push_back('\\');
				out.push_back(escaped);
				return;
			}
		}

		static std::string withLongLongModifier(const std::string& spec, char conversion) {
			return spec.substr(0, spec.size() - 1) + "ll" + conversion;
		}

		static std::string formatPrintfDirective(const std::string& spec, char conversion,
				const AwkValue& value) {
			char buffer[256];
			if (conversion == 's') {
				std::snprintf(buffer, sizeof(buffer), spec.c_str(), value.asString().c_str());
				return buffer;
			}

			if (conversion == 'c') {
				if (value.is_string && !value.asString().empty()) {
					std::snprintf(buffer, sizeof(buffer), spec.c_str(), value.asString()[0]);
				} else {
					std::snprintf(buffer, sizeof(buffer), spec.c_str(),
						static_cast<int>(value.asNumber()));
				}

				return buffer;
			}

			if (conversion == 'd' || conversion == 'i') {
				const std::string modified = withLongLongModifier(spec, conversion);
				std::snprintf(buffer, sizeof(buffer), modified.c_str(),
					static_cast<long long>(value.asNumber()));
				return buffer;
			}

			if (conversion == 'o' || conversion == 'x' || conversion == 'X' || conversion == 'u') {
				const std::string modified = withLongLongModifier(spec, conversion);
				std::snprintf(buffer, sizeof(buffer), modified.c_str(),
					static_cast<unsigned long long>(value.asNumber()));
				return buffer;
			}

			if (conversion == 'f' || conversion == 'e' || conversion == 'E'
					|| conversion == 'g' || conversion == 'G') {
				std::snprintf(buffer, sizeof(buffer), spec.c_str(), value.asNumber());
				return buffer;
			}

			if (conversion == '%') return "%";
			return spec;
		}

		// Advances past the flags, width and precision of a directive starting
		// at fmt[i] == '%' and leaves i on the conversion character.
		static void skipPrintfModifiers(const std::string& fmt, std::size_t& i) {
			++i;
			while (i < fmt.size() && std::strchr("-+0 #", fmt[i]) != nullptr) ++i;
			while (i < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i]))) ++i;
			if (i < fmt.size() && fmt[i] == '.') {
				++i;
				while (i < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i]))) ++i;
			}
		}

		static std::string formatPrintf(const std::vector<AwkValue>& args) {
			if (args.empty()) return std::string();

			const std::string& fmt = args[0].asString();
			std::string out;
			std::size_t next_arg = 1;
			std::size_t i = 0;
			while (i < fmt.size()) {
				const char c = fmt[i];
				if (c == '\\' && i + 1 < fmt.size()) {
					appendPrintfEscape(out, fmt[i + 1]);
					i += 2;
					continue;
				}

				if (c != '%') {
					out.push_back(c);
					++i;
					continue;
				}

				const std::size_t start = i;
				skipPrintfModifiers(fmt, i);
				if (i >= fmt.size()) {
					out.append(fmt, start, std::string::npos);
					break;
				}

				const char conversion = fmt[i++];
				const std::string spec = fmt.substr(start, i - start);
				AwkValue value = AwkValue::str("");
				if (next_arg < args.size()) value = args[next_arg++];
				out.append(formatPrintfDirective(spec, conversion, value));
			}

			return out;
		}

		static FILE* openPipeForWriting(const std::string& command) {
#ifdef _WIN32
			return _popen(command.c_str(), "w");
#else
			return popen(command.c_str(), "w");
#endif
		}

		static void closePipe(FILE* pipe) {
#ifdef _WIN32
			_pclose(pipe);
#else
			pclose(pipe);
#endif
		}

		struct Interpreter {
			enum class Flow { None, Break, Continue, Next };

			AwkProgram& program;
			std::unordered_map<std::string, AwkValue> variables;
			std::unordered_map<std::string, std::map<std::string, AwkValue>> arrays;
			std::vector<std::string> fields;
			std::string record;
			long long NR = 0;
			long long FNR = 0;
			std::string FS = kWhitespaceSeparator;
			std::string OFS = " ";
			std::string ORS = "\n";
			std::string FILENAME;
			std::string SUBSEP = std::string(1, kDefaultSubsep);
			bool exiting = false;
			int exit_status = 0;
			std::map<std::string, FILE*> output_files;
			std::map<std::string, FILE*> input_files;
			Flow flow = Flow::None;
			std::string fs_regex_pattern;
			std::regex fs_regex;
			bool fs_regex_ok = false;
			bool fs_regex_cached = false;

			explicit Interpreter(AwkProgram& parsed) : program(parsed) {}

			~Interpreter() {
				for (auto& entry : output_files) {
					if (entry.second != nullptr) std::fclose(entry.second);
				}

				for (auto& entry : input_files) {
					if (entry.second != nullptr && entry.second != stdin) std::fclose(entry.second);
				}
			}

			bool fieldRegexReady() {
				if (!fs_regex_cached || FS != fs_regex_pattern) {
					fs_regex_pattern = FS;
					fs_regex_ok = compileRegex(fs_regex, FS);
					fs_regex_cached = true;
				}

				return fs_regex_ok;
			}

			void splitRecord() {
				fields.clear();
				if (isWhitespaceSeparator(FS)) {
					fields = splitOnBlanks(record);
					return;
				}

				if (FS.size() == 1) {
					fields = splitOnChar(record, FS[0]);
					return;
				}

				if (!fieldRegexReady()) {
					fields.push_back(record);
					return;
				}

				fields = splitByRegex(record, fs_regex);
			}

			void storeFieldCount() {
				variables["NF"] = AwkValue::num(static_cast<double>(fields.size()));
			}

			void rebuildRecord() {
				record.clear();
				for (std::size_t i = 0; i < fields.size(); ++i) {
					if (i != 0) record += OFS;
					record += fields[i];
				}

				storeFieldCount();
			}

			AwkValue getField(int index) {
				if (index == 0) return AwkValue::str(record);
				if (index < 0) return AwkValue::str("");
				if (static_cast<std::size_t>(index) > fields.size()) return AwkValue::str("");
				return AwkValue::str(fields[static_cast<std::size_t>(index) - 1]);
			}

			void setField(int index, std::string value) {
				if (index == 0) {
					record = value;
					splitRecord();
					storeFieldCount();
					return;
				}

				if (index < 0) return;

				const auto slot = static_cast<std::size_t>(index);
				if (slot > fields.size()) fields.resize(slot);
				fields[slot - 1] = std::move(value);
				rebuildRecord();
			}

			std::string* stringSpecialVar(const std::string& name) {
				if (name == "FS")       return &FS;
				if (name == "OFS")      return &OFS;
				if (name == "ORS")      return &ORS;
				if (name == "FILENAME") return &FILENAME;
				if (name == "SUBSEP")   return &SUBSEP;
				return nullptr;
			}

			AwkValue getVar(const std::string& name) {
				if (name == "NF")  return AwkValue::num(static_cast<double>(fields.size()));
				if (name == "NR")  return AwkValue::num(static_cast<double>(NR));
				if (name == "FNR") return AwkValue::num(static_cast<double>(FNR));

				const std::string* special = stringSpecialVar(name);
				if (special != nullptr) return AwkValue::str(*special);

				const auto it = variables.find(name);
				if (it == variables.end()) return AwkValue::str("");
				return it->second;
			}

			void setFieldCount(int count) {
				if (count < 0) count = 0;
				fields.resize(static_cast<std::size_t>(count));
				rebuildRecord();
			}

			void setVar(const std::string& name, AwkValue value) {
				std::string* special = stringSpecialVar(name);
				if (special != nullptr) {
					*special = value.asString();
					return;
				}

				if (name == "NF") {
					setFieldCount(static_cast<int>(value.asNumber()));
					return;
				}

				if (name == "NR") {
					NR = static_cast<long long>(value.asNumber());
					return;
				}

				if (name == "FNR") {
					FNR = static_cast<long long>(value.asNumber());
					return;
				}

				variables[name] = std::move(value);
			}

			std::string buildSubscript(const std::vector<Expr*>& subscripts) {
				std::string key;
				for (std::size_t i = 0; i < subscripts.size(); ++i) {
					if (i != 0) key += SUBSEP;
					key += eval(*subscripts[i]).asString();
				}

				return key;
			}

			AwkValue evalUnary(Expr& expr) {
				const AwkValue operand = eval(*expr.first);
				if (expr.op == "!") return boolValue(!operand.truthy());
				if (expr.op == "-") return AwkValue::num(-operand.asNumber());
				if (expr.op == "+") return AwkValue::num(operand.asNumber());
				if (expr.op == "++" || expr.op == "--") {
					const double current = operand.asNumber();
					const double updated = (expr.op == "++") ? current + 1 : current - 1;
					assignLValue(*expr.first, AwkValue::num(updated));
					return AwkValue::num(updated);
				}

				return AwkValue::str("");
			}

			AwkValue evalPostIncDec(Expr& expr) {
				const double current = eval(*expr.first).asNumber();
				const double updated = (expr.op == "++") ? current + 1 : current - 1;
				assignLValue(*expr.first, AwkValue::num(updated));
				return AwkValue::num(current);
			}

			AwkValue evalRegexMatch(const std::string& subject, Expr& pattern, bool positive) {
				bool matched = false;
				if (pattern.kind == ExprKind::Regex) {
					const std::regex* re = literalRegex(pattern);
					matched = re != nullptr && searchRegex(subject, *re);
				} else {
					std::regex re;
					const bool compiled = compileRegex(re, eval(pattern).asString());
					matched = compiled && searchRegex(subject, re);
				}

				return boolValue(positive ? matched : !matched);
			}

			AwkValue evalArithmetic(const std::string& op, const AwkValue& left,
					const AwkValue& right) {
				if (op == "+") return AwkValue::num(left.asNumber() + right.asNumber());
				if (op == "-") return AwkValue::num(left.asNumber() - right.asNumber());
				if (op == "*") return AwkValue::num(left.asNumber() * right.asNumber());
				if (op == "/") {
					const double divisor = right.asNumber();
					return AwkValue::num(divisor == 0 ? 0.0 : left.asNumber() / divisor);
				}

				if (op == "%") {
					const double divisor = right.asNumber();
					return AwkValue::num(divisor == 0 ? 0.0 : std::fmod(left.asNumber(), divisor));
				}

				if (op == "^") return AwkValue::num(std::pow(left.asNumber(), right.asNumber()));
				if (isRelationalOp(op)) return compareValues(left, right, op);
				return AwkValue::str("");
			}

			AwkValue evalBinary(Expr& expr) {
				if (expr.op == "&&") {
					if (!eval(*expr.first).truthy()) return AwkValue::num(0.0);
					return boolValue(eval(*expr.second).truthy());
				}

				if (expr.op == "||") {
					if (eval(*expr.first).truthy()) return AwkValue::num(1.0);
					return boolValue(eval(*expr.second).truthy());
				}

				if (expr.op == "~" || expr.op == "!~") {
					const std::string subject = eval(*expr.first).asString();
					return evalRegexMatch(subject, *expr.second, expr.op == "~");
				}

				if (expr.op == " ") {
					return AwkValue::str(eval(*expr.first).asString()
						+ eval(*expr.second).asString());
				}

				const AwkValue left = eval(*expr.first);
				const AwkValue right = eval(*expr.second);
				return evalArithmetic(expr.op, left, right);
			}

			int fieldIndex(Expr& index_expr) {
				return static_cast<int>(eval(index_expr).asNumber());
			}

			AwkValue evalTernary(Expr& expr) {
				if (eval(*expr.first).truthy()) return eval(*expr.second);
				return eval(*expr.third);
			}

			AwkValue evalArrayRef(Expr& expr) {
				const std::string key = buildSubscript(expr.args);
				auto& array = arrays[expr.name];
				const auto it = array.find(key);
				return it == array.end() ? AwkValue::str("") : it->second;
			}

			AwkValue evalArrayInTest(Expr& expr) {
				const std::string key = eval(*expr.first).asString();
				auto& array = arrays[expr.name];
				return boolValue(array.count(key) != 0);
			}

			AwkValue evalAssign(Expr& expr) {
				return assignTo(*expr.first, expr.op, eval(*expr.second));
			}

			AwkValue evalFieldAssign(Expr& expr) {
				const int index = fieldIndex(*expr.first);
				const AwkValue current = getField(index);
				const AwkValue rhs = eval(*expr.second);
				const AwkValue result = applyOp(current, expr.op, rhs);
				setField(index, result.asString());
				return result;
			}

			AwkValue eval(Expr& expr) {
				switch (expr.kind) {
				case ExprKind::Number:      return AwkValue::num(expr.number);
				case ExprKind::String:      return AwkValue::str(expr.text);
				case ExprKind::Regex:       return AwkValue::str(expr.text);
				case ExprKind::Var:         return getVar(expr.name);
				case ExprKind::Field:       return getField(fieldIndex(*expr.first));
				case ExprKind::ArrayRef:    return evalArrayRef(expr);
				case ExprKind::ArrayInTest: return evalArrayInTest(expr);
				case ExprKind::Group:       return eval(*expr.first);
				case ExprKind::Unary:       return evalUnary(expr);
				case ExprKind::PostIncDec:  return evalPostIncDec(expr);
				case ExprKind::Binary:      return evalBinary(expr);
				case ExprKind::Ternary:     return evalTernary(expr);
				case ExprKind::Assign:      return evalAssign(expr);
				case ExprKind::FieldAssign: return evalFieldAssign(expr);
				case ExprKind::Call:        return callBuiltin(expr);
				case ExprKind::Getline:     return doGetline(expr);
				}

				return AwkValue::str("");
			}

			AwkValue applyOp(const AwkValue& current, const std::string& op, const AwkValue& rhs) {
				if (op == "=") return rhs;

				const double left = current.asNumber();
				const double right = rhs.asNumber();
				if (op == "+=") return AwkValue::num(left + right);
				if (op == "-=") return AwkValue::num(left - right);
				if (op == "*=") return AwkValue::num(left * right);
				if (op == "/=") return AwkValue::num(right == 0 ? 0 : left / right);
				if (op == "%=") return AwkValue::num(right == 0 ? 0 : std::fmod(left, right));
				if (op == "^=") return AwkValue::num(std::pow(left, right));
				return rhs;
			}

			AwkValue assignToArrayElement(Expr& lhs, const std::string& op, const AwkValue& rhs) {
				const std::string key = buildSubscript(lhs.args);
				auto& array = arrays[lhs.name];
				const AwkValue current = array.count(key) != 0 ? array[key] : AwkValue::str("");
				const AwkValue result = applyOp(current, op, rhs);
				array[key] = result;
				return result;
			}

			AwkValue assignToField(Expr& lhs, const std::string& op, const AwkValue& rhs) {
				const int index = fieldIndex(*lhs.first);
				const AwkValue current = getField(index);
				const AwkValue result = applyOp(current, op, rhs);
				setField(index, result.asString());
				return result;
			}

			AwkValue assignTo(Expr& lhs, const std::string& op, AwkValue rhs) {
				if (lhs.kind == ExprKind::Var) {
					const AwkValue current = getVar(lhs.name);
					const AwkValue result = applyOp(current, op, rhs);
					setVar(lhs.name, result);
					return result;
				}

				if (lhs.kind == ExprKind::ArrayRef) return assignToArrayElement(lhs, op, rhs);
				if (lhs.kind == ExprKind::Field) return assignToField(lhs, op, rhs);
				return rhs;
			}

			void assignLValue(Expr& lhs, AwkValue value) {
				assignTo(lhs, "=", std::move(value));
			}

			std::string argStr(Expr& call, std::size_t index) {
				if (index >= call.args.size()) return std::string();
				return eval(*call.args[index]).asString();
			}

			double argNum(Expr& call, std::size_t index) {
				if (index >= call.args.size()) return 0.0;
				return eval(*call.args[index]).asNumber();
			}

			AwkValue callLength(Expr& call) {
				if (call.args.empty()) return AwkValue::num(static_cast<double>(record.size()));
				return AwkValue::num(static_cast<double>(argStr(call, 0).size()));
			}

			AwkValue callSubstr(Expr& call) {
				const std::string text = argStr(call, 0);
				const auto text_size = static_cast<long long>(text.size());

				long long start = static_cast<long long>(argNum(call, 1));
				if (start < 1) start = 1;

				const long long length = (call.args.size() >= 3)
					? static_cast<long long>(argNum(call, 2))
					: text_size - start + 1;
				if (start > text_size || length <= 0) return AwkValue::str("");

				const long long available = text_size - start + 1;
				return AwkValue::str(text.substr(static_cast<std::size_t>(start - 1),
					static_cast<std::size_t>(std::min<long long>(length, available))));
			}

			AwkValue callIndex(Expr& call) {
				const std::string haystack = argStr(call, 0);
				const std::string needle = argStr(call, 1);
				if (needle.empty()) return AwkValue::num(0);

				const std::size_t found = haystack.find(needle);
				if (found == std::string::npos) return AwkValue::num(0);
				return AwkValue::num(static_cast<double>(found + 1));
			}

			AwkValue callCaseConvert(Expr& call) {
				const bool lower = call.name == "tolower";
				std::string text = argStr(call, 0);
				for (auto& c : text) {
					const auto raw = static_cast<unsigned char>(c);
					c = static_cast<char>(lower ? std::tolower(raw) : std::toupper(raw));
				}

				return AwkValue::str(std::move(text));
			}

			AwkValue callSplit(Expr& call) {
				const std::string text = argStr(call, 0);
				const std::string separator = (call.args.size() >= 3) ? argStr(call, 2) : FS;

				std::string array_name;
				if (call.args.size() >= 2 && namesArray(*call.args[1])) {
					array_name = call.args[1]->name;
				}

				arrays[array_name].clear();
				const auto parts = splitOnSeparator(text, separator);
				for (std::size_t i = 0; i < parts.size(); ++i) {
					arrays[array_name][std::to_string(i + 1)] = AwkValue::str(parts[i]);
				}

				return AwkValue::num(static_cast<double>(parts.size()));
			}

			AwkValue callSubOrGsub(Expr& call) {
				const bool global = call.name == "gsub";
				const std::string pattern = argStr(call, 0);
				const std::string replacement = argStr(call, 1);
				Expr* target = (call.args.size() >= 3) ? call.args[2] : nullptr;
				std::string subject = target != nullptr ? eval(*target).asString() : record;

				int count = 0;
				std::regex re;
				if (compileRegex(re, pattern)) {
					if (global) count = gsubAll(subject, re, replacement);
					else        count = subFirst(subject, re, replacement);
				}

				if (target != nullptr) {
					assignLValue(*target, AwkValue::str(subject));
				} else {
					record = std::move(subject);
					splitRecord();
				}

				return AwkValue::num(static_cast<double>(count));
			}

			AwkValue callMatch(Expr& call) {
				const std::string text = argStr(call, 0);
				const std::string pattern = argStr(call, 1);

				std::regex re;
				std::smatch match;
				if (compileRegex(re, pattern) && searchRegex(text, re, &match)) {
					const auto start = static_cast<double>(match.position(0) + 1);
					variables["RSTART"]  = AwkValue::num(start);
					variables["RLENGTH"] = AwkValue::num(static_cast<double>(match.length(0)));
					return AwkValue::num(start);
				}

				variables["RSTART"]  = AwkValue::num(0);
				variables["RLENGTH"] = AwkValue::num(-1);
				return AwkValue::num(0);
			}

			std::vector<AwkValue> evalAll(const std::vector<Expr*>& exprs) {
				std::vector<AwkValue> values;
				for (auto& expr : exprs) values.push_back(eval(*expr));
				return values;
			}

			AwkValue callPrintf(Expr& call) {
				std::string out = formatPrintf(evalAll(call.args));
				if (call.name == "sprintf") return AwkValue::str(std::move(out));

				std::fputs(out.c_str(), stdout);
				return AwkValue::str("");
			}

			AwkValue callSystem(Expr& call) {
				std::string command;
				if (!call.args.empty()) command = eval(*call.args[0]).asString();
				return AwkValue::num(static_cast<double>(std::system(command.c_str())));
			}

			AwkValue callMath(Expr& call) {
				const std::string& name = call.name;
				if (name == "int")   return AwkValue::num(std::trunc(argNum(call, 0)));
				if (name == "sqrt")  return AwkValue::num(std::sqrt(argNum(call, 0)));
				if (name == "exp")   return AwkValue::num(std::exp(argNum(call, 0)));
				if (name == "log")   return AwkValue::num(std::log(argNum(call, 0)));
				if (name == "sin")   return AwkValue::num(std::sin(argNum(call, 0)));
				if (name == "cos")   return AwkValue::num(std::cos(argNum(call, 0)));
				if (name == "rand")  return AwkValue::num(randomFraction());
				if (name == "atan2") {
					return AwkValue::num(std::atan2(argNum(call, 0), argNum(call, 1)));
				}
				if (name == "srand") {
					std::srand(static_cast<unsigned>(argNum(call, 0)));
					return AwkValue::num(0);
				}

				return AwkValue::str("");
			}

			AwkValue callBuiltin(Expr& call) {
				const std::string& name = call.name;
				if (name == "length")                        return callLength(call);
				if (name == "substr")                        return callSubstr(call);
				if (name == "index")                         return callIndex(call);
				if (name == "tolower" || name == "toupper")  return callCaseConvert(call);
				if (name == "split")                         return callSplit(call);
				if (name == "sub" || name == "gsub")         return callSubOrGsub(call);
				if (name == "match")                         return callMatch(call);
				if (name == "sprintf" || name == "printf")   return callPrintf(call);
				if (name == "system")                        return callSystem(call);
				return callMath(call);
			}

			bool loopBodyDone() {
				if (flow == Flow::Break) {
					flow = Flow::None;
					return true;
				}

				if (flow == Flow::Continue) flow = Flow::None;
				return flow == Flow::Next || exiting;
			}

			void runIf(Stmt& stmt) {
				if (eval(*stmt.exprs[0]).truthy()) {
					if (!stmt.children.empty()) run(*stmt.children[0]);
					return;
				}

				if (stmt.children.size() >= 2) run(*stmt.children[1]);
			}

			void runWhile(Stmt& stmt) {
				while (eval(*stmt.exprs[0]).truthy()) {
					run(*stmt.children[0]);
					if (loopBodyDone()) return;
				}
			}

			void runDoWhile(Stmt& stmt) {
				do {
					run(*stmt.children[0]);
					if (loopBodyDone()) return;
				} while (eval(*stmt.exprs[0]).truthy());
			}

			void runFor(Stmt& stmt) {
				if (stmt.init != nullptr) eval(*stmt.init);
				while (stmt.cond == nullptr || eval(*stmt.cond).truthy()) {
					run(*stmt.children[0]);
					if (loopBodyDone()) return;
					if (stmt.step != nullptr) eval(*stmt.step);
				}
			}

			// The body may add or delete elements, so the keys are snapshotted first.
			void runForIn(Stmt& stmt) {
				std::vector<std::string> keys;
				for (auto& entry : arrays[stmt.array_name]) keys.push_back(entry.first);

				for (const auto& key : keys) {
					variables[stmt.loop_var] = AwkValue::str(key);
					run(*stmt.children[0]);
					if (loopBodyDone()) return;
				}
			}

			void runDelete(Stmt& stmt) {
				Expr& target = *stmt.exprs[0];
				if (target.kind == ExprKind::Var) {
					arrays.erase(target.name);
					return;
				}

				if (target.kind != ExprKind::ArrayRef) return;
				arrays[target.name].erase(buildSubscript(target.args));
			}

			void runPrint(Stmt& stmt) {
				std::string out;
				if (stmt.exprs.empty()) {
					out = record;
				} else {
					for (std::size_t i = 0; i < stmt.exprs.size(); ++i) {
						if (i != 0) out += OFS;
						out += eval(*stmt.exprs[i]).asString();
					}
				}

				out += ORS;
				emit(out, stmt);
			}

			void runPrintf(Stmt& stmt) {
				emit(formatPrintf(evalAll(stmt.exprs)), stmt);
			}

			void runBlock(Stmt& stmt) {
				for (auto& child : stmt.children) {
					run(*child);
					if (exiting || flow != Flow::None) return;
				}
			}

			void runExit(Stmt& stmt) {
				if (!stmt.exprs.empty()) {
					exit_status = static_cast<int>(eval(*stmt.exprs[0]).asNumber());
				}
				exiting = true;
			}

			void run(Stmt& stmt) {
				if (exiting) return;
				switch (stmt.kind) {
				case StmtKind::Empty:    return;
				case StmtKind::Block:    runBlock(stmt);   return;
				case StmtKind::If:       runIf(stmt);      return;
				case StmtKind::While:    runWhile(stmt);   return;
				case StmtKind::DoWhile:  runDoWhile(stmt); return;
				case StmtKind::For:      runFor(stmt);     return;
				case StmtKind::ForIn:    runForIn(stmt);   return;
				case StmtKind::Break:    flow = Flow::Break;    return;
				case StmtKind::Continue: flow = Flow::Continue; return;
				case StmtKind::Next:     flow = Flow::Next;     return;
				case StmtKind::Exit:     runExit(stmt);    return;
				case StmtKind::Delete:   runDelete(stmt);  return;
				case StmtKind::Return:   return;     // no user functions yet
				case StmtKind::Print:    runPrint(stmt);  return;
				case StmtKind::Printf:   runPrintf(stmt); return;
				case StmtKind::ExprStmt: eval(*stmt.exprs[0]); return;
				}
			}

			FILE* outputFile(const std::string& path, bool truncate) {
				const auto it = output_files.find(path);
				if (it != output_files.end()) return it->second;

				FILE* file = std::fopen(path.c_str(), truncate ? "wb" : "ab");
				if (file == nullptr) return nullptr;

				output_files[path] = file;
				return file;
			}

			FILE* outputPipe(const std::string& command) {
				FILE* pipe = openPipeForWriting(command);
				if (pipe == nullptr) return nullptr;

				FILE*& slot = output_files[std::string("|") + command];
				if (slot != nullptr) closePipe(slot);
				slot = pipe;
				return pipe;
			}

			FILE* emitDestination(Stmt& stmt) {
				if (stmt.redirect == Redirect::None) return stdout;

				const std::string target = eval(*stmt.redirect_target).asString();
				switch (stmt.redirect) {
				case Redirect::Truncate: return outputFile(target, true);
				case Redirect::Append:   return outputFile(target, false);
				case Redirect::Pipe:     return outputPipe(target);
				default:                 return stdout;
				}
			}

			void emit(const std::string& out, Stmt& stmt) {
				FILE* destination = emitDestination(stmt);
				if (destination == nullptr) return;

				std::fwrite(out.data(), 1, out.size(), destination);
			}

			FILE* inputFile(const std::string& path) {
				const auto it = input_files.find(path);
				if (it != input_files.end()) return it->second;

				FILE* file = std::fopen(path.c_str(), "rb");
				if (file == nullptr) return nullptr;

				input_files[path] = file;
				return file;
			}

			void bumpRecordCounters() {
				++NR;
				++FNR;
			}

			AwkValue doGetline(Expr& expr) {
				const bool from_file = expr.second != nullptr;
				FILE* source = stdin;
				if (from_file) {
					source = inputFile(eval(*expr.second).asString());
					if (source == nullptr) return AwkValue::num(-1);
				}

				std::string line;
				if (!readLine(source, line)) return AwkValue::num(0);

				if (expr.name.empty()) {
					record = line;
					splitRecord();
					bumpRecordCounters();
					return AwkValue::num(1);
				}

				variables[expr.name] = AwkValue::str(line);
				if (!from_file) bumpRecordCounters();
				return AwkValue::num(1);
			}

			bool patternHolds(Expr& expr) {
				if (expr.kind != ExprKind::Regex) return eval(expr).truthy();

				const std::regex* re = literalRegex(expr);
				return re != nullptr && searchRegex(record, *re);
			}

			bool rangeHolds(AwkRule& rule) {
				if (!rule.in_range) {
					if (!patternHolds(*rule.pattern.first)) return false;

					rule.in_range = true;
					return true;
				}

				if (patternHolds(*rule.pattern.second)) rule.in_range = false;
				return true;
			}

			bool ruleMatches(AwkRule& rule) {
				switch (rule.pattern.kind) {
				case AwkPattern::Always:    return true;
				case AwkPattern::Condition: return patternHolds(*rule.pattern.first);
				case AwkPattern::Range:     return rangeHolds(rule);
				default:                    return false;
				}
			}

			void runRule(AwkRule& rule) {
				if (ruleMatches(rule)) run(*rule.action);
			}

			void runSpecialBlocks(AwkPattern::Kind kind) {
				for (auto& rule : program.rules) {
					if (rule.pattern.kind != kind) continue;

					run(*rule.action);
					if (exiting) return;
					flow = Flow::None;
				}
			}

			void runBegins() {
				runSpecialBlocks(AwkPattern::Begin);
			}

			void runEnds() {
				runSpecialBlocks(AwkPattern::End);
			}

			void runRecordRules() {
				for (auto& rule : program.rules) {
					if (isSpecialBlock(rule.pattern)) continue;

					runRule(rule);
					if (exiting) return;
					if (flow == Flow::Next) break;
				}

				flow = Flow::None;
			}

			void processStream(FILE* file, const std::string& name) {
				FILENAME = name;
				FNR = 0;

				std::string line;
				while (!exiting) {
					if (!readLine(file, line)) break;

					record = std::move(line);
					splitRecord();
					bumpRecordCounters();
					runRecordRules();
				}
			}
		};

		struct Invocation {
			std::string program_text;
			std::string field_separator;
			std::vector<std::string> files;
			std::vector<std::pair<std::string, std::string>> var_assigns;
			bool have_program = false;
		};

		static bool loadProgramFile(Executor& exec, const std::string& path,
				Invocation& invocation) {
			const std::string native = exec.pathConv().toWin32(path);
			std::ifstream file(native, std::ios::binary);
			if (!file) {
				std::fprintf(stderr, "awk: cannot open program file: %s\n", path.c_str());
				return false;
			}

			std::ostringstream contents;
			contents << file.rdbuf();
			invocation.program_text = contents.str();
			invocation.have_program = true;
			return true;
		}

		static void addVarAssign(const std::string& assignment, Invocation& invocation) {
			const auto eq = assignment.find('=');
			if (eq == std::string::npos) return;

			invocation.var_assigns.emplace_back(assignment.substr(0, eq),
				assignment.substr(eq + 1));
		}

		static void takeProgramOrFile(const std::string& arg, Invocation& invocation) {
			if (invocation.have_program) {
				invocation.files.push_back(arg);
				return;
			}

			invocation.program_text = arg;
			invocation.have_program = true;
		}

		static bool hasFlagPrefix(const std::string& arg, const char* flag) {
			return arg.size() > 2 && arg.compare(0, 2, flag) == 0;
		}

		static int parseArgs(Executor& exec, const std::vector<std::string>& args,
				Invocation& invocation) {
			for (std::size_t i = 0; i < args.size(); ++i) {
				const std::string& arg = args[i];
				const bool has_value = i + 1 < args.size();
				if (arg == "-F" && has_value) {
					invocation.field_separator = args[++i];
					continue;
				}

				if (hasFlagPrefix(arg, "-F")) {
					invocation.field_separator = arg.substr(2);
					continue;
				}

				if (arg == "-f" && has_value) {
					if (!loadProgramFile(exec, args[++i], invocation)) return kUsageError;
					continue;
				}

				if (arg == "-v" && has_value) {
					addVarAssign(args[++i], invocation);
					continue;
				}

				if (arg == "--") {
					for (++i; i < args.size(); ++i) takeProgramOrFile(args[i], invocation);
					break;
				}

				takeProgramOrFile(arg, invocation);
			}

			return 0;
		}

		static void runInputs(Executor& exec, Interpreter& interp,
				const std::vector<std::string>& files) {
			if (files.empty()) {
				interp.processStream(stdin, "");
				return;
			}

			for (const auto& name : files) {
				FILE* file = std::fopen(exec.pathConv().toWin32(name).c_str(), "rb");
				if (file == nullptr) {
					std::fprintf(stderr, "awk: cannot open: %s\n", name.c_str());
					continue;
				}

				interp.processStream(file, name);
				std::fclose(file);
			}
		}

		// A program made only of BEGIN blocks never reads its input.
		static bool readsInput(const AwkProgram& program) {
			for (const auto& rule : program.rules) {
				if (rule.pattern.kind != AwkPattern::Begin) return true;
			}

			return false;
		}

		static void applyInvocation(const Invocation& invocation, Interpreter& interp) {
			if (!invocation.field_separator.empty()) interp.FS = invocation.field_separator;
			for (const auto& assign : invocation.var_assigns) {
				interp.variables[assign.first] = AwkValue::str(assign.second);
			}
		}

	}  // namespace awk_detail

	int builtin_awk(Executor& exec, const std::vector<std::string>& args) {
		using namespace awk_detail;

		Invocation invocation;
		const int status = parseArgs(exec, args, invocation);
		if (status != 0) return status;
		if (!invocation.have_program) {
			std::fprintf(stderr, "awk: missing program text\n");
			return kUsageError;
		}

		Parser parser(invocation.program_text);
		AwkProgram program = parser.parseProgram();
		if (parser.failed()) {
			std::fprintf(stderr, "%s\n", parser.error_msg.c_str());
			return kUsageError;
		}

		Interpreter interp(program);
		applyInvocation(invocation, interp);

		interp.runBegins();
		if (readsInput(program)) runInputs(exec, interp, invocation.files);
		interp.runEnds();
		return interp.exit_status;
	}

}  // namespace wbsh

/**
 * @file awk.cpp
 * @brief Pragmatic awk(1) subset.
 *
 * Coverage:
 *   - BEGIN { } and END { } blocks (multiple of each, in source order)
 *   - /regex/ patterns, expression patterns, range patterns (e1, e2)
 *   - $0..$NF accessors, including assignment to $N
 *   - Variables: NR, NF, FS, OFS, ORS, FILENAME
 *   - All scalar operators: + - * / % ^, == != < <= > >=, && || !,
 *     ~ !~, ?:, =, +=, -=, *=, /=, %=, ++, --
 *   - String concat by juxtaposition
 *   - Statements: if/else, while, do/while, for(;;), for(k in arr),
 *     break, continue, next, exit, delete, return-only-from-builtins
 *   - Builtins: print, printf, sprintf, length, substr, index, split,
 *     sub, gsub, match, tolower, toupper, getline (basic forms), system
 *   - Associative arrays (single-dim subscripts; multi-dim via SUBSEP join)
 *
 * Out of scope: user-defined functions, gawk extensions, the more
 * obscure builtins (asort, asorti, mktime, strftime, etc.), 2-D array
 * magic via SUBSEP beyond the basic concat behaviour.
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
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "executor.h"
#include "pathconv.h"

namespace wbsh {

// ----- AwkValue type -----------------------------------------------------------

// awk values are loosely typed: every value is a string with an optional
// numeric interpretation. We store both forms lazily.
struct AwkValue {
	double n = 0.0;
	std::string s;
	bool has_n = false;
	bool has_s = false;

	AwkValue() = default;
	static AwkValue num(double x) { AwkValue v; v.n = x; v.has_n = true; return v; }
	static AwkValue str(std::string x) { AwkValue v; v.s = std::move(x); v.has_s = true; return v; }

	double asNumber() const {
		if (has_n) return n;
		if (has_s) {
			try { return std::stod(s); } catch (...) { return 0.0; }
		}
		return 0.0;
	}
	std::string asString() const {
		if (has_s) return s;
		if (!has_n) return std::string();
		// Format like awk: integers without decimal, otherwise %g.
		double d = n;
		if (d == std::floor(d) && std::abs(d) < 1e16) {
			char buf[32];
			std::snprintf(buf, sizeof(buf), "%lld", (long long)d);
			return buf;
		}
		char buf[64];
		std::snprintf(buf, sizeof(buf), "%.6g", d);
		return buf;
	}
	bool truthy() const {
		// Strings are truthy iff non-empty (awk numeric-coercion is fine
		// because we treat the string form as authoritative when set).
		if (has_s) {
			// If the string looks numeric, use numeric interpretation.
			if (!s.empty()) {
				try { return std::stod(s) != 0.0; } catch (...) {}
				return !s.empty();
			}
			return false;
		}
		return n != 0.0;
	}
};

// ----- AST ------------------------------------------------------------------

struct Expr;
using AwkExprPtr = std::unique_ptr<Expr>;

enum class EK {
	Number, String, Regex, Var,
	Field,            // $expr
	ArrayRef,         // a[expr]
	ArrayInTest,      // (k in arr)
	Unary,            // - + ! ++ --
	PostIncDec,       // x++ / x--
	Binary,           // + - * / % ^ < <= > >= == != ~ !~ && || string-concat
	Ternary,          // a ? b : c
	Assign,           // = += -= *= /= %=  (also $field = ..)
	FieldAssign,
	Call,             // builtin call
	Getline,          // getline [var] [< file]   |   cmd | getline [var]
	Group,            // ( expr )
};

struct Stmt;
using AwkStmtPtr = std::unique_ptr<Stmt>;

struct Expr {
	EK kind;
	double num_val = 0;
	std::string str_val;
	std::string name;        // var or builtin name
	std::string op;
	AwkExprPtr a, b, c;         // operands
	std::vector<AwkExprPtr> args;
	AwkStmtPtr body;            // unused
	// Compiled regex (built lazily).
	mutable std::shared_ptr<std::regex> compiled;
};

enum class SK {
	Empty, Print, Printf, ExprStmt, Block, If, While, DoWhile, For, ForIn,
	Break, Continue, Next, Exit, Delete, Return,
};

struct Stmt {
	SK kind;
	std::vector<AwkStmtPtr> children;
	std::vector<AwkExprPtr> exprs;          // print arglist, printf arglist, if/while cond
	std::string name1, name2;            // for-in: var, array
	AwkExprPtr init, cond, step;            // for(;;)
	AwkExprPtr to_file;                     // > "file" / >> "file" / | "cmd"
	int redir_kind = 0;                  // 0 none, 1 '>', 2 '>>', 3 '|'
};

struct AwkPattern {
	enum Kind { Begin, End, Always, Expr, Range };
	Kind kind = Always;
	AwkExprPtr e1, e2;
};

struct AwkRule {
	AwkPattern pat;
	AwkStmtPtr action;
	bool in_range = false;   // runtime state for Range patterns
};

struct AwkProgram {
	std::vector<AwkRule> rules;
};

// ----- AwkLexer ---------------------------------------------------------------

enum class TK {
	END, NUM, STR, REGEX, ID, BUILTIN,
	PLUS, MINUS, STAR, SLASH, PERCENT, CARET,
	ASSIGN, PLUS_ASSIGN, MINUS_ASSIGN, STAR_ASSIGN, SLASH_ASSIGN, PERCENT_ASSIGN, CARET_ASSIGN,
	EQ, NE, LT, LE, GT, GE,
	AND, OR, NOT, MATCH, NMATCH,
	INC, DEC,
	LPAREN, RPAREN, LBRACE, RBRACE, LBRACK, RBRACK,
	SEMI, COMMA, NEWLINE, QUESTION, COLON,
	DOLLAR,
	APPEND,                       // >>
	PIPE,                         // |
	K_BEGIN, K_END, K_IF, K_ELSE, K_WHILE, K_DO, K_FOR, K_IN,
	K_BREAK, K_CONTINUE, K_NEXT, K_EXIT, K_DELETE, K_PRINT, K_PRINTF,
	K_FUNCTION, K_RETURN, K_GETLINE,
};

struct AwkTok {
	TK kind = TK::END;
	std::string text;
	double num = 0.0;
	int line = 1;
};

struct AwkLex {
	const std::string& src;
	std::size_t i = 0;
	int line = 1;
	std::vector<AwkTok> pushback;     // LIFO stack
	bool prev_was_value = false;
	explicit AwkLex(const std::string& s) : src(s) {}
	void unread(AwkTok t) { pushback.push_back(std::move(t)); }

	bool atEnd() const { return i >= src.size(); }
	char peekc(std::size_t off = 0) const {
		return i + off < src.size() ? src[i + off] : '\0';
	}
	void advance() {
		if (i < src.size()) {
			if (src[i] == '\n') ++line;
			++i;
		}
	}
	void skipBlanks() {
		while (i < src.size()) {
			char c = src[i];
			if (c == ' ' || c == '\t') { advance(); continue; }
			if (c == '\\' && i + 1 < src.size() && src[i + 1] == '\n') {
				advance(); advance(); continue;
			}
			if (c == '#') {
				while (i < src.size() && src[i] != '\n') advance();
				continue;
			}
			break;
		}
	}
	AwkTok readId() {
		std::size_t start = i;
		while (i < src.size()
		    && (std::isalnum((unsigned char)src[i]) || src[i] == '_')) advance();
		AwkTok t;
		t.line = line;
		t.text = src.substr(start, i - start);
		// Reserved words.
		static const std::unordered_map<std::string, TK> kw = {
			{ "BEGIN", TK::K_BEGIN }, { "END", TK::K_END },
			{ "if", TK::K_IF }, { "else", TK::K_ELSE },
			{ "while", TK::K_WHILE }, { "do", TK::K_DO },
			{ "for", TK::K_FOR }, { "in", TK::K_IN },
			{ "break", TK::K_BREAK }, { "continue", TK::K_CONTINUE },
			{ "next", TK::K_NEXT }, { "exit", TK::K_EXIT },
			{ "delete", TK::K_DELETE }, { "print", TK::K_PRINT },
			{ "printf", TK::K_PRINTF }, { "function", TK::K_FUNCTION },
			{ "return", TK::K_RETURN }, { "getline", TK::K_GETLINE },
		};
		auto it = kw.find(t.text);
		if (it != kw.end()) {
			t.kind = it->second;
		} else {
			t.kind = TK::ID;
		}
		return t;
	}
	AwkTok readNumber() {
		std::size_t start = i;
		while (i < src.size() && std::isdigit((unsigned char)src[i])) advance();
		if (i < src.size() && src[i] == '.') {
			advance();
			while (i < src.size() && std::isdigit((unsigned char)src[i])) advance();
		}
		if (i < src.size() && (src[i] == 'e' || src[i] == 'E')) {
			advance();
			if (i < src.size() && (src[i] == '+' || src[i] == '-')) advance();
			while (i < src.size() && std::isdigit((unsigned char)src[i])) advance();
		}
		AwkTok t;
		t.line = line;
		t.text = src.substr(start, i - start);
		try { t.num = std::stod(t.text); } catch (...) { t.num = 0; }
		t.kind = TK::NUM;
		return t;
	}
	AwkTok readString() {
		// We're at the opening quote.
		advance();
		std::string s;
		while (i < src.size() && src[i] != '"') {
			if (src[i] == '\\' && i + 1 < src.size()) {
				char n = src[i + 1];
				char out = n;
				switch (n) {
				case 'n': out = '\n'; break;
				case 't': out = '\t'; break;
				case 'r': out = '\r'; break;
				case '\\': out = '\\'; break;
				case '"': out = '"'; break;
				case '/': out = '/'; break;
				default: s.push_back('\\'); out = n; break;
				}
				s.push_back(out);
				advance(); advance();
				continue;
			}
			s.push_back(src[i]);
			advance();
		}
		if (i < src.size()) advance();   // closing quote
		AwkTok t;
		t.line = line;
		t.kind = TK::STR;
		t.text = std::move(s);
		return t;
	}
	AwkTok readRegex() {
		// We're at the opening '/'. Bash-awk semantics: only treat '/' as
		// regex when it can't be a division operator. The caller decides.
		advance();
		std::string r;
		while (i < src.size() && src[i] != '/') {
			if (src[i] == '\\' && i + 1 < src.size()) {
				r.push_back(src[i]);
				r.push_back(src[i + 1]);
				advance(); advance();
				continue;
			}
			r.push_back(src[i]);
			advance();
		}
		if (i < src.size()) advance();
		AwkTok t;
		t.line = line;
		t.kind = TK::REGEX;
		t.text = std::move(r);
		return t;
	}
	AwkTok next() {
		if (!pushback.empty()) {
			AwkTok t = std::move(pushback.back());
			pushback.pop_back();
			return t;
		}
		skipBlanks();
		AwkTok t;
		t.line = line;
		if (atEnd()) { t.kind = TK::END; return t; }
		char c = src[i];
		if (c == '\n') { advance(); t.kind = TK::NEWLINE; prev_was_value = false; return t; }
		if (c == ';') { advance(); t.kind = TK::SEMI; prev_was_value = false; return t; }
		if (c == ',') { advance(); t.kind = TK::COMMA; prev_was_value = false; return t; }
		if (c == '(') { advance(); t.kind = TK::LPAREN; prev_was_value = false; return t; }
		if (c == ')') { advance(); t.kind = TK::RPAREN; prev_was_value = true; return t; }
		if (c == '{') { advance(); t.kind = TK::LBRACE; prev_was_value = false; return t; }
		if (c == '}') { advance(); t.kind = TK::RBRACE; prev_was_value = false; return t; }
		if (c == '[') { advance(); t.kind = TK::LBRACK; prev_was_value = false; return t; }
		if (c == ']') { advance(); t.kind = TK::RBRACK; prev_was_value = true; return t; }
		if (c == '?') { advance(); t.kind = TK::QUESTION; prev_was_value = false; return t; }
		if (c == ':') { advance(); t.kind = TK::COLON; prev_was_value = false; return t; }
		if (c == '$') { advance(); t.kind = TK::DOLLAR; prev_was_value = false; return t; }
		if (c == '"') { t = readString(); prev_was_value = true; return t; }
		if (std::isdigit((unsigned char)c)
		    || (c == '.' && i + 1 < src.size() && std::isdigit((unsigned char)src[i + 1])))
		{
			t = readNumber(); prev_was_value = true; return t;
		}
		if (std::isalpha((unsigned char)c) || c == '_') {
			t = readId();
			prev_was_value = (t.kind == TK::ID);
			return t;
		}
		if (c == '/') {
			if (!prev_was_value) {
				t = readRegex();
				prev_was_value = false;
				return t;
			}
			advance();
			if (i < src.size() && src[i] == '=') { advance(); t.kind = TK::SLASH_ASSIGN; prev_was_value = false; return t; }
			t.kind = TK::SLASH; prev_was_value = false; return t;
		}
		if (c == '+') {
			advance();
			if (i < src.size() && src[i] == '+') { advance(); t.kind = TK::INC; prev_was_value = true; return t; }
			if (i < src.size() && src[i] == '=') { advance(); t.kind = TK::PLUS_ASSIGN; prev_was_value = false; return t; }
			t.kind = TK::PLUS; prev_was_value = false; return t;
		}
		if (c == '-') {
			advance();
			if (i < src.size() && src[i] == '-') { advance(); t.kind = TK::DEC; prev_was_value = true; return t; }
			if (i < src.size() && src[i] == '=') { advance(); t.kind = TK::MINUS_ASSIGN; prev_was_value = false; return t; }
			t.kind = TK::MINUS; prev_was_value = false; return t;
		}
		if (c == '*') {
			advance();
			if (i < src.size() && src[i] == '=') { advance(); t.kind = TK::STAR_ASSIGN; prev_was_value = false; return t; }
			t.kind = TK::STAR; prev_was_value = false; return t;
		}
		if (c == '%') {
			advance();
			if (i < src.size() && src[i] == '=') { advance(); t.kind = TK::PERCENT_ASSIGN; prev_was_value = false; return t; }
			t.kind = TK::PERCENT; prev_was_value = false; return t;
		}
		if (c == '^') {
			advance();
			if (i < src.size() && src[i] == '=') { advance(); t.kind = TK::CARET_ASSIGN; prev_was_value = false; return t; }
			t.kind = TK::CARET; prev_was_value = false; return t;
		}
		if (c == '=') {
			advance();
			if (i < src.size() && src[i] == '=') { advance(); t.kind = TK::EQ; prev_was_value = false; return t; }
			t.kind = TK::ASSIGN; prev_was_value = false; return t;
		}
		if (c == '!') {
			advance();
			if (i < src.size() && src[i] == '=') { advance(); t.kind = TK::NE; prev_was_value = false; return t; }
			if (i < src.size() && src[i] == '~') { advance(); t.kind = TK::NMATCH; prev_was_value = false; return t; }
			t.kind = TK::NOT; prev_was_value = false; return t;
		}
		if (c == '<') {
			advance();
			if (i < src.size() && src[i] == '=') { advance(); t.kind = TK::LE; prev_was_value = false; return t; }
			t.kind = TK::LT; prev_was_value = false; return t;
		}
		if (c == '>') {
			advance();
			if (i < src.size() && src[i] == '>') { advance(); t.kind = TK::APPEND; prev_was_value = false; return t; }
			if (i < src.size() && src[i] == '=') { advance(); t.kind = TK::GE; prev_was_value = false; return t; }
			t.kind = TK::GT; prev_was_value = false; return t;
		}
		if (c == '&') {
			advance();
			if (i < src.size() && src[i] == '&') { advance(); t.kind = TK::AND; prev_was_value = false; return t; }
			t.kind = TK::END; return t;   // unsupported
		}
		if (c == '|') {
			advance();
			if (i < src.size() && src[i] == '|') { advance(); t.kind = TK::OR; prev_was_value = false; return t; }
			t.kind = TK::PIPE; prev_was_value = false; return t;
		}
		if (c == '~') { advance(); t.kind = TK::MATCH; prev_was_value = false; return t; }
		// Unknown char: skip.
		advance();
		return next();
	}
	AwkTok& peek() {
		if (pushback.empty()) {
			AwkTok t = next();
			pushback.push_back(std::move(t));
		}
		return pushback.back();
	}
};

// ----- AwkParser -----------------------------------------------------------

struct AwkParser {
	AwkLex lex;
	std::string error_msg;

	explicit AwkParser(const std::string& s) : lex(s) {}

	[[noreturn]] void err(const std::string& m) {
		throw std::runtime_error("awk: parse error: " + m
		    + " (line " + std::to_string(lex.peek().line) + ")");
	}

	void skipTerm() {
		while (true) {
			TK k = lex.peek().kind;
			if (k == TK::SEMI || k == TK::NEWLINE) { lex.next(); continue; }
			break;
		}
	}

	AwkProgram parseAwkProgram() {
		AwkProgram p;
		skipTerm();
		while (lex.peek().kind != TK::END) {
			AwkRule r;
			parseAwkPattern(r.pat);
			if (lex.peek().kind == TK::LBRACE) {
				r.action = parseBlock();
			} else {
				// Default action is `{ print }`.
				auto blk = std::make_unique<Stmt>();
				blk->kind = SK::Block;
				auto pr = std::make_unique<Stmt>();
				pr->kind = SK::Print;
				blk->children.push_back(std::move(pr));
				r.action = std::move(blk);
			}
			p.rules.push_back(std::move(r));
			skipTerm();
		}
		return p;
	}

	void parseAwkPattern(AwkPattern& pat) {
		if (lex.peek().kind == TK::K_BEGIN) {
			lex.next(); pat.kind = AwkPattern::Begin; return;
		}
		if (lex.peek().kind == TK::K_END) {
			lex.next(); pat.kind = AwkPattern::End; return;
		}
		if (lex.peek().kind == TK::LBRACE) {
			pat.kind = AwkPattern::Always; return;
		}
		// expr [, expr]
		pat.e1 = parseExpr();
		if (lex.peek().kind == TK::COMMA) {
			lex.next();
			pat.e2 = parseExpr();
			pat.kind = AwkPattern::Range;
		} else {
			pat.kind = AwkPattern::Expr;
		}
	}

	AwkStmtPtr parseBlock() {
		if (lex.peek().kind != TK::LBRACE) err("expected '{'");
		lex.next();
		auto blk = std::make_unique<Stmt>();
		blk->kind = SK::Block;
		skipTerm();
		while (lex.peek().kind != TK::RBRACE && lex.peek().kind != TK::END) {
			blk->children.push_back(parseStmt());
			skipTerm();
		}
		if (lex.peek().kind == TK::RBRACE) lex.next();
		return blk;
	}

	AwkStmtPtr parseStmt() {
		TK k = lex.peek().kind;
		if (k == TK::LBRACE) return parseBlock();
		if (k == TK::K_IF) return parseIf();
		if (k == TK::K_WHILE) return parseWhile();
		if (k == TK::K_DO) return parseDoWhile();
		if (k == TK::K_FOR) return parseFor();
		if (k == TK::K_BREAK) { lex.next(); auto s = std::make_unique<Stmt>(); s->kind = SK::Break; return s; }
		if (k == TK::K_CONTINUE) { lex.next(); auto s = std::make_unique<Stmt>(); s->kind = SK::Continue; return s; }
		if (k == TK::K_NEXT) { lex.next(); auto s = std::make_unique<Stmt>(); s->kind = SK::Next; return s; }
		if (k == TK::K_EXIT) {
			lex.next();
			auto s = std::make_unique<Stmt>(); s->kind = SK::Exit;
			if (lex.peek().kind != TK::SEMI && lex.peek().kind != TK::NEWLINE
			    && lex.peek().kind != TK::RBRACE && lex.peek().kind != TK::END) {
				s->exprs.push_back(parseExpr());
			}
			return s;
		}
		if (k == TK::K_DELETE) {
			lex.next();
			auto s = std::make_unique<Stmt>(); s->kind = SK::Delete;
			s->exprs.push_back(parseUnary());
			return s;
		}
		if (k == TK::K_PRINT || k == TK::K_PRINTF) {
			lex.next();
			auto s = std::make_unique<Stmt>();
			s->kind = (k == TK::K_PRINT) ? SK::Print : SK::Printf;
			// argument list (possibly empty for print)
			if (lex.peek().kind != TK::SEMI && lex.peek().kind != TK::NEWLINE
			    && lex.peek().kind != TK::RBRACE && lex.peek().kind != TK::END
			    && lex.peek().kind != TK::GT && lex.peek().kind != TK::APPEND
			    && lex.peek().kind != TK::PIPE)
			{
				s->exprs.push_back(parseExpr());
				while (lex.peek().kind == TK::COMMA) {
					lex.next();
					s->exprs.push_back(parseExpr());
				}
			}
			if (lex.peek().kind == TK::GT)     { lex.next(); s->redir_kind = 1; s->to_file = parseExpr(); }
			else if (lex.peek().kind == TK::APPEND) { lex.next(); s->redir_kind = 2; s->to_file = parseExpr(); }
			else if (lex.peek().kind == TK::PIPE)   { lex.next(); s->redir_kind = 3; s->to_file = parseExpr(); }
			return s;
		}
		// Expression statement (possibly assignment).
		auto s = std::make_unique<Stmt>();
		s->kind = SK::ExprStmt;
		s->exprs.push_back(parseExpr());
		return s;
	}

	AwkStmtPtr parseIf() {
		lex.next();   // 'if'
		if (lex.peek().kind != TK::LPAREN) err("expected '(' after if");
		lex.next();
		auto cond = parseExpr();
		if (lex.peek().kind != TK::RPAREN) err("expected ')'");
		lex.next();
		skipTerm();
		auto then_s = parseStmt();
		AwkStmtPtr else_s;
		skipTerm();
		if (lex.peek().kind == TK::K_ELSE) {
			lex.next(); skipTerm();
			else_s = parseStmt();
		}
		auto s = std::make_unique<Stmt>();
		s->kind = SK::If;
		s->exprs.push_back(std::move(cond));
		s->children.push_back(std::move(then_s));
		if (else_s) s->children.push_back(std::move(else_s));
		return s;
	}

	AwkStmtPtr parseWhile() {
		lex.next();
		if (lex.peek().kind != TK::LPAREN) err("expected '('");
		lex.next();
		auto cond = parseExpr();
		if (lex.peek().kind != TK::RPAREN) err("expected ')'");
		lex.next();
		skipTerm();
		auto body = parseStmt();
		auto s = std::make_unique<Stmt>();
		s->kind = SK::While;
		s->exprs.push_back(std::move(cond));
		s->children.push_back(std::move(body));
		return s;
	}

	AwkStmtPtr parseDoWhile() {
		lex.next();
		skipTerm();
		auto body = parseStmt();
		skipTerm();
		if (lex.peek().kind != TK::K_WHILE) err("expected 'while' after do-body");
		lex.next();
		if (lex.peek().kind != TK::LPAREN) err("expected '('");
		lex.next();
		auto cond = parseExpr();
		if (lex.peek().kind != TK::RPAREN) err("expected ')'");
		lex.next();
		auto s = std::make_unique<Stmt>();
		s->kind = SK::DoWhile;
		s->exprs.push_back(std::move(cond));
		s->children.push_back(std::move(body));
		return s;
	}

	AwkStmtPtr parseFor() {
		lex.next();   // 'for'
		if (lex.peek().kind != TK::LPAREN) err("expected '('");
		lex.next();
		// for (k in arr) { ... }
		if (lex.peek().kind == TK::ID) {
			AwkTok save_id = lex.next();
			if (lex.peek().kind == TK::K_IN) {
				lex.next();
				AwkTok arr_name;
				if (lex.peek().kind != TK::ID) err("expected array name in 'for in'");
				arr_name = lex.next();
				if (lex.peek().kind != TK::RPAREN) err("expected ')'");
				lex.next();
				skipTerm();
				auto body = parseStmt();
				auto s = std::make_unique<Stmt>();
				s->kind = SK::ForIn;
				s->name1 = save_id.text;
				s->name2 = arr_name.text;
				s->children.push_back(std::move(body));
				return s;
			}
			// Not for-in; push the ID back onto the lookahead stack.
			lex.unread(std::move(save_id));
		}
		// for (init ; cond ; step) body
		AwkExprPtr init, cond, step;
		if (lex.peek().kind != TK::SEMI) init = parseExpr();
		if (lex.peek().kind != TK::SEMI) err("expected ';'");
		lex.next();
		if (lex.peek().kind != TK::SEMI) cond = parseExpr();
		if (lex.peek().kind != TK::SEMI) err("expected ';'");
		lex.next();
		if (lex.peek().kind != TK::RPAREN) step = parseExpr();
		if (lex.peek().kind != TK::RPAREN) err("expected ')'");
		lex.next();
		skipTerm();
		auto body = parseStmt();
		auto s = std::make_unique<Stmt>();
		s->kind = SK::For;
		s->init = std::move(init);
		s->cond = std::move(cond);
		s->step = std::move(step);
		s->children.push_back(std::move(body));
		return s;
	}

	// ---- Expressions ----
	AwkExprPtr parseExpr() { return parseTernary(); }

	AwkExprPtr parseTernary() {
		auto a = parseLogicOr();
		if (lex.peek().kind == TK::QUESTION) {
			lex.next();
			auto b = parseTernary();
			if (lex.peek().kind != TK::COLON) err("expected ':'");
			lex.next();
			auto c = parseTernary();
			auto e = std::make_unique<Expr>();
			e->kind = EK::Ternary;
			e->a = std::move(a);
			e->b = std::move(b);
			e->c = std::move(c);
			return e;
		}
		// Assignments are right-associative; allow lhs op= rhs after ternary.
		TK k = lex.peek().kind;
		if (k == TK::ASSIGN || k == TK::PLUS_ASSIGN || k == TK::MINUS_ASSIGN
		    || k == TK::STAR_ASSIGN || k == TK::SLASH_ASSIGN
		    || k == TK::PERCENT_ASSIGN || k == TK::CARET_ASSIGN)
		{
			std::string op;
			switch (k) {
			case TK::ASSIGN: op = "="; break;
			case TK::PLUS_ASSIGN: op = "+="; break;
			case TK::MINUS_ASSIGN: op = "-="; break;
			case TK::STAR_ASSIGN: op = "*="; break;
			case TK::SLASH_ASSIGN: op = "/="; break;
			case TK::PERCENT_ASSIGN: op = "%="; break;
			case TK::CARET_ASSIGN: op = "^="; break;
			default: break;
			}
			lex.next();
			auto rhs = parseTernary();
			auto e = std::make_unique<Expr>();
			if (a->kind == EK::Field) {
				e->kind = EK::FieldAssign;
				e->op = op;
				e->a = std::move(a->a);
				e->b = std::move(rhs);
			} else {
				e->kind = EK::Assign;
				e->op = op;
				e->a = std::move(a);
				e->b = std::move(rhs);
			}
			return e;
		}
		return a;
	}

	AwkExprPtr parseLogicOr() {
		auto a = parseLogicAnd();
		while (lex.peek().kind == TK::OR) {
			lex.next();
			auto b = parseLogicAnd();
			auto e = std::make_unique<Expr>();
			e->kind = EK::Binary; e->op = "||";
			e->a = std::move(a); e->b = std::move(b);
			a = std::move(e);
		}
		return a;
	}
	AwkExprPtr parseLogicAnd() {
		auto a = parseInTest();
		while (lex.peek().kind == TK::AND) {
			lex.next();
			auto b = parseInTest();
			auto e = std::make_unique<Expr>();
			e->kind = EK::Binary; e->op = "&&";
			e->a = std::move(a); e->b = std::move(b);
			a = std::move(e);
		}
		return a;
	}
	AwkExprPtr parseInTest() {
		auto a = parseMatch();
		if (lex.peek().kind == TK::K_IN) {
			lex.next();
			if (lex.peek().kind != TK::ID) err("expected array name after 'in'");
			AwkTok n = lex.next();
			auto e = std::make_unique<Expr>();
			e->kind = EK::ArrayInTest;
			e->a = std::move(a);
			e->name = n.text;
			return e;
		}
		return a;
	}
	AwkExprPtr parseMatch() {
		auto a = parseRel();
		while (lex.peek().kind == TK::MATCH || lex.peek().kind == TK::NMATCH) {
			std::string op = lex.peek().kind == TK::MATCH ? "~" : "!~";
			lex.next();
			auto b = parseRel();
			auto e = std::make_unique<Expr>();
			e->kind = EK::Binary; e->op = op;
			e->a = std::move(a); e->b = std::move(b);
			a = std::move(e);
		}
		return a;
	}
	AwkExprPtr parseRel() {
		auto a = parseConcat();
		while (true) {
			TK k = lex.peek().kind;
			std::string op;
			switch (k) {
			case TK::LT: op = "<"; break;
			case TK::LE: op = "<="; break;
			case TK::GT: op = ">"; break;
			case TK::GE: op = ">="; break;
			case TK::EQ: op = "=="; break;
			case TK::NE: op = "!="; break;
			default: return a;
			}
			lex.next();
			auto b = parseConcat();
			auto e = std::make_unique<Expr>();
			e->kind = EK::Binary; e->op = op;
			e->a = std::move(a); e->b = std::move(b);
			a = std::move(e);
		}
	}
	bool startsConcat(TK k) {
		switch (k) {
		case TK::NUM: case TK::STR: case TK::ID: case TK::DOLLAR:
		case TK::LPAREN: case TK::NOT: case TK::MINUS: case TK::PLUS:
		case TK::INC: case TK::DEC: case TK::REGEX:
			return true;
		default:
			return false;
		}
	}
	AwkExprPtr parseConcat() {
		auto a = parseAdd();
		while (startsConcat(lex.peek().kind)) {
			auto b = parseAdd();
			auto e = std::make_unique<Expr>();
			e->kind = EK::Binary; e->op = " ";   // string concat
			e->a = std::move(a); e->b = std::move(b);
			a = std::move(e);
		}
		return a;
	}
	AwkExprPtr parseAdd() {
		auto a = parseMul();
		while (lex.peek().kind == TK::PLUS || lex.peek().kind == TK::MINUS) {
			std::string op = lex.peek().kind == TK::PLUS ? "+" : "-";
			lex.next();
			auto b = parseMul();
			auto e = std::make_unique<Expr>();
			e->kind = EK::Binary; e->op = op;
			e->a = std::move(a); e->b = std::move(b);
			a = std::move(e);
		}
		return a;
	}
	AwkExprPtr parseMul() {
		auto a = parseExp();
		while (true) {
			TK k = lex.peek().kind;
			std::string op;
			if (k == TK::STAR) op = "*";
			else if (k == TK::SLASH) op = "/";
			else if (k == TK::PERCENT) op = "%";
			else return a;
			lex.next();
			auto b = parseExp();
			auto e = std::make_unique<Expr>();
			e->kind = EK::Binary; e->op = op;
			e->a = std::move(a); e->b = std::move(b);
			a = std::move(e);
		}
	}
	AwkExprPtr parseExp() {
		auto a = parseUnary();
		if (lex.peek().kind == TK::CARET) {
			lex.next();
			auto b = parseExp();   // right-associative
			auto e = std::make_unique<Expr>();
			e->kind = EK::Binary; e->op = "^";
			e->a = std::move(a); e->b = std::move(b);
			a = std::move(e);
		}
		return a;
	}
	AwkExprPtr parseUnary() {
		TK k = lex.peek().kind;
		if (k == TK::NOT || k == TK::MINUS || k == TK::PLUS
		    || k == TK::INC || k == TK::DEC)
		{
			std::string op;
			switch (k) {
			case TK::NOT: op = "!"; break;
			case TK::MINUS: op = "-"; break;
			case TK::PLUS: op = "+"; break;
			case TK::INC: op = "++"; break;
			case TK::DEC: op = "--"; break;
			default: break;
			}
			lex.next();
			auto inner = parseUnary();
			auto e = std::make_unique<Expr>();
			e->kind = EK::Unary; e->op = op;
			e->a = std::move(inner);
			return e;
		}
		return parsePostfix();
	}
	AwkExprPtr parsePostfix() {
		auto a = parsePrimary();
		while (true) {
			TK k = lex.peek().kind;
			if (k == TK::INC || k == TK::DEC) {
				std::string op = (k == TK::INC) ? "++" : "--";
				lex.next();
				auto e = std::make_unique<Expr>();
				e->kind = EK::PostIncDec; e->op = op;
				e->a = std::move(a);
				a = std::move(e);
				continue;
			}
			break;
		}
		return a;
	}
	AwkExprPtr parsePrimary() {
		AwkTok t = lex.peek();
		if (t.kind == TK::NUM) {
			lex.next();
			auto e = std::make_unique<Expr>();
			e->kind = EK::Number; e->num_val = t.num;
			return e;
		}
		if (t.kind == TK::STR) {
			lex.next();
			auto e = std::make_unique<Expr>();
			e->kind = EK::String; e->str_val = t.text;
			return e;
		}
		if (t.kind == TK::REGEX) {
			lex.next();
			auto e = std::make_unique<Expr>();
			e->kind = EK::Regex; e->str_val = t.text;
			return e;
		}
		if (t.kind == TK::DOLLAR) {
			lex.next();
			auto inner = parseUnary();
			auto e = std::make_unique<Expr>();
			e->kind = EK::Field;
			e->a = std::move(inner);
			return e;
		}
		if (t.kind == TK::LPAREN) {
			lex.next();
			auto inner = parseExpr();
			if (lex.peek().kind != TK::RPAREN) err("expected ')'");
			lex.next();
			auto e = std::make_unique<Expr>();
			e->kind = EK::Group; e->a = std::move(inner);
			return e;
		}
		if (t.kind == TK::K_GETLINE) {
			lex.next();
			auto e = std::make_unique<Expr>();
			e->kind = EK::Getline;
			// Optional variable target.
			if (lex.peek().kind == TK::ID) {
				e->name = lex.next().text;
			}
			// Optional `< file`.
			if (lex.peek().kind == TK::LT) {
				lex.next();
				e->b = parseUnary();
			}
			return e;
		}
		if (t.kind == TK::ID) {
			lex.next();
			// Function call?
			if (lex.peek().kind == TK::LPAREN) {
				lex.next();
				auto e = std::make_unique<Expr>();
				e->kind = EK::Call;
				e->name = t.text;
				if (lex.peek().kind != TK::RPAREN) {
					e->args.push_back(parseExpr());
					while (lex.peek().kind == TK::COMMA) {
						lex.next();
						e->args.push_back(parseExpr());
					}
				}
				if (lex.peek().kind != TK::RPAREN) err("expected ')'");
				lex.next();
				return e;
			}
			// Array reference?
			if (lex.peek().kind == TK::LBRACK) {
				lex.next();
				std::vector<AwkExprPtr> subs;
				subs.push_back(parseExpr());
				while (lex.peek().kind == TK::COMMA) {
					lex.next();
					subs.push_back(parseExpr());
				}
				if (lex.peek().kind != TK::RBRACK) err("expected ']'");
				lex.next();
				auto e = std::make_unique<Expr>();
				e->kind = EK::ArrayRef;
				e->name = t.text;
				e->args = std::move(subs);
				return e;
			}
			auto e = std::make_unique<Expr>();
			e->kind = EK::Var;
			e->name = t.text;
			return e;
		}
		err("unexpected token");
		return nullptr;
	}
};

// ----- AwkInterpreter ---------------------------------------------------------

struct AwkInterp {
	AwkProgram& prog;
	std::unordered_map<std::string, AwkValue> vars;
	std::unordered_map<std::string, std::map<std::string, AwkValue>> arrays;
	std::vector<std::string> fields;          // $1..$NF (with $0 maintained separately)
	std::string record;                        // $0
	long long NR = 0;
	long long FNR = 0;
	std::string FS = " ";   // " " is special: any-whitespace
	std::string OFS = " ";
	std::string ORS = "\n";
	std::string FILENAME;
	std::string SUBSEP = std::string(1, '\x1c');
	bool exiting = false;
	int exit_status = 0;
	std::vector<FILE*> open_outputs;
	std::map<std::string, FILE*> output_files;
	std::map<std::string, FILE*> input_files;
	// Marker exceptions for control flow inside loops / records.
	struct AwkBreakEx {}; struct AwkContinueEx {}; struct AwkNextEx {}; struct AwkExitEx {};

	explicit AwkInterp(AwkProgram& p) : prog(p) {}
	~AwkInterp() {
		for (auto& kv : output_files) if (kv.second) std::fclose(kv.second);
		for (auto& kv : input_files)  if (kv.second && kv.second != stdin) std::fclose(kv.second);
	}

	// Convert a string value to a field array using current FS.
	void splitRecord() {
		fields.clear();
		if (FS == " ") {
			// Whitespace-FS: any run of spaces/tabs (and leading/trailing
			// stripped). Newlines never split here in practice.
			std::size_t i = 0, n = record.size();
			while (i < n) {
				while (i < n && (record[i] == ' ' || record[i] == '\t')) ++i;
				if (i >= n) break;
				std::size_t s = i;
				while (i < n && record[i] != ' ' && record[i] != '\t') ++i;
				fields.push_back(record.substr(s, i - s));
			}
		} else if (FS.size() == 1) {
			std::size_t i = 0, n = record.size();
			std::string cur;
			while (i < n) {
				if (record[i] == FS[0]) {
					fields.push_back(std::move(cur));
					cur.clear();
				} else cur.push_back(record[i]);
				++i;
			}
			fields.push_back(std::move(cur));
		} else {
			// FS is a regex.
			try {
				std::regex re(FS);
				auto begin = std::sregex_token_iterator(record.begin(), record.end(), re, -1);
				auto end   = std::sregex_token_iterator();
				for (auto it = begin; it != end; ++it) fields.push_back(*it);
			} catch (...) {
				fields.push_back(record);
			}
		}
	}

	void rebuildRecord() {
		// $0 is the OFS-joined fields after any field is mutated.
		record.clear();
		for (std::size_t i = 0; i < fields.size(); ++i) {
			if (i) record += OFS;
			record += fields[i];
		}
		vars["NF"] = AwkValue::num((double)fields.size());
	}

	AwkValue getField(int n) {
		if (n == 0) return AwkValue::str(record);
		if (n < 0 || (std::size_t)n > fields.size()) return AwkValue::str("");
		return AwkValue::str(fields[(std::size_t)n - 1]);
	}
	void setField(int n, std::string v) {
		if (n == 0) {
			record = v;
			splitRecord();
			vars["NF"] = AwkValue::num((double)fields.size());
			return;
		}
		if (n < 0) return;
		if ((std::size_t)n > fields.size()) fields.resize((std::size_t)n);
		fields[(std::size_t)n - 1] = std::move(v);
		rebuildRecord();
	}

	AwkValue getVar(const std::string& name) {
		if (name == "NF") return AwkValue::num((double)fields.size());
		if (name == "NR") return AwkValue::num((double)NR);
		if (name == "FNR") return AwkValue::num((double)FNR);
		if (name == "FS") return AwkValue::str(FS);
		if (name == "OFS") return AwkValue::str(OFS);
		if (name == "ORS") return AwkValue::str(ORS);
		if (name == "FILENAME") return AwkValue::str(FILENAME);
		if (name == "SUBSEP") return AwkValue::str(SUBSEP);
		auto it = vars.find(name);
		if (it == vars.end()) return AwkValue::str("");
		return it->second;
	}
	void setVar(const std::string& name, AwkValue v) {
		if (name == "FS") { FS = v.asString(); return; }
		if (name == "OFS") { OFS = v.asString(); return; }
		if (name == "ORS") { ORS = v.asString(); return; }
		if (name == "NF") {
			int nf = (int)v.asNumber();
			if (nf < 0) nf = 0;
			fields.resize((std::size_t)nf);
			rebuildRecord();
			return;
		}
		if (name == "NR") { NR = (long long)v.asNumber(); return; }
		if (name == "FNR") { FNR = (long long)v.asNumber(); return; }
		if (name == "FILENAME") { FILENAME = v.asString(); return; }
		if (name == "SUBSEP") { SUBSEP = v.asString(); return; }
		vars[name] = std::move(v);
	}

	std::string buildSubscript(const std::vector<AwkExprPtr>& subs) {
		std::string key;
		for (std::size_t i = 0; i < subs.size(); ++i) {
			if (i) key += SUBSEP;
			key += eval(*subs[i]).asString();
		}
		return key;
	}

	// ---- Expression evaluation ----
	AwkValue eval(Expr& e) {
		switch (e.kind) {
		case EK::Number: return AwkValue::num(e.num_val);
		case EK::String: return AwkValue::str(e.str_val);
		case EK::Regex:  return AwkValue::str(e.str_val);
		case EK::Var:    return getVar(e.name);
		case EK::Field: {
			int n = (int)eval(*e.a).asNumber();
			return getField(n);
		}
		case EK::ArrayRef: {
			std::string key = buildSubscript(e.args);
			auto& m = arrays[e.name];
			auto it = m.find(key);
			return it == m.end() ? AwkValue::str("") : it->second;
		}
		case EK::ArrayInTest: {
			std::string key = eval(*e.a).asString();
			auto& m = arrays[e.name];
			return AwkValue::num(m.count(key) ? 1.0 : 0.0);
		}
		case EK::Group: return eval(*e.a);
		case EK::Unary: {
			AwkValue v = eval(*e.a);
			if (e.op == "!") return AwkValue::num(v.truthy() ? 0.0 : 1.0);
			if (e.op == "-") return AwkValue::num(-v.asNumber());
			if (e.op == "+") return AwkValue::num(v.asNumber());
			if (e.op == "++" || e.op == "--") {
				double cur = v.asNumber();
				double nv = (e.op == "++") ? cur + 1 : cur - 1;
				assignLValue(*e.a, AwkValue::num(nv));
				return AwkValue::num(nv);
			}
			return AwkValue::str("");
		}
		case EK::PostIncDec: {
			double cur = eval(*e.a).asNumber();
			double nv = (e.op == "++") ? cur + 1 : cur - 1;
			assignLValue(*e.a, AwkValue::num(nv));
			return AwkValue::num(cur);
		}
		case EK::Binary: {
			if (e.op == "&&") {
				if (!eval(*e.a).truthy()) return AwkValue::num(0.0);
				return AwkValue::num(eval(*e.b).truthy() ? 1.0 : 0.0);
			}
			if (e.op == "||") {
				if (eval(*e.a).truthy()) return AwkValue::num(1.0);
				return AwkValue::num(eval(*e.b).truthy() ? 1.0 : 0.0);
			}
			if (e.op == "~" || e.op == "!~") {
				std::string s = eval(*e.a).asString();
				std::string pat;
				if (e.b->kind == EK::Regex) pat = e.b->str_val;
				else pat = eval(*e.b).asString();
				bool m = false;
				try {
					std::regex re(pat);
					m = std::regex_search(s, re);
				} catch (...) {}
				bool r = (e.op == "~") ? m : !m;
				return AwkValue::num(r ? 1.0 : 0.0);
			}
			if (e.op == " ") {
				std::string a = eval(*e.a).asString();
				std::string b = eval(*e.b).asString();
				return AwkValue::str(a + b);
			}
			AwkValue av = eval(*e.a);
			AwkValue bv = eval(*e.b);
			if (e.op == "+") return AwkValue::num(av.asNumber() + bv.asNumber());
			if (e.op == "-") return AwkValue::num(av.asNumber() - bv.asNumber());
			if (e.op == "*") return AwkValue::num(av.asNumber() * bv.asNumber());
			if (e.op == "/") {
				double d = bv.asNumber();
				if (d == 0) return AwkValue::num(0);
				return AwkValue::num(av.asNumber() / d);
			}
			if (e.op == "%") {
				double d = bv.asNumber();
				if (d == 0) return AwkValue::num(0);
				return AwkValue::num(std::fmod(av.asNumber(), d));
			}
			if (e.op == "^") return AwkValue::num(std::pow(av.asNumber(), bv.asNumber()));
			if (e.op == "==" || e.op == "!=" || e.op == "<" || e.op == "<="
			    || e.op == ">" || e.op == ">=")
			{
				bool numeric = av.has_n || bv.has_n;
				bool r;
				if (numeric) {
					double a = av.asNumber(), b = bv.asNumber();
					if (e.op == "==") r = a == b;
					else if (e.op == "!=") r = a != b;
					else if (e.op == "<")  r = a < b;
					else if (e.op == "<=") r = a <= b;
					else if (e.op == ">")  r = a > b;
					else                    r = a >= b;
				} else {
					std::string a = av.asString(), b = bv.asString();
					if (e.op == "==") r = a == b;
					else if (e.op == "!=") r = a != b;
					else if (e.op == "<")  r = a < b;
					else if (e.op == "<=") r = a <= b;
					else if (e.op == ">")  r = a > b;
					else                    r = a >= b;
				}
				return AwkValue::num(r ? 1.0 : 0.0);
			}
			return AwkValue::str("");
		}
		case EK::Ternary: {
			AwkValue c = eval(*e.a);
			return c.truthy() ? eval(*e.b) : eval(*e.c);
		}
		case EK::Assign:      return assignTo(*e.a, e.op, eval(*e.b));
		case EK::FieldAssign: {
			int n = (int)eval(*e.a).asNumber();
			AwkValue cur = getField(n);
			AwkValue rhs = eval(*e.b);
			AwkValue out = applyOp(cur, e.op, rhs);
			setField(n, out.asString());
			return out;
		}
		case EK::Call:    return callBuiltin(e);
		case EK::Getline: return doGetline(e);
		}
		return AwkValue::str("");
	}

	AwkValue applyOp(const AwkValue& cur, const std::string& op, const AwkValue& rhs) {
		if (op == "=")  return rhs;
		double a = cur.asNumber(), b = rhs.asNumber();
		if (op == "+=") return AwkValue::num(a + b);
		if (op == "-=") return AwkValue::num(a - b);
		if (op == "*=") return AwkValue::num(a * b);
		if (op == "/=") return AwkValue::num(b == 0 ? 0 : a / b);
		if (op == "%=") return AwkValue::num(b == 0 ? 0 : std::fmod(a, b));
		if (op == "^=") return AwkValue::num(std::pow(a, b));
		return rhs;
	}

	AwkValue assignTo(Expr& lhs, const std::string& op, AwkValue rhs) {
		if (lhs.kind == EK::Var) {
			AwkValue cur = getVar(lhs.name);
			AwkValue out = applyOp(cur, op, rhs);
			setVar(lhs.name, out);
			return out;
		}
		if (lhs.kind == EK::ArrayRef) {
			std::string key = buildSubscript(lhs.args);
			auto& m = arrays[lhs.name];
			AwkValue cur = m.count(key) ? m[key] : AwkValue::str("");
			AwkValue out = applyOp(cur, op, rhs);
			m[key] = out;
			return out;
		}
		if (lhs.kind == EK::Field) {
			int n = (int)eval(*lhs.a).asNumber();
			AwkValue cur = getField(n);
			AwkValue out = applyOp(cur, op, rhs);
			setField(n, out.asString());
			return out;
		}
		return rhs;
	}

	void assignLValue(Expr& lhs, AwkValue v) {
		assignTo(lhs, "=", std::move(v));
	}

	AwkValue callBuiltin(Expr& e) {
		const std::string& n = e.name;
		auto sval = [&](std::size_t i) -> std::string {
			return i < e.args.size() ? eval(*e.args[i]).asString() : std::string();
		};
		auto nval = [&](std::size_t i) -> double {
			return i < e.args.size() ? eval(*e.args[i]).asNumber() : 0.0;
		};
		if (n == "length") {
			if (e.args.empty()) return AwkValue::num((double)record.size());
			return AwkValue::num((double)sval(0).size());
		}
		if (n == "substr") {
			std::string s = sval(0);
			long long start = (long long)nval(1);
			if (start < 1) start = 1;
			long long len = (e.args.size() >= 3)
			    ? (long long)nval(2)
			    : (long long)s.size() - start + 1;
			if (start > (long long)s.size() || len <= 0) return AwkValue::str("");
			return AwkValue::str(s.substr((std::size_t)(start - 1),
			    (std::size_t)std::min<long long>(len, (long long)s.size() - start + 1)));
		}
		if (n == "index") {
			auto s = sval(0); auto t = sval(1);
			if (t.empty()) return AwkValue::num(0);
			auto p = s.find(t);
			return AwkValue::num(p == std::string::npos ? 0 : (double)(p + 1));
		}
		if (n == "split") {
			std::string s = sval(0);
			std::string sep = (e.args.size() >= 3) ? sval(2) : FS;
			std::string aname;
			if (e.args.size() >= 2 && e.args[1]->kind == EK::Var) {
				aname = e.args[1]->name;
			} else if (e.args.size() >= 2 && e.args[1]->kind == EK::ArrayRef) {
				aname = e.args[1]->name;
			}
			arrays[aname].clear();
			std::vector<std::string> parts;
			if (sep == " ") {
				std::size_t i = 0, m = s.size();
				while (i < m) {
					while (i < m && (s[i] == ' ' || s[i] == '\t')) ++i;
					if (i >= m) break;
					std::size_t st = i;
					while (i < m && s[i] != ' ' && s[i] != '\t') ++i;
					parts.push_back(s.substr(st, i - st));
				}
			} else if (sep.size() == 1) {
				std::string cur;
				for (char c : s) {
					if (c == sep[0]) { parts.push_back(std::move(cur)); cur.clear(); }
					else cur.push_back(c);
				}
				parts.push_back(std::move(cur));
			} else {
				try {
					std::regex re(sep);
					auto bg = std::sregex_token_iterator(s.begin(), s.end(), re, -1);
					auto ed = std::sregex_token_iterator();
					for (auto it = bg; it != ed; ++it) parts.push_back(*it);
				} catch (...) { parts.push_back(s); }
			}
			for (std::size_t i = 0; i < parts.size(); ++i) {
				arrays[aname][std::to_string(i + 1)] = AwkValue::str(parts[i]);
			}
			return AwkValue::num((double)parts.size());
		}
		if (n == "sub" || n == "gsub") {
			std::string pat = sval(0);
			std::string rep = sval(1);
			Expr* target = (e.args.size() >= 3) ? e.args[2].get() : nullptr;
			std::string subject;
			if (target) subject = eval(*target).asString();
			else subject = record;
			int count = 0;
			try {
				std::regex re(pat);
				if (n == "sub") {
					std::smatch m;
					if (std::regex_search(subject, m, re)) {
						subject = m.prefix().str() + std::regex_replace(m[0].str(), re, rep) + m.suffix().str();
						count = 1;
					}
				} else {
					std::string out;
					auto begin = std::sregex_iterator(subject.begin(), subject.end(), re);
					auto end = std::sregex_iterator();
					std::size_t prev = 0;
					for (auto it = begin; it != end; ++it) {
						out.append(subject, prev, it->position() - prev);
						out.append(it->format(rep));
						prev = it->position() + it->length();
						++count;
					}
					out.append(subject, prev, std::string::npos);
					subject = std::move(out);
				}
			} catch (...) {}
			if (target) assignLValue(*target, AwkValue::str(subject));
			else { record = std::move(subject); splitRecord(); }
			return AwkValue::num((double)count);
		}
		if (n == "match") {
			std::string s = sval(0);
			std::string pat = sval(1);
			try {
				std::regex re(pat);
				std::smatch m;
				if (std::regex_search(s, m, re)) {
					vars["RSTART"] = AwkValue::num((double)(m.position(0) + 1));
					vars["RLENGTH"] = AwkValue::num((double)m.length(0));
					return AwkValue::num((double)(m.position(0) + 1));
				}
			} catch (...) {}
			vars["RSTART"] = AwkValue::num(0);
			vars["RLENGTH"] = AwkValue::num(-1);
			return AwkValue::num(0);
		}
		if (n == "tolower") {
			std::string s = sval(0);
			for (auto& c : s) c = (char)std::tolower((unsigned char)c);
			return AwkValue::str(s);
		}
		if (n == "toupper") {
			std::string s = sval(0);
			for (auto& c : s) c = (char)std::toupper((unsigned char)c);
			return AwkValue::str(s);
		}
		if (n == "sprintf" || n == "printf") {
			std::vector<AwkValue> a;
			for (auto& x : e.args) a.push_back(eval(*x));
			std::string out = formatPrintf(a);
			if (n == "sprintf") return AwkValue::str(out);
			std::fputs(out.c_str(), stdout);
			return AwkValue::str("");
		}
		if (n == "system") {
			std::string cmd = sval(0);
			int rc = std::system(cmd.c_str());
			return AwkValue::num((double)rc);
		}
		if (n == "int") return AwkValue::num(std::trunc(nval(0)));
		if (n == "sqrt") return AwkValue::num(std::sqrt(nval(0)));
		if (n == "exp") return AwkValue::num(std::exp(nval(0)));
		if (n == "log") return AwkValue::num(std::log(nval(0)));
		if (n == "sin") return AwkValue::num(std::sin(nval(0)));
		if (n == "cos") return AwkValue::num(std::cos(nval(0)));
		if (n == "atan2") return AwkValue::num(std::atan2(nval(0), nval(1)));
		if (n == "rand") return AwkValue::num((double)std::rand() / RAND_MAX);
		if (n == "srand") {
			unsigned s = (unsigned)nval(0);
			std::srand(s);
			return AwkValue::num(0);
		}
		// Unknown — return empty.
		return AwkValue::str("");
	}

	std::string formatPrintf(const std::vector<AwkValue>& args) {
		if (args.empty()) return std::string();
		const std::string& fmt = args[0].asString();
		std::string out;
		std::size_t ai = 1;
		std::size_t i = 0;
		while (i < fmt.size()) {
			char c = fmt[i];
			if (c != '%') {
				if (c == '\\' && i + 1 < fmt.size()) {
					char n = fmt[i + 1];
					switch (n) {
					case 'n': out.push_back('\n'); break;
					case 't': out.push_back('\t'); break;
					case 'r': out.push_back('\r'); break;
					case '\\': out.push_back('\\'); break;
					case '"': out.push_back('"'); break;
					default: out.push_back('\\'); out.push_back(n); break;
					}
					i += 2;
					continue;
				}
				out.push_back(c); ++i; continue;
			}
			// %... directive
			std::size_t s = i;
			++i;
			while (i < fmt.size()
			    && std::strchr("-+0 #", fmt[i])) ++i;
			while (i < fmt.size() && std::isdigit((unsigned char)fmt[i])) ++i;
			if (i < fmt.size() && fmt[i] == '.') {
				++i;
				while (i < fmt.size() && std::isdigit((unsigned char)fmt[i])) ++i;
			}
			if (i >= fmt.size()) { out.append(fmt, s, std::string::npos); break; }
			char conv = fmt[i++];
			std::string spec = fmt.substr(s, i - s);
			char buf[256];
			AwkValue v = (ai < args.size()) ? args[ai++] : AwkValue::str("");
			if (conv == 's') {
				std::snprintf(buf, sizeof(buf), spec.c_str(), v.asString().c_str());
				out.append(buf);
			} else if (conv == 'c') {
				if (v.has_s && !v.asString().empty())
					std::snprintf(buf, sizeof(buf), spec.c_str(), v.asString()[0]);
				else
					std::snprintf(buf, sizeof(buf), spec.c_str(), (int)v.asNumber());
				out.append(buf);
			} else if (conv == 'd' || conv == 'i') {
				std::string mod = spec.substr(0, spec.size() - 1) + "ll" + conv;
				std::snprintf(buf, sizeof(buf), mod.c_str(), (long long)v.asNumber());
				out.append(buf);
			} else if (conv == 'o' || conv == 'x' || conv == 'X' || conv == 'u') {
				std::string mod = spec.substr(0, spec.size() - 1) + "ll" + conv;
				std::snprintf(buf, sizeof(buf), mod.c_str(), (unsigned long long)v.asNumber());
				out.append(buf);
			} else if (conv == 'f' || conv == 'e' || conv == 'E'
			    || conv == 'g' || conv == 'G')
			{
				std::snprintf(buf, sizeof(buf), spec.c_str(), v.asNumber());
				out.append(buf);
			} else if (conv == '%') {
				out.push_back('%');
			} else {
				out.append(spec);
			}
		}
		return out;
	}

	// Run statement; throws Break/Continue/Next/Exit for control.
	void run(Stmt& s) {
		if (exiting) return;
		switch (s.kind) {
		case SK::Empty: return;
		case SK::Block:
			for (auto& c : s.children) { run(*c); if (exiting) return; }
			return;
		case SK::If: {
			AwkValue cond = eval(*s.exprs[0]);
			if (cond.truthy()) {
				if (!s.children.empty()) run(*s.children[0]);
			} else if (s.children.size() >= 2) {
				run(*s.children[1]);
			}
			return;
		}
		case SK::While: {
			while (eval(*s.exprs[0]).truthy()) {
				try { run(*s.children[0]); }
				catch (AwkBreakEx&) { return; }
				catch (AwkContinueEx&) { continue; }
			}
			return;
		}
		case SK::DoWhile: {
			do {
				try { run(*s.children[0]); }
				catch (AwkBreakEx&) { return; }
				catch (AwkContinueEx&) {}
			} while (eval(*s.exprs[0]).truthy());
			return;
		}
		case SK::For: {
			if (s.init) eval(*s.init);
			while (!s.cond || eval(*s.cond).truthy()) {
				try { run(*s.children[0]); }
				catch (AwkBreakEx&) { return; }
				catch (AwkContinueEx&) {}
				if (s.step) eval(*s.step);
			}
			return;
		}
		case SK::ForIn: {
			auto& m = arrays[s.name2];
			std::vector<std::string> keys;
			for (auto& kv : m) keys.push_back(kv.first);
			for (const auto& k : keys) {
				vars[s.name1] = AwkValue::str(k);
				try { run(*s.children[0]); }
				catch (AwkBreakEx&) { return; }
				catch (AwkContinueEx&) { continue; }
			}
			return;
		}
		case SK::Break:    throw AwkBreakEx{};
		case SK::Continue: throw AwkContinueEx{};
		case SK::Next:     throw AwkNextEx{};
		case SK::Exit:
			if (!s.exprs.empty()) exit_status = (int)eval(*s.exprs[0]).asNumber();
			exiting = true;
			throw AwkExitEx{};
		case SK::Delete: {
			Expr& tgt = *s.exprs[0];
			if (tgt.kind == EK::Var) { arrays.erase(tgt.name); return; }
			if (tgt.kind == EK::ArrayRef) {
				auto& m = arrays[tgt.name];
				m.erase(buildSubscript(tgt.args));
				return;
			}
			return;
		}
		case SK::Return: return;     // no user functions yet
		case SK::Print: {
			std::string out;
			if (s.exprs.empty()) {
				out = record;
			} else {
				for (std::size_t i = 0; i < s.exprs.size(); ++i) {
					if (i) out += OFS;
					out += eval(*s.exprs[i]).asString();
				}
			}
			out += ORS;
			emit(out, s);
			return;
		}
		case SK::Printf: {
			std::vector<AwkValue> a;
			for (auto& x : s.exprs) a.push_back(eval(*x));
			std::string out = formatPrintf(a);
			emit(out, s);
			return;
		}
		case SK::ExprStmt:
			eval(*s.exprs[0]);
			return;
		}
	}

	void emit(const std::string& out, Stmt& s) {
		FILE* dest = stdout;
		if (s.redir_kind == 1 || s.redir_kind == 2) {
			std::string fn = eval(*s.to_file).asString();
			auto it = output_files.find(fn);
			if (it == output_files.end()) {
				FILE* f = std::fopen(fn.c_str(), s.redir_kind == 1 ? "wb" : "ab");
				if (!f) return;
				output_files[fn] = f;
				dest = f;
			} else dest = it->second;
		} else if (s.redir_kind == 3) {
			std::string cmd = eval(*s.to_file).asString();
#ifdef _WIN32
			FILE* f = _popen(cmd.c_str(), "w");
#else
			FILE* f = popen(cmd.c_str(), "w");
#endif
			if (!f) return;
			auto& slot = output_files[std::string("|") + cmd];
			if (slot) {
#ifdef _WIN32
				_pclose(slot);
#else
				pclose(slot);
#endif
			}
			slot = f;
			dest = f;
		}
		std::fwrite(out.data(), 1, out.size(), dest);
	}

	AwkValue doGetline(Expr& e) {
		FILE* f = stdin;
		bool from_file = (bool)e.b;
		std::string fn;
		if (from_file) {
			fn = eval(*e.b).asString();
			auto it = input_files.find(fn);
			if (it == input_files.end()) {
				FILE* g = std::fopen(fn.c_str(), "rb");
				if (!g) return AwkValue::num(-1);
				input_files[fn] = g;
				f = g;
			} else f = it->second;
		}
		std::string line;
		int c;
		bool any = false;
		while ((c = std::fgetc(f)) != EOF) {
			any = true;
			if (c == '\n') break;
			line.push_back((char)c);
		}
		if (!any) return AwkValue::num(0);
		if (e.name.empty() && !from_file) {
			record = line;
			splitRecord();
			++NR; ++FNR;
		} else if (e.name.empty()) {
			record = line;
			splitRecord();
			++NR; ++FNR;
		} else {
			vars[e.name] = AwkValue::str(line);
			if (!from_file) { ++NR; ++FNR; }
		}
		return AwkValue::num(1);
	}

	bool evalAwkPatternExpr(Expr& e) {
		// A bare /regex/ pattern means `$0 ~ regex`.
		if (e.kind == EK::Regex) {
			try {
				std::regex re(e.str_val);
				return std::regex_search(record, re);
			} catch (...) { return false; }
		}
		return eval(e).truthy();
	}

	void runAwkRule(AwkRule& r) {
		bool match = false;
		switch (r.pat.kind) {
		case AwkPattern::Always: match = true; break;
		case AwkPattern::Expr:   match = evalAwkPatternExpr(*r.pat.e1); break;
		case AwkPattern::Range:
			if (!r.in_range) {
				if (evalAwkPatternExpr(*r.pat.e1)) { r.in_range = true; match = true; }
			} else {
				match = true;
				if (evalAwkPatternExpr(*r.pat.e2)) r.in_range = false;
			}
			break;
		default: return;   // BEGIN/END handled separately
		}
		if (match) try { run(*r.action); }
		catch (AwkNextEx&) { throw; }
		catch (AwkExitEx&) { throw; }
	}

	void runBegins() {
		for (auto& r : prog.rules) {
			if (r.pat.kind != AwkPattern::Begin) continue;
			try { run(*r.action); }
			catch (AwkExitEx&) { return; }
			catch (...) {}
		}
	}
	void runEnds() {
		for (auto& r : prog.rules) {
			if (r.pat.kind != AwkPattern::End) continue;
			try { run(*r.action); }
			catch (AwkExitEx&) { return; }
			catch (...) {}
		}
	}
	void processStream(FILE* f, const std::string& fname) {
		FILENAME = fname;
		FNR = 0;
		std::string line;
		int c;
		while (!exiting) {
			line.clear();
			bool any = false;
			while ((c = std::fgetc(f)) != EOF) {
				any = true;
				if (c == '\n') break;
				line.push_back((char)c);
			}
			if (!any) break;
			record = std::move(line);
			splitRecord();
			++NR; ++FNR;
			try {
				for (auto& r : prog.rules) {
					if (r.pat.kind == AwkPattern::Begin || r.pat.kind == AwkPattern::End) continue;
					runAwkRule(r);
				}
			} catch (AwkNextEx&) { continue; }
			catch (AwkExitEx&)   { return; }
		}
	}
};

int builtin_awk(Executor& exec, const std::vector<std::string>& args) {
	std::string program_text;
	std::string field_sep;
	std::vector<std::string> files;
	std::vector<std::pair<std::string, std::string>> var_assigns;
	bool have_program = false;
	for (std::size_t i = 0; i < args.size(); ++i) {
		const std::string& a = args[i];
		if (a == "-F" && i + 1 < args.size()) { field_sep = args[++i]; continue; }
		if (a.size() > 2 && a.compare(0, 2, "-F") == 0) { field_sep = a.substr(2); continue; }
		if (a == "-f" && i + 1 < args.size()) {
			std::string p = exec.pathConv().toWin32(args[++i]);
			std::ifstream f(p, std::ios::binary);
			if (!f) {
				std::fprintf(stderr, "awk: cannot open program file: %s\n", args[i].c_str());
				return 2;
			}
			std::ostringstream ss; ss << f.rdbuf();
			program_text = ss.str();
			have_program = true;
			continue;
		}
		if (a == "-v" && i + 1 < args.size()) {
			std::string kv = args[++i];
			auto eq = kv.find('=');
			if (eq != std::string::npos) {
				var_assigns.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
			}
			continue;
		}
		if (a == "--") {
			++i;
			if (!have_program && i < args.size()) {
				program_text = args[i++];
				have_program = true;
			}
			while (i < args.size()) files.push_back(args[i++]);
			break;
		}
		if (!have_program) {
			program_text = a;
			have_program = true;
			continue;
		}
		files.push_back(a);
	}
	if (!have_program) {
		std::fprintf(stderr, "awk: missing program text\n");
		return 2;
	}
	AwkProgram prog;
	try {
		AwkParser parser(program_text);
		prog = parser.parseAwkProgram();
	} catch (const std::exception& ex) {
		std::fprintf(stderr, "%s\n", ex.what());
		return 2;
	}
	AwkInterp interp(prog);
	if (!field_sep.empty()) interp.FS = field_sep;
	for (auto& kv : var_assigns) interp.vars[kv.first] = AwkValue::str(kv.second);
	try {
		interp.runBegins();
	} catch (AwkInterp::AwkExitEx&) {
		interp.runEnds();
		return interp.exit_status;
	}
	try {
		if (files.empty()) {
			interp.processStream(stdin, "");
		} else {
			for (const auto& fn : files) {
				FILE* f = std::fopen(exec.pathConv().toWin32(fn).c_str(), "rb");
				if (!f) {
					std::fprintf(stderr, "awk: cannot open: %s\n", fn.c_str());
					continue;
				}
				interp.processStream(f, fn);
				std::fclose(f);
			}
		}
	} catch (AwkInterp::AwkExitEx&) {}
	try { interp.runEnds(); }
	catch (AwkInterp::AwkExitEx&) {}
	return interp.exit_status;
}

}  // namespace wbsh

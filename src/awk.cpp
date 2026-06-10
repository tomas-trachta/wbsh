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
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "arena.h"
#include "executor.h"
#include "numparse.h"
#include "pathconv.h"
#include "regexutil.h"

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
			double d = 0.0;
			parseDouble(s, d);   // non-numeric strings stay 0.0, like awk
			return d;
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
				double d = 0.0;
				if (parseDouble(s, d)) return d != 0.0;
				return true;   // non-numeric, non-empty: truthy
			}
			return false;
		}
		return n != 0.0;
	}
};

// ----- AST ------------------------------------------------------------------

// All Expr/Stmt nodes live in the owning AwkProgram's arena; node-to-node
// pointers are borrows, so the tree needs no per-node ownership machinery.
struct Expr;
struct Stmt;

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

struct Expr {
	EK kind;
	double num_val = 0;
	std::string str_val;
	std::string name;        // var or builtin name
	std::string op;
	Expr* a = nullptr;       // operands
	Expr* b = nullptr;
	Expr* c = nullptr;
	std::vector<Expr*> args;
	Stmt* body = nullptr;    // unused
	// Compiled regex (built lazily).
	mutable std::shared_ptr<std::regex> compiled;
};

enum class SK {
	Empty, Print, Printf, ExprStmt, Block, If, While, DoWhile, For, ForIn,
	Break, Continue, Next, Exit, Delete, Return,
};

struct Stmt {
	SK kind;
	std::vector<Stmt*> children;
	std::vector<Expr*> exprs;            // print arglist, printf arglist, if/while cond
	std::string name1, name2;            // for-in: var, array
	Expr* init = nullptr;                // for(;;)
	Expr* cond = nullptr;
	Expr* step = nullptr;
	Expr* to_file = nullptr;             // > "file" / >> "file" / | "cmd"
	int redir_kind = 0;                  // 0 none, 1 '>', 2 '>>', 3 '|'
};

struct AwkPattern {
	enum Kind { Begin, End, Always, Expr, Range };
	Kind kind = Always;
	// `struct` keyword disambiguates from the Expr enumerator above.
	struct Expr* e1 = nullptr;
	struct Expr* e2 = nullptr;
};

struct AwkRule {
	AwkPattern pat;
	Stmt* action = nullptr;
	bool in_range = false;   // runtime state for Range patterns
};

struct AwkProgram {
	std::vector<AwkRule> rules;
	/// Owns every Expr/Stmt node reachable from `rules`; the raw node
	/// pointers are borrows, which makes AwkProgram move-only.
	Arena arena;
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
		parseDouble(t.text, t.num);   // out-of-range literals stay 0
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
	// Read a single-char punctuation/operator token: \n, ;, , ( ) { } [ ] ? : $ ~.
	// Sets prev_was_value appropriately and advances past the char.
	AwkTok readSimplePunct(char c) {
		AwkTok t;
		t.line = line;
		advance();
		switch (c) {
		case '\n': t.kind = TK::NEWLINE;  prev_was_value = false; return t;
		case ';':  t.kind = TK::SEMI;     prev_was_value = false; return t;
		case ',':  t.kind = TK::COMMA;    prev_was_value = false; return t;
		case '(':  t.kind = TK::LPAREN;   prev_was_value = false; return t;
		case ')':  t.kind = TK::RPAREN;   prev_was_value = true;  return t;
		case '{':  t.kind = TK::LBRACE;   prev_was_value = false; return t;
		case '}':  t.kind = TK::RBRACE;   prev_was_value = false; return t;
		case '[':  t.kind = TK::LBRACK;   prev_was_value = false; return t;
		case ']':  t.kind = TK::RBRACK;   prev_was_value = true;  return t;
		case '?':  t.kind = TK::QUESTION; prev_was_value = false; return t;
		case ':':  t.kind = TK::COLON;    prev_was_value = false; return t;
		case '$':  t.kind = TK::DOLLAR;   prev_was_value = false; return t;
		case '~':  t.kind = TK::MATCH;    prev_was_value = false; return t;
		default:   t.kind = TK::END;      return t;   // unreachable: caller filters
		}
	}

	// '/' is either a regex literal (when no value-producing token preceded it)
	// or division. Disambiguated by `prev_was_value`. Handles `/=` compound form.
	AwkTok readSlashOp() {
		if (!prev_was_value) {
			AwkTok t = readRegex();
			prev_was_value = false;
			return t;
		}
		AwkTok t; t.line = line;
		advance();
		if (i < src.size() && src[i] == '=') {
			advance(); t.kind = TK::SLASH_ASSIGN; prev_was_value = false; return t;
		}
		t.kind = TK::SLASH; prev_was_value = false; return t;
	}

	// Arithmetic operators: +, -, *, %, ^. Handles compound-assign (`+=`, …) and
	// the `++` / `--` doubled forms on + / -.
	AwkTok readArithOp(char c) {
		AwkTok t; t.line = line;
		advance();
		auto compound = [&](TK simple, TK compound_kind) {
			if (i < src.size() && src[i] == '=') {
				advance(); t.kind = compound_kind;
			} else {
				t.kind = simple;
			}
			prev_was_value = false;
		};
		switch (c) {
		case '+':
			if (i < src.size() && src[i] == '+') {
				advance(); t.kind = TK::INC; prev_was_value = true; return t;
			}
			compound(TK::PLUS, TK::PLUS_ASSIGN); return t;
		case '-':
			if (i < src.size() && src[i] == '-') {
				advance(); t.kind = TK::DEC; prev_was_value = true; return t;
			}
			compound(TK::MINUS, TK::MINUS_ASSIGN); return t;
		case '*': compound(TK::STAR,    TK::STAR_ASSIGN);    return t;
		case '%': compound(TK::PERCENT, TK::PERCENT_ASSIGN); return t;
		case '^': compound(TK::CARET,   TK::CARET_ASSIGN);   return t;
		default:  t.kind = TK::END; return t;   // unreachable
		}
	}

	// Comparison / assignment operators: =, !, <, >. Handles doubled forms
	// (`==`, `>>`, `!~`) and compound-assign forms (`<=`, `>=`, `!=`).
	AwkTok readCompareOp(char c) {
		AwkTok t; t.line = line;
		advance();
		switch (c) {
		case '=':
			if (i < src.size() && src[i] == '=') {
				advance(); t.kind = TK::EQ; prev_was_value = false; return t;
			}
			t.kind = TK::ASSIGN; prev_was_value = false; return t;
		case '!':
			if (i < src.size() && src[i] == '=') {
				advance(); t.kind = TK::NE; prev_was_value = false; return t;
			}
			if (i < src.size() && src[i] == '~') {
				advance(); t.kind = TK::NMATCH; prev_was_value = false; return t;
			}
			t.kind = TK::NOT; prev_was_value = false; return t;
		case '<':
			if (i < src.size() && src[i] == '=') {
				advance(); t.kind = TK::LE; prev_was_value = false; return t;
			}
			t.kind = TK::LT; prev_was_value = false; return t;
		case '>':
			if (i < src.size() && src[i] == '>') {
				advance(); t.kind = TK::APPEND; prev_was_value = false; return t;
			}
			if (i < src.size() && src[i] == '=') {
				advance(); t.kind = TK::GE; prev_was_value = false; return t;
			}
			t.kind = TK::GT; prev_was_value = false; return t;
		default: t.kind = TK::END; return t;   // unreachable
		}
	}

	// Logical operators: && and ||. A bare `&` is unsupported and yields END;
	// a bare `|` becomes a literal PIPE token (used for `getline | cmd`).
	AwkTok readLogicalOp(char c) {
		AwkTok t; t.line = line;
		advance();
		if (c == '&') {
			if (i < src.size() && src[i] == '&') {
				advance(); t.kind = TK::AND; prev_was_value = false; return t;
			}
			t.kind = TK::END; return t;   // unsupported bare &
		}
		// c == '|'
		if (i < src.size() && src[i] == '|') {
			advance(); t.kind = TK::OR; prev_was_value = false; return t;
		}
		t.kind = TK::PIPE; prev_was_value = false; return t;
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

		// Range predicates (numbers, identifiers) cannot be expressed as
		// switch cases; handle them first.
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

		switch (c) {
		case '\n': case ';': case ',': case '(': case ')':
		case '{':  case '}': case '[': case ']':
		case '?':  case ':': case '$': case '~':
			return readSimplePunct(c);
		case '"':
			t = readString(); prev_was_value = true; return t;
		case '/':
			return readSlashOp();
		case '+': case '-': case '*': case '%': case '^':
			return readArithOp(c);
		case '=': case '!': case '<': case '>':
			return readCompareOp(c);
		case '&': case '|':
			return readLogicalOp(c);
		default:
			advance();   // unknown char: skip
			return next();
		}
	}
	AwkTok& peek() {
		if (pushback.empty()) {
			AwkTok t = next();
			pushback.push_back(std::move(t));
		}
		return pushback.back();
	}
	// Pin the lexer at end-of-input: peek()/next() yield END from now on.
	// The parser uses this to unwind cleanly after recording an error.
	void abortToEof() {
		pushback.clear();
		i = src.size();
	}
};

// ----- AwkParser -----------------------------------------------------------

struct AwkParser {
	AwkLex lex;
	std::string error_msg;
	// Arena of the AwkProgram being built by parseAwkProgram(); set there so
	// every node the parse helpers allocate ends up owned by that program.
	Arena* arena_ = nullptr;

	explicit AwkParser(const std::string& s) : lex(s) {}

	bool failed() const { return !error_msg.empty(); }

	// Record a parse error (the first one wins; later ones are cascade
	// noise) and pin the lexer at END so every parsing loop terminates.
	void err(const std::string& m) {
		if (error_msg.empty()) {
			error_msg = "awk: parse error: " + m
			    + " (line " + std::to_string(lex.peek().line) + ")";
		}
		lex.abortToEof();
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
		arena_ = &p.arena;
		skipTerm();
		while (lex.peek().kind != TK::END) {
			AwkRule r;
			parseAwkPattern(r.pat);
			// Don't record rules half-built from a failed parse.
			if (failed()) break;
			if (lex.peek().kind == TK::LBRACE) {
				r.action = parseBlock();
			} else {
				// Default action is `{ print }`.
				auto blk = arena_->make<Stmt>();
				blk->kind = SK::Block;
				auto pr = arena_->make<Stmt>();
				pr->kind = SK::Print;
				blk->children.push_back(pr);
				r.action = blk;
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

	Stmt* parseBlock() {
		if (lex.peek().kind != TK::LBRACE) err("expected '{'");
		lex.next();
		auto blk = arena_->make<Stmt>();
		blk->kind = SK::Block;
		skipTerm();
		while (lex.peek().kind != TK::RBRACE && lex.peek().kind != TK::END) {
			blk->children.push_back(parseStmt());
			skipTerm();
		}
		if (lex.peek().kind == TK::RBRACE) lex.next();
		return blk;
	}

	// `print` / `printf` statement body, with optional argument list and
	// optional `> file` / `>> file` / `| cmd` redirection. Caller has not yet
	// consumed the K_PRINT / K_PRINTF token.
	Stmt* parsePrintStmt(TK k) {
		lex.next();
		auto s = arena_->make<Stmt>();
		s->kind = (k == TK::K_PRINT) ? SK::Print : SK::Printf;
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
		if (lex.peek().kind == TK::GT) {
			lex.next(); s->redir_kind = 1; s->to_file = parseExpr();
		} else if (lex.peek().kind == TK::APPEND) {
			lex.next(); s->redir_kind = 2; s->to_file = parseExpr();
		} else if (lex.peek().kind == TK::PIPE) {
			lex.next(); s->redir_kind = 3; s->to_file = parseExpr();
		}
		return s;
	}

	Stmt* parseStmt() {
		TK k = lex.peek().kind;
		if (k == TK::LBRACE) return parseBlock();
		if (k == TK::K_IF) return parseIf();
		if (k == TK::K_WHILE) return parseWhile();
		if (k == TK::K_DO) return parseDoWhile();
		if (k == TK::K_FOR) return parseFor();
		if (k == TK::K_BREAK) {
			lex.next();
			auto s = arena_->make<Stmt>(); s->kind = SK::Break; return s;
		}
		if (k == TK::K_CONTINUE) {
			lex.next();
			auto s = arena_->make<Stmt>(); s->kind = SK::Continue; return s;
		}
		if (k == TK::K_NEXT) {
			lex.next();
			auto s = arena_->make<Stmt>(); s->kind = SK::Next; return s;
		}
		if (k == TK::K_EXIT) {
			lex.next();
			auto s = arena_->make<Stmt>(); s->kind = SK::Exit;
			if (lex.peek().kind != TK::SEMI && lex.peek().kind != TK::NEWLINE
			    && lex.peek().kind != TK::RBRACE && lex.peek().kind != TK::END) {
				s->exprs.push_back(parseExpr());
			}
			return s;
		}
		if (k == TK::K_DELETE) {
			lex.next();
			auto s = arena_->make<Stmt>(); s->kind = SK::Delete;
			s->exprs.push_back(parseUnary());
			return s;
		}
		if (k == TK::K_PRINT || k == TK::K_PRINTF) return parsePrintStmt(k);

		auto s = arena_->make<Stmt>();
		s->kind = SK::ExprStmt;
		s->exprs.push_back(parseExpr());
		return s;
	}

	Stmt* parseIf() {
		lex.next();   // 'if'
		if (lex.peek().kind != TK::LPAREN) err("expected '(' after if");
		lex.next();
		auto cond = parseExpr();
		if (lex.peek().kind != TK::RPAREN) err("expected ')'");
		lex.next();
		skipTerm();
		auto then_s = parseStmt();
		Stmt* else_s = nullptr;
		skipTerm();
		if (lex.peek().kind == TK::K_ELSE) {
			lex.next(); skipTerm();
			else_s = parseStmt();
		}
		auto s = arena_->make<Stmt>();
		s->kind = SK::If;
		s->exprs.push_back(cond);
		s->children.push_back(then_s);
		if (else_s) s->children.push_back(else_s);
		return s;
	}

	Stmt* parseWhile() {
		lex.next();
		if (lex.peek().kind != TK::LPAREN) err("expected '('");
		lex.next();
		auto cond = parseExpr();
		if (lex.peek().kind != TK::RPAREN) err("expected ')'");
		lex.next();
		skipTerm();
		auto body = parseStmt();
		auto s = arena_->make<Stmt>();
		s->kind = SK::While;
		s->exprs.push_back(cond);
		s->children.push_back(body);
		return s;
	}

	Stmt* parseDoWhile() {
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
		auto s = arena_->make<Stmt>();
		s->kind = SK::DoWhile;
		s->exprs.push_back(cond);
		s->children.push_back(body);
		return s;
	}

	Stmt* parseFor() {
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
				auto s = arena_->make<Stmt>();
				s->kind = SK::ForIn;
				s->name1 = save_id.text;
				s->name2 = arr_name.text;
				s->children.push_back(body);
				return s;
			}
			// Not for-in; push the ID back onto the lookahead stack.
			lex.unread(std::move(save_id));
		}
		// for (init ; cond ; step) body
		Expr* init = nullptr;
		Expr* cond = nullptr;
		Expr* step = nullptr;
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
		auto s = arena_->make<Stmt>();
		s->kind = SK::For;
		s->init = init;
		s->cond = cond;
		s->step = step;
		s->children.push_back(body);
		return s;
	}

	// ---- Expressions ----
	Expr* parseExpr() { return parseTernary(); }

	Expr* parseTernary() {
		auto a = parseLogicOr();
		if (lex.peek().kind == TK::QUESTION) {
			lex.next();
			auto b = parseTernary();
			if (lex.peek().kind != TK::COLON) err("expected ':'");
			lex.next();
			auto c = parseTernary();
			auto e = arena_->make<Expr>();
			e->kind = EK::Ternary;
			e->a = a;
			e->b = b;
			e->c = c;
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
			auto e = arena_->make<Expr>();
			if (a->kind == EK::Field) {
				e->kind = EK::FieldAssign;
				e->op = op;
				e->a = a->a;
				e->b = rhs;
			} else {
				e->kind = EK::Assign;
				e->op = op;
				e->a = a;
				e->b = rhs;
			}
			return e;
		}
		return a;
	}

	Expr* parseLogicOr() {
		auto a = parseLogicAnd();
		while (lex.peek().kind == TK::OR) {
			lex.next();
			auto b = parseLogicAnd();
			auto e = arena_->make<Expr>();
			e->kind = EK::Binary; e->op = "||";
			e->a = a; e->b = b;
			a = e;
		}
		return a;
	}
	Expr* parseLogicAnd() {
		auto a = parseInTest();
		while (lex.peek().kind == TK::AND) {
			lex.next();
			auto b = parseInTest();
			auto e = arena_->make<Expr>();
			e->kind = EK::Binary; e->op = "&&";
			e->a = a; e->b = b;
			a = e;
		}
		return a;
	}
	Expr* parseInTest() {
		auto a = parseMatch();
		if (lex.peek().kind == TK::K_IN) {
			lex.next();
			if (lex.peek().kind != TK::ID) err("expected array name after 'in'");
			AwkTok n = lex.next();
			auto e = arena_->make<Expr>();
			e->kind = EK::ArrayInTest;
			e->a = a;
			e->name = n.text;
			return e;
		}
		return a;
	}
	Expr* parseMatch() {
		auto a = parseRel();
		while (lex.peek().kind == TK::MATCH || lex.peek().kind == TK::NMATCH) {
			std::string op = lex.peek().kind == TK::MATCH ? "~" : "!~";
			lex.next();
			auto b = parseRel();
			auto e = arena_->make<Expr>();
			e->kind = EK::Binary; e->op = op;
			e->a = a; e->b = b;
			a = e;
		}
		return a;
	}
	Expr* parseRel() {
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
			auto e = arena_->make<Expr>();
			e->kind = EK::Binary; e->op = op;
			e->a = a; e->b = b;
			a = e;
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
	Expr* parseConcat() {
		auto a = parseAdd();
		while (startsConcat(lex.peek().kind)) {
			auto b = parseAdd();
			auto e = arena_->make<Expr>();
			e->kind = EK::Binary; e->op = " ";   // string concat
			e->a = a; e->b = b;
			a = e;
		}
		return a;
	}
	Expr* parseAdd() {
		auto a = parseMul();
		while (lex.peek().kind == TK::PLUS || lex.peek().kind == TK::MINUS) {
			std::string op = lex.peek().kind == TK::PLUS ? "+" : "-";
			lex.next();
			auto b = parseMul();
			auto e = arena_->make<Expr>();
			e->kind = EK::Binary; e->op = op;
			e->a = a; e->b = b;
			a = e;
		}
		return a;
	}
	Expr* parseMul() {
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
			auto e = arena_->make<Expr>();
			e->kind = EK::Binary; e->op = op;
			e->a = a; e->b = b;
			a = e;
		}
	}
	Expr* parseExp() {
		auto a = parseUnary();
		if (lex.peek().kind == TK::CARET) {
			lex.next();
			auto b = parseExp();   // right-associative
			auto e = arena_->make<Expr>();
			e->kind = EK::Binary; e->op = "^";
			e->a = a; e->b = b;
			a = e;
		}
		return a;
	}
	Expr* parseUnary() {
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
			auto e = arena_->make<Expr>();
			e->kind = EK::Unary; e->op = op;
			e->a = inner;
			return e;
		}
		return parsePostfix();
	}
	Expr* parsePostfix() {
		auto a = parsePrimary();
		while (true) {
			TK k = lex.peek().kind;
			if (k == TK::INC || k == TK::DEC) {
				std::string op = (k == TK::INC) ? "++" : "--";
				lex.next();
				auto e = arena_->make<Expr>();
				e->kind = EK::PostIncDec; e->op = op;
				e->a = a;
				a = e;
				continue;
			}
			break;
		}
		return a;
	}
	// Parse a `getline [var] [< file]` expression. Caller already consumed K_GETLINE.
	Expr* parsePrimaryGetline() {
		auto e = arena_->make<Expr>();
		e->kind = EK::Getline;
		if (lex.peek().kind == TK::ID) {
			e->name = lex.next().text;
		}
		if (lex.peek().kind == TK::LT) {
			lex.next();
			e->b = parseUnary();
		}
		return e;
	}

	// Parse the suffix after an identifier in primary position: function call,
	// array reference, or bare variable.
	Expr* parsePrimaryIdentSuffix(const std::string& name) {
		if (lex.peek().kind == TK::LPAREN) {
			lex.next();
			auto e = arena_->make<Expr>();
			e->kind = EK::Call;
			e->name = name;
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
		if (lex.peek().kind == TK::LBRACK) {
			lex.next();
			std::vector<Expr*> subs;
			subs.push_back(parseExpr());
			while (lex.peek().kind == TK::COMMA) {
				lex.next();
				subs.push_back(parseExpr());
			}
			if (lex.peek().kind != TK::RBRACK) err("expected ']'");
			lex.next();
			auto e = arena_->make<Expr>();
			e->kind = EK::ArrayRef;
			e->name = name;
			e->args = std::move(subs);
			return e;
		}
		auto e = arena_->make<Expr>();
		e->kind = EK::Var;
		e->name = name;
		return e;
	}

	Expr* parsePrimary() {
		AwkTok t = lex.peek();
		switch (t.kind) {
		case TK::NUM: {
			lex.next();
			auto e = arena_->make<Expr>();
			e->kind = EK::Number; e->num_val = t.num;
			return e;
		}
		case TK::STR: {
			lex.next();
			auto e = arena_->make<Expr>();
			e->kind = EK::String; e->str_val = t.text;
			return e;
		}
		case TK::REGEX: {
			lex.next();
			auto e = arena_->make<Expr>();
			e->kind = EK::Regex; e->str_val = t.text;
			return e;
		}
		case TK::DOLLAR: {
			lex.next();
			auto inner = parseUnary();
			auto e = arena_->make<Expr>();
			e->kind = EK::Field;
			e->a = inner;
			return e;
		}
		case TK::LPAREN: {
			lex.next();
			auto inner = parseExpr();
			if (lex.peek().kind != TK::RPAREN) err("expected ')'");
			lex.next();
			auto e = arena_->make<Expr>();
			e->kind = EK::Group; e->a = inner;
			return e;
		}
		case TK::K_GETLINE:
			lex.next();
			return parsePrimaryGetline();
		case TK::ID:
			lex.next();
			return parsePrimaryIdentSuffix(t.text);
		default:
			err("unexpected token");
			return nullptr;
		}
	}
};

// ----- AwkInterpreter ---------------------------------------------------------

// Split `s` on successive matches of `re`, mirroring what
// sregex_token_iterator(.., -1) used to produce: the text between matches,
// with the tail after the last match emitted only when non-empty, and a
// string containing no match at all yielding itself as one (possibly empty)
// field. Hand-rolled so matcher failures (searchRegex -> false) end the
// split instead of throwing out of it mid-iteration.
static std::vector<std::string> splitByRegex(const std::string& s, const std::regex& re) {
	std::vector<std::string> parts;
	std::size_t pos = 0;
	while (true) {
		const std::string rest = s.substr(pos);
		std::smatch m;
		if (!searchRegex(rest, re, &m)) break;
		const auto off = static_cast<std::size_t>(m.position(0));
		const auto len = static_cast<std::size_t>(m.length(0));
		parts.push_back(rest.substr(0, off));
		pos += off + len;
		// A zero-width separator would re-match in place forever; stop and
		// let the tail below become the final field.
		if (len == 0) break;
	}
	if (pos < s.size() || parts.empty()) parts.push_back(s.substr(pos));
	return parts;
}

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
	// Pending control transfer posted by run() for break/continue/next; the
	// enclosing loop (or the per-record rule pass, for Next) consumes it.
	// `exit` uses the separate `exiting` flag, which is never cleared.
	enum class Flow { None, Break, Continue, Next };
	Flow flow = Flow::None;

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
			// FS is a regex; an invalid pattern leaves the record unsplit.
			std::regex re;
			if (compileRegex(re, FS)) fields = splitByRegex(record, re);
			else fields.push_back(record);
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

	std::string buildSubscript(const std::vector<Expr*>& subs) {
		std::string key;
		for (std::size_t i = 0; i < subs.size(); ++i) {
			if (i) key += SUBSEP;
			key += eval(*subs[i]).asString();
		}
		return key;
	}

	// ---- Expression evaluation ----

	AwkValue evalUnary(Expr& e) {
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

	// Apply one of ==, !=, <, <=, >, >= to two values. Numeric comparison when
	// either operand has been computed numerically; string comparison otherwise.
	AwkValue evalCompareOp(const AwkValue& av, const AwkValue& bv, const std::string& op) {
		bool numeric = av.has_n || bv.has_n;
		bool r;
		if (numeric) {
			double a = av.asNumber(), b = bv.asNumber();
			if      (op == "==") r = a == b;
			else if (op == "!=") r = a != b;
			else if (op == "<")  r = a < b;
			else if (op == "<=") r = a <= b;
			else if (op == ">")  r = a > b;
			else                 r = a >= b;
		} else {
			std::string a = av.asString(), b = bv.asString();
			if      (op == "==") r = a == b;
			else if (op == "!=") r = a != b;
			else if (op == "<")  r = a < b;
			else if (op == "<=") r = a <= b;
			else if (op == ">")  r = a > b;
			else                 r = a >= b;
		}
		return AwkValue::num(r ? 1.0 : 0.0);
	}

	// Regex match: `s ~ pat` (`positive=true`) or `s !~ pat` (`positive=false`).
	// `pat_expr` is the RHS; if it's a Regex literal we use its text directly,
	// otherwise we coerce its evaluation to a string and treat that as a pattern.
	AwkValue evalRegexMatch(const std::string& s, Expr& pat_expr, bool positive) {
		std::string pat;
		if (pat_expr.kind == EK::Regex) pat = pat_expr.str_val;
		else pat = eval(pat_expr).asString();
		std::regex re;
		// An invalid pattern matches nothing.
		bool m = compileRegex(re, pat) && searchRegex(s, re);
		bool r = positive ? m : !m;
		return AwkValue::num(r ? 1.0 : 0.0);
	}

	AwkValue evalBinary(Expr& e) {
		if (e.op == "&&") {
			if (!eval(*e.a).truthy()) return AwkValue::num(0.0);
			return AwkValue::num(eval(*e.b).truthy() ? 1.0 : 0.0);
		}
		if (e.op == "||") {
			if (eval(*e.a).truthy()) return AwkValue::num(1.0);
			return AwkValue::num(eval(*e.b).truthy() ? 1.0 : 0.0);
		}
		if (e.op == "~" || e.op == "!~") {
			return evalRegexMatch(eval(*e.a).asString(), *e.b, e.op == "~");
		}
		if (e.op == " ") {
			return AwkValue::str(eval(*e.a).asString() + eval(*e.b).asString());
		}
		AwkValue av = eval(*e.a);
		AwkValue bv = eval(*e.b);
		if (e.op == "+") return AwkValue::num(av.asNumber() + bv.asNumber());
		if (e.op == "-") return AwkValue::num(av.asNumber() - bv.asNumber());
		if (e.op == "*") return AwkValue::num(av.asNumber() * bv.asNumber());
		if (e.op == "/") {
			double d = bv.asNumber();
			return AwkValue::num(d == 0 ? 0.0 : av.asNumber() / d);
		}
		if (e.op == "%") {
			double d = bv.asNumber();
			return AwkValue::num(d == 0 ? 0.0 : std::fmod(av.asNumber(), d));
		}
		if (e.op == "^") return AwkValue::num(std::pow(av.asNumber(), bv.asNumber()));
		if (e.op == "==" || e.op == "!=" || e.op == "<" || e.op == "<="
		    || e.op == ">" || e.op == ">=")
		{
			return evalCompareOp(av, bv, e.op);
		}
		return AwkValue::str("");
	}

	AwkValue eval(Expr& e) {
		switch (e.kind) {
		case EK::Number: return AwkValue::num(e.num_val);
		case EK::String: return AwkValue::str(e.str_val);
		case EK::Regex:  return AwkValue::str(e.str_val);
		case EK::Var:    return getVar(e.name);
		case EK::Field:  return getField((int)eval(*e.a).asNumber());
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
		case EK::Group:  return eval(*e.a);
		case EK::Unary:  return evalUnary(e);
		case EK::PostIncDec: {
			double cur = eval(*e.a).asNumber();
			double nv = (e.op == "++") ? cur + 1 : cur - 1;
			assignLValue(*e.a, AwkValue::num(nv));
			return AwkValue::num(cur);
		}
		case EK::Binary:  return evalBinary(e);
		case EK::Ternary: return eval(*e.a).truthy() ? eval(*e.b) : eval(*e.c);
		case EK::Assign:  return assignTo(*e.a, e.op, eval(*e.b));
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

	// Split `s` on `sep` for the awk `split()` builtin.
	// `sep == " "` is special-cased to mean "any run of spaces / tabs".
	// A single-character separator is matched literally; longer separators
	// are interpreted as ECMAScript regex.
	static std::vector<std::string> splitOnAwkSeparator(const std::string& s,
	                                                    const std::string& sep) {
		std::vector<std::string> parts;
		if (sep == " ") {
			std::size_t i = 0;
			const std::size_t m = s.size();
			while (i < m) {
				while (i < m && (s[i] == ' ' || s[i] == '\t')) ++i;
				if (i >= m) break;
				const std::size_t st = i;
				while (i < m && s[i] != ' ' && s[i] != '\t') ++i;
				parts.push_back(s.substr(st, i - st));
			}
			return parts;
		}
		if (sep.size() == 1) {
			std::string cur;
			for (char c : s) {
				if (c == sep[0]) { parts.push_back(std::move(cur)); cur.clear(); }
				else cur.push_back(c);
			}
			parts.push_back(std::move(cur));
			return parts;
		}
		std::regex re;
		if (compileRegex(re, sep)) return splitByRegex(s, re);
		// Invalid separator regex: the whole string is one field.
		parts.push_back(s);
		return parts;
	}

	AwkValue callStringBuiltin(Expr& e) {
		const std::string& n = e.name;
		auto sval = [&](std::size_t i) {
			return i < e.args.size() ? eval(*e.args[i]).asString() : std::string();
		};
		auto nval = [&](std::size_t i) {
			return i < e.args.size() ? eval(*e.args[i]).asNumber() : 0.0;
		};
		if (n == "length") {
			if (e.args.empty()) return AwkValue::num(static_cast<double>(record.size()));
			return AwkValue::num(static_cast<double>(sval(0).size()));
		}
		if (n == "substr") {
			const std::string s = sval(0);
			long long start = static_cast<long long>(nval(1));
			if (start < 1) start = 1;
			const long long len = (e.args.size() >= 3)
				? static_cast<long long>(nval(2))
				: static_cast<long long>(s.size()) - start + 1;
			if (start > static_cast<long long>(s.size()) || len <= 0)
				return AwkValue::str("");
			return AwkValue::str(s.substr(static_cast<std::size_t>(start - 1),
				static_cast<std::size_t>(std::min<long long>(
					len, static_cast<long long>(s.size()) - start + 1))));
		}
		if (n == "index") {
			const auto s = sval(0);
			const auto t = sval(1);
			if (t.empty()) return AwkValue::num(0);
			const auto p = s.find(t);
			return AwkValue::num(p == std::string::npos
				? 0 : static_cast<double>(p + 1));
		}
		if (n == "tolower" || n == "toupper") {
			std::string s = sval(0);
			for (auto& c : s) {
				c = static_cast<char>(n == "tolower"
					? std::tolower(static_cast<unsigned char>(c))
					: std::toupper(static_cast<unsigned char>(c)));
			}
			return AwkValue::str(std::move(s));
		}
		return AwkValue::str("");
	}

	AwkValue callSplitBuiltin(Expr& e) {
		auto sval = [&](std::size_t i) {
			return i < e.args.size() ? eval(*e.args[i]).asString() : std::string();
		};
		const std::string s = sval(0);
		const std::string sep = (e.args.size() >= 3) ? sval(2) : FS;

		std::string aname;
		if (e.args.size() >= 2 && (e.args[1]->kind == EK::Var
		                           || e.args[1]->kind == EK::ArrayRef)) {
			aname = e.args[1]->name;
		}
		arrays[aname].clear();

		const auto parts = splitOnAwkSeparator(s, sep);
		for (std::size_t i = 0; i < parts.size(); ++i) {
			arrays[aname][std::to_string(i + 1)] = AwkValue::str(parts[i]);
		}
		return AwkValue::num(static_cast<double>(parts.size()));
	}

	// Replace every match of `re` in `subject` with `rep` ($N references via
	// smatch::format). Returns the replacement count. Manual scan rather than
	// sregex_iterator so a matcher failure just stops the scan.
	static int gsubAll(std::string& subject, const std::regex& re, const std::string& rep) {
		std::string out;
		std::size_t pos = 0;
		int count = 0;
		while (pos <= subject.size()) {
			const std::string rest = subject.substr(pos);
			std::smatch m;
			if (!searchRegex(rest, re, &m)) break;
			const auto off = static_cast<std::size_t>(m.position(0));
			const auto len = static_cast<std::size_t>(m.length(0));
			out.append(rest, 0, off);
			out.append(m.format(rep));
			++count;
			pos += off + len;
			if (len == 0) {
				// Zero-width match: copy one char through so the scan advances.
				if (pos >= subject.size()) break;
				out.push_back(subject[pos]);
				++pos;
			}
		}
		out.append(subject, pos, std::string::npos);
		subject = std::move(out);
		return count;
	}

	AwkValue callSubGsubBuiltin(Expr& e) {
		const bool gsub = e.name == "gsub";
		auto sval = [&](std::size_t i) {
			return i < e.args.size() ? eval(*e.args[i]).asString() : std::string();
		};
		const std::string pat = sval(0);
		const std::string rep = sval(1);
		Expr* target = (e.args.size() >= 3) ? e.args[2] : nullptr;
		std::string subject = target ? eval(*target).asString() : record;

		int count = 0;
		std::regex re;
		// A bad regex leaves the subject untouched.
		if (compileRegex(re, pat)) {
			if (gsub) {
				count = gsubAll(subject, re, rep);
			} else {
				std::smatch m;
				if (searchRegex(subject, re, &m)) {
					subject = m.prefix().str() + m.format(rep) + m.suffix().str();
					count = 1;
				}
			}
		}

		if (target) {
			assignLValue(*target, AwkValue::str(subject));
		} else {
			record = std::move(subject);
			splitRecord();
		}
		return AwkValue::num(static_cast<double>(count));
	}

	AwkValue callMatchBuiltin(Expr& e) {
		auto sval = [&](std::size_t i) {
			return i < e.args.size() ? eval(*e.args[i]).asString() : std::string();
		};
		const std::string s = sval(0);
		const std::string pat = sval(1);
		std::regex re;
		std::smatch m;
		// Bad patterns and matcher failures both count as "no match".
		if (compileRegex(re, pat) && searchRegex(s, re, &m)) {
			vars["RSTART"]  = AwkValue::num(static_cast<double>(m.position(0) + 1));
			vars["RLENGTH"] = AwkValue::num(static_cast<double>(m.length(0)));
			return AwkValue::num(static_cast<double>(m.position(0) + 1));
		}
		vars["RSTART"]  = AwkValue::num(0);
		vars["RLENGTH"] = AwkValue::num(-1);
		return AwkValue::num(0);
	}

	AwkValue callPrintfBuiltin(Expr& e) {
		std::vector<AwkValue> a;
		for (auto& x : e.args) a.push_back(eval(*x));
		std::string out = formatPrintf(a);
		if (e.name == "sprintf") return AwkValue::str(std::move(out));
		std::fputs(out.c_str(), stdout);
		return AwkValue::str("");
	}

	AwkValue callMathBuiltin(Expr& e) {
		const std::string& n = e.name;
		auto nval = [&](std::size_t i) {
			return i < e.args.size() ? eval(*e.args[i]).asNumber() : 0.0;
		};
		if (n == "int")   return AwkValue::num(std::trunc(nval(0)));
		if (n == "sqrt")  return AwkValue::num(std::sqrt(nval(0)));
		if (n == "exp")   return AwkValue::num(std::exp(nval(0)));
		if (n == "log")   return AwkValue::num(std::log(nval(0)));
		if (n == "sin")   return AwkValue::num(std::sin(nval(0)));
		if (n == "cos")   return AwkValue::num(std::cos(nval(0)));
		if (n == "atan2") return AwkValue::num(std::atan2(nval(0), nval(1)));
		if (n == "rand")  return AwkValue::num(static_cast<double>(std::rand()) / RAND_MAX);
		if (n == "srand") {
			std::srand(static_cast<unsigned>(nval(0)));
			return AwkValue::num(0);
		}
		return AwkValue::str("");
	}

	AwkValue callBuiltin(Expr& e) {
		const std::string& n = e.name;
		if (n == "length"  || n == "substr"  || n == "index"
		    || n == "tolower" || n == "toupper")
			return callStringBuiltin(e);
		if (n == "split")                    return callSplitBuiltin(e);
		if (n == "sub" || n == "gsub")       return callSubGsubBuiltin(e);
		if (n == "match")                    return callMatchBuiltin(e);
		if (n == "sprintf" || n == "printf") return callPrintfBuiltin(e);
		if (n == "system") {
			const std::string cmd = e.args.empty()
				? std::string()
				: eval(*e.args[0]).asString();
			return AwkValue::num(static_cast<double>(std::system(cmd.c_str())));
		}
		if (n == "int" || n == "sqrt" || n == "exp" || n == "log"
		    || n == "sin" || n == "cos" || n == "atan2"
		    || n == "rand" || n == "srand")
			return callMathBuiltin(e);
		// Unknown — return empty.
		return AwkValue::str("");
	}

	// Append the result of a `\X` escape inside a printf format string.
	static void formatPrintfEscape(std::string& out, char n) {
		switch (n) {
		case 'n':  out.push_back('\n'); return;
		case 't':  out.push_back('\t'); return;
		case 'r':  out.push_back('\r'); return;
		case '\\': out.push_back('\\'); return;
		case '"':  out.push_back('"');  return;
		default:   out.push_back('\\'); out.push_back(n); return;
		}
	}

	// Apply one parsed %-directive: `spec` is the full `%...` spec including
	// the conversion char, `conv` is that final char, and `v` is the argument
	// to format. Returns the rendered text (possibly empty).
	static std::string formatPrintfDirective(const std::string& spec, char conv,
	                                         const AwkValue& v) {
		char buf[256];
		if (conv == 's') {
			std::snprintf(buf, sizeof(buf), spec.c_str(), v.asString().c_str());
			return buf;
		}
		if (conv == 'c') {
			if (v.has_s && !v.asString().empty())
				std::snprintf(buf, sizeof(buf), spec.c_str(), v.asString()[0]);
			else
				std::snprintf(buf, sizeof(buf), spec.c_str(), (int)v.asNumber());
			return buf;
		}
		if (conv == 'd' || conv == 'i') {
			std::string mod = spec.substr(0, spec.size() - 1) + "ll" + conv;
			std::snprintf(buf, sizeof(buf), mod.c_str(), (long long)v.asNumber());
			return buf;
		}
		if (conv == 'o' || conv == 'x' || conv == 'X' || conv == 'u') {
			std::string mod = spec.substr(0, spec.size() - 1) + "ll" + conv;
			std::snprintf(buf, sizeof(buf), mod.c_str(), (unsigned long long)v.asNumber());
			return buf;
		}
		if (conv == 'f' || conv == 'e' || conv == 'E' || conv == 'g' || conv == 'G') {
			std::snprintf(buf, sizeof(buf), spec.c_str(), v.asNumber());
			return buf;
		}
		if (conv == '%') return "%";
		return spec;
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
					formatPrintfEscape(out, fmt[i + 1]);
					i += 2;
					continue;
				}
				out.push_back(c); ++i; continue;
			}
			// %... directive: scan flags, width, precision, then conversion.
			std::size_t s = i;
			++i;
			while (i < fmt.size() && std::strchr("-+0 #", fmt[i])) ++i;
			while (i < fmt.size() && std::isdigit((unsigned char)fmt[i])) ++i;
			if (i < fmt.size() && fmt[i] == '.') {
				++i;
				while (i < fmt.size() && std::isdigit((unsigned char)fmt[i])) ++i;
			}
			if (i >= fmt.size()) { out.append(fmt, s, std::string::npos); break; }
			char conv = fmt[i++];
			std::string spec = fmt.substr(s, i - s);
			AwkValue v = (ai < args.size()) ? args[ai++] : AwkValue::str("");
			out.append(formatPrintfDirective(spec, conv, v));
		}
		return out;
	}

	void runIf(Stmt& s) {
		AwkValue cond = eval(*s.exprs[0]);
		if (cond.truthy()) {
			if (!s.children.empty()) run(*s.children[0]);
		} else if (s.children.size() >= 2) {
			run(*s.children[1]);
		}
	}

	void runWhile(Stmt& s) {
		while (eval(*s.exprs[0]).truthy()) {
			run(*s.children[0]);
			if (flow == Flow::Break) { flow = Flow::None; return; }
			if (flow == Flow::Continue) { flow = Flow::None; continue; }
			if (flow == Flow::Next || exiting) return;   // leave Next pending
		}
	}

	void runDoWhile(Stmt& s) {
		do {
			run(*s.children[0]);
			if (flow == Flow::Break) { flow = Flow::None; return; }
			if (flow == Flow::Continue) flow = Flow::None;   // recheck condition
			if (flow == Flow::Next || exiting) return;       // leave Next pending
		} while (eval(*s.exprs[0]).truthy());
	}

	void runFor(Stmt& s) {
		if (s.init) eval(*s.init);
		while (!s.cond || eval(*s.cond).truthy()) {
			run(*s.children[0]);
			if (flow == Flow::Break) { flow = Flow::None; return; }
			if (flow == Flow::Continue) flow = Flow::None;   // still run the step
			if (flow == Flow::Next || exiting) return;       // leave Next pending
			if (s.step) eval(*s.step);
		}
	}

	void runForIn(Stmt& s) {
		// Snapshot keys so the body can mutate the array safely.
		auto& m = arrays[s.name2];
		std::vector<std::string> keys;
		for (auto& kv : m) keys.push_back(kv.first);
		for (const auto& k : keys) {
			vars[s.name1] = AwkValue::str(k);
			run(*s.children[0]);
			if (flow == Flow::Break) { flow = Flow::None; return; }
			if (flow == Flow::Continue) { flow = Flow::None; continue; }
			if (flow == Flow::Next || exiting) return;   // leave Next pending
		}
	}

	void runDelete(Stmt& s) {
		Expr& tgt = *s.exprs[0];
		if (tgt.kind == EK::Var) { arrays.erase(tgt.name); return; }
		if (tgt.kind == EK::ArrayRef) {
			auto& m = arrays[tgt.name];
			m.erase(buildSubscript(tgt.args));
		}
	}

	void runPrint(Stmt& s) {
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
	}

	void runPrintf(Stmt& s) {
		std::vector<AwkValue> a;
		for (auto& x : s.exprs) a.push_back(eval(*x));
		emit(formatPrintf(a), s);
	}

	// Run statement; break/continue/next post to `flow` and unwind via plain
	// returns; exit sets `exiting`. Enclosing loops / record pass consume them.
	void run(Stmt& s) {
		if (exiting) return;
		switch (s.kind) {
		case SK::Empty: return;
		case SK::Block:
			for (auto& c : s.children) {
				run(*c);
				if (exiting || flow != Flow::None) return;
			}
			return;
		case SK::If:       runIf(s);      return;
		case SK::While:    runWhile(s);   return;
		case SK::DoWhile:  runDoWhile(s); return;
		case SK::For:      runFor(s);     return;
		case SK::ForIn:    runForIn(s);   return;
		case SK::Break:    flow = Flow::Break;    return;
		case SK::Continue: flow = Flow::Continue; return;
		case SK::Next:     flow = Flow::Next;     return;
		case SK::Exit:
			if (!s.exprs.empty()) exit_status = (int)eval(*s.exprs[0]).asNumber();
			exiting = true;
			return;
		case SK::Delete:   runDelete(s); return;
		case SK::Return:   return;     // no user functions yet
		case SK::Print:    runPrint(s);  return;
		case SK::Printf:   runPrintf(s); return;
		case SK::ExprStmt: eval(*s.exprs[0]); return;
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
		bool from_file = e.b != nullptr;
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
			std::regex re;
			return compileRegex(re, e.str_val) && searchRegex(record, re);
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
		// A pending flow signal / `exiting` simply propagates to the caller.
		if (match) run(*r.action);
	}

	void runBegins() {
		for (auto& r : prog.rules) {
			if (r.pat.kind != AwkPattern::Begin) continue;
			run(*r.action);
			if (exiting) return;
			flow = Flow::None;   // stray break/continue/next: drop, next rule
		}
	}
	void runEnds() {
		for (auto& r : prog.rules) {
			if (r.pat.kind != AwkPattern::End) continue;
			run(*r.action);
			if (exiting) return;
			flow = Flow::None;   // stray break/continue/next: drop, next rule
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
			for (auto& r : prog.rules) {
				if (r.pat.kind == AwkPattern::Begin || r.pat.kind == AwkPattern::End) continue;
				runAwkRule(r);
				if (exiting) return;
				if (flow == Flow::Next) break;
			}
			// `next` (and any stray break/continue) ends this record's pass.
			flow = Flow::None;
		}
	}
};

struct AwkInvocation {
	std::string program_text;
	std::string field_sep;
	std::vector<std::string> files;
	std::vector<std::pair<std::string, std::string>> var_assigns;
	bool have_program = false;
};

// Parse a single -f program-file argument into `inv.program_text`. Returns
// false (and prints an error) when the file can't be opened.
static bool loadProgramFile(Executor& exec, const std::string& path, AwkInvocation& inv) {
	std::string p = exec.pathConv().toWin32(path);
	std::ifstream f(p, std::ios::binary);
	if (!f) {
		std::fprintf(stderr, "awk: cannot open program file: %s\n", path.c_str());
		return false;
	}
	std::ostringstream ss; ss << f.rdbuf();
	inv.program_text = ss.str();
	inv.have_program = true;
	return true;
}

// Walk argv collecting flags, the program text, and input files. Returns 0
// on success, or the exit status to propagate from builtin_awk on error.
static int parseAwkArgs(Executor& exec, const std::vector<std::string>& args, AwkInvocation& inv) {
	for (std::size_t i = 0; i < args.size(); ++i) {
		const std::string& a = args[i];
		if (a == "-F" && i + 1 < args.size()) { inv.field_sep = args[++i]; continue; }
		if (a.size() > 2 && a.compare(0, 2, "-F") == 0) { inv.field_sep = a.substr(2); continue; }
		if (a == "-f" && i + 1 < args.size()) {
			if (!loadProgramFile(exec, args[++i], inv)) return 2;
			continue;
		}
		if (a == "-v" && i + 1 < args.size()) {
			std::string kv = args[++i];
			auto eq = kv.find('=');
			if (eq != std::string::npos) {
				inv.var_assigns.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
			}
			continue;
		}
		if (a == "--") {
			++i;
			if (!inv.have_program && i < args.size()) {
				inv.program_text = args[i++];
				inv.have_program = true;
			}
			while (i < args.size()) inv.files.push_back(args[i++]);
			break;
		}
		if (!inv.have_program) {
			inv.program_text = a;
			inv.have_program = true;
			continue;
		}
		inv.files.push_back(a);
	}
	return 0;
}

// Feed each input file (or stdin) through the interpreter. Once a program
// calls exit, the interpreter's `exiting` flag makes the remaining streams
// no-ops, so this simply runs to completion.
static void runAwkInputs(Executor& exec, AwkInterp& interp, const std::vector<std::string>& files) {
	if (files.empty()) {
		interp.processStream(stdin, "");
		return;
	}
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

int builtin_awk(Executor& exec, const std::vector<std::string>& args) {
	AwkInvocation inv;
	if (int rc = parseAwkArgs(exec, args, inv); rc != 0) return rc;
	if (!inv.have_program) {
		std::fprintf(stderr, "awk: missing program text\n");
		return 2;
	}

	AwkParser parser(inv.program_text);
	AwkProgram prog = parser.parseAwkProgram();
	if (!parser.error_msg.empty()) {
		std::fprintf(stderr, "%s\n", parser.error_msg.c_str());
		return 2;
	}

	AwkInterp interp(prog);
	if (!inv.field_sep.empty()) interp.FS = inv.field_sep;
	for (auto& kv : inv.var_assigns) interp.vars[kv.first] = AwkValue::str(kv.second);

	interp.runBegins();
	runAwkInputs(exec, interp, inv.files);
	interp.runEnds();
	return interp.exit_status;
}

}  // namespace wbsh

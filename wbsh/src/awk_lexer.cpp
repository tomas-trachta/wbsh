/**
 * @file awk_lexer.cpp
 * @brief Tokenizer for awk program text.
 */

#include "awk_lexer.h"

#include <cctype>
#include <unordered_map>

#include "awk_internal.h"

namespace wbsh {

	namespace awk_detail {

		static bool isBlank(char c) {
			return c == ' ' || c == '\t' || c == '\r';
		}

		static bool isIdentifierChar(char c) {
			return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
		}

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
				{ "func", TokenKind::KwFunction },
				{ "return", TokenKind::KwReturn }, { "getline", TokenKind::KwGetline },
			};

			const auto it = kKeywords.find(word);
			if (it == kKeywords.end()) return TokenKind::Identifier;
			return it->second;
		}

		bool Lexer::atEnd() const {
			return pos >= source.size();
		}

		char Lexer::current() const {
			return pos < source.size() ? source[pos] : '\0';
		}

		char Lexer::lookahead() const {
			return pos + 1 < source.size() ? source[pos + 1] : '\0';
		}

		void Lexer::advance() {
			if (pos >= source.size()) return;

			if (source[pos] == '\n') ++line;
			++pos;
		}

		bool Lexer::consumeIf(char c) {
			if (pos >= source.size() || source[pos] != c) return false;

			advance();
			return true;
		}

		Token Lexer::startToken() const {
			Token token;
			token.line = line;
			return token;
		}

		Token Lexer::finish(Token token, TokenKind kind, bool value_ends) {
			token.kind = kind;
			prev_was_value = value_ends;
			return token;
		}

		Token Lexer::finishEnd() {
			Token token = startToken();
			token.kind = TokenKind::End;
			return token;
		}

		void Lexer::skipBlanks() {
			while (!atEnd()) {
				const char c = current();
				if (isBlank(c)) {
					advance();
					continue;
				}

				if (c == '\\' && (lookahead() == '\n' || lookahead() == '\r')) {
					advance();
					while (current() == '\r') advance();
					advance();
					continue;
				}

				if (c != '#') break;
				while (!atEnd() && current() != '\n') advance();
			}
		}

		void Lexer::skipDigits() {
			while (!atEnd() && std::isdigit(static_cast<unsigned char>(current()))) advance();
		}

		bool Lexer::startsNumber() const {
			const char c = current();
			if (std::isdigit(static_cast<unsigned char>(c))) return true;
			return c == '.' && std::isdigit(static_cast<unsigned char>(lookahead()));
		}

		bool Lexer::startsIdentifier() const {
			const char c = current();
			return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
		}

		Token Lexer::readIdentifier() {
			Token token = startToken();

			const std::size_t start = pos;
			while (!atEnd() && isIdentifierChar(current())) advance();

			token.text = source.substr(start, pos - start);
			token.kind = identifierKind(token.text);
			if (token.kind == TokenKind::Identifier && current() == '(') {
				token.kind = TokenKind::FuncName;
			}

			return token;
		}

		Token Lexer::readNumber() {
			Token token = startToken();

			const std::size_t start = pos;
			skipDigits();
			if (consumeIf('.')) skipDigits();
			if (current() == 'e' || current() == 'E') {
				const std::size_t exponent = pos;
				advance();
				if (current() == '+' || current() == '-') advance();
				if (!std::isdigit(static_cast<unsigned char>(current()))) pos = exponent;
				skipDigits();
			}

			token.text = source.substr(start, pos - start);
			token.number = parseAwkNumber(token.text);
			token.kind = TokenKind::Number;
			return token;
		}

		Token Lexer::readString() {
			Token token = startToken();
			advance();

			std::string raw;
			while (!atEnd() && current() != '"') {
				if (current() == '\\' && pos + 1 < source.size()) {
					raw.push_back(current());
					advance();
				}

				raw.push_back(current());
				advance();
			}

			if (!atEnd()) advance();
			token.kind = TokenKind::String;
			token.text = expandEscapes(raw);
			return token;
		}

		Token Lexer::readRegex() {
			Token token = startToken();
			advance();

			std::string pattern;
			bool in_bracket = false;
			while (!atEnd() && (in_bracket || current() != '/') && current() != '\n') {
				if (current() == '\\' && pos + 1 < source.size()) {
					pattern.push_back(current());
					advance();
				} else if (current() == '[') {
					in_bracket = true;
				} else if (current() == ']') {
					in_bracket = false;
				}

				pattern.push_back(current());
				advance();
			}

			if (!atEnd() && current() == '/') advance();
			token.kind = TokenKind::Regex;
			token.text = std::move(pattern);
			return token;
		}

		Token Lexer::readSimplePunct(char c) {
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

		Token Lexer::readSlashOp() {
			if (!prev_was_value) {
				Token token = readRegex();
				prev_was_value = true;
				return token;
			}

			Token token = startToken();
			advance();
			if (consumeIf('=')) return finish(token, TokenKind::SlashAssign, false);
			return finish(token, TokenKind::Slash, false);
		}

		Token Lexer::opOrAssign(Token token, TokenKind simple, TokenKind compound) {
			if (consumeIf('=')) return finish(token, compound, false);
			return finish(token, simple, false);
		}

		Token Lexer::readArithOp(char c) {
			Token token = startToken();
			advance();
			switch (c) {
				case '+':
					if (consumeIf('+')) return finish(token, TokenKind::Inc, true);
					return opOrAssign(token, TokenKind::Plus, TokenKind::PlusAssign);
				case '-':
					if (consumeIf('-')) return finish(token, TokenKind::Dec, true);
					return opOrAssign(token, TokenKind::Minus, TokenKind::MinusAssign);
				case '*':
					if (consumeIf('*')) {
						return opOrAssign(token, TokenKind::Caret, TokenKind::CaretAssign);
					}

					return opOrAssign(token, TokenKind::Star, TokenKind::StarAssign);
				case '%': return opOrAssign(token, TokenKind::Percent, TokenKind::PercentAssign);
				case '^': return opOrAssign(token, TokenKind::Caret,   TokenKind::CaretAssign);
				default:  token.kind = TokenKind::End; return token;
			}
		}

		Token Lexer::readCompareOp(char c) {
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

		Token Lexer::readLogicalOp(char c) {
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

		Token Lexer::readWord() {
			Token token = readIdentifier();
			const bool is_value = token.kind == TokenKind::Identifier
				|| token.kind == TokenKind::KwGetline;
			prev_was_value = is_value;
			return token;
		}

		Token Lexer::next() {
			if (!pushback.empty()) {
				Token token = std::move(pushback.back());
				pushback.pop_back();
				return token;
			}

			skipBlanks();
			if (atEnd()) return finishEnd();
			if (startsNumber()) return finish(readNumber(), TokenKind::Number, true);
			if (startsIdentifier()) return readWord();

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

		Token& Lexer::peek() {
			if (pushback.empty()) pushback.push_back(next());
			return pushback.back();
		}

		void Lexer::unread(Token token) {
			pushback.push_back(std::move(token));
		}

		void Lexer::abortToEof() {
			pushback.clear();
			pos = source.size();
		}

		Lexer::Mark Lexer::mark() const {
			return Mark{ pos, line, pushback, prev_was_value };
		}

		void Lexer::rewind(const Mark& saved) {
			pos = saved.pos;
			line = saved.line;
			pushback = saved.pushback;
			prev_was_value = saved.prev_was_value;
		}

	}  // namespace awk_detail

}  // namespace wbsh

#pragma once

/**
 * @file awk_lexer.h
 * @brief Tokenizer for awk program text.
 */

#include <string>
#include <vector>

namespace wbsh {

	namespace awk_detail {

		enum class TokenKind {
			End, Number, String, Regex, Identifier,
			// An identifier written directly before '(': only these can call a
			// user-defined function.
			FuncName,
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

		struct Lexer {
			struct Mark {
				std::size_t pos;
				int line;
				std::vector<Token> pushback;
				bool prev_was_value;
			};

			const std::string& source;
			std::size_t pos = 0;
			int line = 1;
			std::vector<Token> pushback;
			// A '/' after a value is division; anywhere else it opens a regex.
			bool prev_was_value = false;

			explicit Lexer(const std::string& text) : source(text) {}

			Token next();
			Token& peek();
			void unread(Token token);
			void abortToEof();
			Mark mark() const;
			void rewind(const Mark& saved);

		private:
			bool atEnd() const;
			char current() const;
			char lookahead() const;
			void advance();
			bool consumeIf(char c);
			Token startToken() const;
			Token finish(Token token, TokenKind kind, bool value_ends);
			Token finishEnd();
			void skipBlanks();
			void skipDigits();
			bool startsNumber() const;
			bool startsIdentifier() const;
			Token readIdentifier();
			Token readNumber();
			Token readString();
			Token readRegex();
			Token readSimplePunct(char c);
			Token readSlashOp();
			Token opOrAssign(Token token, TokenKind simple, TokenKind compound);
			Token readArithOp(char c);
			Token readCompareOp(char c);
			Token readLogicalOp(char c);
			Token readWord();
		};

	}  // namespace awk_detail

}  // namespace wbsh

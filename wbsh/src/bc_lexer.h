#pragma once

/**
 * @file bc_lexer.h
 * @brief bc tokenizer that pulls source lines lazily, so interactive input runs line by line.
 */

#include <cstddef>
#include <string>

#include "bc_internal.h"

namespace wbsh {

	namespace bc_detail {

		enum class TokKind { End, Newline, Semicolon, Number, Name, String, Op, Error };

		struct Token {
			TokKind kind = TokKind::End;
			std::string text;
			int line = 1;
		};

		class BcLexer {
		public:
			struct Mark {
				std::size_t pos;
				int line;
			};

			explicit BcLexer(BcSource& source);

			Token next();
			int line() const { return line_; }

			/// Drops the text already consumed; only valid between top-level items.
			void compact();
			Mark mark() const;
			void reset(const Mark& saved);

		private:
			bool ensure(std::size_t count);
			char at(std::size_t offset);
			bool skipLineContinuation();
			bool skipBlockComment(Token& error);
			bool skipSpace(Token& error);
			void readDigits(std::string& text);
			Token readNumber();
			Token readName();
			Token readString();
			Token readOperator();

			BcSource& source_;
			std::string buffer_;
			std::size_t pos_ = 0;
			int line_ = 1;
		};

	}  // namespace bc_detail

}  // namespace wbsh

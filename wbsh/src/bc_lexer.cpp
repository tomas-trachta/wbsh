#include "bc_lexer.h"

#include <cstring>

namespace wbsh {

	namespace bc_detail {

		bool BcSource::readLine(std::string& line) {
			line.clear();
			if (file == nullptr) {
				if (text_pos >= text.size()) return false;

				const std::size_t end = text.find('\n', text_pos);
				const std::size_t stop = end == std::string::npos ? text.size() : end + 1;
				line = text.substr(text_pos, stop - text_pos);
				text_pos = stop;
				return true;
			}

			int c = 0;
			while ((c = std::fgetc(file)) != EOF) {
				line.push_back(static_cast<char>(c));
				if (c == '\n') break;
			}

			const std::size_t size = line.size();
			if (size >= 2 && line[size - 2] == '\r' && line[size - 1] == '\n') line.erase(size - 2, 1);
			return !line.empty();
		}

		static bool isBlank(char c) {
			return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v';
		}

		static bool isNameStart(char c) {
			return c >= 'a' && c <= 'z';
		}

		static bool isNameChar(char c) {
			return isNameStart(c) || (c >= '0' && c <= '9') || c == '_';
		}

		static bool isDigitChar(char c) {
			return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z');
		}

		BcLexer::BcLexer(BcSource& source) : source_(source) {}

		bool BcLexer::ensure(std::size_t count) {
			while (pos_ + count > buffer_.size()) {
				std::string line;
				if (!source_.readLine(line)) return false;
				buffer_ += line;
			}

			return true;
		}

		char BcLexer::at(std::size_t offset) {
			return ensure(offset + 1) ? buffer_[pos_ + offset] : '\0';
		}

		void BcLexer::compact() {
			buffer_.erase(0, pos_);
			pos_ = 0;
		}

		BcLexer::Mark BcLexer::mark() const {
			return { pos_, line_ };
		}

		void BcLexer::reset(const Mark& saved) {
			pos_ = saved.pos;
			line_ = saved.line;
		}

		bool BcLexer::skipLineContinuation() {
			if (at(0) != '\\') return false;

			std::size_t width = at(1) == '\r' ? 2 : 1;
			if (at(width) != '\n') return false;

			pos_ += width + 1;
			++line_;
			return true;
		}

		bool BcLexer::skipBlockComment(Token& error) {
			pos_ += 2;
			for (;;) {
				const char c = at(0);
				if (c == '\0') {
					error = { TokKind::Error, "end of file in a comment", line_ };
					return false;
				}

				if (c == '*' && at(1) == '/') {
					pos_ += 2;
					return true;
				}

				if (c == '\n') ++line_;
				++pos_;
			}
		}

		bool BcLexer::skipSpace(Token& error) {
			for (;;) {
				const char c = at(0);
				if (isBlank(c)) {
					++pos_;
				} else if (skipLineContinuation()) {
					continue;
				} else if (c == '/' && at(1) == '*') {
					if (!skipBlockComment(error)) return false;
				} else if (c == '#') {
					while (at(0) != '\0' && at(0) != '\n') ++pos_;
				} else {
					return true;
				}
			}
		}

		void BcLexer::readDigits(std::string& text) {
			for (;;) {
				if (skipLineContinuation()) continue;
				if (!isDigitChar(at(0))) return;

				text.push_back(at(0));
				++pos_;
			}
		}

		Token BcLexer::readNumber() {
			Token token{ TokKind::Number, std::string(), line_ };
			readDigits(token.text);
			if (at(0) != '.') return token;

			token.text.push_back('.');
			++pos_;
			readDigits(token.text);
			return token;
		}

		Token BcLexer::readName() {
			Token token{ TokKind::Name, std::string(), line_ };
			while (isNameChar(at(0))) {
				token.text.push_back(at(0));
				++pos_;
			}

			return token;
		}

		Token BcLexer::readString() {
			Token token{ TokKind::String, std::string(), line_ };
			++pos_;
			for (;;) {
				const char c = at(0);
				if (c == '\0') return { TokKind::Error, "end of file in a string", token.line };

				++pos_;
				if (c == '"') return token;
				if (c == '\n') ++line_;
				token.text.push_back(c);
			}
		}

		static bool isTwoCharOperator(char first, char second) {
			if (second == '=' && std::strchr("=!<>+-*/%^", first) != nullptr) return true;
			if (first == second && std::strchr("&|+-", first) != nullptr) return true;
			return false;
		}

		Token BcLexer::readOperator() {
			const char c = at(0);
			if (isTwoCharOperator(c, at(1))) {
				Token token{ TokKind::Op, buffer_.substr(pos_, 2), line_ };
				pos_ += 2;
				return token;
			}

			if (std::strchr("+-*/%^=<>!()[]{},", c) == nullptr || c == '\0') {
				++pos_;
				return { TokKind::Error, std::string("illegal character: ") + c, line_ };
			}

			++pos_;
			return { TokKind::Op, std::string(1, c), line_ };
		}

		Token BcLexer::next() {
			Token error;
			if (!skipSpace(error)) return error;

			const char c = at(0);
			if (c == '\0') return { TokKind::End, std::string(), line_ };
			if (c == '\n') {
				++pos_;
				return { TokKind::Newline, std::string(), line_++ };
			}

			if (c == ';') {
				++pos_;
				return { TokKind::Semicolon, std::string(), line_ };
			}

			if (c == '"') return readString();
			if (isDigitChar(c) || (c == '.' && isDigitChar(at(1)))) return readNumber();
			if (c == '.') {
				++pos_;
				return { TokKind::Name, ".", line_ };
			}

			if (isNameStart(c)) return readName();
			return readOperator();
		}

	}  // namespace bc_detail

}  // namespace wbsh

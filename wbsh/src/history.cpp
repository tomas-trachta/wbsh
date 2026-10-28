/**
 * @file history.cpp
 * @brief Joining the lines of a multi-line command into one history entry.
 */

#include "history.h"

#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

#include "lexer.h"

namespace wbsh {

	namespace history_detail {

		static const char* const kSemicolon = "; ";
		static const char* const kSpace = " ";
		static const char* const kNewline = "\n";
		static const std::size_t kNoToken = static_cast<std::size_t>(-1);

		static const char* const kReservedWords[] = {
			"!", "{", "}", "case", "do", "done", "elif", "else", "esac", "fi", "for",
			"function", "if", "in", "select", "then", "time", "until", "while", nullptr,
		};

		static const char* const kCommandOpeners[] = {
			"!", "{", "do", "elif", "else", "if", "then", "time", "until", "while", nullptr,
		};

		static const char* const kNoSemicolonAfter[] = {
			"!", "{", "case", "do", "elif", "else", "if", "in", "then", "until", "while", nullptr,
		};

		static bool isOneOf(const std::string& word, const char* const* words) {
			for (int i = 0; words[i] != nullptr; ++i) {
				if (word == words[i]) return true;
			}

			return false;
		}

		static bool isBlank(const std::string& text) {
			for (const char letter : text) {
				if (std::isspace(static_cast<unsigned char>(letter)) == 0) return false;
			}

			return true;
		}

		static bool separatesCommands(TokKind kind) {
			switch (kind) {
			case TokKind::Newline:
			case TokKind::Semi:
			case TokKind::Amp:
			case TokKind::Pipe:
			case TokKind::PipeAmp:
			case TokKind::AndIf:
			case TokKind::OrIf:
			case TokKind::LParen:
			case TokKind::RParen:
			case TokKind::DSemi:
			case TokKind::SemiAmp:
			case TokKind::DSemiAmp:
				return true;
			default:
				return false;
			}
		}

		static bool isReservedAt(const std::vector<Token>& tokens, std::size_t i);

		static bool inCommandPosition(const std::vector<Token>& tokens, std::size_t i) {
			if (i == 0) return true;

			const Token& before = tokens[i - 1];
			if (before.kind != TokKind::Word) return separatesCommands(before.kind);

			return isOneOf(before.text, kCommandOpeners) && isReservedAt(tokens, i - 1);
		}

		static bool introducesIn(const std::vector<Token>& tokens, std::size_t i) {
			const Token& keyword = tokens[i];
			const bool loop_or_case = keyword.text == "for" || keyword.text == "case"
				|| keyword.text == "select";
			return keyword.kind == TokKind::Word && loop_or_case && isReservedAt(tokens, i);
		}

		static bool isReservedAt(const std::vector<Token>& tokens, std::size_t i) {
			const Token& token = tokens[i];
			if (token.kind != TokKind::Word) return false;
			if (token.text == "in") return i >= 2 && introducesIn(tokens, i - 2);

			return isOneOf(token.text, kReservedWords) && inCommandPosition(tokens, i);
		}

		static std::size_t lastMeaningfulToken(const std::vector<Token>& tokens) {
			for (std::size_t i = tokens.size(); i > 0; --i) {
				const TokKind kind = tokens[i - 1].kind;
				if (kind != TokKind::EndOfInput && kind != TokKind::Newline) return i - 1;
			}

			return kNoToken;
		}

		static std::size_t tokenEnd(const Token& token) {
			return token.loc.offset + token.text.size();
		}

		// The lexer drops comments, so text left after the last token
		// can only be one; a newline has to end it in the entry.
		static bool endsInComment(const std::string& pending, const std::vector<Token>& tokens,
		                          std::size_t last) {
			const std::size_t from = last == kNoToken ? 0 : tokenEnd(tokens[last]);
			if (from >= pending.size()) return false;

			return !isBlank(pending.substr(from));
		}

		static bool hasHeredoc(const std::vector<Token>& tokens) {
			for (const Token& token : tokens) {
				if (token.kind == TokKind::DLess || token.kind == TokKind::DLessDash) return true;
			}

			return false;
		}

		static int openCaseCount(const std::vector<Token>& tokens, std::size_t end) {
			int open = 0;
			for (std::size_t i = 0; i < end; ++i) {
				if (!isReservedAt(tokens, i)) continue;
				if (tokens[i].text == "case") ++open;
				if (tokens[i].text == "esac") --open;
			}

			return open;
		}

		static bool opensArrayAssignment(const std::vector<Token>& tokens, std::size_t i) {
			if (i == 0) return false;

			const Token& name = tokens[i - 1];
			return name.kind == TokKind::Word && !name.text.empty() && name.text.back() == '='
			    && tokenEnd(name) == tokens[i].loc.offset;
		}

		static bool insideArrayAssignment(const std::vector<Token>& tokens, std::size_t end) {
			std::vector<bool> open_parens;
			for (std::size_t i = 0; i <= end; ++i) {
				const TokKind kind = tokens[i].kind;
				if (kind == TokKind::LParen) open_parens.push_back(opensArrayAssignment(tokens, i));
				if (kind == TokKind::RParen && !open_parens.empty()) open_parens.pop_back();
			}

			return !open_parens.empty() && open_parens.back();
		}

		static bool startsWithIn(const std::string& line) {
			std::size_t i = 0;
			while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
			if (line.compare(i, 2, "in") != 0) return false;

			return i + 2 >= line.size() || isBlank(line.substr(i + 2, 1));
		}

		static const char* joinAfterOperator(const std::vector<Token>& tokens, std::size_t last) {
			if (tokens[last].kind != TokKind::RParen) return kSpace;
			if (last > 0 && tokens[last - 1].kind == TokKind::LParen) return kSpace;

			return openCaseCount(tokens, last) > 0 ? kSpace : kSemicolon;
		}

		static const char* joinAfterKeywordArgument(const std::string& keyword,
		                                            const std::string& line) {
			const bool loop = keyword == "for" || keyword == "select";
			if (loop) return startsWithIn(line) ? kSpace : kSemicolon;
			if (keyword == "case" || keyword == "function") return kSpace;

			return kSemicolon;
		}

		static const char* joinAfterWord(const std::vector<Token>& tokens, std::size_t last,
		                                 const std::string& line) {
			if (insideArrayAssignment(tokens, last)) return kSpace;
			const bool opens_body = isOneOf(tokens[last].text, kNoSemicolonAfter);
			if (opens_body && isReservedAt(tokens, last)) return kSpace;
			if (last > 0 && isReservedAt(tokens, last - 1)) {
				return joinAfterKeywordArgument(tokens[last - 1].text, line);
			}

			return kSemicolon;
		}

		static const char* delimiterBefore(const std::string& pending, const std::string& line) {
			Lexer lexer(pending);
			const std::vector<Token> tokens = lexer.tokenize();
			if (!lexer.errors().empty() || hasHeredoc(tokens)) return kNewline;

			const std::size_t last = lastMeaningfulToken(tokens);
			if (endsInComment(pending, tokens, last)) return kNewline;
			if (last == kNoToken) return kSpace;
			if (tokens[last].kind != TokKind::Word) return joinAfterOperator(tokens, last);

			return joinAfterWord(tokens, last, line);
		}

	}  // namespace history_detail

	void appendHistoryLine(std::string& entry, const std::string& pending,
	                       const std::string& line) {
		if (pending.empty()) {
			entry = line;
			return;
		}

		const bool continued = endsWithLineContinuation(pending);
		if (continued && !entry.empty() && entry.back() == '\\') {
			entry.pop_back();
			entry += line;
			return;
		}

		const std::string delimiter = history_detail::delimiterBefore(pending, line);
		if (delimiter != history_detail::kNewline && history_detail::isBlank(line)) return;

		entry += delimiter;
		entry += line;
	}

	bool endsWithLineContinuation(const std::string& text) {
		Lexer lexer(text);
		const std::vector<Token> tokens = lexer.tokenize();
		const std::size_t last = history_detail::lastMeaningfulToken(tokens);
		if (last == history_detail::kNoToken) return false;

		const Token& token = tokens[last];
		if (token.kind != TokKind::Word || token.segments.empty()) return false;
		if (history_detail::tokenEnd(token) != text.size()) return false;

		const WordSegment& tail = token.segments.back();
		const bool literal = tail.kind == WordSegment::Kind::Literal;
		return literal && !tail.text.empty() && tail.text.back() == '\\';
	}

}  // namespace wbsh

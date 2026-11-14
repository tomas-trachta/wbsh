/**
 * @file lexer_subscript.cpp
 * @brief Array subscripts in assignment words, read as one unit.
 *
 * `a[hello world]=x` and `a=([foo bar]=x)` are single words in bash:
 * where an assignment is acceptable, an identifier followed by `[`, or a
 * `[` that starts a word of a compound assignment, opens a subscript that
 * runs to the matching `]`, blanks and operator characters included.
 */

#include "lexer.h"

#include <cctype>

namespace wbsh::lexer_subscript_detail {

	static bool isIdentifier(const std::string& text) {
		if (text.empty()) return false;
		if (std::isdigit(static_cast<unsigned char>(text[0]))) return false;

		for (const char c : text) {
			if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
		}

		return true;
	}

	static bool isAssignmentShaped(const Token& token) {
		if (token.kind != TokKind::Word || token.segments.empty()) return false;

		const WordSegment& first = token.segments[0];
		if (first.kind != WordSegment::Kind::Literal) return false;

		const std::size_t name_end = first.text.find_first_of("=+[");
		if (name_end == std::string::npos || name_end == 0) return false;
		return isIdentifier(first.text.substr(0, name_end));
	}

	static bool endsWithAssignmentOperator(const Token& token) {
		const std::string& text = token.text;
		return isAssignmentShaped(token) && !text.empty() && text.back() == '=';
	}

	static void appendLiteral(std::vector<WordSegment>& out, char c) {
		if (out.empty() || out.back().kind != WordSegment::Kind::Literal) {
			out.push_back(WordSegment{});
		}

		out.back().text.push_back(c);
	}

	static bool isQuotingStart(char c) {
		return c == '\\' || c == '\'' || c == '"' || c == '$' || c == '`';
	}

}  // namespace wbsh::lexer_subscript_detail

namespace wbsh {

	using namespace lexer_subscript_detail;

	bool Lexer::inCompoundAssignment() const {
		for (std::size_t k = tokens_.size(); k-- > 0;) {
			const Token& token = tokens_[k];
			if (token.kind == TokKind::Word || token.kind == TokKind::Newline) continue;
			if (token.kind != TokKind::LParen || k == 0) return false;
			return endsWithAssignmentOperator(tokens_[k - 1]);
		}

		return false;
	}

	bool Lexer::followsAssignmentWord() const {
		return !tokens_.empty() && isAssignmentShaped(tokens_.back());
	}

	bool Lexer::assignmentNameBeforeSubscript(const std::string& name) const {
		return isIdentifier(name) && (atCommandWordStart() || followsAssignmentWord());
	}

	bool Lexer::atAssignmentSubscript(const std::vector<WordSegment>& out) const {
		if (peek() != '[') return false;
		if (out.empty()) return inCompoundAssignment();
		if (out.size() != 1 || out[0].kind != WordSegment::Kind::Literal) return false;

		return assignmentNameBeforeSubscript(out[0].text);
	}

	// `]=value` stays in the subscript's literal, so the word reads as the
	// single `name[key]=value` unit the assignment parser expects.
	void Lexer::joinLiteralAfterSubscript(std::vector<WordSegment>& out) {
		std::vector<WordSegment> tail;
		if (!readLiteralRun(tail)) return;

		out.back().text += tail.front().text;
	}

	// Reads from `[` through its matching `]`. A subscript left open at the
	// end of the line is not one: the position is restored and false
	// returned, so the word is scanned the ordinary way.
	bool Lexer::readAssignmentSubscript(std::vector<WordSegment>& out) {
		const std::size_t pos = pos_;
		const SourceLoc loc = loc_;
		const std::size_t error_count = errors_.size();
		const std::vector<WordSegment> before = out;

		int depth = 0;
		while (!eof() && peek() != '\n') {
			const char c = peek();
			if (isQuotingStart(c)) {
				readWordSegment(out);
				continue;
			}

			appendLiteral(out, advance());
			if (c == '[') ++depth;
			if (c != ']' || --depth != 0) continue;

			joinLiteralAfterSubscript(out);
			return true;
		}

		restorePosition(pos, loc, error_count);
		out = before;
		return false;
	}

}  // namespace wbsh

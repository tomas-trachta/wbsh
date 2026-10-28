/**
 * @file vi_text.cpp
 * @brief vi's character-level helpers: stepping over characters, word
 *        classes, indentation and keeping the cursor on the text.
 */

#include "vi_internal.h"

#include <string>

#include "textcells.h"
#include "utf8.h"

namespace wbsh {

	namespace vi_detail {

		std::size_t nextCharStart(const std::string& line, std::size_t pos) {
			if (pos >= line.size()) return line.size();

			return clusterAt(line, pos).end;
		}

		std::size_t previousCharStart(const std::string& line, std::size_t pos) {
			if (pos == 0) return 0;

			return previousClusterStart(line, pos);
		}

		std::size_t lastCharStart(const std::string& line) {
			return line.empty() ? 0 : previousCharStart(line, line.size());
		}

		std::size_t firstNonBlank(const std::string& line) {
			std::size_t at = 0;
			while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) ++at;
			if (at == line.size()) return lastCharStart(line);
			return at;
		}

		bool isBlankChar(const std::string& line, std::size_t pos) {
			return pos < line.size() && (line[pos] == ' ' || line[pos] == '\t');
		}

		static bool isWordCode(char32_t code) {
			if (code >= 0x80) return true;

			const bool letter = (code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z');
			return letter || (code >= '0' && code <= '9') || code == '_';
		}

		// 0 for blanks and the end of the line, 1 for punctuation, 2 for
		// word characters; a WORD (bigword) is any run of non-blanks.
		int charClass(const std::string& line, std::size_t pos, bool bigword) {
			if (pos >= line.size() || isBlankChar(line, pos)) return 0;
			if (bigword) return 1;

			return isWordCode(decodeUtf8(line, pos).code) ? 2 : 1;
		}

		std::string leadingIndent(const std::string& line) {
			std::size_t at = 0;
			while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) ++at;
			return line.substr(0, at);
		}

		std::size_t indentWidth(const Editor& ed, const std::string& line) {
			std::size_t width = 0;
			for (std::size_t at = 0; at < line.size(); ++at) {
				if (line[at] == ' ') ++width;
				else if (line[at] == '\t') width += ed.options.tabstop - width % ed.options.tabstop;
				else break;
			}

			return width;
		}

		std::string indentOfWidth(const Editor& ed, std::size_t width) {
			if (ed.options.expandtab) return std::string(width, ' ');

			const std::size_t tabs = width / ed.options.tabstop;
			return std::string(tabs, '\t') + std::string(width % ed.options.tabstop, ' ');
		}

		std::string keyText(Key key) {
			std::string text;
			if (key < 0x110000) appendUtf8(text, key);
			else if (isRawByteKey(key)) text.push_back(static_cast<char>(key - kKeyRawByte));
			return text;
		}

		bool isRawByteKey(Key key) {
			return key >= kKeyRawByte && key < kKeyRawByte + 0x100;
		}

		void clampCursor(Editor& ed) {
			if (ed.cursor.row >= ed.lines.size()) ed.cursor.row = ed.lines.size() - 1;

			const std::string& line = ed.lines[ed.cursor.row];
			const bool past_end_allowed = ed.mode == Mode::Insert || ed.mode == Mode::Replace;
			if (past_end_allowed) {
				if (ed.cursor.col > line.size()) ed.cursor.col = line.size();
				return;
			}

			if (ed.cursor.col >= line.size()) ed.cursor.col = lastCharStart(line);
		}

		void moveToFirstNonBlank(Editor& ed, std::size_t row) {
			ed.cursor.row = row < ed.lines.size() ? row : ed.lines.size() - 1;
			ed.cursor.col = firstNonBlank(ed.lines[ed.cursor.row]);
			updateWantColumn(ed);
		}

		void updateWantColumn(Editor& ed) {
			ed.want_column = displayColumn(ed, ed.cursor.row, ed.cursor.col);
			ed.want_end = false;
		}

	}  // namespace vi_detail

}  // namespace wbsh

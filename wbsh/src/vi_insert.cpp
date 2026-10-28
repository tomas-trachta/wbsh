/**
 * @file vi_insert.cpp
 * @brief vi's insert and replace modes: typing, line breaks with
 *        autoindent, the erasing keys, literal input, and the count that
 *        repeats what was typed.
 */

#include "vi_internal.h"

#include <string>
#include <utility>
#include <vector>

namespace wbsh {

	namespace vi_detail {

		struct Overwrite {
			std::string typed;
			std::string replaced;
		};

		struct InsertSession {
			InsertKind kind = InsertKind::Insert;
			std::string typed;
			Position start;
			std::vector<Overwrite> overwrites;
			std::size_t indent_row = kNoMark;
			std::size_t indent_length = 0;
		};

		static bool isReplacing(const Editor& ed) {
			return ed.mode == Mode::Replace;
		}

		void openLine(Editor& ed, bool below) {
			const std::string indent = ed.options.autoindent
				? leadingIndent(ed.lines[ed.cursor.row])
				: std::string();
			const std::size_t at = below ? ed.cursor.row + 1 : ed.cursor.row;
			insertLineTexts(ed, at, { indent });
			ed.cursor = { at, indent.size() };
		}

		static void noteAutoIndent(InsertSession& session, const Editor& ed) {
			session.indent_row = ed.cursor.row;
			session.indent_length = ed.cursor.col;
		}

		static void prepareInsert(Editor& ed, InsertSession& session) {
			ed.mode = session.kind == InsertKind::Replace ? Mode::Replace : Mode::Insert;
			const std::string& line = ed.lines[ed.cursor.row];
			switch (session.kind) {
				case InsertKind::Append:
					if (!line.empty()) ed.cursor.col = nextCharStart(line, ed.cursor.col);
					break;
				case InsertKind::AppendLine:
					ed.cursor.col = line.size();
					break;
				case InsertKind::InsertLine:
					ed.cursor.col = leadingIndent(line).size();
					break;
				case InsertKind::OpenBelow:
				case InsertKind::OpenAbove:
					openLine(ed, session.kind == InsertKind::OpenBelow);
					if (ed.cursor.col > 0) noteAutoIndent(session, ed);
					break;
				default:
					break;
			}

			session.start = ed.cursor;
		}

		static void insertAtCursor(Editor& ed, const std::string& text) {
			ed.cursor = insertText(ed, ed.cursor, text);
		}

		static void overwriteAtCursor(Editor& ed, InsertSession& session, const std::string& text) {
			const std::string& line = ed.lines[ed.cursor.row];
			const std::size_t end = nextCharStart(line, ed.cursor.col);
			Overwrite step;
			step.typed = text;
			step.replaced = line.substr(ed.cursor.col, end - ed.cursor.col);
			session.overwrites.push_back(step);

			setLine(ed, ed.cursor.row, line.substr(0, ed.cursor.col) + text + line.substr(end));
			ed.cursor.col += text.size();
		}

		static void typeText(Editor& ed, InsertSession& session, const std::string& text) {
			if (text.empty()) return;

			if (isReplacing(ed)) overwriteAtCursor(ed, session, text);
			else insertAtCursor(ed, text);
			session.typed += text;
			if (ed.cursor.row == session.indent_row) session.indent_row = kNoMark;
		}

		static bool onlyAutoIndent(const Editor& ed, const InsertSession& session,
		                           std::size_t row) {
			if (row != session.indent_row) return false;

			const std::string& line = ed.lines[row];
			return line.size() == session.indent_length
				&& leadingIndent(line).size() == line.size();
		}

		static void breakLine(Editor& ed, InsertSession& session) {
			const std::size_t row = ed.cursor.row;
			const std::string& line = ed.lines[row];
			std::string head = line.substr(0, ed.cursor.col);
			const std::string tail = line.substr(ed.cursor.col);
			const std::string indent = ed.options.autoindent ? leadingIndent(head) : std::string();
			if (onlyAutoIndent(ed, session, row)) head.clear();

			replaceLines(ed, row, 1, { head, indent + tail });
			ed.cursor = { row + 1, indent.size() };
			session.typed += "\n";
			session.overwrites.clear();
			if (!indent.empty()) noteAutoIndent(session, ed);
		}

		static void popTyped(InsertSession& session) {
			std::string& typed = session.typed;
			while (!typed.empty() && (static_cast<unsigned char>(typed.back()) & 0xC0) == 0x80) {
				typed.pop_back();
			}

			if (!typed.empty()) typed.pop_back();
		}

		static void joinWithPrevious(Editor& ed) {
			const std::size_t row = ed.cursor.row;
			const std::string previous = ed.lines[row - 1];
			replaceLines(ed, row - 1, 2, { previous + ed.lines[row] });
			ed.cursor = { row - 1, previous.size() };
		}

		static void undoOverwrite(Editor& ed, InsertSession& session) {
			const Overwrite step = session.overwrites.back();
			session.overwrites.pop_back();
			const std::string& line = ed.lines[ed.cursor.row];
			const std::size_t start = ed.cursor.col - step.typed.size();
			const std::string tail = line.substr(ed.cursor.col);
			setLine(ed, ed.cursor.row, line.substr(0, start) + step.replaced + tail);
			ed.cursor.col = start;
		}

		// Replace mode's Backspace brings back what was typed over.
		static void backspaceOverwrite(Editor& ed, InsertSession& session) {
			if (!session.overwrites.empty()) {
				undoOverwrite(ed, session);
				return;
			}

			const std::string& line = ed.lines[ed.cursor.row];
			ed.cursor.col = previousCharStart(line, ed.cursor.col);
		}

		static void backspace(Editor& ed, InsertSession& session) {
			popTyped(session);
			if (isReplacing(ed)) {
				backspaceOverwrite(ed, session);
				return;
			}

			if (ed.cursor.col == 0) {
				if (ed.cursor.row > 0) joinWithPrevious(ed);
				return;
			}

			const std::string& line = ed.lines[ed.cursor.row];
			const Position start = { ed.cursor.row, previousCharStart(line, ed.cursor.col) };
			deleteBetween(ed, start, ed.cursor);
			ed.cursor = start;
		}

		static void deleteForward(Editor& ed) {
			const std::string& line = ed.lines[ed.cursor.row];
			if (ed.cursor.col < line.size()) {
				deleteBetween(ed, ed.cursor, { ed.cursor.row, nextCharStart(line, ed.cursor.col) });
				return;
			}

			if (ed.cursor.row + 1 >= ed.lines.size()) return;
			replaceLines(ed, ed.cursor.row, 2, { line + ed.lines[ed.cursor.row + 1] });
		}

		static void eraseBefore(Editor& ed, InsertSession& session, std::size_t from) {
			if (from >= ed.cursor.col) return;

			const std::size_t removed = ed.cursor.col - from;
			const std::size_t typed = session.typed.size();
			session.typed.resize(typed > removed ? typed - removed : 0);
			const Position start = { ed.cursor.row, from };
			deleteBetween(ed, start, ed.cursor);
			ed.cursor = start;
		}

		static void eraseWord(Editor& ed, InsertSession& session) {
			if (ed.cursor.col == 0) {
				backspace(ed, session);
				return;
			}

			const std::string& line = ed.lines[ed.cursor.row];
			std::size_t from = ed.cursor.col;
			while (from > 0 && isBlankChar(line, previousCharStart(line, from))) {
				from = previousCharStart(line, from);
			}

			const int kind = from > 0 ? charClass(line, previousCharStart(line, from), false) : 0;
			while (from > 0 && charClass(line, previousCharStart(line, from), false) == kind) {
				from = previousCharStart(line, from);
			}

			eraseBefore(ed, session, from);
		}

		static void eraseLine(Editor& ed, InsertSession& session) {
			const bool after_start = ed.cursor.row == session.start.row
				&& ed.cursor.col > session.start.col;
			eraseBefore(ed, session, after_start ? session.start.col : 0);
		}

		static void typeTab(Editor& ed, InsertSession& session) {
			if (!ed.options.expandtab) {
				typeText(ed, session, "\t");
				return;
			}

			const std::size_t column = displayColumn(ed, ed.cursor.row, ed.cursor.col);
			const std::size_t spaces = ed.options.tabstop - column % ed.options.tabstop;
			typeText(ed, session, std::string(spaces, ' '));
		}

		static void shiftCurrentLine(Editor& ed, bool right) {
			const std::string& line = ed.lines[ed.cursor.row];
			const std::string indent = leadingIndent(line);
			const std::size_t width = indentWidth(ed, line);
			const std::size_t step = ed.options.shiftwidth;
			const std::size_t wanted = right ? width + step : (width > step ? width - step : 0);
			const std::string fresh = indentOfWidth(ed, wanted);
			const std::size_t offset = ed.cursor.col > indent.size()
				? ed.cursor.col - indent.size()
				: 0;

			setLine(ed, ed.cursor.row, fresh + line.substr(indent.size()));
			ed.cursor.col = fresh.size() + offset;
		}

		static void insertLiteral(Editor& ed, InsertSession& session) {
			Key key = 0;
			if (nextKey(ed, key)) typeText(ed, session, keyText(key));
		}

		static void insertRegister(Editor& ed, InsertSession& session) {
			Key name = 0;
			Register value;
			if (!nextKey(ed, name) || name >= 0x80) return;
			if (fetchRegister(ed, static_cast<char>(name), value)) {
				typeText(ed, session, value.text);
			}
		}

		static void restartAt(Editor& ed, InsertSession& session) {
			session.typed.clear();
			session.overwrites.clear();
			session.start = ed.cursor;
		}

		static void moveVertically(Editor& ed, bool down) {
			const std::size_t row = ed.cursor.row;
			if ((down && row + 1 >= ed.lines.size()) || (!down && row == 0)) return;

			const std::size_t column = displayColumn(ed, row, ed.cursor.col);
			ed.cursor.row = down ? row + 1 : row - 1;
			ed.cursor.col = byteAtColumn(ed, ed.cursor.row, column);
		}

		static bool moveInInsert(Editor& ed, InsertSession& session, Key key) {
			const std::string& line = ed.lines[ed.cursor.row];
			switch (key) {
				case kKeyLeft:
					ed.cursor.col = previousCharStart(line, ed.cursor.col);
					break;
				case kKeyRight:
					ed.cursor.col = nextCharStart(line, ed.cursor.col);
					break;
				case kKeyHome:
					ed.cursor.col = 0;
					break;
				case kKeyEnd:
					ed.cursor.col = line.size();
					break;
				case kKeyUp:
				case kKeyDown:
					moveVertically(ed, key == kKeyDown);
					break;
				default:
					return false;
			}

			restartAt(ed, session);
			return true;
		}

		static bool controlKeys(Editor& ed, InsertSession& session, Key key) {
			switch (key) {
				case kKeyEnter:
					breakLine(ed, session);
					return true;
				case kKeyBackspace:
					backspace(ed, session);
					return true;
				case kKeyDelete:
					deleteForward(ed);
					return true;
				case kKeyTab:
					typeTab(ed, session);
					return true;
				case ctrlKey('W'):
					eraseWord(ed, session);
					return true;
				case ctrlKey('U'):
					eraseLine(ed, session);
					return true;
				case ctrlKey('V'):
				case ctrlKey('Q'):
					insertLiteral(ed, session);
					return true;
				case ctrlKey('T'):
				case ctrlKey('D'):
					shiftCurrentLine(ed, key == ctrlKey('T'));
					return true;
				case ctrlKey('R'):
					insertRegister(ed, session);
					return true;
				default:
					return moveInInsert(ed, session, key);
			}
		}

		static bool isTypable(Key key) {
			return (key >= 0x20 && key < 0x110000 && key != 0x7F) || isRawByteKey(key);
		}

		// False once the key ends the insert.
		static bool handleInsertKey(Editor& ed, InsertSession& session, Key key) {
			if (key == kKeyEscape || key == ctrlKey('C')) return false;
			if (controlKeys(ed, session, key)) return true;
			if (isTypable(key)) typeText(ed, session, keyText(key));
			return true;
		}

		static void repeatTyped(Editor& ed, InsertSession& session, long count) {
			const std::string typed = session.typed;
			for (long k = 1; k < count && !typed.empty(); ++k) {
				const bool opens = session.kind == InsertKind::OpenBelow
					|| session.kind == InsertKind::OpenAbove;
				if (opens) openLine(ed, true);

				if (!isReplacing(ed)) {
					insertAtCursor(ed, typed);
					continue;
				}

				for (std::size_t at = 0; at < typed.size(); at = nextCharStart(typed, at)) {
					overwriteAtCursor(ed, session, typed.substr(at, nextCharStart(typed, at) - at));
				}
			}
		}

		static void finishInsert(Editor& ed, InsertSession& session, long count) {
			repeatTyped(ed, session, count);
			if (onlyAutoIndent(ed, session, ed.cursor.row)) {
				setLine(ed, ed.cursor.row, std::string());
				ed.cursor.col = 0;
			}

			ed.mode = Mode::Normal;
			if (ed.cursor.col > 0) {
				ed.cursor.col = previousCharStart(ed.lines[ed.cursor.row], ed.cursor.col);
			}

			clampCursor(ed);
			updateWantColumn(ed);
		}

		void runInsertMode(Editor& ed, InsertKind kind, long count) {
			InsertSession session;
			session.kind = kind;
			prepareInsert(ed, session);

			Key key = 0;
			bool typing = nextKey(ed, key);
			while (typing) typing = handleInsertKey(ed, session, key) && nextKey(ed, key);

			finishInsert(ed, session, count);
		}

	}  // namespace vi_detail

}  // namespace wbsh

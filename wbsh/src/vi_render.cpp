/**
 * @file vi_render.cpp
 * @brief vi's screen: how lines wrap into cells (tabs, control
 *        characters, wide glyphs), keeping the cursor in view, and
 *        drawing a frame with VT sequences.
 */

#include "vi_internal.h"

#include <cstdio>
#include <string>
#include <vector>

#include "textcells.h"
#include "utf8.h"

namespace wbsh {

	namespace vi_detail {

		static const std::size_t kMinimumNumberDigits = 3;
		static const std::size_t kRulerWidth = 18;

		struct CellSpot {
			std::size_t row = 0;
			std::size_t x = 0;
			std::size_t width = 1;
		};

		std::size_t textRows(const Editor& ed) {
			return ed.term.rows > 1 ? ed.term.rows - 1 : 1;
		}

		static std::size_t digitCount(std::size_t value) {
			std::size_t digits = 1;
			for (; value >= 10; value /= 10) ++digits;
			return digits;
		}

		static std::size_t gutterWidth(const Editor& ed) {
			if (!ed.options.number) return 0;

			const std::size_t digits = digitCount(ed.lines.size());
			const std::size_t shown = digits > kMinimumNumberDigits ? digits : kMinimumNumberDigits;
			const std::size_t width = shown + 1;
			return width < ed.term.columns ? width : 0;
		}

		static std::size_t textWidth(const Editor& ed) {
			const std::size_t gutter = gutterWidth(ed);
			return ed.term.columns > gutter ? ed.term.columns - gutter : 1;
		}

		static bool isStrayByte(const std::string& line, std::size_t pos) {
			if (static_cast<unsigned char>(line[pos]) < 0x80) return false;

			const char32_t code = decodeUtf8(line, pos).code;
			return code >= 0xDC80 && code <= 0xDCFF;
		}

		static bool isControlByte(unsigned char byte) {
			return byte < 0x20 || byte == 0x7F;
		}

		static std::size_t cellWidthAt(const Editor& ed, const std::string& line, std::size_t pos,
		                               std::size_t column) {
			const unsigned char byte = static_cast<unsigned char>(line[pos]);
			if (byte == '\t' && !ed.options.list) {
				return ed.options.tabstop - column % ed.options.tabstop;
			}

			if (isControlByte(byte)) return 2;
			if (isStrayByte(line, pos)) return 4;
			return clusterAt(line, pos).width;
		}

		// Where the character at @p target starts on the wrapped line, or
		// where the cursor goes after the last character.
		static CellSpot locate(const Editor& ed, const std::string& line, std::size_t target) {
			const std::size_t width = textWidth(ed);
			CellSpot spot;
			std::size_t column = 0;
			for (std::size_t pos = 0; pos < line.size(); pos = nextCharStart(line, pos)) {
				const std::size_t cells = cellWidthAt(ed, line, pos, column);
				if (spot.x + cells > width && spot.x > 0) {
					++spot.row;
					spot.x = 0;
				}

				if (pos >= target) {
					spot.width = cells;
					return spot;
				}

				spot.x += cells;
				column += cells;
			}

			if (spot.x >= width) {
				++spot.row;
				spot.x = 0;
			}

			spot.width = 1;
			return spot;
		}

		std::size_t displayColumn(const Editor& ed, std::size_t row, std::size_t col) {
			const std::string& line = ed.lines[row];
			std::size_t column = 0;
			const std::size_t end = col < line.size() ? col : line.size();
			for (std::size_t pos = 0; pos < end; pos = nextCharStart(line, pos)) {
				column += cellWidthAt(ed, line, pos, column);
			}

			return column;
		}

		std::size_t byteAtColumn(const Editor& ed, std::size_t row, std::size_t column) {
			const std::string& line = ed.lines[row];
			std::size_t start = 0;
			for (std::size_t pos = 0; pos < line.size(); pos = nextCharStart(line, pos)) {
				const std::size_t cells = cellWidthAt(ed, line, pos, start);
				if (column < start + cells) return pos;
				start += cells;
			}

			return line.size();
		}

		static std::size_t lineScreenRows(const Editor& ed, std::size_t row) {
			const CellSpot end = locate(ed, ed.lines[row], std::string::npos);
			if (end.x == 0 && end.row > 0) return end.row;
			return end.row + 1;
		}

		static CellSpot cursorSpot(const Editor& ed) {
			return locate(ed, ed.lines[ed.cursor.row], ed.cursor.col);
		}

		static std::size_t rowsOf(const Editor& ed, std::size_t row) {
			const std::size_t rows = lineScreenRows(ed, row);
			if (row != ed.cursor.row) return rows;

			const std::size_t needed = cursorSpot(ed).row + 1;
			return needed > rows ? needed : rows;
		}

		static std::size_t visibleRowsOf(const Editor& ed, std::size_t row) {
			const std::size_t rows = rowsOf(ed, row);
			if (row != ed.top) return rows;
			return rows > ed.top_skip ? rows - ed.top_skip : 0;
		}

		static bool cursorFits(const Editor& ed, const CellSpot& spot) {
			std::size_t used = 0;
			for (std::size_t row = ed.top; row < ed.cursor.row; ++row) {
				used += visibleRowsOf(ed, row);
				if (used >= textRows(ed)) return false;
			}

			const std::size_t skip = ed.cursor.row == ed.top ? ed.top_skip : 0;
			return spot.row >= skip && used + spot.row - skip + 1 <= textRows(ed);
		}

		// The topmost start that still shows the cursor within @p limit rows.
		static void placeCursorWithin(Editor& ed, const CellSpot& spot, std::size_t limit) {
			std::size_t needed = spot.row + 1;
			std::size_t row = ed.cursor.row;
			while (row > 0) {
				const std::size_t rows = rowsOf(ed, row - 1);
				if (needed + rows > limit) break;
				needed += rows;
				--row;
			}

			ed.top = row;
			ed.top_skip = needed > textRows(ed) ? needed - textRows(ed) : 0;
		}

		// After a long jump near the end, no screen rows are left to `~`.
		static void fillScreenBottom(Editor& ed) {
			std::size_t used = 0;
			for (std::size_t row = ed.top; row < ed.lines.size() && used <= textRows(ed); ++row) {
				used += visibleRowsOf(ed, row);
			}

			if (ed.top_skip > 0) return;

			while (ed.top > 0 && used + rowsOf(ed, ed.top - 1) <= textRows(ed)) {
				--ed.top;
				used += rowsOf(ed, ed.top);
			}
		}

		void scrollToCursor(Editor& ed) {
			if (ed.top >= ed.lines.size()) ed.top = ed.lines.size() - 1;

			const CellSpot spot = cursorSpot(ed);
			const std::size_t rows = textRows(ed);
			const bool far_above = ed.cursor.row + rows < ed.top;
			const bool far_below = ed.cursor.row > ed.top + 2 * rows;
			if (far_above || far_below) {
				placeCursorWithin(ed, spot, rows / 2 + 1);
				fillScreenBottom(ed);
				return;
			}

			const bool above = ed.cursor.row < ed.top
				|| (ed.cursor.row == ed.top && spot.row < ed.top_skip);
			if (above) {
				ed.top = ed.cursor.row;
				ed.top_skip = spot.row + 1 > rows ? spot.row + 1 - rows : 0;
				return;
			}

			if (!cursorFits(ed, spot)) placeCursorWithin(ed, spot, rows);
		}

		std::size_t lastVisibleLine(const Editor& ed) {
			std::size_t used = 0;
			std::size_t last = ed.top;
			for (std::size_t row = ed.top; row < ed.lines.size(); ++row) {
				used += visibleRowsOf(ed, row);
				if (used > textRows(ed) && row > ed.top) break;
				last = row;
			}

			return last;
		}

		static std::string hexByte(unsigned char byte) {
			char text[8];
			std::snprintf(text, sizeof(text), "<%02x>", static_cast<unsigned int>(byte));
			return text;
		}

		static std::string cellText(const Editor& ed, const std::string& line, std::size_t pos,
		                            std::size_t cells) {
			const unsigned char byte = static_cast<unsigned char>(line[pos]);
			if (byte == '\t' && !ed.options.list) return std::string(cells, ' ');
			if (isControlByte(byte)) {
				const char shown = static_cast<char>(byte == 0x7F ? '?' : byte + '@');
				return std::string("\x1b[94m^") + shown + "\x1b[39m";
			}

			if (isStrayByte(line, pos)) return "\x1b[94m" + hexByte(byte) + "\x1b[39m";
			return line.substr(pos, nextCharStart(line, pos) - pos);
		}

		struct ScreenRow {
			std::string text;
			std::size_t cells = 0;
		};

		static std::vector<ScreenRow> wrapLine(const Editor& ed, const std::string& line) {
			const std::size_t width = textWidth(ed);
			std::vector<ScreenRow> rows(1);
			std::size_t column = 0;
			for (std::size_t pos = 0; pos < line.size(); pos = nextCharStart(line, pos)) {
				const std::size_t cells = cellWidthAt(ed, line, pos, column);
				if (rows.back().cells + cells > width && rows.back().cells > 0) rows.emplace_back();

				rows.back().text += cellText(ed, line, pos, cells);
				rows.back().cells += cells;
				column += cells;
			}

			if (ed.options.list && rows.back().cells < width) {
				rows.back().text += "\x1b[94m$\x1b[39m";
				rows.back().cells += 1;
			}

			return rows;
		}

		static std::string gutterText(const Editor& ed, std::size_t row, bool first) {
			const std::size_t width = gutterWidth(ed);
			if (width == 0) return std::string();
			if (!first) return std::string(width, ' ');

			char number[32];
			std::snprintf(number, sizeof(number), "%*zu ", static_cast<int>(width - 1), row + 1);
			return std::string("\x1b[33m") + number + "\x1b[39m";
		}

		static void appendScreenRow(const Editor& ed, std::size_t screen_row,
		                            const std::string& text, std::size_t cells,
		                            std::string& frame) {
			frame += "\x1b[" + std::to_string(screen_row + 1) + ";1H" + text;
			if (cells < ed.term.columns) frame += "\x1b[K";
		}

		static void appendTextRows(const Editor& ed, std::string& frame) {
			std::size_t screen_row = 0;
			std::size_t skip = ed.top_skip;
			for (std::size_t row = ed.top; screen_row < textRows(ed); ++row) {
				if (row >= ed.lines.size()) {
					appendScreenRow(ed, screen_row++, "\x1b[94m~\x1b[39m", 1, frame);
					continue;
				}

				const std::vector<ScreenRow> pieces = wrapLine(ed, ed.lines[row]);
				for (std::size_t k = skip; k < pieces.size() && screen_row < textRows(ed); ++k) {
					const std::string text = gutterText(ed, row, k == 0) + pieces[k].text;
					const std::size_t cells = gutterWidth(ed) + pieces[k].cells;
					appendScreenRow(ed, screen_row++, text, cells, frame);
				}

				skip = 0;
			}
		}

		static std::string fitToWidth(const std::string& text, std::size_t width) {
			std::size_t end = 0;
			std::size_t used = 0;
			while (end < text.size()) {
				const TextCluster cluster = clusterAt(text, end);
				if (used + cluster.width > width) break;
				used += cluster.width;
				end = cluster.end;
			}

			return text.substr(0, end);
		}

		static std::string tailToWidth(const std::string& text, std::size_t width) {
			std::size_t start = 0;
			while (start < text.size() && displayWidth(text.substr(start), text.size()) > width) {
				start = nextClusterEnd(text, start);
			}

			return text.substr(start);
		}

		static std::string modeText(const Editor& ed) {
			if (ed.mode == Mode::Insert) return "\x1b[1m-- INSERT --\x1b[22m";
			if (ed.mode == Mode::Replace) return "\x1b[1m-- REPLACE --\x1b[22m";
			return std::string();
		}

		static std::string rulerText(const Editor& ed) {
			const std::size_t column = displayColumn(ed, ed.cursor.row, ed.cursor.col) + 1;
			return std::to_string(ed.cursor.row + 1) + "," + std::to_string(column);
		}

		static std::string statusLeft(const Editor& ed, std::size_t width) {
			if (ed.command_active) return tailToWidth(ed.command_line, width);
			if (ed.message.empty()) return modeText(ed);

			const std::string shown = fitToWidth(ed.message, width);
			if (ed.message_error) return "\x1b[97;41m" + shown + "\x1b[0m";
			return shown;
		}

		static void appendStatusLine(const Editor& ed, std::string& frame) {
			const std::size_t columns = ed.term.columns;
			frame += "\x1b[" + std::to_string(ed.term.rows) + ";1H\x1b[K";
			frame += statusLeft(ed, columns > 1 ? columns - 1 : 1);
			if (ed.command_active || columns <= kRulerWidth * 2) return;

			const bool covered = !ed.message.empty() && displayWidth(ed.message, ed.message.size())
				>= columns - kRulerWidth;
			if (covered) return;

			frame += "\x1b[" + std::to_string(ed.term.rows) + ";"
				+ std::to_string(columns - kRulerWidth + 1) + "H";
			frame += rulerText(ed);
		}

		static std::size_t cursorScreenRow(const Editor& ed, const CellSpot& spot) {
			std::size_t used = 0;
			for (std::size_t row = ed.top; row < ed.cursor.row; ++row) {
				used += visibleRowsOf(ed, row);
			}

			const std::size_t skip = ed.cursor.row == ed.top ? ed.top_skip : 0;
			return used + spot.row - skip;
		}

		static std::string cursorShape(const Editor& ed) {
			if (ed.mode == Mode::Insert || ed.command_active) return "\x1b[5 q";
			if (ed.mode == Mode::Replace) return "\x1b[3 q";
			return "\x1b[2 q";
		}

		static void appendCursor(const Editor& ed, std::string& frame) {
			std::size_t row = ed.term.rows - 1;
			std::size_t column = 0;
			if (ed.command_active) {
				const std::string shown = tailToWidth(ed.command_line, ed.term.columns - 1);
				column = displayWidth(shown, std::string::npos);
			} else {
				const CellSpot spot = cursorSpot(ed);
				const bool on_tab = ed.mode == Mode::Normal
					&& ed.cursor.col < ed.lines[ed.cursor.row].size()
					&& ed.lines[ed.cursor.row][ed.cursor.col] == '\t' && !ed.options.list;
				row = cursorScreenRow(ed, spot);
				column = gutterWidth(ed) + spot.x + (on_tab ? spot.width - 1 : 0);
			}

			frame += cursorShape(ed);
			frame += "\x1b[" + std::to_string(row + 1) + ";" + std::to_string(column + 1)
				+ "H\x1b[?25h";
		}

		void renderScreen(Editor& ed) {
			if (ed.term.headless || !ed.term.screen_active) return;

			refreshTerminalSize(ed);
			clampCursor(ed);
			scrollToCursor(ed);

			std::string frame = "\x1b[?25l";
			appendTextRows(ed, frame);
			appendStatusLine(ed, frame);
			appendCursor(ed, frame);
			writeTerminal(ed, frame);
		}

	}  // namespace vi_detail

}  // namespace wbsh

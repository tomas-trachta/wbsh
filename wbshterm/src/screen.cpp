/**
 * @file screen.cpp
 * @brief Grid mutation: printing, cursor motion, erasing, scrolling, SGR.
 */

#include "screen.h"

#include <algorithm>
#include <string>

namespace wbshterm {

	static const int kTabWidth = 8;

	static const std::uint32_t kAnsiPalette[16] = {
		0x000000, 0xCD3131, 0x0DBC79, 0xE5E510, 0x2472C8, 0xBC3FBC, 0x11A8CD, 0xE5E5E5,
		0x666666, 0xF14C4C, 0x23D18B, 0xF5F543, 0x3B8EEA, 0xD670D6, 0x29B8DB, 0xFFFFFF,
	};

	static std::uint32_t indexedColor(int index) {
		if (index < 0) return kDefaultColor;
		if (index < 16) return kAnsiPalette[index];

		if (index < 232) {
			const int offset = index - 16;
			const int steps[6] = { 0, 95, 135, 175, 215, 255 };
			const std::uint32_t red   = static_cast<std::uint32_t>(steps[(offset / 36) % 6]);
			const std::uint32_t green = static_cast<std::uint32_t>(steps[(offset / 6) % 6]);
			const std::uint32_t blue  = static_cast<std::uint32_t>(steps[offset % 6]);
			return (red << 16) | (green << 8) | blue;
		}

		if (index < 256) {
			const std::uint32_t level = static_cast<std::uint32_t>(8 + (index - 232) * 10);
			return (level << 16) | (level << 8) | level;
		}

		return kDefaultColor;
	}

	Screen::Screen(int columns, int rows) {
		resize(columns, rows);
	}

	void Screen::resize(int columns, int rows) {
		columns_ = std::max(1, columns);
		rows_    = std::max(1, rows);

		cells_.assign(static_cast<std::size_t>(columns_) * static_cast<std::size_t>(rows_), Cell());
		dirty_.assign(static_cast<std::size_t>(rows_), true);

		scroll_top_    = 0;
		scroll_bottom_ = rows_ - 1;
		cursor_.row    = std::min(cursor_.row, rows_ - 1);
		cursor_.column = std::min(cursor_.column, columns_ - 1);
		wrap_pending_  = false;
	}

	Cell& Screen::at(int row, int column) {
		const std::size_t index = static_cast<std::size_t>(row) * static_cast<std::size_t>(columns_)
			+ static_cast<std::size_t>(column);
		return cells_[index];
	}

	const Cell& Screen::cell(int row, int column) const {
		const std::size_t index = static_cast<std::size_t>(row) * static_cast<std::size_t>(columns_)
			+ static_cast<std::size_t>(column);
		return cells_[index];
	}

	bool Screen::rowDirty(int row) const {
		return row >= 0 && row < rows_ && dirty_[static_cast<std::size_t>(row)];
	}

	void Screen::clearDirty() {
		dirty_.assign(static_cast<std::size_t>(rows_), false);
	}

	void Screen::markDirty(int row) {
		if (row >= 0 && row < rows_) dirty_[static_cast<std::size_t>(row)] = true;
	}

	void Screen::markAllDirty() {
		dirty_.assign(static_cast<std::size_t>(rows_), true);
	}

	std::string Screen::toText() const {
		std::string text;
		for (int row = 0; row < rows_; ++row) {
			int last = -1;
			for (int column = 0; column < columns_; ++column) {
				if (cell(row, column).code != U' ') last = column;
			}

			for (int column = 0; column <= last; ++column) {
				const char32_t code = cell(row, column).code;
				text.push_back(code < 0x80 ? static_cast<char>(code) : '?');
			}

			text.push_back('\n');
		}

		return text;
	}

	void Screen::vtPrint(char32_t code) {
		writeChar(code);
	}

	void Screen::writeChar(char32_t code) {
		if (wrap_pending_) {
			carriageReturn();
			lineFeed();
			wrap_pending_ = false;
		}

		Cell& target = at(cursor_.row, cursor_.column);
		target = pen_;
		target.code = code;
		markDirty(cursor_.row);

		advanceCursor();
	}

	void Screen::advanceCursor() {
		if (cursor_.column + 1 >= columns_) {
			wrap_pending_ = true;
			return;
		}

		++cursor_.column;
	}

	void Screen::vtExecute(unsigned char control) {
		switch (control) {
		case 0x07: break;
		case 0x08: backspace(); break;
		case 0x09: tab(); break;
		case 0x0A:
		case 0x0B:
		case 0x0C: lineFeed(); break;
		case 0x0D: carriageReturn(); break;
		default:   break;
		}
	}

	void Screen::carriageReturn() {
		cursor_.column = 0;
		wrap_pending_ = false;
	}

	void Screen::lineFeed() {
		wrap_pending_ = false;
		if (cursor_.row == scroll_bottom_) {
			scrollUp(1);
			return;
		}

		if (cursor_.row + 1 < rows_) ++cursor_.row;
	}

	void Screen::backspace() {
		wrap_pending_ = false;
		if (cursor_.column > 0) --cursor_.column;
	}

	void Screen::tab() {
		wrap_pending_ = false;
		const int next = ((cursor_.column / kTabWidth) + 1) * kTabWidth;
		cursor_.column = std::min(next, columns_ - 1);
	}

	void Screen::clearRow(int row, int from_column, int to_column) {
		const int first = std::max(0, from_column);
		const int last  = std::min(columns_ - 1, to_column);

		for (int column = first; column <= last; ++column) {
			Cell& target = at(row, column);
			target = Cell();
			target.background = pen_.background;
		}

		markDirty(row);
	}

	void Screen::scrollUp(int count) {
		const int lines = std::max(1, count);
		for (int step = 0; step < lines; ++step) {
			for (int row = scroll_top_; row < scroll_bottom_; ++row) {
				for (int column = 0; column < columns_; ++column) {
					at(row, column) = cell(row + 1, column);
				}

				markDirty(row);
			}

			clearRow(scroll_bottom_, 0, columns_ - 1);
		}
	}

	void Screen::scrollDown(int count) {
		const int lines = std::max(1, count);
		for (int step = 0; step < lines; ++step) {
			for (int row = scroll_bottom_; row > scroll_top_; --row) {
				for (int column = 0; column < columns_; ++column) {
					at(row, column) = cell(row - 1, column);
				}

				markDirty(row);
			}

			clearRow(scroll_top_, 0, columns_ - 1);
		}
	}

	void Screen::moveCursor(int row, int column) {
		cursor_.row    = std::min(std::max(row, 0), rows_ - 1);
		cursor_.column = std::min(std::max(column, 0), columns_ - 1);
		wrap_pending_  = false;
	}

	void Screen::moveCursorBy(int rows, int columns) {
		moveCursor(cursor_.row + rows, cursor_.column + columns);
	}

	void Screen::vtEsc(const VtSequence& sequence) {
		switch (sequence.final_byte) {
		case '7': saved_cursor_ = cursor_; break;
		case '8': moveCursor(saved_cursor_.row, saved_cursor_.column); break;
		case 'D': lineFeed(); break;
		case 'E': carriageReturn(); lineFeed(); break;
		case 'M':
			if (cursor_.row == scroll_top_) scrollDown(1);
			else moveCursorBy(-1, 0);
			break;
		case 'c':
			pen_ = Cell();
			resize(columns_, rows_);
			moveCursor(0, 0);
			break;
		default: break;
		}
	}

	void Screen::vtOsc(const std::string& text) {
		const std::size_t separator = text.find(';');
		if (separator == std::string::npos) return;

		const std::string code = text.substr(0, separator);
		if (code == "0" || code == "2") title_ = text.substr(separator + 1);
	}

	// vim and friends print a probe glyph, ask where the cursor ended up,
	// and only tidy up once answered; an unanswered query leaves that probe
	// on screen.
	void Screen::answerDeviceAttributes(bool secondary) {
		if (responder_ == nullptr) return;
		responder_->vtRespond(secondary ? "\x1b[>0;10;1c" : "\x1b[?1;2c");
	}

	void Screen::answerStatusReport(const VtSequence& sequence) {
		if (responder_ == nullptr) return;

		if (sequence.param(0, 0) == 5) {
			responder_->vtRespond("\x1b[0n");
			return;
		}

		if (sequence.param(0, 0) != 6) return;

		std::string reply = "\x1b[";
		reply += std::to_string(cursor_.row + 1);
		reply += ";";
		reply += std::to_string(cursor_.column + 1);
		reply += "R";
		responder_->vtRespond(reply);
	}

	void Screen::vtCsi(const VtSequence& sequence) {
		if (sequence.private_byte == '?') {
			if (sequence.final_byte == 'h') applyPrivateMode(sequence, true);
			if (sequence.final_byte == 'l') applyPrivateMode(sequence, false);
			return;
		}

		if (sequence.private_byte != 0) {
			if (sequence.final_byte == 'c') answerDeviceAttributes(true);
			return;
		}

		switch (sequence.final_byte) {
		case 'A': moveCursorBy(-sequence.param(0, 1), 0); break;
		case 'B': moveCursorBy(sequence.param(0, 1), 0); break;
		case 'C': moveCursorBy(0, sequence.param(0, 1)); break;
		case 'D': moveCursorBy(0, -sequence.param(0, 1)); break;
		case 'E': moveCursor(cursor_.row + sequence.param(0, 1), 0); break;
		case 'F': moveCursor(cursor_.row - sequence.param(0, 1), 0); break;
		case 'G': moveCursor(cursor_.row, sequence.param(0, 1) - 1); break;
		case 'H':
		case 'f': moveCursor(sequence.param(0, 1) - 1, sequence.param(1, 1) - 1); break;
		case 'J': applyEraseInDisplay(sequence); break;
		case 'K': applyEraseInLine(sequence); break;
		case 'S': scrollUp(sequence.param(0, 1)); break;
		case 'T': scrollDown(sequence.param(0, 1)); break;
		case '@':
		case 'P':
		case 'L':
		case 'M':
		case 'X': applyInsertDelete(sequence); break;
		case 'd': moveCursor(sequence.param(0, 1) - 1, cursor_.column); break;
		case 'c': answerDeviceAttributes(false); break;
		case 'm': applySgr(sequence); break;
		case 'n': answerStatusReport(sequence); break;
		case 'r': applyScrollRegion(sequence); break;
		case 's': saved_cursor_ = cursor_; break;
		case 'u': moveCursor(saved_cursor_.row, saved_cursor_.column); break;
		default:  break;
		}
	}

	void Screen::applyScrollRegion(const VtSequence& sequence) {
		const int top    = sequence.param(0, 1) - 1;
		const int bottom = sequence.param(1, rows_) - 1;
		if (top < 0 || bottom >= rows_ || top >= bottom) return;

		scroll_top_    = top;
		scroll_bottom_ = bottom;
		moveCursor(0, 0);
	}

	void Screen::applyEraseInDisplay(const VtSequence& sequence) {
		switch (sequence.param(0, 0)) {
		case 0:
			clearRow(cursor_.row, cursor_.column, columns_ - 1);
			for (int row = cursor_.row + 1; row < rows_; ++row) clearRow(row, 0, columns_ - 1);
			break;
		case 1:
			clearRow(cursor_.row, 0, cursor_.column);
			for (int row = 0; row < cursor_.row; ++row) clearRow(row, 0, columns_ - 1);
			break;
		case 2:
		case 3:
			for (int row = 0; row < rows_; ++row) clearRow(row, 0, columns_ - 1);
			break;
		default:
			break;
		}
	}

	void Screen::applyEraseInLine(const VtSequence& sequence) {
		switch (sequence.param(0, 0)) {
		case 0:  clearRow(cursor_.row, cursor_.column, columns_ - 1); break;
		case 1:  clearRow(cursor_.row, 0, cursor_.column); break;
		case 2:  clearRow(cursor_.row, 0, columns_ - 1); break;
		default: break;
		}
	}

	void Screen::applyInsertDelete(const VtSequence& sequence) {
		const int count = std::max(1, sequence.param(0, 1));

		switch (sequence.final_byte) {
		case '@':
			for (int column = columns_ - 1; column >= cursor_.column + count; --column) {
				at(cursor_.row, column) = cell(cursor_.row, column - count);
			}

			clearRow(cursor_.row, cursor_.column, cursor_.column + count - 1);
			break;
		case 'P':
			for (int column = cursor_.column; column < columns_; ++column) {
				at(cursor_.row, column) = column + count < columns_
					? cell(cursor_.row, column + count)
					: Cell();
			}

			markDirty(cursor_.row);
			break;
		case 'X':
			clearRow(cursor_.row, cursor_.column, cursor_.column + count - 1);
			break;
		case 'L':
		case 'M': {
			const int saved_top = scroll_top_;
			scroll_top_ = cursor_.row;
			if (sequence.final_byte == 'L') scrollDown(count);
			else scrollUp(count);
			scroll_top_ = saved_top;
			break;
		}
		default:
			break;
		}
	}

	void Screen::applyPrivateMode(const VtSequence& sequence, bool enable) {
		for (std::size_t i = 0; i < sequence.params.size(); ++i) {
			switch (sequence.param(i, 0)) {
			case 25: cursor_.visible = enable; break;
			default: break;
			}
		}
	}

	bool Screen::applyExtendedColor(const VtSequence& sequence, std::size_t& index,
			std::uint32_t& out_color) {
		const int kind = sequence.param(index + 1, 0);

		if (kind == 5) {
			out_color = indexedColor(sequence.param(index + 2, 0));
			index += 2;
			return true;
		}

		if (kind == 2) {
			const auto red   = static_cast<std::uint32_t>(sequence.param(index + 2, 0) & 0xFF);
			const auto green = static_cast<std::uint32_t>(sequence.param(index + 3, 0) & 0xFF);
			const auto blue  = static_cast<std::uint32_t>(sequence.param(index + 4, 0) & 0xFF);

			out_color = (red << 16) | (green << 8) | blue;
			index += 4;
			return true;
		}

		return false;
	}

	void Screen::applySgr(const VtSequence& sequence) {
		if (sequence.params.empty()) {
			pen_ = Cell();
			return;
		}

		for (std::size_t i = 0; i < sequence.params.size(); ++i) {
			const int code = sequence.param(i, 0);

			if (code == 38 && applyExtendedColor(sequence, i, pen_.foreground)) continue;
			if (code == 48 && applyExtendedColor(sequence, i, pen_.background)) continue;

			if (code >= 30 && code <= 37) {
				pen_.foreground = indexedColor(code - 30);
				continue;
			}

			if (code >= 40 && code <= 47) {
				pen_.background = indexedColor(code - 40);
				continue;
			}

			if (code >= 90 && code <= 97) {
				pen_.foreground = indexedColor(code - 90 + 8);
				continue;
			}

			if (code >= 100 && code <= 107) {
				pen_.background = indexedColor(code - 100 + 8);
				continue;
			}

			switch (code) {
			case 0:  pen_ = Cell(); break;
			case 1:  pen_.attributes |= kAttrBold; break;
			case 2:  pen_.attributes |= kAttrDim; break;
			case 3:  pen_.attributes |= kAttrItalic; break;
			case 4:  pen_.attributes |= kAttrUnderline; break;
			case 7:  pen_.attributes |= kAttrReverse; break;
			case 8:  pen_.attributes |= kAttrInvisible; break;
			case 22: pen_.attributes &= static_cast<std::uint16_t>(~(kAttrBold | kAttrDim)); break;
			case 23: pen_.attributes &= static_cast<std::uint16_t>(~kAttrItalic); break;
			case 24: pen_.attributes &= static_cast<std::uint16_t>(~kAttrUnderline); break;
			case 27: pen_.attributes &= static_cast<std::uint16_t>(~kAttrReverse); break;
			case 28: pen_.attributes &= static_cast<std::uint16_t>(~kAttrInvisible); break;
			case 39: pen_.foreground = kDefaultColor; break;
			case 49: pen_.background = kDefaultColor; break;
			default: break;
			}
		}
	}

} /* namespace wbshterm */

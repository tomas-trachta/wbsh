/**
 * @file screen.cpp
 * @brief Grid mutation: printing, cursor motion, erasing, scrolling, SGR.
 */

#include "screen.h"

#include "charwidth.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace wbshterm {

	static const int kTabWidth = 8;
	static const std::size_t kMaxCommandBlocks = 500;

	static const std::uint32_t kAnsiPalette[16] = {
		0x000000, 0xCD3131, 0x0DBC79, 0xE5E510, 0x2472C8, 0xBC3FBC, 0x11A8CD, 0xE5E5E5,
		0x666666, 0xF14C4C, 0x23D18B, 0xF5F543, 0x3B8EEA, 0xD670D6, 0x29B8DB, 0xFFFFFF,
	};

	static std::uint32_t indexedColor(int index) {
		if (index < 0) return kDefaultColor;
		if (index < 16) return kPaletteColor | static_cast<std::uint32_t>(index);

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
		const std::vector<Cell> old_cells = cells_;
		const int old_columns = columns_;
		const int old_rows    = rows_;

		columns_ = std::max(1, columns);
		rows_    = std::max(1, rows);

		cells_.assign(static_cast<std::size_t>(columns_) * static_cast<std::size_t>(rows_), Cell());
		dirty_.assign(static_cast<std::size_t>(rows_), true);

		scroll_top_    = 0;
		scroll_bottom_ = rows_ - 1;
		wrap_pending_  = false;

		carryContentForward(old_cells, old_columns, old_rows);

		cursor_.row    = std::min(cursor_.row, rows_ - 1);
		cursor_.column = std::min(cursor_.column, columns_ - 1);
	}

	void Screen::carryContentForward(const std::vector<Cell>& old_cells, int old_columns,
			int old_rows) {
		if (old_cells.empty() || old_columns <= 0 || old_rows <= 0) return;

		const int kept    = std::min(rows_, old_rows);
		const int carried = std::min(columns_, old_columns);

		for (int row = 0; row < old_rows - kept; ++row) {
			std::vector<Cell> line(
				old_cells.begin() + static_cast<std::ptrdiff_t>(row) * old_columns,
				old_cells.begin() + static_cast<std::ptrdiff_t>(row + 1) * old_columns);
			if (!alt_screen_) scrollback_.push_back(std::move(line));
		}

		trimScrollback();

		for (int row = 0; row < kept; ++row) {
			const int source = old_rows - kept + row;
			const int target = rows_ - kept + row;
			for (int column = 0; column < carried; ++column) {
				at(target, column) = old_cells[
					static_cast<std::size_t>(source) * static_cast<std::size_t>(old_columns)
					+ static_cast<std::size_t>(column)];
			}
		}

		cursor_.row += rows_ - old_rows;
	}

	void Screen::clearAll() {
		cells_.assign(cells_.size(), Cell());
		markAllDirty();
	}

	void Screen::clearScrollback() {
		const int dropped = scrollbackRows();
		scrollback_.clear();
		shiftBlocksUp(dropped);
	}

	void Screen::trimScrollback() {
		while (scrollback_.size() > scrollback_limit_) {
			scrollback_.pop_front();
			shiftBlocksUp(1);
		}
	}

	void Screen::setScrollbackLimit(int lines) {
		scrollback_limit_ = static_cast<std::size_t>(std::max(0, lines));
		trimScrollback();
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

	const Cell& Screen::cellAt(int absolute_row, int column) const {
		const int history = scrollbackRows();
		if (absolute_row >= history) return cell(absolute_row - history, column);

		static const Cell blank;
		const std::vector<Cell>& line = scrollback_[static_cast<std::size_t>(absolute_row)];
		const std::size_t index = static_cast<std::size_t>(column);
		return index < line.size() ? line[index] : blank;
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
		const int width = characterWidth(code);
		if (width == 0) return;

		if (wrap_pending_) {
			carriageReturn();
			lineFeed();
			wrap_pending_ = false;
		}

		if (width == 2 && cursor_.column + 1 >= columns_) {
			clearRow(cursor_.row, cursor_.column, columns_ - 1);
			carriageReturn();
			lineFeed();
		}

		Cell& target = at(cursor_.row, cursor_.column);
		target = pen_;
		target.code = code;
		if (width == 2) target.attributes |= kAttrWide;
		markDirty(cursor_.row);

		if (width == 2 && cursor_.column + 1 < columns_) {
			Cell& tail = at(cursor_.row, cursor_.column + 1);
			tail = pen_;
			tail.code = U' ';
			tail.attributes |= kAttrWideTail;
			++cursor_.column;
		}

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

	void Screen::pushToScrollback(int row) {
		if (alt_screen_) return;
		if (scroll_top_ != 0 || scroll_bottom_ != rows_ - 1) return;

		const std::size_t start =
			static_cast<std::size_t>(row) * static_cast<std::size_t>(columns_);
		scrollback_.emplace_back(cells_.begin() + static_cast<std::ptrdiff_t>(start),
			cells_.begin() + static_cast<std::ptrdiff_t>(start)
				+ static_cast<std::ptrdiff_t>(columns_));

		trimScrollback();
	}

	void Screen::scrollUp(int count) {
		const int lines = std::max(1, count);
		for (int step = 0; step < lines; ++step) {
			pushToScrollback(scroll_top_);

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
			clearAll();
			moveCursor(0, 0);
			break;
		default: break;
		}
	}

	static std::string percentDecode(const std::string& text);

	int Screen::currentAbsoluteRow() const {
		return scrollbackRows() + cursor_.row;
	}

	// Dropping scrollback, a line at a time when it is full or all of it
	// on a clear, moves every absolute row up; the blocks move with them.
	void Screen::shiftBlocksUp(int lines) {
		for (CommandBlock& block : blocks_) {
			block.prompt_row -= lines;
			if (block.output_row >= 0) block.output_row -= lines;
			if (block.end_row >= 0) block.end_row -= lines;
		}

		while (!blocks_.empty() && blocks_.front().prompt_row < 0) {
			blocks_.erase(blocks_.begin());
		}
	}

	// OSC 633 marks: A starts a prompt, C starts the command's output, and
	// D reports how it ended.
	void Screen::noteShellMark(const std::string& body) {
		if (body.empty()) return;

		if (body[0] == 'A') {
			CommandBlock block;
			block.prompt_row = currentAbsoluteRow();
			blocks_.push_back(block);
			while (blocks_.size() > kMaxCommandBlocks) blocks_.erase(blocks_.begin());
			return;
		}

		if (blocks_.empty()) return;

		if (body[0] == 'C') {
			blocks_.back().output_row = currentAbsoluteRow();
			return;
		}

		if (body[0] != 'D') return;

		blocks_.back().end_row = currentAbsoluteRow();
		blocks_.back().finished = true;

		const std::size_t separator = body.find(';');
		if (separator == std::string::npos) return;

		blocks_.back().exit_status = std::atoi(body.substr(separator + 1).c_str());
	}

	static std::string percentDecode(const std::string& text) {
		std::string out;
		for (std::size_t i = 0; i < text.size(); ++i) {
			if (text[i] != '%' || i + 2 >= text.size()) {
				out.push_back(text[i]);
				continue;
			}

			const std::string digits = text.substr(i + 1, 2);
			out.push_back(static_cast<char>(std::strtol(digits.c_str(), nullptr, 16)));
			i += 2;
		}

		return out;
	}

	// OSC 7 carries a file:// URL; the host part is of no use locally.
	void Screen::noteWorkingDirectory(const std::string& body) {
		static const std::string kPrefix = "file://";
		if (body.rfind(kPrefix, 0) != 0) {
			working_directory_ = percentDecode(body);
			return;
		}

		const std::size_t slash = body.find('/', kPrefix.size());
		if (slash == std::string::npos) return;

		working_directory_ = percentDecode(body.substr(slash + 1));
	}

	// OSC 1337;pick;... is this terminal's own: the shell hands over a list
	// and takes back what was chosen, instead of drawing a picker itself.
	void Screen::noteTerminalRequest(const std::string& body) {
		if (body.rfind("pick;", 0) == 0) notePickRequest(body);
		if (body.rfind("tmux;", 0) == 0) noteTmuxRequest(body);
		if (body == "clear;scrollback") clearScrollback();
	}

	void Screen::noteTmuxRequest(const std::string& body) {
		if (tmux_handler_ == nullptr) return;

		if (body == "tmux;attach") tmux_handler_->tmuxAttach();
	}

	void Screen::notePickRequest(const std::string& body) {
		if (pick_handler_ == nullptr) return;
		if (body.rfind("pick;", 0) != 0) return;

		const std::string rest = body.substr(5);
		const std::size_t separator = rest.find(';');
		const std::string verb = rest.substr(0, separator);
		const std::string value = separator == std::string::npos
			? std::string()
			: percentDecode(rest.substr(separator + 1));

		if (verb == "begin")  pick_handler_->pickBegin(value);
		if (verb == "item")   pick_handler_->pickItem(value);
		if (verb == "list")   notePickList(value);
		if (verb == "end")    pick_handler_->pickEnd();
		if (verb == "cancel") pick_handler_->pickCancel();
	}

	// ConPTY forwards only a few kilobytes of OSC output before it starts
	// dropping the rest, which loses the tail of a long list and the "end"
	// that opens the overlay with it. Past a handful of entries the shell
	// therefore writes the list to a file and sends only its path.
	void Screen::notePickList(const std::string& path) {
		std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
		if (!file) return;

		std::string line;
		while (std::getline(file, line)) {
			if (!line.empty() && line.back() == '\r') line.pop_back();
			pick_handler_->pickItem(line);
		}
	}

	void Screen::vtOsc(const std::string& text) {
		const std::size_t separator = text.find(';');
		if (separator == std::string::npos) return;

		const std::string code = text.substr(0, separator);
		const std::string body = text.substr(separator + 1);

		if (code == "0" || code == "2") title_ = body;
		if (code == "7")   noteWorkingDirectory(body);
		if (code == "633")  noteShellMark(body);
		if (code == "1337") noteTerminalRequest(body);
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
			if (sequence.final_byte == 'p' && sequence.intermediates == "$") {
				answerModeReport(sequence);
			}
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
			for (int row = 0; row < rows_; ++row) clearRow(row, 0, columns_ - 1);
			break;
		case 3:
			clearScrollback();
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

	// The alt screen is a second grid with no history: vim and less draw
	// there so the scrollback still holds what the shell printed before.
	void Screen::enterAltScreen() {
		if (alt_screen_) return;

		primary_cells_  = cells_;
		primary_cursor_ = cursor_;
		alt_screen_     = true;

		cells_.assign(cells_.size(), Cell());
		moveCursor(0, 0);
		markAllDirty();
	}

	void Screen::leaveAltScreen() {
		if (!alt_screen_) return;

		alt_screen_ = false;
		if (primary_cells_.size() == cells_.size()) cells_ = primary_cells_;
		cursor_ = primary_cursor_;
		markAllDirty();
	}

	void Screen::applyPrivateMode(const VtSequence& sequence, bool enable) {
		for (std::size_t i = 0; i < sequence.params.size(); ++i) {
			switch (sequence.param(i, 0)) {
			case 1:    application_cursor_ = enable; break;
			case 25:   cursor_.visible = enable; break;
			case 47:
			case 1047:
			case 1049:
				if (enable) enterAltScreen();
				else leaveAltScreen();
				break;
			case 2004: bracketed_paste_ = enable; break;
			case 2026: synchronized_output_ = enable; break;
			default:   break;
			}
		}
	}

	// DECRPM's answers: 1 set, 2 reset, 0 for a mode this grid does not
	// know. Asking is how a program finds out that synchronized output
	// will be honoured before it relies on it.
	int Screen::privateModeState(int mode) const {
		switch (mode) {
		case 1:    return application_cursor_ ? 1 : 2;
		case 25:   return cursor_.visible ? 1 : 2;
		case 47:
		case 1047:
		case 1049: return alt_screen_ ? 1 : 2;
		case 2004: return bracketed_paste_ ? 1 : 2;
		case 2026: return synchronized_output_ ? 1 : 2;
		default:   return 0;
		}
	}

	void Screen::answerModeReport(const VtSequence& sequence) {
		if (responder_ == nullptr) return;

		const int mode = sequence.param(0, 0);
		std::string reply = "\x1b[?";
		reply += std::to_string(mode);
		reply += ";";
		reply += std::to_string(privateModeState(mode));
		reply += "$y";
		responder_->vtRespond(reply);
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

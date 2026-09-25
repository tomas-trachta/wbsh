/**
 * @file view.cpp
 * @brief Scroll arithmetic, selection ordering, and text extraction.
 */

#include "view.h"

#include <algorithm>
#include <limits>

namespace wbshterm {

	static bool isWordCharacter(char32_t code) {
		if (code >= U'0' && code <= U'9') return true;
		if (code >= U'A' && code <= U'Z') return true;
		if (code >= U'a' && code <= U'z') return true;
		return code == U'_' || code == U'-' || code == U'.' || code == U'/' || code >= 0x80;
	}

	static bool pointBefore(const GridPoint& left, const GridPoint& right) {
		if (left.row != right.row) return left.row < right.row;
		return left.column < right.column;
	}

	int TerminalView::maxScrollOffset(const Screen& screen) const {
		return std::max(0, screen.totalRows() - screen.rows());
	}

	// A grown grid is laid out from the top, the way the console repaints
	// it, which leaves blank rows under the text while history sits just
	// above the window. Resting the view on the last row of text shows
	// that history where the blanks would be.
	int TerminalView::restOffset(const Screen& screen) const {
		if (screen.onAltScreen()) return 0;
		return std::min(screen.rows() - screen.contentRowCount(), maxScrollOffset(screen));
	}

	// New lines push the grid down; holding the offset steady would slide
	// the text the reader is looking at, so grow it by as much as arrived.
	// A view at rest stays at rest, following the text as it fills the
	// grid. History can also vanish under the view, when the shell clears
	// it, and then the offset is pulled back into the rows that remain.
	void TerminalView::followOutput(const Screen& screen) {
		const int total = screen.totalRows();
		const int grown = total - last_total_;
		const bool resting = scroll_offset_ <= last_rest_;
		last_total_ = total;
		last_rest_  = restOffset(screen);

		if (resting) scroll_offset_ = last_rest_;
		else if (grown > 0) scroll_offset_ += grown;

		scroll_offset_ = std::min(scroll_offset_, maxScrollOffset(screen));
	}

	void TerminalView::scrollBy(int lines, const Screen& screen) {
		if (screen.onAltScreen()) {
			scroll_offset_ = 0;
			return;
		}

		scroll_offset_ = std::min(std::max(scroll_offset_ + lines, 0), maxScrollOffset(screen));
	}

	void TerminalView::scrollToBottom(const Screen& screen) {
		last_rest_    = restOffset(screen);
		scroll_offset_ = last_rest_;
	}

	void TerminalView::scrollToRow(int absolute_row, const Screen& screen) {
		const int bottom_top = screen.totalRows() - screen.rows();
		scroll_offset_ = std::min(std::max(bottom_top - absolute_row, 0), maxScrollOffset(screen));
	}

	// Jumping moves between prompts, which is what a reader means by "the
	// previous command" -- the line they typed, with its output below it.
	int TerminalView::neighbouringCommandRow(const Screen& screen, bool backwards) const {
		const std::vector<CommandBlock>& blocks = screen.commandBlocks();
		const int here = topRow(screen);

		if (backwards) {
			int best = -1;
			for (const CommandBlock& block : blocks) {
				if (block.prompt_row < here) best = block.prompt_row;
			}

			return best;
		}

		for (const CommandBlock& block : blocks) {
			if (block.prompt_row > here) return block.prompt_row;
		}

		return -1;
	}

	void TerminalView::selectBlockOutput(const CommandBlock& block, const Screen& screen) {
		if (block.output_row < 0) return;

		const int last = block.end_row > block.output_row
			? block.end_row - 1
			: block.output_row;

		anchor_        = { block.output_row, 0 };
		focus_         = { last, screen.columns() - 1 };
		selecting_     = false;
		has_selection_ = true;
	}

	int TerminalView::topRow(const Screen& screen) const {
		const int bottom_top = screen.totalRows() - screen.rows();
		return std::max(0, bottom_top - scroll_offset_);
	}

	void TerminalView::beginSelection(GridPoint point) {
		anchor_        = point;
		focus_         = point;
		selecting_     = true;
		has_selection_ = false;
	}

	void TerminalView::extendSelection(GridPoint point) {
		focus_ = point;
		has_selection_ = anchor_.row != focus_.row || anchor_.column != focus_.column;
	}

	void TerminalView::selectWord(GridPoint point, const Screen& screen) {
		int first = point.column;
		int last  = point.column;

		while (first > 0 && isWordCharacter(screen.cellAt(point.row, first - 1).code)) --first;
		while (last + 1 < screen.columns()
			&& isWordCharacter(screen.cellAt(point.row, last + 1).code)) {
			++last;
		}

		anchor_        = { point.row, first };
		focus_         = { point.row, last };
		selecting_     = false;
		has_selection_ = isWordCharacter(screen.cellAt(point.row, point.column).code);
	}

	void TerminalView::selectLine(GridPoint point, const Screen& screen) {
		anchor_        = { point.row, 0 };
		focus_         = { point.row, screen.columns() - 1 };
		selecting_     = false;
		has_selection_ = true;
	}

	void TerminalView::clearSelection() {
		selecting_     = false;
		has_selection_ = false;
	}

	GridPoint TerminalView::selectionStart() const {
		return pointBefore(anchor_, focus_) ? anchor_ : focus_;
	}

	GridPoint TerminalView::selectionEnd() const {
		return pointBefore(anchor_, focus_) ? focus_ : anchor_;
	}

	bool TerminalView::isSelected(int absolute_row, int column) const {
		if (!has_selection_) return false;

		const GridPoint start = selectionStart();
		const GridPoint end   = selectionEnd();
		if (absolute_row < start.row || absolute_row > end.row) return false;

		const int first = absolute_row == start.row ? start.column : 0;
		const int last  = absolute_row == end.row
			? end.column
			: std::numeric_limits<int>::max();
		return column >= first && column <= last;
	}

	std::string TerminalView::selectedText(const Screen& screen) const {
		if (!has_selection_) return std::string();

		const GridPoint start = selectionStart();
		const GridPoint end   = selectionEnd();

		std::string text;
		for (int row = start.row; row <= end.row; ++row) {
			const int first = row == start.row ? start.column : 0;
			const int last  = row == end.row ? end.column : screen.columns() - 1;

			std::string line;
			for (int column = first; column <= last && column < screen.columns(); ++column) {
				const char32_t code = screen.cellAt(row, column).code;
				if (code < 0x80) line.push_back(static_cast<char>(code));
				else line.push_back('?');
			}

			while (!line.empty() && line.back() == ' ') line.pop_back();

			text += line;
			if (row != end.row) text += "\r\n";
		}

		return text;
	}

} /* namespace wbshterm */

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

	// New lines push the grid down; holding the offset steady would slide
	// the text the reader is looking at, so grow it by as much as arrived.
	void TerminalView::followOutput(const Screen& screen) {
		const int total = screen.totalRows();
		const int grown = total - last_total_;
		last_total_ = total;

		if (scroll_offset_ <= 0 || grown <= 0) return;
		scroll_offset_ = std::min(scroll_offset_ + grown, maxScrollOffset(screen));
	}

	void TerminalView::scrollBy(int lines, const Screen& screen) {
		if (screen.onAltScreen()) {
			scroll_offset_ = 0;
			return;
		}

		scroll_offset_ = std::min(std::max(scroll_offset_ + lines, 0), maxScrollOffset(screen));
	}

	void TerminalView::scrollToBottom() {
		scroll_offset_ = 0;
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

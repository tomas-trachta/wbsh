#pragma once

/**
 * @file view.h
 * @brief What the window is looking at: scroll position and selection.
 */

#include "screen.h"

#include <string>

namespace wbshterm {

	/** A cell in the scrollback-plus-grid coordinate space. */
	struct GridPoint {
		int row    = 0;
		int column = 0;
	};

	/**
	 * @brief View state, kept apart from the terminal state it looks at.
	 *
	 * Positions are absolute rows, so scrolling never moves a selection
	 * and new output never drags it: the anchor stays on the text it was
	 * put on until the line falls out of scrollback.
	 */
	class TerminalView {
	public:
		/** Keeps the visible text still while new lines arrive underneath. */
		void followOutput(const Screen& screen);

		void scrollBy(int lines, const Screen& screen);
		void scrollToBottom();

		/** Puts @p absolute_row at the top of the window, as far as it can. */
		void scrollToRow(int absolute_row, const Screen& screen);

		/**
		 * @brief The command before or after the one at the top of the window.
		 *
		 * @return The row to scroll to, or -1 when there is no such command.
		 */
		int neighbouringCommandRow(const Screen& screen, bool backwards) const;

		/** Selects the output of @p block, for copying it in one go. */
		void selectBlockOutput(const CommandBlock& block, const Screen& screen);
		int scrollOffset() const { return scroll_offset_; }

		/** Absolute row shown at the top of the window. */
		int topRow(const Screen& screen) const;

		void beginSelection(GridPoint point);
		void extendSelection(GridPoint point);
		void selectWord(GridPoint point, const Screen& screen);
		void selectLine(GridPoint point, const Screen& screen);
		void clearSelection();

		bool selecting() const { return selecting_; }
		void endSelection() { selecting_ = false; }
		bool hasSelection() const { return has_selection_; }
		bool isSelected(int absolute_row, int column) const;

		std::string selectedText(const Screen& screen) const;

	private:
		GridPoint selectionStart() const;
		GridPoint selectionEnd() const;
		int maxScrollOffset(const Screen& screen) const;

		int  scroll_offset_ = 0;
		int  last_total_    = 0;
		bool selecting_     = false;
		bool has_selection_ = false;

		GridPoint anchor_;
		GridPoint focus_;
	};

} /* namespace wbshterm */

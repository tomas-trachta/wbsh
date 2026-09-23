#pragma once

/**
 * @file screen.h
 * @brief The cell grid a VT stream is rendered into.
 */

#include "vtparse.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace wbshterm {

	/** Colors are 0x00RRGGBB, or this sentinel for "whatever the theme says". */
	static const std::uint32_t kDefaultColor = 0xFF000000u;

	enum CellAttr : std::uint16_t {
		kAttrNone      = 0,
		kAttrBold      = 1 << 0,
		kAttrDim       = 1 << 1,
		kAttrItalic    = 1 << 2,
		kAttrUnderline = 1 << 3,
		kAttrReverse   = 1 << 4,
		kAttrInvisible = 1 << 5,
	};

	struct Cell {
		char32_t      code       = U' ';
		std::uint32_t foreground = kDefaultColor;
		std::uint32_t background = kDefaultColor;
		std::uint16_t attributes = kAttrNone;
	};

	struct CursorState {
		int  row     = 0;
		int  column  = 0;
		bool visible = true;
	};

	/**
	 * @brief A fixed-size grid with no scrollback: M1 renders one screenful.
	 *
	 * Implements VtSink, so a VtParser drives it directly. Rows that
	 * change are marked dirty for the renderer and cleared by
	 * clearDirty(); a resize marks everything dirty.
	 */
	class Screen : public VtSink {
	public:
		Screen(int columns, int rows);

		void resize(int columns, int rows);

		int columns() const { return columns_; }
		int rows() const { return rows_; }

		/** Lines that have scrolled off the top and are still remembered. */
		int scrollbackRows() const { return static_cast<int>(scrollback_.size()); }

		/** Scrollback plus the live grid, the coordinate space the view uses. */
		int totalRows() const { return scrollbackRows() + rows_; }

		/** Addresses scrollback and grid alike; row 0 is the oldest line kept. */
		const Cell& cellAt(int absolute_row, int column) const;

		bool onAltScreen() const { return alt_screen_; }

		const Cell& cell(int row, int column) const;
		const CursorState& cursor() const { return cursor_; }
		const std::string& title() const { return title_; }
		bool applicationCursorKeys() const { return application_cursor_; }
		bool bracketedPaste() const { return bracketed_paste_; }

		bool rowDirty(int row) const;
		void clearDirty();

		/** The grid as text, one line per row, trailing blanks trimmed. */
		std::string toText() const;

		void vtPrint(char32_t code) override;
		void vtExecute(unsigned char control) override;
		void vtCsi(const VtSequence& sequence) override;
		void vtEsc(const VtSequence& sequence) override;
		void vtOsc(const std::string& text) override;

		/** Answers device and cursor queries; without one they go unanswered. */
		void setResponder(VtResponder* responder) { responder_ = responder; }

	private:
		Cell& at(int row, int column);
		void markDirty(int row);
		void markAllDirty();

		void writeChar(char32_t code);
		void advanceCursor();
		void lineFeed();
		void carriageReturn();
		void backspace();
		void tab();

		void scrollUp(int count);
		void pushToScrollback(int row);
		void enterAltScreen();
		void leaveAltScreen();
		void scrollDown(int count);
		void clearRow(int row, int from_column, int to_column);

		void moveCursor(int row, int column);
		void moveCursorBy(int rows, int columns);

		void applySgr(const VtSequence& sequence);
		bool applyExtendedColor(const VtSequence& sequence, std::size_t& index,
			std::uint32_t& out_color);
		void applyEraseInDisplay(const VtSequence& sequence);
		void applyEraseInLine(const VtSequence& sequence);
		void applyInsertDelete(const VtSequence& sequence);
		void applyPrivateMode(const VtSequence& sequence, bool enable);
		void applyScrollRegion(const VtSequence& sequence);
		void answerDeviceAttributes(bool secondary);
		void answerStatusReport(const VtSequence& sequence);

		int  scrollTop() const { return scroll_top_; }
		int  scrollBottom() const { return scroll_bottom_; }

		int  columns_ = 0;
		int  rows_    = 0;
		std::vector<Cell> cells_;
		std::deque<std::vector<Cell>> scrollback_;
		std::vector<Cell> primary_cells_;
		CursorState       primary_cursor_;
		bool              alt_screen_ = false;
		std::vector<bool> dirty_;

		CursorState cursor_;
		CursorState saved_cursor_;
		Cell        pen_;
		bool        wrap_pending_ = false;

		int scroll_top_    = 0;
		int scroll_bottom_ = 0;

		std::string   title_;
		bool          application_cursor_ = false;
		bool          bracketed_paste_    = false;
		VtResponder*  responder_ = nullptr;
	};

} /* namespace wbshterm */

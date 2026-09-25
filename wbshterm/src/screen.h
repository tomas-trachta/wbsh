#pragma once

/**
 * @file screen.h
 * @brief The cell grid a VT stream is rendered into.
 */

#include "cell.h"
#include "history.h"
#include "vtparse.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace wbshterm {

	/**
	 * @brief One command, as the shell reported it through OSC 633.
	 *
	 * Rows are absolute, the same space the view scrolls in, so a block
	 * keeps pointing at its own text as output arrives beneath it.
	 */
	struct CommandBlock {
		int  prompt_row  = 0;
		int  output_row  = -1;
		int  end_row     = -1;
		int  exit_status = -1;
		bool finished    = false;
	};

	struct CursorState {
		int  row     = 0;
		int  column  = 0;
		bool visible = true;
	};

	/**
	 * @brief Whether a row's line goes on in the row below.
	 *
	 * The console reports a wrapped row and a finished line the same way,
	 * so for a row it painted this stays unknown and is guessed from the
	 * row's last cell. A reflow knows where it wrapped and where it ended
	 * a line, and a row keeps that knowledge until its text changes.
	 */
	enum class LineBreak : std::uint8_t {
		kUnknown,
		kWraps,
		kEnds,
	};

	struct HistoryLine {
		std::vector<Cell> cells;
		LineBreak         line_break = LineBreak::kUnknown;
	};

	namespace reflow {

		/** One row of text as it was laid out before a resize. */
		struct SourceRow {
			const Cell* cells;
			int         width;
			LineBreak   line_break;
		};

		struct Position {
			int row    = 0;
			int column = 0;
		};

		/** Rows rewrapped to a new width, with where every old row landed. */
		struct Result {
			std::deque<HistoryLine> rows;
			std::vector<int>        row_map;
			Position                cursor;
		};

		bool cellHasInk(const Cell& cell);

		/** Rows at and past break_before never join the row before them. */
		Result rewrap(const std::vector<SourceRow>& sources, int columns,
			std::size_t break_before, int cursor_source, int cursor_column);

	} /* namespace reflow */

	/**
	 * @brief A fixed-size grid with no scrollback: M1 renders one screenful.
	 *
	 * Implements VtSink, so a VtParser drives it directly. Rows that
	 * change are marked dirty for the renderer and cleared by
	 * clearDirty(); a resize marks everything dirty.
	 */
	/** Where a request to start pane mode goes: the window that hosts them. */
	class TmuxHandler {
	public:
		virtual ~TmuxHandler() = default;

		virtual void tmuxAttach() = 0;
	};

	/** Where a pick request from the shell goes: the window's overlay. */
	class PickHandler {
	public:
		virtual ~PickHandler() = default;

		virtual void pickBegin(const std::string& prompt) = 0;
		virtual void pickItem(const std::string& text) = 0;
		virtual void pickEnd() = 0;
		virtual void pickCancel() = 0;
	};

	class Screen : public VtSink {
	public:
		Screen(int columns, int rows);

		void resize(int columns, int rows);
		/** Rows kept in memory before older ones go to disk. */
		void setScrollbackLimit(int lines);

		/** Bytes of history on disk before the oldest is forgotten. */
		void setHistoryDiskLimit(std::uint64_t bytes);
		void clearAll();

		/** Forgets every line that scrolled off; the grid itself stays. */
		void clearScrollback();

		int columns() const { return columns_; }
		int rows() const { return rows_; }

		/** Lines that have scrolled off the top and are still remembered. */
		int scrollbackRows() const { return cold_.rows() + static_cast<int>(scrollback_.size()); }

		/** Rows swapped out to disk; the oldest history is read back on demand. */
		int coldRows() const { return cold_.rows(); }

		/** Lets go of history read back from disk; it is read again when looked at. */
		void releaseColdHistory() const { cold_.releaseCache(); }

		/** Scrollback plus the live grid, the coordinate space the view uses. */
		int totalRows() const { return scrollbackRows() + rows_; }

		/** Addresses scrollback and grid alike; row 0 is the oldest line kept. */
		const Cell& cellAt(int absolute_row, int column) const;

		bool onAltScreen() const { return alt_screen_; }

		/** Grid rows down to the last one with text or the cursor, whichever is lower. */
		int contentRowCount() const;

		/** Commands the shell has marked, oldest first. */
		const std::vector<CommandBlock>& commandBlocks() const { return blocks_; }

		/** Working directory as last reported by OSC 7, or empty. */
		const std::string& workingDirectory() const { return working_directory_; }

		const Cell& cell(int row, int column) const;
		const CursorState& cursor() const { return cursor_; }
		const std::string& title() const { return title_; }
		bool applicationCursorKeys() const { return application_cursor_; }
		bool bracketedPaste() const { return bracketed_paste_; }

		/** True between DECSET 2026 and its reset: a frame is mid-update. */
		bool synchronizedOutput() const { return synchronized_output_; }

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

		/** Without one, pick requests from the shell are ignored. */
		void setPickHandler(PickHandler* handler) { pick_handler_ = handler; }

		/** Without one, a request to start pane mode is ignored. */
		void setTmuxHandler(TmuxHandler* handler) { tmux_handler_ = handler; }

	private:
		Cell& at(int row, int column);
		void putCell(int row, int column, const Cell& value);
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
		void noteShellMark(const std::string& body);
		void noteWorkingDirectory(const std::string& body);
		void noteTerminalRequest(const std::string& body);
		void notePickRequest(const std::string& body);
		void noteTmuxRequest(const std::string& body);
		void notePickList(const std::string& path);
		int  currentAbsoluteRow() const;
		void trimScrollback();
		void swapOutOldestRows();
		std::size_t rowsToSwapOut() const;
		std::vector<std::vector<Cell>> joinRowsForStorage(std::size_t count,
			std::size_t& out_consumed) const;
		void remapBlocksForColdWidth(int new_columns);
		void shiftBlocksUp(int lines);
		void reflowPrimary(int new_columns, int new_rows);
		void swapWithPrimary();
		void clampCursor(CursorState& cursor) const;
		std::vector<reflow::SourceRow> reflowSources() const;
		void remapBlocks(const std::vector<int>& row_map, int new_total);
		void placeReflowedRows(std::deque<HistoryLine>& rows, int grid_top, int new_columns,
			int new_rows);
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
		int  privateModeState(int mode) const;
		void answerModeReport(const VtSequence& sequence);
		void applyScrollRegion(const VtSequence& sequence);
		void answerDeviceAttributes(bool secondary);
		void answerStatusReport(const VtSequence& sequence);

		int  scrollTop() const { return scroll_top_; }
		int  scrollBottom() const { return scroll_bottom_; }

		int  columns_ = 0;
		int  rows_    = 0;
		std::vector<Cell> cells_;
		std::deque<HistoryLine> scrollback_;
		HistoryStore      cold_;
		std::size_t       scrollback_limit_ = 10000;
		std::uint64_t     disk_limit_ = 1024ull * 1024ull * 1024ull;
		std::vector<Cell> primary_cells_;
		CursorState       primary_cursor_;
		std::vector<LineBreak> row_breaks_;
		std::vector<LineBreak> primary_breaks_;
		bool              alt_screen_ = false;
		std::vector<bool> dirty_;

		CursorState cursor_;
		CursorState saved_cursor_;
		Cell        pen_;
		bool        wrap_pending_ = false;

		int scroll_top_    = 0;
		int scroll_bottom_ = 0;

		std::string   title_;
		std::string   working_directory_;
		std::vector<CommandBlock> blocks_;
		bool          application_cursor_ = false;
		bool          bracketed_paste_    = false;
		bool          synchronized_output_ = false;
		VtResponder*  responder_ = nullptr;
		PickHandler*  pick_handler_ = nullptr;
		TmuxHandler*  tmux_handler_ = nullptr;
	};

} /* namespace wbshterm */

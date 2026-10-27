#pragma once

/**
 * @file screen.h
 * @brief The cell grid a VT stream is rendered into.
 */

#include "cell.h"
#include "history.h"
#include "theme.h"
#include "vtparse.h"

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
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

	/** What DECSC keeps and DECRC brings back: the place and the pen. */
	struct SavedCursor {
		int  row          = 0;
		int  column       = 0;
		Cell pen;
		bool wrap_pending = false;
		bool origin_mode  = false;
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

	/** An xterm 256-colour index as a cell colour; 0-15 stay palette slots. */
	std::uint32_t indexedColor(int index);

	/**
	 * @brief Whether a hyperlink a program printed may be handed to the shell.
	 *
	 * Only web and mail URLs: a file URL or a bare path could name a
	 * program to run, and the text the user clicked need not show it.
	 */
	bool linkIsSafeToOpen(const std::string& target);

	/** What DECSCUSR asked for; kDefault leaves the cursor to the config. */
	enum class CursorShape : std::uint8_t {
		kDefault,
		kBlinkingBlock,
		kSteadyBlock,
		kBlinkingUnderline,
		kSteadyUnderline,
		kBlinkingBar,
		kSteadyBar,
	};

	/** Where text a program puts on the clipboard (OSC 52) goes. */
	class ClipboardHandler {
	public:
		virtual ~ClipboardHandler() = default;

		virtual void clipboardWrite(const std::string& utf8) = 0;
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

		CursorShape cursorShape() const { return cursor_shape_; }

		/**
		 * @brief A cell's whole grapheme cluster, or null for a lone code point.
		 *
		 * A cell whose code carries kClusterCode holds a base character
		 * with marks, joiners or selectors after it; the renderer shapes
		 * them together and everything else reads baseCodeOf().
		 */
		const std::u32string* clusterOf(const Cell& cell) const;

		/** The code point a cell starts with, cluster or not. */
		char32_t baseCodeOf(const Cell& cell) const;

		/** The OSC 8 target under a cell, or null when it is not a link. */
		const std::string* linkAt(int absolute_row, int column) const;

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

		/**
		 * @brief Whether a C0 byte the console leaves alone takes a cell.
		 *
		 * The pseudoconsole built into Windows 10 stores such a byte as its
		 * OEM glyph and moves its cursor past it, so the grid must too or
		 * the relative move it sends next lands a column short. Microsoft's
		 * current console treats them as xterm does, zero width.
		 */
		void setControlGlyphs(bool enabled) { control_glyphs_ = enabled; }

		/** Without one, pick requests from the shell are ignored. */
		void setPickHandler(PickHandler* handler) { pick_handler_ = handler; }

		/** Without one, a request to start pane mode is ignored. */
		void setTmuxHandler(TmuxHandler* handler) { tmux_handler_ = handler; }

		/** Without one, OSC 52 clipboard writes are ignored. */
		void setClipboardHandler(ClipboardHandler* handler) { clipboard_handler_ = handler; }

		/**
		 * @brief The colours OSC 4/10/11/12 queries are answered with.
		 *
		 * Borrowed, and read at query time so a theme change is reported
		 * as soon as it shows; null answers with the built-in palette.
		 */
		void setPalette(const Palette* palette) { palette_ = palette; }

	private:
		Cell& at(int row, int column);
		void putCell(int row, int column, const Cell& value);
		void markDirty(int row);
		void markAllDirty();

		struct GridPlace {
			int row    = 0;
			int column = 0;
		};

		/** The cluster still taking marks: where its lead sits and how wide it is. */
		struct OpenCluster {
			bool open       = false;
			int  row        = 0;
			int  column     = 0;
			int  width      = 0;
			int  join_state = 0;
			int  last_class = 0;
		};

		bool writeChar(char32_t code);
		GridPlace placeGlyph(char32_t code, int width);
		bool joinsOpenCluster(int properties);
		bool startCluster(char32_t code, int properties);
		void extendCluster(char32_t code, int properties);
		void appendToClusterCell(char32_t code);
		void widenCluster();
		void closeCluster() { cluster_.open = false; }
		std::uint32_t internCluster(const std::u32string& text);
		void wrapToNextRow();
		void makeRoomForWide();
		void advanceCursor();
		void lineFeed();
		void carriageReturn();
		void backspace();
		void tab();
		void resetTabStops();
		void resizeTabStops(int new_columns);
		void forwardTab(int count);
		void backwardTab(int count);
		void clearTabStops(int mode);
		void repeatLastCharacter(int count);
		void setCursorShape(int shape);
		void noteHyperlink(const std::string& body);
		std::uint16_t internLink(const std::string& target);
		void noteClipboardWrite(const std::string& body);
		void answerColorQuery(int code, const std::string& body);
		void answerPaletteQuery(const std::string& body);
		void replyColor(const std::string& prefix, std::uint32_t rgb);
		const Palette& palette() const;
		void resetVtExtras();
		void printControlGlyph(unsigned char control);

		void scrollUp(int count);
		std::vector<Cell>::iterator rowStart(int row);
		void blankRows(int first, int last);
		void markRowsDirty(int first, int last);
		void shiftRowsUp(int top, int bottom, int count);
		void shiftRowsDown(int top, int bottom, int count);
		bool cursorInScrollRegion() const;
		void insertLines(int count);
		void deleteLines(int count);
		void pushToScrollback(int row);
		void noteShellMark(const std::string& body);
		bool promptAlreadyMarked() const;
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
		void clampSavedCursor(SavedCursor& saved) const;
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
		void moveCursorInOrigin(int row, int column);
		void saveCursor();
		void restoreCursor();
		void setOriginMode(bool enable);
		void resetModes();
		void softReset();
		void hardReset();
		void dropAltScreen();
		void applyIntermediateCsi(const VtSequence& sequence);
		void applyPrivateCsi(const VtSequence& sequence);
		bool applyCursorMotion(const VtSequence& sequence);
		void applyEditingCsi(const VtSequence& sequence);

		void applySgr(const VtSequence& sequence);
		std::size_t applySgrGroup(const VtSequence& sequence, std::size_t index);
		std::uint32_t* penColorFor(int code);
		std::size_t applyColorSgr(const VtSequence& sequence, std::size_t index);
		void applyUnderlineStyle(int style);
		void setUnderlineShape(std::uint16_t shape);
		void applySimpleSgr(int code);
		void applyAttributeSgr(int code);
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
		SavedCursor saved_cursor_;
		Cell        pen_;
		bool        wrap_pending_ = false;
		bool        origin_mode_  = false;

		int scroll_top_    = 0;
		int scroll_bottom_ = 0;

		std::string   title_;
		std::string   working_directory_;
		std::vector<CommandBlock> blocks_;
		bool          application_cursor_ = false;
		bool          bracketed_paste_    = false;
		bool          synchronized_output_ = false;
		bool          control_glyphs_     = true;
		VtResponder*  responder_ = nullptr;
		PickHandler*  pick_handler_ = nullptr;
		TmuxHandler*  tmux_handler_ = nullptr;
		ClipboardHandler* clipboard_handler_ = nullptr;
		const Palette*    palette_ = nullptr;

		std::vector<bool> tab_stops_;
		bool              autowrap_     = true;
		char32_t          last_printed_ = 0;
		CursorShape       cursor_shape_ = CursorShape::kDefault;
		OpenCluster       cluster_;
		std::vector<std::u32string> clusters_;
		std::unordered_map<std::u32string, std::uint32_t> cluster_ids_;
		std::vector<std::string> links_;
		std::unordered_map<std::string, std::uint16_t> link_ids_;
	};

} /* namespace wbshterm */

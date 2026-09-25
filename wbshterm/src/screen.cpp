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
	static const std::size_t kSwapChunkRows = 1024;

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

	namespace reflow {

		bool cellHasInk(const Cell& cell) {
			return cell.code != U' ' || cell.background != kDefaultColor
				|| (cell.attributes & (kAttrWide | kAttrWideTail)) != 0;
		}

		static bool rowFilledToTheEdge(const SourceRow& row) {
			if (row.width <= 0) return false;

			const Cell& last = row.cells[row.width - 1];
			return last.code != U' ' || (last.attributes & (kAttrWide | kAttrWideTail)) != 0;
		}

		static bool rowGoesOn(const SourceRow& row) {
			if (row.line_break == LineBreak::kWraps) return true;
			if (row.line_break == LineBreak::kEnds) return false;
			return rowFilledToTheEdge(row);
		}

		static int inkLength(const SourceRow& row) {
			int length = row.width;
			while (length > 0 && !cellHasInk(row.cells[length - 1])) --length;
			return length;
		}

		struct LogicalLine {
			std::vector<Cell>        cells;
			std::vector<std::size_t> row_starts;
			std::size_t              first_source = 0;
			LineBreak                ending = LineBreak::kEnds;
		};

		static bool isWideLead(const LogicalLine& line, std::size_t index) {
			if ((line.cells[index].attributes & kAttrWide) == 0) return false;
			if (index + 1 >= line.cells.size()) return false;
			return (line.cells[index + 1].attributes & kAttrWideTail) != 0;
		}

		/** Fills rows of a fixed width, keeping a wide character's two cells together. */
		class RowEmitter {
		public:
			RowEmitter(int columns, std::deque<HistoryLine>& out)
				: columns_(static_cast<std::size_t>(columns)), out_(out) {}

			Position put(const Cell& cell, bool wide_lead) {
				const bool pair_would_split =
					wide_lead && columns_ >= 2 && row_.size() == columns_ - 1;
				if (pair_would_split) row_.push_back(Cell());
				if (row_.size() == columns_) flush();

				const Position landed = position();
				row_.push_back(cell);
				return landed;
			}

			Position position() const {
				if (row_.size() == columns_) return { static_cast<int>(out_.size()) + 1, 0 };
				return { static_cast<int>(out_.size()), static_cast<int>(row_.size()) };
			}

			// A line cut short at the console's top row keeps its last row
			// unpadded, so the cells are exactly what precedes the rest.
			void finishLine(LineBreak ending) {
				if (ending == LineBreak::kEnds) row_.resize(columns_, Cell());
				out_.push_back({ std::move(row_), ending });
				row_.clear();
			}

		private:
			void flush() {
				row_.resize(columns_, Cell());
				out_.push_back({ std::move(row_), LineBreak::kWraps });
				row_.clear();
			}

			std::size_t              columns_;
			std::deque<HistoryLine>& out_;
			std::vector<Cell>        row_;
		};

		// A row keeps its trailing blanks only while it continues on the
		// next one; the cursor's row keeps at least the cells up to the
		// cursor, so it lands inside its own line after rewrapping. A line
		// that goes on into the console's top row is cut there, but stays
		// marked as going on: once that row has scrolled into history the
		// two halves meet again and the next reflow rejoins them.
		static std::size_t gatherLogicalLine(const std::vector<SourceRow>& sources,
				std::size_t first, std::size_t break_before, int cursor_source,
				int cursor_column, LogicalLine& line) {
			line.first_source = first;

			std::size_t index = first;
			for (;;) {
				const SourceRow& row = sources[index];
				const bool goes_on   = rowGoesOn(row) && index + 1 < sources.size();
				const bool continues = goes_on && index + 1 != break_before;

				int keep = goes_on ? row.width : inkLength(row);
				if (static_cast<int>(index) == cursor_source) {
					keep = std::max(keep, std::min(cursor_column + 1, row.width));
				}

				line.row_starts.push_back(line.cells.size());
				line.cells.insert(line.cells.end(), row.cells, row.cells + keep);

				++index;
				if (continues) continue;

				line.ending = goes_on ? row.line_break : LineBreak::kEnds;
				return index;
			}
		}

		static void emitLogicalLine(const LogicalLine& line, RowEmitter& emitter,
				int cursor_source, int cursor_column, Result& result) {
			std::vector<Position> landed(line.cells.size() + 1);
			for (std::size_t index = 0; index < line.cells.size(); ++index) {
				landed[index] = emitter.put(line.cells[index], isWideLead(line, index));
			}

			landed[line.cells.size()] = emitter.position();

			for (std::size_t row = 0; row < line.row_starts.size(); ++row) {
				result.row_map[line.first_source + row] = landed[line.row_starts[row]].row;
			}

			const std::size_t cursor_offset =
				static_cast<std::size_t>(cursor_source) - line.first_source;
			if (cursor_offset < line.row_starts.size()) {
				const std::size_t at = std::min(line.row_starts[cursor_offset]
					+ static_cast<std::size_t>(std::max(cursor_column, 0)), line.cells.size());
				result.cursor = landed[at];
			}

			emitter.finishLine(line.ending);
		}

		Result rewrap(const std::vector<SourceRow>& sources, int columns,
				std::size_t break_before, int cursor_source, int cursor_column) {
			Result result;
			result.row_map.assign(sources.size(), 0);

			RowEmitter emitter(columns, result.rows);
			std::size_t next = 0;
			while (next < sources.size()) {
				LogicalLine line;
				next = gatherLogicalLine(sources, next, break_before, cursor_source,
					cursor_column, line);
				emitLogicalLine(line, emitter, cursor_source, cursor_column, result);
			}

			return result;
		}

	} /* namespace reflow */

	Screen::Screen(int columns, int rows) {
		resize(columns, rows);
	}

	// The pseudoconsole keeps no history and, after a resize, repaints
	// its whole viewport: rewrapped content sits at the top when it fits
	// and hangs from the bottom when it does not, the overflow gone for
	// good. The grid here is laid out the same way, so the repaint lands
	// on rows that already hold the same text, and the rows the console
	// forgets are the ones that move into scrollback. Reflow can only
	// guess where a line was wrapped -- the console reports every wrap
	// as a plain line break -- so a row filled to its last cell is taken
	// to continue on the next.
	void Screen::resize(int columns, int rows) {
		const int new_columns = std::max(1, columns);
		const int new_rows    = std::max(1, rows);

		if (alt_screen_) {
			swapWithPrimary();
			reflowPrimary(new_columns, new_rows);
			swapWithPrimary();
			cells_.assign(
				static_cast<std::size_t>(new_columns) * static_cast<std::size_t>(new_rows), Cell());
			row_breaks_.assign(static_cast<std::size_t>(new_rows), LineBreak::kUnknown);
		} else {
			reflowPrimary(new_columns, new_rows);
		}

		columns_ = new_columns;
		rows_    = new_rows;
		dirty_.assign(static_cast<std::size_t>(rows_), true);

		scroll_top_    = 0;
		scroll_bottom_ = rows_ - 1;
		wrap_pending_  = false;

		clampCursor(cursor_);
		clampCursor(saved_cursor_);
	}

	void Screen::swapWithPrimary() {
		std::swap(cells_, primary_cells_);
		std::swap(cursor_, primary_cursor_);
		std::swap(row_breaks_, primary_breaks_);
	}

	void Screen::clampCursor(CursorState& cursor) const {
		cursor.row    = std::min(std::max(cursor.row, 0), rows_ - 1);
		cursor.column = std::min(std::max(cursor.column, 0), columns_ - 1);
	}

	void Screen::reflowPrimary(int new_columns, int new_rows) {
		const std::size_t new_size =
			static_cast<std::size_t>(new_columns) * static_cast<std::size_t>(new_rows);
		if (cells_.empty()) {
			cells_.assign(new_size, Cell());
			row_breaks_.assign(static_cast<std::size_t>(new_rows), LineBreak::kUnknown);
			cold_.setColumns(new_columns);
			return;
		}

		const std::vector<reflow::SourceRow> sources = reflowSources();
		const std::size_t hot_rows = scrollback_.size();
		const int cursor_source = static_cast<int>(hot_rows) + cursor_.row;
		reflow::Result result = reflow::rewrap(sources, new_columns, hot_rows, cursor_source,
			cursor_.column);

		const int total      = static_cast<int>(result.rows.size());
		const int grid_start = result.row_map[hot_rows];
		const int produced   = total - grid_start;
		const int grid_top   = produced >= new_rows ? total - new_rows : grid_start;

		remapBlocksForColdWidth(new_columns);
		remapBlocks(result.row_map, std::max(total, grid_top + new_rows));
		placeReflowedRows(result.rows, grid_top, new_columns, new_rows);

		cursor_.row    = result.cursor.row - grid_top;
		cursor_.column = result.cursor.column;
		trimScrollback();
	}

	int Screen::contentRowCount() const {
		int last = cursor_.row;
		for (int row = rows_ - 1; row > last; --row) {
			for (int column = 0; column < columns_; ++column) {
				if (reflow::cellHasInk(cell(row, column))) {
					last = row;
					break;
				}
			}
		}

		return last + 1;
	}

	std::vector<reflow::SourceRow> Screen::reflowSources() const {
		std::vector<reflow::SourceRow> sources;
		sources.reserve(scrollback_.size() + static_cast<std::size_t>(rows_));

		for (const HistoryLine& line : scrollback_) {
			sources.push_back({ line.cells.data(), static_cast<int>(line.cells.size()),
				line.line_break });
		}

		const int content = contentRowCount();
		for (int row = 0; row < content; ++row) {
			sources.push_back({ &cell(row, 0), columns_,
				row_breaks_[static_cast<std::size_t>(row)] });
		}

		return sources;
	}

	// Rows on disk are not rewrapped here; the store counts them afresh
	// for the new width, and a block pointing into them follows its line.
	void Screen::remapBlocksForColdWidth(int new_columns) {
		const int old_cold = cold_.rows();
		std::vector<HistoryLocation> located;
		for (const CommandBlock& block : blocks_) {
			located.push_back(block.prompt_row < old_cold ? cold_.locate(block.prompt_row)
				: HistoryLocation{ -1, 0 });
			located.push_back(block.output_row >= 0 && block.output_row < old_cold
				? cold_.locate(block.output_row) : HistoryLocation{ -1, 0 });
			located.push_back(block.end_row >= 0 && block.end_row < old_cold
				? cold_.locate(block.end_row) : HistoryLocation{ -1, 0 });
		}

		cold_.setColumns(new_columns);

		const int shift = cold_.rows() - old_cold;
		const auto mapped = [&](int row, const HistoryLocation& where) {
			if (where.line >= 0) return cold_.rowOf(where);
			return row < 0 ? row : row + shift;
		};

		std::size_t at = 0;
		for (CommandBlock& block : blocks_) {
			block.prompt_row = mapped(block.prompt_row, located[at++]);
			block.output_row = mapped(block.output_row, located[at++]);
			block.end_row    = mapped(block.end_row, located[at++]);
		}
	}

	// Rows below the disk history sit at row_map's indices plus the disk
	// rows, already recounted for the new width by the time this runs.
	void Screen::remapBlocks(const std::vector<int>& row_map, int new_total) {
		const int cold = cold_.rows();
		const auto mapped = [&](int row) {
			if (row < cold) return row;

			const std::size_t index = static_cast<std::size_t>(row - cold);
			if (index < row_map.size()) return cold + row_map[index];

			const int past_content = static_cast<int>(index + 1 - row_map.size());
			return cold + std::min(row_map.back() + past_content, new_total - 1);
		};

		for (CommandBlock& block : blocks_) {
			block.prompt_row = mapped(block.prompt_row);
			if (block.output_row >= 0) block.output_row = mapped(block.output_row);
			if (block.end_row >= 0) block.end_row = mapped(block.end_row);
		}
	}

	void Screen::placeReflowedRows(std::deque<HistoryLine>& rows, int grid_top, int new_columns,
			int new_rows) {
		const std::size_t width = static_cast<std::size_t>(new_columns);
		cells_.assign(width * static_cast<std::size_t>(new_rows), Cell());
		row_breaks_.assign(static_cast<std::size_t>(new_rows), LineBreak::kUnknown);

		for (int row = 0; row < new_rows; ++row) {
			const std::size_t source = static_cast<std::size_t>(grid_top + row);
			if (source >= rows.size()) break;

			const std::vector<Cell>& line = rows[source].cells;
			const std::size_t count = std::min(line.size(), width);
			std::copy(line.begin(), line.begin() + static_cast<std::ptrdiff_t>(count),
				cells_.begin()
					+ static_cast<std::ptrdiff_t>(static_cast<std::size_t>(row) * width));
			row_breaks_[static_cast<std::size_t>(row)] = rows[source].line_break;
		}

		scrollback_.clear();
		for (int row = 0; row < grid_top; ++row) {
			scrollback_.push_back(std::move(rows[static_cast<std::size_t>(row)]));
		}
	}

	void Screen::clearAll() {
		cells_.assign(cells_.size(), Cell());
		row_breaks_.assign(static_cast<std::size_t>(rows_), LineBreak::kUnknown);
		markAllDirty();
	}

	void Screen::clearScrollback() {
		const int dropped = scrollbackRows();
		scrollback_.clear();
		cold_.clear();
		shiftBlocksUp(dropped);
	}

	// Memory holds the newest rows; past the limit the oldest go to disk a
	// chunk at a time, and only when the disk cannot take them are they
	// forgotten instead.
	void Screen::trimScrollback() {
		while (rowsToSwapOut() > 0) {
			const std::size_t before = scrollback_.size();
			swapOutOldestRows();
			if (scrollback_.size() == before) break;
		}

		while (cold_.bytesOnDisk() > disk_limit_) {
			const int dropped = cold_.dropOldestChunk();
			if (dropped == 0) break;
			shiftBlocksUp(dropped);
		}
	}

	std::size_t Screen::rowsToSwapOut() const {
		if (scrollback_.size() <= scrollback_limit_ + kSwapChunkRows) return 0;
		return kSwapChunkRows;
	}

	static bool rowGoesOnInHistory(const HistoryLine& line) {
		if (line.line_break == LineBreak::kWraps) return true;
		if (line.line_break == LineBreak::kEnds || line.cells.empty()) return false;

		const Cell& last = line.cells.back();
		return last.code != U' ' || (last.attributes & (kAttrWide | kAttrWideTail)) != 0;
	}

	// Whole lines go to disk, so the chunk stretches to where the last
	// line ends. A stored line spans exactly the rows it did here: a line
	// shorter than its rows would leave everything below it renumbered,
	// so it is padded out to keep its row count.
	std::vector<std::vector<Cell>> Screen::joinRowsForStorage(std::size_t count,
			std::size_t& out_consumed) const {
		std::vector<std::vector<Cell>> lines;
		std::vector<Cell> line;
		int rows_in_line = 0;
		out_consumed = 0;

		for (std::size_t index = 0; index < scrollback_.size(); ++index) {
			const HistoryLine& row = scrollback_[index];
			const bool goes_on = rowGoesOnInHistory(row) && index + 1 < scrollback_.size();
			++rows_in_line;

			if (goes_on) {
				line.insert(line.end(), row.cells.begin(), row.cells.end());
				continue;
			}

			std::size_t ink = row.cells.size();
			while (ink > 0 && !reflow::cellHasInk(row.cells[ink - 1])) --ink;
			line.insert(line.end(), row.cells.begin(),
				row.cells.begin() + static_cast<std::ptrdiff_t>(ink));

			const std::size_t needed = lengthSpanningRows(rows_in_line, columns_);
			if (line.size() < needed) line.resize(needed, Cell());

			lines.push_back(std::move(line));
			line.clear();
			rows_in_line = 0;
			out_consumed = index + 1;
			if (out_consumed >= count) break;
		}

		return lines;
	}

	void Screen::swapOutOldestRows() {
		std::size_t consumed = 0;
		const std::vector<std::vector<Cell>> lines = joinRowsForStorage(rowsToSwapOut(), consumed);
		if (consumed == 0) return;

		if (!cold_.append(lines)) shiftBlocksUp(static_cast<int>(consumed));
		scrollback_.erase(scrollback_.begin(),
			scrollback_.begin() + static_cast<std::ptrdiff_t>(consumed));
	}

	void Screen::setScrollbackLimit(int lines) {
		scrollback_limit_ = static_cast<std::size_t>(std::max(0, lines));
		trimScrollback();
	}

	void Screen::setHistoryDiskLimit(std::uint64_t bytes) {
		disk_limit_ = bytes;
		trimScrollback();
	}

	Cell& Screen::at(int row, int column) {
		const std::size_t index = static_cast<std::size_t>(row) * static_cast<std::size_t>(columns_)
			+ static_cast<std::size_t>(column);
		return cells_[index];
	}

	static bool sameCell(const Cell& left, const Cell& right) {
		return left.code == right.code && left.foreground == right.foreground
			&& left.background == right.background && left.attributes == right.attributes;
	}

	// The console repaints the grid after a resize with the very text the
	// reflow laid out; a write that changes nothing keeps what the reflow
	// knew about the row's line break.
	void Screen::putCell(int row, int column, const Cell& value) {
		Cell& target = at(row, column);
		if (!sameCell(target, value)) {
			row_breaks_[static_cast<std::size_t>(row)] = LineBreak::kUnknown;
		}

		target = value;
	}

	const Cell& Screen::cell(int row, int column) const {
		const std::size_t index = static_cast<std::size_t>(row) * static_cast<std::size_t>(columns_)
			+ static_cast<std::size_t>(column);
		return cells_[index];
	}

	const Cell& Screen::cellAt(int absolute_row, int column) const {
		const int history = scrollbackRows();
		if (absolute_row >= history) return cell(absolute_row - history, column);

		const int cold = cold_.rows();
		if (absolute_row < cold) return cold_.cellAt(absolute_row, column);

		static const Cell blank;
		const std::size_t hot_index = static_cast<std::size_t>(absolute_row - cold);
		const std::vector<Cell>& line = scrollback_[hot_index].cells;
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
			row_breaks_[static_cast<std::size_t>(cursor_.row)] = LineBreak::kWraps;
			carriageReturn();
			lineFeed();
			wrap_pending_ = false;
		}

		if (width == 2 && cursor_.column + 1 >= columns_) {
			clearRow(cursor_.row, cursor_.column, columns_ - 1);
			carriageReturn();
			lineFeed();
		}

		Cell glyph = pen_;
		glyph.code = code;
		if (width == 2) glyph.attributes |= kAttrWide;
		putCell(cursor_.row, cursor_.column, glyph);
		markDirty(cursor_.row);

		if (width == 2 && cursor_.column + 1 < columns_) {
			Cell tail = pen_;
			tail.code = U' ';
			tail.attributes |= kAttrWideTail;
			putCell(cursor_.row, cursor_.column + 1, tail);
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

		Cell blank;
		blank.background = pen_.background;
		for (int column = first; column <= last; ++column) putCell(row, column, blank);

		markDirty(row);
	}

	void Screen::pushToScrollback(int row) {
		if (alt_screen_) return;
		if (scroll_top_ != 0 || scroll_bottom_ != rows_ - 1) return;

		const std::size_t start =
			static_cast<std::size_t>(row) * static_cast<std::size_t>(columns_);
		HistoryLine line;
		line.cells.assign(cells_.begin() + static_cast<std::ptrdiff_t>(start),
			cells_.begin() + static_cast<std::ptrdiff_t>(start)
				+ static_cast<std::ptrdiff_t>(columns_));
		line.line_break = row_breaks_[static_cast<std::size_t>(row)];
		scrollback_.push_back(std::move(line));

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

				row_breaks_[static_cast<std::size_t>(row)] =
					row_breaks_[static_cast<std::size_t>(row) + 1];
				markDirty(row);
			}

			clearRow(scroll_bottom_, 0, columns_ - 1);
			row_breaks_[static_cast<std::size_t>(scroll_bottom_)] = LineBreak::kUnknown;
		}
	}

	void Screen::scrollDown(int count) {
		const int lines = std::max(1, count);
		for (int step = 0; step < lines; ++step) {
			for (int row = scroll_bottom_; row > scroll_top_; --row) {
				for (int column = 0; column < columns_; ++column) {
					at(row, column) = cell(row - 1, column);
				}

				row_breaks_[static_cast<std::size_t>(row)] =
					row_breaks_[static_cast<std::size_t>(row) - 1];
				markDirty(row);
			}

			clearRow(scroll_top_, 0, columns_ - 1);
			row_breaks_[static_cast<std::size_t>(scroll_top_)] = LineBreak::kUnknown;
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
			row_breaks_[static_cast<std::size_t>(cursor_.row)] = LineBreak::kUnknown;
			break;
		case 'P':
			for (int column = cursor_.column; column < columns_; ++column) {
				at(cursor_.row, column) = column + count < columns_
					? cell(cursor_.row, column + count)
					: Cell();
			}

			row_breaks_[static_cast<std::size_t>(cursor_.row)] = LineBreak::kUnknown;
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
		primary_breaks_ = row_breaks_;
		alt_screen_     = true;

		cells_.assign(cells_.size(), Cell());
		row_breaks_.assign(row_breaks_.size(), LineBreak::kUnknown);
		moveCursor(0, 0);
		markAllDirty();
	}

	void Screen::leaveAltScreen() {
		if (!alt_screen_) return;

		alt_screen_ = false;
		if (primary_cells_.size() == cells_.size()) {
			cells_      = primary_cells_;
			row_breaks_ = primary_breaks_;
		}

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

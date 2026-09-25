#pragma once

/**
 * @file history.h
 * @brief Scrollback that has left memory: lines kept in a file, read back on demand.
 */

#include "cell.h"

#include <windows.h>

#include <cstdint>
#include <deque>
#include <vector>

namespace wbshterm {

	/** Where an absolute row falls inside the stored lines. */
	struct HistoryLocation {
		int line   = 0;
		int offset = 0;
	};

	/**
	 * @brief Lines the grid has scrolled past, stored whole and wrapped on the way back.
	 *
	 * Rows are appended a chunk at a time as unwrapped lines, so a resize
	 * needs only the lengths kept here to know how many rows each chunk
	 * spans at the new width; the cells themselves are read from the file
	 * when a row is asked for and let go of again with releaseCache().
	 * Without a file (a temp folder that cannot be written) append() fails
	 * and the caller keeps its rows in memory.
	 */
	class HistoryStore {
	public:
		HistoryStore() = default;
		~HistoryStore();

		HistoryStore(const HistoryStore&)            = delete;
		HistoryStore& operator=(const HistoryStore&) = delete;

		void setColumns(int columns);
		int  columns() const { return columns_; }

		/** Rows the stored lines span at the current width. */
		int rows() const { return total_rows_; }
		int lineCount() const { return line_count_; }

		/** Stores one chunk; false leaves the lines with the caller. */
		bool append(const std::vector<std::vector<Cell>>& lines);

		const Cell& cellAt(int row, int column) const;

		HistoryLocation locate(int row) const;
		int rowOf(HistoryLocation location) const;

		/** The line's cells, as stored; empty when it cannot be read. */
		const std::vector<Cell>& line(int index) const;

		/** Rows the oldest chunk spanned, now gone; 0 when there is none. */
		int dropOldestChunk();

		std::uint64_t bytesOnDisk() const { return bytes_on_disk_; }

		void releaseCache() const;
		void clear();

	private:
		struct Chunk {
			std::uint64_t              offset     = 0;
			std::uint32_t              bytes      = 0;
			int                        first_line = 0;
			int                        first_row  = 0;
			int                        rows       = 0;
			std::vector<std::uint32_t> lengths;
		};

		struct Loaded {
			int                            chunk = -1;
			std::vector<std::vector<Cell>> lines;
		};

		bool ensureOpen();
		bool writeChunk(const std::vector<char>& bytes, std::uint64_t& out_offset);
		bool readChunk(const Chunk& chunk, std::vector<char>& out_bytes) const;
		const Loaded* loaded(int chunk) const;
		void recountRows();
		int rowsOfLength(std::uint32_t length) const;
		std::uint32_t lengthOfLine(int line) const;
		int chunkOfRow(int row) const;
		int chunkOfLine(int line) const;
		HistoryLocation walkToRow(int row) const;
		bool stepCachedLocation(int row) const;

		HANDLE                    file_          = INVALID_HANDLE_VALUE;
		bool                      unavailable_   = false;
		int                       columns_       = 80;
		int                       total_rows_    = 0;
		int                       line_count_    = 0;
		std::uint64_t             file_end_      = 0;
		std::uint64_t             bytes_on_disk_ = 0;
		std::deque<Chunk>         chunks_;
		mutable std::deque<Loaded> cache_;
		mutable HistoryLocation   cached_location_;
		mutable int               cached_row_       = -1;
		mutable int               cached_line_first_ = 0;
		mutable int               cached_line_rows_  = 0;
	};

	/** The length a line needs to span exactly @p rows at @p columns. */
	std::uint32_t lengthSpanningRows(int rows, int columns);

} /* namespace wbshterm */

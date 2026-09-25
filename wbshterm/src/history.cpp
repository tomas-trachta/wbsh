/**
 * @file history.cpp
 * @brief Chunked, run-length encoded lines in a temp file, with a length index in memory.
 */

#include "history.h"

#include <algorithm>
#include <cstring>

namespace wbshterm {

	static const std::size_t kLoadedChunkLimit = 4;

	std::uint32_t lengthSpanningRows(int rows, int columns) {
		if (rows <= 1) return 0;
		return static_cast<std::uint32_t>(rows - 1) * static_cast<std::uint32_t>(columns) + 1;
	}

	namespace encoding {

		static void putU32(std::vector<char>& out, std::uint32_t value) {
			const std::size_t at = out.size();
			out.resize(at + sizeof(value));
			std::memcpy(&out[at], &value, sizeof(value));
		}

		static void putU16(std::vector<char>& out, std::uint16_t value) {
			const std::size_t at = out.size();
			out.resize(at + sizeof(value));
			std::memcpy(&out[at], &value, sizeof(value));
		}

		static bool sameStyle(const Cell& left, const Cell& right) {
			return left.foreground == right.foreground && left.background == right.background
				&& left.attributes == right.attributes;
		}

		static void putStyleRuns(std::vector<char>& out, const std::vector<Cell>& line) {
			std::vector<std::size_t> starts;
			for (std::size_t i = 0; i < line.size(); ++i) {
				if (i == 0 || !sameStyle(line[i - 1], line[i])) starts.push_back(i);
			}

			putU32(out, static_cast<std::uint32_t>(starts.size()));
			for (std::size_t run = 0; run < starts.size(); ++run) {
				const std::size_t end = run + 1 < starts.size() ? starts[run + 1] : line.size();
				const Cell& style = line[starts[run]];
				putU32(out, style.foreground);
				putU32(out, style.background);
				putU16(out, style.attributes);
				putU32(out, static_cast<std::uint32_t>(end - starts[run]));
			}
		}

		static void putLine(std::vector<char>& out, const std::vector<Cell>& line) {
			putU32(out, static_cast<std::uint32_t>(line.size()));
			putStyleRuns(out, line);
			for (const Cell& cell : line) putU32(out, static_cast<std::uint32_t>(cell.code));
		}

		class Reader {
		public:
			explicit Reader(const std::vector<char>& bytes) : bytes_(bytes) {}

			bool u32(std::uint32_t& out) { return take(&out, sizeof(out)); }
			bool u16(std::uint16_t& out) { return take(&out, sizeof(out)); }

		private:
			bool take(void* out, std::size_t size) {
				if (at_ + size > bytes_.size()) return false;
				std::memcpy(out, &bytes_[at_], size);
				at_ += size;
				return true;
			}

			const std::vector<char>& bytes_;
			std::size_t              at_ = 0;
		};

		static bool getStyleRuns(Reader& in, std::vector<Cell>& line) {
			std::uint32_t runs = 0;
			if (!in.u32(runs)) return false;

			std::size_t filled = 0;
			for (std::uint32_t run = 0; run < runs; ++run) {
				Cell style;
				std::uint32_t count = 0;
				if (!in.u32(style.foreground) || !in.u32(style.background)) return false;
				if (!in.u16(style.attributes) || !in.u32(count)) return false;
				if (filled + count > line.size()) return false;

				std::fill(line.begin() + static_cast<std::ptrdiff_t>(filled),
					line.begin() + static_cast<std::ptrdiff_t>(filled + count), style);
				filled += count;
			}

			return filled == line.size();
		}

		static bool getLine(Reader& in, std::vector<Cell>& line) {
			std::uint32_t length = 0;
			if (!in.u32(length)) return false;

			line.assign(length, Cell());
			if (!getStyleRuns(in, line)) return false;

			for (Cell& cell : line) {
				std::uint32_t code = 0;
				if (!in.u32(code)) return false;
				cell.code = static_cast<char32_t>(code);
			}

			return true;
		}

	} /* namespace encoding */

	HistoryStore::~HistoryStore() {
		if (file_ != INVALID_HANDLE_VALUE) ::CloseHandle(file_);
	}

	// The file is deleted by the system when the handle closes, so a
	// crash leaves nothing behind either.
	bool HistoryStore::ensureOpen() {
		if (file_ != INVALID_HANDLE_VALUE) return true;
		if (unavailable_) return false;

		wchar_t folder[MAX_PATH];
		wchar_t path[MAX_PATH];
		if (::GetTempPathW(MAX_PATH, folder) == 0
				|| ::GetTempFileNameW(folder, L"wbh", 0, path) == 0) {
			unavailable_ = true;
			return false;
		}

		file_ = ::CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
			FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
		unavailable_ = file_ == INVALID_HANDLE_VALUE;
		return !unavailable_;
	}

	bool HistoryStore::writeChunk(const std::vector<char>& bytes, std::uint64_t& out_offset) {
		if (!ensureOpen()) return false;

		LARGE_INTEGER where;
		where.QuadPart = static_cast<LONGLONG>(file_end_);
		if (::SetFilePointerEx(file_, where, nullptr, FILE_BEGIN) == 0) return false;

		DWORD written = 0;
		const DWORD wanted = static_cast<DWORD>(bytes.size());
		if (::WriteFile(file_, bytes.data(), wanted, &written, nullptr) == 0) return false;
		if (written != wanted) return false;

		out_offset = file_end_;
		file_end_ += wanted;
		return true;
	}

	bool HistoryStore::readChunk(const Chunk& chunk, std::vector<char>& out_bytes) const {
		if (file_ == INVALID_HANDLE_VALUE) return false;

		LARGE_INTEGER where;
		where.QuadPart = static_cast<LONGLONG>(chunk.offset);
		if (::SetFilePointerEx(file_, where, nullptr, FILE_BEGIN) == 0) return false;

		out_bytes.resize(chunk.bytes);
		DWORD got = 0;
		if (::ReadFile(file_, out_bytes.data(), chunk.bytes, &got, nullptr) == 0) return false;
		return got == chunk.bytes;
	}

	int HistoryStore::rowsOfLength(std::uint32_t length) const {
		const std::uint32_t width = static_cast<std::uint32_t>(std::max(1, columns_));
		return std::max(1, static_cast<int>((length + width - 1) / width));
	}

	void HistoryStore::recountRows() {
		int first_row  = 0;
		int first_line = 0;
		for (Chunk& chunk : chunks_) {
			chunk.first_row  = first_row;
			chunk.first_line = first_line;
			chunk.rows       = 0;
			for (const std::uint32_t length : chunk.lengths) chunk.rows += rowsOfLength(length);

			first_row  += chunk.rows;
			first_line += static_cast<int>(chunk.lengths.size());
		}

		total_rows_  = first_row;
		line_count_  = first_line;
		cached_row_  = -1;
	}

	void HistoryStore::setColumns(int columns) {
		columns_ = std::max(1, columns);
		recountRows();
	}

	bool HistoryStore::append(const std::vector<std::vector<Cell>>& lines) {
		if (lines.empty()) return true;

		std::vector<char> bytes;
		for (const std::vector<Cell>& line : lines) encoding::putLine(bytes, line);

		Chunk chunk;
		if (!writeChunk(bytes, chunk.offset)) return false;

		chunk.bytes = static_cast<std::uint32_t>(bytes.size());
		for (const std::vector<Cell>& line : lines) {
			chunk.lengths.push_back(static_cast<std::uint32_t>(line.size()));
		}

		bytes_on_disk_ += chunk.bytes;
		chunks_.push_back(std::move(chunk));
		recountRows();
		return true;
	}

	int HistoryStore::dropOldestChunk() {
		if (chunks_.empty()) return 0;

		const int rows = chunks_.front().rows;
		bytes_on_disk_ -= chunks_.front().bytes;
		chunks_.pop_front();

		for (Loaded& entry : cache_) --entry.chunk;
		cache_.erase(std::remove_if(cache_.begin(), cache_.end(),
			[](const Loaded& entry) { return entry.chunk < 0; }), cache_.end());

		recountRows();
		return rows;
	}

	void HistoryStore::releaseCache() const {
		cache_.clear();
		cached_row_ = -1;
	}

	void HistoryStore::clear() {
		chunks_.clear();
		releaseCache();
		file_end_      = 0;
		bytes_on_disk_ = 0;
		recountRows();
	}

	int HistoryStore::chunkOfRow(int row) const {
		int low  = 0;
		int high = static_cast<int>(chunks_.size()) - 1;
		while (low < high) {
			const int middle = (low + high + 1) / 2;
			if (chunks_[static_cast<std::size_t>(middle)].first_row <= row) low = middle;
			else high = middle - 1;
		}

		return low;
	}

	int HistoryStore::chunkOfLine(int line) const {
		int low  = 0;
		int high = static_cast<int>(chunks_.size()) - 1;
		while (low < high) {
			const int middle = (low + high + 1) / 2;
			if (chunks_[static_cast<std::size_t>(middle)].first_line <= line) low = middle;
			else high = middle - 1;
		}

		return low;
	}

	std::uint32_t HistoryStore::lengthOfLine(int line) const {
		const Chunk& chunk = chunks_[static_cast<std::size_t>(chunkOfLine(line))];
		const std::size_t within = static_cast<std::size_t>(line - chunk.first_line);
		return within < chunk.lengths.size() ? chunk.lengths[within] : 0;
	}

	HistoryLocation HistoryStore::walkToRow(int row) const {
		const Chunk& chunk = chunks_[static_cast<std::size_t>(chunkOfRow(row))];

		int line_first = chunk.first_row;
		int line       = chunk.first_line;
		for (const std::uint32_t length : chunk.lengths) {
			const int rows = rowsOfLength(length);
			if (row < line_first + rows) {
				cached_line_first_ = line_first;
				cached_line_rows_  = rows;
				return { line, row - line_first };
			}

			line_first += rows;
			++line;
		}

		cached_line_first_ = line_first;
		cached_line_rows_  = 1;
		return { line, 0 };
	}

	// Rows are asked for one after another, so the last answer is usually
	// a line or two away from the next question.
	bool HistoryStore::stepCachedLocation(int row) const {
		if (cached_row_ < 0) return false;

		for (int steps = 0; steps < 8; ++steps) {
			if (row >= cached_line_first_ && row < cached_line_first_ + cached_line_rows_) {
				cached_location_ = { cached_location_.line, row - cached_line_first_ };
				return true;
			}

			if (row >= cached_line_first_ + cached_line_rows_) {
				const int next = cached_location_.line + 1;
				if (next >= line_count_) return false;

				cached_line_first_ += cached_line_rows_;
				cached_location_.line = next;
				cached_line_rows_ = rowsOfLength(lengthOfLine(next));
			} else {
				const int previous = cached_location_.line - 1;
				if (previous < 0) return false;

				cached_line_rows_ = rowsOfLength(lengthOfLine(previous));
				cached_line_first_ -= cached_line_rows_;
				cached_location_.line = previous;
			}
		}

		return false;
	}

	HistoryLocation HistoryStore::locate(int row) const {
		if (chunks_.empty() || row < 0 || row >= total_rows_) return { 0, 0 };
		if (row == cached_row_) return cached_location_;

		if (!stepCachedLocation(row)) cached_location_ = walkToRow(row);
		cached_row_ = row;
		return cached_location_;
	}

	int HistoryStore::rowOf(HistoryLocation location) const {
		if (chunks_.empty()) return 0;

		const Chunk& chunk = chunks_[static_cast<std::size_t>(chunkOfLine(location.line))];
		int row  = chunk.first_row;
		int line = chunk.first_line;
		for (const std::uint32_t length : chunk.lengths) {
			const int rows = rowsOfLength(length);
			if (line == location.line) return row + std::min(location.offset, rows - 1);

			row += rows;
			++line;
		}

		return std::min(row, std::max(0, total_rows_ - 1));
	}

	const HistoryStore::Loaded* HistoryStore::loaded(int index) const {
		for (const Loaded& entry : cache_) {
			if (entry.chunk == index) return &entry;
		}

		std::vector<char> bytes;
		if (!readChunk(chunks_[static_cast<std::size_t>(index)], bytes)) return nullptr;

		Loaded entry;
		entry.chunk = index;
		encoding::Reader reader(bytes);
		for (std::size_t i = 0; i < chunks_[static_cast<std::size_t>(index)].lengths.size(); ++i) {
			std::vector<Cell> line;
			if (!encoding::getLine(reader, line)) return nullptr;
			entry.lines.push_back(std::move(line));
		}

		while (cache_.size() >= kLoadedChunkLimit) cache_.pop_front();
		cache_.push_back(std::move(entry));
		return &cache_.back();
	}

	const std::vector<Cell>& HistoryStore::line(int index) const {
		static const std::vector<Cell> none;
		if (index < 0 || index >= line_count_) return none;

		const int chunk = chunkOfLine(index);
		const Loaded* entry = loaded(chunk);
		if (entry == nullptr) return none;

		const std::size_t within = static_cast<std::size_t>(
			index - chunks_[static_cast<std::size_t>(chunk)].first_line);
		return within < entry->lines.size() ? entry->lines[within] : none;
	}

	const Cell& HistoryStore::cellAt(int row, int column) const {
		static const Cell blank;
		if (row < 0 || row >= total_rows_ || column < 0 || column >= columns_) return blank;

		const HistoryLocation where = locate(row);
		const std::vector<Cell>& cells = line(where.line);
		const std::size_t index = static_cast<std::size_t>(where.offset)
			* static_cast<std::size_t>(columns_) + static_cast<std::size_t>(column);
		return index < cells.size() ? cells[index] : blank;
	}

} /* namespace wbshterm */

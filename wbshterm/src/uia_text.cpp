/**
 * @file uia_text.cpp
 * @brief Unit boundaries and text over a screen's cells, for the UI Automation ranges.
 */

#include "uia_text.h"

#include <algorithm>

namespace wbshterm {

	TextModel::TextModel(const Screen& screen, int page_rows)
		: screen_(screen),
		  columns_(std::max(1, screen.columns())),
		  page_rows_(std::max(1, page_rows)),
		  end_(static_cast<long long>(screen.totalRows()) * std::max(1, screen.columns())) {
	}

	long long TextModel::clamp(long long at) const {
		return std::min(std::max(at, 0LL), end_);
	}

	static int rowOf(long long at, int columns) {
		return static_cast<int>(at / columns);
	}

	static int columnOf(long long at, int columns) {
		return static_cast<int>(at % columns);
	}

	bool TextModel::isWideTail(long long at) const {
		const Cell& cell = screen_.cellAt(rowOf(at, columns_), columnOf(at, columns_));
		return (cell.attributes & kAttrWideTail) != 0;
	}

	bool TextModel::isBlank(long long at) const {
		const Cell& cell = screen_.cellAt(rowOf(at, columns_), columnOf(at, columns_));
		if ((cell.attributes & kAttrWideTail) != 0) return false;

		const char32_t code = screen_.baseCodeOf(cell);
		return code == U' ' || code == 0;
	}

	bool TextModel::startsParagraph(int row) const {
		return row == 0 || !screen_.rowWraps(row - 1);
	}

	bool TextModel::isBoundary(long long at, TextUnit unit) const {
		if (at <= 0 || at >= end_) return true;

		const bool row_start = columnOf(at, columns_) == 0;
		switch (unit) {
		case TextUnit::kCharacter:
			return !isWideTail(at);
		case TextUnit::kFormat:
		case TextUnit::kWord:
			return row_start || (!isBlank(at) && !isWideTail(at) && isBlank(at - 1));
		case TextUnit::kLine:
			return row_start;
		case TextUnit::kParagraph:
			return row_start && startsParagraph(rowOf(at, columns_));
		case TextUnit::kPage:
			return row_start && rowOf(at, columns_) % page_rows_ == 0;
		case TextUnit::kDocument:
			return false;
		default:
			return false;
		}
	}

	long long TextModel::rowStart(long long at) const {
		return static_cast<long long>(rowOf(at, columns_)) * columns_;
	}

	long long TextModel::paragraphStart(long long at) const {
		int row = rowOf(at, columns_);
		while (row > 0 && !startsParagraph(row)) --row;
		return static_cast<long long>(row) * columns_;
	}

	long long TextModel::nextParagraph(long long at) const {
		int row = rowOf(at, columns_) + 1;
		while (row < screen_.totalRows() && !startsParagraph(row)) ++row;
		return std::min(end_, static_cast<long long>(row) * columns_);
	}

	long long TextModel::scanForward(long long at, TextUnit unit) const {
		long long next = at + 1;
		while (next < end_ && !isBoundary(next, unit)) ++next;
		return next;
	}

	long long TextModel::scanBackward(long long at, TextUnit unit) const {
		long long previous = at - 1;
		while (previous > 0 && !isBoundary(previous, unit)) --previous;
		return previous;
	}

	long long TextModel::nextBoundary(long long at, TextUnit unit) const {
		at = clamp(at);
		if (at >= end_) return end_;

		const long long page = static_cast<long long>(page_rows_) * columns_;
		switch (unit) {
		case TextUnit::kLine:
			return std::min(end_, rowStart(at) + columns_);
		case TextUnit::kParagraph:
			return nextParagraph(at);
		case TextUnit::kPage:
			return std::min(end_, (at / page + 1) * page);
		case TextUnit::kDocument:
			return end_;
		default:
			return scanForward(at, unit);
		}
	}

	long long TextModel::previousBoundary(long long at, TextUnit unit) const {
		at = clamp(at);
		if (at <= 0) return 0;

		const long long page = static_cast<long long>(page_rows_) * columns_;
		switch (unit) {
		case TextUnit::kLine:
			return rowStart(at - 1);
		case TextUnit::kParagraph:
			return paragraphStart(at - 1);
		case TextUnit::kPage:
			return ((at - 1) / page) * page;
		case TextUnit::kDocument:
			return 0;
		default:
			return scanBackward(at, unit);
		}
	}

	long long TextModel::unitStart(long long at, TextUnit unit) const {
		at = clamp(at);
		if (at == end_) return previousBoundary(at, unit);
		if (isBoundary(at, unit)) return at;
		return previousBoundary(at, unit);
	}

	int TextModel::move(long long& at, TextUnit unit, int count) const {
		at = clamp(at);

		int moved = 0;
		for (; count > 0 && at < end_; --count, ++moved) at = nextBoundary(at, unit);
		for (; count < 0 && at > 0; ++count, --moved) at = previousBoundary(at, unit);
		return moved;
	}

	static void appendCode(std::wstring& out, char32_t code) {
		if (code < 0x10000) {
			out.push_back(static_cast<wchar_t>(code == 0 ? U' ' : code));
			return;
		}

		const char32_t offset = code - 0x10000;
		out.push_back(static_cast<wchar_t>(0xD800 + (offset >> 10)));
		out.push_back(static_cast<wchar_t>(0xDC00 + (offset & 0x3FF)));
	}

	static void notePositions(std::vector<long long>* out_positions, std::size_t count,
			long long at) {
		if (out_positions != nullptr) out_positions->resize(count, at);
	}

	// What the row shows from @p first up to @p last, the end excluded.
	void TextModel::appendRow(int row, int first, int last, std::wstring& out,
			std::vector<long long>* out_positions) const {
		const long long row_start = static_cast<long long>(row) * columns_;
		const bool breaks = last == columns_ && !screen_.rowWraps(row);

		int stop = last;
		while (breaks && stop > first && isBlank(row_start + stop - 1)) --stop;

		for (int column = first; column < stop; ++column) {
			const Cell& cell = screen_.cellAt(row, column);
			if ((cell.attributes & kAttrWideTail) != 0) continue;

			const std::u32string* cluster = screen_.clusterOf(cell);
			if (cluster == nullptr) {
				appendCode(out, screen_.baseCodeOf(cell));
			} else {
				for (const char32_t code : *cluster) appendCode(out, code);
			}

			notePositions(out_positions, out.size(), row_start + column);
		}

		if (!breaks || row + 1 >= screen_.totalRows()) return;

		out += L"\r\n";
		notePositions(out_positions, out.size(), row_start + stop);
	}

	std::wstring TextModel::text(long long start, long long end, int max_length,
			std::vector<long long>* out_positions) const {
		start = clamp(start);
		end   = clamp(end);

		std::wstring out;
		if (out_positions != nullptr) out_positions->clear();
		if (start >= end) return out;

		const int first_row = rowOf(start, columns_);
		const int last_row  = rowOf(end - 1, columns_);
		const std::size_t limit = max_length < 0
			? out.max_size()
			: static_cast<std::size_t>(max_length);
		for (int row = first_row; row <= last_row && out.size() < limit; ++row) {
			const int first = row == first_row ? columnOf(start, columns_) : 0;
			const long long row_end = static_cast<long long>(row + 1) * columns_;
			const int last = end >= row_end ? columns_ : columnOf(end, columns_);
			appendRow(row, first, last, out, out_positions);
		}

		if (out.size() > limit) out.resize(limit);
		if (out_positions != nullptr && out_positions->size() > out.size()) {
			out_positions->resize(out.size());
		}

		return out;
	}

} /* namespace wbshterm */

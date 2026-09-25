/**
 * @file search.cpp
 * @brief Row-by-row, case-folded matching over the whole session.
 */

#include "search.h"

#include <algorithm>
#include <vector>

namespace wbshterm {

	static char32_t folded(char32_t code) {
		if (code >= U'A' && code <= U'Z') return code + (U'a' - U'A');
		return code;
	}

	struct RowGlyph {
		int      column;
		char32_t code;
	};

	static std::vector<RowGlyph> glyphsOfRow(const Screen& screen, int row) {
		std::vector<RowGlyph> glyphs;
		for (int column = 0; column < screen.columns(); ++column) {
			const Cell& cell = screen.cellAt(row, column);
			if ((cell.attributes & kAttrWideTail) != 0) continue;
			glyphs.push_back({ column, folded(cell.code) });
		}

		return glyphs;
	}

	void SearchBox::open() {
		active_   = true;
		missed_   = false;
		have_hit_ = false;
	}

	void SearchBox::close() {
		active_ = false;
	}

	void SearchBox::typeCharacter(wchar_t character) {
		if (character < 0x20) return;
		query_.push_back(folded(static_cast<char32_t>(character)));
		missed_   = false;
		have_hit_ = false;
	}

	void SearchBox::backspace() {
		if (!query_.empty()) query_.pop_back();
		missed_   = false;
		have_hit_ = false;
	}

	std::wstring SearchBox::caption() const {
		std::wstring text = L"find: ";
		for (const char32_t code : query_) {
			if (code < 0x10000) text.push_back(static_cast<wchar_t>(code));
			else text.push_back(L'?');
		}

		if (missed_) text += L"  (no match)";
		return text;
	}

	bool SearchBox::findInRow(const Screen& screen, int row, int from_column, bool backwards,
			SearchHit& out_hit) const {
		const std::vector<RowGlyph> glyphs = glyphsOfRow(screen, row);
		const int count = static_cast<int>(glyphs.size());
		const int span  = static_cast<int>(query_.size());
		if (span == 0 || count < span) return false;

		const auto matchesAt = [&](int start) {
			for (int i = 0; i < span; ++i) {
				const char32_t code = glyphs[static_cast<std::size_t>(start + i)].code;
				if (code != query_[static_cast<std::size_t>(i)]) return false;
			}

			return true;
		};

		const auto hitAt = [&](int start) {
			const RowGlyph& first = glyphs[static_cast<std::size_t>(start)];
			const RowGlyph& last  = glyphs[static_cast<std::size_t>(start + span - 1)];
			out_hit = { row, first.column, last.column - first.column + 1 };
			return true;
		};

		if (backwards) {
			for (int start = count - span; start >= 0; --start) {
				if (glyphs[static_cast<std::size_t>(start)].column > from_column) continue;
				if (matchesAt(start)) return hitAt(start);
			}

			return false;
		}

		for (int start = 0; start + span <= count; ++start) {
			if (glyphs[static_cast<std::size_t>(start)].column < from_column) continue;
			if (matchesAt(start)) return hitAt(start);
		}

		return false;
	}

	int SearchBox::firstRow(const Screen& screen, const TerminalView& view, bool backwards) const {
		if (have_hit_) return last_hit_.row;

		const int top = view.topRow(screen);
		return backwards ? top + screen.rows() - 1 : top;
	}

	int SearchBox::firstColumn(const Screen& screen, bool backwards) const {
		if (have_hit_) return backwards ? last_hit_.column - 1 : last_hit_.column + 1;
		return backwards ? screen.columns() : 0;
	}

	bool SearchBox::find(const Screen& screen, const TerminalView& view, bool backwards,
			SearchHit& out_hit) {
		if (query_.empty()) return false;

		const int total = screen.totalRows();
		int row    = std::min(std::max(firstRow(screen, view, backwards), 0), total - 1);
		int column = firstColumn(screen, backwards);
		const int step = backwards ? -1 : 1;

		for (; row >= 0 && row < total; row += step) {
			if (findInRow(screen, row, column, backwards, out_hit)) {
				last_hit_ = out_hit;
				have_hit_ = true;
				missed_   = false;
				return true;
			}

			column = backwards ? screen.columns() : 0;
		}

		missed_ = true;
		return false;
	}

} /* namespace wbshterm */

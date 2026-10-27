/**
 * @file search.cpp
 * @brief Line-by-line, case-folded matching over the whole session, a slice at a time.
 */

#include "search.h"

#include <windows.h>

#include <algorithm>

namespace wbshterm {

	namespace search_detail {

		static const int kBmpSize = 0x10000;

		static bool mapCase(DWORD flags, const wchar_t* source, wchar_t* target, int count) {
			return ::LCMapStringEx(LOCALE_NAME_INVARIANT, flags, source, count, target, count,
				nullptr, nullptr, 0) == count;
		}

		static void foldRange(std::vector<wchar_t>& table, int first, int end) {
			const int count = end - first;
			std::vector<wchar_t> upper(static_cast<std::size_t>(count));
			std::vector<wchar_t> lower(static_cast<std::size_t>(count));
			if (!mapCase(LCMAP_UPPERCASE, &table[static_cast<std::size_t>(first)], upper.data(),
					count)) {
				return;
			}

			if (!mapCase(LCMAP_LOWERCASE, upper.data(), lower.data(), count)) return;
			std::copy(lower.begin(), lower.end(), table.begin() + first);
		}

		// Surrogate code units are left out: alone they are no characters
		// to map, and the call would refuse the whole range for them.
		static std::vector<wchar_t> buildBmpFolding() {
			std::vector<wchar_t> table(static_cast<std::size_t>(kBmpSize));
			for (int code = 0; code < kBmpSize; ++code) {
				table[static_cast<std::size_t>(code)] = static_cast<wchar_t>(code);
			}

			foldRange(table, 1, 0xD800);
			foldRange(table, 0xE000, kBmpSize);
			return table;
		}

		static char32_t foldSupplementary(char32_t code) {
			const char32_t offset = code - 0x10000;
			const wchar_t pair[2] = { static_cast<wchar_t>(0xD800 + (offset >> 10)),
				static_cast<wchar_t>(0xDC00 + (offset & 0x3FF)) };
			wchar_t upper[2];
			wchar_t lower[2];
			if (!mapCase(LCMAP_UPPERCASE, pair, upper, 2)) return code;
			if (!mapCase(LCMAP_LOWERCASE, upper, lower, 2)) return code;

			const bool joined = lower[0] >= 0xD800 && lower[0] < 0xDC00
				&& lower[1] >= 0xDC00 && lower[1] < 0xE000;
			if (!joined) return code;
			return 0x10000 + ((static_cast<char32_t>(lower[0]) - 0xD800) << 10)
				+ (static_cast<char32_t>(lower[1]) - 0xDC00);
		}

		static bool comesBefore(int row, int column, int other_row, int other_column) {
			return row < other_row || (row == other_row && column < other_column);
		}

		static int lineStartOf(const Screen& screen, int row) {
			while (row > 0 && screen.rowWraps(row - 1)) --row;
			return row;
		}

	} /* namespace search_detail */

	char32_t foldCase(char32_t code) {
		if (code < 0x80) {
			if (code >= U'A' && code <= U'Z') return code + (U'a' - U'A');
			return code;
		}

		if (code >= 0xD800 && code < 0xE000) return code;
		if (code > 0x10FFFF) return code;
		if (code >= 0x10000) return search_detail::foldSupplementary(code);

		static const std::vector<wchar_t> table = search_detail::buildBmpFolding();
		return static_cast<char32_t>(table[static_cast<std::size_t>(code)]);
	}

	void SearchBox::open() {
		active_   = true;
		missed_   = false;
		have_hit_ = false;
	}

	void SearchBox::close() {
		active_    = false;
		searching_ = false;
	}

	void SearchBox::typeCharacter(char32_t character) {
		if (character < 0x20) return;
		query_.push_back(foldCase(character));
		missed_    = false;
		have_hit_  = false;
		searching_ = false;
	}

	void SearchBox::backspace() {
		if (!query_.empty()) query_.pop_back();
		missed_    = false;
		have_hit_  = false;
		searching_ = false;
	}

	static void appendUtf16(std::wstring& text, char32_t code) {
		if (code < 0x10000) {
			text.push_back(static_cast<wchar_t>(code));
			return;
		}

		const char32_t offset = code - 0x10000;
		text.push_back(static_cast<wchar_t>(0xD800 + (offset >> 10)));
		text.push_back(static_cast<wchar_t>(0xDC00 + (offset & 0x3FF)));
	}

	std::wstring SearchBox::caption() const {
		std::wstring text = L"find: ";
		for (const char32_t code : query_) appendUtf16(text, code);

		if (searching_) text += L"  (searching " + std::to_wstring(progress_) + L"%)";
		else if (missed_) text += L"  (no match)";
		return text;
	}

	void SearchBox::forgetStaleHit(const Screen& screen) {
		if (!have_hit_) return;

		last_hit_.row     -= static_cast<int>(screen.forgottenRows() - hit_forgotten_);
		last_hit_.end_row -= static_cast<int>(screen.forgottenRows() - hit_forgotten_);
		hit_forgotten_ = screen.forgottenRows();
		if (last_hit_.row < 0 || hit_columns_ != screen.columns()) have_hit_ = false;
	}

	SearchBox::Position SearchBox::startPosition(const Screen& screen,
			const TerminalView& view) const {
		if (have_hit_) {
			const int column = backwards_ ? last_hit_.column - 1 : last_hit_.column + 1;
			return { last_hit_.row, column };
		}

		const int top = view.topRow(screen);
		if (backwards_) return { top + screen.rows() - 1, screen.columns() };
		return { top, 0 };
	}

	void SearchBox::start(const Screen& screen, const TerminalView& view, bool backwards) {
		const int total = screen.totalRows();
		searching_ = !query_.empty() && total > 0;
		if (!searching_) return;

		backwards_ = backwards;
		columns_   = screen.columns();
		forgotten_ = screen.forgottenRows();
		progress_  = 0;
		forgetStaleHit(screen);

		from_       = startPosition(screen, view);
		from_.row   = std::min(std::max(from_.row, 0), total - 1);
		origin_row_ = from_.row;
		next_row_   = search_detail::lineStartOf(screen, from_.row);
	}

	// Rows scrolled out of history renumber every row below them, and a
	// new width rewraps every line, so the search starts over then.
	bool SearchBox::followScreen(const Screen& screen, const TerminalView& view) {
		if (screen.columns() != columns_) {
			have_hit_ = false;
			start(screen, view, backwards_);
			return searching_;
		}

		const int shift = static_cast<int>(screen.forgottenRows() - forgotten_);
		forgotten_ = screen.forgottenRows();
		next_row_   -= shift;
		from_.row   -= shift;
		origin_row_ -= shift;

		if (next_row_ >= 0) return true;
		next_row_ = 0;
		return !backwards_;
	}

	void SearchBox::appendRowGlyphs(const Screen& screen, int row) {
		for (int column = 0; column < screen.columns(); ++column) {
			const Cell& cell = screen.cellAt(row, column);
			if ((cell.attributes & kAttrWideTail) != 0) continue;
			line_.push_back({ row, column, foldCase(screen.baseCodeOf(cell)) });
		}
	}

	int SearchBox::gatherLine(const Screen& screen, int first_row) {
		line_.clear();

		const int total = screen.totalRows();
		int row = first_row;
		appendRowGlyphs(screen, row);
		while (row + 1 < total && screen.rowWraps(row)) {
			++row;
			appendRowGlyphs(screen, row);
		}

		return row;
	}

	bool SearchBox::matchesAt(std::size_t start) const {
		for (std::size_t i = 0; i < query_.size(); ++i) {
			if (line_[start + i].code != query_[i]) return false;
		}

		return true;
	}

	bool SearchBox::startAllowed(std::size_t start) const {
		const Glyph& glyph = line_[start];
		if (backwards_) return !search_detail::comesBefore(from_.row, from_.column, glyph.row,
			glyph.column);
		return !search_detail::comesBefore(glyph.row, glyph.column, from_.row, from_.column);
	}

	SearchHit SearchBox::hitAt(std::size_t start) const {
		const Glyph& first = line_[start];
		const Glyph& last  = line_[start + query_.size() - 1];
		return { first.row, first.column, last.row, last.column };
	}

	bool SearchBox::findInLine(SearchHit& out_hit) const {
		if (line_.size() < query_.size()) return false;

		const std::size_t starts = line_.size() - query_.size() + 1;
		for (std::size_t index = 0; index < starts; ++index) {
			const std::size_t start = backwards_ ? starts - 1 - index : index;
			if (!startAllowed(start) || !matchesAt(start)) continue;

			out_hit = hitAt(start);
			return true;
		}

		return false;
	}

	SearchStep SearchBox::scanLine(const Screen& screen, SearchHit& out_hit) {
		if (next_row_ < 0 || next_row_ >= screen.totalRows()) return SearchStep::kMissed;

		const int last_row = gatherLine(screen, next_row_);
		if (findInLine(out_hit)) return SearchStep::kFound;

		if (!backwards_) next_row_ = last_row + 1;
		else if (next_row_ == 0) next_row_ = -1;
		else next_row_ = search_detail::lineStartOf(screen, next_row_ - 1);
		return SearchStep::kPending;
	}

	SearchStep SearchBox::finish(bool found, const SearchHit& hit) {
		searching_ = false;
		missed_    = !found;
		line_.clear();
		if (!found) return SearchStep::kMissed;

		last_hit_      = hit;
		have_hit_      = true;
		hit_forgotten_ = forgotten_;
		hit_columns_   = columns_;
		return SearchStep::kFound;
	}

	void SearchBox::noteProgress(const Screen& screen) {
		const long long done = backwards_ ? origin_row_ - next_row_ : next_row_ - origin_row_;
		const long long span = backwards_ ? origin_row_ + 1 : screen.totalRows() - origin_row_;
		const long long percent = done * 100 / std::max(1LL, span);
		progress_ = static_cast<int>(std::min(99LL, std::max(0LL, percent)));
	}

	SearchStep SearchBox::step(const Screen& screen, const TerminalView& view,
			std::chrono::milliseconds budget, SearchHit& out_hit) {
		if (!searching_) return SearchStep::kMissed;
		if (!followScreen(screen, view)) return finish(false, out_hit);

		const auto deadline = std::chrono::steady_clock::now() + budget;
		do {
			const SearchStep result = scanLine(screen, out_hit);
			if (result == SearchStep::kFound) return finish(true, out_hit);
			if (result == SearchStep::kMissed) return finish(false, out_hit);
		} while (std::chrono::steady_clock::now() < deadline);

		noteProgress(screen);
		return SearchStep::kPending;
	}

	bool SearchBox::find(const Screen& screen, const TerminalView& view, bool backwards,
			SearchHit& out_hit) {
		start(screen, view, backwards);

		SearchStep result = SearchStep::kPending;
		while (result == SearchStep::kPending) {
			result = step(screen, view, std::chrono::milliseconds(1000), out_hit);
		}

		return result == SearchStep::kFound;
	}

} /* namespace wbshterm */

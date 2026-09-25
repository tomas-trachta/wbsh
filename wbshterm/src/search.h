#pragma once

/**
 * @file search.h
 * @brief Finding text anywhere in a session, disk history included.
 */

#include "screen.h"
#include "view.h"

#include <string>

namespace wbshterm {

	struct SearchHit {
		int row    = 0;
		int column = 0;
		int length = 0;
	};

	/**
	 * @brief The search prompt's state and the matching behind it.
	 *
	 * Matching is case-insensitive and runs over visual rows, so a hit
	 * never straddles a wrap. Each find() continues from the last hit;
	 * a changed query starts over from the window.
	 */
	class SearchBox {
	public:
		void open();
		void close();
		bool active() const { return active_; }

		void typeCharacter(wchar_t character);
		void backspace();

		/** What the box shows: the query, and whether the last find failed. */
		std::wstring caption() const;

		bool find(const Screen& screen, const TerminalView& view, bool backwards,
			SearchHit& out_hit);

	private:
		bool findInRow(const Screen& screen, int row, int from_column, bool backwards,
			SearchHit& out_hit) const;
		int  firstRow(const Screen& screen, const TerminalView& view, bool backwards) const;
		int  firstColumn(const Screen& screen, bool backwards) const;

		std::u32string query_;
		bool           active_    = false;
		bool           missed_    = false;
		bool           have_hit_  = false;
		SearchHit      last_hit_;
	};

} /* namespace wbshterm */

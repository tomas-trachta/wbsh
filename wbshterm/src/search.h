#pragma once

/**
 * @file search.h
 * @brief Finding text anywhere in a session, disk history included.
 */

#include "screen.h"
#include "view.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace wbshterm {

	/** A match from its first glyph to its last, which may sit on a later row. */
	struct SearchHit {
		int row        = 0;
		int column     = 0;
		int end_row    = 0;
		int end_column = 0;
	};

	enum class SearchStep {
		kFound,
		kMissed,
		kPending,
	};

	/** @p code as caseless matching compares it: the lowercase of its uppercase. */
	char32_t foldCase(char32_t code);

	/**
	 * @brief The search prompt's state and the matching behind it.
	 *
	 * Matching is caseless and runs over whole lines, so a match may run
	 * across a soft wrap. A search walks history a slice at a time with
	 * step(), so the window stays responsive and can cancel it; rows that
	 * scroll out of history meanwhile are followed, a new width starts the
	 * search over. Each search continues from the last hit; a changed
	 * query starts over from the view.
	 */
	class SearchBox {
	public:
		void open();
		void close();
		bool active() const { return active_; }

		void typeCharacter(char32_t character);
		void backspace();

		/** What the box shows: the query, and whether a search runs or failed. */
		std::wstring caption() const;

		void start(const Screen& screen, const TerminalView& view, bool backwards);
		bool searching() const { return searching_; }
		void cancel() { searching_ = false; }

		/** Searches on for about @p budget; kPending means call again. */
		SearchStep step(const Screen& screen, const TerminalView& view,
			std::chrono::milliseconds budget, SearchHit& out_hit);

		/** start() and step() until the search ends. */
		bool find(const Screen& screen, const TerminalView& view, bool backwards,
			SearchHit& out_hit);

	private:
		struct Position {
			int row    = 0;
			int column = 0;
		};

		struct Glyph {
			int      row;
			int      column;
			char32_t code;
		};

		void forgetStaleHit(const Screen& screen);
		Position startPosition(const Screen& screen, const TerminalView& view) const;
		bool followScreen(const Screen& screen, const TerminalView& view);
		SearchStep scanLine(const Screen& screen, SearchHit& out_hit);
		int  gatherLine(const Screen& screen, int first_row);
		void appendRowGlyphs(const Screen& screen, int row);
		bool findInLine(SearchHit& out_hit) const;
		bool matchesAt(std::size_t start) const;
		bool startAllowed(std::size_t start) const;
		SearchHit hitAt(std::size_t start) const;
		SearchStep finish(bool found, const SearchHit& hit);
		void noteProgress(const Screen& screen);

		std::u32string     query_;
		bool               active_   = false;
		bool               missed_   = false;
		bool               have_hit_ = false;
		SearchHit          last_hit_;
		std::uint64_t      hit_forgotten_ = 0;
		int                hit_columns_   = 0;

		bool               searching_ = false;
		bool               backwards_ = false;
		Position           from_;
		int                next_row_   = 0;
		int                origin_row_ = 0;
		int                columns_    = 0;
		std::uint64_t      forgotten_  = 0;
		int                progress_   = 0;
		std::vector<Glyph> line_;
	};

} /* namespace wbshterm */

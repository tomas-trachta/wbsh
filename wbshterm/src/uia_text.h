#pragma once

/**
 * @file uia_text.h
 * @brief A screen's text the way UI Automation ranges walk it: positions, units, text.
 */

#include "screen.h"

#include <string>
#include <vector>

namespace wbshterm {

	/** The units a screen reader moves by, in UI Automation's order. */
	enum class TextUnit {
		kCharacter,
		kFormat,
		kWord,
		kLine,
		kParagraph,
		kPage,
		kDocument,
	};

	/**
	 * @brief Positions in a screen's text: every cell of every row, in reading order.
	 *
	 * A position is row * columns + column over history and grid alike,
	 * so the document ends at totalRows() * columns. The tail cell of a
	 * wide character is never a stop. A unit the terminal has no notion
	 * of falls back to the next larger one, as UI Automation asks:
	 * format to word. A word takes the blanks after it, a line is a row,
	 * a paragraph is the rows a soft wrap joins, a page is a screenful.
	 */
	class TextModel {
	public:
		TextModel(const Screen& screen, int page_rows);

		long long end() const { return end_; }
		int columns() const { return columns_; }
		long long clamp(long long at) const;

		/** The start of the unit holding @p at; at the end, of the last unit. */
		long long unitStart(long long at, TextUnit unit) const;

		/** The first unit boundary after @p at, or the end. */
		long long nextBoundary(long long at, TextUnit unit) const;

		/** The last unit boundary before @p at, or 0. */
		long long previousBoundary(long long at, TextUnit unit) const;

		/** Moves @p at across up to @p count boundaries; how many it crossed, signed. */
		int move(long long& at, TextUnit unit, int count) const;

		/**
		 * @brief The text from @p start to @p end, at most @p max_length units (-1: all).
		 *
		 * A row the range runs to the end of drops its trailing blanks and
		 * ends in CR LF, unless the row wraps on into the next one.
		 * @p out_positions, when given, gets the position each UTF-16 unit
		 * came from.
		 */
		std::wstring text(long long start, long long end, int max_length,
			std::vector<long long>* out_positions) const;

	private:
		bool isBoundary(long long at, TextUnit unit) const;
		bool isWideTail(long long at) const;
		bool isBlank(long long at) const;
		bool startsParagraph(int row) const;
		long long rowStart(long long at) const;
		long long paragraphStart(long long at) const;
		long long nextParagraph(long long at) const;
		long long scanForward(long long at, TextUnit unit) const;
		long long scanBackward(long long at, TextUnit unit) const;
		void appendRow(int row, int first, int last, std::wstring& out,
			std::vector<long long>* out_positions) const;

		const Screen& screen_;
		int           columns_;
		int           page_rows_;
		long long     end_;
	};

} /* namespace wbshterm */

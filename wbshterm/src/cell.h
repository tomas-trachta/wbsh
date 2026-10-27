#pragma once

/**
 * @file cell.h
 * @brief One character cell of the grid, and the colour sentinels it uses.
 */

#include <cstdint>

namespace wbshterm {

	/** Colors are 0x00RRGGBB, or this sentinel for "whatever the theme says". */
	static const std::uint32_t kDefaultColor = 0xFF000000u;

	/** An ANSI slot rather than a fixed colour: the theme resolves it at paint
	    time, so changing themes recolours text already on screen. */
	static const std::uint32_t kPaletteColor = 0xFE000000u;

	enum CellAttr : std::uint16_t {
		kAttrNone      = 0,
		kAttrBold      = 1 << 0,
		kAttrDim       = 1 << 1,
		kAttrItalic    = 1 << 2,
		kAttrUnderline = 1 << 3,
		kAttrReverse   = 1 << 4,
		kAttrInvisible = 1 << 5,
		kAttrWide      = 1 << 6,
		kAttrWideTail  = 1 << 7,

		/** Underline shapes beyond the plain line; kAttrUnderline is set too. */
		kAttrUnderlineDouble = 1 << 8,
		kAttrUnderlineCurly  = 1 << 9,
		kAttrUnderlineDotted = 1 << 10,
		kAttrUnderlineDashed = 1 << 11,
	};

	static const std::uint16_t kAttrUnderlineShapes = kAttrUnderlineDouble | kAttrUnderlineCurly
		| kAttrUnderlineDotted | kAttrUnderlineDashed;

	/**
	 * The underline colour is kDefaultColor when it follows the text's.
	 * A link is an OSC 8 hyperlink's number in its Screen, 0 for none.
	 */
	struct Cell {
		char32_t      code       = U' ';
		std::uint32_t foreground = kDefaultColor;
		std::uint32_t background = kDefaultColor;
		std::uint32_t underline  = kDefaultColor;
		std::uint16_t attributes = kAttrNone;
		std::uint16_t link       = 0;
	};

	inline bool sameStyle(const Cell& left, const Cell& right) {
		return left.foreground == right.foreground && left.background == right.background
			&& left.underline == right.underline && left.attributes == right.attributes
			&& left.link == right.link;
	}

} /* namespace wbshterm */

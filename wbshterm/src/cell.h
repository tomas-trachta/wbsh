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
	};

	struct Cell {
		char32_t      code       = U' ';
		std::uint32_t foreground = kDefaultColor;
		std::uint32_t background = kDefaultColor;
		std::uint16_t attributes = kAttrNone;
	};

} /* namespace wbshterm */

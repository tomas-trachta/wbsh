#pragma once

/**
 * @file theme.h
 * @brief Named colour schemes and the palette a terminal paints with.
 */

#include <cstdint>
#include <string>
#include <vector>

namespace wbshterm {

	/**
	 * @brief Everything the renderer needs to colour a grid.
	 *
	 * The sixteen ANSI entries are what SGR 30-37 / 90-97 resolve to, so
	 * changing a theme recolours text that is already on screen.
	 */
	struct Palette {
		std::uint32_t background = 0x1E1E2E;
		std::uint32_t foreground = 0xCDD6F4;
		std::uint32_t cursor     = 0xF5E0DC;
		std::uint32_t selection  = 0x45475A;

		std::uint32_t ansi[16] = {
			0x45475A, 0xF38BA8, 0xA6E3A1, 0xF9E2AF, 0x89B4FA, 0xF5C2E7, 0x94E2D5, 0xBAC2DE,
			0x585B70, 0xF38BA8, 0xA6E3A1, 0xF9E2AF, 0x89B4FA, 0xF5C2E7, 0x94E2D5, 0xA6ADC8,
		};
	};

	/** Looks up a theme by name, case-insensitively. */
	bool findBuiltInTheme(const std::string& name, Palette& out_palette);

	std::vector<std::string> builtInThemeNames();

} /* namespace wbshterm */

#pragma once

#include <cstddef>

/**
 * @file charwidth.h
 * @brief How many cells a character occupies.
 */

namespace wbshterm {

	/**
	 * @brief 0 for marks that hang off the previous cell, 2 for the wide
	 *        ranges (CJK, emoji, fullwidth forms), 1 for everything else.
	 *
	 * This is the grid's own idea of width, and it has to agree with what
	 * the shell assumes or the two draw different pictures. Full grapheme
	 * clustering lives in the renderer, not here.
	 */
	int characterWidth(char32_t code);

} /* namespace wbshterm */

#pragma once

/**
 * @file charwidth_ucd.h
 * @brief The raw trie behind charwidth.h; see tools/gen_charwidth.py.
 */

#include <cstdint>

namespace wbshterm {

	namespace ucd {

		/**
		 * A code point's entry is
		 * stage3[stage2[stage1[stage0[cp >> 11] + ((cp >> 6) & 31)] + ((cp >> 3) & 7)] + (cp & 7)]:
		 * the low four bits its grapheme-break class, the top two its width
		 * (3 meaning East Asian ambiguous).
		 */
		extern const std::uint16_t stage0[];
		extern const std::uint16_t stage1[];
		extern const std::uint16_t stage2[];
		extern const std::uint8_t  stage3[];

		/** Two bits per (lead, trail) class pair: the next join state, 3 to break. */
		extern const std::uint32_t join_rules[2][16];

	} /* namespace ucd */

} /* namespace wbshterm */

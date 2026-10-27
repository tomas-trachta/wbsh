/**
 * @file charwidth.cpp
 * @brief Lookups into OpenConsole's width and grapheme-break trie.
 */

#include "charwidth.h"

#include "charwidth_ucd.h"

namespace wbshterm {

	static const char32_t kLastCodePoint = 0x10FFFF;
	static const char32_t kVariationSelector16 = 0xFE0F;
	static const int kAmbiguousWidth = 3;

	int codepointProperties(char32_t code) {
		if (code > kLastCodePoint) code = 0xFFFD;

		const int first  = ucd::stage0[code >> 11];
		const int second = ucd::stage1[first + static_cast<int>((code >> 6) & 31)];
		const int third  = ucd::stage2[second + static_cast<int>((code >> 3) & 7)];
		return ucd::stage3[third + static_cast<int>(code & 7)];
	}

	int propertiesWidth(int properties) {
		const int width = properties >> 6;
		return width == kAmbiguousWidth ? 1 : width;
	}

	// The rule table has a row per state OpenConsole keeps; a join state
	// past its last row cannot arise from well-formed input, and is read
	// as the plain one.
	int graphemeJoin(int state, int lead, int trail) {
		const int row = state == 1 ? 1 : 0;
		return static_cast<int>((ucd::join_rules[row][lead & 15] >> ((trail & 15) * 2)) & 3);
	}

	int clusterWidthOf(char32_t code, int properties) {
		if (code == kVariationSelector16) return 2;
		return propertiesWidth(properties);
	}

	int characterWidth(char32_t code) {
		return propertiesWidth(codepointProperties(code));
	}

} /* namespace wbshterm */

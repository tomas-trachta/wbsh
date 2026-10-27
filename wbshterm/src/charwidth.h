#pragma once

/**
 * @file charwidth.h
 * @brief How many cells a character occupies, and which characters cluster.
 */

namespace wbshterm {

	/** What the next code point does to a cluster: join it, or start anew. */
	static const int kGraphemeBreak = 3;

	/**
	 * @brief A code point's grapheme-break class and width, packed.
	 *
	 * The grid has to agree with the pseudoconsole on every width or the
	 * cursor drifts, so these are OpenConsole's own tables, measured the
	 * way it measures in its default grapheme-cluster mode.
	 */
	int codepointProperties(char32_t code);

	/** 0, 1 or 2 cells; East Asian ambiguous characters are narrow. */
	int propertiesWidth(int properties);

	/**
	 * @brief Whether @p trail joins the cluster that ends in @p lead.
	 *
	 * @p state starts at 0 for a new cluster and carries what the rules
	 * need to remember (a ZWJ inside an emoji sequence, a pair of regional
	 * indicators). Returns the next state, or kGraphemeBreak.
	 */
	int graphemeJoin(int state, int lead, int trail);

	/** The width a code point adds to its cluster: VS16 asks for emoji width. */
	int clusterWidthOf(char32_t code, int properties);

	/** One code point on its own: 0 for marks that hang off the previous cell. */
	int characterWidth(char32_t code);

} /* namespace wbshterm */

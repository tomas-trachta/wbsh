#pragma once

/**
 * @file textcells.h
 * @brief UTF-8 text as the console shows it: grapheme clusters and their cells.
 *
 * The line editor moves and erases whole clusters (a letter with its
 * accents, an emoji with its modifiers), and places the cursor by the
 * cells those clusters take, measured with the tables wbshterm and the
 * pseudoconsole use. A malformed byte is a cluster of its own.
 */

#include <cstddef>
#include <string>

namespace wbsh {

	/** Bytes in the UTF-8 sequence at @p pos; 1 for a malformed byte. */
	std::size_t codePointLength(const std::string& text, std::size_t pos);

	/** Byte offset of the cluster boundary before @p pos; 0 at the start. */
	std::size_t previousClusterStart(const std::string& text, std::size_t pos);

	/** Byte offset of the cluster boundary after @p pos; text.size() at the end. */
	std::size_t nextClusterEnd(const std::string& text, std::size_t pos);

	/** Console cells taken by the clusters that start in text[0, end). */
	std::size_t displayWidth(const std::string& text, std::size_t end);

} /* namespace wbsh */

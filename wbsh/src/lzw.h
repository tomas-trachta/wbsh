#pragma once

/**
 * @file lzw.h
 * @brief Decoder for Unix compress (.Z, LZW with 9-16 bit codes).
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "bytestream.h"

namespace wbsh {

	/// Decodes a .Z stream (magic 1F 9D included) from @p in into @p out;
	/// @p error stays empty when @p out stopped taking output.
	bool uncompressStream(InputBuffer& in, ByteSink& out, std::string& error);

	/// Decodes a .Z file (magic 1F 9D included), appending to @p out.
	bool uncompressData(const std::uint8_t* in, std::size_t size, std::vector<std::uint8_t>& out,
	                    std::string& error);

}  // namespace wbsh

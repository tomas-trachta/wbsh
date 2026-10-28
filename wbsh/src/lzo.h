#pragma once

/**
 * @file lzo.h
 * @brief LZO1X block codec: the LZO1X-1 compressor and a decompressor that
 *        checks every read and write against both buffers, as liblzo's
 *        lzo1x_decompress_safe does.
 */

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wbsh {

	/// The dictionary sizes liblzo offers for LZO1X-1: 2^14 entries is
	/// lzo1x_1, 2^15 is lzo1x_1_15. Both write the same LZO1X format.
	enum class Lzo1xDictionary {
		Bits14 = 14,
		Bits15 = 15,
	};

	/// Replaces @p out with the LZO1X-1 compression of @p data. Incompressible
	/// input grows by at most size / 16 + 67 bytes.
	void lzo1xCompress(const std::uint8_t* data, std::size_t size, Lzo1xDictionary dictionary,
	                   std::vector<std::uint8_t>& out);

	/// Decodes one LZO1X block (any LZO1X compressor's output, LZO1X-999
	/// included) into exactly @p out_size bytes at @p out. False when the
	/// data is malformed, does not end exactly at its end-of-stream marker,
	/// or decodes to any other length.
	bool lzo1xDecompress(const std::uint8_t* data, std::size_t size, std::uint8_t* out,
	                     std::size_t out_size);

}  // namespace wbsh

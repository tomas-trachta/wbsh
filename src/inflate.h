#pragma once

/**
 * @file inflate.h
 * @brief Minimal RFC 1951 (DEFLATE) decoder, puff-style: correctness over throughput.
 */

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wbsh {

	/// Decodes a raw DEFLATE bitstream (RFC 1951, not the gzip / zlib
	/// wrapper) and appends to `out`. False on malformed input.
	bool inflateRaw(const std::uint8_t* in, std::size_t in_len, std::vector<std::uint8_t>& out);

}  // namespace wbsh

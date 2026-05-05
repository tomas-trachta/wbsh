#pragma once

/**
 * @file inflate.h
 * @brief Minimal RFC 1951 (DEFLATE) decoder.
 *
 * Puff-style decoder that works on a bit-level stream and writes
 * decompressed bytes into an output buffer. Used by `builtin_gunzip` /
 * `zcat` to decompress gzip-wrapped DEFLATE streams. Optimised for
 * correctness over throughput.
 */

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wbsh {

/**
 * @brief Inflate a raw DEFLATE bitstream.
 *
 * @param in     Pointer to the compressed bytes (RFC 1951 raw stream,
 *               not the gzip / zlib wrapper).
 * @param in_len Number of input bytes available at @p in.
 * @param out    Destination buffer; appended to (existing contents
 *               are kept).
 *
 * @return true on success, false on malformed input.
 */
bool inflateRaw(const std::uint8_t* in, std::size_t in_len,
                std::vector<std::uint8_t>& out);

}  // namespace wbsh

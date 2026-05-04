#pragma once

// Minimal RFC 1951 (DEFLATE) decoder — puff-style: works on a bit-level
// stream and produces decompressed bytes into an output buffer.
// Returns true on success, false on malformed input.
//
// Used by builtin_gunzip / zcat to decompress gzip-wrapped DEFLATE
// streams. Not optimised for speed; correctness over throughput.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wbsh {

bool inflateRaw(const std::uint8_t* in, std::size_t in_len,
                std::vector<std::uint8_t>& out);

}  // namespace wbsh

#pragma once

/**
 * @file inflate.h
 * @brief Minimal RFC 1951 (DEFLATE) decoder, puff-style: correctness over throughput.
 */

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wbsh {

	class InputBuffer;
	class LzWindow;

	inline constexpr std::size_t kDeflateWindow = 32768;

	/// Decodes one raw DEFLATE stream (RFC 1951, not the gzip / zlib wrapper)
	/// from @p in into @p out, reading nothing past its final block's last
	/// byte. False on malformed input or when the sink stops taking output.
	bool inflateStream(InputBuffer& in, LzWindow& out);

	/// Decodes a raw DEFLATE bitstream held in memory and appends to `out`.
	bool inflateRaw(const std::uint8_t* in, std::size_t in_len, std::vector<std::uint8_t>& out);

	/// inflateRaw that also adds the number of input bytes the stream took,
	/// through its final block's last partial byte, to @p consumed.
	bool inflateRawConsumed(const std::uint8_t* in, std::size_t in_len,
	                        std::vector<std::uint8_t>& out, std::size_t& consumed);

}  // namespace wbsh

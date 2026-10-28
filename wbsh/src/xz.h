#pragma once

/**
 * @file xz.h
 * @brief Decoders for the .xz container (LZMA2 with optional Delta and x86
 *        BCJ filters, CRC32 / CRC64 checks) and the legacy .lzma format.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "bytestream.h"

namespace wbsh {

	/// Decodes every concatenated .xz stream from @p in into @p out as it
	/// goes; @p error stays empty when @p out stopped taking output.
	bool unxzStream(InputBuffer& in, ByteSink& out, std::string& error);

	/// Decodes one .lzma ("LZMA alone") stream from @p in into @p out.
	bool unlzmaStream(InputBuffer& in, ByteSink& out, std::string& error);

	/// Decodes every concatenated .xz stream in @p in, appending to @p out.
	/// On failure returns false and says why in @p error.
	bool unxzData(const std::uint8_t* in, std::size_t size, std::vector<std::uint8_t>& out,
	              std::string& error);

	/// Decodes one .lzma ("LZMA alone") file, appending to @p out.
	bool unlzmaData(const std::uint8_t* in, std::size_t size, std::vector<std::uint8_t>& out,
	                std::string& error);

}  // namespace wbsh

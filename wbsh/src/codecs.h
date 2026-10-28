#pragma once

/**
 * @file codecs.h
 * @brief One entry point per direction for every compressed format the
 *        bundled tools read: gzip, bzip2, xz, lzma and compress (.Z).
 *        The stream functions keep memory bounded whatever the data size.
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "bytestream.h"

namespace wbsh {

	enum class Codec {
		None,
		Gzip,
		Bzip2,
		Xz,
		Lzma,
		Compress,
	};

	/// The format @p data starts with; Codec::None when nothing matches.
	/// LZMA has no magic, so it is only recognised from a plausible header.
	Codec detectCodec(const std::uint8_t* data, std::size_t size);

	/// detectCodec on the first bytes of @p in, which stay unread.
	Codec detectCodec(InputBuffer& in);

	/// Codec::None for a name the tools do not know.
	Codec codecForSuffix(const std::string& path);

	/// Decodes @p in as @p codec into @p out as the data arrives; Codec::None
	/// copies. On failure @p error says why, or stays empty when @p out
	/// stopped taking output.
	bool decompressStream(Codec codec, InputBuffer& in, ByteSink& out, std::string& error);

	/// A sink that compresses what is written to it; finish() ends the stream.
	class Compressor : public ByteSink {
	public:
		virtual bool finish() = 0;
	};

	/// Only gzip, bzip2 and Codec::None (a pass-through) can be written; the
	/// others give nullptr and an @p error.
	std::unique_ptr<Compressor> makeCompressor(Codec codec, int level, ByteSink& out, std::string& error);

	/// decompressStream over a buffer, appending to @p out.
	bool decompressWith(Codec codec, const std::uint8_t* in, std::size_t size,
	                    std::vector<std::uint8_t>& out, std::string& error);

	/// makeCompressor over a buffer, appending to @p out.
	bool compressWith(Codec codec, const std::uint8_t* in, std::size_t size, int level,
	                  std::vector<std::uint8_t>& out, std::string& error);

}  // namespace wbsh

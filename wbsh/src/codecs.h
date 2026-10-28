#pragma once

/**
 * @file codecs.h
 * @brief One entry point per direction for every compressed format the
 *        bundled tools read: gzip, bzip2, xz, lzma, lzop and compress (.Z),
 *        all but the last of which they also write. The stream functions
 *        keep memory bounded whatever the data size.
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "bytestream.h"
#include "lzop.h"
#include "xz_encoder.h"

namespace wbsh {

	enum class Codec {
		None,
		Gzip,
		Bzip2,
		Xz,
		Lzma,
		Lzop,
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

	struct CompressSettings {
		int level = 6;
		bool extreme = false;
		XzCheck check = XzCheck::Crc64;
	};

	/// False for compress (.Z), which is only read.
	bool canCompress(Codec codec);

	/// Codec::None is a pass-through; a codec canCompress refuses gives
	/// nullptr and an @p error.
	std::unique_ptr<Compressor> makeCompressor(Codec codec, const CompressSettings& settings,
	                                           ByteSink& out, std::string& error);

	/// makeCompressor at @p level with the other settings at their defaults.
	std::unique_ptr<Compressor> makeCompressor(Codec codec, int level, ByteSink& out, std::string& error);

	/// An lzop writer with header fields and checksums chosen by the caller.
	std::unique_ptr<Compressor> makeLzopCompressor(const LzopWriteOptions& options, ByteSink& out);

	/// decompressStream over a buffer, appending to @p out.
	bool decompressWith(Codec codec, const std::uint8_t* in, std::size_t size,
	                    std::vector<std::uint8_t>& out, std::string& error);

	/// makeCompressor over a buffer, appending to @p out.
	bool compressWith(Codec codec, const std::uint8_t* in, std::size_t size, int level,
	                  std::vector<std::uint8_t>& out, std::string& error);

}  // namespace wbsh

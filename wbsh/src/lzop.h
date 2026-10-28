#pragma once

/**
 * @file lzop.h
 * @brief The lzop file format (lzop 1.0x): a checksummed header, then
 *        LZO1X blocks of up to 256 KiB with adler32 / crc32 checks, written
 *        and read one block at a time.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "bytestream.h"
#include "lzo.h"

namespace wbsh {

	struct LzopWriteOptions {
		/// 1 is LZO1X-1(15) as lzop -1 writes it, 2-9 LZO1X-1.
		int level = 3;
		bool checksum_plain = true;
		bool checksum_packed = false;

		/// The original file name; only its first 255 bytes fit the header.
		std::string name;
		std::uint32_t mode = 0100644;
		std::int64_t mtime = 0;
	};

	struct LzopReadOptions {
		bool verify_checksums = true;
	};

	struct LzopSummary {
		int method = 0;
		std::uint64_t packed = 0;
		std::uint64_t unpacked = 0;
	};

	/// Streaming encoder for one lzop file; finish() writes the end marker.
	class LzopEncoder : public ByteSink {
	public:
		LzopEncoder(ByteSink& out, const LzopWriteOptions& options);

		bool write(const std::uint8_t* data, std::size_t size) override;
		bool finish();

	private:
		bool emitBlock();

		ByteSink& out_;
		std::uint32_t flags_ = 0;
		Lzo1xDictionary dictionary_ = Lzo1xDictionary::Bits14;
		std::vector<std::uint8_t> block_;
		std::vector<std::uint8_t> packed_;
		bool ok_ = true;
	};

	/// True when @p in starts with the lzop magic; nothing is consumed.
	bool lzopMagicFollows(InputBuffer& in);

	/// Decodes every concatenated lzop file from @p in, block by block.
	/// Bytes after a file that do not start another one are ignored. On
	/// failure @p error says why, or stays empty when @p out stopped taking
	/// output.
	bool unlzopStream(InputBuffer& in, ByteSink& out, const LzopReadOptions& options,
	                  std::string& error);

	/// Walks the headers and block sizes of @p in without decompressing.
	bool scanLzopStream(InputBuffer& in, LzopSummary& summary, std::string& error);

	/// The name lzop -l prints for a header's method byte.
	const char* lzopMethodName(int method);

}  // namespace wbsh

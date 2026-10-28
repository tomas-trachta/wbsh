#pragma once

/**
 * @file filemagic.h
 * @brief Content-based file type detection for `file`: descriptions and
 *        MIME types worded as file(1) 5.4x words them.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace wbsh {

	/// The first bytes of a file, its size, and (for regular files) the
	/// open file itself, so a parser can look further than the sample.
	struct FileSample {
		std::vector<std::uint8_t> bytes;
		std::uint64_t size = 0;
		std::FILE* file = nullptr;
	};

	struct MagicResult {
		std::string description;
		std::string mime_type;
		std::string encoding = "binary";
	};

	/// Bytes file(1) looks at; the rest of a file only matters to parsers
	/// that seek (ELF section headers, gzip's trailing size).
	inline constexpr std::size_t kMagicSampleBytes = 1 << 20;

	/// "empty", "very short file (no magic)", a recognised format, text, or "data".
	MagicResult identifyContent(const FileSample& sample);

	bool identifyExecutable(const FileSample& sample, MagicResult& result);
	bool identifyMedia(const FileSample& sample, MagicResult& result);
	bool identifyText(const FileSample& sample, MagicResult& result);

	/// @p size bytes at @p offset, from the sample when it holds them and
	/// from the file otherwise; false when the file is shorter.
	bool readSampleAt(const FileSample& sample, std::uint64_t offset, std::size_t size,
	                  std::vector<std::uint8_t>& out);

	/// The way file(1) prints a time stamp: asctime() in UTC, without the newline.
	std::string magicTimeText(long long epoch_seconds);

	namespace magic_bytes {

		inline bool has(const FileSample& sample, std::size_t offset, std::size_t count) {
			return offset <= sample.bytes.size() && count <= sample.bytes.size() - offset;
		}

		inline bool matches(const FileSample& sample, std::size_t offset, const char* text,
		                    std::size_t count) {
			if (!has(sample, offset, count)) return false;
			for (std::size_t k = 0; k < count; ++k) {
				if (sample.bytes[offset + k] != static_cast<std::uint8_t>(text[k])) return false;
			}

			return true;
		}

		inline std::uint32_t le16(const std::uint8_t* p) {
			return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8);
		}

		inline std::uint32_t le32(const std::uint8_t* p) {
			return le16(p) | (le16(p + 2) << 16);
		}

		inline std::uint64_t le64(const std::uint8_t* p) {
			return static_cast<std::uint64_t>(le32(p))
				| (static_cast<std::uint64_t>(le32(p + 4)) << 32);
		}

		inline std::uint32_t be16(const std::uint8_t* p) {
			return (static_cast<std::uint32_t>(p[0]) << 8) | static_cast<std::uint32_t>(p[1]);
		}

		inline std::uint32_t be32(const std::uint8_t* p) {
			return (be16(p) << 16) | be16(p + 2);
		}

		inline std::uint64_t be64(const std::uint8_t* p) {
			return (static_cast<std::uint64_t>(be32(p)) << 32) | be32(p + 4);
		}

	}  // namespace magic_bytes

}  // namespace wbsh

/**
 * @file codecs.cpp
 * @brief Format detection, the gzip (RFC 1952) member framing and the
 *        streaming entry points; the other formats delegate to their own
 *        decoders and encoders.
 */

#include "codecs.h"

#include <cstring>

#include "bzip2.h"
#include "deflate.h"
#include "inflate.h"
#include "lzw.h"
#include "xz.h"

namespace wbsh {

	static const std::uint8_t kGzipFlagCrc     = 0x02;
	static const std::uint8_t kGzipFlagExtra   = 0x04;
	static const std::uint8_t kGzipFlagName    = 0x08;
	static const std::uint8_t kGzipFlagComment = 0x10;
	static const std::size_t  kGzipHeaderSize  = 10;
	static const std::size_t  kGzipTrailerSize = 8;
	static const std::size_t  kDetectBytes     = 14;
	static const std::size_t  kCopyChunk       = 64 * 1024;

	namespace codecs_detail {

		/// Keeps the CRC-32 and length of a gzip member's output.
		class CrcSink : public ByteSink {
		public:
			explicit CrcSink(ByteSink& next) : next_(next) {}

			bool write(const std::uint8_t* data, std::size_t size) override {
				crc_ = crc32Of(crc_, data, size);
				size_ += size;
				return next_.write(data, size);
			}

			std::uint32_t crc() const { return crc_; }
			std::uint32_t size32() const { return static_cast<std::uint32_t>(size_); }

		private:
			ByteSink& next_;
			std::uint32_t crc_ = 0;
			std::uint64_t size_ = 0;
		};

		class GzipCompressor : public Compressor {
		public:
			GzipCompressor(ByteSink& out, int level) : out_(out), deflate_(out, level) {
				std::uint8_t header[kGzipHeaderSize] = {
					0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
				};
				header[8] = static_cast<std::uint8_t>(level >= 9 ? 2 : (level <= 1 ? 4 : 0));
				ok_ = out_.write(header, sizeof(header));
			}

			bool write(const std::uint8_t* data, std::size_t size) override {
				crc_ = crc32Of(crc_, data, size);
				size_ += size;
				return ok_ && deflate_.write(data, size);
			}

			bool finish() override {
				if (!ok_ || !deflate_.finish()) return false;

				std::uint8_t trailer[kGzipTrailerSize];
				for (int k = 0; k < 4; ++k) trailer[k] = static_cast<std::uint8_t>(crc_ >> (8 * k));
				for (int k = 0; k < 4; ++k) trailer[4 + k] = static_cast<std::uint8_t>(size_ >> (8 * k));
				return out_.write(trailer, sizeof(trailer));
			}

		private:
			ByteSink& out_;
			DeflateEncoder deflate_;
			bool ok_ = true;
			std::uint32_t crc_ = 0;
			std::uint64_t size_ = 0;
		};

		class Bzip2Compressor : public Compressor {
		public:
			Bzip2Compressor(ByteSink& out, int level) : encoder_(out, level) {}
			bool write(const std::uint8_t* data, std::size_t size) override { return encoder_.write(data, size); }
			bool finish() override { return encoder_.finish(); }

		private:
			Bzip2Encoder encoder_;
		};

		class PlainCompressor : public Compressor {
		public:
			explicit PlainCompressor(ByteSink& out) : out_(out) {}
			bool write(const std::uint8_t* data, std::size_t size) override { return out_.write(data, size); }
			bool finish() override { return true; }

		private:
			ByteSink& out_;
		};

	}  // namespace codecs_detail

	using codecs_detail::CrcSink;

	static std::uint32_t readLe32(const std::uint8_t* bytes) {
		return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8)
			| (static_cast<std::uint32_t>(bytes[2]) << 16)
			| (static_cast<std::uint32_t>(bytes[3]) << 24);
	}

	static bool skipZeroTerminated(InputBuffer& in) {
		for (;;) {
			const int byte = in.get();
			if (byte < 0) return false;
			if (byte == 0) return true;
		}
	}

	static bool skipCount(InputBuffer& in, std::size_t count) {
		for (; count > 0; --count) {
			if (in.get() < 0) return false;
		}

		return true;
	}

	static bool skipGzipExtras(InputBuffer& in, std::uint8_t flags) {
		if ((flags & kGzipFlagExtra) != 0) {
			std::uint8_t length[2];
			if (!in.readExact(length, 2)) return false;
			if (!skipCount(in, static_cast<std::size_t>(length[0]) | (static_cast<std::size_t>(length[1]) << 8))) {
				return false;
			}
		}

		if ((flags & kGzipFlagName) != 0 && !skipZeroTerminated(in)) return false;
		if ((flags & kGzipFlagComment) != 0 && !skipZeroTerminated(in)) return false;
		return (flags & kGzipFlagCrc) == 0 || skipCount(in, 2);
	}

	static bool gzipHeaderFollows(InputBuffer& in) {
		const std::uint8_t* next = nullptr;
		return in.peek(next, 3) == 3 && next[0] == 0x1F && next[1] == 0x8B && next[2] == 8;
	}

	static bool readGzipHeader(InputBuffer& in) {
		std::uint8_t header[kGzipHeaderSize];
		if (!gzipHeaderFollows(in) || !in.readExact(header, sizeof(header))) return false;
		return skipGzipExtras(in, header[3]);
	}

	static bool inflateMember(InputBuffer& in, ByteSink& out, std::string& error) {
		CrcSink checked(out);
		LzWindow window(checked, kDeflateWindow);
		if (!inflateStream(in, window)) {
			if (window.ok()) error = in.atEnd() ? "unexpected end of file" : "invalid compressed data";
			return false;
		}

		std::uint8_t trailer[kGzipTrailerSize];
		if (!in.readExact(trailer, sizeof(trailer))) {
			error = "unexpected end of file";
			return false;
		}

		if (readLe32(trailer) == checked.crc() && readLe32(trailer + 4) == checked.size32()) return true;
		error = "invalid compressed data--crc error";
		return false;
	}

	static void skipZeroPadding(InputBuffer& in) {
		const std::uint8_t* next = nullptr;
		while (in.peek(next, 1) == 1 && next[0] == 0) in.get();
	}

	// Members after the first that are not gzip are trailing garbage, which
	// gzip ignores.
	static bool gunzipStream(InputBuffer& in, ByteSink& out, std::string& error) {
		if (!readGzipHeader(in)) {
			error = "not in gzip format";
			return false;
		}

		for (;;) {
			if (!inflateMember(in, out, error)) return false;
			skipZeroPadding(in);
			if (in.atEnd() || !readGzipHeader(in)) return true;
		}
	}

	static bool copyStream(InputBuffer& in, ByteSink& out, std::string& error) {
		std::vector<std::uint8_t> chunk(kCopyChunk);
		for (;;) {
			const std::size_t got = in.readSome(chunk.data(), chunk.size());
			if (got == 0) break;
			if (!out.write(chunk.data(), got)) return false;
		}

		if (!in.sourceFailed()) return true;
		error = "read error";
		return false;
	}

	// .lzma has no magic number: accept the header xz and lzma-utils write,
	// a properties byte, a dictionary of 2^n or 2^n + 2^(n-1) bytes (4 KiB
	// or more), a size that is unknown or below 2^48, and the range coder's
	// mandatory zero first byte.
	static bool plausibleLzmaHeader(const std::uint8_t* data, std::size_t size) {
		if (size < 14 || data[0] >= 9 * 5 * 5) return false;

		const std::uint32_t dictionary = readLe32(data + 1);
		const std::uint32_t trimmed = dictionary & (dictionary - 1);
		const bool usual_dictionary = dictionary >= 4096
			&& (trimmed == 0 || (trimmed & (trimmed - 1)) == 0);

		bool unknown_size = true;
		for (int k = 5; k < 13; ++k) unknown_size = unknown_size && data[k] == 0xFF;
		const bool sane_size = unknown_size || (data[11] == 0 && data[12] == 0);
		return usual_dictionary && sane_size && data[13] == 0;
	}

	Codec detectCodec(const std::uint8_t* data, std::size_t size) {
		if (size >= 2 && data[0] == 0x1F && data[1] == 0x8B) return Codec::Gzip;
		if (size >= 2 && data[0] == 0x1F && data[1] == 0x9D) return Codec::Compress;
		if (size >= 3 && std::memcmp(data, "BZh", 3) == 0) return Codec::Bzip2;
		if (size >= 6 && std::memcmp(data, "\xFD" "7zXZ", 6) == 0) return Codec::Xz;
		if (plausibleLzmaHeader(data, size)) return Codec::Lzma;
		return Codec::None;
	}

	Codec detectCodec(InputBuffer& in) {
		const std::uint8_t* data = nullptr;
		const std::size_t size = in.peek(data, kDetectBytes);
		return detectCodec(data, size);
	}

	static bool endsWith(const std::string& text, const char* suffix) {
		const std::size_t length = std::strlen(suffix);
		return text.size() >= length && text.compare(text.size() - length, length, suffix) == 0;
	}

	Codec codecForSuffix(const std::string& path) {
		if (endsWith(path, ".gz") || endsWith(path, ".tgz") || endsWith(path, ".taz")) {
			return Codec::Gzip;
		}

		if (endsWith(path, ".bz2") || endsWith(path, ".tbz") || endsWith(path, ".tbz2")) {
			return Codec::Bzip2;
		}

		if (endsWith(path, ".xz") || endsWith(path, ".txz")) return Codec::Xz;
		if (endsWith(path, ".lzma") || endsWith(path, ".tlz")) return Codec::Lzma;
		if (endsWith(path, ".Z") || endsWith(path, ".taZ")) return Codec::Compress;
		return Codec::None;
	}

	bool decompressStream(Codec codec, InputBuffer& in, ByteSink& out, std::string& error) {
		switch (codec) {
			case Codec::Gzip:
				return gunzipStream(in, out, error);
			case Codec::Bzip2:
				return bunzip2Stream(in, out, error);
			case Codec::Xz:
				return unxzStream(in, out, error);
			case Codec::Lzma:
				return unlzmaStream(in, out, error);
			case Codec::Compress:
				return uncompressStream(in, out, error);
			default:
				return copyStream(in, out, error);
		}
	}

	std::unique_ptr<Compressor> makeCompressor(Codec codec, int level, ByteSink& out, std::string& error) {
		switch (codec) {
			case Codec::Gzip:
				return std::make_unique<codecs_detail::GzipCompressor>(out, level);
			case Codec::Bzip2:
				return std::make_unique<codecs_detail::Bzip2Compressor>(out, level);
			case Codec::None:
				return std::make_unique<codecs_detail::PlainCompressor>(out);
			default:
				error = "compression to this format is not supported";
				return nullptr;
		}
	}

	bool decompressWith(Codec codec, const std::uint8_t* in, std::size_t size,
	                    std::vector<std::uint8_t>& out, std::string& error) {
		MemorySource source(in, size);
		InputBuffer input(source);
		VectorSink sink(out);
		return decompressStream(codec, input, sink, error);
	}

	bool compressWith(Codec codec, const std::uint8_t* in, std::size_t size, int level,
	                  std::vector<std::uint8_t>& out, std::string& error) {
		VectorSink sink(out);
		const std::unique_ptr<Compressor> compressor = makeCompressor(codec, level, sink, error);
		return compressor && compressor->write(in, size) && compressor->finish();
	}

}  // namespace wbsh

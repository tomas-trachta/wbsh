/**
 * @file lzop.cpp
 * @brief lzop framing, laid out as lzop 1.04 writes it; every number is
 *        big-endian. A block whose LZO1X form would not be smaller is
 *        stored as is, which readers tell from its packed size equalling
 *        its plain size, and then carries no packed-data checksums.
 */

#include "lzop.h"

#include <algorithm>
#include <cstring>

namespace wbsh {

	namespace lzop_detail {

		struct Checks {
			std::uint32_t adler = 1;
			std::uint32_t crc = 0;
		};

		/// The flag bits that ask for one kind of data's adler32 and crc32.
		struct CheckFlags {
			std::uint32_t adler;
			std::uint32_t crc;
		};

		/// Reads header fields while keeping both checksums, since which one
		/// the header is checked with is only known from the flags.
		struct HeaderReader {
			InputBuffer* in = nullptr;
			Checks running;
			bool ok = true;
		};

		struct LzopHeader {
			std::uint32_t version = 0;
			std::uint32_t version_needed = 0;
			std::uint32_t method = 0;
			std::uint32_t flags = 0;
		};

		struct BlockHeader {
			std::uint32_t plain_size = 0;
			std::uint32_t packed_size = 0;
			Checks plain_checks;
			Checks packed_checks;
		};

		struct Member {
			std::uint32_t flags = 0;
			bool verify = true;
			std::vector<std::uint8_t> packed;
			std::vector<std::uint8_t> plain;
		};

	}  // namespace lzop_detail

	using lzop_detail::BlockHeader;
	using lzop_detail::CheckFlags;
	using lzop_detail::Checks;
	using lzop_detail::HeaderReader;
	using lzop_detail::LzopHeader;
	using lzop_detail::Member;

	static const std::uint8_t kMagic[9] = { 0x89, 0x4C, 0x5A, 0x4F, 0x00, 0x0D, 0x0A, 0x1A, 0x0A };

	static const std::uint32_t kWriterVersion     = 0x1040;
	static const std::uint32_t kLibraryVersion    = 0x20A0;
	static const std::uint32_t kVersionNeeded     = 0x0940;
	static const std::uint32_t kOldestVersion     = 0x0900;
	static const std::uint32_t kModernLayout      = 0x0940;
	static const std::uint32_t kMethodLzo1x1      = 1;
	static const std::uint32_t kMethodLzo1x1_15   = 2;
	static const std::uint32_t kMethodLzo1x999    = 3;
	static const std::uint32_t kFastLevel         = 1;
	static const std::uint32_t kLzo1x1Level       = 5;
	static const std::uint32_t kFlagAdler32Plain  = 0x00000001;
	static const std::uint32_t kFlagAdler32Packed = 0x00000002;
	static const std::uint32_t kFlagExtraField    = 0x00000040;
	static const std::uint32_t kFlagCrc32Plain    = 0x00000100;
	static const std::uint32_t kFlagCrc32Packed   = 0x00000200;
	static const std::uint32_t kFlagFilter        = 0x00000800;
	static const std::uint32_t kFlagHeaderCrc32   = 0x00001000;
	static const std::uint32_t kFlagOsUnix        = 0x03000000;
	static const std::uint32_t kFlagsDefined      = 0xFFF03FFF;
	static const std::size_t   kBlockSize         = 256 * 1024;
	static const std::uint32_t kMaxBlockSize      = 64 * 1024 * 1024;
	static const std::uint32_t kSplitMarker       = 0xFFFFFFFF;
	static const std::uint32_t kAdlerModulus      = 65521;
	static const std::size_t   kAdlerRun          = 5552;
	static const std::size_t   kMaxNameLength     = 255;

	static const CheckFlags kPlainChecks  = { kFlagAdler32Plain, kFlagCrc32Plain };
	static const CheckFlags kPackedChecks = { kFlagAdler32Packed, kFlagCrc32Packed };

	static const char* const kTruncated  = "unexpected end of file";
	static const char* const kBadHeader  = "header corrupted";
	static const char* const kViolation  = "compressed data violation";
	static const char* const kBadSum     = "checksum error";
	static const char* const kNotLzop    = "not a lzop file";
	static const char* const kSplitFile  = "split lzop files are not supported";


	static bool hasFlag(std::uint32_t flags, std::uint32_t flag) {
		return (flags & flag) != 0;
	}

	static std::uint32_t adler32Of(std::uint32_t adler, const std::uint8_t* data,
	                               std::size_t size) {
		std::uint32_t low = adler & 0xFFFF;
		std::uint32_t high = adler >> 16;
		while (size > 0) {
			const std::size_t run = (std::min)(size, kAdlerRun);
			for (std::size_t k = 0; k < run; ++k) {
				low += data[k];
				high += low;
			}

			low %= kAdlerModulus;
			high %= kAdlerModulus;
			data += run;
			size -= run;
		}

		return (high << 16) | low;
	}

	static std::uint32_t adlerOf(const std::vector<std::uint8_t>& data) {
		return adler32Of(1, data.data(), data.size());
	}

	static std::uint32_t crcOf(const std::vector<std::uint8_t>& data) {
		return crc32Of(0, data.data(), data.size());
	}

	static bool checksMatch(const std::vector<std::uint8_t>& data, std::uint32_t flags,
	                        const CheckFlags& wanted, const Checks& expected) {
		if (hasFlag(flags, wanted.adler) && adlerOf(data) != expected.adler) return false;
		return !hasFlag(flags, wanted.crc) || crcOf(data) == expected.crc;
	}

	static void putBe(std::vector<std::uint8_t>& bytes, std::uint64_t value, std::size_t width) {
		for (std::size_t k = width; k > 0; --k) {
			bytes.push_back(static_cast<std::uint8_t>(value >> (8 * (k - 1))));
		}
	}

	static void putChecks(std::vector<std::uint8_t>& bytes, const std::vector<std::uint8_t>& data,
	                      std::uint32_t flags, const CheckFlags& wanted) {
		if (hasFlag(flags, wanted.adler)) putBe(bytes, adlerOf(data), 4);
		if (hasFlag(flags, wanted.crc)) putBe(bytes, crcOf(data), 4);
	}

	static std::uint32_t flagsFor(const LzopWriteOptions& options) {
		std::uint32_t flags = kFlagOsUnix;
		if (options.checksum_plain) flags |= kFlagAdler32Plain;
		if (options.checksum_packed) flags |= kFlagAdler32Packed;
		return flags;
	}

	static bool fastLevel(const LzopWriteOptions& options) {
		return options.level <= static_cast<int>(kFastLevel);
	}

	static std::vector<std::uint8_t> headerFields(const LzopWriteOptions& options,
	                                              std::uint32_t flags) {
		const std::uint64_t mtime = static_cast<std::uint64_t>(options.mtime);
		const std::size_t name_length = (std::min)(options.name.size(), kMaxNameLength);

		std::vector<std::uint8_t> fields;
		putBe(fields, kWriterVersion, 2);
		putBe(fields, kLibraryVersion, 2);
		putBe(fields, kVersionNeeded, 2);
		putBe(fields, fastLevel(options) ? kMethodLzo1x1_15 : kMethodLzo1x1, 1);
		putBe(fields, fastLevel(options) ? kFastLevel : kLzo1x1Level, 1);
		putBe(fields, flags, 4);
		putBe(fields, options.mode, 4);
		putBe(fields, mtime & 0xFFFFFFFF, 4);
		putBe(fields, mtime >> 32, 4);
		putBe(fields, name_length, 1);
		fields.insert(fields.end(), options.name.begin(), options.name.begin() + name_length);
		return fields;
	}

	LzopEncoder::LzopEncoder(ByteSink& out, const LzopWriteOptions& options)
		: out_(out), flags_(flagsFor(options)),
		  dictionary_(fastLevel(options) ? Lzo1xDictionary::Bits15 : Lzo1xDictionary::Bits14) {
		const std::vector<std::uint8_t> fields = headerFields(options, flags_);

		std::vector<std::uint8_t> header(kMagic, kMagic + sizeof(kMagic));
		header.insert(header.end(), fields.begin(), fields.end());
		putBe(header, adlerOf(fields), 4);
		ok_ = out_.write(header.data(), header.size());
		block_.reserve(kBlockSize);
	}

	bool LzopEncoder::write(const std::uint8_t* data, std::size_t size) {
		while (size > 0 && ok_) {
			const std::size_t take = (std::min)(size, kBlockSize - block_.size());
			block_.insert(block_.end(), data, data + take);
			data += take;
			size -= take;
			if (block_.size() == kBlockSize) ok_ = emitBlock();
		}

		return ok_;
	}

	bool LzopEncoder::finish() {
		if (ok_ && !block_.empty()) ok_ = emitBlock();

		const std::uint8_t end_marker[4] = { 0, 0, 0, 0 };
		return ok_ && out_.write(end_marker, sizeof(end_marker));
	}

	bool LzopEncoder::emitBlock() {
		lzo1xCompress(block_.data(), block_.size(), dictionary_, packed_);
		const bool stored = packed_.size() >= block_.size();
		const std::vector<std::uint8_t>& payload = stored ? block_ : packed_;

		std::vector<std::uint8_t> head;
		putBe(head, block_.size(), 4);
		putBe(head, payload.size(), 4);
		putChecks(head, block_, flags_, kPlainChecks);
		if (!stored) putChecks(head, packed_, flags_, kPackedChecks);

		const bool written = out_.write(head.data(), head.size())
			&& out_.write(payload.data(), payload.size());
		block_.clear();
		return written;
	}

	static bool fail(std::string& error, const char* message) {
		error = message;
		return false;
	}

	static bool readBe32(InputBuffer& in, std::uint32_t& value) {
		std::uint8_t bytes[4];
		if (!in.readExact(bytes, sizeof(bytes))) return false;

		value = 0;
		for (const std::uint8_t byte : bytes) value = (value << 8) | byte;
		return true;
	}

	static bool isMagic(const std::uint8_t* bytes) {
		return std::memcmp(bytes, kMagic, sizeof(kMagic)) == 0;
	}

	bool lzopMagicFollows(InputBuffer& in) {
		const std::uint8_t* next = nullptr;
		return in.peek(next, sizeof(kMagic)) == sizeof(kMagic) && isMagic(next);
	}

	static std::uint32_t takeField(HeaderReader& reader, std::size_t width) {
		std::uint32_t value = 0;
		for (std::size_t k = 0; k < width; ++k) {
			const int byte = reader.in->get();
			if (byte < 0) {
				reader.ok = false;
				return 0;
			}

			const std::uint8_t octet = static_cast<std::uint8_t>(byte);
			reader.running.adler = adler32Of(reader.running.adler, &octet, 1);
			reader.running.crc = crc32Of(reader.running.crc, &octet, 1);
			value = (value << 8) | octet;
		}

		return value;
	}

	static void skipFields(HeaderReader& reader, std::uint32_t count) {
		for (std::uint32_t k = 0; k < count && reader.ok; ++k) takeField(reader, 1);
	}

	// Files older than lzop 0.94 lack the version-needed, level and mtime-high fields.
	static void readHeaderFields(HeaderReader& reader, LzopHeader& header) {
		header.version = takeField(reader, 2);
		const bool modern = header.version >= kModernLayout;
		takeField(reader, 2);                                         // library version
		header.version_needed = modern ? takeField(reader, 2) : kOldestVersion;
		header.method = takeField(reader, 1);
		if (modern) takeField(reader, 1);                             // level
		header.flags = takeField(reader, 4);
		if (hasFlag(header.flags, kFlagFilter)) takeField(reader, 4); // filter
		takeField(reader, 4);                                         // mode
		takeField(reader, 4);                                         // mtime, low 32 bits
		if (modern) takeField(reader, 4);                             // mtime, high 32 bits
		skipFields(reader, takeField(reader, 1));                     // name length, name
	}

	static const char* headerProblem(const LzopHeader& header) {
		if (header.version < kOldestVersion) return kBadHeader;
		if (header.version_needed < kOldestVersion) return kBadHeader;
		if (header.version_needed > kWriterVersion) return "needs a newer version of lzop";
		if (header.method < kMethodLzo1x1 || header.method > kMethodLzo1x999) {
			return "unsupported compression method";
		}

		if (hasFlag(header.flags, kFlagFilter)) return "filtered lzop files are not supported";
		if (hasFlag(header.flags, ~kFlagsDefined)) return kBadHeader;
		return nullptr;
	}

	static bool checksumFollows(InputBuffer& in, const HeaderReader& reader, std::uint32_t flags) {
		const bool crc = hasFlag(flags, kFlagHeaderCrc32);
		const std::uint32_t expected = crc ? reader.running.crc : reader.running.adler;
		std::uint32_t stored = 0;
		return readBe32(in, stored) && stored == expected;
	}

	// The extra field's checksum covers its length word as well.
	static bool skipExtraField(InputBuffer& in, std::uint32_t flags) {
		if (!hasFlag(flags, kFlagExtraField)) return true;

		HeaderReader reader;
		reader.in = &in;
		skipFields(reader, takeField(reader, 4));
		return reader.ok && checksumFollows(in, reader, flags);
	}

	static bool readHeader(InputBuffer& in, LzopHeader& header, std::string& error) {
		std::uint8_t magic[sizeof(kMagic)];
		if (!in.readExact(magic, sizeof(magic)) || !isMagic(magic)) return fail(error, kNotLzop);

		HeaderReader reader;
		reader.in = &in;
		readHeaderFields(reader, header);
		if (!reader.ok) return fail(error, kTruncated);

		const char* problem = headerProblem(header);
		if (problem != nullptr) return fail(error, problem);

		const bool checked = checksumFollows(in, reader, header.flags)
			&& skipExtraField(in, header.flags);
		return checked || fail(error, in.atEnd() ? kTruncated : kBadHeader);
	}

	static bool readOptionalBe32(InputBuffer& in, bool present, std::uint32_t& value) {
		return !present || readBe32(in, value);
	}

	static bool readChecks(InputBuffer& in, std::uint32_t flags, const CheckFlags& wanted,
	                       Checks& checks) {
		return readOptionalBe32(in, hasFlag(flags, wanted.adler), checks.adler)
			&& readOptionalBe32(in, hasFlag(flags, wanted.crc), checks.crc);
	}

	// Packed-data checksums are only there when the block was compressed.
	static bool readBlockChecks(InputBuffer& in, std::uint32_t flags, BlockHeader& block,
	                            std::string& error) {
		const std::uint32_t packed_flags = block.packed_size < block.plain_size ? flags : 0;
		const bool read = readChecks(in, flags, kPlainChecks, block.plain_checks)
			&& readChecks(in, packed_flags, kPackedChecks, block.packed_checks);
		return read || fail(error, kTruncated);
	}

	// A zero plain size is the end of the file.
	static bool readBlockHeader(InputBuffer& in, std::uint32_t flags, BlockHeader& block,
	                            std::string& error) {
		if (!readBe32(in, block.plain_size)) return fail(error, kTruncated);
		if (block.plain_size == 0) return true;
		if (block.plain_size == kSplitMarker) return fail(error, kSplitFile);
		if (block.plain_size > kMaxBlockSize) return fail(error, kViolation);

		if (!readBe32(in, block.packed_size)) return fail(error, kTruncated);
		const bool sane = block.packed_size != 0 && block.packed_size <= block.plain_size;
		return sane ? readBlockChecks(in, flags, block, error) : fail(error, kViolation);
	}

	static bool readPacked(InputBuffer& in, Member& member, const BlockHeader& block,
	                       std::string& error) {
		member.packed.resize(block.packed_size);
		return in.readExact(member.packed.data(), member.packed.size()) || fail(error, kTruncated);
	}

	static bool unpackBlock(Member& member, const BlockHeader& block, std::string& error) {
		if (block.packed_size == block.plain_size) {
			member.plain.swap(member.packed);
			return true;
		}

		const bool intact = !member.verify
			|| checksMatch(member.packed, member.flags, kPackedChecks, block.packed_checks);
		if (!intact) return fail(error, kBadSum);

		member.plain.resize(block.plain_size);
		const bool decoded = lzo1xDecompress(member.packed.data(), member.packed.size(),
			member.plain.data(), member.plain.size());
		return decoded || fail(error, kViolation);
	}

	static bool verifyPlain(const Member& member, const BlockHeader& block, std::string& error) {
		const bool intact = !member.verify
			|| checksMatch(member.plain, member.flags, kPlainChecks, block.plain_checks);
		return intact || fail(error, kBadSum);
	}

	static bool decodeBlocks(InputBuffer& in, ByteSink& out, Member& member, std::string& error) {
		for (;;) {
			BlockHeader block;
			if (!readBlockHeader(in, member.flags, block, error)) return false;
			if (block.plain_size == 0) return true;

			if (!readPacked(in, member, block, error)) return false;
			if (!unpackBlock(member, block, error)) return false;
			if (!verifyPlain(member, block, error)) return false;
			if (!out.write(member.plain.data(), member.plain.size())) return false;
		}
	}

	bool unlzopStream(InputBuffer& in, ByteSink& out, const LzopReadOptions& options,
	                  std::string& error) {
		Member member;
		member.verify = options.verify_checksums;
		do {
			LzopHeader header;
			if (!readHeader(in, header, error)) return false;

			member.flags = header.flags;
			if (!decodeBlocks(in, out, member, error)) return false;
		} while (lzopMagicFollows(in));

		return true;
	}

	static bool scanBlocks(InputBuffer& in, Member& member, LzopSummary& summary,
	                       std::string& error) {
		for (;;) {
			BlockHeader block;
			if (!readBlockHeader(in, member.flags, block, error)) return false;
			if (block.plain_size == 0) return true;

			if (!readPacked(in, member, block, error)) return false;
			summary.unpacked += block.plain_size;
		}
	}

	bool scanLzopStream(InputBuffer& in, LzopSummary& summary, std::string& error) {
		Member member;
		do {
			LzopHeader header;
			if (!readHeader(in, header, error)) return false;

			if (summary.method == 0) summary.method = static_cast<int>(header.method);
			member.flags = header.flags;
			if (!scanBlocks(in, member, summary, error)) return false;
		} while (lzopMagicFollows(in));

		summary.packed = in.consumed();
		return true;
	}

	const char* lzopMethodName(int method) {
		switch (method) {
			case kMethodLzo1x1:
				return "LZO1X-1";
			case kMethodLzo1x1_15:
				return "LZO1X-1(15)";
			case kMethodLzo1x999:
				return "LZO1X-999";
			default:
				return "unknown";
		}
	}

}  // namespace wbsh

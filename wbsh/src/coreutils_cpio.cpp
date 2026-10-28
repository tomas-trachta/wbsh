/**
 * @file coreutils_cpio.cpp
 * @brief cpio: copy-out and copy-in of newc, crc, odc, bin, ustar and old
 *        tar archives. Archives stream one member at a time in both
 *        directions.
 *
 * `-H` picks the format written; reading recognizes every one of them
 * from the first header. crc members are verified when extracted.
 */

#include "coreutils_internal.h"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "bytestream.h"
#include "executor.h"
#include "fnmatch.h"
#include "numparse.h"
#include "optscan.h"
#include "pathconv.h"

namespace wbsh {

	namespace fs = std::filesystem;

	namespace cpio_detail {

		enum class CpioMode {
			None,
			Extract,
			Create,
			List,
		};

		enum class CpioFormat {
			Newc,
			Crc,
			Odc,
			Binary,
			Ustar,
			Tar,
		};

		struct CpioOptions {
			CpioMode mode = CpioMode::None;
			CpioFormat format = CpioFormat::Newc;
			bool make_directories = false;
			bool verbose = false;
			bool unconditional = false;
			bool keep_mtime = false;
			std::string archive;
			std::vector<std::string> patterns;
		};

		struct CpioEntry {
			std::string name;
			std::string link;
			std::uint32_t mode = 0;
			std::uint64_t size = 0;
			std::int64_t mtime = 0;
			std::uint32_t checksum = 0;
			bool has_checksum = false;
			std::uint64_t alignment = 4;
		};

		struct CpioWriter {
			ByteSink* sink = nullptr;
			CpioFormat format = CpioFormat::Newc;
			std::uint64_t written = 0;
			std::uint64_t next_inode = 1;
			bool failed = false;
		};

	}  // namespace cpio_detail

	using cpio_detail::CpioEntry;
	using cpio_detail::CpioFormat;
	using cpio_detail::CpioMode;
	using cpio_detail::CpioOptions;
	using cpio_detail::CpioWriter;

	static const std::uint32_t kTypeMask      = 0170000;
	static const std::uint32_t kTypeDirectory = 0040000;
	static const std::uint32_t kTypeRegular   = 0100000;
	static const std::uint32_t kTypeSymlink   = 0120000;
	static const std::size_t kNewcHeader   = 110;
	static const std::size_t kOdcHeader    = 76;
	static const std::size_t kBinaryHeader = 26;
	static const std::size_t kTarBlock     = 512;
	static const std::uint32_t kBinaryMagic = 070707;
	static const char kTrailer[] = "TRAILER!!!";

	// tar header fields (POSIX.1-1988); old tar leaves magic, owner names,
	// device numbers and prefix empty.
	static const std::size_t kTarNameWidth    = 100;
	static const std::size_t kTarModeOffset   = 100;
	static const std::size_t kTarUidOffset    = 108;
	static const std::size_t kTarGidOffset    = 116;
	static const std::size_t kTarSizeOffset   = 124;
	static const std::size_t kTarMtimeOffset  = 136;
	static const std::size_t kTarSumOffset    = 148;
	static const std::size_t kTarTypeOffset   = 156;
	static const std::size_t kTarLinkOffset   = 157;
	static const std::size_t kTarMagicOffset  = 257;
	static const std::size_t kTarOwnerOffset  = 265;
	static const std::size_t kTarGroupOffset  = 297;
	static const std::size_t kTarMajorOffset  = 329;
	static const std::size_t kTarMinorOffset  = 337;
	static const std::size_t kTarPrefixOffset = 345;
	static const std::size_t kTarPrefixWidth  = 155;

	static std::uint64_t numberField(const std::uint8_t* field, std::size_t width, int base) {
		unsigned long long value = 0;
		parseULL(std::string(reinterpret_cast<const char*>(field), width), value, nullptr, base);
		return value;
	}

	static std::uint64_t hexField(const std::uint8_t* header, std::size_t index) {
		return numberField(header + 6 + index * 8, 8, 16);
	}

	static std::uint64_t octalField(const std::uint8_t* header, std::size_t offset,
	                                std::size_t width) {
		return numberField(header + offset, width, 8);
	}

	static bool readName(InputBuffer& in, std::size_t name_size, CpioEntry& entry) {
		if (name_size == 0) return false;

		std::string name(name_size, '\0');
		if (!in.readExact(reinterpret_cast<std::uint8_t*>(&name[0]), name_size)) return false;
		name.pop_back();
		entry.name = name;
		return true;
	}

	static bool skipBytes(InputBuffer& in, std::uint64_t count) {
		for (; count > 0; --count) {
			if (in.get() < 0) return false;
		}

		return true;
	}

	static bool skipToMultiple(InputBuffer& in, std::uint64_t multiple) {
		return skipBytes(in, (multiple - in.consumed() % multiple) % multiple);
	}

	// newc / crc: 13 eight-digit hex fields (the last one is crc's checksum);
	// header plus name, and the data, are each padded to a multiple of four.
	static bool parseNewc(InputBuffer& in, CpioEntry& entry) {
		std::uint8_t header[kNewcHeader];
		if (!in.readExact(header, kNewcHeader)) return false;

		entry.has_checksum = header[5] == '2';
		entry.mode = static_cast<std::uint32_t>(hexField(header, 1));
		entry.mtime = static_cast<std::int64_t>(hexField(header, 5));
		entry.size = hexField(header, 6);
		entry.checksum = static_cast<std::uint32_t>(hexField(header, 12));
		entry.alignment = 4;
		const std::size_t name_size = static_cast<std::size_t>(hexField(header, 11));
		return readName(in, name_size, entry) && skipToMultiple(in, 4);
	}

	// odc: fixed-width octal fields with no padding at all.
	static bool parseOdc(InputBuffer& in, CpioEntry& entry) {
		std::uint8_t header[kOdcHeader];
		if (!in.readExact(header, kOdcHeader)) return false;

		entry.mode = static_cast<std::uint32_t>(octalField(header, 18, 6));
		entry.mtime = static_cast<std::int64_t>(octalField(header, 48, 11));
		entry.size = octalField(header, 65, 11);
		entry.alignment = 1;
		const std::size_t name_size = static_cast<std::size_t>(octalField(header, 59, 6));
		return readName(in, name_size, entry);
	}

	static std::uint32_t binaryWord(const std::uint8_t* header, std::size_t index,
	                                bool little_endian) {
		const std::uint32_t first = header[index * 2];
		const std::uint32_t second = header[index * 2 + 1];
		return little_endian ? first | (second << 8) : (first << 8) | second;
	}

	static std::uint32_t binaryLong(const std::uint8_t* header, std::size_t index,
	                                bool little_endian) {
		const std::uint32_t high = binaryWord(header, index, little_endian);
		const std::uint32_t low = binaryWord(header, index + 1, little_endian);
		return (high << 16) | low;
	}

	static bool isBinaryMagic(const std::uint8_t* header) {
		const bool little_endian = binaryWord(header, 0, true) == kBinaryMagic;
		return little_endian || binaryWord(header, 0, false) == kBinaryMagic;
	}

	// bin: thirteen 16-bit words in the writer's byte order (magic dev ino mode
	// uid gid nlink rdev mtime[2] namesize filesize[2], long values high word
	// first); the name and the data are each padded to an even length.
	static bool parseBinary(InputBuffer& in, CpioEntry& entry) {
		std::uint8_t header[kBinaryHeader];
		if (!in.readExact(header, kBinaryHeader)) return false;

		const bool little_endian = binaryWord(header, 0, true) == kBinaryMagic;
		entry.mode = binaryWord(header, 3, little_endian);
		entry.mtime = binaryLong(header, 8, little_endian);
		entry.size = binaryLong(header, 11, little_endian);
		entry.alignment = 2;
		const std::size_t name_size = binaryWord(header, 10, little_endian);
		return readName(in, name_size, entry) && skipToMultiple(in, 2);
	}

	static std::string tarText(const std::uint8_t* header, std::size_t offset, std::size_t width) {
		const char* start = reinterpret_cast<const char*>(header + offset);
		return std::string(start, ::strnlen(start, width));
	}

	static std::uint64_t tarNumber(const std::uint8_t* header, std::size_t offset,
	                               std::size_t width) {
		std::uint64_t value = 0;
		for (std::size_t k = 0; k < width; ++k) {
			const std::uint8_t digit = header[offset + k];
			if (digit == ' ' && value == 0) continue;
			if (digit < '0' || digit > '7') break;
			value = (value << 3) | static_cast<std::uint64_t>(digit - '0');
		}

		return value;
	}

	static bool isZeroBlock(const std::uint8_t* block) {
		for (std::size_t k = 0; k < kTarBlock; ++k) {
			if (block[k] != 0) return false;
		}

		return true;
	}

	static bool tarChecksumMatches(const std::uint8_t* header) {
		std::uint64_t unsigned_sum = 0;
		std::int64_t signed_sum = 0;
		for (std::size_t k = 0; k < kTarBlock; ++k) {
			const bool in_field = k >= kTarSumOffset && k < kTarSumOffset + 8;
			const std::uint8_t byte = in_field ? ' ' : header[k];
			unsigned_sum += byte;
			signed_sum += static_cast<signed char>(byte);
		}

		const std::uint64_t stored = tarNumber(header, kTarSumOffset, 8);
		return stored == unsigned_sum || static_cast<std::int64_t>(stored) == signed_sum;
	}

	static bool isTarHeader(const std::uint8_t* block) {
		return isZeroBlock(block) || tarChecksumMatches(block);
	}

	static std::uint32_t tarTypeBits(std::uint8_t type) {
		switch (type) {
		case '0':
		case '\0':
		case '7': return kTypeRegular;
		case '5': return kTypeDirectory;
		case '2': return kTypeSymlink;
		default:  return 0;
		}
	}

	static std::string tarName(const std::uint8_t* header) {
		std::string name = tarText(header, 0, kTarNameWidth);
		const std::string prefix = tarText(header, kTarPrefixOffset, kTarPrefixWidth);
		const bool ustar = std::memcmp(header + kTarMagicOffset, "ustar", 5) == 0;
		if (ustar && !prefix.empty()) name = prefix + "/" + name;
		while (name.size() > 1 && name.back() == '/') name.pop_back();
		return name;
	}

	static bool parseTar(InputBuffer& in, CpioEntry& entry) {
		std::uint8_t header[kTarBlock];
		if (!in.readExact(header, kTarBlock)) return false;

		if (isZeroBlock(header)) {
			entry.name = kTrailer;
			return true;
		}

		const std::uint64_t mode_field = tarNumber(header, kTarModeOffset, 8);
		const std::uint32_t permissions = static_cast<std::uint32_t>(mode_field & 07777);
		entry.name = tarName(header);
		entry.link = tarText(header, kTarLinkOffset, kTarNameWidth);
		entry.mode = permissions | tarTypeBits(header[kTarTypeOffset]);
		entry.mtime = static_cast<std::int64_t>(tarNumber(header, kTarMtimeOffset, 12));
		entry.size = tarNumber(header, kTarSizeOffset, 12);
		entry.alignment = kTarBlock;
		return true;
	}

	static bool startsWith(const std::uint8_t* data, std::size_t size, const char* magic) {
		const std::size_t length = std::strlen(magic);
		return size >= length && std::memcmp(data, magic, length) == 0;
	}

	static bool nextCpioEntry(InputBuffer& in, CpioEntry& entry) {
		const std::uint8_t* head = nullptr;
		const std::size_t seen = in.peek(head, kTarBlock);
		entry = CpioEntry();
		const bool newc = startsWith(head, seen, "070701") || startsWith(head, seen, "070702");
		if (newc) return parseNewc(in, entry);
		if (startsWith(head, seen, "070707")) return parseOdc(in, entry);
		if (seen >= kBinaryHeader && isBinaryMagic(head)) return parseBinary(in, entry);
		if (seen == kTarBlock && isTarHeader(head)) return parseTar(in, entry);
		return false;
	}

	static bool finishEntryData(InputBuffer& in, const CpioEntry& entry, std::uint64_t left) {
		return skipBytes(in, left) && skipToMultiple(in, entry.alignment);
	}

	static bool cpioSelected(const CpioOptions& options, const std::string& name) {
		if (options.patterns.empty()) return true;
		for (const std::string& pattern : options.patterns) {
			if (fnmatchFull(pattern, name)) return true;
		}

		return false;
	}

	static bool safeCpioName(const std::string& name) {
		if (name.empty() || name[0] == '/' || (name.size() > 1 && name[1] == ':')) return false;
		return name != ".." && name.compare(0, 3, "../") != 0
			&& name.find("/../") == std::string::npos
			&& (name.size() < 3 || name.compare(name.size() - 3, 3, "/..") != 0);
	}

	static const std::size_t kCopyChunk = 64 * 1024;

	static std::size_t chunkLength(std::uint64_t left) {
		return static_cast<std::size_t>((std::min<std::uint64_t>)(left, kCopyChunk));
	}

	static std::uint32_t byteSum(std::uint32_t sum, const std::uint8_t* data, std::size_t size) {
		for (std::size_t k = 0; k < size; ++k) sum += data[k];
		return sum;
	}

	// Copies the entry's data to @p out, returning how much of it is left
	// unread when the archive ends early.
	static std::uint64_t copyEntryData(InputBuffer& in, std::uint64_t size, std::FILE* out,
	                                   std::uint32_t& sum) {
		std::vector<std::uint8_t> chunk(kCopyChunk);
		while (size > 0) {
			const std::size_t want = chunkLength(size);
			if (!in.readExact(chunk.data(), want)) return size;
			std::fwrite(chunk.data(), 1, want, out);
			sum = byteSum(sum, chunk.data(), want);
			size -= want;
		}

		return 0;
	}

	static bool checksumHolds(const CpioEntry& entry, std::uint32_t sum) {
		if (!entry.has_checksum || sum == entry.checksum) return true;

		std::fprintf(stderr, "cpio: %s: checksum error (0x%x, should be 0x%x)\n",
			entry.name.c_str(), sum, entry.checksum);
		return false;
	}

	static bool extractCpioFile(Executor& exec, const CpioEntry& entry,
	                            const CpioOptions& options, InputBuffer& in, std::uint64_t& left) {
		const fs::path path = toNative(exec, entry.name);
		std::error_code ec;
		if (!options.unconditional && fs::exists(path, ec)) {
			std::fprintf(stderr, "cpio: %s not created: newer or same age file exists\n",
				entry.name.c_str());
			return false;
		}

		if (options.make_directories) fs::create_directories(path.parent_path(), ec);
		std::FILE* out = fopenNative(exec, entry.name, "wb");
		if (out == nullptr) {
			std::fprintf(stderr, "cpio: %s: %s\n", entry.name.c_str(), std::strerror(errno));
			return false;
		}

		std::uint32_t sum = 0;
		left = copyEntryData(in, entry.size, out, sum);
		std::fclose(out);
		return left == 0 && checksumHolds(entry, sum);
	}

	static const std::uint64_t kMaxLinkTarget = 64 * 1024;

	static bool readLinkTarget(const CpioEntry& entry, InputBuffer& in, std::string& target) {
		if (!entry.link.empty()) {
			target = entry.link;
			return true;
		}

		if (entry.size > kMaxLinkTarget) return false;

		target.assign(static_cast<std::size_t>(entry.size), '\0');
		if (target.empty()) return true;
		return in.readExact(reinterpret_cast<std::uint8_t*>(&target[0]), target.size());
	}

	static bool extractCpioLink(Executor& exec, const CpioEntry& entry, InputBuffer& in,
	                            std::uint64_t& left) {
		std::string target;
		if (!readLinkTarget(entry, in, target)) return false;
		left = entry.link.empty() ? 0 : entry.size;

		std::error_code ec;
		fs::create_symlink(utf8ToPath(target), toNative(exec, entry.name), ec);
		return !ec;
	}

	static bool extractCpioEntry(Executor& exec, const CpioEntry& entry,
	                             const CpioOptions& options, InputBuffer& in, std::uint64_t& left) {
		if (!safeCpioName(entry.name)) {
			std::fprintf(stderr, "cpio: %s: refusing to write outside the current directory\n",
				entry.name.c_str());
			return false;
		}

		std::error_code ec;
		const std::uint32_t type = entry.mode & kTypeMask;
		if (type == kTypeDirectory) {
			fs::create_directories(toNative(exec, entry.name), ec);
			return true;
		}

		if (type == kTypeSymlink) return extractCpioLink(exec, entry, in, left);
		if (type != kTypeRegular) return true;
		return extractCpioFile(exec, entry, options, in, left);
	}

	static bool handleCpioEntry(Executor& exec, const CpioOptions& options, InputBuffer& in,
	                            const CpioEntry& entry, bool& ok) {
		std::uint64_t left = entry.size;
		if (cpioSelected(options, entry.name)) {
			const bool list = options.mode == CpioMode::List;
			std::FILE* report = list ? stdout : stderr;
			if (list || options.verbose) std::fprintf(report, "%s\n", entry.name.c_str());
			if (!list && !extractCpioEntry(exec, entry, options, in, left)) ok = false;
		}

		return finishEntryData(in, entry, left);
	}

	static int walkCpio(Executor& exec, const CpioOptions& options, InputBuffer& in) {
		int status = 0;
		bool any = false;
		CpioEntry entry;
		while (nextCpioEntry(in, entry)) {
			any = true;
			if (entry.name == kTrailer) break;

			bool ok = true;
			const bool complete = handleCpioEntry(exec, options, in, entry, ok);
			if (!ok) status = 1;
			if (!complete) {
				perr("cpio", "premature end of archive");
				return 2;
			}
		}

		if (!any) {
			perr("cpio", "unrecognized archive format");
			return 2;
		}

		std::fprintf(stderr, "%llu blocks\n", static_cast<unsigned long long>((in.consumed() + 511) / 512));
		return status;
	}

	static int readCpio(Executor& exec, const CpioOptions& options) {
		const bool from_stdin = options.archive.empty() || options.archive == "-";
		std::FILE* file = from_stdin ? stdin : fopenNative(exec, options.archive, "rb");
		if (file == nullptr) {
			perr("cpio", options.archive + ": " + std::strerror(errno));
			return 1;
		}

		FileSource source(file);
		InputBuffer in(source);
		const int status = walkCpio(exec, options, in);
		if (file != stdin) std::fclose(file);
		std::fflush(stdout);
		return status;
	}

	static void emit(CpioWriter& writer, const void* data, std::size_t size) {
		if (writer.failed || size == 0) return;
		writer.failed = !writer.sink->write(static_cast<const std::uint8_t*>(data), size);
		writer.written += size;
	}

	static void padTo(CpioWriter& writer, std::uint64_t multiple) {
		static const std::uint8_t kZeros[kTarBlock] = {};
		emit(writer, kZeros, static_cast<std::size_t>((multiple - writer.written % multiple) % multiple));
	}

	static void emitName(CpioWriter& writer, const std::string& name) {
		emit(writer, name.c_str(), name.size() + 1);
	}

	static void appendHexField(std::string& header, std::uint64_t value) {
		char text[16];
		const unsigned long long low = value & 0xFFFFFFFFULL;
		std::snprintf(text, sizeof(text), "%08llX", low);
		header += text;
	}

	static void writeNewcHeader(CpioWriter& writer, const CpioEntry& entry) {
		const bool crc = writer.format == CpioFormat::Crc;
		std::string header = crc ? "070702" : "070701";
		const std::uint64_t fields[13] = {
			writer.next_inode++, entry.mode, 0, 0, 1, static_cast<std::uint64_t>(entry.mtime),
			entry.size, 0, 0, 0, 0, entry.name.size() + 1, crc ? entry.checksum : 0,
		};
		for (const std::uint64_t field : fields) appendHexField(header, field);

		emit(writer, header.data(), header.size());
		emitName(writer, entry.name);
		padTo(writer, 4);
	}

	static void appendOctalField(std::string& header, std::uint64_t value, int digits) {
		char text[32];
		const unsigned long long limited = value & ((1ULL << (3 * digits)) - 1);
		std::snprintf(text, sizeof(text), "%0*llo", digits, limited);
		header += text;
	}

	// odc: magic, then dev ino mode uid gid nlink rdev in six octal digits,
	// mtime in eleven, namesize in six and filesize in eleven.
	static void writeOdcHeader(CpioWriter& writer, const CpioEntry& entry) {
		std::string header = "070707";
		const std::uint64_t fields[7] = { 0, writer.next_inode++, entry.mode, 0, 0, 1, 0 };
		for (const std::uint64_t field : fields) appendOctalField(header, field, 6);
		appendOctalField(header, static_cast<std::uint64_t>(entry.mtime), 11);
		appendOctalField(header, entry.name.size() + 1, 6);
		appendOctalField(header, entry.size, 11);

		emit(writer, header.data(), header.size());
		emitName(writer, entry.name);
	}

	static void appendWord(std::vector<std::uint8_t>& header, std::uint64_t value) {
		header.push_back(static_cast<std::uint8_t>(value & 0xFF));
		header.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
	}

	static void appendLong(std::vector<std::uint8_t>& header, std::uint64_t value) {
		appendWord(header, (value >> 16) & 0xFFFF);
		appendWord(header, value & 0xFFFF);
	}

	static void writeBinaryHeader(CpioWriter& writer, const CpioEntry& entry) {
		std::vector<std::uint8_t> header;
		const std::uint64_t words[8] = {
			kBinaryMagic, 0, writer.next_inode++, entry.mode, 0, 0, 1, 0,
		};
		for (const std::uint64_t word : words) appendWord(header, word);
		appendLong(header, static_cast<std::uint64_t>(entry.mtime));
		appendWord(header, entry.name.size() + 1);
		appendLong(header, entry.size);

		emit(writer, header.data(), header.size());
		emitName(writer, entry.name);
		padTo(writer, 2);
	}

	static bool splitTarName(const std::string& name, bool ustar, std::string& prefix,
	                         std::string& rest) {
		if (name.size() <= kTarNameWidth) {
			rest = name;
			return true;
		}

		for (std::size_t slash = name.find('/'); ustar && slash != std::string::npos;
		     slash = name.find('/', slash + 1)) {
			if (slash > kTarPrefixWidth) return false;
			if (name.size() - slash - 1 > kTarNameWidth || slash + 1 == name.size()) continue;
			prefix = name.substr(0, slash);
			rest = name.substr(slash + 1);
			return true;
		}

		return false;
	}

	static void putTarText(std::uint8_t* header, std::size_t offset, const std::string& text) {
		std::memcpy(header + offset, text.data(), text.size());
	}

	static void putTarNumber(std::uint8_t* header, std::size_t offset, std::size_t width,
	                         std::uint64_t value) {
		char text[32];
		std::snprintf(text, sizeof(text), "%0*llo", static_cast<int>(width - 1),
			static_cast<unsigned long long>(value));
		std::memcpy(header + offset, text, width - 1);
	}

	static void putTarChecksum(std::uint8_t* header) {
		std::memset(header + kTarSumOffset, ' ', 8);
		unsigned sum = 0;
		for (std::size_t k = 0; k < kTarBlock; ++k) sum += header[k];

		char text[8];
		std::snprintf(text, sizeof(text), "%06o", sum);
		std::memcpy(header + kTarSumOffset, text, 7);
	}

	static char tarTypeFlag(std::uint32_t mode) {
		switch (mode & kTypeMask) {
		case kTypeDirectory: return '5';
		case kTypeSymlink:   return '2';
		default:             return '0';
		}
	}

	static void putUstarFields(std::uint8_t* header, const std::string& prefix) {
		putTarText(header, kTarMagicOffset, std::string("ustar\0" "00", 8));
		putTarText(header, kTarOwnerOffset, "root");
		putTarText(header, kTarGroupOffset, "root");
		putTarNumber(header, kTarMajorOffset, 8, 0);
		putTarNumber(header, kTarMinorOffset, 8, 0);
		putTarText(header, kTarPrefixOffset, prefix);
	}

	static bool tarFieldsFit(const CpioEntry& entry, bool ustar, std::string& prefix,
	                         std::string& rest) {
		if (!splitTarName(entry.name, ustar, prefix, rest)) {
			std::fprintf(stderr, "cpio: %s: file name too long\n", entry.name.c_str());
			return false;
		}

		if (entry.link.size() <= kTarNameWidth) return true;

		std::fprintf(stderr, "cpio: %s: symbolic link too long\n", entry.name.c_str());
		return false;
	}

	static bool writeTarHeader(CpioWriter& writer, const CpioEntry& entry) {
		const bool ustar = writer.format == CpioFormat::Ustar;
		std::string prefix;
		std::string rest;
		if (!tarFieldsFit(entry, ustar, prefix, rest)) return false;

		std::uint8_t header[kTarBlock] = {};
		putTarText(header, 0, rest);
		putTarNumber(header, kTarModeOffset, 8, ustar ? entry.mode & 07777 : entry.mode);
		putTarNumber(header, kTarUidOffset, 8, 0);
		putTarNumber(header, kTarGidOffset, 8, 0);
		putTarNumber(header, kTarSizeOffset, 12, entry.size);
		putTarNumber(header, kTarMtimeOffset, 12, static_cast<std::uint64_t>(entry.mtime));
		header[kTarTypeOffset] = static_cast<std::uint8_t>(tarTypeFlag(entry.mode));
		putTarText(header, kTarLinkOffset, entry.link);
		if (ustar) putUstarFields(header, prefix);
		putTarChecksum(header);
		emit(writer, header, kTarBlock);
		return true;
	}

	static bool isTarFormat(CpioFormat format) {
		return format == CpioFormat::Ustar || format == CpioFormat::Tar;
	}

	static std::uint64_t dataAlignment(CpioFormat format) {
		switch (format) {
		case CpioFormat::Newc:
		case CpioFormat::Crc:    return 4;
		case CpioFormat::Binary: return 2;
		case CpioFormat::Ustar:
		case CpioFormat::Tar:    return kTarBlock;
		default:                 return 1;
		}
	}

	static bool writeHeader(CpioWriter& writer, const CpioEntry& entry) {
		switch (writer.format) {
		case CpioFormat::Odc:    writeOdcHeader(writer, entry);    return true;
		case CpioFormat::Binary: writeBinaryHeader(writer, entry); return true;
		case CpioFormat::Ustar:
		case CpioFormat::Tar:    return writeTarHeader(writer, entry);
		default:                 writeNewcHeader(writer, entry);   return true;
		}
	}

	static std::uint32_t streamSum(std::FILE* in, std::uint64_t size) {
		std::vector<std::uint8_t> chunk(kCopyChunk);
		std::uint32_t sum = 0;
		while (size > 0) {
			const std::size_t want = chunkLength(size);
			const std::size_t got = std::fread(chunk.data(), 1, want, in);
			if (got == 0) break;
			sum = byteSum(sum, chunk.data(), got);
			size -= got;
		}

		std::rewind(in);
		return sum;
	}

	// Streams exactly @p size bytes: a file that shrank meanwhile is padded
	// with zeros so the header stays true.
	static void writeStreamData(CpioWriter& writer, std::FILE* in, std::uint64_t size) {
		std::vector<std::uint8_t> chunk(kCopyChunk);
		while (size > 0) {
			const std::size_t want = chunkLength(size);
			const std::size_t got = std::fread(chunk.data(), 1, want, in);
			if (got < want) std::memset(chunk.data() + got, 0, want - got);
			emit(writer, chunk.data(), want);
			size -= want;
		}
	}

	static std::int64_t mtimeOf(const fs::path& path) {
		std::error_code ec;
		const auto stamp = fs::last_write_time(path, ec);
		if (ec) return 0;
		return std::chrono::duration_cast<std::chrono::seconds>(stamp.time_since_epoch()).count()
			- 11644473600LL;
	}

	static bool addDirectory(CpioWriter& writer, CpioEntry& entry) {
		entry.mode = kTypeDirectory | 0755;
		return writeHeader(writer, entry);
	}

	static bool addSymlink(CpioWriter& writer, CpioEntry& entry, const fs::path& path) {
		std::error_code ec;
		const std::string target = pathToUtf8(fs::read_symlink(path, ec).generic_wstring());
		entry.mode = kTypeSymlink | 0777;
		if (isTarFormat(writer.format)) entry.link = target;
		else entry.size = target.size();
		if (!writeHeader(writer, entry)) return false;

		emit(writer, target.data(), static_cast<std::size_t>(entry.size));
		padTo(writer, dataAlignment(writer.format));
		return true;
	}

	static bool addRegularFile(Executor& exec, CpioWriter& writer, CpioEntry& entry,
	                           const fs::path& path) {
		std::error_code ec;
		entry.size = static_cast<std::uint64_t>(fs::file_size(path, ec));
		std::FILE* in = ec ? nullptr : fopenNative(exec, entry.name, "rb");
		if (in == nullptr) {
			std::fprintf(stderr, "cpio: %s: %s\n", entry.name.c_str(), std::strerror(errno));
			return false;
		}

		entry.mode = kTypeRegular | 0644;
		if (writer.format == CpioFormat::Crc) entry.checksum = streamSum(in, entry.size);
		const bool written = writeHeader(writer, entry);
		if (written) writeStreamData(writer, in, entry.size);
		if (written) padTo(writer, dataAlignment(writer.format));
		std::fclose(in);
		return written;
	}

	static bool addCpioMember(Executor& exec, const std::string& name, CpioWriter& writer) {
		const fs::path path = toNative(exec, name);
		std::error_code ec;
		const fs::file_status status = fs::symlink_status(path, ec);
		if (ec || !fs::exists(status)) {
			std::fprintf(stderr, "cpio: %s: No such file or directory\n", name.c_str());
			return false;
		}

		CpioEntry entry;
		entry.name = name;
		entry.mtime = mtimeOf(path);
		if (fs::is_directory(status)) return addDirectory(writer, entry);
		if (fs::is_symlink(status)) return addSymlink(writer, entry, path);
		return addRegularFile(exec, writer, entry, path);
	}

	static void writeTrailer(CpioWriter& writer) {
		static const std::uint8_t kEndBlocks[kTarBlock * 2] = {};
		if (isTarFormat(writer.format)) {
			emit(writer, kEndBlocks, sizeof(kEndBlocks));
			return;
		}

		CpioEntry trailer;
		trailer.name = kTrailer;
		writeHeader(writer, trailer);
		padTo(writer, kTarBlock);
	}

	static int createCpio(Executor& exec, const CpioOptions& options) {
		std::vector<std::string> names;
		readAllLines(exec, "-", names);

		std::FILE* file = options.archive.empty() ? stdout : fopenNative(exec, options.archive, "wb");
		if (file == nullptr) {
			perr("cpio", options.archive + ": " + std::strerror(errno));
			return 1;
		}

		FileSink sink(file);
		CpioWriter writer;
		writer.sink = &sink;
		writer.format = options.format;
		int status = 0;
		for (const std::string& name : names) {
			if (name.empty()) continue;
			if (options.verbose) std::fprintf(stderr, "%s\n", name.c_str());
			if (!addCpioMember(exec, name, writer)) status = 1;
		}

		writeTrailer(writer);
		if (file != stdout) std::fclose(file);
		std::fflush(stdout);
		std::fprintf(stderr, "%llu blocks\n", static_cast<unsigned long long>(writer.written / 512));
		return writer.failed ? 1 : status;
	}

	static bool parseCpioFormat(const std::string& name, CpioFormat& format) {
		static const struct { const char* name; CpioFormat format; } kFormats[] = {
			{ "newc",  CpioFormat::Newc },
			{ "crc",   CpioFormat::Crc },
			{ "odc",   CpioFormat::Odc },
			{ "bin",   CpioFormat::Binary },
			{ "ustar", CpioFormat::Ustar },
			{ "tar",   CpioFormat::Tar },
		};
		for (const auto& known : kFormats) {
			if (name != known.name) continue;
			format = known.format;
			return true;
		}

		perr("cpio", "invalid archive format '" + name + "'");
		return false;
	}

	static const std::vector<OptDef> kCpioOptionDefs = {
		{ 'i', 'i', "extract",                    OptArg::None },
		{ 'o', 'o', "create",                     OptArg::None },
		{ 't', 't', "list",                       OptArg::None },
		{ 'd', 'd', "make-directories",           OptArg::None },
		{ 'v', 'v', "verbose",                    OptArg::None },
		{ 'u', 'u', "unconditional",              OptArg::None },
		{ 'm', 'm', "preserve-modification-time", OptArg::None },
		{ 'c', 'c', nullptr,                      OptArg::None },
		{ 'F', 'F', "file",                       OptArg::Required },
		{ 'H', 'H', "format",                     OptArg::Required },
	};

	static bool applyCpioHit(const OptHit& hit, CpioOptions& options) {
		switch (hit.id) {
		case 'i': options.mode = CpioMode::Extract;    break;
		case 'o': options.mode = CpioMode::Create;     break;
		case 't': options.mode = CpioMode::List;       break;
		case 'd': options.make_directories = true;     break;
		case 'v': options.verbose = true;              break;
		case 'u': options.unconditional = true;        break;
		case 'm': options.keep_mtime = true;           break;
		case 'c': options.format = CpioFormat::Newc;   break;
		case 'F': options.archive = hit.value;         break;
		case 'H': return parseCpioFormat(hit.value, options.format);
		default:  return false;
		}

		return true;
	}

	static bool parseCpioArgs(const std::vector<std::string>& args, CpioOptions& options) {
		OptScanResult scan;
		if (!scanOptions("cpio", args, kCpioOptionDefs, scan)) return false;

		for (const OptHit& hit : scan.hits) {
			if (!applyCpioHit(hit, options)) return false;
		}

		options.patterns = scan.operands;
		return options.mode != CpioMode::None;
	}

	static int builtin_cpio(Executor& exec, const std::vector<std::string>& args) {
		CpioOptions options;
		if (!parseCpioArgs(args, options)) {
			perr("cpio", "usage: cpio -i|-o|-t [-dmuv] [-H FORMAT] [-F ARCHIVE] [PATTERN]...");
			return 2;
		}

		if (options.mode == CpioMode::Create) return createCpio(exec, options);
		return readCpio(exec, options);
	}

	void registerCpioBuiltin(Executor& exec) {
		exec.registerBuiltin("cpio", builtin_cpio);
	}

}  // namespace wbsh

/**
 * @file filemagic.cpp
 * @brief The `file` detection order, and archives, compressed data and
 *        documents: zip (with JAR, OOXML, OpenDocument, EPUB), gzip,
 *        bzip2, xz, lzma, lzop, zstd, 7-zip, RAR, tar, cpio, ar, compress,
 *        cabinet, ISO 9660, fonts, PDF, SQLite, Windows shortcuts and
 *        OLE / MSI.
 */

#include "filemagic.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "codecs.h"

namespace wbsh {

	using magic_bytes::be16;
	using magic_bytes::be32;
	using magic_bytes::has;
	using magic_bytes::le16;
	using magic_bytes::le32;
	using magic_bytes::le64;
	using magic_bytes::matches;

	namespace magic_detail {

		struct CodeName {
			std::uint32_t code;
			const char* name;
		};

		struct ZipEntry {
			std::size_t offset = 0;
			std::uint32_t version = 0;
			std::uint32_t method = 0;
			std::uint32_t compressed = 0;
			std::string name;
			std::size_t data = 0;
		};

		struct SignatureName {
			const char* signature;
			const char* description;
			const char* mime;
		};

		struct OpenDocumentName {
			const char* mime;
			const char* description;
		};

		typedef bool (*Identifier)(const FileSample& sample, MagicResult& result);

	}  // namespace magic_detail

	using magic_detail::CodeName;
	using magic_detail::Identifier;
	using magic_detail::OpenDocumentName;
	using magic_detail::SignatureName;
	using magic_detail::ZipEntry;

	static const CodeName kGzipSystems[] = {
		{ 0, "FAT filesystem (MS-DOS, OS/2, NT)" }, { 1, "Amiga" }, { 2, "VMS" }, { 3, "Unix" },
		{ 4, "VM/CMS" }, { 5, "Atari" }, { 6, "HPFS filesystem (OS/2, NT)" }, { 7, "MacOS" },
		{ 8, "Z-System" }, { 9, "CP/M" }, { 10, "TOPS/20" }, { 11, "NTFS filesystem (NT)" },
		{ 12, "QDOS" }, { 13, "Acorn RISCOS" },
	};

	static const CodeName kZipMethods[] = {
		{ 0, "store" }, { 8, "deflate" }, { 9, "deflate64" }, { 12, "bzip2" }, { 14, "lzma" },
		{ 93, "zstd" }, { 95, "xz" }, { 98, "ppmd" },
	};

	static const CodeName kXzChecks[] = {
		{ 0, "NONE" }, { 1, "CRC32" }, { 4, "CRC64" }, { 10, "SHA-256" },
	};

	static const CodeName kLzopMethods[] = {
		{ 1, "LZO1X-1" }, { 2, "LZO1X-1(15)" }, { 3, "LZO1X-999" },
	};

	static const CodeName kLzopSystems[] = { { 0, "MS-DOS" }, { 3, "Unix" }, { 11, "Windows" } };

	static const CodeName kShortcutFlags[] = {
		{ 0x1, "Item id list present" }, { 0x2, "Points to a file or directory" },
		{ 0x4, "Has Description string" }, { 0x8, "Has Relative path" },
		{ 0x10, "Has Working directory" }, { 0x20, "Has command line arguments" },
		{ 0x40, "Icon" },
	};

	static const CodeName kShortcutAttributes[] = {
		{ 0x1, "Read-Only" }, { 0x2, "Hidden" }, { 0x4, "System" }, { 0x10, "Directory" },
		{ 0x20, "Archive" }, { 0x80, "Normal" }, { 0x100, "Temporary" }, { 0x800, "Compressed" },
		{ 0x4000, "Encrypted" },
	};

	static const CodeName kShortcutWindows[] = {
		{ 0, "hide" }, { 3, "showmaximized" }, { 7, "showminnoactive" },
	};

	static const OpenDocumentName kOpenDocuments[] = {
		{ "application/vnd.oasis.opendocument.text", "OpenDocument Text" },
		{ "application/vnd.oasis.opendocument.text-template", "OpenDocument Text Template" },
		{ "application/vnd.oasis.opendocument.spreadsheet", "OpenDocument Spreadsheet" },
		{ "application/vnd.oasis.opendocument.presentation", "OpenDocument Presentation" },
		{ "application/vnd.oasis.opendocument.graphics", "OpenDocument Drawing" },
		{ "application/epub+zip", "EPUB document" },
	};

	static const SignatureName kFontSignatures[] = {
		{ "OTTO", "OpenType font data", "font/otf" },
		{ "wOFF", "Web Open Font Format", "font/woff" },
		{ "wOF2", "Web Open Font Format (Version 2)", "font/woff2" },
	};

	static const std::uint8_t kMsiClsid[16] = {
		0x84, 0x10, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00,
		0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46,
	};

	static const std::uint64_t kFiletimeEpochOffset = 11644473600ULL;
	static const std::uint64_t kFiletimeTicks = 10000000ULL;
	static const std::size_t kTarMagicOffset = 257;
	static const std::size_t kIsoDescriptor = 32769;
	static const std::size_t kZipScanEntries = 64;

	template <std::size_t N>
	static const char* nameFor(const CodeName (&table)[N], std::uint32_t code) {
		for (const CodeName& entry : table) {
			if (entry.code == code) return entry.name;
		}

		return nullptr;
	}

	template <std::size_t N>
	static std::string flagNames(const CodeName (&table)[N], std::uint32_t flags) {
		std::string text;
		for (const CodeName& entry : table) {
			if ((flags & entry.code) != 0) text += std::string(", ") + entry.name;
		}

		return text;
	}

	static void setResult(MagicResult& result, const std::string& description, const char* mime) {
		result.description = description;
		result.mime_type = mime;
	}

	bool readSampleAt(const FileSample& sample, std::uint64_t offset, std::size_t size,
	                  std::vector<std::uint8_t>& out) {
		out.clear();
		if (offset <= sample.bytes.size() && size <= sample.bytes.size() - offset) {
			const auto first = sample.bytes.begin() + static_cast<std::ptrdiff_t>(offset);
			out.assign(first, first + static_cast<std::ptrdiff_t>(size));
			return true;
		}

		if (sample.file == nullptr || offset + size > sample.size) return false;
		if (_fseeki64(sample.file, static_cast<long long>(offset), SEEK_SET) != 0) return false;

		out.resize(size);
		return std::fread(out.data(), 1, size, sample.file) == size;
	}

	std::string magicTimeText(long long epoch_seconds) {
		const std::time_t when = static_cast<std::time_t>(epoch_seconds);
		std::tm parts{};
		if (gmtime_s(&parts, &when) != 0) return "?";

		char text[64];
		std::strftime(text, sizeof(text), "%a %b %e %H:%M:%S %Y", &parts);
		return text;
	}

	static bool readZipEntry(const FileSample& sample, std::size_t offset, ZipEntry& entry) {
		if (!matches(sample, offset, "PK\x03\x04", 4) || !has(sample, offset, 30)) return false;

		const std::uint8_t* header = &sample.bytes[offset];
		const std::size_t name_length = le16(header + 26);
		const std::size_t extra_length = le16(header + 28);
		if (!has(sample, offset + 30, name_length)) return false;

		entry.offset = offset;
		entry.version = le16(header + 4);
		entry.method = le16(header + 8);
		entry.compressed = le32(header + 18);
		entry.name.assign(sample.bytes.begin() + static_cast<std::ptrdiff_t>(offset + 30),
			sample.bytes.begin() + static_cast<std::ptrdiff_t>(offset + 30 + name_length));
		entry.data = offset + 30 + name_length + extra_length;
		return true;
	}

	static std::vector<std::string> zipEntryNames(const FileSample& sample) {
		std::vector<std::string> names;
		ZipEntry entry;
		std::size_t offset = 0;
		while (names.size() < kZipScanEntries && readZipEntry(sample, offset, entry)) {
			names.push_back(entry.name);
			const bool sized = (le16(&sample.bytes[offset + 6]) & 0x8) == 0;
			if (!sized) break;
			offset = entry.data + entry.compressed;
		}

		return names;
	}

	static bool anyNameStarts(const std::vector<std::string>& names, const char* prefix) {
		for (const std::string& name : names) {
			if (name.compare(0, std::strlen(prefix), prefix) == 0) return true;
		}

		return false;
	}

	static bool describeOoxml(const FileSample& sample, MagicResult& result) {
		const std::vector<std::string> names = zipEntryNames(sample);
		if (anyNameStarts(names, "word/")) {
			setResult(result, "Microsoft Word 2007+",
				"application/vnd.openxmlformats-officedocument.wordprocessingml.document");
		} else if (anyNameStarts(names, "xl/")) {
			setResult(result, "Microsoft Excel 2007+",
				"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet");
		} else if (anyNameStarts(names, "ppt/")) {
			setResult(result, "Microsoft PowerPoint 2007+",
				"application/vnd.openxmlformats-officedocument.presentationml.presentation");
		} else {
			setResult(result, "Microsoft OOXML", "application/octet-stream");
		}

		return true;
	}

	static bool describeOpenDocument(const FileSample& sample, const ZipEntry& entry,
	                                 MagicResult& result) {
		if (entry.method != 0 || !has(sample, entry.data, entry.compressed)) return false;

		const std::string mime(sample.bytes.begin() + static_cast<std::ptrdiff_t>(entry.data),
			sample.bytes.begin() + static_cast<std::ptrdiff_t>(entry.data + entry.compressed));
		for (const OpenDocumentName& document : kOpenDocuments) {
			if (mime != document.mime) continue;
			setResult(result, document.description, document.mime);
			return true;
		}

		return false;
	}

	static std::string zipVersionText(std::uint32_t version) {
		return "v" + std::to_string(version / 10) + "." + std::to_string(version % 10);
	}

	static bool identifyZip(const FileSample& sample, MagicResult& result) {
		if (matches(sample, 0, "PK\x05\x06", 4)) {
			setResult(result, "Zip archive data (empty)", "application/zip");
			return true;
		}

		ZipEntry entry;
		if (!readZipEntry(sample, 0, entry)) return false;
		if (entry.name == "[Content_Types].xml") return describeOoxml(sample, result);
		if (entry.name == "mimetype" && describeOpenDocument(sample, entry, result)) return true;
		if (entry.name == "META-INF/MANIFEST.MF" || entry.name == "META-INF/") {
			setResult(result, "Java archive data (JAR)", "application/java-archive");
			return true;
		}

		std::string text = "Zip archive data, at least " + zipVersionText(entry.version)
			+ " to extract";
		const char* method = nameFor(kZipMethods, entry.method);
		if (method != nullptr) text += std::string(", compression method=") + method;
		setResult(result, text, "application/zip");
		return true;
	}

	static std::size_t skipCString(const FileSample& sample, std::size_t at, std::string* out) {
		while (at < sample.bytes.size() && sample.bytes[at] != 0) {
			if (out != nullptr) out->push_back(static_cast<char>(sample.bytes[at]));
			++at;
		}

		return at + 1;
	}

	static std::string gzipNameText(const FileSample& sample) {
		const std::uint32_t flags = sample.bytes[3];
		std::size_t at = 10;
		if ((flags & 0x4) != 0 && has(sample, at, 2)) at += 2 + le16(&sample.bytes[at]);
		if ((flags & 0x8) == 0) return std::string();

		std::string name;
		skipCString(sample, at, &name);
		return ", was \"" + name + "\"";
	}

	static std::string gzipSizeText(const FileSample& sample) {
		std::vector<std::uint8_t> tail;
		if (sample.size < 18) return std::string();
		if (!readSampleAt(sample, sample.size - 4, 4, tail)) return std::string();
		return ", original size modulo 2^32 " + std::to_string(le32(tail.data()));
	}

	static bool identifyGzip(const FileSample& sample, MagicResult& result) {
		if (!matches(sample, 0, "\x1f\x8b", 2) || !has(sample, 0, 10)) return false;

		std::string text = "gzip compressed data";
		if (sample.bytes[2] != 8) text += ", reserved method";
		text += gzipNameText(sample);

		const std::uint32_t mtime = le32(&sample.bytes[4]);
		if (mtime != 0) text += ", last modified: " + magicTimeText(mtime);
		if (sample.bytes[8] == 2) text += ", max compression";
		if (sample.bytes[8] == 4) text += ", max speed";

		const char* system = nameFor(kGzipSystems, sample.bytes[9]);
		if (system != nullptr) text += std::string(", from ") + system;
		setResult(result, text + gzipSizeText(sample), "application/gzip");
		return true;
	}

	static bool identifyBzip2(const FileSample& sample, MagicResult& result) {
		if (!matches(sample, 0, "BZh", 3) || !has(sample, 3, 1)) return false;

		const char level = static_cast<char>(sample.bytes[3]);
		if (level < '1' || level > '9') return false;

		setResult(result, std::string("bzip2 compressed data, block size = ") + level + "00k",
			"application/x-bzip2");
		return true;
	}

	static bool identifyXz(const FileSample& sample, MagicResult& result) {
		if (!matches(sample, 0, "\xfd" "7zXZ\0", 6)) return false;

		std::string text = "XZ compressed data";
		const char* check = has(sample, 7, 1) ? nameFor(kXzChecks, sample.bytes[7] & 0x0f)
			: nullptr;
		if (check != nullptr) text += std::string(", checksum ") + check;
		setResult(result, text, "application/x-xz");
		return true;
	}

	static bool identifyLzma(const FileSample& sample, MagicResult& result) {
		if (!has(sample, 0, 13) || sample.bytes[0] != 0x5d) return false;
		if (detectCodec(sample.bytes.data(), sample.bytes.size()) != Codec::Lzma) return false;

		const std::uint64_t size = le64(&sample.bytes[5]);
		const std::string text = size == ~0ULL ? "LZMA compressed data, streamed"
			: "LZMA compressed data, non-streamed, size " + std::to_string(size);
		setResult(result, text, "application/x-lzma");
		return true;
	}

	static std::string lzopDetails(const FileSample& sample) {
		const std::uint32_t version = be16(&sample.bytes[9]);
		const std::size_t method_at = version >= 0x0940 ? 15 : 13;
		std::string text = " - version " + std::to_string(version >> 12) + ".";
		const std::string minor = std::to_string(((version >> 8) & 0xf) * 100
			+ ((version >> 4) & 0xf) * 10 + (version & 0xf));
		text += std::string(3 - std::min<std::size_t>(3, minor.size()), '0') + minor;
		if (!has(sample, method_at, 1)) return text;

		const char* method = nameFor(kLzopMethods, sample.bytes[method_at]);
		if (method != nullptr) text += std::string(", ") + method;

		const std::size_t flags_at = method_at + (version >= 0x0940 ? 2 : 1);
		const char* system = has(sample, flags_at, 4)
			? nameFor(kLzopSystems, sample.bytes[flags_at]) : nullptr;
		if (system != nullptr) text += std::string(", os: ") + system;
		return text;
	}

	static bool identifyLzop(const FileSample& sample, MagicResult& result) {
		if (!matches(sample, 0, "\x89LZO\0\r\n\x1a\n", 9)) return false;

		const std::string details = has(sample, 9, 2) ? lzopDetails(sample) : std::string();
		setResult(result, "lzop compressed data" + details, "application/x-lzop");
		return true;
	}

	static bool identifyZstd(const FileSample& sample, MagicResult& result) {
		if (!matches(sample, 0, "\x28\xb5\x2f\xfd", 4) || !has(sample, 4, 1)) return false;

		const std::uint32_t dictionary_bytes = sample.bytes[4] & 3;
		std::string text = "Zstandard compressed data (v0.8+), Dictionary ID: ";
		if (dictionary_bytes == 0) text += "None";
		else if (has(sample, 6, 1)) text += std::to_string(le16(&sample.bytes[5]));
		setResult(result, text, "application/zstd");
		return true;
	}

	static bool identifySevenZip(const FileSample& sample, MagicResult& result) {
		if (!matches(sample, 0, "7z\xbc\xaf\x27\x1c", 6) || !has(sample, 6, 2)) return false;

		setResult(result, "7-zip archive data, version " + std::to_string(sample.bytes[6]) + "."
			+ std::to_string(sample.bytes[7]), "application/x-7z-compressed");
		return true;
	}

	static bool identifyRar(const FileSample& sample, MagicResult& result) {
		if (matches(sample, 0, "Rar!\x1a\x07\x01\x00", 8)) {
			setResult(result, "RAR archive data, v5", "application/x-rar");
			return true;
		}

		if (!matches(sample, 0, "Rar!\x1a\x07\x00", 7)) return false;

		setResult(result, "RAR archive data, v4", "application/x-rar");
		return true;
	}

	static bool tarChecksumValid(const FileSample& sample) {
		if (!has(sample, 0, 512)) return false;

		unsigned long stored = 0;
		bool digits = false;
		for (std::size_t k = 148; k < 156; ++k) {
			const std::uint8_t c = sample.bytes[k];
			if (c < '0' || c > '7') continue;
			stored = stored * 8 + (c - '0');
			digits = true;
		}

		unsigned long sum = 0;
		for (std::size_t k = 0; k < 512; ++k) sum += k >= 148 && k < 156 ? ' ' : sample.bytes[k];
		return digits && sum == stored;
	}

	static bool identifyTar(const FileSample& sample, MagicResult& result) {
		if (matches(sample, kTarMagicOffset, "ustar  \0", 8)) {
			setResult(result, "POSIX tar archive (GNU)", "application/x-tar");
			return true;
		}

		if (matches(sample, kTarMagicOffset, "ustar\0", 6)) {
			setResult(result, "POSIX tar archive", "application/x-tar");
			return true;
		}

		if (!tarChecksumValid(sample)) return false;

		setResult(result, "tar archive", "application/x-tar");
		return true;
	}

	static bool identifyCpio(const FileSample& sample, MagicResult& result) {
		const char* text = nullptr;
		if (matches(sample, 0, "070701", 6)) text = "ASCII cpio archive (SVR4 with no CRC)";
		else if (matches(sample, 0, "070702", 6)) text = "ASCII cpio archive (SVR4 with CRC)";
		else if (matches(sample, 0, "070707", 6)) text = "ASCII cpio archive (pre-SVR4 or odc)";
		else if (matches(sample, 0, "\xc7\x71", 2)) text = "cpio archive";
		else if (matches(sample, 0, "\x71\xc7", 2)) text = "byte-swapped cpio archive";
		if (text == nullptr) return false;

		setResult(result, text, "application/x-cpio");
		return true;
	}

	static bool identifyAr(const FileSample& sample, MagicResult& result) {
		if (!matches(sample, 0, "!<arch>\n", 8)) return false;

		if (matches(sample, 8, "debian-binary   ", 16)) {
			setResult(result, "Debian binary package (format 2.0)",
				"application/vnd.debian.binary-package");
			return true;
		}

		setResult(result, "current ar archive", "application/x-archive");
		return true;
	}

	static bool identifyCompress(const FileSample& sample, MagicResult& result) {
		if (!matches(sample, 0, "\x1f\x9d", 2) || !has(sample, 2, 1)) return false;

		setResult(result, "compress'd data " + std::to_string(sample.bytes[2] & 0x1f) + " bits",
			"application/x-compress");
		return true;
	}

	static bool identifyCabinet(const FileSample& sample, MagicResult& result) {
		if (!matches(sample, 0, "MSCF\0\0\0\0", 8) || !has(sample, 0, 30)) return false;

		const std::uint32_t files = le16(&sample.bytes[28]);
		const std::uint32_t bytes = le32(&sample.bytes[8]);
		setResult(result, "Microsoft Cabinet archive data, " + std::to_string(bytes)
			+ " bytes, " + std::to_string(files) + (files == 1 ? " file" : " files"),
			"application/vnd.ms-cab-compressed");
		return true;
	}

	static bool identifyIso9660(const FileSample& sample, MagicResult& result) {
		if (!matches(sample, kIsoDescriptor, "CD001", 5) || !has(sample, kIsoDescriptor + 39, 32)) {
			return false;
		}

		const auto label_start = sample.bytes.begin()
			+ static_cast<std::ptrdiff_t>(kIsoDescriptor + 39);
		std::string label(label_start, label_start + 32);
		while (!label.empty() && (label.back() == ' ' || label.back() == '\0')) label.pop_back();
		setResult(result, "ISO 9660 CD-ROM filesystem data '" + label + "'",
			"application/x-iso9660-image");
		return true;
	}

	static std::string pdfPageCount(const FileSample& sample) {
		const std::string text(sample.bytes.begin(), sample.bytes.end());
		std::size_t at = text.find("/Type/Pages");
		if (at == std::string::npos) at = text.find("/Type /Pages");
		if (at == std::string::npos) return std::string();

		const std::size_t object_start = text.rfind("obj", at);
		const std::size_t from = object_start == std::string::npos ? at : object_start;
		const std::size_t count = text.find("/Count", from);
		const std::size_t object_end = text.find("endobj", at);
		if (count == std::string::npos || (object_end != std::string::npos && count > object_end)) {
			return std::string();
		}

		std::size_t digit = count + 6;
		while (digit < text.size() && text[digit] == ' ') ++digit;
		std::string number;
		while (digit < text.size() && text[digit] >= '0' && text[digit] <= '9') {
			number += text[digit++];
		}

		return number.empty() ? std::string() : ", " + number + " page(s)";
	}

	static bool identifyPdf(const FileSample& sample, MagicResult& result) {
		if (!matches(sample, 0, "%PDF-", 5)) return false;

		std::string version;
		for (std::size_t at = 5; at < sample.bytes.size() && at < 12; ++at) {
			const char c = static_cast<char>(sample.bytes[at]);
			if ((c < '0' || c > '9') && c != '.') break;
			version += c;
		}

		setResult(result, "PDF document, version " + version + pdfPageCount(sample),
			"application/pdf");
		return true;
	}

	static const char* sqliteEncoding(std::uint32_t code) {
		if (code == 2) return "UTF-16 little endian";
		if (code == 3) return "UTF-16 big endian";
		return "UTF-8";
	}

	static bool identifySqlite(const FileSample& sample, MagicResult& result) {
		if (!matches(sample, 0, "SQLite format 3\0", 16) || !has(sample, 0, 100)) return false;

		const std::uint8_t* header = sample.bytes.data();
		char cookie[16];
		std::snprintf(cookie, sizeof(cookie), "0x%x", be32(header + 40));
		const std::string text = "SQLite 3.x database, last written using SQLite version "
			+ std::to_string(be32(header + 96))
			+ ", file counter " + std::to_string(be32(header + 24))
			+ ", database pages " + std::to_string(be32(header + 28)) + ", cookie " + cookie
			+ ", schema " + std::to_string(be32(header + 44))
			+ ", " + sqliteEncoding(be32(header + 56))
			+ ", version-valid-for " + std::to_string(be32(header + 92));
		setResult(result, text, "application/vnd.sqlite3");
		return true;
	}

	static std::string filetimeText(const char* label, std::uint64_t filetime) {
		if (filetime == 0) return std::string();

		const std::uint64_t unix_seconds = filetime / kFiletimeTicks - kFiletimeEpochOffset;
		const long long seconds = static_cast<long long>(unix_seconds);
		return std::string(", ") + label + "=" + magicTimeText(seconds);
	}

	static bool identifyShortcut(const FileSample& sample, MagicResult& result) {
		static const char kHeader[] = "\x4c\0\0\0\x01\x14\x02\0\0\0\0\0\xc0\0\0\0\0\0\0\x46";
		if (!matches(sample, 0, kHeader, 20) || !has(sample, 0, 76)) return false;

		const std::uint8_t* header = sample.bytes.data();
		std::string text = "MS Windows shortcut" + flagNames(kShortcutFlags, le32(header + 20))
			+ flagNames(kShortcutAttributes, le32(header + 24));
		text += filetimeText("ctime", le64(header + 28)) + filetimeText("mtime", le64(header + 44))
			+ filetimeText("atime", le64(header + 36));
		text += ", length=" + std::to_string(le32(header + 52));

		const char* window = nameFor(kShortcutWindows, le32(header + 60));
		if (window != nullptr) text += std::string(", window=") + window;
		setResult(result, text, "application/x-ms-shortcut");
		return true;
	}

	static bool rootClsidIs(const FileSample& sample, const std::uint8_t* clsid) {
		const std::uint32_t shift = le16(&sample.bytes[30]);
		if (shift < 7 || shift > 16) return false;

		const std::uint64_t sector = 1ULL << shift;
		const std::uint64_t first_directory = le32(&sample.bytes[48]);
		const std::uint64_t root = (first_directory + 1) * sector;
		std::vector<std::uint8_t> entry;
		if (!readSampleAt(sample, root + 80, 16, entry)) return false;
		return std::memcmp(entry.data(), clsid, 16) == 0;
	}

	static bool identifyCompoundDocument(const FileSample& sample, MagicResult& result) {
		if (!matches(sample, 0, "\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", 8) || !has(sample, 0, 512)) {
			return false;
		}

		if (rootClsidIs(sample, kMsiClsid)) {
			setResult(result, "Composite Document File V2 Document, MSI Installer",
				"application/x-msi");
			return true;
		}

		setResult(result, "Composite Document File V2 Document", "application/x-ole-storage");
		return true;
	}

	static bool identifyFont(const FileSample& sample, MagicResult& result) {
		if (matches(sample, 0, "\0\x01\0\0", 4) && has(sample, 4, 2)) {
			setResult(result, "TrueType Font data, " + std::to_string(be16(&sample.bytes[4]))
				+ " tables", "font/sfnt");
			return true;
		}

		for (const SignatureName& font : kFontSignatures) {
			if (!matches(sample, 0, font.signature, 4)) continue;
			setResult(result, font.description, font.mime);
			return true;
		}

		return false;
	}

	// A UTF-16 byte-order mark also reads as an MPEG audio sync word, so
	// marked text is settled before the media checks run.
	static bool identifyMarkedText(const FileSample& sample, MagicResult& result) {
		const bool marked = matches(sample, 0, "\xff\xfe", 2) || matches(sample, 0, "\xfe\xff", 2);
		return marked && identifyText(sample, result);
	}

	static const Identifier kIdentifiers[] = {
		identifyMarkedText, identifyExecutable, identifyZip, identifyGzip, identifyBzip2,
		identifyXz, identifyLzop, identifyZstd, identifySevenZip, identifyRar, identifyCompress,
		identifyCabinet, identifyPdf, identifySqlite, identifyShortcut, identifyCompoundDocument,
		identifyAr, identifyCpio, identifyTar, identifyIso9660, identifyFont, identifyMedia,
		identifyLzma, identifyText,
	};

	MagicResult identifyContent(const FileSample& sample) {
		MagicResult result;
		if (sample.bytes.empty()) {
			setResult(result, "empty", "application/x-empty");
			return result;
		}

		if (sample.bytes.size() == 1) {
			setResult(result, "very short file (no magic)", "application/octet-stream");
			return result;
		}

		for (const Identifier identify : kIdentifiers) {
			if (identify(sample, result)) return result;
		}

		setResult(result, "data", "application/octet-stream");
		return result;
	}

}  // namespace wbsh

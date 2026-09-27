/**
 * @file coreutils_archive.cpp
 * @brief Archive / compression coreutils: tar (ustar, uncompressed),
 *        gzip, gunzip, zcat, zip, unzip (zip writes stored entries
 *        only; both directions stay compatible with other tools).
 */

#include "coreutils_internal.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "executor.h"
#include "inflate.h"
#include "numparse.h"

namespace wbsh {

	namespace fs = std::filesystem;

	static const std::size_t kTarBlock = 512;
	static const std::size_t kTarTrailerBlocks = 2;
	static const char kTarTypeFile = '0';
	static const char kTarTypeDirectory = '5';

	static const std::size_t kReadChunk = 4096;

	static const std::uint32_t kCrcPolynomial = 0xEDB88320u;
	static const std::size_t kGzipMinSize = 18;
	static const std::size_t kGzipFooterSize = 8;
	static const std::size_t kStoredBlockMax = 65535;

	static const std::uint32_t kZipLocalHeaderSig = 0x04034B50u;
	static const std::uint32_t kZipCentralDirSig  = 0x02014B50u;
	static const std::uint32_t kZipEocdSig        = 0x06054B50u;
	static const std::size_t kZipEocdSize = 22;
	static const std::size_t kZipEocdSearchLimit = 65557;
	static const std::size_t kZipLocalHeaderSize = 30;
	static const std::size_t kZipCentralEntrySize = 46;
	static const std::uint16_t kZipMethodStored = 0;
	static const std::uint16_t kZipMethodDeflate = 8;
	static const std::uint16_t kZipVersion = 20;

	struct TarHeader {
		char name[100];
		char mode[8];
		char uid[8];
		char gid[8];
		char size[12];
		char mtime[12];
		char chksum[8];
		char typeflag;
		char linkname[100];
		char magic[6];     // "ustar\0"
		char version[2];   // "00"
		char uname[32];
		char gname[32];
		char devmajor[8];
		char devminor[8];
		char prefix[155];
		char pad[12];
	};
	static_assert(sizeof(TarHeader) == kTarBlock, "tar header must be 512 bytes");

	static std::string octalDigits(std::uintmax_t value) {
		std::string digits;
		while (value > 0) {
			digits.insert(digits.begin(), static_cast<char>('0' + (value & 7)));
			value >>= 3;
		}

		if (digits.empty()) digits = "0";
		return digits;
	}

	static void tarOctal(char* dst, std::size_t width, std::uintmax_t value) {
		std::string digits = octalDigits(value);
		while (digits.size() < width - 1) digits.insert(digits.begin(), '0');

		std::memset(dst, 0, width);
		std::memcpy(dst, digits.data(), (std::min)(digits.size(), width - 1));
	}

	static std::uintmax_t tarParseOctal(const char* text, std::size_t length) {
		std::uintmax_t value = 0;
		for (std::size_t i = 0; i < length && text[i] != '\0' && text[i] != ' '; ++i) {
			if (text[i] < '0' || text[i] > '7') break;
			value = (value << 3) | static_cast<std::uintmax_t>(text[i] - '0');
		}

		return value;
	}

	static void tarFillChecksum(TarHeader& header) {
		std::memset(header.chksum, ' ', sizeof(header.chksum));

		std::uintmax_t sum = 0;
		const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&header);
		for (std::size_t i = 0; i < sizeof(header); ++i) sum += bytes[i];

		tarOctal(header.chksum, 7, sum);
		header.chksum[6] = '\0';
		header.chksum[7] = ' ';
	}

	static std::time_t tarFileMtime(const fs::path& src) {
		std::error_code ec;
		const auto written = fs::last_write_time(src, ec);
		if (ec) return 0;

		return std::chrono::system_clock::to_time_t(
			std::chrono::system_clock::now()
			+ std::chrono::duration_cast<std::chrono::system_clock::duration>(
				written - fs::file_time_type::clock::now()));
	}

	static bool tarWriteFileData(FILE* out, const fs::path& src, std::uintmax_t size) {
		FILE* in = openUtf8(pathToUtf8(src), "rb");
		if (in == nullptr) return false;

		char block[kTarBlock];
		std::uintmax_t left = size;
		while (left > 0) {
			const std::size_t want = static_cast<std::size_t>(
				(std::min)(static_cast<std::uintmax_t>(kTarBlock), left));
			const std::size_t got = std::fread(block, 1, want, in);
			if (got == 0) break;

			std::fwrite(block, 1, got, out);
			if (got < kTarBlock) {
				char zero[kTarBlock] = {};
				std::fwrite(zero, 1, kTarBlock - got, out);
			}

			left -= got;
		}

		std::fclose(in);
		return true;
	}

	static std::string tarEntryName(const std::string& rel, bool is_directory) {
		std::string name = rel;
		std::replace(name.begin(), name.end(), '\\', '/');
		if (is_directory && !name.empty() && name.back() != '/') name.push_back('/');
		return name;
	}

	static void tarFillHeader(TarHeader& header, const std::string& name, char typeflag,
			std::uintmax_t size, std::time_t mtime) {
		std::strncpy(header.name, name.c_str(), sizeof(header.name) - 1);
		tarOctal(header.mode, 8, 0644);
		tarOctal(header.uid, 8, 0);
		tarOctal(header.gid, 8, 0);
		header.typeflag = typeflag;
		tarOctal(header.size, 12, size);
		tarOctal(header.mtime, 12, static_cast<std::uintmax_t>(mtime));
		std::memcpy(header.magic, "ustar\0", 6);
		std::memcpy(header.version, "00", 2);
		tarFillChecksum(header);
	}

	static bool tarWriteEntry(FILE* out, const fs::path& src, const std::string& rel,
			bool verbose) {
		std::error_code ec;
		const fs::file_status status = fs::symlink_status(src, ec);
		if (ec) return false;

		const bool is_directory = fs::is_directory(status);
		const std::string name = tarEntryName(rel, is_directory);
		if (name.size() >= sizeof(TarHeader::name)) {
			std::fprintf(stderr, "wbsh: tar: name too long: %s\n", name.c_str());
			return false;
		}

		std::uintmax_t size = is_directory ? 0 : fs::file_size(src, ec);
		if (ec) size = 0;

		TarHeader header{};
		tarFillHeader(header, name, is_directory ? kTarTypeDirectory : kTarTypeFile,
			size, tarFileMtime(src));
		std::fwrite(&header, 1, sizeof(header), out);
		if (verbose) std::fprintf(stderr, "%s\n", name.c_str());

		if (!is_directory && size > 0) return tarWriteFileData(out, src, size);
		return true;
	}

	static void tarAddDirectory(FILE* out, const fs::path& native, const std::string& item,
			bool verbose) {
		std::error_code ec;
		fs::recursive_directory_iterator it(native,
			fs::directory_options::skip_permission_denied, ec);
		if (ec) return;

		for (auto cur = it; cur != fs::recursive_directory_iterator(); cur.increment(ec)) {
			if (ec) break;

			std::error_code rel_ec;
			const std::string rel = item + "/"
				+ pathToUtf8(fs::relative(cur->path(), native, rel_ec));
			tarWriteEntry(out, cur->path(), rel, verbose);
		}
	}

	static bool tarAddItem(Executor& exec, FILE* out, const std::string& item, bool verbose) {
		const fs::path native(toNative(exec, item));
		std::error_code ec;
		if (!fs::exists(native, ec)) {
			perr("tar", item + ": not found");
			return false;
		}

		tarWriteEntry(out, native, item, verbose);
		if (fs::is_directory(native, ec)) tarAddDirectory(out, native, item, verbose);
		return true;
	}

	static int tarCreate(Executor& exec, const std::string& archive,
			const std::vector<std::string>& items, bool verbose) {
		FILE* out = fopenNative(exec, archive, "wb");
		if (out == nullptr) {
			perr("tar", archive + ": " + std::strerror(errno));
			return 1;
		}

		int status = 0;
		for (const auto& item : items) {
			if (!tarAddItem(exec, out, item, verbose)) status = 1;
		}

		char trailer[kTarBlock * kTarTrailerBlocks] = {};
		std::fwrite(trailer, 1, sizeof(trailer), out);
		std::fclose(out);
		return status;
	}

	static void tarSkipPadded(FILE* in, std::uintmax_t size) {
		const std::uintmax_t skip =
			(size + kTarBlock - 1) & ~static_cast<std::uintmax_t>(kTarBlock - 1);
		std::fseek(in, static_cast<long>(skip), SEEK_CUR);
	}

	static void tarCopyFileData(FILE* in, FILE* out, std::uintmax_t size) {
		char block[kTarBlock];
		std::uintmax_t left = size;
		while (left > 0) {
			const std::size_t got = std::fread(block, 1, kTarBlock, in);
			if (got == 0) break;

			const std::size_t take =
				(left >= kTarBlock) ? kTarBlock : static_cast<std::size_t>(left);
			std::fwrite(block, 1, take, out);
			left -= take;
		}
	}

	static bool tarHeaderIsAllZero(const TarHeader& header) {
		const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&header);
		for (std::size_t k = 0; k < sizeof(header); ++k) {
			if (bytes[k] != 0) return false;
		}

		return true;
	}

	static bool tarExtractFile(Executor& exec, FILE* in, const std::string& name,
			std::uintmax_t size) {
		const fs::path native(toNative(exec, name));
		std::error_code ec;
		fs::create_directories(native.parent_path(), ec);

		FILE* out = fopenNative(exec, name, "wb");
		if (out == nullptr) {
			perr("tar", name + ": " + std::strerror(errno));
			tarSkipPadded(in, size);
			return false;
		}

		tarCopyFileData(in, out, size);
		std::fclose(out);
		return true;
	}

	static bool tarProcessEntry(Executor& exec, FILE* in, const TarHeader& header,
			bool verbose, bool list_only) {
		const std::string name(header.name, ::strnlen(header.name, sizeof(header.name)));
		const std::uintmax_t size = tarParseOctal(header.size, sizeof(header.size));
		const bool is_directory = header.typeflag == kTarTypeDirectory
			|| (!name.empty() && name.back() == '/');
		if (verbose || list_only) std::printf("%s\n", name.c_str());

		if (list_only) {
			tarSkipPadded(in, size);
			return true;
		}

		if (is_directory) {
			std::error_code ec;
			fs::create_directories(toNative(exec, name), ec);
			tarSkipPadded(in, size);
			return true;
		}

		return tarExtractFile(exec, in, name, size);
	}

	static int tarExtract(Executor& exec, const std::string& archive,
			bool verbose, bool list_only) {
		FILE* in = fopenNative(exec, archive, "rb");
		if (in == nullptr) {
			perr("tar", archive + ": " + std::strerror(errno));
			return 1;
		}

		int status = 0;
		while (true) {
			TarHeader header{};
			const std::size_t got = std::fread(&header, 1, sizeof(header), in);
			if (got != sizeof(header)) break;
			if (tarHeaderIsAllZero(header)) break;

			if (!tarProcessEntry(exec, in, header, verbose, list_only)) status = 1;
		}

		std::fclose(in);
		return status;
	}

	struct TarOptions {
		char mode = 0;
		bool verbose = false;
		std::string archive;
		std::vector<std::string> items;
	};

	static void applyTarFlagCluster(const std::vector<std::string>& args, std::size_t& i,
			TarOptions& options) {
		const std::string& cluster = args[i];
		for (std::size_t k = 1; k < cluster.size(); ++k) {
			const char flag = cluster[k];
			if (flag == 'c' || flag == 'x' || flag == 't') {
				options.mode = flag;
			} else if (flag == 'v') {
				options.verbose = true;
			} else if (flag == 'f') {
				if (k + 1 < cluster.size()) options.archive = cluster.substr(k + 1);
				else if (i + 1 < args.size()) options.archive = args[++i];
				k = cluster.size();
			}
		}
	}

	static TarOptions parseTarArgs(const std::vector<std::string>& args) {
		TarOptions options;
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (!arg.empty() && arg[0] == '-' && arg.size() > 1) {
				applyTarFlagCluster(args, i, options);
				continue;
			}

			options.items.push_back(arg);
		}

		return options;
	}

	static int builtin_tar(Executor& exec, const std::vector<std::string>& args) {
		const TarOptions options = parseTarArgs(args);
		if (options.mode == 0) {
			perr("tar", "specify -c, -x, or -t");
			return 2;
		}

		if (options.archive.empty()) {
			perr("tar", "missing -f ARCHIVE");
			return 2;
		}

		switch (options.mode) {
		case 'c': return tarCreate(exec, options.archive, options.items, options.verbose);
		case 'x': return tarExtract(exec, options.archive, options.verbose, false);
		case 't': return tarExtract(exec, options.archive, false, true);
		}

		return 2;
	}

	static void buildCrcTable(std::uint32_t (&table)[256]) {
		for (std::uint32_t i = 0; i < 256; ++i) {
			std::uint32_t entry = i;
			for (int k = 0; k < 8; ++k) {
				entry = ((entry & 1) != 0) ? (kCrcPolynomial ^ (entry >> 1)) : (entry >> 1);
			}

			table[i] = entry;
		}
	}

	// CRC-32/IEEE 802.3 (poly 0xEDB88320). Required for gzip footer.
	static std::uint32_t crc32Update(std::uint32_t crc, const std::uint8_t* data,
			std::size_t length) {
		static std::uint32_t table[256];
		static bool built = false;
		if (!built) {
			buildCrcTable(table);
			built = true;
		}

		crc ^= 0xFFFFFFFFu;
		for (std::size_t i = 0; i < length; ++i) {
			crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
		}

		return crc ^ 0xFFFFFFFFu;
	}

	static std::vector<std::uint8_t> readAllBytesFromFile(FILE* file) {
		std::vector<std::uint8_t> bytes;
		std::uint8_t chunk[kReadChunk];
		while (true) {
			const std::size_t got = std::fread(chunk, 1, sizeof(chunk), file);
			if (got == 0) break;
			bytes.insert(bytes.end(), chunk, chunk + got);
		}

		return bytes;
	}

	static void appendLe16(std::vector<std::uint8_t>& out, std::uint16_t value) {
		out.push_back(static_cast<std::uint8_t>(value & 0xFF));
		out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
	}

	static void appendLe32(std::vector<std::uint8_t>& out, std::uint32_t value) {
		for (int k = 0; k < 4; ++k) {
			out.push_back(static_cast<std::uint8_t>((value >> (8 * k)) & 0xFF));
		}
	}

	static std::uint16_t readLe16(const std::uint8_t* bytes) {
		return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
	}

	static std::uint32_t readLe32(const std::uint8_t* bytes) {
		return static_cast<std::uint32_t>(bytes[0])
			| (static_cast<std::uint32_t>(bytes[1]) << 8)
			| (static_cast<std::uint32_t>(bytes[2]) << 16)
			| (static_cast<std::uint32_t>(bytes[3]) << 24);
	}

	static void skipZeroTerminated(const std::vector<std::uint8_t>& bytes, std::size_t& i) {
		while (i < bytes.size() && bytes[i] != 0) ++i;
		if (i < bytes.size()) ++i;
	}

	static bool gzipParseHeader(const std::vector<std::uint8_t>& bytes,
			std::size_t& deflate_start, std::size_t& deflate_end) {
		if (bytes.size() < kGzipMinSize) return false;
		if (bytes[0] != 0x1F || bytes[1] != 0x8B) return false;
		if (bytes[2] != 8) return false;   // CM = deflate

		const std::uint8_t flags = bytes[3];
		std::size_t i = 10;   // skip MTIME, XFL, OS
		if ((flags & 0x04) != 0) {     // FEXTRA
			if (i + 2 > bytes.size()) return false;
			const std::size_t extra_length = readLe16(bytes.data() + i);
			i += 2 + extra_length;
		}

		if ((flags & 0x08) != 0) skipZeroTerminated(bytes, i);   // FNAME
		if ((flags & 0x10) != 0) skipZeroTerminated(bytes, i);   // FCOMMENT
		if ((flags & 0x02) != 0) i += 2;                         // FHCRC
		if (i + kGzipFooterSize > bytes.size()) return false;

		deflate_start = i;
		deflate_end = bytes.size() - kGzipFooterSize;
		return true;
	}

	static void appendGzipHeader(std::vector<std::uint8_t>& out) {
		out.push_back(0x1F);
		out.push_back(0x8B);
		out.push_back(0x08);   // CM = deflate
		out.push_back(0x00);   // FLG = 0
		for (int k = 0; k < 4; ++k) out.push_back(0x00);   // MTIME
		out.push_back(0x00);   // XFL
		out.push_back(0xFF);   // OS = unknown
	}

	static void appendStoredBlockHeader(std::vector<std::uint8_t>& out, std::size_t length,
			bool last) {
		out.push_back(last ? 0x01 : 0x00);
		appendLe16(out, static_cast<std::uint16_t>(length));
		appendLe16(out, static_cast<std::uint16_t>(~length));
	}

	static void appendDeflateStoredBlocks(const std::vector<std::uint8_t>& data,
			std::vector<std::uint8_t>& out) {
		const std::size_t size = data.size();
		if (size == 0) appendStoredBlockHeader(out, 0, true);

		std::size_t i = 0;
		while (i < size) {
			const std::size_t take = (std::min)(size - i, kStoredBlockMax);
			const bool last = i + take >= size;
			appendStoredBlockHeader(out, take, last);
			out.insert(out.end(), data.begin() + i, data.begin() + i + take);
			i += take;
		}
	}

	static void appendGzipFooter(const std::vector<std::uint8_t>& data,
			std::vector<std::uint8_t>& out) {
		appendLe32(out, crc32Update(0, data.data(), data.size()));
		appendLe32(out, static_cast<std::uint32_t>(data.size() & 0xFFFFFFFFu));
	}

	static void gzipEncodeStored(const std::vector<std::uint8_t>& data,
			std::vector<std::uint8_t>& out) {
		appendGzipHeader(out);
		appendDeflateStoredBlocks(data, out);
		appendGzipFooter(data, out);
	}

	struct GzipOptions {
		bool decompress = false;
		bool to_stdout = false;
		bool keep = false;
		std::vector<std::string> files;
	};

	static bool isOptionArg(const std::string& arg) {
		return !arg.empty() && arg[0] == '-' && arg != "-";
	}

	static GzipOptions parseGzipArgs(const std::vector<std::string>& args) {
		GzipOptions options;
		for (const auto& arg : args) {
			if (arg == "-d" || arg == "--decompress") options.decompress = true;
			else if (arg == "-c" || arg == "--stdout") options.to_stdout = true;
			else if (arg == "-k" || arg == "--keep") options.keep = true;
			else if (!isOptionArg(arg)) options.files.push_back(arg);
		}

		return options;
	}

	static bool gzipLoadInput(Executor& exec, const std::string& tool, const std::string& path,
			std::vector<std::uint8_t>& input) {
		if (path == "-" || path.empty()) {
			input = readAllBytesFromFile(stdin);
			return true;
		}

		FILE* file = fopenNative(exec, path, "rb");
		if (file == nullptr) {
			perr(tool, path, std::error_code(errno, std::system_category()));
			return false;
		}

		input = readAllBytesFromFile(file);
		std::fclose(file);
		return true;
	}

	static std::string gzipDeriveOutputPath(const std::string& path, bool decompress) {
		if (!decompress) return path + ".gz";

		if (path.size() > 3 && path.substr(path.size() - 3) == ".gz") {
			return path.substr(0, path.size() - 3);
		}

		return path + ".out";
	}

	static int gzipTransform(const GzipOptions& options, const std::string& path,
			const std::vector<std::uint8_t>& input, std::vector<std::uint8_t>& output) {
		if (!options.decompress) {
			gzipEncodeStored(input, output);
			return 0;
		}

		std::size_t deflate_start;
		std::size_t deflate_end;
		if (!gzipParseHeader(input, deflate_start, deflate_end)) {
			std::fprintf(stderr, "wbsh: gunzip: not in gzip format: %s\n", path.c_str());
			return 1;
		}

		if (!inflateRaw(input.data() + deflate_start, deflate_end - deflate_start, output)) {
			std::fprintf(stderr, "wbsh: gunzip: invalid compressed data: %s\n", path.c_str());
			return 1;
		}

		return 0;
	}

	static bool gzipWritesToFile(const GzipOptions& options, const std::string& path) {
		return !options.to_stdout && !path.empty() && path != "-";
	}

	static int gzipProcessOne(Executor& exec, const GzipOptions& options, const std::string& path) {
		const char* tool = options.decompress ? "gunzip" : "gzip";
		std::vector<std::uint8_t> input;
		if (!gzipLoadInput(exec, tool, path, input)) return 1;

		std::vector<std::uint8_t> output;
		if (int status = gzipTransform(options, path, input, output); status != 0) return status;

		FILE* out = stdout;
		if (gzipWritesToFile(options, path)) {
			const std::string out_path = gzipDeriveOutputPath(path, options.decompress);
			out = fopenNative(exec, out_path, "wb");
			if (out == nullptr) {
				perr(tool, out_path, std::error_code(errno, std::system_category()));
				return 1;
			}
		}

		std::fwrite(output.data(), 1, output.size(), out);
		if (out != stdout) std::fclose(out);

		if (gzipWritesToFile(options, path) && !options.keep) {
			std::error_code ec;
			fs::remove(toNative(exec, path), ec);
		}

		return 0;
	}

	static int builtin_gzip(Executor& exec, const std::vector<std::string>& args) {
		GzipOptions options = parseGzipArgs(args);
		if (options.files.empty()) {
			options.to_stdout = true;
			return gzipProcessOne(exec, options, "");
		}

		int status = 0;
		for (const auto& file : options.files) {
			const int file_status = gzipProcessOne(exec, options, file);
			if (file_status != 0) status = file_status;
		}

		return status;
	}

	static int builtin_gunzip(Executor& exec, const std::vector<std::string>& args) {
		std::vector<std::string> gzip_args = { "-d" };
		gzip_args.insert(gzip_args.end(), args.begin(), args.end());
		return builtin_gzip(exec, gzip_args);
	}

	static int builtin_zcat(Executor& exec, const std::vector<std::string>& args) {
		std::vector<std::string> gzip_args = { "-d", "-c" };
		gzip_args.insert(gzip_args.end(), args.begin(), args.end());
		return builtin_gzip(exec, gzip_args);
	}

	static bool zipFindEocd(const std::vector<std::uint8_t>& bytes, std::size_t& pos) {
		if (bytes.size() < kZipEocdSize) return false;

		const std::size_t max_back = (std::min)(bytes.size(), kZipEocdSearchLimit);
		const std::size_t search_floor = bytes.size() - max_back;
		for (std::size_t i = bytes.size() - kZipEocdSize; i + kZipEocdSize >= search_floor; --i) {
			if (readLe32(bytes.data() + i) == kZipEocdSig) {
				pos = i;
				return true;
			}

			if (i == 0) break;
		}

		return false;
	}

	namespace unzip_detail {
		struct UnzipOptions {
			bool list_only = false;
			bool to_stdout = false;
			std::string archive;
			std::vector<std::string> select;
			std::string outdir;
		};

		struct CentralEntry {
			std::uint16_t method;
			std::uint32_t csize;
			std::uint32_t usize;
			std::uint32_t lfh_off;
			std::string name;
		};
	}  // namespace unzip_detail

	using unzip_detail::UnzipOptions;
	using unzip_detail::CentralEntry;

	// -o (overwrite) is the default and -n (never overwrite) is not
	// implemented, so both are accepted and ignored.
	static UnzipOptions parseUnzipArgs(const std::vector<std::string>& args) {
		UnzipOptions options;
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg == "-l") options.list_only = true;
			else if (arg == "-p") options.to_stdout = true;
			else if (arg == "-d" && i + 1 < args.size()) options.outdir = args[++i];
			else if (isOptionArg(arg)) continue;
			else if (options.archive.empty()) options.archive = arg;
			else options.select.push_back(arg);
		}

		return options;
	}

	static bool readZipCentralEntry(const std::vector<std::uint8_t>& bytes, std::size_t& pos,
			CentralEntry& entry) {
		if (pos + kZipCentralEntrySize > bytes.size()) return false;
		const std::uint8_t* record = bytes.data() + pos;
		if (readLe32(record) != kZipCentralDirSig) return false;

		entry.method  = readLe16(record + 10);
		entry.csize   = readLe32(record + 20);
		entry.usize   = readLe32(record + 24);
		const std::uint16_t name_length    = readLe16(record + 28);
		const std::uint16_t extra_length   = readLe16(record + 30);
		const std::uint16_t comment_length = readLe16(record + 32);
		entry.lfh_off = readLe32(record + 42);
		entry.name.assign(reinterpret_cast<const char*>(record + kZipCentralEntrySize),
			name_length);

		pos += kZipCentralEntrySize + name_length + extra_length + comment_length;
		return true;
	}

	static bool decompressZipEntry(const std::vector<std::uint8_t>& bytes,
			const CentralEntry& entry, std::vector<std::uint8_t>& out_data) {
		if (entry.lfh_off + kZipLocalHeaderSize > bytes.size()) return false;
		const std::uint8_t* header = bytes.data() + entry.lfh_off;
		if (readLe32(header) != kZipLocalHeaderSig) return false;

		const std::uint16_t name_length  = readLe16(header + 26);
		const std::uint16_t extra_length = readLe16(header + 28);
		const std::size_t data_off =
			entry.lfh_off + kZipLocalHeaderSize + name_length + extra_length;
		if (data_off + entry.csize > bytes.size()) return false;

		if (entry.method == kZipMethodStored) {
			out_data.assign(bytes.begin() + data_off, bytes.begin() + data_off + entry.csize);
			return true;
		}

		if (entry.method == kZipMethodDeflate) {
			return inflateRaw(bytes.data() + data_off, entry.csize, out_data);
		}

		return false;
	}

	static void writeZipEntryToDisk(Executor& exec, const std::string& outdir,
			const std::string& name, const std::vector<std::uint8_t>& data) {
		const std::string out_path = outdir.empty() ? name : (outdir + "/" + name);
		std::error_code ec;
		if (!name.empty() && name.back() == '/') {
			fs::create_directories(toNative(exec, out_path), ec);
			return;
		}

		const fs::path native = toNative(exec, out_path);
		if (native.has_parent_path()) fs::create_directories(native.parent_path(), ec);

		FILE* out = openUtf8(pathToUtf8(native), "wb");
		if (out == nullptr) {
			perr("unzip", out_path, std::error_code(errno, std::system_category()));
			return;
		}

		std::fwrite(data.data(), 1, data.size(), out);
		std::fclose(out);
	}

	static bool entryIsSelected(const std::vector<std::string>& select, const std::string& name) {
		if (select.empty()) return true;
		for (const auto& wanted : select) {
			if (wanted == name) return true;
		}

		return false;
	}

	static bool unzipLoadArchive(Executor& exec, const std::string& path,
			std::vector<std::uint8_t>& bytes) {
		FILE* file = fopenNative(exec, path, "rb");
		if (file == nullptr) {
			perr("unzip", path, std::error_code(errno, std::system_category()));
			return false;
		}

		bytes = readAllBytesFromFile(file);
		std::fclose(file);
		return true;
	}

	static void unzipHandleEntry(Executor& exec, const UnzipOptions& options,
			const std::vector<std::uint8_t>& bytes, const CentralEntry& entry,
			std::size_t& total_bytes) {
		if (options.list_only) {
			std::printf("%9u  ----------- ------  %s\n",
				static_cast<unsigned>(entry.usize), entry.name.c_str());
			total_bytes += entry.usize;
			return;
		}

		std::vector<std::uint8_t> data;
		if (!decompressZipEntry(bytes, entry, data)) {
			std::fprintf(stderr,
				"wbsh: unzip: inflate / unsupported-method on %s\n", entry.name.c_str());
			return;
		}

		if (options.to_stdout) {
			std::fwrite(data.data(), 1, data.size(), stdout);
			return;
		}

		writeZipEntryToDisk(exec, options.outdir, entry.name, data);
		std::printf("  inflating: %s\n", entry.name.c_str());
	}

	static void printUnzipListHeader(const std::string& archive) {
		std::printf("Archive:  %s\n", archive.c_str());
		std::printf("  Length      Date    Time    Name\n");
		std::printf("---------  ---------- -----   ----\n");
	}

	static void printUnzipListFooter(std::size_t total_bytes, std::uint16_t total) {
		std::printf("---------                     -------\n");
		std::printf("%9zu                     %u files\n",
			total_bytes, static_cast<unsigned>(total));
	}

	static int builtin_unzip(Executor& exec, const std::vector<std::string>& args) {
		const UnzipOptions options = parseUnzipArgs(args);
		if (options.archive.empty()) {
			perr("unzip", "missing archive name");
			return 1;
		}

		std::vector<std::uint8_t> bytes;
		if (!unzipLoadArchive(exec, options.archive, bytes)) return 1;

		std::size_t eocd = 0;
		if (!zipFindEocd(bytes, eocd)) {
			perr("unzip", "not a zip archive: " + options.archive);
			return 1;
		}

		const std::uint16_t total = readLe16(bytes.data() + eocd + 10);
		std::size_t pos = readLe32(bytes.data() + eocd + 16);
		if (options.list_only) printUnzipListHeader(options.archive);

		std::size_t total_bytes = 0;
		for (std::size_t k = 0; k < total; ++k) {
			CentralEntry entry;
			if (!readZipCentralEntry(bytes, pos, entry)) break;
			if (!entryIsSelected(options.select, entry.name)) continue;
			unzipHandleEntry(exec, options, bytes, entry, total_bytes);
		}

		if (options.list_only) printUnzipListFooter(total_bytes, total);
		return 0;
	}

	namespace zip_detail {
		struct ZipCdEntry {
			std::string name;
			std::uint32_t crc;
			std::uint32_t size;
			std::uint32_t lfh_off;
		};

		struct ZipOptions {
			bool recurse = false;
			std::string archive;
			std::vector<std::string> inputs;
		};
	}  // namespace zip_detail

	using zip_detail::ZipCdEntry;
	using zip_detail::ZipOptions;

	static void gatherZipDirectory(const fs::path& dir, std::vector<std::string>& paths) {
		std::error_code ec;
		for (auto it = fs::recursive_directory_iterator(dir, ec);
				it != fs::recursive_directory_iterator(); it.increment(ec)) {
			if (ec) break;
			if (!it->is_regular_file(ec)) continue;

			std::string rel = pathToUtf8(fs::relative(it->path(), fs::current_path(ec)));
			std::replace(rel.begin(), rel.end(), '\\', '/');
			paths.push_back(std::move(rel));
		}
	}

	static std::vector<std::string> gatherZipInputs(Executor& exec,
			const std::vector<std::string>& inputs, bool recurse) {
		std::vector<std::string> paths;
		for (const auto& input : inputs) {
			const fs::path native = toNative(exec, input);
			std::error_code ec;
			if (fs::is_directory(native, ec) && recurse) {
				gatherZipDirectory(native, paths);
			} else if (fs::is_regular_file(native, ec)) {
				paths.push_back(input);
			}
		}

		return paths;
	}

	static void appendZipLocalHeader(std::vector<std::uint8_t>& out, const std::string& path,
			std::uint32_t crc, std::uint32_t size) {
		appendLe32(out, kZipLocalHeaderSig);
		appendLe16(out, kZipVersion);                   // version needed
		appendLe16(out, 0);                             // flags
		appendLe16(out, kZipMethodStored);              // method = stored
		appendLe16(out, 0);                             // mod time
		appendLe16(out, 0);                             // mod date
		appendLe32(out, crc);
		appendLe32(out, size);                          // comp size
		appendLe32(out, size);                          // uncomp size
		appendLe16(out, static_cast<std::uint16_t>(path.size()));
		appendLe16(out, 0);                             // extra
		out.insert(out.end(), path.begin(), path.end());
	}

	static ZipCdEntry writeZipLocalEntry(Executor& exec, const std::string& path,
			std::vector<std::uint8_t>& out) {
		ZipCdEntry meta{ path, 0, 0, 0 };

		FILE* file = fopenNative(exec, path, "rb");
		if (file == nullptr) {
			perr("zip", path, std::error_code(errno, std::system_category()));
			meta.name.clear();
			return meta;
		}

		const std::vector<std::uint8_t> data = readAllBytesFromFile(file);
		std::fclose(file);

		meta.crc = crc32Update(0, data.data(), data.size());
		meta.size = static_cast<std::uint32_t>(data.size());
		meta.lfh_off = static_cast<std::uint32_t>(out.size());
		appendZipLocalHeader(out, path, meta.crc, meta.size);
		out.insert(out.end(), data.begin(), data.end());
		return meta;
	}

	static void appendZipCentralEntry(std::vector<std::uint8_t>& out, const ZipCdEntry& entry) {
		appendLe32(out, kZipCentralDirSig);
		appendLe16(out, kZipVersion);                   // version made
		appendLe16(out, kZipVersion);                   // version needed
		appendLe16(out, 0);                             // flags
		appendLe16(out, kZipMethodStored);              // method
		appendLe16(out, 0);                             // mod time
		appendLe16(out, 0);                             // mod date
		appendLe32(out, entry.crc);
		appendLe32(out, entry.size);                    // comp size
		appendLe32(out, entry.size);                    // uncomp size
		appendLe16(out, static_cast<std::uint16_t>(entry.name.size()));
		appendLe16(out, 0);                             // extra
		appendLe16(out, 0);                             // comment len
		appendLe16(out, 0);                             // disk
		appendLe16(out, 0);                             // int attr
		appendLe32(out, 0);                             // ext attr
		appendLe32(out, entry.lfh_off);
		out.insert(out.end(), entry.name.begin(), entry.name.end());
	}

	static void writeZipCentralDir(std::vector<std::uint8_t>& out,
			const std::vector<ZipCdEntry>& entries) {
		for (const auto& entry : entries) appendZipCentralEntry(out, entry);
	}

	static void writeZipEocd(std::vector<std::uint8_t>& out, std::uint32_t cd_off,
			std::uint32_t cd_size, std::uint16_t entry_count) {
		appendLe32(out, kZipEocdSig);
		appendLe16(out, 0);                             // this disk
		appendLe16(out, 0);                             // central dir disk
		appendLe16(out, entry_count);                   // entries on this disk
		appendLe16(out, entry_count);                   // entries total
		appendLe32(out, cd_size);
		appendLe32(out, cd_off);
		appendLe16(out, 0);                             // comment len
	}

	static ZipOptions parseZipArgs(const std::vector<std::string>& args) {
		ZipOptions options;
		for (const auto& arg : args) {
			if (arg == "-r" || arg == "--recurse-paths") options.recurse = true;
			else if (isOptionArg(arg)) continue;
			else if (options.archive.empty()) options.archive = arg;
			else options.inputs.push_back(arg);
		}

		return options;
	}

	static bool writeZipArchive(Executor& exec, const std::string& archive,
			const std::vector<std::uint8_t>& bytes) {
		FILE* out = fopenNative(exec, archive, "wb");
		if (out == nullptr) {
			perr("zip", archive, std::error_code(errno, std::system_category()));
			return false;
		}

		std::fwrite(bytes.data(), 1, bytes.size(), out);
		std::fclose(out);
		return true;
	}

	static int builtin_zip(Executor& exec, const std::vector<std::string>& args) {
		const ZipOptions options = parseZipArgs(args);
		if (options.archive.empty()) {
			perr("zip", "missing archive name");
			return 1;
		}

		if (options.inputs.empty()) {
			perr("zip", "no input files");
			return 1;
		}

		const std::vector<std::string> paths =
			gatherZipInputs(exec, options.inputs, options.recurse);

		std::vector<std::uint8_t> out;
		std::vector<ZipCdEntry> entries;
		for (const auto& path : paths) {
			ZipCdEntry entry = writeZipLocalEntry(exec, path, out);
			if (entry.name.empty()) continue;

			std::printf("  adding: %s (stored)\n", path.c_str());
			entries.push_back(std::move(entry));
		}

		const std::uint32_t cd_off = static_cast<std::uint32_t>(out.size());
		writeZipCentralDir(out, entries);
		const std::uint32_t cd_size = static_cast<std::uint32_t>(out.size() - cd_off);
		writeZipEocd(out, cd_off, cd_size, static_cast<std::uint16_t>(entries.size()));

		return writeZipArchive(exec, options.archive, out) ? 0 : 1;
	}

	void registerArchiveBuiltins(Executor& exec) {
		exec.registerBuiltin("gzip",   builtin_gzip);
		exec.registerBuiltin("gunzip", builtin_gunzip);
		exec.registerBuiltin("zcat",   builtin_zcat);
		exec.registerBuiltin("zip",    builtin_zip);
		exec.registerBuiltin("unzip",  builtin_unzip);
		exec.registerBuiltin("tar",    builtin_tar);
	}

}  // namespace wbsh

/**
 * @file coreutils_archive.cpp
 * @brief zip and unzip (stored and deflated entries).
 */

#include "coreutils_internal.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "bytestream.h"
#include "executor.h"
#include "deflate.h"
#include "inflate.h"
#include "numparse.h"

namespace wbsh {

	namespace fs = std::filesystem;

	static const std::size_t kReadChunk = 4096;

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
	static const int kZipDeflateLevel = 6;

	std::uint32_t crc32Update(std::uint32_t crc, const std::uint8_t* data,
			std::size_t length) {
		return crc32Of(crc, data, length);
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

	static bool isOptionArg(const std::string& arg) {
		return !arg.empty() && arg[0] == '-' && arg != "-";
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
			std::uint32_t crc = 0;
			std::uint32_t size = 0;
			std::uint32_t compressed_size = 0;
			std::uint32_t lfh_off = 0;
			std::uint16_t method = 0;
			std::uint16_t dos_time = 0;
			std::uint16_t dos_date = 0;
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

	static void appendZipLocalHeader(std::vector<std::uint8_t>& out, const ZipCdEntry& entry) {
		appendLe32(out, kZipLocalHeaderSig);
		appendLe16(out, kZipVersion);                   // version needed
		appendLe16(out, 0);                             // flags
		appendLe16(out, entry.method);
		appendLe16(out, entry.dos_time);
		appendLe16(out, entry.dos_date);
		appendLe32(out, entry.crc);
		appendLe32(out, entry.compressed_size);
		appendLe32(out, entry.size);                    // uncomp size
		appendLe16(out, static_cast<std::uint16_t>(entry.name.size()));
		appendLe16(out, 0);                             // extra
		out.insert(out.end(), entry.name.begin(), entry.name.end());
	}

	// MSVC's file clock counts from 1601-01-01, the FILETIME epoch.
	static std::time_t fileTimeToEpoch(fs::file_time_type stamp) {
		const long long seconds = std::chrono::duration_cast<std::chrono::seconds>(
			stamp.time_since_epoch()).count();
		return static_cast<std::time_t>(seconds - 11644473600LL);
	}

	static void setDosTimestamp(Executor& exec, const std::string& path, ZipCdEntry& entry) {
		std::error_code ec;
		const fs::file_time_type stamp = fs::last_write_time(toNative(exec, path), ec);
		if (ec) return;

		const std::time_t epoch = fileTimeToEpoch(stamp);
		std::tm local{};
		if (localtime_s(&local, &epoch) != 0 || local.tm_year < 80) return;

		entry.dos_time = static_cast<std::uint16_t>((local.tm_hour << 11) | (local.tm_min << 5)
			| (local.tm_sec / 2));
		entry.dos_date = static_cast<std::uint16_t>(((local.tm_year - 80) << 9)
			| ((local.tm_mon + 1) << 5) | local.tm_mday);
	}

	static ZipCdEntry writeZipLocalEntry(Executor& exec, const std::string& path,
			std::vector<std::uint8_t>& out) {
		ZipCdEntry meta;
		meta.name = path;

		FILE* file = fopenNative(exec, path, "rb");
		if (file == nullptr) {
			perr("zip", path, std::error_code(errno, std::system_category()));
			meta.name.clear();
			return meta;
		}

		const std::vector<std::uint8_t> data = readAllBytesFromFile(file);
		std::fclose(file);

		std::vector<std::uint8_t> packed;
		deflateRaw(data.data(), data.size(), kZipDeflateLevel, packed);
		const bool deflated = packed.size() < data.size();

		meta.crc = crc32Update(0, data.data(), data.size());
		meta.size = static_cast<std::uint32_t>(data.size());
		meta.method = deflated ? kZipMethodDeflate : kZipMethodStored;
		meta.compressed_size = static_cast<std::uint32_t>(deflated ? packed.size() : data.size());
		meta.lfh_off = static_cast<std::uint32_t>(out.size());
		setDosTimestamp(exec, path, meta);
		appendZipLocalHeader(out, meta);
		if (deflated) out.insert(out.end(), packed.begin(), packed.end());
		else          out.insert(out.end(), data.begin(), data.end());
		return meta;
	}

	static void appendZipCentralEntry(std::vector<std::uint8_t>& out, const ZipCdEntry& entry) {
		appendLe32(out, kZipCentralDirSig);
		appendLe16(out, kZipVersion);                   // version made
		appendLe16(out, kZipVersion);                   // version needed
		appendLe16(out, 0);                             // flags
		appendLe16(out, entry.method);
		appendLe16(out, entry.dos_time);
		appendLe16(out, entry.dos_date);
		appendLe32(out, entry.crc);
		appendLe32(out, entry.compressed_size);
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

	static void printZipAdding(const ZipCdEntry& entry) {
		if (entry.method == kZipMethodStored) {
			std::printf("  adding: %s (stored 0%%)\n", entry.name.c_str());
			return;
		}

		const unsigned saved = entry.size == 0 ? 0 : static_cast<unsigned>(
			100 - (static_cast<unsigned long long>(entry.compressed_size) * 100) / entry.size);
		std::printf("  adding: %s (deflated %u%%)\n", entry.name.c_str(), saved);
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

			printZipAdding(entry);
			entries.push_back(std::move(entry));
		}

		const std::uint32_t cd_off = static_cast<std::uint32_t>(out.size());
		writeZipCentralDir(out, entries);
		const std::uint32_t cd_size = static_cast<std::uint32_t>(out.size() - cd_off);
		writeZipEocd(out, cd_off, cd_size, static_cast<std::uint16_t>(entries.size()));

		return writeZipArchive(exec, options.archive, out) ? 0 : 1;
	}

	void registerArchiveBuiltins(Executor& exec) {
		exec.registerBuiltin("zip",    builtin_zip);
		exec.registerBuiltin("unzip",  builtin_unzip);
	}

}  // namespace wbsh

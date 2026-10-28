/**
 * @file coreutils_cpio.cpp
 * @brief cpio (read newc / crc / odc, write newc). cpio streams its
 *        archives one member at a time in both directions.
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

		struct CpioOptions {
			CpioMode mode = CpioMode::None;
			bool make_directories = false;
			bool verbose = false;
			bool unconditional = false;
			bool keep_mtime = false;
			std::string archive;
			std::vector<std::string> patterns;
		};

		struct CpioEntry {
			std::string name;
			std::uint32_t mode = 0;
			std::uint64_t size = 0;
			std::int64_t mtime = 0;
			bool newc = true;
		};

		struct CpioWriter {
			ByteSink* sink = nullptr;
			std::uint64_t written = 0;
			bool failed = false;
		};

	}  // namespace cpio_detail

	using cpio_detail::CpioEntry;
	using cpio_detail::CpioMode;
	using cpio_detail::CpioOptions;
	using cpio_detail::CpioWriter;

	static const std::uint32_t kTypeMask      = 0170000;
	static const std::uint32_t kTypeDirectory = 0040000;
	static const std::uint32_t kTypeRegular   = 0100000;
	static const std::uint32_t kTypeSymlink   = 0120000;
	static const std::size_t kNewcHeader = 110;
	static const std::size_t kOdcHeader  = 76;
	static const char kTrailer[] = "TRAILER!!!";

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

	static bool skipToFour(InputBuffer& in) {
		return skipBytes(in, (4 - in.consumed() % 4) % 4);
	}

	// newc / crc: 13 eight-digit hex fields; header plus name, and the data,
	// are each padded to a multiple of four bytes.
	static bool parseNewc(InputBuffer& in, const std::uint8_t* magic, CpioEntry& entry) {
		std::uint8_t header[kNewcHeader];
		std::memcpy(header, magic, 6);
		if (!in.readExact(header + 6, kNewcHeader - 6)) return false;

		entry.newc = true;
		entry.mode = static_cast<std::uint32_t>(hexField(header, 1));
		entry.mtime = static_cast<std::int64_t>(hexField(header, 5));
		entry.size = hexField(header, 6);
		const std::size_t name_size = static_cast<std::size_t>(hexField(header, 11));
		return readName(in, name_size, entry) && skipToFour(in);
	}

	// odc: fixed-width octal fields with no padding at all.
	static bool parseOdc(InputBuffer& in, const std::uint8_t* magic, CpioEntry& entry) {
		std::uint8_t header[kOdcHeader];
		std::memcpy(header, magic, 6);
		if (!in.readExact(header + 6, kOdcHeader - 6)) return false;

		entry.newc = false;
		entry.mode = static_cast<std::uint32_t>(octalField(header, 18, 6));
		entry.mtime = static_cast<std::int64_t>(octalField(header, 48, 11));
		const std::size_t name_size = static_cast<std::size_t>(octalField(header, 59, 6));
		entry.size = octalField(header, 65, 11);
		return readName(in, name_size, entry);
	}

	static bool nextCpioEntry(InputBuffer& in, CpioEntry& entry) {
		std::uint8_t magic[6];
		if (!in.readExact(magic, sizeof(magic))) return false;

		const char* text = reinterpret_cast<const char*>(magic);
		if (std::strncmp(text, "070701", 6) == 0 || std::strncmp(text, "070702", 6) == 0) {
			return parseNewc(in, magic, entry);
		}

		if (std::strncmp(text, "070707", 6) == 0) return parseOdc(in, magic, entry);
		return false;
	}

	static bool finishEntryData(InputBuffer& in, const CpioEntry& entry, std::uint64_t left) {
		return skipBytes(in, left) && (!entry.newc || skipToFour(in));
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

	// Copies the entry's data to @p out, returning how much of it is left
	// unread when the archive ends early.
	static std::uint64_t copyEntryData(InputBuffer& in, std::uint64_t size, std::FILE* out) {
		std::vector<std::uint8_t> chunk(kCopyChunk);
		while (size > 0) {
			const std::size_t want = static_cast<std::size_t>((std::min<std::uint64_t>)(size, chunk.size()));
			if (!in.readExact(chunk.data(), want)) return size;
			std::fwrite(chunk.data(), 1, want, out);
			size -= want;
		}

		return 0;
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

		left = copyEntryData(in, entry.size, out);
		std::fclose(out);
		return left == 0;
	}

	static const std::uint64_t kMaxLinkTarget = 64 * 1024;

	static bool extractCpioLink(Executor& exec, const CpioEntry& entry, InputBuffer& in, std::uint64_t& left) {
		if (entry.size > kMaxLinkTarget) return false;

		std::string target(static_cast<std::size_t>(entry.size), '\0');
		if (!target.empty() && !in.readExact(reinterpret_cast<std::uint8_t*>(&target[0]), target.size())) return false;
		left = 0;

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

	static void appendHexField(std::string& header, std::uint64_t value) {
		char text[16];
		const unsigned long long low = value & 0xFFFFFFFFULL;
		std::snprintf(text, sizeof(text), "%08llX", low);
		header += text;
	}

	static void emit(CpioWriter& writer, const void* data, std::size_t size) {
		if (writer.failed || size == 0) return;
		writer.failed = !writer.sink->write(static_cast<const std::uint8_t*>(data), size);
		writer.written += size;
	}

	static void padTo(CpioWriter& writer, std::uint64_t multiple) {
		static const std::uint8_t kZeros[512] = {};
		emit(writer, kZeros, static_cast<std::size_t>((multiple - writer.written % multiple) % multiple));
	}

	static void writeNewcHeader(CpioWriter& writer, const std::string& name, std::uint32_t mode,
	                            std::int64_t mtime, std::uint64_t size) {
		static std::uint64_t inode = 1;
		std::string header = "070701";
		const std::uint64_t fields[13] = {
			inode++, mode, 0, 0, 1, static_cast<std::uint64_t>(mtime), size, 0, 0, 0, 0,
			name.size() + 1, 0,
		};
		for (const std::uint64_t field : fields) appendHexField(header, field);

		emit(writer, header.data(), header.size());
		emit(writer, name.c_str(), name.size() + 1);
		padTo(writer, 4);
	}

	// Streams exactly @p size bytes of @p path: a file that shrank meanwhile
	// is padded with zeros so the header stays true.
	static bool writeFileData(Executor& exec, CpioWriter& writer, const std::string& name, std::uint64_t size) {
		std::FILE* in = fopenNative(exec, name, "rb");
		if (in == nullptr) return false;

		std::vector<std::uint8_t> chunk(kCopyChunk);
		while (size > 0) {
			const std::size_t want = static_cast<std::size_t>((std::min<std::uint64_t>)(size, chunk.size()));
			const std::size_t got = std::fread(chunk.data(), 1, want, in);
			if (got < want) std::memset(chunk.data() + got, 0, want - got);
			emit(writer, chunk.data(), want);
			size -= want;
		}

		std::fclose(in);
		return true;
	}

	static std::int64_t mtimeOf(const fs::path& path) {
		std::error_code ec;
		const auto stamp = fs::last_write_time(path, ec);
		if (ec) return 0;
		return std::chrono::duration_cast<std::chrono::seconds>(stamp.time_since_epoch()).count()
			- 11644473600LL;
	}

	static bool addCpioMember(Executor& exec, const std::string& name, CpioWriter& writer) {
		const fs::path path = toNative(exec, name);
		std::error_code ec;
		const fs::file_status status = fs::symlink_status(path, ec);
		if (ec || !fs::exists(status)) {
			std::fprintf(stderr, "cpio: %s: No such file or directory\n", name.c_str());
			return false;
		}

		if (fs::is_directory(status)) {
			writeNewcHeader(writer, name, kTypeDirectory | 0755, mtimeOf(path), 0);
			return true;
		}

		if (fs::is_symlink(status)) {
			const std::string target = pathToUtf8(fs::read_symlink(path, ec).generic_wstring());
			writeNewcHeader(writer, name, kTypeSymlink | 0777, mtimeOf(path), target.size());
			emit(writer, target.data(), target.size());
			padTo(writer, 4);
			return true;
		}

		const std::uint64_t size = static_cast<std::uint64_t>(fs::file_size(path, ec));
		std::FILE* probe = ec ? nullptr : fopenNative(exec, name, "rb");
		if (probe == nullptr) {
			std::fprintf(stderr, "cpio: %s: %s\n", name.c_str(), std::strerror(errno));
			return false;
		}

		std::fclose(probe);
		writeNewcHeader(writer, name, kTypeRegular | 0644, mtimeOf(path), size);
		const bool ok = writeFileData(exec, writer, name, size);
		padTo(writer, 4);
		return ok;
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
		int status = 0;
		for (const std::string& name : names) {
			if (name.empty()) continue;
			if (options.verbose) std::fprintf(stderr, "%s\n", name.c_str());
			if (!addCpioMember(exec, name, writer)) status = 1;
		}

		writeNewcHeader(writer, kTrailer, 0, 0, 0);
		padTo(writer, 512);
		if (file != stdout) std::fclose(file);
		std::fflush(stdout);
		std::fprintf(stderr, "%llu blocks\n", static_cast<unsigned long long>(writer.written / 512));
		return writer.failed ? 1 : status;
	}

	static bool applyCpioCluster(const std::string& cluster, CpioOptions& options) {
		for (std::size_t k = 1; k < cluster.size(); ++k) {
			switch (cluster[k]) {
			case 'i': options.mode = CpioMode::Extract;  break;
			case 'o': options.mode = CpioMode::Create;   break;
			case 't': options.mode = CpioMode::List;     break;
			case 'd': options.make_directories = true;  break;
			case 'v': options.verbose = true;           break;
			case 'u': options.unconditional = true;     break;
			case 'm': options.keep_mtime = true;        break;
			default:  return false;
			}
		}

		return true;
	}

	static bool parseCpioArgs(const std::vector<std::string>& args, CpioOptions& options) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			const bool has_value = i + 1 < args.size();
			if ((arg == "-F" || arg == "--file") && has_value) {
				options.archive = args[++i];
				continue;
			}

			if (arg == "-H" && has_value) {
				++i;
				continue;
			}

			if (arg.compare(0, 7, "--file=") == 0) options.archive = arg.substr(7);
			else if (arg.size() < 2 || arg[0] != '-') options.patterns.push_back(arg);
			else if (!applyCpioCluster(arg, options)) return false;
		}

		if (options.mode == CpioMode::List && options.patterns.empty()) return true;
		return options.mode != CpioMode::None;
	}

	static int builtin_cpio(Executor& exec, const std::vector<std::string>& args) {
		CpioOptions options;
		if (!parseCpioArgs(args, options)) {
			perr("cpio", "usage: cpio -i|-o|-t [-dvu] [-F ARCHIVE] [PATTERN]...");
			return 2;
		}

		if (options.mode == CpioMode::Create) return createCpio(exec, options);
		return readCpio(exec, options);
	}

	void registerCpioBuiltin(Executor& exec) {
		exec.registerBuiltin("cpio", builtin_cpio);
	}

}  // namespace wbsh

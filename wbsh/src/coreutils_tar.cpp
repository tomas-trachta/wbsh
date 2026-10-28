/**
 * @file coreutils_tar.cpp
 * @brief tar: create, extract and list ustar archives, compressed with
 *        any codec the bundled tools know.
 *
 * Reading auto-detects gzip, bzip2, xz, lzma and compress; writing uses
 * the codec named by -z / -j / -a. Long names are written as GNU 'L'
 * records; pax 'x' headers and base-256 sizes are understood on read.
 * Members are refused when they would land outside the target directory.
 */

#include "coreutils_internal.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <io.h>
#endif /* _WIN32 */

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "codecs.h"
#include "executor.h"
#include "fnmatch.h"
#include "numparse.h"
#include "pathconv.h"

namespace wbsh {

	namespace fs = std::filesystem;

	namespace tar_detail {

		enum class TarMode {
			None,
			Create,
			Extract,
			List,
		};

		struct TarOptions {
			TarMode mode = TarMode::None;
			std::string archive = "-";
			std::string directory;
			Codec codec = Codec::None;
			bool codec_forced = false;
			bool auto_compress = false;
			int verbose = 0;
			bool to_stdout = false;
			bool keep_old = false;
			bool no_mtime = false;
			bool dereference = false;
			bool absolute = false;
			int strip = 0;
			std::vector<std::string> excludes;
			std::vector<std::string> members;
			std::vector<std::string> list_files;
		};

		struct TarEntry {
			std::string name;
			std::string link;
			char type = '0';
			std::uint64_t size = 0;
			std::int64_t mtime = 0;
			unsigned mode = 0644;
			std::string owner = "root";
			std::string group = "root";
			const std::uint8_t* data = nullptr;
		};

		struct TarWriter {
			std::FILE* file = nullptr;
			bool buffered = false;
			std::vector<std::uint8_t> buffer;
			bool failed = false;
		};

		struct PendingNames {
			std::string long_name;
			std::string long_link;
			std::string pax_path;
			std::string pax_link;
			std::uint64_t pax_size = 0;
			bool has_pax_size = false;
		};

		struct ExtractState {
			std::vector<std::pair<fs::path, std::int64_t>> directory_times;
			bool warned_absolute = false;
			int failures = 0;
		};

	}  // namespace tar_detail

	using tar_detail::ExtractState;
	using tar_detail::PendingNames;
	using tar_detail::TarEntry;
	using tar_detail::TarMode;
	using tar_detail::TarOptions;
	using tar_detail::TarWriter;

	static const std::size_t kBlock = 512;
	static const std::int64_t kFileTimeEpochOffset = 11644473600LL;

	// ustar header field offsets and widths (POSIX.1-1988).
	static const std::size_t kNameOffset     = 0;
	static const std::size_t kNameWidth      = 100;
	static const std::size_t kModeOffset     = 100;
	static const std::size_t kUidOffset      = 108;
	static const std::size_t kGidOffset      = 116;
	static const std::size_t kSizeOffset     = 124;
	static const std::size_t kMtimeOffset    = 136;
	static const std::size_t kChecksumOffset = 148;
	static const std::size_t kTypeOffset     = 156;
	static const std::size_t kLinkOffset     = 157;
	static const std::size_t kMagicOffset    = 257;
	static const std::size_t kOwnerOffset    = 265;
	static const std::size_t kGroupOffset    = 297;
	static const std::size_t kPrefixOffset   = 345;
	static const std::size_t kPrefixWidth    = 155;

	static std::uint64_t paddedSize(std::uint64_t size) {
		return (size + kBlock - 1) / kBlock * kBlock;
	}

	static bool takeTarValue(const std::vector<std::string>& args, std::size_t& i,
	                         std::string& value) {
		if (i + 1 >= args.size()) return false;
		value = args[++i];
		return true;
	}

	static bool applyTarModeFlag(char flag, TarOptions& options) {
		switch (flag) {
		case 'c': options.mode = TarMode::Create;  return true;
		case 'x': options.mode = TarMode::Extract; return true;
		case 't': options.mode = TarMode::List;    return true;
		default:  return false;
		}
	}

	static bool applyTarCodecFlag(char flag, TarOptions& options) {
		Codec codec = Codec::None;
		switch (flag) {
		case 'z': codec = Codec::Gzip;     break;
		case 'j': codec = Codec::Bzip2;    break;
		case 'J': codec = Codec::Xz;       break;
		case 'Z': codec = Codec::Compress; break;
		case 'a':
			options.auto_compress = true;
			return true;
		default:
			return false;
		}

		options.codec = codec;
		options.codec_forced = true;
		return true;
	}

	static bool applyTarSimpleFlag(char flag, TarOptions& options) {
		switch (flag) {
		case 'v': ++options.verbose;           return true;
		case 'O': options.to_stdout = true;   return true;
		case 'k': options.keep_old = true;    return true;
		case 'm': options.no_mtime = true;    return true;
		case 'h': options.dereference = true; return true;
		case 'P': options.absolute = true;    return true;
		case 'p':
		case 'o':
		case 'B':
			return true;
		default:
			return applyTarModeFlag(flag, options) || applyTarCodecFlag(flag, options);
		}
	}

	// Value-taking letters in a cluster take the following arguments in
	// order, so `tar -cfT out.tar list` and `tar cvf a.tar dir` both work.
	static bool applyTarCluster(const std::vector<std::string>& args, std::size_t& i,
	                            std::size_t start, TarOptions& options) {
		const std::string cluster = args[i];
		std::size_t value_index = i;
		for (std::size_t k = start; k < cluster.size(); ++k) {
			const char flag = cluster[k];
			if (applyTarSimpleFlag(flag, options)) continue;

			std::string value;
			if (!takeTarValue(args, value_index, value)) return false;
			if (flag == 'f')      options.archive = value;
			else if (flag == 'C') options.directory = value;
			else if (flag == 'T') options.list_files.push_back(value);
			else if (flag == 'X') options.list_files.push_back("\x01" + value);
			else return false;
		}

		i = value_index;
		return true;
	}

	static bool parseStripCount(const std::string& value, TarOptions& options) {
		return parseInt(value, options.strip) && options.strip >= 0;
	}

	static bool applyTarLongValue(const std::string& arg, TarOptions& options) {
		const std::size_t eq = arg.find('=');
		if (eq == std::string::npos) return false;

		const std::string key = arg.substr(0, eq);
		const std::string value = arg.substr(eq + 1);
		if (key == "--file")                options.archive = value;
		else if (key == "--directory")      options.directory = value;
		else if (key == "--exclude")        options.excludes.push_back(value);
		else if (key == "--files-from")     options.list_files.push_back(value);
		else if (key == "--exclude-from")   options.list_files.push_back("\x01" + value);
		else if (key == "--strip-components") return parseStripCount(value, options);
		else return false;
		return true;
	}

	static bool applyTarLongFlag(const std::string& arg, TarOptions& options) {
		static const struct { const char* name; char flag; } kLongFlags[] = {
			{ "--create", 'c' }, { "--extract", 'x' }, { "--get", 'x' }, { "--list", 't' },
			{ "--gzip", 'z' }, { "--gunzip", 'z' }, { "--bzip2", 'j' }, { "--xz", 'J' },
			{ "--compress", 'Z' }, { "--auto-compress", 'a' }, { "--verbose", 'v' },
			{ "--to-stdout", 'O' }, { "--keep-old-files", 'k' }, { "--touch", 'm' },
			{ "--dereference", 'h' }, { "--absolute-names", 'P' }, { "--no-same-owner", 'o' },
			{ "--same-permissions", 'p' }, { "--preserve-permissions", 'p' },
		};
		for (const auto& entry : kLongFlags) {
			if (arg == entry.name) return applyTarSimpleFlag(entry.flag, options);
		}

		if (arg == "--lzma") {
			options.codec = Codec::Lzma;
			options.codec_forced = true;
			return true;
		}

		return arg == "--overwrite" || arg == "--no-recursion" || applyTarLongValue(arg, options);
	}

	static bool parseTarArgs(const std::vector<std::string>& args, TarOptions& options) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			const bool bundled_first = i == 0 && !arg.empty() && arg[0] != '-';
			if (bundled_first || (arg.size() > 1 && arg[0] == '-' && arg[1] != '-')) {
				if (applyTarCluster(args, i, bundled_first ? 0 : 1, options)) continue;
				perr("tar", "invalid option -- '" + arg + "'");
				return false;
			}

			if (arg.size() > 2 && arg.compare(0, 2, "--") == 0) {
				if (applyTarLongFlag(arg, options)) continue;
				perr("tar", "unrecognized option '" + arg + "'");
				return false;
			}

			if (arg == "--") continue;
			options.members.push_back(arg);
		}

		if (options.mode != TarMode::None) return true;
		perr("tar", "you must specify one of the '-c', '-t', '-x' options");
		return false;
	}

	static bool loadListFiles(Executor& exec, TarOptions& options) {
		for (const std::string& spec : options.list_files) {
			const bool exclude = !spec.empty() && spec[0] == '\x01';
			const std::string file = exclude ? spec.substr(1) : spec;
			std::vector<std::string> lines;
			if (!readAllLines(exec, file, lines)) {
				perr("tar", file + ": " + std::strerror(errno));
				return false;
			}

			for (const std::string& line : lines) {
				if (line.empty()) continue;
				if (exclude) options.excludes.push_back(line);
				else         options.members.push_back(line);
			}
		}

		return true;
	}

	static std::string baseName(const std::string& path) {
		const std::size_t slash = path.find_last_of('/');
		return slash == std::string::npos ? path : path.substr(slash + 1);
	}

	static bool isExcluded(const TarOptions& options, const std::string& name) {
		std::string trimmed = name;
		while (trimmed.size() > 1 && trimmed.back() == '/') trimmed.pop_back();
		for (const std::string& pattern : options.excludes) {
			if (fnmatchFull(pattern, trimmed)) return true;
			if (fnmatchFull(pattern, baseName(trimmed))) return true;
		}

		return false;
	}

	static void putText(std::uint8_t* header, std::size_t offset, std::size_t width,
	                    const std::string& text) {
		std::memcpy(header + offset, text.data(), (std::min)(width, text.size()));
	}

	// Fields too large for octal switch to GNU's base-256 form: the top
	// bit of the first byte set, then the value big-endian.
	static void putNumber(std::uint8_t* header, std::size_t offset, std::size_t width,
	                      std::uint64_t value) {
		const std::uint64_t octal_limit = 1ULL << (3 * (width - 1));
		if (value < octal_limit) {
			char text[32];
			std::snprintf(text, sizeof(text), "%0*llo", static_cast<int>(width - 1), value);
			std::memcpy(header + offset, text, width - 1);
			return;
		}

		header[offset] = 0x80;
		for (std::size_t k = width - 1; k > 0; --k) {
			header[offset + k] = static_cast<std::uint8_t>(value & 0xFF);
			value >>= 8;
		}
	}

	static void putChecksum(std::uint8_t* header) {
		std::memset(header + kChecksumOffset, ' ', 8);
		unsigned sum = 0;
		for (std::size_t k = 0; k < kBlock; ++k) sum += header[k];

		char text[8];
		std::snprintf(text, sizeof(text), "%06o", sum);
		std::memcpy(header + kChecksumOffset, text, 7);
	}

	static bool splitUstarName(const std::string& name, std::string& prefix, std::string& rest) {
		if (name.size() <= kNameWidth) {
			rest = name;
			return true;
		}

		for (std::size_t slash = name.find('/'); slash != std::string::npos;
		     slash = name.find('/', slash + 1)) {
			if (slash > kPrefixWidth) return false;
			if (name.size() - slash - 1 > kNameWidth || slash + 1 == name.size()) continue;
			prefix = name.substr(0, slash);
			rest = name.substr(slash + 1);
			return true;
		}

		return false;
	}

	static void writeBytes(TarWriter& writer, const std::uint8_t* data, std::size_t size) {
		if (writer.buffered) {
			writer.buffer.insert(writer.buffer.end(), data, data + size);
			return;
		}

		if (std::fwrite(data, 1, size, writer.file) != size) writer.failed = true;
	}

	static void writePadding(TarWriter& writer, std::uint64_t size) {
		static const std::uint8_t kZeros[kBlock] = {};
		const std::size_t pad = static_cast<std::size_t>(paddedSize(size) - size);
		if (pad != 0) writeBytes(writer, kZeros, pad);
	}

	static void writeHeader(TarWriter& writer, const TarEntry& entry, const std::string& name) {
		std::uint8_t header[kBlock] = {};
		std::string prefix;
		std::string rest;
		splitUstarName(name, prefix, rest);
		putText(header, kNameOffset, kNameWidth, rest);
		putNumber(header, kModeOffset, 8, entry.mode);
		putNumber(header, kUidOffset, 8, 0);
		putNumber(header, kGidOffset, 8, 0);
		putNumber(header, kSizeOffset, 12, entry.size);
		putNumber(header, kMtimeOffset, 12, static_cast<std::uint64_t>((std::max)(entry.mtime,
			static_cast<std::int64_t>(0))));
		header[kTypeOffset] = static_cast<std::uint8_t>(entry.type);
		putText(header, kLinkOffset, kNameWidth, entry.link);
		putText(header, kMagicOffset, 8, std::string("ustar\0" "00", 8));
		putText(header, kOwnerOffset, 32, entry.owner);
		putText(header, kGroupOffset, 32, entry.group);
		putText(header, kPrefixOffset, kPrefixWidth, prefix);
		putChecksum(header);
		writeBytes(writer, header, kBlock);
	}

	static void writeLongRecord(TarWriter& writer, char type, const std::string& text) {
		TarEntry record;
		record.type = type;
		record.size = text.size() + 1;
		record.mode = 0644;
		writeHeader(writer, record, "././@LongLink");
		writeBytes(writer, reinterpret_cast<const std::uint8_t*>(text.c_str()), text.size() + 1);
		writePadding(writer, text.size() + 1);
	}

	static void writeEntry(TarWriter& writer, const TarEntry& entry) {
		std::string prefix;
		std::string rest;
		if (!splitUstarName(entry.name, prefix, rest)) writeLongRecord(writer, 'L', entry.name);
		if (entry.link.size() > kNameWidth) writeLongRecord(writer, 'K', entry.link);

		std::string stored = entry.name;
		if (!splitUstarName(entry.name, prefix, rest)) stored = entry.name.substr(0, kNameWidth);
		writeHeader(writer, entry, stored);
	}

	static std::int64_t epochOf(fs::file_time_type stamp) {
		const long long seconds = std::chrono::duration_cast<std::chrono::seconds>(
			stamp.time_since_epoch()).count();
		return seconds - kFileTimeEpochOffset;
	}

	static fs::file_time_type fileTimeOf(std::int64_t epoch) {
		const auto since = std::chrono::seconds(epoch + kFileTimeEpochOffset);
		return fs::file_time_type(std::chrono::duration_cast<fs::file_time_type::duration>(since));
	}

	static bool looksExecutable(const fs::path& path) {
		std::string extension = path.extension().string();
		for (char& c : extension) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}

		return extension == ".exe" || extension == ".sh" || extension == ".bat"
			|| extension == ".cmd" || extension == ".com";
	}

	static unsigned modeFor(const fs::path& path, fs::file_type type) {
		if (type == fs::file_type::directory) return 0755;
		if (type == fs::file_type::symlink) return 0777;

		std::error_code ec;
		const fs::perms perms = fs::status(path, ec).permissions();
		const bool writable = (perms & fs::perms::owner_write) != fs::perms::none;
		const unsigned base = looksExecutable(path) ? 0755 : 0644;
		return writable ? base : base & 0555;
	}

	static void printVerboseName(const TarOptions& options, const std::string& name) {
		std::FILE* stream = options.archive == "-" ? stderr : stdout;
		std::fprintf(stream, "%s\n", name.c_str());
	}

	static bool addFileContents(TarWriter& writer, const fs::path& path, std::uint64_t size) {
		std::FILE* in = ::_wfopen(path.c_str(), L"rb");
		if (in == nullptr) return false;

		std::vector<std::uint8_t> chunk(64 * 1024);
		std::uint64_t left = size;
		while (left > 0) {
			const std::size_t want = left < chunk.size() ? static_cast<std::size_t>(left)
				: chunk.size();
			const std::size_t got = std::fread(chunk.data(), 1, want, in);
			if (got == 0) {
				std::vector<std::uint8_t> zeros(static_cast<std::size_t>((std::min)(left,
					static_cast<std::uint64_t>(chunk.size()))), 0);
				writeBytes(writer, zeros.data(), zeros.size());
				left -= zeros.size();
				continue;
			}

			writeBytes(writer, chunk.data(), got);
			left -= got;
		}

		std::fclose(in);
		writePadding(writer, size);
		return true;
	}

	static void addTree(Executor& exec, TarWriter& writer, const fs::path& path,
	                    const std::string& name, const TarOptions& options);

	static void addDirectoryChildren(Executor& exec, TarWriter& writer, const fs::path& path,
	                                 const std::string& name, const TarOptions& options) {
		std::error_code ec;
		std::vector<fs::path> children;
		for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) {
			children.push_back(longPathSafe(it->path()));
		}

		std::sort(children.begin(), children.end());
		for (const fs::path& child : children) {
			addTree(exec, writer, child, name + pathToUtf8(child.filename()), options);
		}
	}

	static TarEntry entryFor(const fs::path& path, const std::string& name, fs::file_type type) {
		std::error_code ec;
		TarEntry entry;
		entry.name = name;
		entry.mode = modeFor(path, type);
		entry.mtime = epochOf(fs::last_write_time(path, ec));
		if (type == fs::file_type::directory) {
			entry.type = '5';
			if (entry.name.empty() || entry.name.back() != '/') entry.name.push_back('/');
		} else if (type == fs::file_type::symlink) {
			entry.type = '2';
			entry.link = pathToUtf8(fs::read_symlink(path, ec).generic_wstring());
		} else {
			entry.size = fs::file_size(path, ec);
		}

		return entry;
	}

	static void addTree(Executor& exec, TarWriter& writer, const fs::path& path,
	                    const std::string& name, const TarOptions& options) {
		if (isExcluded(options, name)) return;

		std::error_code ec;
		const fs::file_status status = options.dereference ? fs::status(path, ec)
			: fs::symlink_status(path, ec);
		if (ec || !fs::exists(status)) {
			perr("tar", name + ": Cannot stat: No such file or directory");
			return;
		}

		const TarEntry entry = entryFor(path, name, status.type());
		if (options.verbose > 0) printVerboseName(options, entry.name);
		writeEntry(writer, entry);
		if (entry.type == '0' && !addFileContents(writer, path, entry.size)) {
			perr("tar", name + ": Cannot open: " + std::strerror(errno));
		}

		if (entry.type == '5') addDirectoryChildren(exec, writer, path, entry.name, options);
	}

	static std::string archiveName(const std::string& item, bool absolute) {
		std::string name = item;
		for (char& c : name) {
			if (c == '\\') c = '/';
		}

		if (name.size() >= 2 && name[1] == ':') name.erase(0, 2);
		while (!absolute && !name.empty() && name[0] == '/') name.erase(0, 1);
		while (name.size() > 1 && name.back() == '/') name.pop_back();
		return name.empty() ? "." : name;
	}

	static std::FILE* openArchiveOutput(Executor& exec, const TarOptions& options) {
		if (options.archive != "-") return fopenNative(exec, options.archive, "wb");
		if (isConsoleStream(stdout)) {
			perr("tar", "Refusing to write archive contents to terminal (missing -f option?)");
			return nullptr;
		}

		return stdout;
	}

	static bool finishArchive(TarWriter& writer, Codec codec) {
		static const std::uint8_t kTrailer[kBlock * 2] = {};
		writeBytes(writer, kTrailer, sizeof(kTrailer));
		if (!writer.buffered) return !writer.failed;

		std::vector<std::uint8_t> packed;
		std::string error;
		if (!compressWith(codec, writer.buffer.data(), writer.buffer.size(), 6, packed, error)) {
			perr("tar", error);
			return false;
		}

		return std::fwrite(packed.data(), 1, packed.size(), writer.file) == packed.size();
	}

	static fs::path baseDirectory(Executor& exec, const TarOptions& options) {
		if (options.directory.empty()) return fs::path();
		return toNative(exec, options.directory);
	}

	static int createArchive(Executor& exec, const TarOptions& options) {
		if (options.members.empty()) {
			perr("tar", "Cowardly refusing to create an empty archive");
			return 2;
		}

		Codec codec = options.codec;
		if (options.auto_compress && !options.codec_forced) codec = codecForSuffix(options.archive);
		if (codec != Codec::None && codec != Codec::Gzip && codec != Codec::Bzip2) {
			perr("tar", "writing this compression format is not supported (gzip and bzip2 are)");
			return 2;
		}

		TarWriter writer;
		writer.file = openArchiveOutput(exec, options);
		if (writer.file == nullptr) {
			if (options.archive != "-") perr("tar", options.archive + ": " + std::strerror(errno));
			return 2;
		}

		writer.buffered = codec != Codec::None;
		const fs::path base = baseDirectory(exec, options);
		for (const std::string& item : options.members) {
			const fs::path native = base.empty() ? toNative(exec, item) : base / utf8ToPath(item);
			addTree(exec, writer, native, archiveName(item, options.absolute), options);
		}

		const bool ok = finishArchive(writer, codec);
		if (writer.file != stdout) std::fclose(writer.file);
		std::fflush(stdout);
		return ok ? 0 : 2;
	}

	static bool readArchiveBytes(Executor& exec, const TarOptions& options,
	                             std::vector<std::uint8_t>& bytes) {
		std::FILE* in = options.archive == "-" ? stdin : fopenNative(exec, options.archive, "rb");
		if (in == nullptr) {
			perr("tar", options.archive + ": Cannot open: " + std::strerror(errno));
			return false;
		}

		std::vector<std::uint8_t> chunk(64 * 1024);
		for (std::size_t got = 0; (got = std::fread(chunk.data(), 1, chunk.size(), in)) != 0;) {
			const auto end = chunk.begin() + static_cast<std::ptrdiff_t>(got);
			bytes.insert(bytes.end(), chunk.begin(), end);
		}

		if (in != stdin) std::fclose(in);
		return true;
	}

	static bool isPlainTar(const std::vector<std::uint8_t>& bytes) {
		return bytes.size() >= kMagicOffset + 5
			&& std::memcmp(bytes.data() + kMagicOffset, "ustar", 5) == 0;
	}

	static bool loadArchive(Executor& exec, const TarOptions& options,
	                        std::vector<std::uint8_t>& archive) {
		std::vector<std::uint8_t> raw;
		if (!readArchiveBytes(exec, options, raw)) return false;

		Codec codec = options.codec;
		if (!options.codec_forced) {
			codec = isPlainTar(raw) ? Codec::None : detectCodec(raw.data(), raw.size());
		}

		if (codec == Codec::None) {
			archive.swap(raw);
			return true;
		}

		std::string error;
		if (decompressWith(codec, raw.data(), raw.size(), archive, error)) return true;
		perr("tar", "Child returned status 1: " + error);
		return false;
	}

	static std::string fieldText(const std::uint8_t* header, std::size_t offset,
	                             std::size_t width) {
		const char* start = reinterpret_cast<const char*>(header + offset);
		return std::string(start, ::strnlen(start, width));
	}

	static std::uint64_t fieldNumber(const std::uint8_t* header, std::size_t offset,
	                                 std::size_t width) {
		std::uint64_t value = 0;
		if ((header[offset] & 0x80) != 0) {
			for (std::size_t k = 1; k < width; ++k) value = (value << 8) | header[offset + k];
			return value;
		}

		for (std::size_t k = 0; k < width; ++k) {
			const std::uint8_t digit = header[offset + k];
			if (digit == ' ' && value == 0) continue;
			if (digit < '0' || digit > '7') break;
			value = (value << 3) | static_cast<std::uint64_t>(digit - '0');
		}

		return value;
	}

	static bool checksumMatches(const std::uint8_t* header) {
		unsigned unsigned_sum = 0;
		int signed_sum = 0;
		for (std::size_t k = 0; k < kBlock; ++k) {
			const bool in_field = k >= kChecksumOffset && k < kChecksumOffset + 8;
			const std::uint8_t byte = in_field ? ' ' : header[k];
			unsigned_sum += byte;
			signed_sum += static_cast<signed char>(byte);
		}

		const std::uint64_t stored = fieldNumber(header, kChecksumOffset, 8);
		return stored == unsigned_sum || static_cast<std::int64_t>(stored) == signed_sum;
	}

	static bool isZeroBlock(const std::uint8_t* header) {
		for (std::size_t k = 0; k < kBlock; ++k) {
			if (header[k] != 0) return false;
		}

		return true;
	}

	static void applyPaxRecord(const std::string& record, PendingNames& pending) {
		const std::size_t eq = record.find('=');
		if (eq == std::string::npos) return;

		const std::string key = record.substr(0, eq);
		const std::string value = record.substr(eq + 1);
		if (key == "path") pending.pax_path = value;
		if (key == "linkpath") pending.pax_link = value;
		if (key == "size") pending.has_pax_size = parseULL(value, pending.pax_size);
	}

	static void parsePax(const std::uint8_t* data, std::size_t size, PendingNames& pending) {
		std::size_t at = 0;
		while (at < size) {
			const std::size_t space = std::string(reinterpret_cast<const char*>(data + at),
				size - at).find(' ');
			unsigned long long length = 0;
			if (space == std::string::npos
			    || !parseULL(std::string(reinterpret_cast<const char*>(data + at), space), length)
			    || length <= space + 1 || at + length > size) {
				return;
			}

			const std::size_t body = at + space + 1;
			const std::size_t body_length = static_cast<std::size_t>(length) - space - 2;
			const char* text = reinterpret_cast<const char*>(data + body);
			applyPaxRecord(std::string(text, body_length), pending);
			at += static_cast<std::size_t>(length);
		}
	}

	static std::string trimmedText(const std::uint8_t* data, std::uint64_t size) {
		std::string text(reinterpret_cast<const char*>(data), static_cast<std::size_t>(size));
		while (!text.empty() && text.back() == '\0') text.pop_back();
		return text;
	}

	static TarEntry entryFromHeader(const std::uint8_t* header, const PendingNames& pending) {
		TarEntry entry;
		entry.type = static_cast<char>(header[kTypeOffset]);
		entry.size = pending.has_pax_size ? pending.pax_size : fieldNumber(header, kSizeOffset, 12);
		entry.mtime = static_cast<std::int64_t>(fieldNumber(header, kMtimeOffset, 12));
		entry.mode = static_cast<unsigned>(fieldNumber(header, kModeOffset, 8));
		entry.owner = fieldText(header, kOwnerOffset, 32);
		entry.group = fieldText(header, kGroupOffset, 32);
		entry.link = fieldText(header, kLinkOffset, kNameWidth);

		const std::string prefix = fieldText(header, kPrefixOffset, kPrefixWidth);
		const bool ustar = std::memcmp(header + kMagicOffset, "ustar", 5) == 0;
		entry.name = fieldText(header, kNameOffset, kNameWidth);
		if (ustar && !prefix.empty()) entry.name = prefix + "/" + entry.name;
		if (!pending.long_name.empty()) entry.name = pending.long_name;
		if (!pending.pax_path.empty()) entry.name = pending.pax_path;
		if (!pending.long_link.empty()) entry.link = pending.long_link;
		if (!pending.pax_link.empty()) entry.link = pending.pax_link;
		return entry;
	}

	static bool memberSelected(const TarOptions& options, const std::string& name) {
		if (options.members.empty()) return true;

		std::string trimmed = name;
		while (trimmed.size() > 1 && trimmed.back() == '/') trimmed.pop_back();
		for (std::string wanted : options.members) {
			while (wanted.size() > 1 && wanted.back() == '/') wanted.pop_back();
			if (trimmed == wanted) return true;
			if (trimmed.compare(0, wanted.size() + 1, wanted + "/") == 0) return true;
			if (fnmatchFull(wanted, trimmed)) return true;
		}

		return false;
	}

	static const char* typeLetter(char type) {
		switch (type) {
		case '5': return "d";
		case '2': return "l";
		case '1': return "h";
		case '3': return "c";
		case '4': return "b";
		case '6': return "p";
		default:  return "-";
		}
	}

	static std::string permissionText(const TarEntry& entry) {
		static const char kLetters[] = "rwxrwxrwx";
		std::string text = typeLetter(entry.type);
		for (int bit = 0; bit < 9; ++bit) {
			text.push_back((entry.mode & (0400u >> bit)) != 0 ? kLetters[bit] : '-');
		}

		return text;
	}

	static void printLongListing(const TarEntry& entry) {
		const std::time_t when = static_cast<std::time_t>(entry.mtime);
		std::tm local{};
		localtime_s(&local, &when);
		char stamp[32];
		std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M", &local);

		const std::string owner = (entry.owner.empty() ? "0" : entry.owner) + "/"
			+ (entry.group.empty() ? "0" : entry.group);
		std::printf("%s %s %8llu %s %s", permissionText(entry).c_str(), owner.c_str(),
			static_cast<unsigned long long>(entry.size), stamp, entry.name.c_str());
		if (entry.type == '2') std::printf(" -> %s", entry.link.c_str());
		if (entry.type == '1') std::printf(" link to %s", entry.link.c_str());
		std::printf("\n");
	}

	static bool hasParentComponent(const std::string& name) {
		std::size_t start = 0;
		while (start <= name.size()) {
			std::size_t end = name.find('/', start);
			if (end == std::string::npos) end = name.size();
			if (name.compare(start, end - start, "..") == 0 && end - start == 2) return true;
			start = end + 1;
		}

		return false;
	}

	static bool stripComponents(std::string& name, int count) {
		for (int k = 0; k < count; ++k) {
			const std::size_t slash = name.find('/');
			if (slash == std::string::npos) return false;
			name.erase(0, slash + 1);
		}

		return !name.empty();
	}

	static bool safeMemberName(const TarOptions& options, ExtractState& state, std::string& name) {
		if (!options.absolute) {
			bool stripped = false;
			if (name.size() >= 2 && name[1] == ':') {
				name.erase(0, 2);
				stripped = true;
			}

			while (!name.empty() && name[0] == '/') {
				name.erase(0, 1);
				stripped = true;
			}

			if (stripped && !state.warned_absolute) {
				std::fprintf(stderr, "tar: Removing leading '/' from member names\n");
				state.warned_absolute = true;
			}
		}

		if (!options.absolute && hasParentComponent(name)) {
			std::fprintf(stderr, "tar: %s: Member name contains '..'\n", name.c_str());
			++state.failures;
			return false;
		}

		return stripComponents(name, options.strip) && name != "." && name != "./";
	}

	static fs::path outputPath(Executor& exec, const TarOptions& options, const std::string& name) {
		const fs::path base = baseDirectory(exec, options);
		if (base.empty()) return toNative(exec, name);
		return base / utf8ToPath(name);
	}

	static void setTimes(const fs::path& path, const TarEntry& entry, const TarOptions& options) {
		if (options.no_mtime) return;
		std::error_code ec;
		fs::last_write_time(path, fileTimeOf(entry.mtime), ec);
	}

	static bool writeMemberFile(const fs::path& path, const TarEntry& entry,
	                            const TarOptions& options) {
		std::error_code ec;
		if (options.keep_old && fs::exists(path, ec)) {
			std::fprintf(stderr, "tar: %s: Cannot open: File exists\n", entry.name.c_str());
			return false;
		}

		fs::create_directories(path.parent_path(), ec);
		fs::permissions(path, fs::perms::owner_write, fs::perm_options::add, ec);
		std::FILE* out = ::_wfopen(path.c_str(), L"wb");
		if (out == nullptr) {
			std::fprintf(stderr, "tar: %s: Cannot open: %s\n", entry.name.c_str(),
				std::strerror(errno));
			return false;
		}

		const std::size_t size = static_cast<std::size_t>(entry.size);
		const bool ok = std::fwrite(entry.data, 1, size, out) == size;
		std::fclose(out);
		setTimes(path, entry, options);
		if ((entry.mode & 0222) == 0) {
			fs::permissions(path, fs::perms::owner_write, fs::perm_options::remove, ec);
		}

		return ok;
	}

	// Windows needs Developer Mode or an elevated shell for symbolic
	// links; without either, a link to a regular file falls back to a copy.
	static bool writeMemberLink(const fs::path& path, const TarEntry& entry,
	                            const fs::path& hard_target) {
		std::error_code ec;
		fs::create_directories(path.parent_path(), ec);
		fs::remove(path, ec);

		const fs::copy_options overwrite = fs::copy_options::overwrite_existing;
		if (entry.type == '1') {
			fs::create_hard_link(hard_target, path, ec);
			if (ec) fs::copy_file(hard_target, path, overwrite, ec);
		} else {
			const fs::path target = utf8ToPath(entry.link);
			fs::create_symlink(target, path, ec);
			if (ec) fs::copy_file(path.parent_path() / target, path, overwrite, ec);
		}

		if (!ec) return true;
		std::fprintf(stderr, "tar: %s: Cannot create link: %s\n", entry.name.c_str(),
			ec.message().c_str());
		return false;
	}

	static fs::path hardLinkTarget(Executor& exec, const TarOptions& options,
	                               const TarEntry& entry) {
		std::string target = entry.link;
		while (!options.absolute && !target.empty() && target[0] == '/') target.erase(0, 1);
		if (!stripComponents(target, options.strip)) return fs::path();
		return outputPath(exec, options, target);
	}

	static void extractMember(Executor& exec, const TarOptions& options, TarEntry entry,
	                          ExtractState& state) {
		if (!safeMemberName(options, state, entry.name)) return;

		if (options.to_stdout) {
			if (entry.type == '0' || entry.type == '\0' || entry.type == '7') {
				std::fwrite(entry.data, 1, static_cast<std::size_t>(entry.size), stdout);
			}

			return;
		}

		if (options.verbose > 0) std::printf("%s\n", entry.name.c_str());
		const fs::path path = outputPath(exec, options, entry.name);
		std::error_code ec;
		switch (entry.type) {
		case '5':
			fs::create_directories(path, ec);
			state.directory_times.emplace_back(path, entry.mtime);
			break;
		case '1':
		case '2':
			if (!writeMemberLink(path, entry, hardLinkTarget(exec, options, entry))) {
				++state.failures;
			}

			break;
		case '0':
		case '\0':
		case '7':
			if (!writeMemberFile(path, entry, options)) ++state.failures;
			break;
		default:
			std::fprintf(stderr, "tar: %s: Unsupported member type '%c'\n", entry.name.c_str(),
				entry.type);
			break;
		}
	}

	static void handleMember(Executor& exec, const TarOptions& options, const TarEntry& entry,
	                         ExtractState& state) {
		if (!memberSelected(options, entry.name) || isExcluded(options, entry.name)) return;

		if (options.mode == TarMode::Extract) {
			extractMember(exec, options, entry, state);
			return;
		}

		if (options.verbose > 0) printLongListing(entry);
		else                     std::printf("%s\n", entry.name.c_str());
	}

	static bool consumeSpecialHeader(const TarEntry& raw, const std::uint8_t* data,
	                                 PendingNames& pending) {
		switch (raw.type) {
		case 'L': pending.long_name = trimmedText(data, raw.size);         return true;
		case 'K': pending.long_link = trimmedText(data, raw.size);         return true;
		case 'x': parsePax(data, static_cast<std::size_t>(raw.size), pending); return true;
		case 'g': return true;
		default:  return false;
		}
	}

	static int walkArchive(Executor& exec, const TarOptions& options,
	                       const std::vector<std::uint8_t>& archive, ExtractState& state) {
		PendingNames pending;
		std::size_t at = 0;
		while (at + kBlock <= archive.size()) {
			const std::uint8_t* header = archive.data() + at;
			if (isZeroBlock(header)) break;
			if (!checksumMatches(header)) {
				perr("tar", "This does not look like a tar archive");
				return 2;
			}

			TarEntry entry = entryFromHeader(header, pending);
			const std::size_t data_at = at + kBlock;
			if (data_at + entry.size > archive.size()) {
				perr("tar", "Unexpected EOF in archive");
				return 2;
			}

			entry.data = archive.data() + data_at;
			at = data_at + static_cast<std::size_t>(paddedSize(entry.size));
			if (consumeSpecialHeader(entry, entry.data, pending)) continue;

			pending = PendingNames();
			handleMember(exec, options, entry, state);
		}

		return 0;
	}

	static void applyDirectoryTimes(const ExtractState& state, const TarOptions& options) {
		if (options.no_mtime) return;
		for (auto it = state.directory_times.rbegin(); it != state.directory_times.rend(); ++it) {
			std::error_code ec;
			fs::last_write_time(it->first, fileTimeOf(it->second), ec);
		}
	}

	static int readArchive(Executor& exec, const TarOptions& options) {
		std::vector<std::uint8_t> archive;
		if (!loadArchive(exec, options, archive)) return 2;

		ExtractState state;
		const int status = walkArchive(exec, options, archive, state);
		applyDirectoryTimes(state, options);
		std::fflush(stdout);
		if (status != 0) return status;
		return state.failures == 0 ? 0 : 2;
	}

	static int builtin_tar(Executor& exec, const std::vector<std::string>& args) {
		TarOptions options;
		if (!parseTarArgs(args, options) || !loadListFiles(exec, options)) return 2;
		if (options.mode == TarMode::Create) return createArchive(exec, options);
		return readArchive(exec, options);
	}

	void registerTarBuiltin(Executor& exec) {
		exec.registerBuiltin("tar", builtin_tar);
	}

}  // namespace wbsh

/**
 * @file coreutils_file.cpp
 * @brief file: magic-based file type detection.
 *
 * The command line and the file-system side of file(1): names from
 * operands and -f, links (not followed unless -L or POSIXLY_CORRECT),
 * directories, stdin as `-`, the aligned `NAME: ` prefix, -b, -i and its
 * halves, and -z / -Z, which describe the data inside gzip, bzip2, xz,
 * lzma, lzop and compress streams. What the bytes are is filemagic.h's
 * business.
 */

#include "coreutils_internal.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "bytestream.h"
#include "codecs.h"
#include "executor.h"
#include "filemagic.h"
#include "optscan.h"
#include "pathconv.h"
#include "utf8.h"

namespace wbsh {

	namespace fs = std::filesystem;

	namespace filetype_detail {

		enum OptId {
			kOptBrief,
			kOptMime,
			kOptMimeType,
			kOptMimeEncoding,
			kOptDereference,
			kOptNoDereference,
			kOptUncompress,
			kOptUncompressQuiet,
			kOptFilesFrom,
			kOptSeparator,
			kOptNoPad,
			kOptPrintNul,
			kOptErrorExit,
			kOptVersion,
			kOptIgnored,
			kOptIgnoredValue,
		};

		enum class MimeOutput { None, Full, Type, Encoding };

		enum class Unwrap { None, Report, Quiet };

		struct FileOptions {
			bool brief = false;
			bool follow = false;
			bool no_pad = false;
			bool print_nul = false;
			bool error_exit = false;
			bool version = false;
			MimeOutput mime = MimeOutput::None;
			Unwrap unwrap = Unwrap::None;
			std::string separator = ":";
			std::vector<std::string> lists;
			std::vector<std::string> names;
		};

		struct FlagMember {
			int id;
			bool FileOptions::*member;
		};

		class LimitedSink : public ByteSink {
		public:
			LimitedSink(std::vector<std::uint8_t>& out, std::size_t limit)
				: out_(out), limit_(limit) {}

			bool write(const std::uint8_t* data, std::size_t size) override {
				const std::size_t room = limit_ - out_.size();
				out_.insert(out_.end(), data, data + (std::min)(room, size));
				return size <= room;
			}

		private:
			std::vector<std::uint8_t>& out_;
			std::size_t limit_;
		};

	}  // namespace filetype_detail

	using filetype_detail::FileOptions;
	using filetype_detail::FlagMember;
	using filetype_detail::LimitedSink;
	using filetype_detail::MimeOutput;
	using filetype_detail::Unwrap;

	static const std::vector<OptDef>& fileOptionDefs() {
		static const std::vector<OptDef> defs = {
			{ filetype_detail::kOptBrief,           'b', "brief",                OptArg::None },
			{ filetype_detail::kOptMime,            'i', "mime",                 OptArg::None },
			{ filetype_detail::kOptMimeType,        0,   "mime-type",            OptArg::None },
			{ filetype_detail::kOptMimeEncoding,    0,   "mime-encoding",        OptArg::None },
			{ filetype_detail::kOptDereference,     'L', "dereference",          OptArg::None },
			{ filetype_detail::kOptNoDereference,   'h', "no-dereference",       OptArg::None },
			{ filetype_detail::kOptUncompress,      'z', "uncompress",           OptArg::None },
			{ filetype_detail::kOptUncompressQuiet, 'Z', "uncompress-noreport",  OptArg::None },
			{ filetype_detail::kOptFilesFrom,       'f', "files-from",           OptArg::Required },
			{ filetype_detail::kOptSeparator,       'F', "separator",            OptArg::Required },
			{ filetype_detail::kOptNoPad,           'N', "no-pad",               OptArg::None },
			{ filetype_detail::kOptPrintNul,        '0', "print0",               OptArg::None },
			{ filetype_detail::kOptErrorExit,       'E', nullptr,                OptArg::None },
			{ filetype_detail::kOptVersion,         'v', "version",              OptArg::None },
			{ filetype_detail::kOptIgnored,         'k', "keep-going",           OptArg::None },
			{ filetype_detail::kOptIgnored,         's', "special-files",        OptArg::None },
			{ filetype_detail::kOptIgnored,         'p', "preserve-date",        OptArg::None },
			{ filetype_detail::kOptIgnored,         'r', "raw",                  OptArg::None },
			{ filetype_detail::kOptIgnored,         'n', "no-buffer",            OptArg::None },
			{ filetype_detail::kOptIgnored,         'S', "no-sandbox",           OptArg::None },
			{ filetype_detail::kOptIgnoredValue,    'e', "exclude",              OptArg::Required },
			{ filetype_detail::kOptIgnoredValue,    'P', "parameter",            OptArg::Required },
		};
		return defs;
	}

	static bool applyFileFlag(const OptHit& hit, FileOptions& options) {
		static const FlagMember kFlags[] = {
			{ filetype_detail::kOptBrief,     &FileOptions::brief },
			{ filetype_detail::kOptNoPad,     &FileOptions::no_pad },
			{ filetype_detail::kOptPrintNul,  &FileOptions::print_nul },
			{ filetype_detail::kOptErrorExit, &FileOptions::error_exit },
			{ filetype_detail::kOptVersion,   &FileOptions::version },
		};

		for (const FlagMember& flag : kFlags) {
			if (flag.id != hit.id) continue;
			options.*flag.member = true;
			return true;
		}

		return false;
	}

	static void applyFileOption(const OptHit& hit, FileOptions& options) {
		if (applyFileFlag(hit, options)) return;

		switch (hit.id) {
			case filetype_detail::kOptMime:
				options.mime = MimeOutput::Full;
				break;
			case filetype_detail::kOptMimeType:
				options.mime = MimeOutput::Type;
				break;
			case filetype_detail::kOptMimeEncoding:
				options.mime = MimeOutput::Encoding;
				break;
			case filetype_detail::kOptDereference:
			case filetype_detail::kOptNoDereference:
				options.follow = hit.id == filetype_detail::kOptDereference;
				break;
			case filetype_detail::kOptUncompress:
				options.unwrap = Unwrap::Report;
				break;
			case filetype_detail::kOptUncompressQuiet:
				options.unwrap = Unwrap::Quiet;
				break;
			case filetype_detail::kOptFilesFrom:
				options.lists.push_back(hit.value);
				break;
			case filetype_detail::kOptSeparator:
				options.separator = hit.value;
				break;
			default:
				break;
		}
	}

	static bool parseFileArgs(Executor& exec, const std::vector<std::string>& args,
	                          FileOptions& options) {
		options.follow = !exec.env().get("POSIXLY_CORRECT").empty();

		OptScanResult scanned;
		if (!scanOptions("file", args, fileOptionDefs(), scanned)) return false;

		for (const OptHit& hit : scanned.hits) applyFileOption(hit, options);
		options.names = scanned.operands;
		return true;
	}

	static bool readNameList(Executor& exec, const std::string& list, FileOptions& options) {
		std::vector<std::string> lines;
		if (!readAllLines(exec, list, lines)) {
			perr("file", "cannot open `" + list + "' (No such file or directory)");
			return false;
		}

		for (const std::string& line : lines) {
			if (!line.empty()) options.names.push_back(line);
		}

		return true;
	}

	static bool collectNames(Executor& exec, FileOptions& options) {
		for (const std::string& list : options.lists) {
			if (!readNameList(exec, list, options)) return false;
		}

		if (!options.names.empty()) return true;

		perr("file", "Usage: file [-bcdEhikLlNnprsSvzZ0] [--apple] [--extension] [--mime-encoding] "
			"[--mime-type] [-e <testname>] [-F <separator>] [-f <namefile>] [-m <magicfiles>] "
			"[-P <parameter=value>] [--exclude-quiet] <file> ...");
		return false;
	}

	static std::string shownName(const std::string& name) {
		return name == "-" ? "/dev/stdin" : name;
	}

	static std::size_t nameWidth(const FileOptions& options) {
		std::size_t width = 0;
		for (const std::string& name : options.names) width = (std::max)(width, utf8Count(name));
		return width;
	}

	static void printPrefix(const FileOptions& options, const std::string& name,
	                        std::size_t width) {
		if (options.brief) return;

		std::fputs(shownName(name).c_str(), stdout);
		if (options.print_nul) {
			std::fputc('\0', stdout);
			return;
		}

		const std::size_t pad = options.no_pad ? 0 : width - (std::min)(width, utf8Count(name));
		std::printf("%s%s ", options.separator.c_str(), std::string(pad, ' ').c_str());
	}

	static std::string resultText(const FileOptions& options, const MagicResult& result) {
		switch (options.mime) {
			case MimeOutput::Full:
				return result.mime_type + "; charset=" + result.encoding;
			case MimeOutput::Type:
				return result.mime_type;
			case MimeOutput::Encoding:
				return result.encoding;
			default:
				return result.description;
		}
	}

	static MagicResult inodeResult(const char* description, const char* mime) {
		MagicResult result;
		result.description = description;
		result.mime_type = mime;
		return result;
	}

	static bool readSample(std::FILE* in, FileSample& sample) {
		sample.bytes.resize(kMagicSampleBytes);
		const std::size_t got = std::fread(sample.bytes.data(), 1, kMagicSampleBytes, in);
		sample.bytes.resize(got);
		return std::ferror(in) == 0;
	}

	static bool decompressedSample(const FileSample& sample, Codec codec, FileSample& inner) {
		std::FILE* in = sample.file;
		MemorySource memory(sample.bytes.data(), sample.bytes.size());
		FileSource file(in);
		if (in != nullptr) std::fseek(in, 0, SEEK_SET);

		ByteSource& source = in != nullptr ? static_cast<ByteSource&>(file) : memory;
		InputBuffer buffer(source);
		LimitedSink sink(inner.bytes, kMagicSampleBytes);
		std::string error;
		const bool finished = decompressStream(codec, buffer, sink, error);
		if (!finished && !error.empty() && inner.bytes.empty()) return false;

		inner.size = finished ? inner.bytes.size() : inner.bytes.size() + 1;
		return true;
	}

	static MagicResult unwrapped(const FileOptions& options, const FileSample& sample,
	                             const MagicResult& outer) {
		const Codec codec = detectCodec(sample.bytes.data(), sample.bytes.size());
		FileSample inner;
		if (options.unwrap == Unwrap::None || codec == Codec::None) return outer;
		if (!decompressedSample(sample, codec, inner)) return outer;

		MagicResult result = identifyContent(inner);
		if (options.unwrap == Unwrap::Quiet) return result;

		result.description += " (" + outer.description + ")";
		result.encoding += " compressor: " + outer.mime_type;
		return result;
	}

	static MagicResult describeStream(const FileOptions& options, std::FILE* in, bool seekable,
	                                  std::uint64_t size) {
		FileSample sample;
		readSample(in, sample);
		sample.file = seekable ? in : nullptr;
		sample.size = seekable ? size : sample.bytes.size();
		if (!seekable && sample.bytes.size() == kMagicSampleBytes) sample.size += 1;

		MagicResult result = identifyContent(sample);
		if (seekable && sample.bytes.empty()) result.mime_type = "inode/x-empty";
		return unwrapped(options, sample, result);
	}

	static std::string linkTargetText(Executor& exec, const fs::path& native) {
		std::error_code ec;
		const std::string target = pathToUtf8(fs::read_symlink(native, ec));
		if (ec) return "?";

		const bool absolute = target.size() > 1 && (target[1] == ':' || target[0] == '\\');
		if (absolute) return exec.pathConv().toPosix(target);

		std::string text = target;
		std::replace(text.begin(), text.end(), '\\', '/');
		return text;
	}

	static MagicResult describeLink(Executor& exec, const fs::path& native) {
		std::error_code ec;
		const bool resolves = fs::exists(fs::status(native, ec));
		const std::string target = linkTargetText(exec, native);
		const char* kind = resolves ? "symbolic link to " : "broken symbolic link to ";
		const std::string text = kind + target;
		MagicResult result;
		result.description = text;
		result.mime_type = "inode/symlink";
		return result;
	}

	static bool describeRegular(Executor& exec, const FileOptions& options, const std::string& name,
	                            MagicResult& result) {
		std::FILE* in = fopenNative(exec, name, "rb");
		if (in == nullptr) return false;

		std::error_code ec;
		const std::uint64_t size = fs::file_size(toNative(exec, name), ec);
		result = describeStream(options, in, !ec, ec ? 0 : size);
		std::fclose(in);
		return true;
	}

	static bool describeName(Executor& exec, const FileOptions& options, const std::string& name,
	                         MagicResult& result) {
		if (name == "-") {
			result = describeStream(options, stdin, false, 0);
			return true;
		}

		const fs::path native = toNative(exec, name);
		std::error_code ec;
		const fs::file_status status = options.follow ? fs::status(native, ec)
			: fs::symlink_status(native, ec);
		if (ec || !fs::exists(status)) return false;

		if (isLinkStatus(status)) result = describeLink(exec, native);
		else if (fs::is_directory(status)) result = inodeResult("directory", "inode/directory");
		else return describeRegular(exec, options, name, result);
		return true;
	}

	static bool reportOne(Executor& exec, const FileOptions& options, const std::string& name,
	                      std::size_t width) {
		MagicResult result;
		const bool described = describeName(exec, options, name, result);
		if (!described && options.error_exit) {
			perr("file", "cannot open `" + name + "' (No such file or directory)");
			return false;
		}

		printPrefix(options, name, width);
		if (!described) std::printf("cannot open `%s' (No such file or directory)\n", name.c_str());
		else std::printf("%s\n", resultText(options, result).c_str());
		return described;
	}

	static int builtin_file(Executor& exec, const std::vector<std::string>& args) {
		FileOptions options;
		if (!parseFileArgs(exec, args, options)) return 1;
		if (options.version) {
			std::printf("file-5.45 (wbsh)\nmagic file from (built in)\n");
			return 0;
		}

		if (!collectNames(exec, options)) return 1;

		const std::size_t width = nameWidth(options);
		bool ok = true;
		for (const std::string& name : options.names) {
			if (!reportOne(exec, options, name, width)) ok = false;
			if (stdoutFailed()) return stdoutFailureStatus();
			if (!ok && options.error_exit) break;
		}

		std::fflush(stdout);
		return ok || !options.error_exit ? 0 : 1;
	}

	void registerFileTypeBuiltin(Executor& exec) {
		exec.registerBuiltin("file", builtin_file);
	}

}  // namespace wbsh

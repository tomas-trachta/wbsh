/**
 * @file coreutils_compress.cpp
 * @brief The single-file compressors, all sharing gzip's command-line
 *        behaviour: gzip / gunzip / zcat, bzip2 / bunzip2 / bzcat,
 *        xz / unxz / xzcat, lzma / unlzma / lzcat, uncompress.
 *
 * xz and lzma decompress only, as busybox's do; asking them to compress
 * says so instead of writing a file.
 */

#include "coreutils_internal.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <io.h>
#endif /* _WIN32 */

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "bytestream.h"
#include "codecs.h"
#include "executor.h"

namespace wbsh {

	namespace fs = std::filesystem;

	namespace compress_detail {

		struct SuffixRule {
			const char* compressed;
			const char* plain;
		};

		struct CompressTool {
			const char* name;
			Codec codec;
			const SuffixRule* suffixes;
			bool decompress;
			bool to_stdout;
			int default_level;
		};

		struct CompressOptions {
			bool decompress = false;
			bool to_stdout = false;
			bool keep = false;
			bool force = false;
			bool test = false;
			bool quiet = false;
			bool verbose = false;
			int level = 6;
			std::string suffix;
			std::vector<std::string> files;
		};

	}  // namespace compress_detail

	using compress_detail::CompressOptions;
	using compress_detail::CompressTool;
	using compress_detail::SuffixRule;

	static const SuffixRule kGzipSuffixes[] = {
		{ ".gz", "" }, { ".tgz", ".tar" }, { ".taz", ".tar" }, { "-gz", "" }, { ".z", "" },
		{ "_z", "" }, { ".Z", "" }, { nullptr, nullptr },
	};

	static const SuffixRule kBzip2Suffixes[] = {
		{ ".bz2", "" }, { ".bz", "" }, { ".tbz2", ".tar" }, { ".tbz", ".tar" },
		{ nullptr, nullptr },
	};

	static const SuffixRule kXzSuffixes[] = {
		{ ".xz", "" }, { ".txz", ".tar" }, { ".lzma", "" }, { ".tlz", ".tar" },
		{ nullptr, nullptr },
	};

	static const SuffixRule kLzmaSuffixes[] = {
		{ ".lzma", "" }, { ".tlz", ".tar" }, { nullptr, nullptr },
	};

	static const SuffixRule kCompressSuffixes[] = {
		{ ".Z", "" }, { ".taZ", ".tar" }, { nullptr, nullptr },
	};

	static const CompressTool kTools[] = {
		{ "gzip",       Codec::Gzip,     kGzipSuffixes,     false, false, 6 },
		{ "gunzip",     Codec::Gzip,     kGzipSuffixes,     true,  false, 6 },
		{ "zcat",       Codec::Gzip,     kGzipSuffixes,     true,  true,  6 },
		{ "bzip2",      Codec::Bzip2,    kBzip2Suffixes,    false, false, 9 },
		{ "bunzip2",    Codec::Bzip2,    kBzip2Suffixes,    true,  false, 9 },
		{ "bzcat",      Codec::Bzip2,    kBzip2Suffixes,    true,  true,  9 },
		{ "xz",         Codec::Xz,       kXzSuffixes,       false, false, 6 },
		{ "unxz",       Codec::Xz,       kXzSuffixes,       true,  false, 6 },
		{ "xzcat",      Codec::Xz,       kXzSuffixes,       true,  true,  6 },
		{ "lzma",       Codec::Lzma,     kLzmaSuffixes,     false, false, 6 },
		{ "unlzma",     Codec::Lzma,     kLzmaSuffixes,     true,  false, 6 },
		{ "lzcat",      Codec::Lzma,     kLzmaSuffixes,     true,  true,  6 },
		{ "uncompress", Codec::Compress, kCompressSuffixes, true,  false, 6 },
	};

	static const std::size_t kReadChunk = 64 * 1024;

	static bool applyCompressFlag(char flag, CompressOptions& options) {
		if (flag >= '1' && flag <= '9') {
			options.level = flag - '0';
			return true;
		}

		switch (flag) {
		case 'd': options.decompress = true; return true;
		case 'c': options.to_stdout = true;  return true;
		case 'k': options.keep = true;       return true;
		case 'f': options.force = true;      return true;
		case 't': options.test = true;       return true;
		case 'q': options.quiet = true;      return true;
		case 'v': options.verbose = true;    return true;
		case 'n':
		case 'N':
		case 'z':
			return true;
		default:
			return false;
		}
	}

	static bool applyCompressCluster(const std::string& cluster, CompressOptions& options) {
		for (std::size_t k = 1; k < cluster.size(); ++k) {
			if (!applyCompressFlag(cluster[k], options)) return false;
		}

		return true;
	}

	static bool applyLongCompressOption(const std::string& arg, CompressOptions& options) {
		if (arg == "--decompress" || arg == "--uncompress") options.decompress = true;
		else if (arg == "--stdout" || arg == "--to-stdout")  options.to_stdout = true;
		else if (arg == "--keep")    options.keep = true;
		else if (arg == "--force")   options.force = true;
		else if (arg == "--test")    options.test = true;
		else if (arg == "--quiet")   options.quiet = true;
		else if (arg == "--verbose") options.verbose = true;
		else if (arg == "--fast")    options.level = 1;
		else if (arg == "--best")    options.level = 9;
		else if (arg.compare(0, 9, "--suffix=") == 0) options.suffix = arg.substr(9);
		else return arg == "--no-name" || arg == "--name";
		return true;
	}

	static bool parseCompressArgs(const CompressTool& tool, const std::vector<std::string>& args,
	                              CompressOptions& options) {
		options.decompress = tool.decompress;
		options.to_stdout = tool.to_stdout;
		options.level = tool.default_level;
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg == "-S" && i + 1 < args.size()) {
				options.suffix = args[++i];
				continue;
			}

			if (arg.size() < 2 || arg[0] != '-') {
				options.files.push_back(arg);
				continue;
			}

			const bool known = arg[1] == '-'
				? applyLongCompressOption(arg, options)
				: applyCompressCluster(arg, options);
			if (known) continue;

			perr(tool.name, "invalid option -- '" + arg + "'");
			return false;
		}

		if (options.test) options.decompress = true;
		return true;
	}

	static bool streamIsTerminal(std::FILE* stream) {
		return isConsoleStream(stream);
	}

	static bool acceptsCodec(const CompressTool& tool, Codec found) {
		if (found == tool.codec) return true;
		if (tool.codec == Codec::Gzip) return found == Codec::Compress;
		if (tool.codec == Codec::Xz) return found == Codec::Lzma;
		return false;
	}

	static const char* formatName(Codec codec) {
		switch (codec) {
		case Codec::Gzip:     return "gzip";
		case Codec::Bzip2:    return "bzip2";
		case Codec::Xz:       return "xz";
		case Codec::Lzma:     return "lzma";
		case Codec::Compress: return "compress";
		case Codec::None:
		default:              return "known";
		}
	}

	struct CodecRun {
		const CompressTool& tool;
		const CompressOptions& options;
		std::string name;
		std::uint64_t in_bytes = 0;
		std::uint64_t out_bytes = 0;
	};

	static bool decodeStream(CodecRun& run, InputBuffer& in, ByteSink& out) {
		Codec codec = detectCodec(in);
		if (codec == Codec::None && run.tool.codec == Codec::Lzma) codec = Codec::Lzma;

		std::string error;
		if (!acceptsCodec(run.tool, codec)) {
			if (run.options.force && run.options.to_stdout) return decompressStream(Codec::None, in, out, error);

			perr(run.tool.name, run.name + ": not in " + formatName(run.tool.codec) + " format");
			return false;
		}

		if (decompressStream(codec, in, out, error)) return true;
		if (!error.empty()) perr(run.tool.name, run.name + ": " + error);
		return false;
	}

	static bool encodeStream(CodecRun& run, InputBuffer& in, ByteSink& out) {
		std::string error;
		const std::unique_ptr<Compressor> compressor = makeCompressor(run.tool.codec, run.options.level, out, error);
		if (!compressor) {
			perr(run.tool.name, error + "; use -d to decompress");
			return false;
		}

		std::vector<std::uint8_t> chunk(kReadChunk);
		for (;;) {
			const std::size_t got = in.readSome(chunk.data(), chunk.size());
			if (got == 0) break;
			if (!compressor->write(chunk.data(), got)) return false;
		}

		if (!in.sourceFailed()) return compressor->finish();
		perr(run.tool.name, run.name + ": read error");
		return false;
	}

	// Streams @p input through the codec into @p out; an empty error from the
	// codec means the output went away, which is not worth a message.
	static bool transformStream(CodecRun& run, std::FILE* input, ByteSink& out) {
		FileSource source(input);
		InputBuffer in(source);
		CountingSink counted(out);
		const bool ok = run.options.decompress ? decodeStream(run, in, counted) : encodeStream(run, in, counted);
		run.in_bytes = in.consumed();
		run.out_bytes = counted.count();
		return ok;
	}

	static int compressStdin(const CompressTool& tool, const CompressOptions& options) {
		if (!options.decompress && !options.force && streamIsTerminal(stdout)) {
			perr(tool.name, "compressed data not written to a terminal. "
				"Use -f to force compression.");
			return 1;
		}

		if (options.decompress && !options.force && streamIsTerminal(stdin)) {
			perr(tool.name, "compressed data not read from a terminal. "
				"Use -f to force decompression.");
			return 1;
		}

		CodecRun run{ tool, options, "stdin" };
		FileSink to_stdout(stdout);
		NullSink discard;
		ByteSink& out = options.test ? static_cast<ByteSink&>(discard) : to_stdout;
		const bool ok = transformStream(run, stdin, out);
		std::fflush(stdout);
		return ok ? 0 : 1;
	}

	static bool endsWith(const std::string& text, const std::string& suffix) {
		return !suffix.empty() && text.size() > suffix.size()
			&& text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
	}

	static bool plainName(const CompressTool& tool, const CompressOptions& options,
	                      const std::string& path, std::string& plain) {
		if (endsWith(path, options.suffix)) {
			plain = path.substr(0, path.size() - options.suffix.size());
			return true;
		}

		for (const SuffixRule* rule = tool.suffixes; rule->compressed != nullptr; ++rule) {
			if (!endsWith(path, rule->compressed)) continue;
			plain = path.substr(0, path.size() - std::strlen(rule->compressed)) + rule->plain;
			return true;
		}

		return false;
	}

	static bool outputName(const CompressTool& tool, const CompressOptions& options,
	                       const std::string& path, std::string& target) {
		if (options.decompress) {
			if (plainName(tool, options, path, target)) return true;
			if (!options.quiet) perr(tool.name, path + ": unknown suffix -- ignored");
			return false;
		}

		const std::string suffix = options.suffix.empty() ? tool.suffixes[0].compressed
			: options.suffix;
		std::string ignored;
		if (plainName(tool, options, path, ignored) && !options.force) {
			if (!options.quiet) {
				perr(tool.name, path + " already has " + suffix + " suffix -- unchanged");
			}

			return false;
		}

		target = path + suffix;
		return true;
	}

	static std::FILE* openInput(Executor& exec, const CompressTool& tool, const std::string& path) {
		std::error_code ec;
		if (fs::is_directory(toNative(exec, path), ec)) {
			perr(tool.name, path + " is a directory -- ignored");
			return nullptr;
		}

		std::FILE* in = fopenNative(exec, path, "rb");
		if (in == nullptr) perr(tool.name, path + ": " + std::strerror(errno));
		return in;
	}

	static std::FILE* openTarget(Executor& exec, const CompressTool& tool, const CompressOptions& options,
	                             const std::string& target) {
		std::error_code ec;
		if (!options.force && fs::exists(toNative(exec, target), ec)) {
			perr(tool.name, target + " already exists; not overwritten");
			return nullptr;
		}

		std::FILE* out = fopenNative(exec, target, "wb");
		if (out == nullptr) perr(tool.name, target + ": " + std::strerror(errno));
		return out;
	}

	// A failed in-place run leaves no half-written target behind.
	static bool storeStream(Executor& exec, CodecRun& run, std::FILE* input, const std::string& target) {
		std::FILE* out = openTarget(exec, run.tool, run.options, target);
		if (out == nullptr) return false;

		FileSink sink(out);
		const bool ok = transformStream(run, input, sink);
		const bool closed = std::fclose(out) == 0;
		if (ok && !closed) perr(run.tool.name, target + ": write error");
		if (ok && closed) return true;

		std::error_code ec;
		fs::remove(toNative(exec, target), ec);
		return false;
	}

	static void reportRatio(const std::string& path, const std::string& target,
	                        std::uint64_t before, std::uint64_t after, bool decompressed) {
		const std::uint64_t plain = decompressed ? after : before;
		const std::uint64_t packed = decompressed ? before : after;
		const double saved = plain == 0 ? 0.0
			: 100.0 * (static_cast<double>(plain) - static_cast<double>(packed))
				/ static_cast<double>(plain);
		std::fprintf(stderr, "%s:\t%5.1f%% -- replaced with %s\n", path.c_str(), saved,
			target.c_str());
	}

	static void finishInPlace(Executor& exec, const std::string& path, const std::string& target,
	                          const CompressOptions& options) {
		std::error_code ec;
		const fs::path source = toNative(exec, path);
		const fs::file_time_type stamp = fs::last_write_time(source, ec);
		if (!ec) fs::last_write_time(toNative(exec, target), stamp, ec);
		if (!options.keep) fs::remove(source, ec);
	}

	static int streamToStdout(CodecRun& run, std::FILE* input) {
		FileSink to_stdout(stdout);
		NullSink discard;
		ByteSink& out = run.options.test ? static_cast<ByteSink&>(discard) : to_stdout;
		const bool ok = transformStream(run, input, out);
		if (ok && run.options.test && run.options.verbose) std::fprintf(stderr, "%s:\tOK\n", run.name.c_str());
		return ok ? 0 : 1;
	}

	static int processFile(Executor& exec, const CompressTool& tool, const CompressOptions& options,
	                       const std::string& path) {
		std::string target;
		const bool in_place = !options.to_stdout && !options.test;
		if (in_place && !outputName(tool, options, path, target)) return options.quiet ? 0 : 1;

		std::FILE* input = openInput(exec, tool, path);
		if (input == nullptr) return 1;

		CodecRun run{ tool, options, path };
		if (!in_place) {
			const int status = streamToStdout(run, input);
			std::fclose(input);
			return status;
		}

		const bool stored = storeStream(exec, run, input, target);
		std::fclose(input);
		if (!stored) return 1;

		finishInPlace(exec, path, target, options);
		if (options.verbose) reportRatio(path, target, run.in_bytes, run.out_bytes, options.decompress);
		return 0;
	}

	static int runCompressTool(Executor& exec, const CompressTool& tool,
	                           const std::vector<std::string>& args) {
		CompressOptions options;
		if (!parseCompressArgs(tool, args, options)) return 1;
		if (options.files.empty()) return compressStdin(tool, options);

		int status = 0;
		for (const std::string& file : options.files) {
			const int file_status = file == "-" ? compressStdin(tool, options)
				: processFile(exec, tool, options, file);
			if (file_status != 0) status = file_status;
		}

		std::fflush(stdout);
		return status;
	}

	static const CompressTool& toolNamed(const char* name) {
		for (const CompressTool& tool : kTools) {
			if (std::strcmp(tool.name, name) == 0) return tool;
		}

		return kTools[0];
	}

	static int builtin_gzip(Executor& exec, const std::vector<std::string>& args) {
		return runCompressTool(exec, toolNamed("gzip"), args);
	}

	static int builtin_gunzip(Executor& exec, const std::vector<std::string>& args) {
		return runCompressTool(exec, toolNamed("gunzip"), args);
	}

	static int builtin_zcat(Executor& exec, const std::vector<std::string>& args) {
		return runCompressTool(exec, toolNamed("zcat"), args);
	}

	static int builtin_bzip2(Executor& exec, const std::vector<std::string>& args) {
		return runCompressTool(exec, toolNamed("bzip2"), args);
	}

	static int builtin_bunzip2(Executor& exec, const std::vector<std::string>& args) {
		return runCompressTool(exec, toolNamed("bunzip2"), args);
	}

	static int builtin_bzcat(Executor& exec, const std::vector<std::string>& args) {
		return runCompressTool(exec, toolNamed("bzcat"), args);
	}

	static int builtin_xz(Executor& exec, const std::vector<std::string>& args) {
		return runCompressTool(exec, toolNamed("xz"), args);
	}

	static int builtin_unxz(Executor& exec, const std::vector<std::string>& args) {
		return runCompressTool(exec, toolNamed("unxz"), args);
	}

	static int builtin_xzcat(Executor& exec, const std::vector<std::string>& args) {
		return runCompressTool(exec, toolNamed("xzcat"), args);
	}

	static int builtin_lzma(Executor& exec, const std::vector<std::string>& args) {
		return runCompressTool(exec, toolNamed("lzma"), args);
	}

	static int builtin_unlzma(Executor& exec, const std::vector<std::string>& args) {
		return runCompressTool(exec, toolNamed("unlzma"), args);
	}

	static int builtin_lzcat(Executor& exec, const std::vector<std::string>& args) {
		return runCompressTool(exec, toolNamed("lzcat"), args);
	}

	static int builtin_uncompress(Executor& exec, const std::vector<std::string>& args) {
		return runCompressTool(exec, toolNamed("uncompress"), args);
	}

	void registerCompressBuiltins(Executor& exec) {
		exec.registerBuiltin("gzip",       builtin_gzip);
		exec.registerBuiltin("gunzip",     builtin_gunzip);
		exec.registerBuiltin("zcat",       builtin_zcat);
		exec.registerBuiltin("bzip2",      builtin_bzip2);
		exec.registerBuiltin("bunzip2",    builtin_bunzip2);
		exec.registerBuiltin("bzcat",      builtin_bzcat);
		exec.registerBuiltin("xz",         builtin_xz);
		exec.registerBuiltin("unxz",       builtin_unxz);
		exec.registerBuiltin("xzcat",      builtin_xzcat);
		exec.registerBuiltin("lzma",       builtin_lzma);
		exec.registerBuiltin("unlzma",     builtin_unlzma);
		exec.registerBuiltin("lzcat",      builtin_lzcat);
		exec.registerBuiltin("uncompress", builtin_uncompress);
	}

}  // namespace wbsh

/**
 * @file coreutils_headtail.cpp
 * @brief head and tail: line or byte counts, from the end or the start,
 *        file headers, and tail's follow mode.
 */

#include "coreutils_internal.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <thread>
#include <vector>

#include "executor.h"
#include "interrupt.h"
#include "numparse.h"

namespace wbsh {

	namespace headtail_detail {

		enum class CountUnit {
			Lines,
			Bytes,
		};

		enum class HeaderMode {
			Auto,
			Never,
			Always,
		};

		struct HeadTailOptions {
			CountUnit unit = CountUnit::Lines;
			unsigned long long count = 10;
			bool from_start = false;
			bool all_but_last = false;
			bool follow = false;
			HeaderMode headers = HeaderMode::Auto;
			double sleep_seconds = 1.0;
			std::vector<std::string> files;
		};

		struct ChunkReader {
			std::FILE* stream = nullptr;
			std::vector<char> buffer = std::vector<char>(64 * 1024);
			std::size_t position = 0;
			std::size_t length = 0;

			bool refill() {
				position = 0;
				length = std::fread(buffer.data(), 1, buffer.size(), stream);
				return length != 0;
			}

			// The line keeps its '\n'; only the final line of the input may
			// lack one. False once nothing at all is left.
			bool readLine(std::string& line) {
				line.clear();
				for (;;) {
					if (position == length && !refill()) return !line.empty();
					const char* start = buffer.data() + position;
					const std::size_t available = length - position;
					const void* newline = std::memchr(start, '\n', available);
					const std::size_t take = newline == nullptr
						? available
						: static_cast<std::size_t>(static_cast<const char*>(newline) - start) + 1;
					line.append(start, take);
					position += take;
					if (newline != nullptr) return true;
				}
			}
		};

	}  // namespace headtail_detail

	using headtail_detail::ChunkReader;
	using headtail_detail::CountUnit;
	using headtail_detail::HeaderMode;
	using headtail_detail::HeadTailOptions;

	static const std::size_t kCopyChunk     = 64 * 1024;
	static const long long   kFollowPollMs  = 100;
	static const std::size_t kTrimSlack     = 1024 * 1024;

	static bool parseCountSpec(const std::string& text, bool is_tail, HeadTailOptions& options) {
		const char sign = text.empty() ? '\0' : text[0];
		options.from_start = is_tail && sign == '+';
		options.all_but_last = !is_tail && sign == '-';
		const bool signed_count = options.from_start || options.all_but_last;
		return parseByteSize(signed_count ? text.substr(1) : text, options.count);
	}

	static bool takeOptionValue(const std::vector<std::string>& args, std::size_t& i,
	                            const char* short_flag, const char* long_flag, std::string& value) {
		const std::string& arg = args[i];
		const std::string long_prefix = std::string(long_flag) + "=";
		if (arg.compare(0, long_prefix.size(), long_prefix) == 0) {
			value = arg.substr(long_prefix.size());
			return true;
		}

		if (arg.size() > 2 && arg.compare(0, 2, short_flag) == 0) {
			value = arg.substr(2);
			return true;
		}

		if (arg != short_flag && arg != long_flag) return false;
		if (i + 1 >= args.size()) return false;
		value = args[++i];
		return true;
	}

	static bool isFollowFlag(char flag) {
		return flag == 'f' || flag == 'F';
	}

	static bool applyFlagCluster(const std::string& arg, HeadTailOptions& options, bool is_tail) {
		for (std::size_t k = 1; k < arg.size(); ++k) {
			const char flag = arg[k];
			if (flag == 'q')                         options.headers = HeaderMode::Never;
			else if (flag == 'v')                    options.headers = HeaderMode::Always;
			else if (is_tail && isFollowFlag(flag)) options.follow = true;
			else return false;
		}

		return true;
	}

	static bool applyLongFlag(const std::string& arg, HeadTailOptions& options, bool is_tail) {
		if (arg == "--quiet" || arg == "--silent") options.headers = HeaderMode::Never;
		else if (arg == "--verbose")              options.headers = HeaderMode::Always;
		else if (is_tail && arg == "--follow")    options.follow = true;
		else return false;
		return true;
	}

	static bool applyCountOption(const std::vector<std::string>& args, std::size_t& i,
	                             HeadTailOptions& options, bool is_tail, bool& valid) {
		std::string value;
		valid = true;
		if (takeOptionValue(args, i, "-n", "--lines", value)) {
			options.unit = CountUnit::Lines;
			valid = parseCountSpec(value, is_tail, options);
			return true;
		}

		if (takeOptionValue(args, i, "-c", "--bytes", value)) {
			options.unit = CountUnit::Bytes;
			valid = parseCountSpec(value, is_tail, options);
			return true;
		}

		if (is_tail && takeOptionValue(args, i, "-s", "--sleep-interval", value)) {
			valid = parseDouble(value, options.sleep_seconds);
			return true;
		}

		return false;
	}

	static bool isLegacyCount(const std::string& arg, bool is_tail) {
		if (arg.size() < 2) return false;
		const bool sign_ok = arg[0] == '-' || (is_tail && arg[0] == '+');
		return sign_ok && arg[1] >= '0' && arg[1] <= '9';
	}

	static bool applyLegacyCount(const std::string& arg, HeadTailOptions& options, bool is_tail) {
		options.unit = CountUnit::Lines;
		options.from_start = arg[0] == '+';
		std::string digits = arg.substr(1);
		if (is_tail && !digits.empty() && isFollowFlag(digits.back())) {
			options.follow = true;
			digits.pop_back();
		}

		return parseByteSize(digits, options.count);
	}

	static bool parseOneHeadTailArg(const std::vector<std::string>& args, std::size_t& i,
	                                HeadTailOptions& options, const char* cmd, bool is_tail) {
		const std::string& arg = args[i];
		bool valid = true;
		if (applyCountOption(args, i, options, is_tail, valid)) {
			if (!valid) perr(cmd, "invalid number: " + arg);
			return valid;
		}

		if (isLegacyCount(arg, is_tail)) {
			if (applyLegacyCount(arg, options, is_tail)) return true;
			perr(cmd, "invalid number: " + arg);
			return false;
		}

		if (applyLongFlag(arg, options, is_tail)) return true;
		const bool short_cluster = arg.size() > 1 && arg[0] == '-' && arg[1] != '-';
		if (short_cluster && applyFlagCluster(arg, options, is_tail)) return true;

		perr(cmd, "unknown option: " + arg);
		return false;
	}

	static bool looksLikeOption(const std::string& arg, bool is_tail,
	                            const HeadTailOptions& options) {
		if (arg.size() < 2) return false;
		if (arg[0] == '-') return true;
		return is_tail && options.files.empty() && isLegacyCount(arg, true);
	}

	static bool parseHeadTailArgs(const std::vector<std::string>& args, const char* cmd,
	                              bool is_tail, HeadTailOptions& options) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg == "--") {
				const auto rest = args.begin() + static_cast<std::ptrdiff_t>(i) + 1;
				options.files.insert(options.files.end(), rest, args.end());
				break;
			}

			if (!looksLikeOption(arg, is_tail, options)) {
				options.files.push_back(arg);
				continue;
			}

			if (!parseOneHeadTailArg(args, i, options, cmd, is_tail)) return false;
		}

		if (options.files.empty()) options.files.push_back("-");
		return true;
	}

	static void writeBytes(const char* data, std::size_t size) {
		std::fwrite(data, 1, size, stdout);
	}

	static void writeText(const std::string& text) {
		writeBytes(text.data(), text.size());
	}

	static void copyRest(std::FILE* stream) {
		std::vector<char> chunk(kCopyChunk);
		for (;;) {
			const std::size_t got = std::fread(chunk.data(), 1, chunk.size(), stream);
			if (got == 0) return;
			writeBytes(chunk.data(), got);
		}
	}

	static std::size_t chunkFor(unsigned long long remaining, std::size_t chunk) {
		return remaining < chunk ? static_cast<std::size_t>(remaining) : chunk;
	}

	static void copyBytes(std::FILE* stream, unsigned long long count) {
		std::vector<char> chunk(kCopyChunk);
		while (count > 0) {
			const std::size_t want = chunkFor(count, chunk.size());
			const std::size_t got = std::fread(chunk.data(), 1, want, stream);
			if (got == 0) return;
			writeBytes(chunk.data(), got);
			count -= got;
		}
	}

	static void skipBytes(std::FILE* stream, unsigned long long count) {
		std::vector<char> chunk(kCopyChunk);
		while (count > 0) {
			const std::size_t want = chunkFor(count, chunk.size());
			const std::size_t got = std::fread(chunk.data(), 1, want, stream);
			if (got == 0) return;
			count -= got;
		}
	}

	// As GNU head does on a seekable input, leave the file positioned just
	// after the last line printed, for whatever reads it next. From a pipe
	// the read-ahead is gone, as with GNU head.
	static void giveBackUnread(ChunkReader& reader) {
		const std::size_t unread = reader.length - reader.position;
		if (unread == 0) return;

		_fseeki64(reader.stream, -static_cast<long long>(unread), SEEK_CUR);
	}

	static void headLines(std::FILE* stream, unsigned long long count) {
		ChunkReader reader;
		reader.stream = stream;
		std::string line;
		for (unsigned long long printed = 0; printed < count && reader.readLine(line); ++printed) {
			writeText(line);
		}

		giveBackUnread(reader);
	}

	static void headAllButLastLines(std::FILE* stream, unsigned long long count) {
		ChunkReader reader;
		reader.stream = stream;
		std::deque<std::string> held;
		std::string line;
		while (reader.readLine(line)) {
			held.push_back(line);
			if (held.size() <= count) continue;
			writeText(held.front());
			held.pop_front();
		}
	}

	static void headAllButLastBytes(std::FILE* stream, unsigned long long count) {
		std::string held;
		std::vector<char> chunk(kCopyChunk);
		for (;;) {
			const std::size_t got = std::fread(chunk.data(), 1, chunk.size(), stream);
			if (got == 0) return;
			held.append(chunk.data(), got);
			if (held.size() <= count) continue;
			const std::size_t ready = held.size() - static_cast<std::size_t>(count);
			writeBytes(held.data(), ready);
			held.erase(0, ready);
		}
	}

	static void runHead(std::FILE* stream, const HeadTailOptions& options) {
		const bool bytes = options.unit == CountUnit::Bytes;
		if (options.all_but_last && bytes) headAllButLastBytes(stream, options.count);
		else if (options.all_but_last)     headAllButLastLines(stream, options.count);
		else if (bytes)                    copyBytes(stream, options.count);
		else                               headLines(stream, options.count);
	}

	static void tailFromLine(std::FILE* stream, unsigned long long first) {
		ChunkReader reader;
		reader.stream = stream;
		std::string line;
		unsigned long long number = 1;
		while (number < first && reader.readLine(line)) ++number;

		writeBytes(reader.buffer.data() + reader.position, reader.length - reader.position);
		copyRest(stream);
	}

	static void tailLastLines(std::FILE* stream, unsigned long long count) {
		ChunkReader reader;
		reader.stream = stream;
		std::deque<std::string> held;
		std::string line;
		while (reader.readLine(line)) {
			held.push_back(line);
			if (held.size() > count) held.pop_front();
		}

		for (const std::string& kept : held) writeText(kept);
	}

	static void tailLastBytes(std::FILE* stream, unsigned long long count) {
		std::string held;
		std::vector<char> chunk(kCopyChunk);
		for (;;) {
			const std::size_t got = std::fread(chunk.data(), 1, chunk.size(), stream);
			if (got == 0) break;
			held.append(chunk.data(), got);
			if (held.size() > count && held.size() - count > kTrimSlack) {
				held.erase(0, held.size() - static_cast<std::size_t>(count));
			}
		}

		if (held.size() > count) held.erase(0, held.size() - static_cast<std::size_t>(count));
		writeText(held);
	}

	static void runTail(std::FILE* stream, const HeadTailOptions& options) {
		const bool bytes = options.unit == CountUnit::Bytes;
		const unsigned long long first = options.count == 0 ? 1 : options.count;
		if (options.from_start && bytes) {
			skipBytes(stream, first - 1);
			copyRest(stream);
			return;
		}

		if (options.from_start) tailFromLine(stream, first);
		else if (bytes)         tailLastBytes(stream, options.count);
		else                    tailLastLines(stream, options.count);
	}

	static bool showHeaders(const HeadTailOptions& options) {
		if (options.headers == HeaderMode::Always) return true;
		return options.headers == HeaderMode::Auto && options.files.size() > 1;
	}

	static std::string displayName(const std::string& file) {
		return file == "-" ? "standard input" : file;
	}

	static void printHeader(const std::string& file, bool first) {
		std::printf("%s==> %s <==\n", first ? "" : "\n", displayName(file).c_str());
	}

	static std::FILE* openForHeadTail(Executor& exec, const std::string& file, const char* cmd) {
		std::FILE* stream = file == "-" ? stdin : fopenNative(exec, file, "rb");
		if (stream == nullptr) {
			perr(cmd, "cannot open '" + file + "' for reading: " + std::strerror(errno));
		}

		return stream;
	}

	static int builtin_head(Executor& exec, const std::vector<std::string>& args) {
		HeadTailOptions options;
		if (!parseHeadTailArgs(args, "head", false, options)) return 1;

		int status = 0;
		bool first = true;
		for (const std::string& file : options.files) {
			std::FILE* stream = openForHeadTail(exec, file, "head");
			if (stream == nullptr) {
				status = 1;
				continue;
			}

			if (showHeaders(options)) printHeader(file, first);
			first = false;
			runHead(stream, options);
			if (stream != stdin) std::fclose(stream);
		}

		std::fflush(stdout);
		return status;
	}

	static long long streamSize(std::FILE* stream) {
		const long long here = _ftelli64(stream);
		_fseeki64(stream, 0, SEEK_END);
		const long long size = _ftelli64(stream);
		_fseeki64(stream, here, SEEK_SET);
		return size;
	}

	static bool followOnce(std::FILE* stream, const std::string& file,
	                       const std::string& last_shown, bool headers) {
		const long long position = _ftelli64(stream);
		const long long size = streamSize(stream);
		if (size < position) {
			std::fprintf(stderr, "tail: %s: file truncated\n", file.c_str());
			_fseeki64(stream, 0, SEEK_SET);
		}

		if (size == position) return false;

		std::clearerr(stream);
		if (headers && last_shown != file) printHeader(file, false);
		copyRest(stream);
		std::clearerr(stream);
		std::fflush(stdout);
		return true;
	}

	// Follow ends on Ctrl+C, like the real thing; there is no --pid.
	static void followFiles(const std::vector<std::FILE*>& streams,
	                        const HeadTailOptions& options) {
		ScopedCtrlCCapture capture;
		std::string last_shown = options.files.back();
		const auto interval = std::chrono::milliseconds(
			static_cast<long long>(options.sleep_seconds * 1000.0));
		while (!ctrlCPending()) {
			for (std::size_t k = 0; k < streams.size(); ++k) {
				if (streams[k] == nullptr || streams[k] == stdin) continue;
				if (followOnce(streams[k], options.files[k], last_shown, showHeaders(options))) {
					last_shown = options.files[k];
				}
			}

			const auto deadline = std::chrono::steady_clock::now() + interval;
			while (!ctrlCPending() && std::chrono::steady_clock::now() < deadline) {
				std::this_thread::sleep_for(std::chrono::milliseconds(kFollowPollMs));
			}
		}

		takeCtrlC();
	}

	static int builtin_tail(Executor& exec, const std::vector<std::string>& args) {
		HeadTailOptions options;
		if (!parseHeadTailArgs(args, "tail", true, options)) return 1;

		int status = 0;
		bool first = true;
		std::vector<std::FILE*> streams;
		for (const std::string& file : options.files) {
			std::FILE* stream = openForHeadTail(exec, file, "tail");
			streams.push_back(stream);
			if (stream == nullptr) {
				status = 1;
				continue;
			}

			if (showHeaders(options)) printHeader(file, first);
			first = false;
			runTail(stream, options);
		}

		std::fflush(stdout);
		if (options.follow) followFiles(streams, options);

		for (std::FILE* stream : streams) {
			if (stream != nullptr && stream != stdin) std::fclose(stream);
		}

		return status;
	}

	void registerHeadTailBuiltins(Executor& exec) {
		exec.registerBuiltin("head", builtin_head);
		exec.registerBuiltin("tail", builtin_tail);
	}

}  // namespace wbsh

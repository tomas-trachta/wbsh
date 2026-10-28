/**
 * @file builtins_read.cpp
 * @brief The `read` builtin.
 *
 * Input is read one byte at a time straight from the file descriptor,
 * never through the CRT's FILE buffer, so `read` takes exactly the bytes
 * it consumes and leaves the rest of the stream to whatever runs next.
 */

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>

#  include <fcntl.h>
#  include <io.h>
#endif /* _WIN32 */

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "executor.h"
#include "numparse.h"
#include "stdinsync.h"

namespace wbsh::read_detail {

	static const int kStatusTimeout = 142;
	static const int kReadAheadBytes = 4096;
	static const DWORD kPipePollMillis = 10;
	static const double kMillisPerSecond = 1000.0;

	struct Options {
		bool raw    = false;
		bool silent = false;
		bool exact  = false;
		bool has_timeout = false;
		char delim  = '\n';
		long long nchars = -1;
		double timeout = 0.0;
		int fd = 0;
		std::string prompt;
		std::string array_name;
	};

	enum class End { Delimiter, Count, Eof, Timeout };

	/// The bytes read, and which of them a backslash escaped: those are
	/// never field separators.
	struct Input {
		std::string text;
		std::vector<bool> escaped;

		void push(char c, bool was_escaped) {
			text.push_back(c);
			escaped.push_back(was_escaped);
		}
	};

	static const int kByteEof     = -1;
	static const int kByteTimeout = -2;
	static const int kByteSkip    = -3;

	struct Source {
		int fd = 0;
		HANDLE handle = INVALID_HANDLE_VALUE;
		bool console = false;
		bool file = false;
		bool line_mode = true;
		bool echo = false;
		bool has_deadline = false;
		ULONGLONG deadline = 0;
		std::string ahead;
		std::size_t ahead_pos = 0;
	};

	static const char kOptionLetters[] = "ersa:d:i:n:N:p:t:u:";

	static int reportUsage() {
		std::fprintf(stderr, "read: usage: read [-ers] [-a array] [-d delim] [-i text] "
			"[-n nchars] [-N nchars] [-p prompt] [-t timeout] [-u fd] [name ...]\n");
		return 2;
	}

	static int reportInvalidOption(char letter) {
		std::fprintf(stderr, "wbsh: read: -%c: invalid option\n", letter);
		return reportUsage();
	}

	static int reportMissingArgument(char letter) {
		std::fprintf(stderr, "wbsh: read: -%c: option requires an argument\n", letter);
		return reportUsage();
	}

	static int reportBadValue(const std::string& value, const char* what) {
		std::fprintf(stderr, "wbsh: read: %s: %s\n", value.c_str(), what);
		return 1;
	}

	static bool parseWholeLL(const std::string& text, long long& out) {
		std::size_t used = 0;
		return parseLL(text, out, 10, &used) && used == text.size();
	}

	static int applyCount(const std::string& value, bool exact, Options& opts) {
		long long count = 0;
		if (!parseWholeLL(value, count) || count < 0) return reportBadValue(value, "invalid number");

		opts.nchars = count;
		opts.exact = exact;
		return 0;
	}

	static int applyTimeout(const std::string& value, Options& opts) {
		double seconds = 0.0;
		std::size_t used = 0;
		if (!parseDouble(value, seconds, &used) || used != value.size() || seconds < 0) {
			return reportBadValue(value, "invalid timeout specification");
		}

		opts.has_timeout = true;
		opts.timeout = seconds;
		return 0;
	}

	static int applyFd(const std::string& value, Options& opts) {
		long long fd = 0;
		if (!parseWholeLL(value, fd) || fd < 0 || fd > INT_MAX) {
			return reportBadValue(value, "invalid file descriptor specification");
		}

		opts.fd = static_cast<int>(fd);
		return 0;
	}

	static int applyValueOption(char letter, const std::string& value, Options& opts) {
		switch (letter) {
			case 'a': opts.array_name = value; return 0;
			case 'd': opts.delim = value.empty() ? '\0' : value[0]; return 0;
			case 'p': opts.prompt = value; return 0;
			case 'n': return applyCount(value, false, opts);
			case 'N': return applyCount(value, true, opts);
			case 't': return applyTimeout(value, opts);
			case 'u': return applyFd(value, opts);
			default:  return 0;
		}
	}

	static void applyFlag(char letter, Options& opts) {
		if (letter == 'r') opts.raw = true;
		if (letter == 's') opts.silent = true;
	}

	static bool takesValue(char letter) {
		const char* spec = std::strchr(kOptionLetters, letter);
		return spec != nullptr && spec[1] == ':';
	}

	static int parseCluster(const std::vector<std::string>& args, std::size_t& index,
			Options& opts) {
		const std::string& arg = args[index++];
		for (std::size_t k = 1; k < arg.size(); ++k) {
			const char letter = arg[k];
			if (letter == ':' || std::strchr(kOptionLetters, letter) == nullptr) {
				return reportInvalidOption(letter);
			}

			if (!takesValue(letter)) {
				applyFlag(letter, opts);
				continue;
			}

			if (k + 1 < arg.size()) return applyValueOption(letter, arg.substr(k + 1), opts);
			if (index >= args.size()) return reportMissingArgument(letter);
			return applyValueOption(letter, args[index++], opts);
		}

		return 0;
	}

	static int parseOptions(const std::vector<std::string>& args, Options& opts,
			std::size_t& first) {
		first = 0;
		while (first < args.size() && args[first].size() > 1 && args[first][0] == '-') {
			if (args[first] == "--") {
				++first;
				return 0;
			}

			const int status = parseCluster(args, first, opts);
			if (status != 0) return status;
		}

		return 0;
	}

	static bool isValidName(const std::string& name) {
		if (name.empty()) return false;
		if (!std::isalpha(static_cast<unsigned char>(name[0])) && name[0] != '_') return false;

		for (const char c : name) {
			if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
		}

		return true;
	}

	static bool validateNames(const std::vector<std::string>& names) {
		for (const std::string& name : names) {
			if (isValidName(name)) continue;

			std::fprintf(stderr, "wbsh: read: `%s': not a valid identifier\n", name.c_str());
			return false;
		}

		return true;
	}

	static bool openSource(const Options& opts, Source& source) {
		source.fd = opts.fd;
		source.handle = reinterpret_cast<HANDLE>(_get_osfhandle(opts.fd));
		if (source.handle == INVALID_HANDLE_VALUE) {
			std::fprintf(stderr, "wbsh: read: %d: invalid file descriptor: Bad file descriptor\n",
				opts.fd);
			return false;
		}

		DWORD mode = 0;
		source.console = GetConsoleMode(source.handle, &mode) != 0;
		source.file = GetFileType(source.handle) == FILE_TYPE_DISK;
		if (opts.has_timeout) {
			source.has_deadline = true;
			source.deadline = GetTickCount64()
				+ static_cast<ULONGLONG>(opts.timeout * kMillisPerSecond);
		}

		return true;
	}

	/// Console input normally arrives a line at a time with the console
	/// echoing it; `-n` / `-N` need every key, and `-s` no echo. The fd is
	/// binary meanwhile: in text mode the CRT holds a CR back until it sees
	/// whether a LF follows, which would wait for a second key.
	struct ConsoleModeGuard {
		HANDLE handle = INVALID_HANDLE_VALUE;
		DWORD saved = 0;
		bool mode_changed = false;
		int fd = -1;
		int saved_crt_mode = -1;

		~ConsoleModeGuard() {
			if (mode_changed) SetConsoleMode(handle, saved);
			if (saved_crt_mode != -1) _setmode(fd, saved_crt_mode);
		}
	};

	static DWORD consoleModeFor(const Options& opts, DWORD mode, Source& source) {
		if (opts.silent) mode &= ~static_cast<DWORD>(ENABLE_ECHO_INPUT);
		if (opts.nchars < 0) return mode;

		mode &= ~static_cast<DWORD>(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
		source.line_mode = false;
		source.echo = !opts.silent;
		return mode;
	}

	static void enterConsoleMode(const Options& opts, Source& source, ConsoleModeGuard& guard) {
		if (!source.console) return;

		guard.fd = source.fd;
		guard.saved_crt_mode = _setmode(source.fd, _O_BINARY);
		if (!GetConsoleMode(source.handle, &guard.saved)) return;

		const DWORD mode = consoleModeFor(opts, guard.saved, source);
		if (mode == guard.saved) return;

		guard.handle = source.handle;
		guard.mode_changed = SetConsoleMode(source.handle, mode) != 0;
	}

	static ULONGLONG millisLeft(Source& source) {
		const ULONGLONG now = GetTickCount64();
		return now >= source.deadline ? 0 : source.deadline - now;
	}

	static bool pipeReadyBy(Source& source) {
		for (;;) {
			DWORD available = 0;
			if (!PeekNamedPipe(source.handle, nullptr, 0, nullptr, &available, nullptr)) return true;
			if (available != 0) return true;

			const ULONGLONG left = millisLeft(source);
			if (left == 0) return false;
			Sleep(static_cast<DWORD>(left < kPipePollMillis ? left : kPipePollMillis));
		}
	}

	static bool inputReadyBy(Source& source) {
		if (!source.has_deadline) return true;

		if (source.console) {
			return WaitForSingleObject(source.handle, static_cast<DWORD>(millisLeft(source)))
				== WAIT_OBJECT_0;
		}

		if (GetFileType(source.handle) == FILE_TYPE_PIPE) return pipeReadyBy(source);
		return true;
	}

	static const unsigned char kCtrlD = 0x04;
	static const unsigned char kCtrlZ = 0x1a;

	// On a console Ctrl-Z / Ctrl-D end the input, and Enter's CR is the
	// newline: a line arrives as CR LF, a single key as a bare CR.
	static int consoleByte(Source& source, unsigned char byte) {
		if (byte == kCtrlZ || byte == kCtrlD) return kByteEof;
		if (byte != '\r') return byte;

		return source.line_mode ? kByteSkip : '\n';
	}

	// A file is read a block at a time and the unused rest handed back
	// by giveBackReadAhead(); a pipe or console only ever byte by byte.
	static int nextFileByte(Source& source) {
		if (source.ahead_pos >= source.ahead.size()) {
			source.ahead.resize(kReadAheadBytes);
			const int got = _read(source.fd, &source.ahead[0], kReadAheadBytes);
			source.ahead.resize(got > 0 ? static_cast<std::size_t>(got) : 0);
			source.ahead_pos = 0;
			if (got <= 0) return kByteEof;
		}

		return static_cast<unsigned char>(source.ahead[source.ahead_pos++]);
	}

	static void giveBackReadAhead(Source& source) {
		const std::size_t unused = source.ahead.size() - source.ahead_pos;
		if (unused != 0) _lseeki64(source.fd, -static_cast<long long>(unused), SEEK_CUR);

		source.ahead.clear();
		source.ahead_pos = 0;
	}

	static int readOneByte(Source& source) {
		if (source.fd == 0 && stdinBufferedBytes() > 0) return std::fgetc(stdin);
		if (source.file) return nextFileByte(source);

		unsigned char byte = 0;
		if (_read(source.fd, &byte, 1) != 1) return kByteEof;
		return source.console ? consoleByte(source, byte) : byte;
	}

	static int nextByte(Source& source) {
		int byte = kByteSkip;
		while (byte == kByteSkip) {
			if (!inputReadyBy(source)) return kByteTimeout;
			byte = readOneByte(source);
		}

		if (source.echo && byte >= 0) {
			std::fputc(byte, stderr);
			std::fflush(stderr);
		}

		return byte;
	}

	static bool countReached(const Options& opts, const Input& input) {
		return opts.nchars >= 0 && static_cast<long long>(input.text.size()) >= opts.nchars;
	}

	static bool readEscaped(Source& source, Input& input, End& end) {
		const int next = nextByte(source);
		if (next == kByteEof) {
			end = End::Eof;
			return false;
		}

		if (next == kByteTimeout) {
			end = End::Timeout;
			return false;
		}

		if (next != '\n') input.push(static_cast<char>(next), true);
		return true;
	}

	static End collectInput(const Options& opts, Source& source, Input& input) {
		for (;;) {
			if (countReached(opts, input)) return End::Count;

			const int c = nextByte(source);
			if (c == kByteEof) return End::Eof;
			if (c == kByteTimeout) return End::Timeout;
			if (c == '\0' && opts.delim != '\0') continue;
			if (!opts.exact && c == static_cast<unsigned char>(opts.delim)) return End::Delimiter;

			if (!opts.raw && c == '\\') {
				End end = End::Eof;
				if (!readEscaped(source, input, end)) return end;
				continue;
			}

			input.push(static_cast<char>(c), false);
		}
	}

	static bool isSeparator(const Input& input, std::size_t i, const std::string& ifs) {
		return !input.escaped[i] && ifs.find(input.text[i]) != std::string::npos;
	}

	static bool isSeparatorSpace(const Input& input, std::size_t i, const std::string& ifs) {
		const char c = input.text[i];
		return (c == ' ' || c == '\t' || c == '\n') && isSeparator(input, i, ifs);
	}

	static void skipSpaces(const Input& input, std::size_t& pos, const std::string& ifs) {
		while (pos < input.text.size() && isSeparatorSpace(input, pos, ifs)) ++pos;
	}

	// One non-whitespace IFS character ends a field on its own; the IFS
	// whitespace around it goes with it.
	static void skipSeparator(const Input& input, std::size_t& pos, const std::string& ifs) {
		skipSpaces(input, pos, ifs);
		if (pos < input.text.size() && isSeparator(input, pos, ifs)) ++pos;
		skipSpaces(input, pos, ifs);
	}

	static std::string takeField(const Input& input, std::size_t& pos, const std::string& ifs) {
		const std::size_t start = pos;
		while (pos < input.text.size() && !isSeparator(input, pos, ifs)) ++pos;

		const std::string field = input.text.substr(start, pos - start);
		skipSeparator(input, pos, ifs);
		return field;
	}

	// The last name takes the rest of the line, less trailing IFS
	// whitespace; a lone remaining field also loses its delimiter.
	static std::string takeRest(const Input& input, std::size_t pos, const std::string& ifs) {
		std::size_t probe = pos;
		const std::string field = takeField(input, probe, ifs);
		if (probe >= input.text.size()) return field;

		std::size_t end = input.text.size();
		while (end > pos && isSeparatorSpace(input, end - 1, ifs)) --end;
		return input.text.substr(pos, end - pos);
	}

	static std::vector<std::string> splitForNames(const Input& input, const std::string& ifs,
			std::size_t count) {
		std::vector<std::string> values(count);
		if (ifs.empty()) {
			values[0] = input.text;
			return values;
		}

		std::size_t pos = 0;
		skipSpaces(input, pos, ifs);
		for (std::size_t k = 0; k + 1 < count && pos < input.text.size(); ++k) {
			values[k] = takeField(input, pos, ifs);
		}

		if (pos < input.text.size()) values[count - 1] = takeRest(input, pos, ifs);
		return values;
	}

	static std::vector<std::string> splitAll(const Input& input, const std::string& ifs) {
		std::vector<std::string> fields;
		if (ifs.empty()) {
			if (!input.text.empty()) fields.push_back(input.text);
			return fields;
		}

		std::size_t pos = 0;
		skipSpaces(input, pos, ifs);
		while (pos < input.text.size()) fields.push_back(takeField(input, pos, ifs));
		return fields;
	}

	static int assignArray(Environment& env, const Options& opts, const Input& input) {
		if (env.isReadonly(opts.array_name)) {
			std::fprintf(stderr, "wbsh: %s: readonly variable\n", opts.array_name.c_str());
			return 1;
		}

		const std::string ifs = opts.exact ? std::string() : env.ifs();
		env.setIndexedArrayFromList(opts.array_name, splitAll(input, ifs));
		return 0;
	}

	static int assignNames(Environment& env, const Options& opts,
			const std::vector<std::string>& names, const Input& input) {
		const bool whole_line = names.empty() || opts.exact;
		const std::vector<std::string> targets = names.empty()
			? std::vector<std::string>{ "REPLY" }
			: names;
		const std::vector<std::string> values = whole_line
			? std::vector<std::string>{ input.text }
			: splitForNames(input, env.ifs(), targets.size());

		int status = 0;
		for (std::size_t k = 0; k < targets.size(); ++k) {
			if (env.isReadonly(targets[k])) status = 1;
			env.set(targets[k], k < values.size() ? values[k] : std::string());
		}

		return status;
	}

	static void showPrompt(const Options& opts, Source& source) {
		if (opts.prompt.empty() || _isatty(source.fd) == 0) return;

		std::fwrite(opts.prompt.data(), 1, opts.prompt.size(), stderr);
		std::fflush(stderr);
	}

	static int endStatus(End end) {
		switch (end) {
			case End::Delimiter: return 0;
			case End::Count:     return 0;
			case End::Timeout:   return kStatusTimeout;
			default:             return 1;
		}
	}

	static bool inputAvailableNow(Source& source) {
		if (source.console) return WaitForSingleObject(source.handle, 0) == WAIT_OBJECT_0;
		if (GetFileType(source.handle) != FILE_TYPE_PIPE) return true;

		DWORD available = 0;
		if (!PeekNamedPipe(source.handle, nullptr, 0, nullptr, &available, nullptr)) return true;
		return available != 0;
	}

	static int runRead(Executor& exec, const Options& opts, const std::vector<std::string>& names) {
		Source source;
		if (!openSource(opts, source)) return 1;
		if (opts.has_timeout && opts.timeout == 0.0) return inputAvailableNow(source) ? 0 : 1;

		std::fflush(stdout);
		showPrompt(opts, source);

		if (source.fd == 0) giveBackStdinReadAhead();

		Input input;
		ConsoleModeGuard guard;
		enterConsoleMode(opts, source, guard);
		const End end = collectInput(opts, source, input);
		giveBackReadAhead(source);

		const int assigned = opts.array_name.empty()
			? assignNames(exec.env(), opts, names, input)
			: assignArray(exec.env(), opts, input);
		return end == End::Delimiter || end == End::Count ? assigned : endStatus(end);
	}

	static int builtin_read(Executor& exec, const std::vector<std::string>& args) {
		Options opts;
		std::size_t first = 0;
		const int status = parseOptions(args, opts, first);
		if (status != 0) return status;

		const std::vector<std::string> names(args.begin() + first, args.end());
		if (!validateNames(names)) return 1;
		if (!opts.array_name.empty() && !validateNames({ opts.array_name })) return 1;

		return runRead(exec, opts, names);
	}

}  // namespace wbsh::read_detail

namespace wbsh {

	void registerReadBuiltin(Executor& exec) {
		exec.registerBuiltin("read", read_detail::builtin_read);
	}

}  // namespace wbsh

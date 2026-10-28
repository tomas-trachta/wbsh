/**
 * @file coreutils_xargs.cpp
 * @brief xargs: GNU input parsing (quotes, backslashes, -0 / -d, -E),
 *        command-line length batching, -n / -L / -I, -P parallel
 *        children, -r / -t / -p / -x / -a / -o, and GNU exit statuses.
 */

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>

#  include <fcntl.h>
#  include <io.h>
#endif /* _WIN32 */

#include "coreutils_internal.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "executor.h"
#include "numparse.h"
#include "optscan.h"

namespace wbsh {

	namespace xargs_detail {

		// CreateProcess caps the command line at 32767 UTF-16 units; the rest
		// is headroom for the resolved executable path.
		static const std::size_t kMaxCommandChars = 32000;
		static const std::size_t kMaxParallel = MAXIMUM_WAIT_OBJECTS;
		static const std::size_t kReadChunk = 4096;
		static const int kStatusFailed = 1;
		static const int kStatusChildFailed = 123;
		static const int kStatusChild255 = 124;
		static const int kStatusNotFound = 127;
		static const int kStatusKilled = 125;
		static const int kSignalPipe = 13;
		static const unsigned kSignalLimit = 65;
		static const int kChildStop = 255;

		struct Options {
			std::vector<std::string> command;
			bool delimited = false;
			char delimiter = '\n';
			bool have_eof = false;
			std::string eof;
			bool replacing = false;
			std::string replace;
			std::size_t max_args = 0;
			std::size_t max_lines = 0;
			std::size_t max_chars = kMaxCommandChars;
			std::size_t parallel = 1;
			bool no_run_if_empty = false;
			bool trace = false;
			bool prompt = false;
			bool exit_if_large = false;
			bool open_tty = false;
			bool have_arg_file = false;
			std::string arg_file;
		};

		struct Reader {
			FILE* stream = nullptr;
			int fd = -1;
			std::vector<char> buffer = std::vector<char>(kReadChunk);
			std::size_t pos = 0;
			std::size_t len = 0;
			bool eof = false;
			bool stdio_drained = false;
			int pushed = EOF;
		};

		enum class Read { Item, End, Error };

		struct Item {
			std::string text;
			bool line_end = false;
		};

		struct Batch {
			std::vector<std::string> args;
			std::size_t chars = 0;
			std::size_t windows_chars = 0;
			std::size_t lines = 0;
		};

		struct Runner {
			Executor& exec;
			const Options& options;
			int child_stdin = -1;
			std::vector<HANDLE> running;
			bool any_failed = false;
			bool stopped = false;
			int stop_status = 0;
			bool ran = false;
		};

		enum OptionId {
			kNull, kArgFile, kDelimiter, kEofShort, kEof, kReplaceShort, kReplace, kMaxLinesShort,
			kMaxLines, kMaxArgs, kOpenTty, kMaxProcs, kInteractive, kNoRunIfEmpty, kMaxChars,
			kVerbose, kExit
		};

	}  // namespace xargs_detail

	using xargs_detail::Batch;
	using xargs_detail::Item;
	using xargs_detail::Reader;
	using xargs_detail::Runner;
	using XargsOptions = xargs_detail::Options;
	using XargsRead = xargs_detail::Read;

	static bool xargsFail(const std::string& message) {
		perr("xargs", message);
		return false;
	}

	static const std::vector<OptDef> kXargsOptionDefs = {
		{ xargs_detail::kNull,          '0', "null",            OptArg::None     },
		{ xargs_detail::kArgFile,       'a', "arg-file",        OptArg::Required },
		{ xargs_detail::kDelimiter,     'd', "delimiter",       OptArg::Required },
		{ xargs_detail::kEofShort,      'E', nullptr,           OptArg::Required },
		{ xargs_detail::kEof,           'e', "eof",             OptArg::Optional },
		{ xargs_detail::kReplaceShort,  'I', nullptr,           OptArg::Required },
		{ xargs_detail::kReplace,       'i', "replace",         OptArg::Optional },
		{ xargs_detail::kMaxLinesShort, 'L', nullptr,           OptArg::Required },
		{ xargs_detail::kMaxLines,      'l', "max-lines",       OptArg::Optional },
		{ xargs_detail::kMaxArgs,       'n', "max-args",        OptArg::Required },
		{ xargs_detail::kOpenTty,       'o', "open-tty",        OptArg::None     },
		{ xargs_detail::kMaxProcs,      'P', "max-procs",       OptArg::Required },
		{ xargs_detail::kInteractive,   'p', "interactive",     OptArg::None     },
		{ xargs_detail::kNoRunIfEmpty,  'r', "no-run-if-empty", OptArg::None     },
		{ xargs_detail::kMaxChars,      's', "max-chars",       OptArg::Required },
		{ xargs_detail::kVerbose,       't', "verbose",         OptArg::None     },
		{ xargs_detail::kExit,          'x', "exit",            OptArg::None     },
	};

	static bool parseXargsCount(const std::string& value, char option, std::size_t minimum,
			std::size_t& out) {
		long long number = 0;
		std::size_t consumed = 0;
		if (value.empty() || !parseLL(value, number, 10, &consumed) || consumed != value.size()) {
			return xargsFail("invalid number \"" + value + "\" for -" + option + " option");
		}

		if (number < static_cast<long long>(minimum)) {
			return xargsFail("value " + value + " for -" + option + " option should be >= "
				+ std::to_string(minimum));
		}

		out = static_cast<std::size_t>(number);
		return true;
	}

	static bool parseXargsMaxChars(const std::string& value, std::size_t& out) {
		if (!parseXargsCount(value, 's', 1, out)) return false;
		if (out <= xargs_detail::kMaxCommandChars) return true;

		perr("xargs", "value " + value + " for -s option should be <= "
			+ std::to_string(xargs_detail::kMaxCommandChars));
		out = xargs_detail::kMaxCommandChars;
		return true;
	}

	static char xargsEscapeLetter(char letter) {
		switch (letter) {
		case 'a':
			return '\a';
		case 'b':
			return '\b';
		case 'f':
			return '\f';
		case 'n':
			return '\n';
		case 'r':
			return '\r';
		case 't':
			return '\t';
		case 'v':
			return '\v';
		case '\\':
			return '\\';
		default:
			return 0;
		}
	}

	static bool parseXargsEscapedDelimiter(const std::string& spec, char& out) {
		const std::string body = spec.substr(1);
		if (body.size() == 1 && xargsEscapeLetter(body[0]) != 0) {
			out = xargsEscapeLetter(body[0]);
			return true;
		}

		const bool hex = body[0] == 'x';
		const std::string digits = hex ? body.substr(1) : body;
		unsigned long value = 0;
		std::size_t consumed = 0;
		const bool ok = !digits.empty() && parseUL(digits, value, hex ? 16 : 8, &consumed)
			&& consumed == digits.size() && value <= 0xFF;
		out = static_cast<char>(value);
		return ok;
	}

	static bool parseXargsDelimiter(const std::string& spec, char& out) {
		const bool single = spec.size() == 1;
		const bool escaped = spec.size() > 1 && spec[0] == '\\'
			&& parseXargsEscapedDelimiter(spec, out);
		if (single) out = spec[0];
		if (single || escaped) return true;

		return xargsFail("invalid input delimiter specification " + spec + ": the delimiter must be"
			" either a single character or an escape sequence starting with \\.");
	}

	static void setXargsReplace(XargsOptions& options, const std::string& value) {
		options.replacing = true;
		options.replace = value;
	}

	static void setXargsEof(XargsOptions& options, const OptHit& hit) {
		options.have_eof = hit.has_value && !hit.value.empty();
		options.eof = hit.value;
	}

	static bool applyXargsValueHit(const OptHit& hit, XargsOptions& options) {
		switch (hit.id) {
		case xargs_detail::kArgFile:
			options.have_arg_file = true;
			options.arg_file = hit.value;
			return true;
		case xargs_detail::kDelimiter:
			options.delimited = true;
			return parseXargsDelimiter(hit.value, options.delimiter);
		case xargs_detail::kMaxLinesShort:
			return parseXargsCount(hit.value, 'L', 1, options.max_lines);
		case xargs_detail::kMaxLines:
			if (!hit.has_value) options.max_lines = 1;
			return !hit.has_value || parseXargsCount(hit.value, 'l', 1, options.max_lines);
		case xargs_detail::kMaxArgs:
			return parseXargsCount(hit.value, 'n', 1, options.max_args);
		case xargs_detail::kMaxProcs:
			return parseXargsCount(hit.value, 'P', 0, options.parallel);
		case xargs_detail::kMaxChars:
			return parseXargsMaxChars(hit.value, options.max_chars);
		default:
			return false;
		}
	}

	static bool applyXargsHit(const OptHit& hit, XargsOptions& options) {
		switch (hit.id) {
		case xargs_detail::kNull:
			options.delimited = true;
			options.delimiter = '\0';
			return true;
		case xargs_detail::kEofShort:
		case xargs_detail::kEof:
			setXargsEof(options, hit);
			return true;
		case xargs_detail::kReplaceShort:
			setXargsReplace(options, hit.value);
			return true;
		case xargs_detail::kReplace:
			setXargsReplace(options, hit.has_value ? hit.value : "{}");
			return true;
		case xargs_detail::kOpenTty:
			options.open_tty = true;
			return true;
		case xargs_detail::kInteractive:
			options.prompt = true;
			return true;
		case xargs_detail::kNoRunIfEmpty:
			options.no_run_if_empty = true;
			return true;
		case xargs_detail::kVerbose:
			options.trace = true;
			return true;
		case xargs_detail::kExit:
			options.exit_if_large = true;
			return true;
		default:
			return applyXargsValueHit(hit, options);
		}
	}

	static bool parseXargsArgs(const std::vector<std::string>& args, XargsOptions& options) {
		OptScanResult scan;
		if (!scanOptions("xargs", args, kXargsOptionDefs, scan, true)) return false;

		for (const OptHit& hit : scan.hits) {
			if (!applyXargsHit(hit, options)) return false;
		}

		options.command = scan.operands;
		if (options.command.empty()) options.command.push_back("echo");
		if (options.parallel == 0 || options.parallel > xargs_detail::kMaxParallel) {
			options.parallel = xargs_detail::kMaxParallel;
		}

		return true;
	}

	// Bytes stdio already buffered (say, after a `read` builtin) are taken
	// first; after that the descriptor is read directly, so a child that
	// inherits stdin never sees input xargs has not consumed yet.
	static std::size_t stdioBufferedBytes(FILE* stream) {
		char** base = nullptr;
		char** ptr = nullptr;
		int* count = nullptr;
		if (_get_stream_buffer_pointers(stream, &base, &ptr, &count) != 0 || count == nullptr) {
			return 0;
		}

		return *count > 0 ? static_cast<std::size_t>(*count) : 0;
	}

	static int readXargsByte(Reader& reader) {
		if (reader.pushed != EOF) {
			const int c = reader.pushed;
			reader.pushed = EOF;
			return c;
		}

		if (!reader.stdio_drained) {
			if (stdioBufferedBytes(reader.stream) > 0) return std::fgetc(reader.stream);
			reader.stdio_drained = true;
		}

		if (reader.eof) return EOF;

		if (reader.pos == reader.len) {
			const int got = _read(reader.fd, reader.buffer.data(),
				static_cast<unsigned>(reader.buffer.size()));
			if (got <= 0) {
				reader.eof = true;
				return EOF;
			}

			reader.pos = 0;
			reader.len = static_cast<std::size_t>(got);
		}

		return static_cast<unsigned char>(reader.buffer[reader.pos++]);
	}

	static bool isXargsSpace(int c) {
		return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
	}

	static bool isXargsBlank(int c) {
		return c == ' ' || c == '\t';
	}

	static bool readXargsQuoted(Reader& reader, int quote, std::string& text) {
		for (;;) {
			const int c = readXargsByte(reader);
			if (c == quote) return true;
			if (c == EOF || c == '\n') {
				return xargsFail(std::string("unmatched ") + (quote == '\'' ? "single" : "double")
					+ " quote; by default quotes are special to xargs"
					" unless you use the -0 option");
			}

			text.push_back(static_cast<char>(c));
		}
	}

	static int skipXargsSeparators(Reader& reader) {
		int c = readXargsByte(reader);
		while (c != EOF && isXargsSpace(c)) c = readXargsByte(reader);
		return c;
	}

	static bool appendXargsChar(Reader& reader, int c, Item& item) {
		if (c == '\'' || c == '"') return readXargsQuoted(reader, c, item.text);

		if (c == '\\') {
			const int escaped = readXargsByte(reader);
			if (escaped != EOF) item.text.push_back(static_cast<char>(escaped));
			return true;
		}

		item.text.push_back(static_cast<char>(c));
		return true;
	}

	// A newline right after trailing blanks continues the logical line for
	// -L, as in GNU.
	static void noteXargsItemEnd(Reader& reader, int terminator, Item& item) {
		if (terminator == EOF || terminator == '\n') {
			item.line_end = true;
			return;
		}

		int c = readXargsByte(reader);
		while (isXargsBlank(c)) c = readXargsByte(reader);
		if (c != '\n') reader.pushed = c;
	}

	static XargsRead readXargsWord(Reader& reader, const XargsOptions& options, Item& item) {
		int c = skipXargsSeparators(reader);
		if (c == EOF) return XargsRead::End;

		for (;;) {
			if (c == EOF || c == '\n') break;
			if (isXargsSpace(c) && !options.replacing) break;
			if (!appendXargsChar(reader, c, item)) return XargsRead::Error;
			c = readXargsByte(reader);
		}

		if (!options.replacing) noteXargsItemEnd(reader, c, item);
		else item.line_end = true;

		if (options.have_eof && item.text == options.eof) return XargsRead::End;
		return XargsRead::Item;
	}

	static XargsRead readXargsDelimited(Reader& reader, const XargsOptions& options, Item& item) {
		int c = readXargsByte(reader);
		if (c == EOF) return XargsRead::End;

		while (c != EOF && c != static_cast<unsigned char>(options.delimiter)) {
			item.text.push_back(static_cast<char>(c));
			c = readXargsByte(reader);
		}

		item.line_end = true;
		return XargsRead::Item;
	}

	static XargsRead readXargsItem(Reader& reader, const XargsOptions& options, Item& item) {
		item.text.clear();
		item.line_end = false;
		if (options.delimited) return readXargsDelimited(reader, options, item);
		return readXargsWord(reader, options, item);
	}

	static std::size_t xargsCost(const std::string& arg) {
		return arg.size() + 1;
	}

	static std::size_t xargsWindowsCost(const std::string& arg) {
		std::size_t cost = arg.size() + 1;
		if (arg.empty() || arg.find_first_of(" \t") != std::string::npos) cost += 2;
		for (char c : arg) {
			if (c == '"' || c == '\\') ++cost;
		}

		return cost;
	}

	static void addToBatch(Batch& batch, const std::string& arg) {
		batch.chars += xargsCost(arg);
		batch.windows_chars += xargsWindowsCost(arg);
		batch.args.push_back(arg);
	}

	static bool batchFits(const Batch& batch, const std::string& arg, const XargsOptions& options) {
		return batch.chars + xargsCost(arg) <= options.max_chars
			&& batch.windows_chars + xargsWindowsCost(arg) <= xargs_detail::kMaxCommandChars;
	}

	static std::string joinXargsArgv(const std::vector<std::string>& argv) {
		std::string line;
		for (const std::string& arg : argv) {
			if (!line.empty()) line.push_back(' ');
			line += arg;
		}

		return line;
	}

	static bool confirmXargsCommand(const std::vector<std::string>& argv) {
		std::fprintf(stderr, "%s ?...", joinXargsArgv(argv).c_str());
		std::fflush(stderr);

		FILE* tty = std::fopen("CONIN$", "r");
		if (tty == nullptr) return xargsFail("failed to open /dev/tty for reading");

		char answer[256] = {};
		const bool got = std::fgets(answer, sizeof(answer), tty) != nullptr;
		std::fclose(tty);
		return got && (answer[0] == 'y' || answer[0] == 'Y');
	}

	static bool announceXargsCommand(const Runner& runner, const std::vector<std::string>& argv) {
		if (runner.options.prompt) return confirmXargsCommand(argv);

		if (runner.options.trace) {
			std::fprintf(stderr, "%s\n", joinXargsArgv(argv).c_str());
			std::fflush(stderr);
		}

		return true;
	}

	static void stopForXargsSignal(Runner& runner, const std::string& name, int signal) {
		perr("xargs", name + ": terminated by signal " + std::to_string(signal));
		runner.stopped = true;
		runner.stop_status = xargs_detail::kStatusKilled;
	}

	// An in-process builtin whose output pipe closed would have died of
	// SIGPIPE on POSIX; its failed stdout stands in for that.
	static void stopIfXargsOutputGone(Runner& runner, const std::string& name) {
		if (runner.stopped || std::ferror(stdout) == 0) return;
		stopForXargsSignal(runner, name, xargs_detail::kSignalPipe);
	}

	// MSYS / Cygwin children killed by a signal exit with the signal number
	// in the second byte (SIGPIPE comes back as 3328).
	static int xargsChildSignal(int status) {
		const unsigned code = static_cast<unsigned>(status);
		const unsigned signal = code >> 8;
		if ((code & 0xFF) != 0 || signal == 0 || signal >= xargs_detail::kSignalLimit) return 0;
		return static_cast<int>(signal);
	}

	static void noteXargsStatus(Runner& runner, const std::string& name, int status) {
		if (runner.stopped) return;

		const int signal = xargsChildSignal(status);
		if (signal != 0) {
			stopForXargsSignal(runner, name, signal);
			return;
		}

		if (status == xargs_detail::kChildStop) {
			perr("xargs", name + ": exited with status 255; aborting");
			runner.stopped = true;
			runner.stop_status = xargs_detail::kStatusChild255;
			return;
		}

		if (status != 0) runner.any_failed = true;
	}

	static bool xargsCommandExists(Runner& runner, const std::string& name) {
		Executor& exec = runner.exec;
		if (exec.isBuiltin(name) || exec.isFunction(name) || !exec.findExecutable(name).empty()) {
			return true;
		}

		perr("xargs", name + ": No such file or directory");
		runner.stopped = true;
		runner.stop_status = xargs_detail::kStatusNotFound;
		return false;
	}

	static int invokeXargsInProcess(Executor& exec, const std::vector<std::string>& argv) {
		const std::vector<std::string> call_args(argv.begin() + 1, argv.end());
		if (exec.isBuiltin(argv[0])) return exec.callBuiltin(argv[0], call_args);
		if (exec.isFunction(argv[0])) return exec.callFunction(argv[0], call_args);
		return exec.executeText(quoteShellArgv(argv), "<xargs>");
	}

	// The child gets NUL (or the console with -o) as its stdin unless the
	// items come from -a FILE, so it cannot eat input meant for xargs.
	template <typename Fn>
	static auto withXargsChildStdin(const Runner& runner, Fn fn) -> decltype(fn()) {
		if (runner.child_stdin < 0) return fn();

		std::fflush(stdout);
		const int saved = _dup(0);
		_dup2(runner.child_stdin, 0);
		auto result = fn();
		_dup2(saved, 0);
		_close(saved);
		std::clearerr(stdin);
		return result;
	}

	static void waitForXargsChild(Runner& runner) {
		const DWORD count = static_cast<DWORD>(runner.running.size());
		const DWORD waited =
			::WaitForMultipleObjects(count, runner.running.data(), FALSE, INFINITE);
		const std::size_t index = waited >= WAIT_OBJECT_0 && waited < WAIT_OBJECT_0 + count
			? waited - WAIT_OBJECT_0
			: 0;

		DWORD exit_code = 0;
		::GetExitCodeProcess(runner.running[index], &exit_code);
		::CloseHandle(runner.running[index]);
		runner.running.erase(runner.running.begin() + static_cast<std::ptrdiff_t>(index));
		noteXargsStatus(runner, runner.options.command[0], static_cast<int>(exit_code));
	}

	static void waitForAllXargsChildren(Runner& runner) {
		while (!runner.running.empty()) waitForXargsChild(runner);
	}

	static void spawnXargsChild(Runner& runner, const std::vector<std::string>& argv) {
		while (runner.running.size() >= runner.options.parallel) waitForXargsChild(runner);

		const HANDLE child = withXargsChildStdin(runner,
			[&]() { return runner.exec.spawnCommand(argv); });
		if (child == INVALID_HANDLE_VALUE) {
			runner.stopped = true;
			runner.stop_status = xargs_detail::kStatusNotFound;
			return;
		}

		runner.running.push_back(child);
		stopIfXargsOutputGone(runner, argv[0]);
	}

	static void runXargsCommand(Runner& runner, const std::vector<std::string>& argv) {
		runner.ran = true;
		if (!xargsCommandExists(runner, argv[0])) return;
		if (!announceXargsCommand(runner, argv)) return;

		if (runner.options.parallel > 1) {
			spawnXargsChild(runner, argv);
			return;
		}

		const int status = withXargsChildStdin(runner,
			[&]() { return invokeXargsInProcess(runner.exec, argv); });
		std::fflush(stdout);
		noteXargsStatus(runner, argv[0], status);
		stopIfXargsOutputGone(runner, argv[0]);
	}

	static Batch startXargsBatch(const XargsOptions& options) {
		Batch batch;
		for (const std::string& word : options.command) addToBatch(batch, word);
		batch.args.clear();
		return batch;
	}

	static void flushXargsBatch(Runner& runner, Batch& batch) {
		std::vector<std::string> argv = runner.options.command;
		argv.insert(argv.end(), batch.args.begin(), batch.args.end());
		runXargsCommand(runner, argv);
		batch = startXargsBatch(runner.options);
	}

	static std::string xargsSubstitute(const std::string& word, const std::string& pattern,
			const std::string& item) {
		if (pattern.empty()) return word;

		std::string out;
		std::size_t pos = 0;
		for (std::size_t hit = word.find(pattern); hit != std::string::npos;
				hit = word.find(pattern, pos)) {
			out.append(word, pos, hit - pos);
			out += item;
			pos = hit + pattern.size();
		}

		out.append(word, pos, std::string::npos);
		return out;
	}

	static void runXargsReplacing(Runner& runner, const std::string& item) {
		std::vector<std::string> argv;
		for (const std::string& word : runner.options.command) {
			argv.push_back(xargsSubstitute(word, runner.options.replace, item));
		}

		runXargsCommand(runner, argv);
	}

	static bool batchIsFull(const Batch& batch, const XargsOptions& options) {
		if (options.max_args != 0 && batch.args.size() >= options.max_args) return true;
		return options.max_lines != 0 && batch.lines >= options.max_lines;
	}

	static bool makeRoomForItem(Runner& runner, Batch& batch, const std::string& item) {
		const XargsOptions& options = runner.options;
		if (batchFits(batch, item, options)) return true;

		const bool counted = options.max_args != 0 || options.max_lines != 0;
		if (batch.args.empty() || (options.exit_if_large && counted)) {
			return xargsFail(batch.args.empty()
				? "argument line too long"
				: "argument list too long");
		}

		flushXargsBatch(runner, batch);
		return batchFits(batch, item, options) || xargsFail("argument line too long");
	}

	static bool addXargsItem(Runner& runner, Batch& batch, const Item& item) {
		if (runner.options.replacing) {
			runXargsReplacing(runner, item.text);
			return true;
		}

		if (!makeRoomForItem(runner, batch, item.text)) return false;

		addToBatch(batch, item.text);
		if (item.line_end) ++batch.lines;
		if (batchIsFull(batch, runner.options)) flushXargsBatch(runner, batch);
		return true;
	}

	static bool consumeXargsInput(Runner& runner, Reader& reader) {
		Batch batch = startXargsBatch(runner.options);
		Item item;
		for (;;) {
			if (runner.stopped) return true;

			const XargsRead read = readXargsItem(reader, runner.options, item);
			if (read == XargsRead::Error) return false;
			if (read == XargsRead::End) break;
			if (!addXargsItem(runner, batch, item)) return false;
		}

		const bool run_empty = !runner.ran && !runner.options.no_run_if_empty
			&& !runner.options.replacing;
		if (!batch.args.empty() || run_empty) flushXargsBatch(runner, batch);
		return true;
	}

	static int openXargsChildStdin(const XargsOptions& options) {
		if (options.open_tty) return _open("CONIN$", _O_RDONLY | _O_BINARY);
		if (options.have_arg_file) return -1;
		return _open("NUL", _O_RDONLY | _O_BINARY);
	}

	static bool openXargsReader(Executor& exec, const XargsOptions& options, Reader& reader) {
		reader.stream = stdin;
		if (options.have_arg_file && options.arg_file != "-") {
			reader.stream = fopenNative(exec, options.arg_file, "rb");
			if (reader.stream == nullptr) {
				return xargsFail("cannot open input file '" + options.arg_file + "': "
					+ std::strerror(errno));
			}
		}

		reader.fd = _fileno(reader.stream);
		return true;
	}

	static int xargsExitStatus(const Runner& runner, bool ok) {
		if (!ok) return xargs_detail::kStatusFailed;
		if (runner.stopped) return runner.stop_status;
		return runner.any_failed ? xargs_detail::kStatusChildFailed : 0;
	}

	static int builtin_xargs(Executor& exec, const std::vector<std::string>& args) {
		XargsOptions options;
		if (!parseXargsArgs(args, options)) return xargs_detail::kStatusFailed;

		Reader reader;
		if (!openXargsReader(exec, options, reader)) return xargs_detail::kStatusFailed;

		Runner runner{ exec, options };
		runner.child_stdin = openXargsChildStdin(options);

		const bool ok = consumeXargsInput(runner, reader);
		waitForAllXargsChildren(runner);

		if (runner.child_stdin >= 0) _close(runner.child_stdin);
		if (reader.stream != stdin) std::fclose(reader.stream);
		std::fflush(stdout);
		return xargsExitStatus(runner, ok);
	}

	void registerXargsBuiltin(Executor& exec) {
		exec.registerBuiltin("xargs", builtin_xargs);
	}

}  // namespace wbsh

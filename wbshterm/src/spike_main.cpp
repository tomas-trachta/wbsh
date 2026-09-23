/**
 * @file spike_main.cpp
 * @brief M0 spike: host wbsh on a pseudoconsole and relay bytes both ways.
 *
 * Interactive mode passes this process's own console straight through, so
 * the spike is a terminal only in the sense that it proves the plumbing.
 * `--feed` drives the same session non-interactively and reports
 * throughput, which is the ConPTY risk the spike exists to measure.
 */

#include "pty.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace wbshterm {

	struct SpikeOptions {
		std::wstring shell_path;
		std::wstring shell_args = L"-i";
		std::string  feed;
		DWORD        timeout_ms = 10000;
		DWORD        delay_ms   = 250;
		bool         echo       = true;
	};

	struct FeedStats {
		unsigned long long bytes      = 0;
		double             elapsed_ms = 0.0;
		bool               alt_screen = false;
	};

	static const DWORD kReadChunk = 16384;

	static std::string narrow(const std::wstring& text) {
		if (text.empty()) return std::string();

		const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
			static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
		std::string out(static_cast<std::size_t>(needed), '\0');
		::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
			out.data(), needed, nullptr, nullptr);
		return out;
	}

	static std::string unescape(const std::string& text) {
		std::string out;
		for (std::size_t i = 0; i < text.size(); ++i) {
			if (text[i] != '\\' || i + 1 == text.size()) {
				out.push_back(text[i]);
				continue;
			}

			switch (text[++i]) {
			case 'n':  out.push_back('\n'); break;
			case 'r':  out.push_back('\r'); break;
			case 't':  out.push_back('\t'); break;
			case 'e':  out.push_back('\x1b'); break;
			case '\\': out.push_back('\\'); break;
			default:   out.push_back('\\'); out.push_back(text[i]); break;
			}
		}

		return out;
	}

	static void printUsage() {
		std::fprintf(stderr,
			"wbshterm-spike -- M0 ConPTY passthrough\n"
			"\n"
			"usage:\n"
			"  wbshterm-spike [--shell <path>]          interactive passthrough\n"
			"  wbshterm-spike --feed \"ls -la\\nexit\\n\"   scripted run + stats\n"
			"\n"
			"options:\n"
			"  --shell <path>    program to host (default: wbsh.exe found near this exe)\n"
			"  --args <text>     arguments for the hosted program (default: -i)\n"
			"  --feed <text>     write text to the shell, then drain its output\n"
			"                    (backslash n, r, t, e escapes are translated)\n"
			"  --delay <ms>      how long --feed waits before typing (default: 250)\n"
			"  --timeout <ms>    how long --feed waits for the shell to exit\n"
			"  --quiet           with --feed, suppress the raw output echo\n");
	}

	static bool parseArgs(int argc, wchar_t** argv, SpikeOptions& options) {
		for (int i = 1; i < argc; ++i) {
			const std::wstring arg = argv[i];
			const bool needs_value = (arg == L"--shell" || arg == L"--args" || arg == L"--feed"
				|| arg == L"--timeout" || arg == L"--delay");
			if (needs_value && i + 1 >= argc) return false;

			if (arg == L"--shell")   { options.shell_path = argv[++i]; continue; }
			if (arg == L"--feed")    { options.feed = unescape(narrow(argv[++i])); continue; }
			if (arg == L"--quiet")   { options.echo = false; continue; }
			if (arg == L"--args")    { options.shell_args = argv[++i]; continue; }
			if (arg == L"--timeout") { options.timeout_ms = std::stoul(argv[++i]); continue; }
			if (arg == L"--delay")   { options.delay_ms = std::stoul(argv[++i]); continue; }

			return false;
		}

		return true;
	}

	static std::wstring directoryOfThisExe() {
		wchar_t path[MAX_PATH] = {};
		const DWORD length = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
		const std::wstring full(path, length);

		const std::size_t cut = full.find_last_of(L'\\');
		return cut == std::wstring::npos ? std::wstring() : full.substr(0, cut + 1);
	}

	static bool fileExists(const std::wstring& path) {
		const DWORD attributes = ::GetFileAttributesW(path.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES
			&& (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
	}

	static std::wstring locateShell() {
		const std::wstring here = directoryOfThisExe();
		const std::wstring candidates[] = {
			here + L"wbsh.exe",
			here + L"..\\..\\x64\\Release\\wbsh.exe",
			here + L"..\\..\\x64\\Debug\\wbsh.exe",
		};

		for (const std::wstring& candidate : candidates) {
			if (fileExists(candidate)) return candidate;
		}

		wchar_t found[MAX_PATH] = {};
		if (::SearchPathW(nullptr, L"wbsh.exe", nullptr, MAX_PATH, found, nullptr) != 0) {
			return found;
		}

		return std::wstring();
	}

	static PtySize currentConsoleSize() {
		CONSOLE_SCREEN_BUFFER_INFO info{};
		PtySize size;
		if (!::GetConsoleScreenBufferInfo(::GetStdHandle(STD_OUTPUT_HANDLE), &info)) return size;

		size.columns = static_cast<SHORT>(info.srWindow.Right - info.srWindow.Left + 1);
		size.rows    = static_cast<SHORT>(info.srWindow.Bottom - info.srWindow.Top + 1);
		return size;
	}

	static void writeToStdout(const char* data, DWORD length) {
		DWORD written = 0;
		::WriteFile(::GetStdHandle(STD_OUTPUT_HANDLE), data, length, &written, nullptr);
	}

	/**
	 * @brief Puts this process's console into raw VT mode for the child.
	 *
	 * Restores the entry modes on destruction; restore() is idempotent so
	 * the caller can put the console back before a hard exit.
	 */
	class ConsoleModeGuard {
	public:
		~ConsoleModeGuard() { restore(); }

		bool enterRawMode(std::string& out_error);
		void restore();

	private:
		HANDLE input_             = INVALID_HANDLE_VALUE;
		HANDLE output_            = INVALID_HANDLE_VALUE;
		DWORD  saved_input_mode_  = 0;
		DWORD  saved_output_mode_ = 0;
		bool   saved_             = false;
	};

	bool ConsoleModeGuard::enterRawMode(std::string& out_error) {
		input_  = ::GetStdHandle(STD_INPUT_HANDLE);
		output_ = ::GetStdHandle(STD_OUTPUT_HANDLE);

		if (!::GetConsoleMode(input_, &saved_input_mode_)
			|| !::GetConsoleMode(output_, &saved_output_mode_)) {
			out_error = "not attached to a console (use --feed for scripted runs)";
			return false;
		}

		saved_ = true;
		::SetConsoleOutputCP(CP_UTF8);
		::SetConsoleCP(CP_UTF8);

		const DWORD output_mode = saved_output_mode_ | ENABLE_PROCESSED_OUTPUT
			| ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
		const DWORD input_mode = ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_EXTENDED_FLAGS;

		if (!::SetConsoleMode(output_, output_mode) || !::SetConsoleMode(input_, input_mode)) {
			out_error = "SetConsoleMode failed";
			return false;
		}

		return true;
	}

	void ConsoleModeGuard::restore() {
		if (!saved_) return;

		::SetConsoleMode(input_, saved_input_mode_);
		::SetConsoleMode(output_, saved_output_mode_);
		saved_ = false;
	}

	static void pumpPtyToStdout(PtySession& session) {
		std::vector<char> buffer(kReadChunk);
		for (;;) {
			const DWORD got = session.read(buffer.data(), kReadChunk);
			if (got == 0) return;
			writeToStdout(buffer.data(), got);
		}
	}

	// The console handle is signalled by focus and menu records too, which
	// ReadFile then discards; a spurious wake just blocks until the next
	// keypress, which is why teardown also cancels the pending read.
	static void pumpConsoleToPty(PtySession& session, std::atomic<bool>& stop) {
		const HANDLE input = ::GetStdHandle(STD_INPUT_HANDLE);
		std::vector<char> buffer(4096);

		while (!stop.load()) {
			if (::WaitForSingleObject(input, 100) != WAIT_OBJECT_0) continue;

			DWORD got = 0;
			if (!::ReadFile(input, buffer.data(), 4096, &got, nullptr) || got == 0) return;
			if (!session.write(buffer.data(), got)) return;
		}
	}

	static void watchConsoleResize(PtySession& session, std::atomic<bool>& stop) {
		PtySize last = currentConsoleSize();

		while (!stop.load()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(150));

			const PtySize now = currentConsoleSize();
			if (now.columns == last.columns && now.rows == last.rows) continue;

			session.resize(now);
			last = now;
		}
	}

	static int runInteractive(PtySession& session) {
		ConsoleModeGuard console;
		std::string error;
		if (!console.enterRawMode(error)) {
			std::fprintf(stderr, "wbshterm-spike: %s\n", error.c_str());
			return 1;
		}

		std::atomic<bool> stop{false};
		std::thread reader(pumpPtyToStdout, std::ref(session));
		std::thread resizer(watchConsoleResize, std::ref(session), std::ref(stop));
		std::thread input(pumpConsoleToPty, std::ref(session), std::ref(stop));

		DWORD exit_code = 0;
		session.waitForExit(INFINITE, exit_code);
		stop.store(true);

		session.endSession();
		reader.join();
		resizer.join();

		::CancelIoEx(::GetStdHandle(STD_INPUT_HANDLE), nullptr);
		input.join();

		console.restore();
		return static_cast<int>(exit_code);
	}

	static void drainFeedOutput(PtySession& session, const SpikeOptions& options,
			FeedStats& stats) {
		std::vector<char> buffer(kReadChunk);
		std::string carry;

		for (;;) {
			const DWORD got = session.read(buffer.data(), kReadChunk);
			if (got == 0) return;

			stats.bytes += got;
			if (options.echo) writeToStdout(buffer.data(), got);

			carry.append(buffer.data(), got);
			if (carry.find("\x1b[?1049h") != std::string::npos) stats.alt_screen = true;
			if (carry.size() > 64) carry.erase(0, carry.size() - 64);
		}
	}

	static void reportFeedStats(const FeedStats& stats, DWORD exit_code, bool exited) {
		const double seconds = stats.elapsed_ms / 1000.0;
		const double mb_per_second = seconds > 0.0
			? static_cast<double>(stats.bytes) / seconds / (1024.0 * 1024.0)
			: 0.0;

		std::fprintf(stderr,
			"\nwbshterm-spike: %llu bytes in %.0f ms (%.1f MB/s), alt-screen %s, %s\n",
			stats.bytes, stats.elapsed_ms, mb_per_second,
			stats.alt_screen ? "yes" : "no",
			exited ? "shell exited" : "shell still running at timeout");

		if (exited) std::fprintf(stderr, "wbshterm-spike: shell exit code %lu\n", exit_code);
	}

	static int runFeed(PtySession& session, const SpikeOptions& options) {
		FeedStats stats;
		std::thread reader(drainFeedOutput, std::ref(session), std::cref(options), std::ref(stats));

		std::this_thread::sleep_for(std::chrono::milliseconds(options.delay_ms));

		const auto started = std::chrono::steady_clock::now();

		if (!session.write(options.feed.data(), static_cast<DWORD>(options.feed.size()))) {
			std::fprintf(stderr, "wbshterm-spike: writing to the shell failed\n");
		}

		DWORD exit_code = 0;
		const bool exited = session.waitForExit(options.timeout_ms, exit_code);

		session.endSession();
		reader.join();

		const auto finished = std::chrono::steady_clock::now();
		stats.elapsed_ms = std::chrono::duration<double, std::milli>(finished - started).count();

		reportFeedStats(stats, exit_code, exited);
		return (stats.bytes > 0 && exited) ? 0 : 1;
	}

} /* namespace wbshterm */

int wmain(int argc, wchar_t** argv) {
	wbshterm::SpikeOptions options;
	if (!wbshterm::parseArgs(argc, argv, options)) {
		wbshterm::printUsage();
		return 2;
	}

	if (options.shell_path.empty()) options.shell_path = wbshterm::locateShell();
	if (options.shell_path.empty()) {
		std::fprintf(stderr, "wbshterm-spike: cannot find wbsh.exe (pass --shell <path>)\n");
		return 2;
	}

	wbshterm::PtySession session;
	std::string error;
	std::wstring command_line = L"\"" + options.shell_path + L"\"";
	if (!options.shell_args.empty()) command_line += L" " + options.shell_args;
	if (!session.open(command_line, wbshterm::currentConsoleSize(), error)) {
		std::fprintf(stderr, "wbshterm-spike: %s\n", error.c_str());
		return 1;
	}

	return options.feed.empty()
		? wbshterm::runInteractive(session)
		: wbshterm::runFeed(session, options);
}

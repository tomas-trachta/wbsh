/**
 * @file main.cpp
 * @brief Entry point: locate the shell, then window, snapshot or selftest.
 */

#include "replay.h"
#include "selftest.h"
#include "snapshot.h"
#include "window.h"

#include <shellapi.h>

#include <cctype>
#include <objbase.h>
#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace wbshterm {

	enum class Mode {
		Window,
		Snapshot,
		SelfTest,
		Replay,
	};

	struct Options {
		Mode         mode = Mode::Window;
		std::wstring shell_path;
		std::wstring shell_args = L"-i";
		std::wstring output_path;
		std::wstring record_path;
		std::wstring replay_path;
		std::wstring dump_path;
		std::string  feed;
		int          columns    = 100;
		int          rows       = 30;
		unsigned int delay_ms   = 400;
		unsigned int settle_ms  = 600;
		int          scroll_lines = 0;
		std::wstring selection;
	};

	static std::string narrow(const std::wstring& text) {
		if (text.empty()) return std::string();

		const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
			static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
		std::string out(static_cast<std::size_t>(needed), '\0');
		::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
			out.data(), needed, nullptr, nullptr);
		return out;
	}

	// Reads the two hex digits after a \x, advancing past them. Anything
	// shorter is taken literally, so a lone \x is not a parse error.
	static char readHexByte(const std::string& text, std::size_t& index) {
		int value = 0;
		int digits = 0;

		while (digits < 2 && index + 1 < text.size() && std::isxdigit(
				static_cast<unsigned char>(text[index + 1])) != 0) {
			const char digit = text[++index];
			const int part = digit <= '9' ? digit - '0' : (std::tolower(digit) - 'a' + 10);
			value = value * 16 + part;
			++digits;
		}

		return digits == 0 ? 'x' : static_cast<char>(value);
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
			case 'x':  out.push_back(readHexByte(text, i)); break;
			default:   out.push_back('\\'); out.push_back(text[i]); break;
			}
		}

		return out;
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

	static bool parseArgs(int argc, wchar_t** argv, Options& options) {
		for (int i = 1; i < argc; ++i) {
			const std::wstring arg = argv[i];
			const bool needs_value = arg == L"--shell" || arg == L"--args" || arg == L"--feed"
				|| arg == L"--snapshot" || arg == L"--selftest" || arg == L"--size"
				|| arg == L"--record" || arg == L"--replay" || arg == L"--dump"
				|| arg == L"--scroll" || arg == L"--select"
				|| arg == L"--delay" || arg == L"--settle";
			if (needs_value && i + 1 >= argc) return false;

			if (arg == L"--shell")    { options.shell_path = argv[++i]; continue; }
			if (arg == L"--args")     { options.shell_args = argv[++i]; continue; }
			if (arg == L"--feed")     { options.feed = unescape(narrow(argv[++i])); continue; }
			if (arg == L"--record")   { options.record_path = argv[++i]; continue; }
			if (arg == L"--dump")     { options.dump_path = argv[++i]; continue; }
			if (arg == L"--scroll")   { options.scroll_lines = std::stoi(argv[++i]); continue; }
			if (arg == L"--select")   { options.selection = argv[++i]; continue; }
			if (arg == L"--replay") {
				options.mode = Mode::Replay;
				options.replay_path = argv[++i];
				continue;
			}

			if (arg == L"--delay")    { options.delay_ms = std::stoul(argv[++i]); continue; }
			if (arg == L"--settle")   { options.settle_ms = std::stoul(argv[++i]); continue; }
			if (arg == L"--snapshot") {
				options.mode = Mode::Snapshot;
				options.output_path = argv[++i];
				continue;
			}

			if (arg == L"--selftest") {
				options.mode = Mode::SelfTest;
				options.output_path = argv[++i];
				continue;
			}

			if (arg == L"--size") {
				const std::wstring value = argv[++i];
				const std::size_t cross = value.find(L'x');
				if (cross == std::wstring::npos) return false;
				options.columns = std::stoi(value.substr(0, cross));
				options.rows    = std::stoi(value.substr(cross + 1));
				continue;
			}

			return false;
		}

		return true;
	}

	static void reportFailure(const std::string& message) {
		const int needed = ::MultiByteToWideChar(CP_UTF8, 0, message.c_str(),
			static_cast<int>(message.size()), nullptr, 0);
		std::wstring wide(static_cast<std::size_t>(needed), L'\0');
		::MultiByteToWideChar(CP_UTF8, 0, message.c_str(), static_cast<int>(message.size()),
			wide.data(), needed);

		::MessageBoxW(nullptr, wide.c_str(), L"wbshterm", MB_ICONERROR | MB_OK);
	}

	static std::wstring shellCommandLine(const Options& options) {
		std::wstring command_line = L"\"" + options.shell_path + L"\"";
		if (!options.shell_args.empty()) command_line += L" " + options.shell_args;
		return command_line;
	}

	// "row:col-row:col" in absolute rows, for showing a selection in a
	// snapshot without a mouse.
	static void applySelection(const std::wstring& text, SnapshotRequest& request) {
		if (text.empty()) return;

		int values[4] = { 0, 0, 0, 0 };
		std::size_t at = 0;
		for (int index = 0; index < 4 && at < text.size(); ++index) {
			std::size_t used = 0;
			values[index] = std::stoi(text.substr(at), &used);
			at += used + 1;
		}

		request.select        = true;
		request.select_row    = values[0];
		request.select_column = values[1];
		request.select_to_row = values[2];
		request.select_to_col = values[3];
	}

	static int runSnapshot(const Options& options) {
		SnapshotRequest request;
		request.command_line = shellCommandLine(options);
		request.output_path  = options.output_path;
		request.record_path  = options.record_path;
		request.feed         = options.feed;
		request.columns      = options.columns;
		request.rows         = options.rows;
		request.delay_ms     = options.delay_ms;
		request.settle_ms    = options.settle_ms;
		request.scroll_lines = options.scroll_lines;
		applySelection(options.selection, request);

		std::string error;
		if (renderSnapshot(request, error)) return 0;

		reportFailure(error);
		return 1;
	}

	static int runReplay(const Options& options) {
		ReplayRequest request;
		request.input_path = options.replay_path;
		request.text_path  = options.dump_path;
		request.image_path = options.output_path;
		request.columns    = options.columns;
		request.rows       = options.rows;

		std::string error;
		if (replayStream(request, error)) return 0;

		reportFailure(error);
		return 1;
	}

	static int runWindow(const Options& options) {
		TerminalWindow window;
		std::string error;
		if (!window.create(shellCommandLine(options), error)) {
			reportFailure(error);
			return 1;
		}

		return window.runMessageLoop();
	}

} /* namespace wbshterm */

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
	::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

	int argc = 0;
	wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);

	wbshterm::Options options;
	const bool parsed = argv != nullptr && wbshterm::parseArgs(argc, argv, options);
	if (argv != nullptr) ::LocalFree(argv);

	if (!parsed) {
		wbshterm::reportFailure("usage: wbshterm [--shell <path>] [--args <text>]\n"
			"       wbshterm --snapshot <png> [--feed <text>] [--size <cols>x<rows>]\n"
			"       wbshterm --selftest <report.txt>");
		return 2;
	}

	if (options.shell_path.empty()) options.shell_path = wbshterm::locateShell();
	if (options.shell_path.empty()) {
		wbshterm::reportFailure("cannot find wbsh.exe (pass --shell <path>)");
		return 2;
	}

	switch (options.mode) {
	case wbshterm::Mode::Replay:
		return wbshterm::runReplay(options);
	case wbshterm::Mode::Snapshot:
		return wbshterm::runSnapshot(options);
	case wbshterm::Mode::SelfTest:
		return wbshterm::runSelfTest(options.output_path,
			L"\"" + options.shell_path + L"\" " + options.shell_args) ? 0 : 1;
	case wbshterm::Mode::Window:
	default:
		return wbshterm::runWindow(options);
	}
}

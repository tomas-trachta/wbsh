/**
 * @file pty_relay.cpp
 * @brief The stdio <-> pseudoconsole relay behind script and unbuffer.
 */

#include "pty_relay.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <io.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "coreutils_internal.h"
#include "executor.h"
#include "exitstatus.h"
#include "interrupt.h"
#include "pathconv.h"
#include "pseudoconsole.h"
#include "stdinsync.h"

namespace wbsh {

	namespace pty_detail {

		static const DWORD kSliceMs = 100;
		static const DWORD kChunk = 16384;
		static const DWORD kRecordBatch = 128;

		// A console reads Ctrl+Z, Enter as the end of a cooked line read;
		// wbsh's own line editor and the readline-style programs take Ctrl+D.
		static const char kEndOfInput[] = "\x1a\r\x04";
		static const char kInterruptKey[] = "\x03";

		// The pseudoconsole asks its terminal for its attributes and waits
		// seconds for the answer, and asks for focus reports and its own
		// key encoding. The relay is that terminal: it answers as wbshterm
		// does and keeps the rest from the user's screen and the logs.
		static const char kDeviceAttributes[] = "\x1b[?1;2c";
		static const char* const kHandshakeSequences[] = {
			"c", "0c", "1t", "?1004h", "?1004l", "?9001h", "?9001l",
		};
		static const std::size_t kLongestPendingSequence = 32;

		enum class CsiScan { NotCsi, Incomplete, Complete };

		static const DWORD kRawVtInput = ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_WINDOW_INPUT;
		static const DWORD kVtOutput = ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT
			| ENABLE_VIRTUAL_TERMINAL_PROCESSING;

		struct OutputPump {
			PseudoConsole* console = nullptr;
			PtyLogs logs;
			std::string pending_sequence;
			std::atomic<bool> stdout_failed{ false };
		};

		struct InputPump {
			PseudoConsole* console = nullptr;
			HANDLE input = INVALID_HANDLE_VALUE;
			std::FILE* log = nullptr;
			bool flush_log = false;
			bool previous_was_cr = false;
			std::atomic<bool> stop{ false };
			std::atomic<bool> finished{ false };
		};

		struct ConsoleModes {
			HANDLE input = INVALID_HANDLE_VALUE;
			HANDLE output = INVALID_HANDLE_VALUE;
			DWORD input_mode = 0;
			DWORD output_mode = 0;
			UINT output_code_page = 0;
			bool input_changed = false;
			bool output_changed = false;
		};

		struct Relay {
			PseudoConsole console;
			PseudoConsoleSize size;
			HANDLE size_source = INVALID_HANDLE_VALUE;
			HANDLE child = INVALID_HANDLE_VALUE;
			ConsoleModes modes;
			bool reads_console = false;
			OutputPump output;
			InputPump input;
			std::thread output_thread;
			std::thread input_thread;
		};

	}  // namespace pty_detail

	using pty_detail::ConsoleModes;
	using pty_detail::CsiScan;
	using pty_detail::InputPump;
	using pty_detail::OutputPump;
	using pty_detail::Relay;
	using pty_detail::kChunk;
	using pty_detail::kSliceMs;

	static HANDLE stdioHandle(int fd) {
		return reinterpret_cast<HANDLE>(_get_osfhandle(fd));
	}

	static bool isConsoleHandle(HANDLE handle) {
		DWORD mode = 0;
		return handle != INVALID_HANDLE_VALUE && ::GetConsoleMode(handle, &mode) != 0;
	}

	static void writeLog(std::FILE* log, const char* data, std::size_t size, bool flush) {
		if (log == nullptr) return;

		std::fwrite(data, 1, size, log);
		if (flush) std::fflush(log);
	}

	static HANDLE openConsoleOutput() {
		return ::CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	}

	static bool readConsoleSize(HANDLE output, PseudoConsoleSize& size) {
		CONSOLE_SCREEN_BUFFER_INFO info{};
		if (output == INVALID_HANDLE_VALUE) return false;
		if (::GetConsoleScreenBufferInfo(output, &info) == 0) return false;

		size.columns = static_cast<SHORT>(info.srWindow.Right - info.srWindow.Left + 1);
		size.rows    = static_cast<SHORT>(info.srWindow.Bottom - info.srWindow.Top + 1);
		return size.columns > 0 && size.rows > 0;
	}

	static void followConsoleSize(Relay& relay) {
		PseudoConsoleSize size;
		if (!readConsoleSize(relay.size_source, size)) return;
		if (size.columns == relay.size.columns && size.rows == relay.size.rows) return;

		relay.size = size;
		relay.console.resize(size);
	}

	static CsiScan scanCsi(const std::string& text, std::size_t start, std::size_t& end) {
		if (start + 1 >= text.size()) return CsiScan::Incomplete;
		if (text[start + 1] != '[') return CsiScan::NotCsi;

		for (std::size_t i = start + 2; i < text.size(); ++i) {
			const unsigned char c = static_cast<unsigned char>(text[i]);
			if (c >= 0x40 && c <= 0x7e) {
				end = i + 1;
				return CsiScan::Complete;
			}

			if (c < 0x20 || c > 0x3f) return CsiScan::NotCsi;
		}

		return CsiScan::Incomplete;
	}

	static bool isHandshake(const std::string& body) {
		for (const char* sequence : pty_detail::kHandshakeSequences) {
			if (body == sequence) return true;
		}

		return false;
	}

	static bool takeHandshake(OutputPump& pump, const std::string& body) {
		if (!isHandshake(body)) return false;
		if (body != "c" && body != "0c") return true;

		const DWORD length = sizeof(pty_detail::kDeviceAttributes) - 1;
		pump.console->write(pty_detail::kDeviceAttributes, length);
		return true;
	}

	static bool keepsPending(const std::string& text, std::size_t start) {
		return text.size() - start < pty_detail::kLongestPendingSequence;
	}

	// Returns where the scan goes on after the escape at @p escape, or npos
	// once a sequence the chunk cuts short is held back for the next one.
	static std::size_t filterEscape(OutputPump& pump, const std::string& text, std::size_t escape,
	                                std::string& kept) {
		std::size_t end = escape + 1;
		const CsiScan scan = scanCsi(text, escape, end);
		if (scan == CsiScan::Incomplete && keepsPending(text, escape)) {
			pump.pending_sequence = text.substr(escape);
			return std::string::npos;
		}

		if (scan != CsiScan::Complete) {
			kept.push_back(text[escape]);
			return escape + 1;
		}

		const std::string body = text.substr(escape + 2, end - escape - 2);
		if (!takeHandshake(pump, body)) kept.append(text, escape, end - escape);
		return end;
	}

	static std::string withoutHandshakes(OutputPump& pump, const char* data, DWORD size) {
		const std::string text = pump.pending_sequence + std::string(data, size);
		pump.pending_sequence.clear();

		std::string kept;
		kept.reserve(text.size());
		std::size_t at = 0;
		while (at < text.size()) {
			const std::size_t escape = text.find('\x1b', at);
			if (escape == std::string::npos) {
				kept.append(text, at, std::string::npos);
				break;
			}

			kept.append(text, at, escape - at);
			at = filterEscape(pump, text, escape, kept);
		}

		return kept;
	}

	static void writeOutput(OutputPump& pump, const std::string& text) {
		if (text.empty()) return;

		writeLog(pump.logs.output, text.data(), text.size(), pump.logs.flush);
		if (pump.stdout_failed.load()) return;

		const bool written = std::fwrite(text.data(), 1, text.size(), stdout) == text.size();
		if (std::fflush(stdout) != 0 || !written) pump.stdout_failed.store(true);
	}

	static void runOutputPump(OutputPump& pump) {
		std::vector<char> buffer(kChunk);
		for (;;) {
			const DWORD got = pump.console->read(buffer.data(), kChunk);
			if (got == 0) break;

			writeOutput(pump, withoutHandshakes(pump, buffer.data(), got));
		}

		writeOutput(pump, pump.pending_sequence);
	}

	static std::string asConsoleKeys(InputPump& pump, const char* data, DWORD size) {
		std::string keys;
		keys.reserve(size);
		for (DWORD i = 0; i < size; ++i) {
			const char c = data[i];
			if (c != '\n') keys.push_back(c);
			else if (!pump.previous_was_cr) keys.push_back('\r');

			pump.previous_was_cr = c == '\r';
		}

		return keys;
	}

	static bool forwardInput(InputPump& pump, const char* data, DWORD size) {
		writeLog(pump.log, data, size, pump.flush_log);
		const std::string keys = asConsoleKeys(pump, data, size);
		return pump.console->write(keys.data(), static_cast<DWORD>(keys.size()));
	}

	static bool forwardCrtReadAhead(InputPump& pump) {
		const int buffered = stdinBufferedBytes();
		if (buffered <= 0) return true;

		std::string data(static_cast<std::size_t>(buffered), '\0');
		const std::size_t got = std::fread(data.data(), 1, data.size(), stdin);
		return forwardInput(pump, data.data(), static_cast<DWORD>(got));
	}

	static void runInputPump(InputPump& pump) {
		std::vector<char> buffer(kChunk);
		bool at_eof = false;
		bool flowing = forwardCrtReadAhead(pump);
		while (flowing && !pump.stop.load()) {
			DWORD got = 0;
			const BOOL ok = ::ReadFile(pump.input, buffer.data(), kChunk, &got, nullptr);
			at_eof = (ok != 0 && got == 0) || (ok == 0 && ::GetLastError() == ERROR_BROKEN_PIPE);
			flowing = ok != 0 && got > 0 && forwardInput(pump, buffer.data(), got);
		}

		if (at_eof && !pump.stop.load()) {
			pump.console->write(pty_detail::kEndOfInput, sizeof(pty_detail::kEndOfInput) - 1);
		}

		pump.finished.store(true);
	}

	// The thread may sit in a read that only its own cancellation can end.
	static void stopInputPump(Relay& relay) {
		if (!relay.input_thread.joinable()) return;

		relay.input.stop.store(true);
		while (!relay.input.finished.load()) {
			::CancelSynchronousIo(relay.input_thread.native_handle());
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}

		relay.input_thread.join();
	}

	static void appendKeyText(const KEY_EVENT_RECORD& key, std::wstring& text) {
		if (key.bKeyDown == 0 || key.uChar.UnicodeChar == 0) return;

		const WORD repeat = key.wRepeatCount == 0 ? 1 : key.wRepeatCount;
		text.append(repeat, key.uChar.UnicodeChar);
	}

	// In VT input mode the console has already turned each key into the
	// characters a terminal would send, so only their text is forwarded.
	static void forwardConsoleKeys(Relay& relay) {
		const DWORD capacity = pty_detail::kRecordBatch;
		INPUT_RECORD records[pty_detail::kRecordBatch];
		DWORD got = 0;
		if (::ReadConsoleInputW(relay.modes.input, records, capacity, &got) == 0) return;

		std::wstring text;
		for (DWORD i = 0; i < got; ++i) {
			if (records[i].EventType == KEY_EVENT) appendKeyText(records[i].Event.KeyEvent, text);
		}

		if (text.empty()) return;

		const std::string bytes = wideToUtf8(text);
		writeLog(relay.input.log, bytes.data(), bytes.size(), relay.input.flush_log);
		relay.console.write(bytes.data(), static_cast<DWORD>(bytes.size()));
	}

	static bool enterRawInput(ConsoleModes& modes) {
		modes.input = stdioHandle(0);
		if (!isConsoleHandle(modes.input)) return false;

		::GetConsoleMode(modes.input, &modes.input_mode);
		modes.input_changed = ::SetConsoleMode(modes.input, pty_detail::kRawVtInput) != 0;
		return modes.input_changed;
	}

	static void enterVtOutput(ConsoleModes& modes) {
		modes.output = stdioHandle(1);
		if (!isConsoleHandle(modes.output)) return;

		::GetConsoleMode(modes.output, &modes.output_mode);
		modes.output_code_page = ::GetConsoleOutputCP();
		::SetConsoleOutputCP(CP_UTF8);
		modes.output_changed = ::SetConsoleMode(modes.output,
			modes.output_mode | pty_detail::kVtOutput) != 0;
	}

	static void restoreConsoleModes(const ConsoleModes& modes) {
		if (modes.input_changed) ::SetConsoleMode(modes.input, modes.input_mode);
		if (modes.output_changed) ::SetConsoleMode(modes.output, modes.output_mode);
		if (modes.output_code_page != 0) ::SetConsoleOutputCP(modes.output_code_page);
	}

	static void startInput(Relay& relay, const PtyRun& run) {
		relay.input.console = &relay.console;
		relay.input.log = run.logs.input;
		relay.input.flush_log = run.logs.flush;
		if (!run.forward_input) return;

		relay.reads_console = enterRawInput(relay.modes);
		if (relay.reads_console) return;

		relay.input.input = stdioHandle(0);
		if (relay.input.input == INVALID_HANDLE_VALUE) return;

		relay.input_thread = std::thread(runInputPump, std::ref(relay.input));
	}

	static void startOutput(Relay& relay, const PtyRun& run) {
		enterVtOutput(relay.modes);
		relay.output.console = &relay.console;
		relay.output.logs = run.logs;
		relay.output_thread = std::thread(runOutputPump, std::ref(relay.output));
	}

	static bool openConsole(Relay& relay) {
		relay.size_source = openConsoleOutput();
		readConsoleSize(relay.size_source, relay.size);

		std::string error;
		if (relay.console.open(relay.size, error)) return true;

		std::fprintf(stderr, "wbsh: %s\n", error.c_str());
		return false;
	}

	static bool startChild(Executor& exec, Relay& relay, const std::vector<std::string>& argv) {
		SpawnPlacement placement;
		placement.pseudo_console = relay.console.handle();
		relay.child = exec.spawnCommand(argv, placement);
		return relay.child != INVALID_HANDLE_VALUE;
	}

	// Ctrl+C reaches the shell rather than the command whenever the keys
	// are not being read straight from the console.
	static void passOnCtrlC(Relay& relay) {
		if (!takeCtrlC()) return;
		relay.console.write(pty_detail::kInterruptKey, sizeof(pty_detail::kInterruptKey) - 1);
	}

	static bool relayStep(Relay& relay) {
		const HANDLE waits[2] = { relay.child, relay.modes.input };
		const DWORD count = relay.reads_console ? 2 : 1;
		const DWORD woke = ::WaitForMultipleObjects(count, waits, FALSE, kSliceMs);
		if (woke == WAIT_OBJECT_0 || woke == WAIT_FAILED) return false;
		if (woke == WAIT_OBJECT_0 + 1) forwardConsoleKeys(relay);

		passOnCtrlC(relay);
		followConsoleSize(relay);
		return !relay.output.stdout_failed.load();
	}

	static int childStatus(const Relay& relay) {
		if (relay.output.stdout_failed.load()) return stdoutFailureStatus();

		DWORD code = 0;
		::GetExitCodeProcess(relay.child, &code);
		return shellStatusFromExitCode(code);
	}

	// Closing the pseudoconsole flushes what it has yet to render, so the
	// output thread is only joined after it.
	static void finishRelay(Relay& relay) {
		stopInputPump(relay);
		relay.console.endSession();
		if (relay.output_thread.joinable()) relay.output_thread.join();

		restoreConsoleModes(relay.modes);
		relay.console.close();
		if (relay.child != INVALID_HANDLE_VALUE) ::CloseHandle(relay.child);
		if (relay.size_source != INVALID_HANDLE_VALUE) ::CloseHandle(relay.size_source);
	}

	bool runOnPseudoConsole(Executor& exec, const PtyRun& run, int& status) {
		const ScopedCtrlCCapture capture;
		Relay relay;
		if (!openConsole(relay) || !startChild(exec, relay, run.argv)) {
			finishRelay(relay);
			return false;
		}

		startOutput(relay, run);
		startInput(relay, run);
		bool relaying = true;
		while (relaying) relaying = relayStep(relay);

		status = childStatus(relay);
		finishRelay(relay);
		return true;
	}

	bool controllingConsoleSize(int& columns, int& rows) {
		const HANDLE output = openConsoleOutput();
		PseudoConsoleSize size;
		const bool known = readConsoleSize(output, size);
		if (output != INVALID_HANDLE_VALUE) ::CloseHandle(output);
		if (!known) return false;

		columns = size.columns;
		rows = size.rows;
		return true;
	}

}  // namespace wbsh

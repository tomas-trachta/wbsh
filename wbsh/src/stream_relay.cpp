/**
 * @file stream_relay.cpp
 * @brief The socket <-> descriptor copy behind nc, with half-close and the -q / -w timers.
 */

#include "stream_relay.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <io.h>
#endif /* _WIN32 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "interrupt.h"
#include "stdinsync.h"

namespace wbsh {

	namespace relay_detail {

		using Clock = std::chrono::steady_clock;

		static const int kSliceMs = 100;
		static const std::size_t kChunk = 16384;

		struct InputPump {
			NetSocket socket = kNoSocket;
			HANDLE input = INVALID_HANDLE_VALUE;
			bool from_stdin = false;
			bool console = false;
			bool udp = false;
			bool crlf = false;
			bool shutdown_on_eof = false;
			bool previous_was_cr = false;

			std::atomic<bool> stop{ false };
			std::atomic<bool> finished{ false };
			std::atomic<bool> reached_eof{ false };
			std::atomic<long long> finished_at_ms{ 0 };
			std::atomic<long long> last_activity_ms{ 0 };
		};

		static long long nowMs() {
			return std::chrono::duration_cast<std::chrono::milliseconds>(
				Clock::now().time_since_epoch()).count();
		}

		static bool isConsoleHandle(HANDLE handle) {
			DWORD mode = 0;
			return ::GetConsoleMode(handle, &mode) != 0;
		}

		static std::string withoutConsoleCr(const std::string& data) {
			std::string out;
			out.reserve(data.size());
			for (std::size_t i = 0; i < data.size(); ++i) {
				if (data[i] == '\r' && i + 1 < data.size() && data[i + 1] == '\n') continue;
				out.push_back(data[i]);
			}

			return out;
		}

		static std::string withCrlf(InputPump& pump, const std::string& data) {
			std::string out;
			out.reserve(data.size() + data.size() / 16);
			for (const char c : data) {
				if (c == '\n' && !pump.previous_was_cr) out.push_back('\r');
				out.push_back(c);
				pump.previous_was_cr = c == '\r';
			}

			return out;
		}

		static std::string translateInput(InputPump& pump, const std::string& data) {
			const std::string text = pump.console ? withoutConsoleCr(data) : data;
			return pump.crlf ? withCrlf(pump, text) : text;
		}

		static bool sendUnlessStopped(InputPump& pump, const std::string& data) {
			if (pump.udp) {
				return ::send(static_cast<SOCKET>(pump.socket), data.data(), static_cast<int>(data.size()), 0)
					!= SOCKET_ERROR;
			}

			std::size_t sent = 0;
			while (sent < data.size()) {
				if (pump.stop.load()) return false;
				const NetWait wait = netWaitWritable(pump.socket, kSliceMs);
				if (wait == NetWait::Timeout) continue;
				if (wait != NetWait::Ready) return false;

				const std::size_t chunk = (std::min)(data.size() - sent, kChunk);
				const int put = ::send(static_cast<SOCKET>(pump.socket), data.data() + sent,
					static_cast<int>(chunk), 0);
				if (put == SOCKET_ERROR) return false;
				sent += static_cast<std::size_t>(put);
			}

			return true;
		}

		static bool forward(InputPump& pump, const std::string& data) {
			if (data.empty()) return true;

			pump.last_activity_ms.store(nowMs());
			return sendUnlessStopped(pump, translateInput(pump, data));
		}

		static bool forwardCrtReadAhead(InputPump& pump) {
			if (!pump.from_stdin) return true;

			const int buffered = stdinBufferedBytes();
			if (buffered <= 0) return true;

			std::string data(static_cast<std::size_t>(buffered), '\0');
			const std::size_t got = std::fread(data.data(), 1, data.size(), stdin);
			data.resize(got);
			return forward(pump, data);
		}

		// A console read that Ctrl+C cut short is no end of file.
		static bool readChunk(InputPump& pump, std::string& data, bool& at_eof) {
			char buffer[kChunk];
			DWORD got = 0;
			const BOOL ok = ::ReadFile(pump.input, buffer, sizeof(buffer), &got, nullptr);
			at_eof = (ok != 0 && got == 0 && !ctrlCPending()) || (ok == 0 && ::GetLastError() == ERROR_BROKEN_PIPE);
			data.assign(buffer, got);
			return ok != 0 && got > 0;
		}

		static void finishPump(InputPump& pump, bool at_eof) {
			if (at_eof && pump.shutdown_on_eof && !pump.stop.load()) netShutdownSend(pump.socket);

			pump.reached_eof.store(at_eof);
			pump.finished_at_ms.store(nowMs());
			pump.finished.store(true);
		}

		static void runPump(InputPump& pump) {
			bool at_eof = false;
			bool flowing = forwardCrtReadAhead(pump);
			while (flowing && !pump.stop.load()) {
				std::string data;
				flowing = readChunk(pump, data, at_eof) && forward(pump, data);
			}

			finishPump(pump, at_eof);
		}

		// The thread may sit in a read that only its own cancellation can end.
		static void stopPump(InputPump& pump, std::thread& thread) {
			pump.stop.store(true);
			while (!pump.finished.load()) {
				::CancelSynchronousIo(thread.native_handle());
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}

			thread.join();
		}

		struct RelayRun {
			NetSocket socket = kNoSocket;
			RelayEnds& ends;
			const RelayPolicy& policy;
			InputPump& pump;
			bool pumping = false;
			bool network_closed = false;
			long long last_network_ms = 0;
		};

		static bool writeOutput(RelayRun& run, const char* data, std::size_t size) {
			if (run.ends.output_is_stdout) {
				const bool written = std::fwrite(data, 1, size, stdout) == size;
				return std::fflush(stdout) == 0 && written;
			}

			if (run.ends.output_fd < 0) return true;
			return _write(run.ends.output_fd, data, static_cast<unsigned int>(size)) == static_cast<int>(size);
		}

		static void closeOutput(RelayRun& run) {
			if (run.ends.output_is_stdout || run.ends.output_fd < 0) return;

			_close(run.ends.output_fd);
			run.ends.output_fd = -1;
		}

		static bool inputDone(const RelayRun& run) {
			return !run.pumping || run.pump.finished.load();
		}

		static bool quitTimerExpired(const RelayRun& run) {
			if (run.policy.quit_after_input_eof < 0 || !inputDone(run)) return false;
			if (!run.pumping) return true;

			const long long waited = nowMs() - run.pump.finished_at_ms.load();
			return waited >= static_cast<long long>(run.policy.quit_after_input_eof * 1000.0);
		}

		static bool idleExpired(const RelayRun& run) {
			if (run.policy.idle_timeout < 0) return false;

			const long long last = (std::max)(run.last_network_ms, run.pump.last_activity_ms.load());
			return nowMs() - last >= static_cast<long long>(run.policy.idle_timeout * 1000.0);
		}

		static bool ignorableReceiveError(const RelayRun& run) {
			const int code = netLastError();
			return run.policy.udp && (code == WSAECONNRESET || code == WSAEMSGSIZE);
		}

		static bool receiveOnce(RelayRun& run, RelayEnd& end) {
			char buffer[65536];
			const int got = netReceive(run.socket, buffer, sizeof(buffer));
			if (got < 0 && ignorableReceiveError(run)) return true;
			if (got < 0) {
				end = RelayEnd::NetworkFailed;
				return false;
			}

			run.last_network_ms = nowMs();
			if (got == 0 && run.policy.udp) return true;
			if (got > 0 && writeOutput(run, buffer, static_cast<std::size_t>(got))) return true;
			if (got > 0) {
				end = RelayEnd::OutputFailed;
				return false;
			}

			run.network_closed = true;
			closeOutput(run);
			end = RelayEnd::NetworkClosed;
			return !run.policy.end_on_network_eof;
		}

		static bool stepRelay(RelayRun& run, RelayEnd& end) {
			if (ctrlCPending()) {
				end = RelayEnd::Interrupted;
				return false;
			}

			if (quitTimerExpired(run) || (run.network_closed && inputDone(run))) {
				end = RelayEnd::InputDone;
				return false;
			}

			if (idleExpired(run)) {
				end = RelayEnd::IdleTimeout;
				return false;
			}

			if (run.network_closed) {
				std::this_thread::sleep_for(std::chrono::milliseconds(kSliceMs / 4));
				return true;
			}

			const NetWait wait = netWaitReadable(run.socket, kSliceMs);
			if (wait == NetWait::Timeout || wait == NetWait::Interrupted) return true;
			if (wait == NetWait::Ready) return receiveOnce(run, end);

			end = RelayEnd::NetworkFailed;
			return false;
		}

		static void preparePump(InputPump& pump, NetSocket socket, const RelayEnds& ends,
		                        const RelayPolicy& policy) {
			pump.socket = socket;
			pump.input = reinterpret_cast<HANDLE>(_get_osfhandle(ends.input_fd));
			pump.from_stdin = ends.input_is_stdin;
			pump.console = isConsoleHandle(pump.input);
			pump.udp = policy.udp;
			pump.crlf = policy.crlf;
			pump.shutdown_on_eof = policy.shutdown_on_input_eof;
			pump.last_activity_ms.store(nowMs());
		}

		static bool hasInput(const RelayEnds& ends) {
			if (ends.input_exhausted || ends.input_fd < 0) return false;

			const intptr_t handle = _get_osfhandle(ends.input_fd);
			return handle != -1 && handle != -2;
		}

	}  // namespace relay_detail

	RelayEnd relayStreams(NetSocket socket, RelayEnds& ends, const RelayPolicy& policy) {
		relay_detail::InputPump pump;
		relay_detail::preparePump(pump, socket, ends, policy);

		relay_detail::RelayRun run{ socket, ends, policy, pump };
		run.last_network_ms = relay_detail::nowMs();
		run.pumping = relay_detail::hasInput(ends);
		if (!run.pumping && policy.shutdown_on_input_eof && !policy.udp) netShutdownSend(socket);

		std::thread thread;
		if (run.pumping) thread = std::thread([&pump] { relay_detail::runPump(pump); });

		RelayEnd end = RelayEnd::NetworkClosed;
		bool running = true;
		while (running) running = relay_detail::stepRelay(run, end);

		if (run.pumping) relay_detail::stopPump(pump, thread);
		if (run.pumping && pump.reached_eof.load()) ends.input_exhausted = true;
		return end;
	}

}  // namespace wbsh

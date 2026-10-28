/**
 * @file coreutils_nc.cpp
 * @brief nc: TCP / UDP connections and listeners.
 *
 * busybox / OpenBSD netcat: a client (`nc HOST PORT`), a listener
 * (`-l`), a port scanner (`-z`), and a relay that either copies between
 * the socket and this shell's stdin / stdout or hands the socket to a
 * program (`-e` / `-c`). OpenBSD binds every interface for `-l`; a plain
 * `-l` here does the same, which may raise the Windows Firewall dialog
 * the first time, so a loopback test passes `-s 127.0.0.1`.
 */

#include "coreutils_internal.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <winsock2.h>
#  include <windows.h>
#  include <fcntl.h>
#  include <io.h>
#endif /* _WIN32 */

#include <cstdio>
#include <string>
#include <vector>

#include "executor.h"
#include "interrupt.h"
#include "numparse.h"
#include "optscan.h"
#include "sockets.h"
#include "stream_relay.h"

namespace wbsh {

#ifdef _WIN32

	namespace nc_detail {

		enum OptionId {
			kListen = 1, kUdp, kKeep, kSourcePort, kSourceAddr, kWait, kQuit, kShutdown,
			kScan, kVerbose, kNumeric, kExec, kCommand, kV4, kV6,
		};

		struct NcOptions {
			bool listen = false;
			bool udp = false;
			bool keep = false;
			bool scan = false;
			bool verbose = false;
			bool numeric = false;
			bool shutdown_on_eof = false;
			double wait_seconds = -1.0;
			double quit_seconds = -1.0;
			NetFamily family = NetFamily::Any;
			std::string source_port;
			std::string source_addr;
			std::string exec_program;
			std::string exec_command;
			std::string host;
			std::string port;
		};

		static const std::vector<OptDef>& optionDefs() {
			static const std::vector<OptDef> defs = {
				{ kListen,     'l', "listen",  OptArg::None },
				{ kUdp,        'u', "udp",     OptArg::None },
				{ kKeep,       'k', "keep",    OptArg::None },
				{ kSourcePort, 'p', "source-port", OptArg::Required },
				{ kSourceAddr, 's', "source", OptArg::Required },
				{ kWait,       'w', "wait",    OptArg::Required },
				{ kQuit,       'q', nullptr,   OptArg::Required },
				{ kShutdown,   'N', nullptr,   OptArg::None },
				{ kScan,       'z', "zero",    OptArg::None },
				{ kVerbose,    'v', "verbose", OptArg::None },
				{ kNumeric,    'n', nullptr,   OptArg::None },
				{ kExec,       'e', nullptr,   OptArg::Required },
				{ kCommand,    'c', nullptr,   OptArg::Required },
				{ kV4,         '4', nullptr,   OptArg::None },
				{ kV6,         '6', nullptr,   OptArg::None },
			};
			return defs;
		}

		static bool applySeconds(const OptHit& hit, double& out) {
			double seconds = 0;
			if (!parseDurationSeconds(hit.value, seconds)) return false;
			out = seconds;
			return true;
		}

		static bool applyOption(const OptHit& hit, NcOptions& options) {
			switch (hit.id) {
				case kListen: options.listen = true; return true;
				case kUdp: options.udp = true; return true;
				case kKeep: options.keep = true; return true;
				case kScan: options.scan = true; return true;
				case kVerbose: options.verbose = true; return true;
				case kNumeric: options.numeric = true; return true;
				case kShutdown: options.shutdown_on_eof = true; return true;
				case kSourcePort: options.source_port = hit.value; return true;
				case kSourceAddr: options.source_addr = hit.value; return true;
				case kExec: options.exec_program = hit.value; return true;
				case kCommand: options.exec_command = hit.value; return true;
				case kV4: options.family = NetFamily::V4; return true;
				case kV6: options.family = NetFamily::V6; return true;
				case kWait: return applySeconds(hit, options.wait_seconds);
				case kQuit: return applySeconds(hit, options.quit_seconds);
				default: return false;
			}
		}

		static void takeOperands(const std::vector<std::string>& operands, NcOptions& options) {
			if (!operands.empty()) options.host = operands[0];
			if (operands.size() >= 2) options.port = operands[1];
			if (options.listen && operands.size() == 1 && options.source_port.empty()) {
				options.port = operands[0];
				options.host.clear();
			}
		}

		static bool parse(const std::vector<std::string>& args, NcOptions& options) {
			OptScanResult scanned;
			if (!scanOptions("nc", args, optionDefs(), scanned)) return false;

			for (const OptHit& hit : scanned.hits) {
				if (!applyOption(hit, options)) {
					perr("nc", "invalid timeout value");
					return false;
				}
			}

			takeOperands(scanned.operands, options);
			if (!options.port.empty() || (options.listen && !options.source_port.empty())) return true;

			perr("nc", "missing port");
			return false;
		}

		static int connectTimeoutMs(const NcOptions& options) {
			if (options.wait_seconds < 0) return kNetForever;
			return static_cast<int>(options.wait_seconds * 1000.0);
		}

		static NetEndpoint remoteEndpoint(const NcOptions& options, const std::string& port) {
			NetEndpoint endpoint;
			endpoint.host = options.host.empty() ? "localhost" : options.host;
			endpoint.port = port;
			endpoint.udp = options.udp;
			endpoint.numeric = options.numeric;
			endpoint.family = options.family;
			return endpoint;
		}

		static NetEndpoint localEndpoint(const NcOptions& options) {
			NetEndpoint endpoint;
			endpoint.host = options.source_addr;
			endpoint.port = options.source_port;
			endpoint.udp = options.udp;
			endpoint.family = options.family;
			return endpoint;
		}

		static void reportVerbose(const NcOptions& options, const std::string& message) {
			if (options.verbose) std::fprintf(stderr, "nc: %s\n", message.c_str());
		}

		static RelayPolicy relayPolicy(const NcOptions& options) {
			RelayPolicy policy;
			policy.udp = options.udp;
			policy.shutdown_on_input_eof = options.shutdown_on_eof;
			policy.quit_after_input_eof = options.quit_seconds;
			policy.idle_timeout = options.listen ? -1.0 : options.wait_seconds;
			return policy;
		}

		struct StdioSwap {
			int saved[2] = { -1, -1 };
			bool swapped[2] = { false, false };
		};

		static void swapIn(StdioSwap& swap, int fd, HANDLE handle) {
			const HANDLE self = ::GetCurrentProcess();
			HANDLE copy = nullptr;
			if (::DuplicateHandle(self, handle, self, &copy, 0, TRUE, DUPLICATE_SAME_ACCESS) == 0) return;

			const int replacement = _open_osfhandle(reinterpret_cast<intptr_t>(copy), _O_BINARY);
			if (replacement < 0) {
				::CloseHandle(copy);
				return;
			}

			swap.saved[fd] = _dup(fd);
			swap.swapped[fd] = true;
			_dup2(replacement, fd);
			_close(replacement);
		}

		static void restore(StdioSwap& swap) {
			for (int fd = 0; fd < 2; ++fd) {
				if (!swap.swapped[fd]) continue;
				if (swap.saved[fd] >= 0) {
					_dup2(swap.saved[fd], fd);
					_close(swap.saved[fd]);
				}
			}
		}

		static std::vector<std::string> execArgv(const NcOptions& options) {
			if (!options.exec_command.empty()) return { "eval", options.exec_command };
			return { options.exec_program };
		}

		static int runWithSocketStdio(Executor& exec, NetSocket socket, const NcOptions& options) {
			const HANDLE handle = reinterpret_cast<HANDLE>(socket);
			StdioSwap swap;
			swapIn(swap, 0, handle);
			swapIn(swap, 1, handle);

			const HANDLE process = exec.spawnCommand(execArgv(options));
			restore(swap);
			if (process == INVALID_HANDLE_VALUE) return 1;

			::WaitForSingleObject(process, INFINITE);
			DWORD code = 0;
			::GetExitCodeProcess(process, &code);
			::CloseHandle(process);
			return static_cast<int>(code & 0xFFu);
		}

		static bool wantsExec(const NcOptions& options) {
			return !options.exec_program.empty() || !options.exec_command.empty();
		}

		static int serveSocket(Executor& exec, NetSocket socket, const NcOptions& options) {
			if (wantsExec(options)) return runWithSocketStdio(exec, socket, options);

			RelayEnds ends;
			ends.input_fd = 0;
			ends.input_is_stdin = true;
			ends.output_fd = 1;
			ends.output_is_stdout = true;
			const RelayEnd end = relayStreams(socket, ends, relayPolicy(options));
			return end == RelayEnd::Interrupted ? 130 : 0;
		}

	}  // namespace nc_detail

	using nc_detail::NcOptions;

	namespace nc_detail {

		static int connectClient(Executor& exec, const NcOptions& options) {
			NetError error;
			const NetSocket socket = netConnect(remoteEndpoint(options, options.port),
				localEndpoint(options), connectTimeoutMs(options), error);
			if (socket == kNoSocket) {
				perr("nc", options.host + " (" + options.port + "): " + netErrorText(error.code));
				return 1;
			}

			reportVerbose(options, "connected to " + options.host + ":" + options.port);
			const int status = serveSocket(exec, socket, options);
			netClose(socket);
			return status;
		}

		static bool portRange(const std::string& text, int& low, int& high) {
			const std::size_t dash = text.find('-');
			if (dash == std::string::npos) return parseInt(text, low) && (high = low) >= 0;
			return parseInt(text.substr(0, dash), low) && parseInt(text.substr(dash + 1), high);
		}

		static int probePort(const NcOptions& options, int port) {
			NetError error;
			const int timeout = options.wait_seconds < 0 ? 4000 : connectTimeoutMs(options);
			const NetSocket socket = netConnect(remoteEndpoint(options, std::to_string(port)),
				localEndpoint(options), timeout, error);
			if (socket == kNoSocket) {
				if (options.verbose) {
					std::fprintf(stderr, "nc: connect to %s port %d (%s) failed: %s\n",
						options.host.c_str(), port, options.udp ? "udp" : "tcp",
						netErrorText(error.code).c_str());
				}
				return 1;
			}

			std::fprintf(stderr, "Connection to %s %d port [%s] succeeded!\n", options.host.c_str(),
				port, options.udp ? "udp" : "tcp");
			netClose(socket);
			return 0;
		}

		static int scanPorts(const NcOptions& options) {
			int low = 0;
			int high = 0;
			if (!portRange(options.port, low, high)) {
				perr("nc", "invalid port range: " + options.port);
				return 1;
			}

			int open_count = 0;
			for (int port = (std::min)(low, high); port <= (std::max)(low, high); ++port) {
				if (ctrlCPending()) return 130;
				if (probePort(options, port) == 0) ++open_count;
			}

			return open_count > 0 ? 0 : 1;
		}

		static int reportListenFailure(const NetError& error) {
			perr("nc", std::string("failed to bind/listen: ") + netErrorText(error.code));
			return 1;
		}

		static NetEndpoint listenEndpoint(const NcOptions& options) {
			NetEndpoint endpoint;
			endpoint.host = !options.source_addr.empty() ? options.source_addr : options.host;
			endpoint.port = !options.source_port.empty() ? options.source_port : options.port;
			endpoint.udp = options.udp;
			endpoint.family = options.family;
			endpoint.numeric = options.numeric && !endpoint.host.empty();
			return endpoint;
		}

		static int serveListener(Executor& exec, NetSocket listener, const NcOptions& options) {
			for (;;) {
				NetWait wait = NetWait::Ready;
				const int timeout = options.wait_seconds < 0 ? kNetForever : connectTimeoutMs(options);
				const NetSocket client = netAccept(listener, timeout, wait);
				if (wait == NetWait::Interrupted) return 130;
				if (wait == NetWait::Timeout) return 0;
				if (client == kNoSocket) return reportListenFailure(NetError{ NetStage::Listen, netLastError() });

				reportVerbose(options, "connect from " + netPeerName(client));
				const int status = serveSocket(exec, client, options);
				netClose(client);
				if (!options.keep) return status;
			}
		}

		static int listenUdp(NetSocket socket, const NcOptions& options) {
			std::string first;
			const int timeout = options.wait_seconds < 0 ? kNetForever : connectTimeoutMs(options);
			const NetWait wait = netUdpAcceptPeer(socket, first, timeout);
			if (wait == NetWait::Interrupted) return 130;
			if (wait == NetWait::Timeout) return 0;
			if (wait != NetWait::Ready) return 1;

			reportVerbose(options, "connect from " + netPeerName(socket));
			std::fwrite(first.data(), 1, first.size(), stdout);
			std::fflush(stdout);

			RelayEnds ends;
			ends.input_fd = 0;
			ends.input_is_stdin = true;
			ends.output_fd = 1;
			ends.output_is_stdout = true;
			const RelayEnd end = relayStreams(socket, ends, relayPolicy(options));
			return end == RelayEnd::Interrupted ? 130 : 0;
		}

		static int runListener(Executor& exec, const NcOptions& options) {
			NetError error;
			const NetSocket listener = netListen(listenEndpoint(options), error);
			if (listener == kNoSocket) return reportListenFailure(error);

			reportVerbose(options, "listening");
			const int status = options.udp ? listenUdp(listener, options)
				: serveListener(exec, listener, options);
			netClose(listener);
			return status;
		}

	}  // namespace nc_detail

	static int builtin_nc(Executor& exec, const std::vector<std::string>& args) {
		NcOptions options;
		if (!nc_detail::parse(args, options)) return 1;

		const ScopedCtrlCCapture capture;
		if (options.listen) return nc_detail::runListener(exec, options);
		if (options.scan) return nc_detail::scanPorts(options);
		return nc_detail::connectClient(exec, options);
	}

	void registerNcBuiltin(Executor& exec) {
		exec.registerBuiltin("nc", builtin_nc);
		exec.registerBuiltin("netcat", builtin_nc);
	}

#else

	void registerNcBuiltin(Executor&) {}

#endif /* _WIN32 */

}  // namespace wbsh

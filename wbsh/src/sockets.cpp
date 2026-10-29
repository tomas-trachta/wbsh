/**
 * @file sockets.cpp
 * @brief Winsock startup, resolving, connecting with a timeout, listening and buffered reads.
 */

#include "sockets.h"

#ifdef _WIN32
// winsock2.h must precede windows.h or the legacy winsock symbols leak in.
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <afunix.h>
#  pragma comment(lib, "ws2_32.lib")
#endif /* _WIN32 */

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>

#include "interrupt.h"
#include "numparse.h"

namespace wbsh {

	namespace net_detail {

		struct ErrorName {
			int code;
			const char* text;
		};

		static const ErrorName kErrorNames[] = {
			{ WSAECONNREFUSED,    "Connection refused" },
			{ WSAETIMEDOUT,       "Connection timed out" },
			{ WSAEHOSTUNREACH,    "No route to host" },
			{ WSAENETUNREACH,     "Network is unreachable" },
			{ WSAENETDOWN,        "Network is down" },
			{ WSAEADDRINUSE,      "Address already in use" },
			{ WSAEADDRNOTAVAIL,   "Cannot assign requested address" },
			{ WSAEACCES,          "Permission denied" },
			{ WSAECONNRESET,      "Connection reset by peer" },
			{ WSAECONNABORTED,    "Software caused connection abort" },
			{ WSAEAFNOSUPPORT,    "Address family not supported by protocol" },
			{ WSAEINVAL,          "Invalid argument" },
			{ WSAENOTCONN,        "Transport endpoint is not connected" },
			{ WSAEMSGSIZE,        "Message too long" },
			{ WSAHOST_NOT_FOUND,  "Name or service not known" },
			{ WSANO_DATA,         "No address associated with hostname" },
			{ WSATRY_AGAIN,       "Temporary failure in name resolution" },
			{ WSANO_RECOVERY,     "Non-recoverable failure in name resolution" },
			{ WSATYPE_NOT_FOUND,  "Servname not supported for ai_socktype" },
		};

		static const int kPollSliceMs = 100;
		static const std::size_t kSendChunk = 16384;
		static const std::size_t kReadChunk = 16384;

		static std::string systemMessage(int code) {
			wchar_t* buffer = nullptr;
			const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
				| FORMAT_MESSAGE_IGNORE_INSERTS;
			const DWORD length = ::FormatMessageW(flags, nullptr, static_cast<DWORD>(code), 0,
				reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
			if (length == 0 || buffer == nullptr) return "error " + std::to_string(code);

			std::wstring wide(buffer, length);
			::LocalFree(buffer);
			while (!wide.empty() && (wide.back() == L'\n' || wide.back() == L'\r' || wide.back() == L'.')) {
				wide.pop_back();
			}

			const int size = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
				nullptr, 0, nullptr, nullptr);
			std::string text(static_cast<std::size_t>(size), '\0');
			::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), text.data(),
				size, nullptr, nullptr);
			return text;
		}

		static int familyOf(NetFamily family) {
			switch (family) {
				case NetFamily::V4: return AF_INET;
				case NetFamily::V6: return AF_INET6;
				case NetFamily::Any: return AF_UNSPEC;
				default: return AF_UNSPEC;
			}
		}

		static addrinfo* resolve(const NetEndpoint& endpoint, bool passive, NetError& error) {
			addrinfo hints{};
			hints.ai_family = familyOf(endpoint.family);
			hints.ai_socktype = endpoint.udp ? SOCK_DGRAM : SOCK_STREAM;
			hints.ai_protocol = endpoint.udp ? IPPROTO_UDP : IPPROTO_TCP;
			if (passive) hints.ai_flags |= AI_PASSIVE;
			if (endpoint.numeric) hints.ai_flags |= AI_NUMERICHOST | AI_NUMERICSERV;

			const char* host = endpoint.host.empty() ? nullptr : endpoint.host.c_str();
			const char* port = endpoint.port.empty() ? "0" : endpoint.port.c_str();
			addrinfo* result = nullptr;
			const int status = ::getaddrinfo(host, port, &hints, &result);
			if (status == 0) return result;

			error.stage = NetStage::Resolve;
			error.code = status;
			return nullptr;
		}

		static void setBlocking(SOCKET socket, bool blocking) {
			u_long non_blocking = blocking ? 0 : 1;
			::ioctlsocket(socket, FIONBIO, &non_blocking);
		}

		static bool bindLocal(SOCKET socket, const addrinfo& candidate, const NetEndpoint& local,
		                      NetError& error) {
			if (local.host.empty() && local.port.empty()) return true;

			NetEndpoint wanted = local;
			wanted.udp = candidate.ai_socktype == SOCK_DGRAM;
			wanted.family = candidate.ai_family == AF_INET6 ? NetFamily::V6 : NetFamily::V4;
			addrinfo* address = resolve(wanted, true, error);
			if (address == nullptr) return false;

			const int status = ::bind(socket, address->ai_addr, static_cast<int>(address->ai_addrlen));
			::freeaddrinfo(address);
			if (status == 0) return true;

			error.stage = NetStage::Bind;
			error.code = ::WSAGetLastError();
			return false;
		}

		static int remainingMs(std::chrono::steady_clock::time_point deadline) {
			const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
				deadline - std::chrono::steady_clock::now()).count();
			return left <= 0 ? 0 : static_cast<int>(left);
		}

		static NetWait waitSocket(SOCKET socket, int timeout_ms, bool for_write) {
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(
				timeout_ms < 0 ? 0 : timeout_ms);
			for (;;) {
				if (ctrlCPending()) return NetWait::Interrupted;

				const int left = timeout_ms < 0 ? kPollSliceMs : remainingMs(deadline);
				const int slice = (std::min)(left, kPollSliceMs);
				fd_set ready;
				fd_set failed;
				FD_ZERO(&ready);
				FD_ZERO(&failed);
				FD_SET(socket, &ready);
				FD_SET(socket, &failed);
				timeval wait{ slice / 1000, (slice % 1000) * 1000 };
				const int status = for_write ? ::select(0, nullptr, &ready, &failed, &wait)
					: ::select(0, &ready, nullptr, &failed, &wait);
				if (status < 0) return NetWait::Failed;
				if (status > 0) return FD_ISSET(socket, &failed) ? NetWait::Failed : NetWait::Ready;
				if (timeout_ms >= 0 && left <= kPollSliceMs) return NetWait::Timeout;
			}
		}

		static int pendingSocketError(SOCKET socket) {
			int code = 0;
			int length = sizeof(code);
			::getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&code), &length);
			return code == 0 ? WSAECONNREFUSED : code;
		}

		static bool finishConnect(SOCKET socket, int timeout_ms, NetError& error) {
			const NetWait wait = waitSocket(socket, timeout_ms, true);
			if (wait == NetWait::Ready) return true;

			error.stage = wait == NetWait::Interrupted ? NetStage::Interrupted : NetStage::Connect;
			error.code = wait == NetWait::Timeout ? WSAETIMEDOUT : pendingSocketError(socket);
			return false;
		}

		static SOCKET connectCandidate(const addrinfo& candidate, const NetEndpoint& local,
		                               int timeout_ms, NetError& error) {
			const SOCKET socket = ::socket(candidate.ai_family, candidate.ai_socktype, candidate.ai_protocol);
			if (socket == INVALID_SOCKET) {
				error.stage = NetStage::Socket;
				error.code = ::WSAGetLastError();
				return INVALID_SOCKET;
			}

			if (!bindLocal(socket, candidate, local, error)) {
				::closesocket(socket);
				return INVALID_SOCKET;
			}

			setBlocking(socket, false);
			const int status = ::connect(socket, candidate.ai_addr, static_cast<int>(candidate.ai_addrlen));
			const bool started = status == 0 || ::WSAGetLastError() == WSAEWOULDBLOCK;
			if (!started) {
				error.stage = NetStage::Connect;
				error.code = ::WSAGetLastError();
			}

			if (!started || (status != 0 && !finishConnect(socket, timeout_ms, error))) {
				::closesocket(socket);
				return INVALID_SOCKET;
			}

			setBlocking(socket, true);
			return socket;
		}

		// Windows makes an IPv6 socket IPv6-only unless told otherwise, so an
		// "every interface" listener asks for both families explicitly.
		static void allowBothFamilies(SOCKET socket, const addrinfo& candidate, const NetEndpoint& local) {
			if (candidate.ai_family != AF_INET6 || !local.host.empty()) return;
			if (local.family != NetFamily::Any) return;

			DWORD v6_only = 0;
			::setsockopt(socket, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6_only),
				sizeof(v6_only));
		}

		static SOCKET listenCandidate(const addrinfo& candidate, const NetEndpoint& local, NetError& error) {
			const SOCKET socket = ::socket(candidate.ai_family, candidate.ai_socktype, candidate.ai_protocol);
			if (socket == INVALID_SOCKET) {
				error.stage = NetStage::Socket;
				error.code = ::WSAGetLastError();
				return INVALID_SOCKET;
			}

			allowBothFamilies(socket, candidate, local);
			const bool bound = ::bind(socket, candidate.ai_addr, static_cast<int>(candidate.ai_addrlen)) == 0;
			const bool listening = bound && (local.udp || ::listen(socket, SOMAXCONN) == 0);
			if (listening) return socket;

			error.stage = bound ? NetStage::Listen : NetStage::Bind;
			error.code = ::WSAGetLastError();
			::closesocket(socket);
			return INVALID_SOCKET;
		}

		static const addrinfo* preferIpv6ForWildcard(const addrinfo* list, const NetEndpoint& local) {
			if (!local.host.empty() || local.family != NetFamily::Any) return list;

			for (const addrinfo* entry = list; entry != nullptr; entry = entry->ai_next) {
				if (entry->ai_family == AF_INET6) return entry;
			}

			return list;
		}

		static std::string unmappedHost(const std::string& host) {
			const std::string prefix = "::ffff:";
			if (host.compare(0, prefix.size(), prefix) == 0 && host.find('.') != std::string::npos) {
				return host.substr(prefix.size());
			}

			return host;
		}

		static bool addressText(const sockaddr_storage& address, int length, std::string& host, int& port) {
			char host_text[NI_MAXHOST];
			char port_text[NI_MAXSERV];
			const int status = ::getnameinfo(reinterpret_cast<const sockaddr*>(&address), length, host_text,
				sizeof(host_text), port_text, sizeof(port_text), NI_NUMERICHOST | NI_NUMERICSERV);
			if (status != 0) return false;

			host = unmappedHost(host_text);
			return parseInt(port_text, port);
		}

		static bool takeBuffered(NetReader& reader, const std::string& delimiter, std::string& out) {
			const std::size_t found = reader.pending.find(delimiter);
			if (found == std::string::npos) return false;

			const std::size_t end = found + delimiter.size();
			out = reader.pending.substr(0, end);
			reader.pending.erase(0, end);
			return true;
		}

		static NetWait fillReader(NetReader& reader, int timeout_ms) {
			const NetWait wait = netWaitReadable(reader.socket, timeout_ms);
			if (wait != NetWait::Ready) return wait;

			char buffer[kReadChunk];
			const int got = netReceive(reader.socket, buffer, sizeof(buffer));
			if (got < 0) return NetWait::Failed;
			if (got == 0) {
				reader.closed = true;
				return NetWait::Closed;
			}

			reader.pending.append(buffer, static_cast<std::size_t>(got));
			return NetWait::Ready;
		}

	}  // namespace net_detail

	std::string netErrorText(int code) {
		for (const net_detail::ErrorName& entry : net_detail::kErrorNames) {
			if (entry.code == code) return entry.text;
		}

		return net_detail::systemMessage(code);
	}

	bool netStartup() {
		static std::once_flag once;
		static bool started = false;
		std::call_once(once, [] {
			WSADATA data;
			started = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
		});
		return started;
	}

	static bool failAt(NetError& error, NetStage stage, int code) {
		error.stage = stage;
		error.code = code;
		return false;
	}

	bool netCreateUnixSocketFile(const std::string& win32_path, NetError& error) {
		if (!netStartup()) return failAt(error, NetStage::Startup, ::WSAGetLastError());

		sockaddr_un address{};
		address.sun_family = AF_UNIX;
		if (win32_path.size() >= sizeof(address.sun_path)) {
			return failAt(error, NetStage::Bind, WSAENAMETOOLONG);
		}

		std::memcpy(address.sun_path, win32_path.data(), win32_path.size());
		const SOCKET sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
		if (sock == INVALID_SOCKET) return failAt(error, NetStage::Socket, ::WSAGetLastError());

		const auto* name = reinterpret_cast<const sockaddr*>(&address);
		const bool bound = ::bind(sock, name, sizeof(address)) == 0;
		if (!bound) failAt(error, NetStage::Bind, ::WSAGetLastError());
		::closesocket(sock);
		return bound;
	}

	NetSocket netConnect(const NetEndpoint& remote, const NetEndpoint& local, int timeout_ms,
	                     NetError& error) {
		if (!netStartup()) {
			error.stage = NetStage::Startup;
			error.code = ::WSAGetLastError();
			return kNoSocket;
		}

		addrinfo* candidates = net_detail::resolve(remote, false, error);
		if (candidates == nullptr) return kNoSocket;

		SOCKET socket = INVALID_SOCKET;
		for (const addrinfo* entry = candidates; entry != nullptr && socket == INVALID_SOCKET;
		     entry = entry->ai_next) {
			socket = net_detail::connectCandidate(*entry, local, timeout_ms, error);
			if (error.stage == NetStage::Interrupted) break;
		}

		::freeaddrinfo(candidates);
		return socket == INVALID_SOCKET ? kNoSocket : static_cast<NetSocket>(socket);
	}

	NetSocket netListen(const NetEndpoint& local, NetError& error) {
		if (!netStartup()) {
			error.stage = NetStage::Startup;
			error.code = ::WSAGetLastError();
			return kNoSocket;
		}

		addrinfo* candidates = net_detail::resolve(local, true, error);
		if (candidates == nullptr) return kNoSocket;

		const addrinfo* first = net_detail::preferIpv6ForWildcard(candidates, local);
		SOCKET socket = net_detail::listenCandidate(*first, local, error);
		for (const addrinfo* entry = candidates; entry != nullptr && socket == INVALID_SOCKET;
		     entry = entry->ai_next) {
			if (entry != first) socket = net_detail::listenCandidate(*entry, local, error);
		}

		::freeaddrinfo(candidates);
		return socket == INVALID_SOCKET ? kNoSocket : static_cast<NetSocket>(socket);
	}

	NetWait netWaitReadable(NetSocket socket, int timeout_ms) {
		return net_detail::waitSocket(static_cast<SOCKET>(socket), timeout_ms, false);
	}

	NetWait netWaitWritable(NetSocket socket, int timeout_ms) {
		return net_detail::waitSocket(static_cast<SOCKET>(socket), timeout_ms, true);
	}

	NetSocket netAccept(NetSocket listener, int timeout_ms, NetWait& wait) {
		wait = netWaitReadable(listener, timeout_ms);
		if (wait != NetWait::Ready) return kNoSocket;

		const SOCKET accepted = ::accept(static_cast<SOCKET>(listener), nullptr, nullptr);
		if (accepted != INVALID_SOCKET) return static_cast<NetSocket>(accepted);

		wait = NetWait::Failed;
		return kNoSocket;
	}

	NetWait netUdpAcceptPeer(NetSocket socket, std::string& first_datagram, int timeout_ms) {
		const NetWait wait = netWaitReadable(socket, timeout_ms);
		if (wait != NetWait::Ready) return wait;

		char buffer[65536];
		sockaddr_storage from{};
		int from_length = sizeof(from);
		const int got = ::recvfrom(static_cast<SOCKET>(socket), buffer, sizeof(buffer), 0,
			reinterpret_cast<sockaddr*>(&from), &from_length);
		if (got < 0) return NetWait::Failed;

		first_datagram.assign(buffer, static_cast<std::size_t>(got));
		if (::connect(static_cast<SOCKET>(socket), reinterpret_cast<sockaddr*>(&from), from_length) != 0) {
			return NetWait::Failed;
		}

		return NetWait::Ready;
	}

	int netReceive(NetSocket socket, char* buffer, std::size_t size) {
		const int capped = static_cast<int>((std::min)(size, static_cast<std::size_t>(1) << 30));
		const int got = ::recv(static_cast<SOCKET>(socket), buffer, capped, 0);
		return got == SOCKET_ERROR ? -1 : got;
	}

	bool netSendAll(NetSocket socket, const char* data, std::size_t size) {
		std::size_t sent = 0;
		while (sent < size) {
			if (netWaitWritable(socket, kNetForever) != NetWait::Ready) return false;

			const std::size_t chunk = (std::min)(size - sent, net_detail::kSendChunk);
			const int put = ::send(static_cast<SOCKET>(socket), data + sent, static_cast<int>(chunk), 0);
			if (put == SOCKET_ERROR) return false;
			sent += static_cast<std::size_t>(put);
		}

		return true;
	}

	bool netSendText(NetSocket socket, const std::string& text) {
		return netSendAll(socket, text.data(), text.size());
	}

	void netShutdownSend(NetSocket socket) {
		::shutdown(static_cast<SOCKET>(socket), SD_SEND);
	}

	void netClose(NetSocket socket) {
		if (socket != kNoSocket) ::closesocket(static_cast<SOCKET>(socket));
	}

	int netLastError() {
		return ::WSAGetLastError();
	}

	bool netLocalAddress(NetSocket socket, std::string& host, int& port) {
		sockaddr_storage address{};
		int length = sizeof(address);
		if (::getsockname(static_cast<SOCKET>(socket), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
			return false;
		}

		return net_detail::addressText(address, length, host, port);
	}

	bool netPeerAddress(NetSocket socket, std::string& host, int& port) {
		sockaddr_storage address{};
		int length = sizeof(address);
		if (::getpeername(static_cast<SOCKET>(socket), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
			return false;
		}

		return net_detail::addressText(address, length, host, port);
	}

	std::string netPeerName(NetSocket socket) {
		sockaddr_storage address{};
		int length = sizeof(address);
		if (::getpeername(static_cast<SOCKET>(socket), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
			return "unknown";
		}

		char name[NI_MAXHOST];
		const int status = ::getnameinfo(reinterpret_cast<const sockaddr*>(&address), length, name,
			sizeof(name), nullptr, 0, NI_NAMEREQD);
		if (status == 0) return name;

		std::string host;
		int port = 0;
		return net_detail::addressText(address, length, host, port) ? host : "unknown";
	}

	bool netIsIpv6Address(const std::string& host) {
		return host.find(':') != std::string::npos;
	}

	NetWait netReadUntil(NetReader& reader, const std::string& delimiter, std::size_t limit,
	                     std::string& out, int timeout_ms) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(
			timeout_ms < 0 ? 0 : timeout_ms);
		for (;;) {
			if (net_detail::takeBuffered(reader, delimiter, out)) return NetWait::Ready;
			if (reader.pending.size() >= limit) return NetWait::Failed;

			if (reader.closed) {
				out = reader.pending;
				reader.pending.clear();
				return NetWait::Closed;
			}

			const int left = timeout_ms < 0 ? kNetForever : net_detail::remainingMs(deadline);
			const NetWait wait = net_detail::fillReader(reader, left);
			if (wait != NetWait::Ready && wait != NetWait::Closed) return wait;
		}
	}

	NetWait netReadLine(NetReader& reader, std::string& line, int timeout_ms) {
		const NetWait wait = netReadUntil(reader, "\n", static_cast<std::size_t>(1) << 20, line,
			timeout_ms);
		if (!line.empty() && line.back() == '\n') line.pop_back();
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (wait == NetWait::Closed && !line.empty()) return NetWait::Ready;
		return wait;
	}

	NetWait netReadSome(NetReader& reader, std::size_t limit, std::string& out, int timeout_ms) {
		if (reader.pending.empty() && !reader.closed) {
			const NetWait wait = net_detail::fillReader(reader, timeout_ms);
			if (wait != NetWait::Ready && wait != NetWait::Closed) return wait;
		}

		if (reader.pending.empty()) return NetWait::Closed;

		const std::size_t take = (std::min)(limit, reader.pending.size());
		out = reader.pending.substr(0, take);
		reader.pending.erase(0, take);
		return NetWait::Ready;
	}

}  // namespace wbsh

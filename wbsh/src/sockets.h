#pragma once

/**
 * @file sockets.h
 * @brief The Winsock plumbing nc, ftp, whois and httpd share.
 *
 * Sockets travel as plain integers so that this header needs no Winsock
 * include of its own. Every wait is cut into short polls and gives up as
 * soon as Ctrl+C is pending (interrupt.h); kNetForever never times out.
 */

#include <cstddef>
#include <cstdint>
#include <string>

namespace wbsh {

	/// A Winsock SOCKET.
	using NetSocket = std::uintptr_t;

	inline constexpr NetSocket kNoSocket = ~static_cast<NetSocket>(0);
	inline constexpr int kNetForever = -1;

	enum class NetFamily { Any, V4, V6 };

	enum class NetWait { Ready, Timeout, Interrupted, Closed, Failed };

	enum class NetStage { None, Startup, Resolve, Socket, Bind, Listen, Connect, Interrupted };

	struct NetError {
		NetStage stage = NetStage::None;
		int code = 0;
	};

	/// An address to connect to or bind; an empty host binds every interface.
	struct NetEndpoint {
		std::string host;
		std::string port;
		bool udp = false;
		bool numeric = false;
		NetFamily family = NetFamily::Any;
	};

	/// strerror-style text for a Winsock or getaddrinfo error code.
	std::string netErrorText(int code);

	/// Starts Winsock once per process.
	bool netStartup();

	/**
	 * @brief Connects to @p remote, bound first to @p local when it names
	 *        a host or port, trying every address the name resolves to.
	 * @return the connected (blocking) socket, or kNoSocket with @p error set.
	 */
	NetSocket netConnect(const NetEndpoint& remote, const NetEndpoint& local, int timeout_ms,
	                     NetError& error);

	/// A bound socket, listening when it is TCP. An empty host listens on
	/// every interface, IPv6 and IPv4 alike where the system allows.
	NetSocket netListen(const NetEndpoint& local, NetError& error);

	/// Binds an AF_UNIX socket at @p win32_path and closes it, which leaves
	/// the socket file behind as on Unix (the `__mksock` test hook).
	bool netCreateUnixSocketFile(const std::string& win32_path, NetError& error);

	NetWait netWaitReadable(NetSocket socket, int timeout_ms);
	NetWait netWaitWritable(NetSocket socket, int timeout_ms);

	NetSocket netAccept(NetSocket listener, int timeout_ms, NetWait& wait);

	/// Waits for the first datagram on a bound UDP socket, connects the socket
	/// to its sender (so later recv / send reach that peer, as OpenBSD nc does)
	/// and hands back that first datagram's bytes.
	NetWait netUdpAcceptPeer(NetSocket socket, std::string& first_datagram, int timeout_ms);

	/// Bytes received, 0 at end of stream, -1 on error.
	int netReceive(NetSocket socket, char* buffer, std::size_t size);

	bool netSendAll(NetSocket socket, const char* data, std::size_t size);
	bool netSendText(NetSocket socket, const std::string& text);

	void netShutdownSend(NetSocket socket);
	void netClose(NetSocket socket);

	int netLastError();

	/// Numeric host and port of either end; IPv4-mapped IPv6 reads as IPv4.
	bool netLocalAddress(NetSocket socket, std::string& host, int& port);
	bool netPeerAddress(NetSocket socket, std::string& host, int& port);

	/// The peer's DNS name, or its numeric address when it has none.
	std::string netPeerName(NetSocket socket);

	bool netIsIpv6Address(const std::string& host);

	/// Buffered reads for line-oriented protocols (FTP, HTTP, whois).
	struct NetReader {
		NetSocket socket = kNoSocket;
		std::string pending;
		bool closed = false;
	};

	/**
	 * @brief Reads through the next @p delimiter, which stays on @p out.
	 *
	 * Closed with whatever arrived before the end of the stream in @p out;
	 * Failed when @p limit bytes came without the delimiter.
	 */
	NetWait netReadUntil(NetReader& reader, const std::string& delimiter, std::size_t limit,
	                     std::string& out, int timeout_ms);

	/// One line without its "\n" or "\r\n".
	NetWait netReadLine(NetReader& reader, std::string& line, int timeout_ms);

	/// Whatever is buffered, else the next chunk from the socket (at most @p limit bytes).
	NetWait netReadSome(NetReader& reader, std::size_t limit, std::string& out, int timeout_ms);

}  // namespace wbsh

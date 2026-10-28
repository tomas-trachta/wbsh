/**
 * @file ftp_session.cpp
 * @brief FTP control replies, EPSV / PASV / active data connections and transfers.
 */

#include "ftp_session.h"

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include "interrupt.h"
#include "numparse.h"

namespace wbsh {

	namespace ftp_detail {

		static const int kConnectTimeoutMs = 30000;
		static const int kReplyTimeoutMs = 60000;
		static const std::size_t kReplyLimit = 1 << 20;

		static bool hasLeadingCode(const std::string& line, int& code) {
			if (line.size() < 3) return false;
			for (int i = 0; i < 3; ++i) {
				if (std::isdigit(static_cast<unsigned char>(line[i])) == 0) return false;
			}

			return parseInt(line.substr(0, 3), code);
		}

		// A reply is either one "ddd text" line or a block opened by
		// "ddd-text" and closed by the next "ddd text" of the same code.
		static bool readReply(FtpSession& session, FtpReply& reply) {
			reply = FtpReply();
			int opened = -1;
			for (;;) {
				std::string line;
				if (netReadLine(session.reader, line, kReplyTimeoutMs) != NetWait::Ready) {
					session.error = "control connection closed";
					return false;
				}

				reply.text += line + "\n";
				int code = 0;
				const bool has_code = hasLeadingCode(line, code);
				if (has_code && reply.code == 0) reply.code = code;

				if (opened < 0 && has_code && line.size() > 3 && line[3] == '-') opened = code;
				else if (opened < 0) return true;
				else if (has_code && code == opened && line.size() > 3 && line[3] == ' ') return true;
			}
		}

	}  // namespace ftp_detail

	namespace ftp_detail {

		static NetEndpoint controlEndpoint(const std::string& host, const std::string& port,
		                                   NetFamily family) {
			NetEndpoint endpoint;
			endpoint.host = host;
			endpoint.port = port;
			endpoint.family = family;
			return endpoint;
		}

		static bool parsePasv(const std::string& text, std::string& host, std::string& port) {
			const std::size_t open = text.find('(');
			const std::size_t close = text.find(')', open);
			if (open == std::string::npos || close == std::string::npos) return false;

			const std::string body = text.substr(open + 1, close - open - 1);
			std::vector<int> numbers;
			std::size_t start = 0;
			while (start < body.size() && numbers.size() < 6) {
				const std::size_t comma = body.find(',', start);
				int value = 0;
				if (!parseInt(body.substr(start, comma - start), value)) return false;
				numbers.push_back(value);
				if (comma == std::string::npos) break;
				start = comma + 1;
			}

			if (numbers.size() != 6) return false;
			host = std::to_string(numbers[0]) + "." + std::to_string(numbers[1]) + "."
				+ std::to_string(numbers[2]) + "." + std::to_string(numbers[3]);
			port = std::to_string(numbers[4] * 256 + numbers[5]);
			return true;
		}

		static bool parseEpsv(const std::string& text, std::string& port) {
			const std::size_t open = text.find('(');
			const std::size_t close = text.find(')', open);
			if (open == std::string::npos || close == std::string::npos) return false;

			const std::string body = text.substr(open + 1, close - open - 1);
			const char delimiter = body.empty() ? '|' : body[0];
			std::size_t last = body.find_last_of(delimiter);
			if (last == std::string::npos || last == 0) return false;

			std::size_t prev = body.find_last_of(delimiter, last - 1);
			if (prev == std::string::npos) return false;
			const std::string digits = body.substr(prev + 1, last - prev - 1);
			int value = 0;
			if (!parseInt(digits, value)) return false;
			port = std::to_string(value);
			return true;
		}

	}  // namespace ftp_detail

	bool ftpConnect(FtpSession& session, const std::string& host, const std::string& port) {
		NetError error;
		session.control = netConnect(ftp_detail::controlEndpoint(host, port, session.family),
			NetEndpoint{}, ftp_detail::kConnectTimeoutMs, error);
		if (session.control == kNoSocket) {
			session.error = netErrorText(error.code);
			return false;
		}

		session.reader = NetReader{ session.control };
		session.host = host;
		FtpReply reply;
		if (ftp_detail::readReply(session, reply) && reply.code == 220) return true;

		session.error = reply.text.empty() ? "no greeting" : reply.text;
		ftpClose(session);
		return false;
	}

	void ftpClose(FtpSession& session) {
		if (session.control != kNoSocket) {
			netSendText(session.control, "QUIT\r\n");
			std::string farewell;
			netReadLine(session.reader, farewell, 2000);
			netShutdownSend(session.control);
			netClose(session.control);
		}

		session.control = kNoSocket;
		session.reader = NetReader{};
	}

	bool ftpCommand(FtpSession& session, const std::string& line, FtpReply& reply) {
		if (session.verbose) std::fprintf(stderr, "---> %s\n", line.c_str());
		if (!netSendText(session.control, line + "\r\n")) {
			session.error = "write failed";
			return false;
		}

		if (!ftp_detail::readReply(session, reply)) return false;
		if (session.verbose) std::fputs(reply.text.c_str(), stderr);
		return true;
	}

	bool ftpLogin(FtpSession& session, const std::string& user, const std::string& password) {
		FtpReply reply;
		if (!ftpCommand(session, "USER " + user, reply)) return false;
		if (reply.code == 230) return true;

		if (reply.code == 331) {
			if (!ftpCommand(session, "PASS " + password, reply)) return false;
			if (reply.code == 230 || reply.code == 202) return true;
		}

		session.error = "login failed: " + reply.text;
		return false;
	}

	bool ftpSetType(FtpSession& session, bool binary) {
		FtpReply reply;
		if (!ftpCommand(session, binary ? "TYPE I" : "TYPE A", reply)) return false;
		session.binary = binary;
		return reply.code == 200;
	}

	namespace ftp_detail {

		static NetSocket connectData(FtpSession& session, const std::string& host,
		                             const std::string& port) {
			NetEndpoint endpoint;
			endpoint.host = host;
			endpoint.port = port;
			endpoint.numeric = true;
			NetError error;
			const NetSocket data = netConnect(endpoint, NetEndpoint{}, kConnectTimeoutMs, error);
			if (data == kNoSocket) session.error = netErrorText(error.code);
			return data;
		}

		static NetSocket passiveData(FtpSession& session) {
			FtpReply reply;
			std::string host;
			std::string port;
			if (ftpCommand(session, "EPSV", reply) && reply.code == 229
				&& parseEpsv(reply.text, port)) {
				return connectData(session, session.host, port);
			}

			if (!ftpCommand(session, "PASV", reply) || reply.code != 227
				|| !parsePasv(reply.text, host, port)) {
				session.error = "cannot enter passive mode";
				return kNoSocket;
			}

			return connectData(session, host, port);
		}

		// The data listener binds the control connection's own local address,
		// so an active-mode loopback transfer never touches 0.0.0.0.
		static NetSocket activeListener(FtpSession& session, std::string& host, int& port) {
			if (!netLocalAddress(session.control, host, port)) {
				session.error = "cannot read local address";
				return kNoSocket;
			}

			NetEndpoint local;
			local.host = host;
			local.port = "0";
			local.numeric = true;
			local.family = netIsIpv6Address(host) ? NetFamily::V6 : NetFamily::V4;
			NetError error;
			const NetSocket listener = netListen(local, error);
			if (listener == kNoSocket) session.error = netErrorText(error.code);
			else netLocalAddress(listener, host, port);
			return listener;
		}

		static std::string portCommand(const std::string& host, int port) {
			if (netIsIpv6Address(host)) {
				return "EPRT |2|" + host + "|" + std::to_string(port) + "|";
			}

			std::string octets = host;
			for (char& c : octets) {
				if (c == '.') c = ',';
			}

			return "PORT " + octets + "," + std::to_string(port / 256) + "," + std::to_string(port % 256);
		}

		static NetSocket activeData(FtpSession& session, const std::string& command) {
			std::string host;
			int port = 0;
			const NetSocket listener = activeListener(session, host, port);
			if (listener == kNoSocket) return kNoSocket;

			FtpReply reply;
			if (!ftpCommand(session, portCommand(host, port), reply) || reply.code != 200) {
				session.error = "PORT rejected";
				netClose(listener);
				return kNoSocket;
			}

			if (!ftpCommand(session, command, reply) || (reply.code != 150 && reply.code != 125)) {
				session.error = reply.text.empty() ? "transfer rejected" : reply.text;
				netClose(listener);
				return kNoSocket;
			}

			NetWait wait = NetWait::Ready;
			const NetSocket data = netAccept(listener, 30000, wait);
			netClose(listener);
			return data;
		}

	}  // namespace ftp_detail

	NetSocket ftpOpenData(FtpSession& session, const std::string& command, FtpDirection direction) {
		static_cast<void>(direction);
		if (!session.passive) return ftp_detail::activeData(session, command);

		const NetSocket data = ftp_detail::passiveData(session);
		if (data == kNoSocket) return kNoSocket;

		FtpReply reply;
		if (!ftpCommand(session, command, reply) || (reply.code != 150 && reply.code != 125)) {
			session.error = reply.text.empty() ? "transfer rejected" : reply.text;
			netClose(data);
			return kNoSocket;
		}

		return data;
	}

	bool ftpReceive(FtpSession& session, NetSocket data, std::FILE* out) {
		char buffer[65536];
		for (;;) {
			if (ctrlCPending()) {
				netClose(data);
				session.error = "interrupted";
				return false;
			}

			const NetWait wait = netWaitReadable(data, 200);
			if (wait == NetWait::Timeout) continue;
			if (wait != NetWait::Ready) break;

			const int got = netReceive(data, buffer, sizeof(buffer));
			if (got <= 0) break;
			if (out != nullptr) std::fwrite(buffer, 1, static_cast<std::size_t>(got), out);
		}

		netClose(data);
		FtpReply reply;
		return ftp_detail::readReply(session, reply) && (reply.code == 226 || reply.code == 250);
	}

	bool ftpSend(FtpSession& session, NetSocket data, std::FILE* in) {
		char buffer[65536];
		bool ok = true;
		for (;;) {
			const std::size_t got = std::fread(buffer, 1, sizeof(buffer), in);
			if (got == 0) break;
			if (!netSendAll(data, buffer, got)) {
				ok = false;
				break;
			}
		}

		netShutdownSend(data);
		netClose(data);
		FtpReply reply;
		const bool replied = ftp_detail::readReply(session, reply);
		return ok && replied && (reply.code == 226 || reply.code == 250);
	}

	bool ftpList(FtpSession& session, const std::string& path, bool names_only, std::string& out) {
		const std::string verb = names_only ? "NLST" : "LIST";
		const std::string command = path.empty() ? verb : verb + " " + path;
		const NetSocket data = ftpOpenData(session, command, FtpDirection::Download);
		if (data == kNoSocket) return false;

		char buffer[65536];
		for (;;) {
			const NetWait wait = netWaitReadable(data, 200);
			if (wait == NetWait::Timeout) {
				if (ctrlCPending()) break;
				continue;
			}

			if (wait != NetWait::Ready) break;
			const int got = netReceive(data, buffer, sizeof(buffer));
			if (got <= 0) break;
			out.append(buffer, static_cast<std::size_t>(got));
		}

		netClose(data);
		FtpReply reply;
		return ftp_detail::readReply(session, reply) && (reply.code == 226 || reply.code == 250);
	}

}  // namespace wbsh

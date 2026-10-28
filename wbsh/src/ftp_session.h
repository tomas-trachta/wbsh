#pragma once

/**
 * @file ftp_session.h
 * @brief The FTP control / data protocol shared by ftp, ftpget and ftpput.
 */

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "sockets.h"

namespace wbsh {

	struct FtpReply {
		int code = 0;
		std::string text;
	};

	/// A control connection and the transfer settings that ride on it.
	struct FtpSession {
		NetSocket control = kNoSocket;
		NetReader reader;
		bool binary = true;
		bool passive = true;
		bool verbose = false;
		NetFamily family = NetFamily::Any;
		std::string host;
		std::string error;
	};

	/// Connects the control channel and reads the 220 greeting.
	bool ftpConnect(FtpSession& session, const std::string& host, const std::string& port);

	void ftpClose(FtpSession& session);

	/// Sends one command line and reads its (possibly multiline) reply.
	bool ftpCommand(FtpSession& session, const std::string& line, FtpReply& reply);

	/// USER / PASS (and ACCT when the server asks); true once logged in.
	bool ftpLogin(FtpSession& session, const std::string& user, const std::string& password);

	/// Sets TYPE to match @p binary, remembering it on the session.
	bool ftpSetType(FtpSession& session, bool binary);

	enum class FtpDirection { Download, Upload };

	/// Opens a data connection (EPSV then PASV, else active) and sends @p command.
	NetSocket ftpOpenData(FtpSession& session, const std::string& command, FtpDirection direction);

	/// Receives the data connection into @p out (a FILE); closes it and reads the final reply.
	bool ftpReceive(FtpSession& session, NetSocket data, std::FILE* out);

	/// Sends @p in over the data connection; closes it and reads the final reply.
	bool ftpSend(FtpSession& session, NetSocket data, std::FILE* in);

	/// A directory listing (LIST, or NLST when @p names_only) as raw text.
	bool ftpList(FtpSession& session, const std::string& path, bool names_only, std::string& out);

}  // namespace wbsh

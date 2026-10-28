/**
 * @file coreutils_ftp.cpp
 * @brief ftp clients: the interactive `ftp` plus busybox `ftpget` / `ftpput`.
 *
 * All three share ftp_session.cpp for the protocol; this file is the
 * command surface. Transfers default to passive mode (EPSV, then PASV).
 */

#include "coreutils_internal.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#endif /* _WIN32 */

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include "curl_internal.h"
#include "executor.h"
#include "ftp_session.h"
#include "interrupt.h"
#include "numparse.h"
#include "optscan.h"
#include "sockets.h"

namespace wbsh {

#ifdef _WIN32

	namespace ftp_detail {

		static const char* kAnonymousUser = "anonymous";
		static const char* kAnonymousPass = "anonymous@";

		static std::vector<std::string> splitWords(const std::string& line) {
			std::vector<std::string> words;
			std::size_t i = 0;
			while (i < line.size()) {
				while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])) != 0) ++i;
				const std::size_t start = i;
				while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])) == 0) ++i;
				if (i > start) words.push_back(line.substr(start, i - start));
			}

			return words;
		}

		static std::string baseName(const std::string& path) {
			const std::size_t slash = path.find_last_of("/\\");
			return slash == std::string::npos ? path : path.substr(slash + 1);
		}

		struct FtpUrl {
			std::string host;
			std::string port = "21";
			std::string user;
			std::string password;
			std::string path;
			bool valid = false;
		};

		static bool isFtpUrl(const std::string& text) {
			return text.compare(0, 6, "ftp://") == 0;
		}

		static FtpUrl parseFtpUrl(const std::string& text) {
			FtpUrl url;
			curl_detail::UrlParts parts;
			if (!curl_detail::splitUrl(text, parts) || parts.scheme != "ftp") return url;

			url.host = parts.host;
			url.port = parts.port != 0 ? std::to_string(parts.port) : "21";
			url.user = parts.user;
			url.password = parts.password;
			url.path = parts.path.empty() || parts.path == "/" ? std::string() : parts.path.substr(1);
			url.valid = !url.host.empty();
			return url;
		}

		struct NetrcEntry {
			std::string login;
			std::string password;
			std::string account;
			bool found = false;
		};

		static std::vector<std::string> tokenizeNetrc(const std::string& text) {
			std::vector<std::string> tokens;
			std::size_t i = 0;
			while (i < text.size()) {
				while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) ++i;
				const std::size_t start = i;
				while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) == 0) ++i;
				if (i > start) tokens.push_back(text.substr(start, i - start));
			}

			return tokens;
		}

		static void applyNetrcToken(const std::string& key, const std::string& value, NetrcEntry& entry) {
			if (key == "login") entry.login = value;
			if (key == "password") entry.password = value;
			if (key == "account") entry.account = value;
		}

		static NetrcEntry matchNetrc(const std::vector<std::string>& tokens, const std::string& host) {
			NetrcEntry entry;
			bool active = false;
			for (std::size_t i = 0; i + 1 < tokens.size(); i += 2) {
				const std::string& key = tokens[i];
				const std::string& value = tokens[i + 1];
				if (key == "machine") active = value == host;
				else if (key == "default") active = !entry.found;
				else if (active) {
					applyNetrcToken(key, value, entry);
					entry.found = true;
				}
			}

			return entry;
		}

		static NetrcEntry loadNetrc(Executor& exec, const std::string& host) {
			const std::string home = exec.env().get("HOME");
			if (home.empty()) return NetrcEntry();

			std::FILE* file = fopenNative(exec, home + "/.netrc", "rb");
			if (file == nullptr) return NetrcEntry();

			std::string text;
			char buffer[4096];
			std::size_t got = 0;
			while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, got);
			std::fclose(file);
			return matchNetrc(tokenizeNetrc(text), host);
		}

		struct Login {
			std::string user;
			std::string password;
			bool explicit_user = false;
		};

		static Login resolveLogin(Executor& exec, const std::string& host, const Login& given) {
			Login login = given;
			if (login.explicit_user) return login;

			const NetrcEntry entry = loadNetrc(exec, host);
			if (entry.found && !entry.login.empty()) {
				login.user = entry.login;
				login.password = entry.password;
				return login;
			}

			if (login.user.empty()) login.user = kAnonymousUser;
			if (login.password.empty()) login.password = kAnonymousPass;
			return login;
		}

		static bool connectAndLogin(Executor& exec, FtpSession& session, const std::string& host,
		                            const std::string& port, const Login& given) {
			if (!ftpConnect(session, host, port)) {
				perr("ftp", host + ": " + session.error);
				return false;
			}

			const Login login = resolveLogin(exec, host, given);
			if (!ftpLogin(session, login.user, login.password)) {
				perr("ftp", session.error);
				ftpClose(session);
				return false;
			}

			return true;
		}

		static bool downloadTo(Executor& exec, FtpSession& session, const std::string& remote,
		                       const std::string& local, bool resume) {
			std::FILE* file = fopenNative(exec, local, resume ? "ab" : "wb");
			if (file == nullptr) {
				perr("ftp", "cannot open " + local);
				return false;
			}

			long long offset = resume ? _ftelli64(file) : 0;
			if (resume && offset > 0) {
				FtpReply reply;
				ftpCommand(session, "REST " + std::to_string(offset), reply);
			}

			const NetSocket data = ftpOpenData(session, "RETR " + remote, FtpDirection::Download);
			const bool ok = data != kNoSocket && ftpReceive(session, data, file);
			std::fclose(file);
			if (!ok) perr("ftp", "RETR " + remote + " failed: " + session.error);
			return ok;
		}

		static bool uploadFrom(Executor& exec, FtpSession& session, const std::string& local,
		                       const std::string& remote) {
			std::FILE* file = fopenNative(exec, local, "rb");
			if (file == nullptr) {
				perr("ftp", "cannot open " + local);
				return false;
			}

			const NetSocket data = ftpOpenData(session, "STOR " + remote, FtpDirection::Upload);
			const bool ok = data != kNoSocket && ftpSend(session, data, file);
			std::fclose(file);
			if (!ok) perr("ftp", "STOR " + remote + " failed: " + session.error);
			return ok;
		}

		enum ToolOptionId { kUser = 1, kPass, kPort, kContinue, kVerbose };

		struct ToolOptions {
			Login login;
			std::string port = "21";
			bool resume = false;
			bool verbose = false;
			std::vector<std::string> operands;
		};

		static const std::vector<OptDef>& toolDefs() {
			static const std::vector<OptDef> defs = {
				{ kUser, 'u', "username", OptArg::Required },
				{ kPass, 'p', "password", OptArg::Required },
				{ kPort, 'P', "port", OptArg::Required },
				{ kContinue, 'c', "continue", OptArg::None },
				{ kVerbose, 'v', "verbose", OptArg::None },
			};
			return defs;
		}

		static bool parseTool(const char* name, const std::vector<std::string>& args, ToolOptions& out) {
			OptScanResult scanned;
			if (!scanOptions(name, args, toolDefs(), scanned)) return false;

			for (const OptHit& hit : scanned.hits) {
				if (hit.id == kUser) { out.login.user = hit.value; out.login.explicit_user = true; }
				if (hit.id == kPass) out.login.password = hit.value;
				if (hit.id == kPort) out.port = hit.value;
				if (hit.id == kContinue) out.resume = true;
				if (hit.id == kVerbose) out.verbose = true;
			}

			out.operands = scanned.operands;
			return true;
		}

		static int ftpGetUrl(Executor& exec, const ToolOptions& options) {
			const FtpUrl url = parseFtpUrl(options.operands[0]);
			if (!url.valid || url.path.empty()) {
				perr("ftpget", "bad ftp URL: " + options.operands[0]);
				return 1;
			}

			const std::string local = options.operands.size() >= 2 ? options.operands[1]
				: baseName(url.path);
			Login login = options.login;
			if (!url.user.empty()) { login.user = url.user; login.password = url.password;
				login.explicit_user = true; }

			FtpSession session;
			session.verbose = options.verbose;
			if (!connectAndLogin(exec, session, url.host, url.port, login)) return 1;

			ftpSetType(session, true);
			const bool ok = downloadTo(exec, session, url.path, local, options.resume);
			ftpClose(session);
			return ok ? 0 : 1;
		}

		static int ftpGet(Executor& exec, const std::vector<std::string>& args) {
			ToolOptions options;
			if (!parseTool("ftpget", args, options) || options.operands.empty()) {
				perr("ftpget", "usage: ftpget [-cv] [-u USER] [-p PASS] [-P PORT] HOST LOCAL [REMOTE]");
				return 1;
			}

			if (isFtpUrl(options.operands[0])) return ftpGetUrl(exec, options);
			if (options.operands.size() < 2) {
				perr("ftpget", "usage: ftpget [-cv] [-u USER] [-p PASS] [-P PORT] HOST LOCAL [REMOTE]");
				return 1;
			}

			const std::string host = options.operands[0];
			const std::string local = options.operands[1];
			const std::string remote = options.operands.size() >= 3 ? options.operands[2]
				: baseName(local);
			FtpSession session;
			session.verbose = options.verbose;
			if (!connectAndLogin(exec, session, host, options.port, options.login)) return 1;

			ftpSetType(session, true);
			const bool ok = downloadTo(exec, session, remote, local, options.resume);
			ftpClose(session);
			return ok ? 0 : 1;
		}

		static int ftpPut(Executor& exec, const std::vector<std::string>& args) {
			ToolOptions options;
			if (!parseTool("ftpput", args, options) || options.operands.size() < 2) {
				perr("ftpput", "usage: ftpput [-v] [-u USER] [-p PASS] [-P PORT] HOST REMOTE LOCAL");
				return 1;
			}

			const std::string host = options.operands[0];
			const std::string remote = options.operands[1];
			const std::string local = options.operands.size() >= 3 ? options.operands[2]
				: baseName(remote);
			FtpSession session;
			session.verbose = options.verbose;
			if (!connectAndLogin(exec, session, host, options.port, options.login)) return 1;

			ftpSetType(session, true);
			const bool ok = uploadFrom(exec, session, local, remote);
			ftpClose(session);
			return ok ? 0 : 1;
		}

		struct Client {
			Executor& exec;
			FtpSession session;
			bool connected = false;
			bool prompting = true;
			bool hashing = false;
			bool interactive = true;
		};

		static void printReply(const FtpReply& reply) {
			std::fputs(reply.text.c_str(), stdout);
		}

		static bool requireConnection(const Client& client) {
			if (client.connected) return true;
			std::fputs("Not connected.\n", stdout);
			return false;
		}

		static void simpleCommand(Client& client, const std::string& wire) {
			if (!requireConnection(client)) return;
			FtpReply reply;
			if (ftpCommand(client.session, wire, reply)) printReply(reply);
			else std::fputs((client.session.error + "\n").c_str(), stdout);
		}

		static void doList(Client& client, const std::vector<std::string>& words, bool names_only) {
			if (!requireConnection(client)) return;
			ftpSetType(client.session, false);
			std::string listing;
			const std::string path = words.size() >= 2 ? words[1] : std::string();
			if (ftpList(client.session, path, names_only, listing)) std::fputs(listing.c_str(), stdout);
			else std::fputs((client.session.error + "\n").c_str(), stdout);
			ftpSetType(client.session, client.session.binary);
		}

		static void doGet(Client& client, const std::vector<std::string>& words) {
			if (!requireConnection(client) || words.size() < 2) return;
			const std::string local = words.size() >= 3 ? words[2] : baseName(words[1]);
			downloadTo(client.exec, client.session, words[1], local, false);
		}

		static void doPut(Client& client, const std::vector<std::string>& words) {
			if (!requireConnection(client) || words.size() < 2) return;
			const std::string remote = words.size() >= 3 ? words[2] : baseName(words[1]);
			uploadFrom(client.exec, client.session, words[1], remote);
		}

		static bool confirmTransfer(Client& client, const std::string& name) {
			if (!client.prompting || !client.interactive) return true;

			std::fprintf(stdout, "mget %s [anpqy?]? ", name.c_str());
			std::fflush(stdout);
			char answer[16] = { 0 };
			if (std::fgets(answer, sizeof(answer), stdin) == nullptr) return false;
			return answer[0] != 'n' && answer[0] != 'N';
		}

		static void doMget(Client& client, const std::vector<std::string>& words) {
			if (!requireConnection(client)) return;
			for (std::size_t i = 1; i < words.size(); ++i) {
				if (confirmTransfer(client, words[i])) {
					downloadTo(client.exec, client.session, words[i], baseName(words[i]), false);
				}
			}
		}

		static void doMput(Client& client, const std::vector<std::string>& words) {
			if (!requireConnection(client)) return;
			for (std::size_t i = 1; i < words.size(); ++i) {
				if (confirmTransfer(client, words[i])) {
					uploadFrom(client.exec, client.session, words[i], baseName(words[i]));
				}
			}
		}

		static void doLocalCd(Client& client, const std::vector<std::string>& words) {
			const std::string target = words.size() >= 2 ? words[1] : client.exec.env().get("HOME");
			std::string error;
			if (!changeDirectory(client.exec, target, error)) std::fputs((error + "\n").c_str(), stdout);
			else std::fprintf(stdout, "Local directory now %s\n", target.c_str());
		}

		static void doBang(Client& client, const std::string& line) {
			const std::size_t bang = line.find('!');
			const std::string command = line.substr(bang + 1);
			client.exec.executeText(command.empty() ? std::string() : command, "ftp");
		}

		static void doOpen(Client& client, const std::vector<std::string>& words, const Login& login) {
			if (client.connected) {
				std::fputs("Already connected.\n", stdout);
				return;
			}

			if (words.size() < 2) {
				std::fputs("usage: open host [port]\n", stdout);
				return;
			}

			const std::string port = words.size() >= 3 ? words[2] : "21";
			client.connected = connectAndLogin(client.exec, client.session, words[1], port, login);
		}

		static void doUser(Client& client, const std::vector<std::string>& words) {
			if (!requireConnection(client) || words.size() < 2) return;
			Login login;
			login.user = words[1];
			login.password = words.size() >= 3 ? words[2] : std::string();
			login.explicit_user = true;
			if (!ftpLogin(client.session, login.user, login.password)) {
				std::fputs((client.session.error + "\n").c_str(), stdout);
			}
		}

		static void toggle(bool& flag, const char* name) {
			flag = !flag;
			std::fprintf(stdout, "%s %s.\n", name, flag ? "on" : "off");
		}

		static void doRename(Client& client, const std::vector<std::string>& words) {
			if (!requireConnection(client)) return;
			FtpReply reply;
			if (ftpCommand(client.session, "RNFR " + words[1], reply) && reply.code == 350) {
				ftpCommand(client.session, "RNTO " + words[2], reply);
			}

			printReply(reply);
		}

		static void setMode(Client& client, bool binary) {
			if (!requireConnection(client)) return;
			ftpSetType(client.session, binary);
			std::fprintf(stdout, "Type set to %s.\n", binary ? "I" : "A");
		}

		static void printStatus(Client& client) {
			std::fprintf(stdout, "Connected: %s\nMode: %s\nType: %s\n",
				client.connected ? client.session.host.c_str() : "no",
				client.session.passive ? "passive" : "active",
				client.session.binary ? "binary" : "ascii");
		}

		static bool dispatch(Client& client, const std::string& line, const Login& login) {
			const std::vector<std::string> words = splitWords(line);
			if (words.empty()) return true;

			const std::string& command = words[0];
			if (command == "quit" || command == "bye") return false;
			if (!line.empty() && line[0] == '!') doBang(client, line);
			else if (command == "open") doOpen(client, words, login);
			else if (command == "user") doUser(client, words);
			else if (command == "ls" || command == "nlist") doList(client, words, true);
			else if (command == "dir") doList(client, words, false);
			else if (command == "get" || command == "recv") doGet(client, words);
			else if (command == "put" || command == "send") doPut(client, words);
			else if (command == "mget") doMget(client, words);
			else if (command == "mput") doMput(client, words);
			else if (command == "cd") simpleCommand(client, words.size() >= 2 ? "CWD " + words[1] : "CWD");
			else if (command == "pwd") simpleCommand(client, "PWD");
			else if (command == "lcd") doLocalCd(client, words);
			else if (command == "delete") simpleCommand(client, words.size() >= 2 ? "DELE " + words[1] : "DELE");
			else if (command == "mkdir") simpleCommand(client, words.size() >= 2 ? "MKD " + words[1] : "MKD");
			else if (command == "rmdir") simpleCommand(client, words.size() >= 2 ? "RMD " + words[1] : "RMD");
			else if (command == "rename" && words.size() >= 3) doRename(client, words);
			else if (command == "binary") setMode(client, true);
			else if (command == "ascii") setMode(client, false);
			else if (command == "passive") toggle(client.session.passive, "Passive mode");
			else if (command == "prompt") toggle(client.prompting, "Interactive prompting");
			else if (command == "hash") toggle(client.hashing, "Hash mark printing");
			else if (command == "verbose") toggle(client.session.verbose, "Verbose mode");
			else if (command == "status") printStatus(client);
			else std::fprintf(stdout, "?Invalid command: %s\n", command.c_str());
			return true;
		}

		static bool readCommand(bool interactive, std::string& line) {
			if (interactive) {
				std::fputs("ftp> ", stdout);
				std::fflush(stdout);
			}

			char buffer[4096];
			if (std::fgets(buffer, sizeof(buffer), stdin) == nullptr) return false;
			line = buffer;
			while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
			return true;
		}

		static void runClient(Client& client, const Login& login) {
			std::string line;
			while (!ctrlCPending() && readCommand(client.interactive, line)) {
				if (!dispatch(client, line, login)) break;
			}

			if (client.connected) ftpClose(client.session);
		}

		enum ClientOptionId { kNoAutoLogin = 1, kNoPrompt, kClientVerbose, kClientPassive };

		static const std::vector<OptDef>& clientDefs() {
			static const std::vector<OptDef> defs = {
				{ kNoAutoLogin, 'n', nullptr, OptArg::None },
				{ kNoPrompt, 'i', nullptr, OptArg::None },
				{ kClientVerbose, 'v', nullptr, OptArg::None },
				{ kClientPassive, 'p', nullptr, OptArg::None },
			};
			return defs;
		}

		// BSD-style autofetch: `ftp ftp://host/path` downloads and exits.
		static int autofetch(Executor& exec, const std::string& url_text, bool verbose) {
			const FtpUrl url = parseFtpUrl(url_text);
			if (!url.valid || url.path.empty()) {
				perr("ftp", "bad ftp URL: " + url_text);
				return 1;
			}

			Login login;
			if (!url.user.empty()) { login.user = url.user; login.password = url.password;
				login.explicit_user = true; }

			FtpSession session;
			session.verbose = verbose;
			if (!connectAndLogin(exec, session, url.host, url.port, login)) return 1;

			ftpSetType(session, true);
			const bool ok = downloadTo(exec, session, url.path, baseName(url.path), false);
			ftpClose(session);
			return ok ? 0 : 1;
		}

		static int ftpClient(Executor& exec, const std::vector<std::string>& args) {
			OptScanResult scanned;
			if (!scanOptions("ftp", args, clientDefs(), scanned)) return 1;

			Client client{ exec };
			client.interactive = isConsoleStream(stdin);
			Login login;
			bool auto_login = true;
			for (const OptHit& hit : scanned.hits) {
				if (hit.id == kNoAutoLogin) auto_login = false;
				if (hit.id == kNoPrompt) client.prompting = false;
				if (hit.id == kClientVerbose) client.session.verbose = true;
			}

			if (!scanned.operands.empty() && isFtpUrl(scanned.operands[0])) {
				return autofetch(exec, scanned.operands[0], client.session.verbose);
			}

			if (!scanned.operands.empty()) {
				const std::string port = scanned.operands.size() >= 2 ? scanned.operands[1] : "21";
				const Login use = auto_login ? login : Login{ "", "", true };
				client.connected = connectAndLogin(exec, client.session, scanned.operands[0], port, use);
			}

			runClient(client, login);
			return 0;
		}

	}  // namespace ftp_detail

	void registerFtpBuiltins(Executor& exec) {
		exec.registerBuiltin("ftp", ftp_detail::ftpClient);
		exec.registerBuiltin("ftpget", ftp_detail::ftpGet);
		exec.registerBuiltin("ftpput", ftp_detail::ftpPut);
	}

#else

	void registerFtpBuiltins(Executor&) {}

#endif /* _WIN32 */

}  // namespace wbsh

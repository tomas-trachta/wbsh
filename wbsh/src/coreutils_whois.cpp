/**
 * @file coreutils_whois.cpp
 * @brief whois: domain registration lookups (RFC 3912).
 *
 * A whois query is one line ("QUERY\r\n") to port 43, whose answer is
 * everything the server sends before it closes. With no `-h`, the query
 * starts at whois.iana.org, whose answer names the responsible server on
 * a `refer:` or `whois:` line; whois follows that once, and for a thin
 * .com / .net registry follows the registry's "Registrar WHOIS Server:"
 * line one step further, as GNU / busybox whois do.
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

#include "executor.h"
#include "interrupt.h"
#include "numparse.h"
#include "optscan.h"
#include "sockets.h"

namespace wbsh {

#ifdef _WIN32

	namespace whois_detail {

		enum OptionId { kHost = 1, kPort, kIana };

		struct WhoisOptions {
			std::string host;
			std::string port = "43";
			std::string query;
			bool iana_only = false;
			bool verbose = false;
		};

		static const std::string kIanaServer = "whois.iana.org";
		static const int kConnectTimeoutMs = 20000;
		static const int kReadTimeoutMs = 30000;
		static const std::size_t kReadLimit = static_cast<std::size_t>(4) << 20;

		static const std::vector<OptDef>& optionDefs() {
			static const std::vector<OptDef> defs = {
				{ kHost, 'h', "host", OptArg::Required },
				{ kPort, 'p', "port", OptArg::Required },
				{ kIana, 'I', nullptr, OptArg::None },
			};
			return defs;
		}

		static bool parse(const std::vector<std::string>& args, WhoisOptions& options) {
			OptScanResult scanned;
			if (!scanOptions("whois", args, optionDefs(), scanned)) return false;

			for (const OptHit& hit : scanned.hits) {
				if (hit.id == kHost) options.host = hit.value;
				if (hit.id == kPort) options.port = hit.value;
				if (hit.id == kIana) options.iana_only = true;
			}

			for (const std::string& operand : scanned.operands) {
				if (!options.query.empty()) options.query.push_back(' ');
				options.query += operand;
			}

			if (!options.query.empty()) return true;
			perr("whois", "no query given");
			return false;
		}

		static std::string trim(const std::string& text) {
			std::size_t start = 0;
			std::size_t end = text.size();
			while (start < end && std::isspace(static_cast<unsigned char>(text[start])) != 0) ++start;
			while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) --end;
			return text.substr(start, end - start);
		}

		static std::string lowerAscii(std::string text) {
			for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return text;
		}

		static bool drain(NetReader& reader, std::string& response) {
			while (response.size() < kReadLimit) {
				std::string chunk;
				const NetWait wait = netReadSome(reader, 65536, chunk, kReadTimeoutMs);
				if (wait == NetWait::Closed) return true;
				if (wait != NetWait::Ready) return wait == NetWait::Interrupted;
				response += chunk;
			}

			return true;
		}

		static bool query(const std::string& host, const std::string& port, const std::string& request,
		                  std::string& response) {
			NetEndpoint remote;
			remote.host = host;
			remote.port = port;
			NetError error;
			const NetSocket socket = netConnect(remote, NetEndpoint{}, kConnectTimeoutMs, error);
			if (socket == kNoSocket) {
				perr("whois", host + ": " + netErrorText(error.code));
				return false;
			}

			NetReader reader{ socket };
			const bool ok = netSendText(socket, request + "\r\n") && drain(reader, response);
			netClose(socket);
			if (ok) return true;

			perr("whois", host + ": connection error");
			return false;
		}

		static bool lineHasKey(const std::string& line, const char* key) {
			const std::string lower = lowerAscii(trim(line));
			return lower.compare(0, std::char_traits<char>::length(key), key) == 0;
		}

		static std::string valueAfterColon(const std::string& line) {
			const std::size_t colon = line.find(':');
			return colon == std::string::npos ? std::string() : trim(line.substr(colon + 1));
		}

		static std::string findReferral(const std::string& response, bool registrar) {
			std::size_t start = 0;
			while (start <= response.size()) {
				const std::size_t newline = response.find('\n', start);
				const std::string line = response.substr(start, newline - start);
				const bool matches = registrar ? lineHasKey(line, "registrar whois server:")
					: (lineHasKey(line, "refer:") || lineHasKey(line, "whois:"));
				if (matches) {
					const std::string value = valueAfterColon(line);
					if (!value.empty()) return value;
				}

				if (newline == std::string::npos) break;
				start = newline + 1;
			}

			return std::string();
		}

		static std::string hostOfReferral(const std::string& referral) {
			std::string host = referral;
			const std::size_t scheme = host.find("://");
			if (scheme != std::string::npos) host = host.substr(scheme + 3);
			host = host.substr(0, host.find_first_of("/ \t"));
			return host;
		}

		static bool isThinRegistry(const std::string& query_text) {
			const std::string lower = lowerAscii(query_text);
			const std::size_t dot = lower.rfind('.');
			if (dot == std::string::npos) return false;
			const std::string tld = lower.substr(dot + 1);
			return tld == "com" || tld == "net" || tld == "jobs";
		}

		static void emit(const std::string& host, const std::string& response, bool verbose) {
			if (verbose) std::fprintf(stderr, "[Querying %s]\n", host.c_str());
			std::fputs(response.c_str(), stdout);
			if (!response.empty() && response.back() != '\n') std::fputc('\n', stdout);
		}

		static int queryDirect(const WhoisOptions& options) {
			std::string response;
			if (!query(options.host, options.port, options.query, response)) return 1;
			emit(options.host, response, false);
			std::fflush(stdout);
			return 0;
		}

		static std::string chooseServer(const WhoisOptions& options, bool& have_server) {
			std::string iana_response;
			if (!query(kIanaServer, "43", options.query, iana_response)) {
				have_server = false;
				return std::string();
			}

			if (options.iana_only) {
				emit(kIanaServer, iana_response, false);
				have_server = false;
				return std::string();
			}

			have_server = true;
			return hostOfReferral(findReferral(iana_response, false));
		}

		static int followRegistrar(const WhoisOptions& options, const std::string& response) {
			if (!isThinRegistry(options.query)) return 0;

			const std::string registrar = hostOfReferral(findReferral(response, true));
			if (registrar.empty() || lowerAscii(registrar) == lowerAscii(options.host)) return 0;

			std::string registrar_response;
			if (!query(registrar, "43", options.query, registrar_response)) return 0;
			emit(registrar, registrar_response, false);
			return 0;
		}

		static int queryWithReferral(const WhoisOptions& options) {
			bool have_server = false;
			const std::string server = chooseServer(options, have_server);
			if (!have_server) return 0;
			if (server.empty()) {
				perr("whois", "no referral server for " + options.query);
				return 1;
			}

			WhoisOptions resolved = options;
			resolved.host = server;
			std::string response;
			if (!query(server, "43", options.query, response)) return 1;

			emit(server, response, false);
			followRegistrar(resolved, response);
			std::fflush(stdout);
			return 0;
		}

	}  // namespace whois_detail

	static int builtin_whois(Executor&, const std::vector<std::string>& args) {
		whois_detail::WhoisOptions options;
		if (!whois_detail::parse(args, options)) return 1;

		const ScopedCtrlCCapture capture;
		if (!options.host.empty()) return whois_detail::queryDirect(options);
		return whois_detail::queryWithReferral(options);
	}

	void registerWhoisBuiltin(Executor& exec) {
		exec.registerBuiltin("whois", builtin_whois);
	}

#else

	void registerWhoisBuiltin(Executor&) {}

#endif /* _WIN32 */

}  // namespace wbsh

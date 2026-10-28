#include "curl_internal.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "coreutils_internal.h"
#include "executor.h"
#include "numparse.h"
#include "pathconv.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <winhttp.h>
#  pragma comment(lib, "winhttp.lib")
#endif /* _WIN32 */

namespace wbsh {

	namespace curl_detail {

		bool isRetryableStatus(long status) {
			return status == 408 || status == 429 || status == 500 || status == 502 || status == 503 || status == 504;
		}

		static double secondsSince(std::chrono::steady_clock::time_point start) {
			return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		}

		static int fail(Transfer& transfer, int code, const std::string& message) {
			transfer.info.error = message;
			return code;
		}

		static int sinkFailure(Transfer& transfer) {
			if (!transfer.sink.open_error.empty()) return fail(transfer, kExitWrite, transfer.sink.open_error);
			return fail(transfer, kExitWrite, "Failure writing output to destination");
		}

		static void verboseLines(const std::string& text, const char* prefix) {
			std::size_t start = 0;
			while (start < text.size()) {
				std::size_t end = text.find('\n', start);
				if (end == std::string::npos) end = text.size();
				std::string line = text.substr(start, end - start);
				if (!line.empty() && line.back() == '\r') line.pop_back();
				std::fprintf(stderr, "%s%s\n", prefix, line.c_str());
				start = end + 1;
			}
		}

#ifdef _WIN32
		static const DWORD kNulTerminated = static_cast<DWORD>(-1L);
		static const DWORD kDefaultConnectMs = 300000;
		static const std::size_t kChunkBytes = 64 * 1024;

		// The request handle may be closed by the --max-time watchdog thread,
		// which is how a blocked synchronous WinHTTP call gets cancelled.
		struct Handles {
			HINTERNET session = nullptr;
			HINTERNET connect = nullptr;
			std::atomic<HINTERNET> request{ nullptr };
			std::mutex guard;

			void cancelRequest() {
				std::lock_guard<std::mutex> lock(guard);
				const HINTERNET handle = request.exchange(nullptr);
				if (handle != nullptr) ::WinHttpCloseHandle(handle);
			}

			void closeHop() {
				cancelRequest();
				if (connect != nullptr) ::WinHttpCloseHandle(connect);
				connect = nullptr;
			}

			~Handles() {
				closeHop();
				if (session != nullptr) ::WinHttpCloseHandle(session);
			}
		};

		class Watchdog {
		public:
			void start(Handles& handles, std::chrono::steady_clock::time_point deadline) {
				thread_ = std::thread([this, &handles, deadline]() {
					std::unique_lock<std::mutex> lock(mutex_);
					if (!wake_.wait_until(lock, deadline, [this]() { return finished_; })) handles.cancelRequest();
				});
			}

			~Watchdog() {
				markFinished();
				wake_.notify_all();
				if (thread_.joinable()) thread_.join();
			}

		private:
			void markFinished() {
				std::lock_guard<std::mutex> lock(mutex_);
				finished_ = true;
			}

			std::thread thread_;
			std::mutex mutex_;
			std::condition_variable wake_;
			bool finished_ = false;
		};

		struct Hop {
			std::string url;
			UrlParts parts;
			std::string method;
			bool send_body = true;
			bool send_auth = true;
		};

		static std::string envFirst(Executor& exec, const char* lower, const char* upper) {
			const std::string value = exec.env().get(lower);
			return value.empty() && upper != nullptr ? exec.env().get(upper) : value;
		}

		static std::string proxyFor(Transfer& transfer, const std::string& scheme) {
			if (transfer.options.proxy_set) return transfer.options.proxy;

			std::string proxy = scheme == "https" ? envFirst(transfer.exec, "https_proxy", "HTTPS_PROXY")
				: envFirst(transfer.exec, "http_proxy", nullptr);
			if (proxy.empty()) proxy = envFirst(transfer.exec, "all_proxy", "ALL_PROXY");
			return proxy;
		}

		static std::wstring proxyName(std::string proxy) {
			const std::size_t scheme = proxy.find("://");
			if (scheme != std::string::npos) proxy = proxy.substr(scheme + 3);

			const std::size_t at = proxy.rfind('@');
			if (at != std::string::npos) proxy = proxy.substr(at + 1);
			while (!proxy.empty() && proxy.back() == '/') proxy.pop_back();
			return utf8ToWide(proxy);
		}

		static std::wstring bypassList(Transfer& transfer) {
			std::string list = transfer.options.noproxy_set ? transfer.options.noproxy
				: envFirst(transfer.exec, "no_proxy", "NO_PROXY");
			std::replace(list.begin(), list.end(), ',', ';');
			list.erase(std::remove(list.begin(), list.end(), ' '), list.end());

			// WinHTTP never proxies loopback hosts unless told to; curl does.
			return utf8ToWide(list.empty() ? "<-loopback>" : list + ";<-loopback>");
		}

		static DWORD millis(double seconds) {
			return static_cast<DWORD>((std::min)(seconds * 1000.0, 2000000000.0));
		}

		// Milliseconds left of --max-time, 0 for "no limit", -1 once it ran out.
		static long long remainingMs(const Transfer& transfer) {
			if (transfer.options.max_time <= 0) return 0;

			const double left = transfer.options.max_time - secondsSince(transfer.started);
			return left <= 0 ? -1 : (std::max<long long>)(1, static_cast<long long>(left * 1000.0));
		}

		static void applyTimeouts(Transfer& transfer, HINTERNET handle) {
			const double connect_seconds = transfer.options.connect_timeout;
			DWORD connect = connect_seconds > 0 ? millis(connect_seconds) : kDefaultConnectMs;
			const long long left = remainingMs(transfer);
			if (left > 0) connect = (std::min)(connect, static_cast<DWORD>(left));
			::WinHttpSetTimeouts(handle, static_cast<int>(connect), static_cast<int>(connect), 0, 0);

			DWORD response = INFINITE;
			::WinHttpSetOption(handle, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &response, sizeof(response));
		}

		static bool openSession(Transfer& transfer, const std::string& scheme, Handles& handles) {
			const std::string proxy = proxyFor(transfer, scheme);
			if (proxy.empty()) {
				handles.session = ::WinHttpOpen(nullptr, WINHTTP_ACCESS_TYPE_NO_PROXY,
					WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
			} else {
				const std::wstring name = proxyName(proxy);
				const std::wstring bypass = bypassList(transfer);
				handles.session = ::WinHttpOpen(nullptr, WINHTTP_ACCESS_TYPE_NAMED_PROXY, name.c_str(),
					bypass.c_str(), 0);
			}

			if (handles.session == nullptr) return false;
			applyTimeouts(transfer, handles.session);
			return true;
		}

		static int timeoutFailure(Transfer& transfer);

		static int mapError(Transfer& transfer, DWORD error, const Hop& hop, bool receiving) {
			if (remainingMs(transfer) < 0) return timeoutFailure(transfer);

			const std::string host = hop.parts.host;
			const std::string ms = std::to_string(static_cast<long long>(secondsSince(transfer.started) * 1000));
			switch (error) {
				case ERROR_WINHTTP_NAME_NOT_RESOLVED:
					if (!proxyFor(transfer, hop.parts.scheme).empty()) {
						const std::string proxy = proxyFor(transfer, hop.parts.scheme);
						return fail(transfer, kExitProxyResolve, "Could not resolve proxy: " + proxy);
					}
					return fail(transfer, kExitResolve, "Could not resolve host: " + host);
				case ERROR_WINHTTP_CANNOT_CONNECT:
					return fail(transfer, kExitConnect, "Failed to connect to " + host + " port "
						+ std::to_string(hop.parts.port) + " after " + ms + " ms: Couldn't connect to server");
				case ERROR_WINHTTP_TIMEOUT:
					return fail(transfer, kExitTimeout, "Operation timed out after " + ms + " milliseconds with "
						+ std::to_string(transfer.info.size_download) + " bytes received");
				case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
					return fail(transfer, kExitSslConnect, "SSL connect error");
				case ERROR_WINHTTP_SECURE_FAILURE:
				case ERROR_WINHTTP_SECURE_INVALID_CA:
				case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
				case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
				case ERROR_WINHTTP_SECURE_INVALID_CERT:
				case ERROR_WINHTTP_SECURE_CERT_REVOKED:
				case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
					return fail(transfer, kExitSslCert, "SSL certificate problem: unable to verify the certificate");
				case ERROR_WINHTTP_CONNECTION_ERROR:
				case ERROR_WINHTTP_INVALID_SERVER_RESPONSE:
					if (receiving) return fail(transfer, kExitEmptyReply, "Empty reply from server");
					return fail(transfer, kExitSend, "Failed sending data to the peer");
				case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
					return fail(transfer, kExitUnsupportedProtocol,
						"Protocol \"" + hop.parts.scheme + "\" not supported");
				case ERROR_WINHTTP_INVALID_URL:
					return fail(transfer, kExitMalformedUrl, "URL rejected: Malformed input to a URL function");
				default:
					return fail(transfer, kExitReceive, "Failure when receiving data from the peer (WinHTTP error "
						+ std::to_string(error) + ")");
			}
		}

		static void setDwordOption(HINTERNET handle, DWORD option, DWORD value) {
			::WinHttpSetOption(handle, option, &value, sizeof(value));
		}

		static void configureRequest(Transfer& transfer, HINTERNET request) {
			const DWORD disabled = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_REDIRECTS;
			setDwordOption(request, WINHTTP_OPTION_DISABLE_FEATURE, disabled);
			if (transfer.options.insecure) {
				setDwordOption(request, WINHTTP_OPTION_SECURITY_FLAGS, SECURITY_FLAG_IGNORE_UNKNOWN_CA
					| SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_CN_INVALID
					| SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE);
			}

			if (transfer.options.compressed) {
				setDwordOption(request, WINHTTP_OPTION_DECOMPRESSION, WINHTTP_DECOMPRESSION_FLAG_ALL);
			}

			applyTimeouts(transfer, request);
		}

		static std::wstring hostForConnect(const std::string& host) {
			const bool bracketed = host.size() > 2 && host.front() == '[' && host.back() == ']';
			if (bracketed) return utf8ToWide(host.substr(1, host.size() - 2));
			return utf8ToWide(host);
		}

		static bool openHop(Transfer& transfer, Handles& handles, const Hop& hop) {
			handles.connect = ::WinHttpConnect(handles.session, hostForConnect(hop.parts.host).c_str(),
				static_cast<INTERNET_PORT>(hop.parts.port), 0);
			if (handles.connect == nullptr) return false;

			const std::wstring method = utf8ToWide(hop.method);
			const std::wstring path = utf8ToWide(hop.parts.path);
			const DWORD flags = hop.parts.scheme == "https" ? WINHTTP_FLAG_SECURE : 0;
			const HINTERNET request = ::WinHttpOpenRequest(handles.connect, method.c_str(), path.c_str(), nullptr,
				WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
			if (request == nullptr) return false;

			configureRequest(transfer, request);
			handles.request = request;
			return true;
		}

		static std::string headerName(const std::string& header) {
			const std::size_t end = header.find_first_of(":;");
			return end == std::string::npos ? header : header.substr(0, end);
		}

		static bool userHasHeader(const CurlOptions& options, const std::string& name) {
			for (const std::string& header : options.headers) {
				if (_stricmp(headerName(header).c_str(), name.c_str()) == 0) return true;
			}

			return false;
		}

		static std::string authorizationFor(const Transfer& transfer, const Hop& hop) {
			if (!hop.send_auth || userHasHeader(transfer.options, "Authorization")) return std::string();
			if (!transfer.options.user.empty()) return "Authorization: Basic " + base64Encode(transfer.options.user);
			if (hop.parts.user.empty()) return std::string();
			return "Authorization: Basic " + base64Encode(hop.parts.user + ":" + hop.parts.password);
		}

		static std::string cookieFor(const Transfer& transfer, const Request& request, const Hop& hop) {
			std::string cookie = request.cookie;
			if (!transfer.jar.enabled) return cookie;

			const bool https = hop.parts.scheme == "https";
			const std::string stored = cookieHeaderFor(transfer.jar, hop.parts.host, hop.parts.path, https);
			if (!stored.empty()) cookie += (cookie.empty() ? "" : "; ") + stored;
			return cookie;
		}

		static std::vector<std::string> generatedHeaders(const Transfer& transfer, const Request& request,
				const Hop& hop) {
			const CurlOptions& options = transfer.options;
			std::vector<std::string> headers;
			const std::string agent = options.user_agent_set ? options.user_agent : "curl/8.10.1";
			if (!agent.empty() && !userHasHeader(options, "User-Agent")) headers.push_back("User-Agent: " + agent);
			if (!userHasHeader(options, "Accept")) headers.push_back("Accept: */*");
			if (!options.referer.empty()) headers.push_back("Referer: " + options.referer);

			const std::string auth = authorizationFor(transfer, hop);
			if (!auth.empty()) headers.push_back(auth);

			const std::string cookie = cookieFor(transfer, request, hop);
			if (!cookie.empty()) headers.push_back("Cookie: " + cookie);

			if (request.resume_from > 0) headers.push_back("Range: bytes=" + std::to_string(request.resume_from) + "-");
			else if (!options.range.empty()) headers.push_back("Range: bytes=" + options.range);
			return headers;
		}

		static void addHeader(HINTERNET request, const std::string& header, DWORD flags) {
			const std::wstring wide = utf8ToWide(header + "\r\n");
			::WinHttpAddRequestHeaders(request, wide.c_str(), kNulTerminated, flags);
		}

		// curl's -H rules: "Name:" removes a header, "Name;" sends it empty.
		static void addUserHeader(HINTERNET request, const std::string& header) {
			const std::size_t colon = header.find(':');
			const bool removal = colon != std::string::npos
				&& header.find_first_not_of(" \t", colon + 1) == std::string::npos;
			if (removal) {
				addHeader(request, header.substr(0, colon + 1), WINHTTP_ADDREQ_FLAG_REPLACE);
				return;
			}

			if (colon == std::string::npos && !header.empty() && header.back() == ';') {
				addHeader(request, header.substr(0, header.size() - 1) + ":", WINHTTP_ADDREQ_FLAG_ADD);
				return;
			}

			addHeader(request, header, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
		}

		static void addHopHeaders(Transfer& transfer, HINTERNET handle, const Request& request, const Hop& hop) {
			for (const std::string& header : generatedHeaders(transfer, request, hop)) {
				addHeader(handle, header, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
			}

			for (const std::string& header : request.headers) addUserHeader(handle, header);
		}

		static std::string queryRawHeaders(HINTERNET request, DWORD which) {
			DWORD size = 0;
			::WinHttpQueryHeaders(request, which, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size,
				WINHTTP_NO_HEADER_INDEX);
			if (size == 0) return std::string();

			std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, 0);
			if (::WinHttpQueryHeaders(request, which, WINHTTP_HEADER_NAME_BY_INDEX, buffer.data(), &size,
			                          WINHTTP_NO_HEADER_INDEX) == 0) {
				return std::string();
			}

			return wideToUtf8(buffer.data());
		}

		static bool streamUpload(Transfer& transfer, HINTERNET request, const Request& upload) {
			std::FILE* file = fopenNative(transfer.exec, upload.upload_path, "rb");
			if (file == nullptr) return false;

			std::vector<char> buffer(kChunkBytes);
			std::size_t got = 0;
			bool ok = true;
			while (ok && (got = std::fread(buffer.data(), 1, buffer.size(), file)) > 0) {
				DWORD written = 0;
				ok = ::WinHttpWriteData(request, buffer.data(), static_cast<DWORD>(got), &written) != 0;
				transfer.info.size_upload += written;
			}

			std::fclose(file);
			return ok;
		}

		static bool sendHop(Transfer& transfer, HINTERNET handle, const Request& request, const Hop& hop) {
			const bool body = hop.send_body && request.has_body;
			const bool streamed = body && !request.upload_path.empty();
			const bool inline_body = body && !streamed && !request.body.empty();
			LPVOID data = inline_body ? const_cast<char*>(request.body.data()) : WINHTTP_NO_REQUEST_DATA;
			const DWORD inline_size = body && !streamed ? static_cast<DWORD>(request.body.size()) : 0;
			const DWORD total = streamed ? static_cast<DWORD>(request.upload_size) : inline_size;

			if (::WinHttpSendRequest(handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0, data, inline_size, total, 0) == 0) {
				return false;
			}

			if (!streamed) transfer.info.size_upload += inline_size;
			if (transfer.options.verbose) {
				std::fprintf(stderr, "* Connected to %s port %d\n", hop.parts.host.c_str(), hop.parts.port);
				const DWORD which = WINHTTP_QUERY_RAW_HEADERS_CRLF | WINHTTP_QUERY_FLAG_REQUEST_HEADERS;
				verboseLines(queryRawHeaders(handle, which), "> ");
			}

			return !streamed || streamUpload(transfer, handle, request);
		}

		static DWORD statusCode(HINTERNET request) {
			DWORD code = 0;
			DWORD size = sizeof(code);
			::WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
			return code;
		}

		static void recordResponse(Transfer& transfer, HINTERNET handle, const Hop& hop) {
			ResponseInfo& info = transfer.info;
			info.status = static_cast<long>(statusCode(handle));
			info.last_headers = queryRawHeaders(handle, WINHTTP_QUERY_RAW_HEADERS_CRLF);
			info.size_header += info.last_headers.size();
			info.effective_url = hop.url;
			info.method = hop.method;
			info.scheme = hop.parts.scheme;
			info.remote_host = hop.parts.host;
			info.remote_port = hop.parts.port;
			info.time_starttransfer = secondsSince(transfer.started);
			info.content_type.clear();
			findHeader(info.last_headers, "Content-Type", info.content_type);

			std::string location;
			const bool moved = findHeader(info.last_headers, "Location", location);
			info.redirect_url = moved ? resolveUrl(hop.url, location) : std::string();

			if (transfer.jar.enabled) {
				for (const std::string& value : findAllHeaders(info.last_headers, "Set-Cookie")) {
					storeSetCookie(transfer.jar, hop.parts.host, value);
				}

			}

			if (transfer.options.verbose) verboseLines(info.last_headers, "< ");
			if (transfer.header_dump != nullptr) std::fputs(info.last_headers.c_str(), transfer.header_dump);
		}

		static bool isRedirect(long status) {
			return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
		}

		static bool writeHeadersToOutput(Transfer& transfer) {
			if (!transfer.options.include && !transfer.options.head) return true;
			const std::string& raw = transfer.info.last_headers;
			return transfer.sink.write(raw.data(), raw.size());
		}

		static bool nextHop(Transfer& transfer, Hop& hop) {
			const long status = transfer.info.status;
			const std::string target = transfer.info.redirect_url;
			UrlParts parts;
			if (!splitUrl(target, parts)) return false;

			const bool same_host = parts.host == hop.parts.host && parts.port == hop.parts.port;
			const bool to_get = transfer.options.method.empty() && hop.method != "HEAD"
				&& (status == 303 || ((status == 301 || status == 302) && hop.method == "POST"));
			if (to_get) {
				hop.method = "GET";
				hop.send_body = false;
			}

			hop.send_auth = hop.send_auth && same_host;
			hop.url = target;
			hop.parts = parts;
			return true;
		}

		static int readFailure(Transfer& transfer, const Hop& hop) {
			const DWORD error = ::GetLastError();
			if (remainingMs(transfer) < 0) return timeoutFailure(transfer);
			if (error == ERROR_WINHTTP_CONNECTION_ERROR || error == ERROR_WINHTTP_INVALID_SERVER_RESPONSE) {
				return fail(transfer, kExitPartial, "transfer closed with outstanding read data remaining");
			}

			return mapError(transfer, error, hop, true);
		}

		static bool expectsFullLength(const Transfer& transfer, unsigned long long& expected) {
			const ResponseInfo& info = transfer.info;
			if (info.status == 204 || info.status == 304) return false;

			std::string text;
			if (transfer.options.compressed && findHeader(info.last_headers, "Content-Encoding", text)) return false;
			if (!findHeader(info.last_headers, "Content-Length", text)) return false;
			return parseULL(text, expected);
		}

		static int partialFailure(Transfer& transfer, unsigned long long expected) {
			const std::string missing = std::to_string(expected - transfer.info.size_download);
			return fail(transfer, kExitPartial, "transfer closed with " + missing + " bytes remaining to read");
		}

		static int timeoutFailure(Transfer& transfer) {
			const std::string ms = std::to_string(static_cast<long long>(secondsSince(transfer.started) * 1000));
			return fail(transfer, kExitTimeout, "Operation timed out after " + ms + " milliseconds with "
				+ std::to_string(transfer.info.size_download) + " bytes received");
		}

		// WinHTTP reports a connection closed before Content-Length bytes as a
		// normal end of data, so the byte count is checked here.
		static int streamBody(Transfer& transfer, HINTERNET handle, const Hop& hop) {
			std::vector<char> buffer(kChunkBytes);
			for (;;) {
				if (remainingMs(transfer) < 0) return timeoutFailure(transfer);

				DWORD got = 0;
				if (::WinHttpReadData(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &got) == 0) {
					return readFailure(transfer, hop);
				}

				if (got == 0) break;
				if (!transfer.sink.write(buffer.data(), got)) return sinkFailure(transfer);
				transfer.info.size_download += got;
			}

			unsigned long long expected = 0;
			if (expectsFullLength(transfer, expected) && transfer.info.size_download < expected) {
				return partialFailure(transfer, expected);
			}

			return 0;
		}

		static int resumeCheck(Transfer& transfer, const Request& request, bool& done) {
			done = false;
			if (request.resume_from == 0) return 0;

			const long status = transfer.info.status;
			if (status == 416) {
				done = true;
				return 0;
			}

			if (status == 200) {
				return fail(transfer, kExitRange, "HTTP server doesn't seem to support byte ranges. Cannot resume.");
			}

			return 0;
		}

		static int failureResponse(Transfer& transfer, HINTERNET handle, const Hop& hop) {
			const std::string message = "The requested URL returned error: " + std::to_string(transfer.info.status);
			if (transfer.options.fail_with_body) {
				if (!writeHeadersToOutput(transfer)) return sinkFailure(transfer);
				const int status = streamBody(transfer, handle, hop);
				if (status != 0) return status;
			}

			return fail(transfer, kExitHttp, message);
		}

		static int finalResponse(Transfer& transfer, HINTERNET handle, const Request& request, const Hop& hop) {
			const long status = transfer.info.status;
			if ((transfer.options.fail || transfer.options.fail_with_body) && status >= 400) {
				return failureResponse(transfer, handle, hop);
			}

			if (transfer.retry_allowed && isRetryableStatus(status)) return kRetryStatus;

			bool done = false;
			const int resume = resumeCheck(transfer, request, done);
			if (resume != 0 || done) return resume;

			if (!writeHeadersToOutput(transfer)) return sinkFailure(transfer);
			if (hop.method == "HEAD") return 0;
			return streamBody(transfer, handle, hop);
		}

		static int exchange(Transfer& transfer, Handles& handles, const Request& request, const Hop& hop) {
			if (remainingMs(transfer) < 0) return timeoutFailure(transfer);
			if (!openHop(transfer, handles, hop)) return mapError(transfer, ::GetLastError(), hop, false);

			addHopHeaders(transfer, handles.request, request, hop);
			if (!sendHop(transfer, handles.request, request, hop)) {
				return mapError(transfer, ::GetLastError(), hop, false);
			}

			if (::WinHttpReceiveResponse(handles.request, nullptr) == 0) {
				return mapError(transfer, ::GetLastError(), hop, true);
			}

			recordResponse(transfer, handles.request, hop);
			return 0;
		}

		static int malformedUrl(Transfer& transfer) {
			return fail(transfer, kExitMalformedUrl, "URL rejected: Malformed input to a URL function");
		}

		static int followRedirect(Transfer& transfer, Hop& hop) {
			const long long limit = transfer.options.max_redirs;
			if (limit >= 0 && transfer.info.num_redirects >= limit) {
				const std::string count = std::to_string(limit);
				return fail(transfer, kExitTooManyRedirects, "Maximum (" + count + ") redirects followed");
			}

			if (!writeHeadersToOutput(transfer)) return sinkFailure(transfer);
			if (!nextHop(transfer, hop)) return malformedUrl(transfer);

			++transfer.info.num_redirects;
			return -1;
		}

		static bool firstHop(Transfer& transfer, const Request& request, Hop& hop) {
			hop.url = request.url;
			hop.method = request.method;
			transfer.info.url = request.url;
			return splitUrl(request.url, hop.parts);
		}

		int performHttp(Transfer& transfer, Request& request) {
			Hop hop;
			if (!firstHop(transfer, request, hop)) return malformedUrl(transfer);

			Handles handles;
			Watchdog watchdog;
			if (transfer.options.max_time > 0) {
				using Clock = std::chrono::steady_clock;
				const auto budget = std::chrono::duration<double>(transfer.options.max_time);
				watchdog.start(handles, transfer.started + std::chrono::duration_cast<Clock::duration>(budget));
			}

			if (!openSession(transfer, hop.parts.scheme, handles)) {
				return mapError(transfer, ::GetLastError(), hop, false);
			}

			for (;;) {
				const int status = exchange(transfer, handles, request, hop);
				if (status != 0) return status;

				const bool redirected = isRedirect(transfer.info.status) && !transfer.info.redirect_url.empty();
				const bool follow = transfer.options.location && redirected;
				if (!follow) return finalResponse(transfer, handles.request, request, hop);

				const int redirect = followRedirect(transfer, hop);
				if (redirect >= 0) return redirect;
				handles.closeHop();
			}
		}
#else
		int performHttp(Transfer& transfer, Request&) {
			return fail(transfer, kExitUnsupportedProtocol, "HTTP is not supported on this platform");
		}
#endif /* _WIN32 */

		static std::string localPath(const std::string& url) {
			std::string path = url.substr(std::strlen("file://"));
			if (path.rfind("localhost/", 0) == 0) path = path.substr(std::strlen("localhost"));
			const bool drive = path.size() > 2 && path[0] == '/'
				&& std::isalpha(static_cast<unsigned char>(path[1])) && path[2] == ':';
			return drive ? path.substr(1) : path;
		}

		static int copyLocalFile(Transfer& transfer, std::FILE* file) {
			std::vector<char> buffer(64 * 1024);
			std::size_t got = 0;
			while ((got = std::fread(buffer.data(), 1, buffer.size(), file)) > 0) {
				if (!transfer.sink.write(buffer.data(), got)) return sinkFailure(transfer);
				transfer.info.size_download += got;
			}

			return 0;
		}

		int performFile(Transfer& transfer, const Request& request) {
			transfer.info.url = request.url;
			transfer.info.effective_url = request.url;
			transfer.info.scheme = "file";

			const std::string path = localPath(request.url);
			std::FILE* file = fopenNative(transfer.exec, path, "rb");
			if (file == nullptr) return fail(transfer, kExitFileRead, "Couldn't open file " + path);

			int status = 0;
			if (transfer.options.head) {
				std::fseek(file, 0, SEEK_END);
				const std::string size = std::to_string(std::ftell(file));
				const std::string headers = "Content-Length: " + size + "\r\nAccept-ranges: bytes\r\n\r\n";
				if (!transfer.sink.write(headers.data(), headers.size())) status = sinkFailure(transfer);
			} else {
				std::fseek(file, static_cast<long>(request.resume_from), SEEK_SET);
				status = copyLocalFile(transfer, file);
			}

			std::fclose(file);
			transfer.info.time_total = secondsSince(transfer.started);
			return status;
		}

	}  // namespace curl_detail

}  // namespace wbsh

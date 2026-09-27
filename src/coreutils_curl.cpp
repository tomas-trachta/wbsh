/**
 * @file coreutils_curl.cpp
 * @brief Bundled `curl` builtin (basic HTTP/HTTPS client).
 *
 * Speaks just enough of curl's CLI to be useful interactively and in
 * scripts: -X, -H, -d, -o, -O, -L, -I, -i, -s and their `--long` aliases.
 * Uses WinHTTP on Windows; on other platforms the builtin returns an
 * error.
 */

#include "coreutils_internal.h"

#include <cstdio>
#include <string>
#include <vector>

#include "executor.h"
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

		// curl's own exit status for an HTTP error response.
		static const int kHttpFailureStatus = 22;

		static void curlPerr(const std::string& msg) {
			std::fprintf(stderr, "wbsh: curl: %s\n", msg.c_str());
		}

#ifdef _WIN32
		static const wchar_t kUserAgent[] = L"wbsh-curl/0.1";
		static const std::size_t kHostBufferChars = 256;
		static const std::size_t kPathBufferChars = 2048;
		// WinHttpAddRequestHeaders takes -1 for a NUL-terminated header string.
		static const DWORD kNulTerminated = static_cast<DWORD>(-1L);

		struct CurlOptions {
			bool silent = false;
			bool include_headers = false;
			bool head_only = false;
			bool save_remote = false;
			std::string output_file;
			std::string method = "GET";
			std::string body_data;
			std::vector<std::string> headers;
			std::string url;
		};

		struct CrackedUrl {
			std::wstring host;
			INTERNET_PORT port = 0;
			bool https = false;
			std::wstring path = L"/";
		};

		struct WinHttpHandles {
			HINTERNET session = nullptr;
			HINTERNET connect = nullptr;
			HINTERNET request = nullptr;
			~WinHttpHandles() {
				if (request != nullptr) ::WinHttpCloseHandle(request);
				if (connect != nullptr) ::WinHttpCloseHandle(connect);
				if (session != nullptr) ::WinHttpCloseHandle(session);
			}
		};

		static void setBodyData(CurlOptions& options, const std::string& data) {
			options.body_data = data;
			if (options.method == "GET") options.method = "POST";
		}

		static void parseShortFlag(char flag, bool last, const std::vector<std::string>& args,
		                           std::size_t& i, CurlOptions& options) {
			const bool has_value = last && i + 1 < args.size();
			switch (flag) {
			case 's': options.silent = true; break;
			case 'i': options.include_headers = true; break;
			case 'I': options.head_only = true; options.method = "HEAD"; break;
			case 'L': break;   // accept but no-op (we always follow redirects)
			case 'O': options.save_remote = true; break;
			case 'o': if (has_value) options.output_file = args[++i]; break;
			case 'X': if (has_value) options.method = args[++i]; break;
			case 'H': if (has_value) options.headers.push_back(args[++i]); break;
			case 'd': if (has_value) setBodyData(options, args[++i]); break;
			default: break;
			}
		}

		static CurlOptions parseArgs(const std::vector<std::string>& args) {
			CurlOptions options;
			for (std::size_t i = 0; i < args.size(); ++i) {
				const std::string& arg = args[i];
				if (arg == "--silent")      { options.silent          = true; continue; }
				if (arg == "--include")     { options.include_headers = true; continue; }
				if (arg == "--remote-name") { options.save_remote     = true; continue; }
				if (arg == "--location")    continue;
				if (arg == "--head") {
					options.head_only = true;
					options.method = "HEAD";
					continue;
				}

				const bool has_value = i + 1 < args.size();
				if (arg == "--output" && has_value) {
					options.output_file = args[++i];
					continue;
				}

				if (arg == "--request" && has_value) {
					options.method = args[++i];
					continue;
				}

				if (arg == "--header" && has_value) {
					options.headers.push_back(args[++i]);
					continue;
				}

				if (arg == "--data" && has_value) {
					setBodyData(options, args[++i]);
					continue;
				}

				if (arg.size() > 1 && arg[0] == '-' && arg[1] != '-') {
					for (std::size_t k = 1; k < arg.size(); ++k) {
						parseShortFlag(arg[k], k + 1 == arg.size(), args, i, options);
					}

					continue;
				}

				if (!arg.empty() && arg[0] != '-') options.url = arg;
			}

			return options;
		}

		static bool crackHttpUrl(const std::string& url, CrackedUrl& out) {
			wchar_t host_buffer[kHostBufferChars] = {};
			wchar_t path_buffer[kPathBufferChars] = {};

			URL_COMPONENTS components{};
			components.dwStructSize = sizeof(components);
			components.lpszHostName = host_buffer;
			components.dwHostNameLength = static_cast<DWORD>(kHostBufferChars);
			components.lpszUrlPath = path_buffer;
			components.dwUrlPathLength = static_cast<DWORD>(kPathBufferChars);

			const std::wstring wide_url = utf8ToWide(url);
			if (::WinHttpCrackUrl(wide_url.c_str(), 0, 0, &components) == 0) return false;

			out.host = host_buffer;
			out.https = (components.nScheme == INTERNET_SCHEME_HTTPS);
			out.port = components.nPort;
			out.path = L"/";
			if (components.dwUrlPathLength != 0) {
				out.path.assign(components.lpszUrlPath, components.dwUrlPathLength);
			}

			return true;
		}

		static void addRequestHeaders(HINTERNET request, const std::vector<std::string>& headers) {
			for (const auto& header : headers) {
				const std::wstring wide_header = utf8ToWide(header + "\r\n");
				::WinHttpAddRequestHeaders(request, wide_header.c_str(), kNulTerminated,
					WINHTTP_ADDREQ_FLAG_ADD);
			}
		}

		static bool openRequest(const CurlOptions& options, const CrackedUrl& url,
		                        WinHttpHandles& handles) {
			handles.session = ::WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
				WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
			if (handles.session == nullptr) {
				if (!options.silent) curlPerr("WinHttpOpen failed");
				return false;
			}

			handles.connect = ::WinHttpConnect(handles.session, url.host.c_str(), url.port, 0);
			if (handles.connect == nullptr) {
				if (!options.silent) curlPerr("connect failed");
				return false;
			}

			const std::wstring method = utf8ToWide(options.method);
			const DWORD flags = url.https ? WINHTTP_FLAG_SECURE : 0;
			handles.request = ::WinHttpOpenRequest(handles.connect, method.c_str(),
				url.path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
			if (handles.request == nullptr) {
				if (!options.silent) curlPerr("open request failed");
				return false;
			}

			addRequestHeaders(handles.request, options.headers);
			return true;
		}

		static bool sendRequest(HINTERNET request, const CurlOptions& options) {
			LPVOID body = WINHTTP_NO_REQUEST_DATA;
			if (!options.body_data.empty()) body = const_cast<char*>(options.body_data.data());
			const DWORD body_size = static_cast<DWORD>(options.body_data.size());

			const BOOL sent = ::WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
				body, body_size, body_size, 0);
			if (sent != 0 && ::WinHttpReceiveResponse(request, nullptr) != 0) return true;

			const DWORD err = ::GetLastError();
			if (!options.silent) {
				std::fprintf(stderr, "wbsh: curl: request failed (err=%lu)\n", err);
			}

			return false;
		}

		static DWORD readStatusCode(HINTERNET request) {
			DWORD status_code = 0;
			DWORD size = sizeof(status_code);
			::WinHttpQueryHeaders(request,
				WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX,
				&status_code, &size, WINHTTP_NO_HEADER_INDEX);
			return status_code;
		}

		static std::string remoteFileName(const std::wstring& url_path) {
			const std::string path = wideToUtf8(url_path);
			const std::size_t slash = path.find_last_of('/');
			std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
			if (name.empty() || name == "/") name = "index.html";
			return name;
		}

		static FILE* openOutputSink(Executor& exec, CurlOptions& options,
		                            const std::wstring& url_path) {
			if (options.save_remote) options.output_file = remoteFileName(url_path);
			if (options.output_file.empty()) return stdout;

			FILE* out = openUtf8(exec.pathConv().toWin32(options.output_file), "wb");
			if (out == nullptr && !options.silent) curlPerr(options.output_file + ": cannot open");
			return out;
		}

		static void writeResponseHeaders(HINTERNET request, FILE* out) {
			DWORD header_bytes = 0;
			::WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
				WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &header_bytes, WINHTTP_NO_HEADER_INDEX);
			if (header_bytes == 0) return;

			std::vector<wchar_t> headers(header_bytes / sizeof(wchar_t) + 1);
			const BOOL queried = ::WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
				WINHTTP_HEADER_NAME_BY_INDEX, headers.data(), &header_bytes,
				WINHTTP_NO_HEADER_INDEX);
			if (queried == 0) return;

			const int length = ::WideCharToMultiByte(CP_UTF8, 0, headers.data(), -1,
				nullptr, 0, nullptr, nullptr);
			std::string utf8(length, '\0');
			::WideCharToMultiByte(CP_UTF8, 0, headers.data(), -1, utf8.data(), length,
				nullptr, nullptr);

			const std::size_t without_nul = utf8.empty() ? 0 : utf8.size() - 1;
			std::fwrite(utf8.data(), 1, without_nul, out);
		}

		static void streamResponseBody(HINTERNET request, FILE* out) {
			for (;;) {
				DWORD available = 0;
				if (::WinHttpQueryDataAvailable(request, &available) == 0) return;
				if (available == 0) return;

				std::vector<char> buffer(available);
				DWORD got = 0;
				if (::WinHttpReadData(request, buffer.data(), available, &got) == 0) return;
				if (got == 0) return;

				std::fwrite(buffer.data(), 1, got, out);
			}
		}
#endif /* _WIN32 */

		static int builtin_curl(Executor& exec, const std::vector<std::string>& args) {
#ifndef _WIN32
			(void)exec;
			(void)args;
			curlPerr("not supported");
			return 1;
#else
			CurlOptions options = parseArgs(args);
			if (options.url.empty()) {
				if (!options.silent) curlPerr("no URL specified");
				return 2;
			}

			if (options.url.find("://") == std::string::npos) {
				options.url = "https://" + options.url;
			}

			CrackedUrl url;
			if (!crackHttpUrl(options.url, url)) {
				if (!options.silent) curlPerr("bad URL");
				return 1;
			}

			WinHttpHandles handles;
			if (!openRequest(options, url, handles)) return 1;
			if (!sendRequest(handles.request, options)) return 1;

			const DWORD status_code = readStatusCode(handles.request);
			FILE* out = openOutputSink(exec, options, url.path);
			if (out == nullptr) return 1;

			const bool with_headers = options.include_headers || options.head_only;
			if (with_headers) writeResponseHeaders(handles.request, out);
			if (!options.head_only) streamResponseBody(handles.request, out);

			if (out != stdout) std::fclose(out);
			std::fflush(stdout);
			if (status_code >= 400) return kHttpFailureStatus;
			return 0;
#endif /* _WIN32 */
		}

	}  // namespace curl_detail

	void registerCurlBuiltin(Executor& exec) {
		exec.registerBuiltin("curl", curl_detail::builtin_curl);
	}

}  // namespace wbsh

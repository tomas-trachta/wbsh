/**
 * @file coreutils_curl.cpp
 * @brief Bundled `curl` builtin (basic HTTP/HTTPS client).
 *
 * Speaks just enough of curl's CLI to be useful interactively and in
 * scripts: -X, -H, -d, -o, -O, -L, -I, -i, -s and their `--long` aliases.
 * Uses WinHTTP on Windows; on other platforms the builtin returns an
 * error.
 *
 * Split out of coreutils.cpp so the WinHTTP-heavy implementation is not
 * mixed into the generic file-handling code. The single public entry
 * point is registerCurlBuiltin().
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
#endif

namespace wbsh {

	namespace curl_detail {

		static void curlPerr(const std::string& msg) {
			std::fprintf(stderr, "wbsh: curl: %s\n", msg.c_str());
		}

#ifdef _WIN32
		static std::wstring utf8ToWideLocal(const std::string& s) {
			if (s.empty()) return {};
			const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
				static_cast<int>(s.size()), nullptr, 0);
			std::wstring r(n, L'\0');
			MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
				r.data(), n);
			return r;
		}

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

		struct WinHttpHandles {
			HINTERNET session = nullptr;
			HINTERNET connect = nullptr;
			HINTERNET request = nullptr;
			~WinHttpHandles() {
				if (request) WinHttpCloseHandle(request);
				if (connect) WinHttpCloseHandle(connect);
				if (session) WinHttpCloseHandle(session);
			}
		};

		// Apply one short-flag char from a `-XYZ` cluster. Value-taking flags
		// (`-o`, `-X`, `-H`, `-d`) must be the last char in the cluster; if
		// so, they consume `args[*i + 1]` and bump `*i`.
		static void parseShortFlag(char c, bool last,
		                           const std::vector<std::string>& args,
		                           std::size_t* i, CurlOptions& o)
		{
			switch (c) {
			case 's': o.silent = true; break;
			case 'i': o.include_headers = true; break;
			case 'I': o.head_only = true; o.method = "HEAD"; break;
			case 'L': break;   // accept but no-op (we always follow redirects)
			case 'O': o.save_remote = true; break;
			case 'o':
				if (last && *i + 1 < args.size()) o.output_file = args[++(*i)];
				break;
			case 'X':
				if (last && *i + 1 < args.size()) o.method = args[++(*i)];
				break;
			case 'H':
				if (last && *i + 1 < args.size()) o.headers.push_back(args[++(*i)]);
				break;
			case 'd':
				if (last && *i + 1 < args.size()) {
					o.body_data = args[++(*i)];
					if (o.method == "GET") o.method = "POST";
				}
				break;
			default:
				break;
			}
		}

		static CurlOptions parseArgs(const std::vector<std::string>& args) {
			CurlOptions o;
			for (std::size_t i = 0; i < args.size(); ++i) {
				const std::string& a = args[i];
				// Long forms.
				if (a == "--silent")       { o.silent          = true; continue; }
				if (a == "--include")      { o.include_headers = true; continue; }
				if (a == "--head")         { o.head_only = true; o.method = "HEAD"; continue; }
				if (a == "--location")     continue;
				if (a == "--remote-name")  { o.save_remote = true; continue; }
				if (a == "--output"  && i + 1 < args.size()) {
					o.output_file = args[++i]; continue;
				}
				if (a == "--request" && i + 1 < args.size()) {
					o.method = args[++i]; continue;
				}
				if (a == "--header"  && i + 1 < args.size()) {
					o.headers.push_back(args[++i]); continue;
				}
				if (a == "--data"    && i + 1 < args.size()) {
					o.body_data = args[++i];
					if (o.method == "GET") o.method = "POST";
					continue;
				}
				// Short-flag cluster.
				if (a.size() > 1 && a[0] == '-' && a[1] != '-') {
					for (std::size_t k = 1; k < a.size(); ++k) {
						parseShortFlag(a[k], k + 1 == a.size(), args, &i, o);
					}
					continue;
				}
				// Positional URL.
				if (!a.empty() && a[0] != '-') o.url = a;
			}
			return o;
		}

		// Crack `url` into host buffer and request path. Returns false on
		// parse failure; on true, `*is_https` and `*path` are filled.
		static bool crackHttpUrl(const std::string& url,
		                         wchar_t* host_buf, std::size_t host_buf_n,
		                         INTERNET_PORT* out_port, bool* is_https,
		                         std::wstring* out_path)
		{
			URL_COMPONENTS uc{};
			uc.dwStructSize = sizeof(uc);
			wchar_t path_buf[2048] = {};
			uc.lpszHostName = host_buf;
			uc.dwHostNameLength = static_cast<DWORD>(host_buf_n);
			uc.lpszUrlPath = path_buf;
			uc.dwUrlPathLength = sizeof(path_buf) / sizeof(path_buf[0]);

			const std::wstring wurl = utf8ToWideLocal(url);
			if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return false;

			*is_https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
			*out_port = uc.nPort;
			*out_path = uc.dwUrlPathLength
				? std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength)
				: std::wstring(L"/");
			return true;
		}

		// Open the WinHTTP session / connection / request handles and apply
		// custom headers from `o`. On failure prints a diagnostic (when not
		// silent) and returns false.
		static bool openRequest(const CurlOptions& o, WinHttpHandles& h) {
			wchar_t host_buf[256] = {};
			INTERNET_PORT port = 0;
			bool is_https = false;
			std::wstring path;
			if (!crackHttpUrl(o.url, host_buf,
			                  sizeof(host_buf) / sizeof(host_buf[0]),
			                  &port, &is_https, &path))
			{
				if (!o.silent) curlPerr("bad URL");
				return false;
			}

			h.session = WinHttpOpen(L"wbsh-curl/0.1",
				WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
				WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
			if (!h.session) {
				if (!o.silent) curlPerr("WinHttpOpen failed");
				return false;
			}
			h.connect = WinHttpConnect(h.session, host_buf, port, 0);
			if (!h.connect) {
				if (!o.silent) curlPerr("connect failed");
				return false;
			}
			const std::wstring wmethod = utf8ToWideLocal(o.method);
			h.request = WinHttpOpenRequest(h.connect, wmethod.c_str(),
				path.c_str(), nullptr, WINHTTP_NO_REFERER,
				WINHTTP_DEFAULT_ACCEPT_TYPES,
				is_https ? WINHTTP_FLAG_SECURE : 0);
			if (!h.request) {
				if (!o.silent) curlPerr("open request failed");
				return false;
			}
			for (const auto& hv : o.headers) {
				const std::wstring wh = utf8ToWideLocal(hv + "\r\n");
				WinHttpAddRequestHeaders(h.request, wh.c_str(),
					static_cast<DWORD>(-1L), WINHTTP_ADDREQ_FLAG_ADD);
			}
			return true;
		}

		// Send the prepared request. On failure prints a diagnostic (if not
		// silent) and returns false.
		static bool sendRequest(HINTERNET request, const CurlOptions& o) {
			const BOOL r = WinHttpSendRequest(request,
				WINHTTP_NO_ADDITIONAL_HEADERS, 0,
				o.body_data.empty()
					? WINHTTP_NO_REQUEST_DATA
					: const_cast<char*>(o.body_data.data()),
				static_cast<DWORD>(o.body_data.size()),
				static_cast<DWORD>(o.body_data.size()), 0);
			if (!r || !WinHttpReceiveResponse(request, nullptr)) {
				const DWORD err = GetLastError();
				if (!o.silent) {
					std::fprintf(stderr,
						"wbsh: curl: request failed (err=%lu)\n", err);
				}
				return false;
			}
			return true;
		}

		static DWORD readStatusCode(HINTERNET request) {
			DWORD status_code = 0;
			DWORD sz = sizeof(status_code);
			WinHttpQueryHeaders(request,
				WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX,
				&status_code, &sz, WINHTTP_NO_HEADER_INDEX);
			return status_code;
		}

		// Decide where to write the response body. Honours `--remote-name` (-O),
		// `--output` (-o), or stdout. On a remote-name save with no usable
		// basename, falls back to "index.html".
		static FILE* openOutputSink(Executor& exec, CurlOptions& o,
		                            const std::wstring& url_path)
		{
			if (o.save_remote) {
				std::string p(url_path.begin(), url_path.end());
				const auto slash = p.find_last_of('/');
				std::string name = (slash == std::string::npos) ? p : p.substr(slash + 1);
				if (name.empty() || name == "/") name = "index.html";
				o.output_file = std::move(name);
			}
			if (o.output_file.empty()) return stdout;
			FILE* out = openUtf8(exec.pathConv().toWin32(o.output_file), "wb");
			if (!out && !o.silent) curlPerr(o.output_file + ": cannot open");
			return out;
		}

		// Print the response's raw header block to `out` as UTF-8.
		static void writeResponseHeaders(HINTERNET request, FILE* out) {
			DWORD hdr_size = 0;
			WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
				WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &hdr_size,
				WINHTTP_NO_HEADER_INDEX);
			if (hdr_size == 0) return;
			std::vector<wchar_t> hbuf(hdr_size / sizeof(wchar_t) + 1);
			if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
				WINHTTP_HEADER_NAME_BY_INDEX,
				hbuf.data(), &hdr_size, WINHTTP_NO_HEADER_INDEX))
			{
				return;
			}
			const int n = WideCharToMultiByte(CP_UTF8, 0, hbuf.data(), -1,
				nullptr, 0, nullptr, nullptr);
			std::string utf(n, '\0');
			WideCharToMultiByte(CP_UTF8, 0, hbuf.data(), -1,
				utf.data(), n, nullptr, nullptr);
			std::fwrite(utf.data(), 1, utf.size() ? utf.size() - 1 : 0, out);
		}

		// Stream the response body to `out`, chunk by chunk.
		static void streamResponseBody(HINTERNET request, FILE* out) {
			while (true) {
				DWORD avail = 0;
				if (!WinHttpQueryDataAvailable(request, &avail)) return;
				if (avail == 0) return;
				std::vector<char> buf(avail);
				DWORD got = 0;
				if (!WinHttpReadData(request, buf.data(), avail, &got)) return;
				if (got == 0) return;
				std::fwrite(buf.data(), 1, got, out);
			}
		}
#endif  // _WIN32

		static int builtin_curl(Executor& exec, const std::vector<std::string>& args) {
#ifndef _WIN32
			(void)exec; (void)args;
			curlPerr("not supported");
			return 1;
#else
			CurlOptions o = parseArgs(args);
			if (o.url.empty()) {
				if (!o.silent) curlPerr("no URL specified");
				return 2;
			}
			// Default to https:// if no scheme.
			if (o.url.find("://") == std::string::npos) o.url = "https://" + o.url;

			WinHttpHandles h;
			if (!openRequest(o, h)) return 1;
			if (!sendRequest(h.request, o)) return 1;

			const DWORD status_code = readStatusCode(h.request);

			// Re-crack the URL to get the path for `-O` (we don't keep the
			// cracked path past openRequest()).
			std::wstring path = L"/";
			{
				wchar_t host_buf[256] = {};
				INTERNET_PORT port = 0;
				bool https_unused = false;
				(void)crackHttpUrl(o.url, host_buf,
				                   sizeof(host_buf) / sizeof(host_buf[0]),
				                   &port, &https_unused, &path);
			}

			FILE* out = openOutputSink(exec, o, path);
			if (!out) return 1;

			if (o.include_headers || o.head_only) {
				writeResponseHeaders(h.request, out);
			}
			if (!o.head_only) {
				streamResponseBody(h.request, out);
			}

			if (out != stdout) std::fclose(out);
			std::fflush(stdout);
			return (status_code >= 400) ? 22 : 0;
#endif
		}

	}  // namespace curl_detail

	void registerCurlBuiltin(Executor& exec) {
		exec.registerBuiltin("curl", curl_detail::builtin_curl);
	}

}  // namespace wbsh

/**
 * @file coreutils_wget.cpp
 * @brief wget: download over HTTP(S) with WinHTTP — resumable, quiet or
 *        with a progress bar, to a derived name, a named file or stdout.
 *
 * Redirects are followed by WinHTTP itself. Exit codes follow GNU wget:
 * 1 generic, 2 bad usage, 3 file I/O, 4 network, 5 TLS, 8 server error.
 */

#include "coreutils_internal.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <winhttp.h>
#  pragma comment(lib, "winhttp.lib")
#endif /* _WIN32 */

#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "executor.h"
#include "numparse.h"
#include "pathconv.h"

namespace wbsh {

#ifdef _WIN32

	namespace fs = std::filesystem;

	namespace wget_detail {

		struct WgetOptions {
			bool quiet = false;
			bool resume = false;
			bool server_response = false;
			bool spider = false;
			bool insecure = false;
			int timeout_seconds = 0;
			std::string output;
			std::string directory;
			std::string user_agent = "Wget/1.21 (wbsh)";
			std::string post_data;
			bool post = false;
			std::vector<std::string> headers;
			std::vector<std::string> urls;
		};

		struct ParsedUrl {
			std::wstring host;
			std::wstring path = L"/";
			std::wstring user;
			std::wstring password;
			INTERNET_PORT port = 0;
			bool https = false;
		};

		struct HttpHandles {
			HINTERNET session = nullptr;
			HINTERNET connect = nullptr;
			HINTERNET request = nullptr;

			~HttpHandles() {
				if (request != nullptr) ::WinHttpCloseHandle(request);
				if (connect != nullptr) ::WinHttpCloseHandle(connect);
				if (session != nullptr) ::WinHttpCloseHandle(session);
			}
		};

		struct Progress {
			std::string name;
			unsigned long long total = 0;
			unsigned long long done = 0;
			unsigned long long resumed_from = 0;
			std::chrono::steady_clock::time_point started;
			std::chrono::steady_clock::time_point last_draw;
		};

	}  // namespace wget_detail

	using wget_detail::HttpHandles;
	using wget_detail::ParsedUrl;
	using wget_detail::Progress;
	using wget_detail::WgetOptions;

	static const int kExitUsage   = 2;
	static const int kExitFileIo  = 3;
	static const int kExitNetwork = 4;
	static const int kExitServer  = 8;
	static const int kBarWidth    = 30;
	static const DWORD kNulTerminated = static_cast<DWORD>(-1L);

	static bool takeWgetValue(const std::vector<std::string>& args, std::size_t& i,
	                          const char* short_flag, const char* long_flag, std::string& value) {
		const std::string& arg = args[i];
		const std::string long_prefix = std::string(long_flag) + "=";
		if (arg.compare(0, long_prefix.size(), long_prefix) == 0) {
			value = arg.substr(long_prefix.size());
			return true;
		}

		const bool short_attached = short_flag != nullptr && arg.size() > 2
			&& arg.compare(0, 2, short_flag) == 0;
		if (short_attached) {
			value = arg.substr(2);
			return true;
		}

		const bool named = arg == long_flag || (short_flag != nullptr && arg == short_flag);
		if (!named || i + 1 >= args.size()) return false;
		value = args[++i];
		return true;
	}

	static bool readPostFile(Executor& exec, const std::string& file, std::string& data) {
		std::FILE* in = fopenNative(exec, file, "rb");
		if (in == nullptr) return false;
		char chunk[8192];
		for (std::size_t got = 0; (got = std::fread(chunk, 1, sizeof(chunk), in)) != 0;) {
			data.append(chunk, got);
		}

		std::fclose(in);
		return true;
	}

	static bool applyWgetText(const std::vector<std::string>& args, std::size_t& i,
	                          WgetOptions& options) {
		std::string value;
		if (takeWgetValue(args, i, "-O", "--output-document", value)) {
			options.output = value;
			return true;
		}

		if (takeWgetValue(args, i, "-P", "--directory-prefix", value)) {
			options.directory = value;
			return true;
		}

		if (takeWgetValue(args, i, "-U", "--user-agent", value)) {
			options.user_agent = value;
			return true;
		}

		if (!takeWgetValue(args, i, nullptr, "--header", value)) return false;
		options.headers.push_back(value);
		return true;
	}

	static bool applyWgetValue(Executor& exec, const std::vector<std::string>& args,
	                           std::size_t& i, WgetOptions& options, bool& valid) {
		if (applyWgetText(args, i, options)) return true;

		std::string value;
		if (takeWgetValue(args, i, "-T", "--timeout", value)) {
			valid = parseInt(value, options.timeout_seconds);
			return true;
		}

		if (takeWgetValue(args, i, nullptr, "--post-data", value)) {
			options.post = true;
			options.post_data = value;
			return true;
		}

		if (takeWgetValue(args, i, nullptr, "--post-file", value)) {
			options.post = true;
			valid = readPostFile(exec, value, options.post_data);
			return true;
		}

		return takeWgetValue(args, i, "-t", "--tries", value);
	}

	// `-qO -` and `-cq` are how scripts usually spell it: split such a
	// cluster into its flags, the value-taking letter last.
	static std::vector<std::string> expandShortClusters(const std::vector<std::string>& args) {
		static const char kFlags[] = "qcS";
		static const char kValued[] = "OPUTt";
		std::vector<std::string> expanded;
		for (const std::string& arg : args) {
			const bool cluster = arg.size() > 2 && arg[0] == '-' && arg[1] != '-'
				&& std::strchr(kFlags, arg[1]) != nullptr;
			if (!cluster) {
				expanded.push_back(arg);
				continue;
			}

			for (std::size_t k = 1; k < arg.size(); ++k) {
				expanded.push_back(std::string("-") + arg[k]);
				if (std::strchr(kValued, arg[k]) == nullptr) continue;
				if (k + 1 < arg.size()) expanded.push_back(arg.substr(k + 1));
				break;
			}
		}

		return expanded;
	}

	static bool applyWgetFlag(const std::string& arg, WgetOptions& options) {
		const bool insecure = arg == "--no-check-certificate";
		if (arg == "-q" || arg == "--quiet")                 options.quiet = true;
		else if (arg == "-c" || arg == "--continue")         options.resume = true;
		else if (arg == "-S" || arg == "--server-response")  options.server_response = true;
		else if (arg == "--spider")                          options.spider = true;
		else if (insecure)                                   options.insecure = true;
		else return arg == "-nv" || arg == "--no-verbose" || arg == "-Y" || arg == "-nc";
		return true;
	}

	static bool parseWgetArgs(Executor& exec, const std::vector<std::string>& args,
	                          WgetOptions& options) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg.size() < 2 || arg[0] != '-') {
				options.urls.push_back(arg);
				continue;
			}

			bool valid = true;
			if (applyWgetValue(exec, args, i, options, valid) && valid) continue;
			if (valid && applyWgetFlag(arg, options)) continue;
			perr("wget", valid ? "unrecognized option '" + arg + "'" : "invalid value for " + arg);
			return false;
		}

		if (!options.urls.empty()) return true;
		perr("wget", "missing URL");
		return false;
	}

	static bool parseUrl(const std::string& url, ParsedUrl& parsed) {
		std::vector<wchar_t> host(512);
		std::vector<wchar_t> path(8192);
		std::vector<wchar_t> user(256);
		std::vector<wchar_t> password(256);
		URL_COMPONENTS parts{};
		parts.dwStructSize = sizeof(parts);
		parts.lpszHostName = host.data();
		parts.dwHostNameLength = static_cast<DWORD>(host.size());
		parts.lpszUrlPath = path.data();
		parts.dwUrlPathLength = static_cast<DWORD>(path.size());
		parts.lpszUserName = user.data();
		parts.dwUserNameLength = static_cast<DWORD>(user.size());
		parts.lpszPassword = password.data();
		parts.dwPasswordLength = static_cast<DWORD>(password.size());

		const std::wstring wide = utf8ToWide(url);
		if (::WinHttpCrackUrl(wide.c_str(), 0, 0, &parts) == 0) return false;

		parsed.host = host.data();
		parsed.user = user.data();
		parsed.password = password.data();
		parsed.https = parts.nScheme == INTERNET_SCHEME_HTTPS;
		parsed.port = parts.nPort;
		if (parts.dwUrlPathLength != 0) {
			parsed.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
		}

		return parts.nScheme == INTERNET_SCHEME_HTTP || parsed.https;
	}

	static std::string decodePercent(const std::string& text) {
		std::string out;
		for (std::size_t k = 0; k < text.size(); ++k) {
			unsigned long value = 0;
			std::size_t used = 0;
			const bool escaped = text[k] == '%' && k + 2 < text.size()
				&& parseUL(text.substr(k + 1, 2), value, 16, &used) && used == 2;
			if (!escaped) {
				out.push_back(text[k]);
				continue;
			}

			out.push_back(static_cast<char>(value));
			k += 2;
		}

		return out;
	}

	static std::string remoteName(const ParsedUrl& url) {
		std::string path = wideToUtf8(url.path);
		const std::size_t query = path.find_first_of("?#");
		if (query != std::string::npos) path.resize(query);

		const std::size_t slash = path.find_last_of('/');
		const std::string last = slash == std::string::npos ? path : path.substr(slash + 1);
		const std::string name = decodePercent(last);
		return name.empty() ? "index.html" : name;
	}

	static std::string freeName(Executor& exec, const std::string& wanted) {
		std::error_code ec;
		if (!fs::exists(toNative(exec, wanted), ec)) return wanted;
		for (int k = 1;; ++k) {
			const std::string candidate = wanted + "." + std::to_string(k);
			if (!fs::exists(toNative(exec, candidate), ec)) return candidate;
		}
	}

	static std::string targetPath(Executor& exec, const WgetOptions& options,
	                              const ParsedUrl& url) {
		if (!options.output.empty()) return options.output;

		std::string name = remoteName(url);
		if (!options.directory.empty()) name = options.directory + "/" + name;
		return options.resume ? name : freeName(exec, name);
	}

	static std::wstring basicAuthorization(const ParsedUrl& url) {
		static const char kAlphabet[] =
			"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		const std::string plain = wideToUtf8(url.user) + ":" + wideToUtf8(url.password);
		std::string encoded;
		for (std::size_t k = 0; k < plain.size(); k += 3) {
			unsigned value = static_cast<unsigned char>(plain[k]) << 16;
			if (k + 1 < plain.size()) value |= static_cast<unsigned char>(plain[k + 1]) << 8;
			if (k + 2 < plain.size()) value |= static_cast<unsigned char>(plain[k + 2]);
			encoded.push_back(kAlphabet[(value >> 18) & 63]);
			encoded.push_back(kAlphabet[(value >> 12) & 63]);
			encoded.push_back(k + 1 < plain.size() ? kAlphabet[(value >> 6) & 63] : '=');
			encoded.push_back(k + 2 < plain.size() ? kAlphabet[value & 63] : '=');
		}

		return L"Authorization: Basic " + utf8ToWide(encoded);
	}

	static void addHeader(HINTERNET request, const std::wstring& header) {
		::WinHttpAddRequestHeaders(request, header.c_str(), kNulTerminated,
			WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
	}

	static void configureRequest(HttpHandles& handles, const WgetOptions& options,
	                             const ParsedUrl& url, unsigned long long resume_from) {
		if (options.timeout_seconds > 0) {
			const int ms = options.timeout_seconds * 1000;
			::WinHttpSetTimeouts(handles.request, ms, ms, ms, ms);
		}

		if (options.insecure) {
			DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID
				| SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
			::WinHttpSetOption(handles.request, WINHTTP_OPTION_SECURITY_FLAGS, &flags,
				sizeof(flags));
		}

		for (const std::string& header : options.headers) {
			addHeader(handles.request, utf8ToWide(header));
		}

		if (!url.user.empty()) addHeader(handles.request, basicAuthorization(url));
		const std::wstring range = L"Range: bytes=" + std::to_wstring(resume_from) + L"-";
		if (resume_from > 0) addHeader(handles.request, range);
		if (!options.post) return;
		addHeader(handles.request, L"Content-Type: application/x-www-form-urlencoded");
	}

	static bool openRequest(HttpHandles& handles, const WgetOptions& options, const ParsedUrl& url,
	                        unsigned long long resume_from) {
		const std::wstring agent = utf8ToWide(options.user_agent);
		handles.session = ::WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
			WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (handles.session == nullptr) {
			handles.session = ::WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
				WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		}

		if (handles.session == nullptr) return false;
		handles.connect = ::WinHttpConnect(handles.session, url.host.c_str(), url.port, 0);
		if (handles.connect == nullptr) return false;

		const wchar_t* verb = options.spider ? L"HEAD" : (options.post ? L"POST" : L"GET");
		handles.request = ::WinHttpOpenRequest(handles.connect, verb, url.path.c_str(), nullptr,
			WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, url.https ? WINHTTP_FLAG_SECURE : 0);
		if (handles.request == nullptr) return false;

		configureRequest(handles, options, url, resume_from);
		return true;
	}

	static bool sendRequest(HttpHandles& handles, const WgetOptions& options) {
		LPVOID body = WINHTTP_NO_REQUEST_DATA;
		if (options.post) body = const_cast<char*>(options.post_data.data());
		const DWORD size = static_cast<DWORD>(options.post_data.size());
		return ::WinHttpSendRequest(handles.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, body, size,
			size, 0) != 0 && ::WinHttpReceiveResponse(handles.request, nullptr) != 0;
	}

	static DWORD numberHeader(HINTERNET request, DWORD which, bool& present) {
		DWORD value = 0;
		DWORD size = sizeof(value);
		present = ::WinHttpQueryHeaders(request, which | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &value, &size, WINHTTP_NO_HEADER_INDEX) != 0;
		return value;
	}

	static unsigned long long contentLength(HINTERNET request) {
		wchar_t text[32] = {};
		DWORD size = sizeof(text);
		if (::WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH,
		                          WINHTTP_HEADER_NAME_BY_INDEX,
		                          text, &size, WINHTTP_NO_HEADER_INDEX) == 0) {
			return 0;
		}

		unsigned long long length = 0;
		parseULL(wideToUtf8(text), length);
		return length;
	}

	static std::string rawHeaders(HINTERNET request) {
		DWORD size = 0;
		::WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
			nullptr, &size, WINHTTP_NO_HEADER_INDEX);
		if (size == 0) return std::string();

		std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, 0);
		if (::WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
		                          WINHTTP_HEADER_NAME_BY_INDEX,
		                          buffer.data(), &size, WINHTTP_NO_HEADER_INDEX) == 0) {
			return std::string();
		}

		return wideToUtf8(buffer.data());
	}

	static std::string statusLine(HINTERNET request) {
		const std::string headers = rawHeaders(request);
		const std::size_t end = headers.find("\r\n");
		return end == std::string::npos ? headers : headers.substr(0, end);
	}

	static std::string sizeText(unsigned long long bytes) {
		static const char kUnits[] = " KMGT";
		double value = static_cast<double>(bytes);
		int unit = 0;
		while (value >= 1024.0 && unit < 4) {
			value /= 1024.0;
			++unit;
		}

		char text[32];
		if (unit == 0) std::snprintf(text, sizeof(text), "%llu", bytes);
		else           std::snprintf(text, sizeof(text), "%.1f%c", value, kUnits[unit]);
		return text;
	}

	static void drawProgress(Progress& progress, bool final_draw) {
		const auto now = std::chrono::steady_clock::now();
		if (!final_draw && now - progress.last_draw < std::chrono::milliseconds(200)) return;
		progress.last_draw = now;

		const unsigned long long have = progress.resumed_from + progress.done;
		const unsigned long long total = progress.total == 0 ? 0
			: progress.resumed_from + progress.total;
		const double seconds = std::chrono::duration<double>(now - progress.started).count();
		const double rate = seconds > 0.0 ? static_cast<double>(progress.done) / seconds : 0.0;
		std::string bar(kBarWidth, ' ');
		int percent = 0;
		if (total > 0) {
			percent = static_cast<int>(have * 100 / total);
			const int filled = static_cast<int>(have * kBarWidth / total);
			bar.replace(0, static_cast<std::size_t>(filled), static_cast<std::size_t>(filled), '*');
		}

		std::fprintf(stderr, "\r%-20.20s %3d%% |%s| %7s %8s/s", progress.name.c_str(), percent,
			bar.c_str(), sizeText(have).c_str(),
			sizeText(static_cast<unsigned long long>(rate)).c_str());
		if (final_draw) std::fputc('\n', stderr);
	}

	// WinHTTP reports a connection closed before Content-Length bytes as a
	// normal end of data, so the byte count is checked here.
	static int streamBody(HINTERNET request, std::FILE* out, Progress& progress, bool quiet) {
		std::vector<char> buffer(64 * 1024);
		for (;;) {
			DWORD got = 0;
			const DWORD capacity = static_cast<DWORD>(buffer.size());
			if (::WinHttpReadData(request, buffer.data(), capacity, &got) == 0) return kExitNetwork;
			if (got == 0) break;
			if (std::fwrite(buffer.data(), 1, got, out) != got) return kExitFileIo;

			progress.done += got;
			if (!quiet) drawProgress(progress, false);
		}

		if (!quiet) drawProgress(progress, true);
		if (progress.total > 0 && progress.done < progress.total) return kExitNetwork;
		return 0;
	}

	static unsigned long long existingSize(Executor& exec, const std::string& path) {
		std::error_code ec;
		const auto size = fs::file_size(toNative(exec, path), ec);
		return ec ? 0 : static_cast<unsigned long long>(size);
	}

	static int reportServerError(const WgetOptions& options, HINTERNET request) {
		if (!options.quiet) perr("wget", "server returned error: " + statusLine(request));
		return kExitServer;
	}

	static std::FILE* openTarget(Executor& exec, const std::string& path, bool append) {
		if (path == "-") return stdout;
		return fopenNative(exec, path, append ? "ab" : "wb");
	}

	static int reportSaveStatus(const std::string& path, const Progress& progress, int status) {
		if (status == kExitFileIo) {
			perr("wget", "error writing '" + path + "': " + std::strerror(errno));
		} else if (status == kExitNetwork) {
			const unsigned long long have = progress.resumed_from + progress.done;
			perr("wget", "download of '" + path + "' was interrupted after "
				+ std::to_string(have) + " bytes");
		}

		return status;
	}

	static int saveBody(Executor& exec, HINTERNET request, const WgetOptions& options,
	                    const std::string& path, Progress& progress, bool append) {
		std::FILE* out = openTarget(exec, path, append);
		if (out == nullptr) {
			perr("wget", "can't open '" + path + "': " + std::strerror(errno));
			return kExitFileIo;
		}

		const bool to_stdout = out == stdout;
		int status = streamBody(request, out, progress, options.quiet || to_stdout);
		const bool flushed = std::fflush(out) == 0 && std::ferror(out) == 0;
		const bool closed = to_stdout || std::fclose(out) == 0;
		if (status == 0 && !(flushed && closed)) status = kExitFileIo;
		return reportSaveStatus(path, progress, status);
	}

	static int handleResponse(Executor& exec, HttpHandles& handles, const WgetOptions& options,
	                          const std::string& path, unsigned long long resume_from) {
		bool present = false;
		const DWORD status = numberHeader(handles.request, WINHTTP_QUERY_STATUS_CODE, present);
		if (options.server_response) {
			std::fprintf(stderr, "%s", rawHeaders(handles.request).c_str());
		}

		if (resume_from > 0 && status == 416) {
			if (!options.quiet) {
				std::fprintf(stderr, "wget: '%s' is already complete\n", path.c_str());
			}

			return 0;
		}

		if (status >= 400) return reportServerError(options, handles.request);
		if (options.spider) {
			if (!options.quiet) std::fprintf(stderr, "Remote file exists.\n");
			return 0;
		}

		Progress progress;
		progress.name = path == "-" ? "-" : fs::path(utf8ToPath(path)).filename().string();
		progress.total = contentLength(handles.request);
		progress.resumed_from = status == 206 ? resume_from : 0;
		progress.started = std::chrono::steady_clock::now();
		return saveBody(exec, handles.request, options, path, progress, status == 206);
	}

	static int fetchOne(Executor& exec, const WgetOptions& options, std::string url_text) {
		if (url_text.find("://") == std::string::npos) url_text = "http://" + url_text;

		ParsedUrl url;
		if (!parseUrl(url_text, url)) {
			perr("wget", "unsupported or invalid URL '" + url_text + "'");
			return kExitUsage;
		}

		const std::string path = options.spider ? std::string() : targetPath(exec, options, url);
		const bool can_resume = options.resume && path != "-";
		const unsigned long long resume_from = can_resume ? existingSize(exec, path) : 0;
		if (!options.quiet) {
			std::fprintf(stderr, "Connecting to %s\n", wideToUtf8(url.host).c_str());
		}


		HttpHandles handles;
		if (!openRequest(handles, options, url, resume_from) || !sendRequest(handles, options)) {
			if (!options.quiet) {
				std::fprintf(stderr, "wget: can't connect to remote host (error %lu)\n",
					::GetLastError());
			}

			return kExitNetwork;
		}

		return handleResponse(exec, handles, options, path, resume_from);
	}

	static int builtin_wget(Executor& exec, const std::vector<std::string>& args) {
		WgetOptions options;
		if (!parseWgetArgs(exec, expandShortClusters(args), options)) return kExitUsage;

		std::error_code ec;
		if (!options.directory.empty()) {
			fs::create_directories(toNative(exec, options.directory), ec);
		}


		int status = 0;
		for (const std::string& url : options.urls) {
			const int one = fetchOne(exec, options, url);
			if (one != 0) status = one;
		}

		return status;
	}

	void registerWgetBuiltin(Executor& exec) {
		exec.registerBuiltin("wget", builtin_wget);
	}

#else

	void registerWgetBuiltin(Executor&) {}

#endif /* _WIN32 */

}  // namespace wbsh

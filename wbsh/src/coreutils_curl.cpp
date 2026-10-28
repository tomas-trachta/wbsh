/**
 * @file coreutils_curl.cpp
 * @brief Bundled `curl`: the per-URL driver (outputs, retries, cookies, -w).
 *
 * Option parsing lives in curl_options.cpp, request bodies in
 * curl_request.cpp, URL / cookie helpers in curl_support.cpp, the WinHTTP
 * transfer in curl_http.cpp and --write-out in curl_writeout.cpp.
 */

#include "coreutils_internal.h"

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif /* _WIN32 */

#include "curl_internal.h"
#include "executor.h"
#include "numparse.h"
#include "pathconv.h"

namespace wbsh {

	namespace curl_detail {

		static const double kMaxBackoffSeconds = 600;

		std::FILE* OutputSink::open() {
			if (opened) return file;

			opened = true;
			if (path.empty()) {
				file = stdout;
				return file;
			}

			if (create_dirs) {
				std::error_code ec;
				const std::filesystem::path parent = toNative(exec, path).parent_path();
				if (!parent.empty()) std::filesystem::create_directories(parent, ec);
			}

			file = fopenNative(exec, path, append ? "ab" : "wb");
			if (file == nullptr) open_error = "Failed to open the file " + path + ": " + std::strerror(errno);
			return file;
		}

		bool OutputSink::write(const char* data, std::size_t size) {
			std::FILE* out = open();
			return out != nullptr && std::fwrite(data, 1, size, out) == size;
		}

		bool OutputSink::finish() {
			if (file == nullptr) return open_error.empty();

			const bool flushed = std::fflush(file) == 0 && std::ferror(file) == 0;
			const bool closed = file == stdout || std::fclose(file) == 0;
			file = nullptr;
			return flushed && closed;
		}

		void OutputSink::reset() {
			if (file != nullptr && file != stdout) std::fclose(file);
			file = nullptr;
			opened = false;
			open_error.clear();
		}

		static bool reportsErrors(const CurlOptions& options) {
			return !options.silent || options.show_error;
		}

		static void reportError(const CurlOptions& options, int code, const std::string& message) {
			if (reportsErrors(options)) std::fprintf(stderr, "curl: (%d) %s\n", code, message.c_str());
		}

		static std::string withScheme(const std::string& url) {
			return url.find("://") == std::string::npos ? "http://" + url : url;
		}

		static bool remoteNameFor(const CurlOptions& options, std::size_t index) {
			if (index < options.outputs.size()) return options.outputs[index].remote_name;
			return options.remote_name_all;
		}

		static int outputPathFor(const CurlOptions& options, std::size_t index, const std::string& url,
				std::string& path) {
			path.clear();
			if (remoteNameFor(options, index)) {
				path = lastPathSegment(url);
				if (path.empty()) {
					if (reportsErrors(options)) std::fprintf(stderr, "curl: Remote file name has no length!\n");
					return kExitWrite;
				}
			} else if (index < options.outputs.size()) {
				path = options.outputs[index].path;
				if (path == "-") path.clear();
			}

			if (!path.empty() && !options.output_dir.empty()) path = options.output_dir + "/" + path;
			return 0;
		}

		static unsigned long long resumeOffset(Executor& exec, const CurlOptions& options, const std::string& path) {
			if (options.continue_at.empty()) return 0;
			unsigned long long offset = 0;
			if (options.continue_at != "-") return parseULL(options.continue_at, offset) ? offset : 0;
			if (path.empty()) return 0;

			std::error_code ec;
			const auto size = std::filesystem::file_size(toNative(exec, path), ec);
			return ec ? 0 : static_cast<unsigned long long>(size);
		}

		struct UrlJob {
			Executor& exec;
			const CurlOptions& options;
			CookieJar& jar;
			std::FILE* header_dump;
			std::string url;
			std::string output;
			Request request;
		};

		static bool isTransient(const CurlOptions& options, int code) {
			if (code == kRetryStatus || code == kExitTimeout) return true;
			if (code == kExitConnect && options.retry_connrefused) return true;
			return code != 0 && options.retry_all_errors;
		}

		static const char* problemName(int code) {
			if (code == kRetryStatus) return "HTTP error";
			if (code == kExitTimeout) return "timeout";
			if (code == kExitConnect) return "connection refused";
			return "error";
		}

		static void warnRetry(const CurlOptions& options, int code, double delay, int left) {
			if (options.silent) return;
			std::fprintf(stderr, "Warning: Problem : %s. Will retry in %g seconds. %d retr%s left.\n",
				problemName(code), delay, left, left == 1 ? "y" : "ies");
		}

		static int runOnce(UrlJob& job, Transfer& transfer) {
			transfer.started = std::chrono::steady_clock::now();
			transfer.header_dump = job.header_dump;
			const bool local = job.request.url.rfind("file://", 0) == 0;
			int code = local ? performFile(transfer, job.request) : performHttp(transfer, job.request);
			const auto spent = std::chrono::steady_clock::now() - transfer.started;
			transfer.info.time_total = std::chrono::duration<double>(spent).count();
			if (code == 0 && !transfer.sink.opened) transfer.sink.open();
			if (code == kRetryStatus) return code;

			const bool closed = transfer.sink.finish();
			if (code == 0 && !closed) {
				transfer.info.error = transfer.sink.open_error.empty() ? "Failure writing output to destination"
					: transfer.sink.open_error;
				code = kExitWrite;
			}

			return code;
		}

		struct RetryState {
			int left = 0;
			double delay = 1;
			std::chrono::steady_clock::time_point first;
		};

		static bool retryAllowed(const CurlOptions& options, const RetryState& retry) {
			if (retry.left <= 0) return false;
			if (options.retry_max_time <= 0) return true;

			const double spent = std::chrono::duration<double>(std::chrono::steady_clock::now() - retry.first).count();
			return spent < options.retry_max_time;
		}

		static void waitBeforeRetry(const CurlOptions& options, int code, RetryState& retry) {
			const double delay = options.retry_delay > 0 ? options.retry_delay : retry.delay;
			--retry.left;
			warnRetry(options, code, delay, retry.left);
			std::this_thread::sleep_for(std::chrono::duration<double>(delay));
			retry.delay = (std::min)(retry.delay * 2, kMaxBackoffSeconds);
		}

		static int transferWithRetries(UrlJob& job, ResponseInfo& info) {
			RetryState retry;
			retry.left = job.options.retry;
			retry.first = std::chrono::steady_clock::now();

			for (;;) {
				OutputSink sink(job.exec);
				sink.path = job.output;
				sink.append = job.request.resume_from > 0;
				sink.create_dirs = job.options.create_dirs;

				Transfer transfer(job.exec, job.options, job.jar, sink);
				transfer.retry_allowed = retryAllowed(job.options, retry);
				const int code = runOnce(job, transfer);
				info = transfer.info;
				sink.reset();
				if (!isTransient(job.options, code) || !retryAllowed(job.options, retry)) return code;

				waitBeforeRetry(job.options, code, retry);
			}
		}

		static bool supportedScheme(const std::string& url, std::string& scheme) {
			scheme = url.substr(0, url.find("://"));
			for (char& c : scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return scheme == "http" || scheme == "https" || scheme == "file";
		}

		static int prepareJob(UrlJob& job, std::size_t index, std::string& error) {
			std::string scheme;
			if (!supportedScheme(job.url, scheme)) {
				error = "Protocol \"" + scheme + "\" not supported";
				return kExitUnsupportedProtocol;
			}

			const int output = outputPathFor(job.options, index, job.url, job.output);
			if (output != 0) return -output;

			const int status = buildRequest(job.exec, job.options, job.url, job.request, error);
			if (status != 0) return status;

			job.request.resume_from = resumeOffset(job.exec, job.options, job.output);
			return 0;
		}

		static int runUrl(Executor& exec, const CurlOptions& options, CookieJar& jar, std::FILE* header_dump,
				std::size_t index) {
			UrlJob job{ exec, options, jar, header_dump, withScheme(options.urls[index]), std::string(), Request() };
			ResponseInfo info;
			info.url = job.url;

			std::string error;
			int code = prepareJob(job, index, error);
			if (code < 0) return -code;
			if (code == 0) code = transferWithRetries(job, info);
			else info.error = error;

			if (code == kRetryStatus) code = 0;
			writeOut(exec, options, info, code, job.output);
			if (code != 0) reportError(options, code, info.error);
			return code;
		}

#ifdef _WIN32
		static std::string promptPassword(const std::string& user) {
			HANDLE input = ::CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
				nullptr, OPEN_EXISTING, 0, nullptr);
			if (input == INVALID_HANDLE_VALUE) return std::string();

			std::fprintf(stderr, "Enter host password for user '%s':", user.c_str());
			DWORD mode = 0;
			::GetConsoleMode(input, &mode);
			::SetConsoleMode(input, mode & ~static_cast<DWORD>(ENABLE_ECHO_INPUT));

			wchar_t buffer[512] = {};
			DWORD read = 0;
			::ReadConsoleW(input, buffer, 511, &read, nullptr);
			::SetConsoleMode(input, mode);
			::CloseHandle(input);
			std::fputc('\n', stderr);

			std::wstring password(buffer, read);
			while (!password.empty() && (password.back() == L'\n' || password.back() == L'\r')) password.pop_back();
			return wideToUtf8(password);
		}
#else
		static std::string promptPassword(const std::string&) {
			return std::string();
		}
#endif /* _WIN32 */

		static void completeCredentials(CurlOptions& options) {
			if (options.user.empty() || options.user.find(':') != std::string::npos) return;
			options.user += ":" + promptPassword(options.user);
		}

		static void prepareCookies(Executor& exec, const CurlOptions& options, CookieJar& jar) {
			if (!options.cookie.empty() && options.cookie.find('=') == std::string::npos) {
				loadCookieFile(exec, options.cookie, jar);
			}

			if (!options.cookie_jar.empty()) jar.enabled = true;
		}

		static std::FILE* openHeaderDump(Executor& exec, const CurlOptions& options) {
			if (options.dump_header.empty()) return nullptr;
			if (options.dump_header == "-") return stdout;
			return fopenNative(exec, options.dump_header, "wb");
		}

		static int builtin_curl(Executor& exec, const std::vector<std::string>& args) {
			CurlOptions options;
			const int parsed = parseCurlArgs(exec, args, options);
			if (parsed >= 0) return parsed;

			completeCredentials(options);
			CookieJar jar;
			prepareCookies(exec, options, jar);

			std::FILE* header_dump = openHeaderDump(exec, options);
			int status = 0;
			for (std::size_t i = 0; i < options.urls.size(); ++i) {
				const int code = runUrl(exec, options, jar, header_dump, i);
				if (code != 0) status = code;
			}

			if (header_dump != nullptr && header_dump != stdout) std::fclose(header_dump);
			if (!options.cookie_jar.empty() && !saveCookieFile(exec, options.cookie_jar, jar)) {
				reportError(options, kExitWrite, "Failed to save cookies in " + options.cookie_jar);
			}

			std::fflush(stdout);
			return status;
		}

	}  // namespace curl_detail

	void registerCurlBuiltin(Executor& exec) {
		exec.registerBuiltin("curl", curl_detail::builtin_curl);
	}

}  // namespace wbsh

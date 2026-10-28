#pragma once

/**
 * @file curl_internal.h
 * @brief Options, request / response records and the entry points shared by the curl translation units.
 */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace wbsh {

	class Executor;

	namespace curl_detail {

		inline constexpr int kExitUnsupportedProtocol = 1;
		inline constexpr int kExitUsage = 2;
		inline constexpr int kExitMalformedUrl = 3;
		inline constexpr int kExitProxyResolve = 5;
		inline constexpr int kExitResolve = 6;
		inline constexpr int kExitConnect = 7;
		inline constexpr int kExitWeirdReply = 8;
		inline constexpr int kExitPartial = 18;
		inline constexpr int kExitHttp = 22;
		inline constexpr int kExitWrite = 23;
		inline constexpr int kExitRead = 26;
		inline constexpr int kExitTimeout = 28;
		inline constexpr int kExitRange = 33;
		inline constexpr int kExitSslConnect = 35;
		inline constexpr int kExitFileRead = 37;
		inline constexpr int kExitTooManyRedirects = 47;
		inline constexpr int kExitEmptyReply = 52;
		inline constexpr int kExitSend = 55;
		inline constexpr int kExitReceive = 56;
		inline constexpr int kExitSslCert = 60;

		/// Internal only: the response asks for a retry, nothing was written.
		inline constexpr int kRetryStatus = -1;

		enum class DataKind { Ascii, Binary, Raw, UrlEncode, Json };

		struct DataArg {
			DataKind kind = DataKind::Ascii;
			std::string text;
		};

		struct OutputArg {
			std::string path;
			bool remote_name = false;
		};

		struct CurlOptions {
			bool silent = false;
			bool show_error = false;
			bool verbose = false;
			bool include = false;
			bool head = false;
			bool fail = false;
			bool fail_with_body = false;
			bool create_dirs = false;
			bool remote_name_all = false;
			bool location = false;
			bool insecure = false;
			bool compressed = false;
			bool get_mode = false;
			bool retry_all_errors = false;
			bool retry_connrefused = false;
			bool user_agent_set = false;
			bool proxy_set = false;
			bool noproxy_set = false;

			std::vector<OutputArg> outputs;
			std::string output_dir;
			std::string dump_header;
			std::string write_out;
			std::string method;
			std::string user_agent;
			std::string referer;
			std::string user;
			std::string cookie;
			std::string cookie_jar;
			std::string range;
			std::string proxy;
			std::string noproxy;
			std::string upload_file;
			std::string continue_at;
			std::vector<std::string> headers;
			std::vector<DataArg> data;
			std::vector<DataArg> form;
			std::vector<std::string> urls;

			long long max_redirs = 50;
			double max_time = 0;
			double connect_timeout = 0;
			double retry_delay = 0;
			double retry_max_time = 0;
			int retry = 0;
		};

		/// Parses the command line; -1 to run, otherwise the exit status.
		int parseCurlArgs(Executor& exec, const std::vector<std::string>& args, CurlOptions& options);

		struct Cookie {
			std::string domain;
			bool subdomains = false;
			std::string path = "/";
			bool secure = false;
			bool http_only = false;
			long long expires = 0;
			std::string name;
			std::string value;
		};

		struct CookieJar {
			bool enabled = false;
			std::vector<Cookie> cookies;
		};

		bool loadCookieFile(Executor& exec, const std::string& path, CookieJar& jar);
		bool saveCookieFile(Executor& exec, const std::string& path, const CookieJar& jar);
		void storeSetCookie(CookieJar& jar, const std::string& host, const std::string& header_value);
		std::string cookieHeaderFor(const CookieJar& jar, const std::string& host, const std::string& path, bool https);

		struct UrlParts {
			std::string scheme;
			std::string user;
			std::string password;
			std::string host;
			int port = 0;
			std::string path = "/";
		};

		bool splitUrl(const std::string& url, UrlParts& parts);
		std::string resolveUrl(const std::string& base, const std::string& location);
		std::string urlEncode(const std::string& text);
		std::string base64Encode(const std::string& text);
		std::string lastPathSegment(const std::string& url);

		/// Header value from raw "Name: value\r\n" lines; the last occurrence wins.
		bool findHeader(const std::string& raw, const std::string& name, std::string& value);
		std::vector<std::string> findAllHeaders(const std::string& raw, const std::string& name);

		struct Request {
			std::string url;
			std::string method;
			std::vector<std::string> headers;
			std::string body;
			std::string cookie;
			bool has_body = false;
			std::string upload_path;
			unsigned long long upload_size = 0;
			unsigned long long resume_from = 0;
		};

		int buildRequest(Executor& exec, const CurlOptions& options, const std::string& url, Request& request,
			std::string& error);

		struct ResponseInfo {
			long status = 0;
			std::string url;
			std::string effective_url;
			std::string content_type;
			std::string redirect_url;
			std::string method;
			std::string scheme;
			std::string last_headers;
			std::string remote_host;
			int remote_port = 0;
			int num_redirects = 0;
			unsigned long long size_download = 0;
			unsigned long long size_header = 0;
			unsigned long long size_upload = 0;
			double time_starttransfer = 0;
			double time_total = 0;
			std::string error;
		};

		/// Where the response goes: opened lazily so a failed transfer leaves no file.
		struct OutputSink {
			Executor& exec;
			std::string path;
			bool append = false;
			bool create_dirs = false;
			std::FILE* file = nullptr;
			bool opened = false;
			std::string open_error;

			explicit OutputSink(Executor& executor) : exec(executor) {}

			std::FILE* open();
			bool write(const char* data, std::size_t size);
			bool finish();
			void reset();
		};

		struct Transfer {
			Executor& exec;
			const CurlOptions& options;
			CookieJar& jar;
			OutputSink& sink;
			std::FILE* header_dump = nullptr;
			bool retry_allowed = false;
			std::chrono::steady_clock::time_point started;
			ResponseInfo info;

			Transfer(Executor& executor, const CurlOptions& opts, CookieJar& cookies, OutputSink& out)
				: exec(executor), options(opts), jar(cookies), sink(out) {}
		};

		int performHttp(Transfer& transfer, Request& request);
		int performFile(Transfer& transfer, const Request& request);

		bool isRetryableStatus(long status);
		void writeOut(Executor& exec, const CurlOptions& options, const ResponseInfo& info, int exit_code,
			const std::string& filename);
		bool readDataSource(Executor& exec, const std::string& name, std::string& data);

	}  // namespace curl_detail

}  // namespace wbsh

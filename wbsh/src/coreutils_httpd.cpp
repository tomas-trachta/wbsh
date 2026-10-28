/**
 * @file coreutils_httpd.cpp
 * @brief httpd: a small static-file web server (busybox httpd).
 *
 * Serves a document root over HTTP/1.1 with keep-alive and a thread per
 * connection. GET and HEAD, directory indexes (index.html, else a
 * generated listing), MIME types by extension, byte ranges, and the
 * If-Modified-Since / Last-Modified pair. The request target is
 * percent-decoded and forced to stay under the root: `..`, drive letters,
 * UNC roots and NTFS alternate-stream `:` are all refused.
 */

#include "coreutils_internal.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif /* _WIN32 */

#include <cctype>
#include <cstdio>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "executor.h"
#include "filetimes.h"
#include "interrupt.h"
#include "numparse.h"
#include "optscan.h"
#include "pathconv.h"
#include "sockets.h"

namespace wbsh {

#ifdef _WIN32

	namespace httpd_detail {

		namespace fs = std::filesystem;

		enum OptionId { kPort = 1, kHome, kForeground, kVerbose, kConfig };

		struct AccessRule {
			bool allow = false;
			std::string prefix;
		};

		struct AuthRule {
			std::string path;
			std::string user;
			std::string password;
		};

		struct HttpdConfig {
			std::vector<AccessRule> access;
			std::vector<AuthRule> auth;
			std::vector<std::pair<std::string, std::string>> mime;
			std::vector<std::pair<int, std::string>> error_pages;
			std::vector<std::string> index_names;
		};

		struct HttpdOptions {
			std::string bind_addr = "0.0.0.0";
			std::string port = "80";
			fs::path home;
			bool foreground = false;
			bool verbose = false;
			HttpdConfig config;
		};

		struct Request {
			std::string method;
			std::string target;
			std::string path;
			std::string query;
			std::string version;
			std::string if_modified_since;
			std::string range;
			std::string local_path;
			std::string content_type;
			std::string authorization;
			std::vector<std::pair<std::string, std::string>> headers;
			std::string body;
			long long content_length = 0;
			bool keep_alive = true;
			bool valid = false;
			bool complete = false;
		};

		static const int kAcceptPollMs = 200;
		static const int kRequestTimeoutMs = 15000;
		static const int kCgiTimeoutMs = 30000;
		static const std::size_t kHeaderLimit = 64 * 1024;
		static const std::size_t kBodyLimit = static_cast<std::size_t>(64) << 20;

		static const std::vector<OptDef>& optionDefs() {
			static const std::vector<OptDef> defs = {
				{ kPort, 'p', "port", OptArg::Required },
				{ kHome, 'h', "home", OptArg::Required },
				{ kForeground, 'f', "foreground", OptArg::None },
				{ kVerbose, 'v', "verbose", OptArg::None },
				{ kConfig, 'c', "config", OptArg::Required },
			};
			return defs;
		}

		static void applyBind(HttpdOptions& options, const std::string& value) {
			const std::size_t colon = value.rfind(':');
			if (colon == std::string::npos) {
				options.port = value;
				return;
			}

			options.bind_addr = value.substr(0, colon);
			options.port = value.substr(colon + 1);
		}

		static std::string lowerAscii(std::string text);
		static void loadConfig(Executor& exec, const fs::path& path, HttpdConfig& config);

		static void chooseConfig(Executor& exec, const std::string& config_arg, HttpdOptions& options) {
			if (!config_arg.empty()) {
				loadConfig(exec, toNative(exec, config_arg), options.config);
				return;
			}

			const fs::path fallback = options.home / "httpd.conf";
			std::error_code ec;
			if (fs::is_regular_file(fallback, ec)) loadConfig(exec, fallback, options.config);
		}

		static bool parse(Executor& exec, const std::vector<std::string>& args, HttpdOptions& options) {
			OptScanResult scanned;
			if (!scanOptions("httpd", args, optionDefs(), scanned)) return false;

			std::string home_arg;
			std::string config_arg;
			for (const OptHit& hit : scanned.hits) {
				if (hit.id == kPort) applyBind(options, hit.value);
				if (hit.id == kHome) home_arg = hit.value;
				if (hit.id == kForeground) options.foreground = true;
				if (hit.id == kVerbose) options.verbose = true;
				if (hit.id == kConfig) config_arg = hit.value;
			}

			options.home = home_arg.empty() ? fs::current_path() : toNative(exec, home_arg);
			std::error_code ec;
			options.home = fs::weakly_canonical(options.home, ec);
			chooseConfig(exec, config_arg, options);
			return true;
		}

		static std::string trimAscii(const std::string& text) {
			std::size_t start = 0;
			std::size_t end = text.size();
			while (start < end && std::isspace(static_cast<unsigned char>(text[start])) != 0) ++start;
			while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) --end;
			return text.substr(start, end - start);
		}

		static void applyAccessLine(bool allow, const std::string& value, HttpdConfig& config) {
			AccessRule rule;
			rule.allow = allow;
			rule.prefix = value == "*" ? std::string() : value;
			config.access.push_back(rule);
		}

		static void applyAuthLine(const std::string& value, HttpdConfig& config) {
			const std::size_t first = value.find(':');
			const std::size_t second = value.find(':', first + 1);
			if (first == std::string::npos || second == std::string::npos) return;

			AuthRule rule;
			rule.path = value.substr(0, first);
			rule.user = value.substr(first + 1, second - first - 1);
			rule.password = value.substr(second + 1);
			config.auth.push_back(rule);
		}

		static void applyConfigLine(const std::string& raw, HttpdConfig& config) {
			const std::string line = trimAscii(raw);
			if (line.empty() || line[0] == '#') return;

			const std::size_t colon = line.find(':');
			if (colon == std::string::npos) return;
			const std::string key = line.substr(0, colon);
			const std::string value = line.substr(colon + 1);

			if (key == "A") applyAccessLine(true, value, config);
			else if (key == "D") applyAccessLine(false, value, config);
			else if (key == "I") config.index_names.push_back(value);
			else if (!key.empty() && key[0] == '/') applyAuthLine(line, config);
			else if (!key.empty() && key[0] == '.') config.mime.emplace_back(lowerAscii(key), value);
			else if (key.compare(0, 1, "E") == 0) {
				int code = 0;
				if (parseInt(key.substr(1), code)) config.error_pages.emplace_back(code, value);
			}
		}

		static void loadConfig(Executor& exec, const fs::path& path, HttpdConfig& config) {
			std::FILE* file = _wfopen(path.wstring().c_str(), L"rb");
			if (file == nullptr) {
				perr("httpd", "cannot read config " + pathToUtf8(path));
				return;
			}

			std::string text;
			char buffer[4096];
			std::size_t got = 0;
			while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, got);
			std::fclose(file);
			static_cast<void>(exec);

			std::size_t start = 0;
			while (start <= text.size()) {
				const std::size_t newline = text.find('\n', start);
				applyConfigLine(text.substr(start, newline - start), config);
				if (newline == std::string::npos) break;
				start = newline + 1;
			}
		}

		static std::string lowerAscii(std::string text) {
			for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return text;
		}

		static std::string mimeType(const fs::path& path) {
			static const std::pair<const char*, const char*> kTypes[] = {
				{ ".html", "text/html" }, { ".htm", "text/html" }, { ".txt", "text/plain" },
				{ ".css", "text/css" }, { ".js", "application/javascript" },
				{ ".json", "application/json" }, { ".xml", "application/xml" },
				{ ".png", "image/png" }, { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" },
				{ ".gif", "image/gif" }, { ".svg", "image/svg+xml" }, { ".ico", "image/x-icon" },
				{ ".pdf", "application/pdf" }, { ".zip", "application/zip" },
				{ ".gz", "application/gzip" }, { ".tar", "application/x-tar" },
				{ ".mp4", "video/mp4" }, { ".mp3", "audio/mpeg" }, { ".wasm", "application/wasm" },
			};
			const std::string ext = lowerAscii(pathToUtf8(path.extension()));
			for (const auto& entry : kTypes) {
				if (ext == entry.first) return entry.second;
			}

			return "application/octet-stream";
		}

		static std::string mimeFor(const HttpdConfig& config, const fs::path& path) {
			const std::string ext = lowerAscii(pathToUtf8(path.extension()));
			for (const auto& entry : config.mime) {
				if (entry.first == ext) return entry.second;
			}

			return mimeType(path);
		}

		static std::string httpDate(std::time_t when) {
			std::tm utc{};
			gmtime_s(&utc, &when);
			char text[40];
			std::strftime(text, sizeof(text), "%a, %d %b %Y %H:%M:%S GMT", &utc);
			return text;
		}

		static int monthIndex(const std::string& name) {
			static const char* months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug",
				"Sep", "Oct", "Nov", "Dec" };
			for (int i = 0; i < 12; ++i) {
				if (name.compare(0, 3, months[i]) == 0) return i;
			}

			return -1;
		}

		// Parses the RFC 1123 form clients echo from Last-Modified; other
		// formats are treated as "no valid date", which just serves the body.
		static bool parseHttpDate(const std::string& text, std::time_t& out) {
			std::tm when{};
			char weekday[8];
			char month[8];
			const int fields = std::sscanf(text.c_str(), "%3[^,], %d %3s %d %d:%d:%d", weekday,
				&when.tm_mday, month, &when.tm_year, &when.tm_hour, &when.tm_min, &when.tm_sec);
			if (fields != 7) return false;

			when.tm_mon = monthIndex(month);
			if (when.tm_mon < 0) return false;
			when.tm_year -= 1900;
			out = _mkgmtime(&when);
			return out != static_cast<std::time_t>(-1);
		}

		static std::string percentDecode(const std::string& text) {
			std::string out;
			out.reserve(text.size());
			for (std::size_t i = 0; i < text.size(); ++i) {
				if (text[i] == '%' && i + 2 < text.size()) {
					int value = 0;
					if (std::sscanf(text.substr(i + 1, 2).c_str(), "%2x", &value) == 1) {
						out.push_back(static_cast<char>(value));
						i += 2;
						continue;
					}
				}

				out.push_back(text[i]);
			}

			return out;
		}

		// Windows ignores a trailing dot or space and reads ':' as a drive or
		// an alternate data stream, so such a segment could name a file the
		// request does not seem to.
		static bool isSafeSegment(const std::string& segment) {
			if (segment == ".." || segment.find(':') != std::string::npos) return false;
			return segment.back() != '.' && segment.back() != ' ';
		}

		/// The request path decoded, with '\\' as '/', '.' and empty segments
		/// dropped; false for anything that could step outside the root.
		static bool normalizeRequestPath(const std::string& raw, std::string& out) {
			std::string decoded = percentDecode(raw);
			for (char& c : decoded) {
				if (c == '\\') c = '/';
			}

			if (decoded.empty() || decoded[0] != '/' || decoded.find('\0') != std::string::npos) return false;

			std::string normal;
			std::size_t start = 1;
			while (start <= decoded.size()) {
				const std::size_t slash = decoded.find('/', start);
				const std::string segment = decoded.substr(start, slash - start);
				if (!segment.empty() && segment != ".") {
					if (!isSafeSegment(segment)) return false;
					normal += "/" + segment;
				}

				if (slash == std::string::npos) break;
				start = slash + 1;
			}

			if (normal.empty()) normal = "/";
			else if (decoded.back() == '/') normal += "/";
			out = normal;
			return true;
		}

		static std::wstring foldedWide(const fs::path& path) {
			std::wstring text = path.wstring();
			for (wchar_t& c : text) c = static_cast<wchar_t>(::towlower(c));
			while (text.size() > 3 && (text.back() == L'\\' || text.back() == L'/')) text.pop_back();
			return text;
		}

		static bool isWithin(const fs::path& root, const fs::path& candidate) {
			const std::wstring base = foldedWide(root);
			const std::wstring full = foldedWide(candidate);
			if (full.compare(0, base.size(), base) != 0) return false;
			return full.size() == base.size() || full[base.size()] == L'\\' || base.back() == L'\\';
		}

		static bool resolveTarget(const HttpdOptions& options, const std::string& local_path,
		                          fs::path& resolved) {
			resolved = options.home;
			if (local_path.size() > 1) resolved /= utf8ToPath(local_path.substr(1));
			resolved = resolved.lexically_normal();

			std::error_code ec;
			const fs::path canonical = fs::weakly_canonical(resolved, ec);
			return ec || isWithin(options.home, canonical);
		}

		/// The on-disk spelling of @p local_path relative to the root ("/Dir/f"),
		/// which is what auth rules and the cgi-bin check must see: it folds
		/// short names, case and the like back to the real names.
		static std::string canonicalRelative(const HttpdOptions& options, const std::string& local_path) {
			fs::path target;
			if (!resolveTarget(options, local_path, target)) return local_path;

			std::error_code ec;
			const fs::path canonical = fs::weakly_canonical(target, ec);
			if (ec) return local_path;

			const fs::path relative = canonical.lexically_relative(options.home);
			const std::string text = pathToUtf8(relative.generic_wstring());
			return text == "." || text.empty() ? std::string("/") : "/" + text;
		}

		static void sendRaw(NetSocket socket, const std::string& data) {
			netSendAll(socket, data.data(), data.size());
		}

		static std::string statusLine(int status) {
			switch (status) {
				case 200: return "200 OK";
				case 206: return "206 Partial Content";
				case 301: return "301 Moved Permanently";
				case 302: return "302 Found";
				case 304: return "304 Not Modified";
				case 400: return "400 Bad Request";
				case 401: return "401 Unauthorized";
				case 403: return "403 Forbidden";
				case 404: return "404 Not Found";
				case 405: return "405 Method Not Allowed";
				case 416: return "416 Requested Range Not Satisfiable";
				case 500: return "500 Internal Server Error";
				case 501: return "501 Not Implemented";
				default: return std::to_string(status) + " Status";
			}
		}

		struct Response {
			int status = 200;
			std::vector<std::string> headers;
			bool keep_alive = true;
		};

		static std::string renderHead(const Response& response, std::size_t body_length) {
			std::string head = "HTTP/1.1 " + statusLine(response.status) + "\r\n";
			head += "Server: wbsh-httpd\r\n";
			head += "Date: " + httpDate(std::time(nullptr)) + "\r\n";
			for (const std::string& header : response.headers) head += header + "\r\n";
			head += "Content-Length: " + std::to_string(body_length) + "\r\n";
			head += std::string("Connection: ") + (response.keep_alive ? "keep-alive" : "close") + "\r\n";
			return head + "\r\n";
		}

		static void sendError(NetSocket socket, int status, bool keep_alive, bool head_only) {
			const std::string message = statusLine(status);
			const std::string body = "<html><head><title>" + message + "</title></head><body><h1>"
				+ message + "</h1></body></html>\r\n";
			Response response;
			response.status = status;
			response.keep_alive = keep_alive;
			response.headers.push_back("Content-Type: text/html");
			sendRaw(socket, renderHead(response, body.size()));
			if (!head_only) sendRaw(socket, body);
		}

		static NetWait readHeaderBlock(NetReader& reader, std::string& block) {
			return netReadUntil(reader, "\r\n\r\n", kHeaderLimit, block, kRequestTimeoutMs);
		}

		static void splitHeader(const std::string& line, Request& request) {
			const std::size_t colon = line.find(':');
			if (colon == std::string::npos) return;

			const std::string name = lowerAscii(line.substr(0, colon));
			std::string value = line.substr(colon + 1);
			while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
			while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) value.pop_back();

			request.headers.emplace_back(name, value);
			if (name == "connection") request.keep_alive = lowerAscii(value) != "close";
			if (name == "if-modified-since") request.if_modified_since = value;
			if (name == "range") request.range = value;
			if (name == "content-type") request.content_type = value;
			if (name == "authorization") request.authorization = value;
			if (name == "content-length") parseLL(value, request.content_length);
		}

		static void parseRequestLine(const std::string& line, Request& request) {
			const std::size_t first = line.find(' ');
			const std::size_t second = line.find(' ', first + 1);
			if (first == std::string::npos || second == std::string::npos) return;

			request.method = line.substr(0, first);
			request.target = line.substr(first + 1, second - first - 1);
			request.version = line.substr(second + 1);
			const std::size_t qmark = request.target.find('?');
			request.path = request.target.substr(0, qmark);
			if (qmark != std::string::npos) request.query = request.target.substr(qmark + 1);
			request.valid = !request.method.empty() && !request.target.empty();
		}

		static Request parseRequest(const std::string& block) {
			Request request;
			request.complete = true;
			std::size_t start = 0;
			bool first_line = true;
			while (start < block.size()) {
				const std::size_t newline = block.find('\n', start);
				const std::string line = block.substr(start, newline - start);
				if (line == "\r" || line.empty()) break;

				if (first_line) parseRequestLine(line, request);
				else splitHeader(line, request);
				first_line = false;
				if (newline == std::string::npos) break;
				start = newline + 1;
			}

			request.keep_alive = request.keep_alive && request.version == "HTTP/1.1";
			return request;
		}

		static std::string readFileBytes(const fs::path& path, std::uint64_t offset, std::uint64_t length,
		                                 bool& ok) {
			std::FILE* file = _wfopen(path.wstring().c_str(), L"rb");
			if (file == nullptr) {
				ok = false;
				return std::string();
			}

			_fseeki64(file, static_cast<long long>(offset), SEEK_SET);
			std::string data(static_cast<std::size_t>(length), '\0');
			const std::size_t got = std::fread(data.data(), 1, data.size(), file);
			std::fclose(file);
			data.resize(got);
			ok = true;
			return data;
		}

		static std::string htmlEscape(const std::string& text) {
			std::string out;
			out.reserve(text.size());
			for (const char c : text) {
				switch (c) {
					case '&': out += "&amp;"; break;
					case '<': out += "&lt;"; break;
					case '>': out += "&gt;"; break;
					case '"': out += "&quot;"; break;
					case '\'': out += "&#39;"; break;
					default: out.push_back(c); break;
				}
			}

			return out;
		}

		static std::string percentEncode(const std::string& text) {
			static const char* kHex = "0123456789ABCDEF";
			std::string out;
			for (const char c : text) {
				const unsigned char byte = static_cast<unsigned char>(c);
				if (std::isalnum(byte) != 0 || c == '-' || c == '.' || c == '_' || c == '~') {
					out.push_back(c);
					continue;
				}

				out.push_back('%');
				out.push_back(kHex[byte >> 4]);
				out.push_back(kHex[byte & 0x0F]);
			}

			return out;
		}

		static std::string listingEntry(const fs::directory_entry& entry) {
			std::error_code ec;
			const std::string name = pathToUtf8(entry.path().filename());
			const std::string slash = entry.is_directory(ec) ? "/" : "";
			return "<a href=\"" + percentEncode(name) + slash + "\">" + htmlEscape(name) + slash + "</a>\n";
		}

		static std::string listingBody(const fs::path& dir, const std::string& display_path) {
			const std::string title = htmlEscape(display_path);
			std::string body = "<html><head><meta charset=\"utf-8\"><title>Index of " + title
				+ "</title></head><body><h1>Index of " + title + "</h1><hr><pre>";
			if (display_path != "/") body += "<a href=\"../\">../</a>\n";

			std::error_code ec;
			for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
				body += listingEntry(entry);
			}

			return body + "</pre><hr></body></html>\r\n";
		}

		static bool notModified(const Request& request, std::time_t modified) {
			std::time_t since = 0;
			return !request.if_modified_since.empty() && parseHttpDate(request.if_modified_since, since)
				&& modified <= since;
		}

		struct RangeSpec {
			std::uint64_t start = 0;
			std::uint64_t length = 0;
			bool partial = false;
			bool satisfiable = true;
		};

		static RangeSpec markUnsatisfiable(RangeSpec spec) {
			spec.satisfiable = false;
			return spec;
		}

		static RangeSpec suffixRange(RangeSpec spec, const std::string& tail, std::uint64_t size) {
			long long last = 0;
			if (!parseLL(tail, last) || last <= 0) return markUnsatisfiable(spec);

			const std::uint64_t want = static_cast<std::uint64_t>(last);
			spec.start = want >= size ? 0 : size - want;
			spec.length = size - spec.start;
			return spec;
		}

		static RangeSpec finishRange(RangeSpec spec, const std::string& body, std::size_t dash,
		                             std::uint64_t size) {
			const std::string head = body.substr(0, dash);
			const std::string tail = body.substr(dash + 1);
			if (head.empty()) return suffixRange(spec, tail, size);

			long long first = 0;
			long long last = 0;
			if (!parseLL(head, first) || static_cast<std::uint64_t>(first) >= size) {
				return markUnsatisfiable(spec);
			}

			spec.start = static_cast<std::uint64_t>(first);
			const std::uint64_t end = tail.empty() || !parseLL(tail, last)
				? size - 1 : (std::min)(static_cast<std::uint64_t>(last), size - 1);
			spec.length = end - spec.start + 1;
			return spec;
		}

		static RangeSpec parseRange(const std::string& header, std::uint64_t size) {
			RangeSpec spec;
			spec.length = size;
			if (header.compare(0, 6, "bytes=") != 0) return spec;

			const std::string body = header.substr(6);
			const std::size_t dash = body.find('-');
			if (dash == std::string::npos) return spec;

			spec.partial = true;
			return finishRange(spec, body, dash, size);
		}

		static void sendFile(NetSocket socket, const HttpdOptions& options, const fs::path& path,
		                     const Request& request, std::uint64_t size, std::time_t modified) {
			RangeSpec spec = request.range.empty() ? RangeSpec{ 0, size, false, true }
				: parseRange(request.range, size);
			if (!spec.satisfiable) {
				sendError(socket, 416, request.keep_alive, false);
				return;
			}

			Response response;
			response.status = spec.partial ? 206 : 200;
			response.keep_alive = request.keep_alive;
			response.headers.push_back("Content-Type: " + mimeFor(options.config, path));
			response.headers.push_back("Last-Modified: " + httpDate(modified));
			response.headers.push_back("Accept-Ranges: bytes");
			if (spec.partial) {
				response.headers.push_back("Content-Range: bytes " + std::to_string(spec.start) + "-"
					+ std::to_string(spec.start + spec.length - 1) + "/" + std::to_string(size));
			}

			const bool head_only = request.method == "HEAD";
			bool ok = true;
			const std::string body = head_only ? std::string()
				: readFileBytes(path, spec.start, spec.length, ok);
			sendRaw(socket, renderHead(response, spec.length));
			if (!head_only && ok) sendRaw(socket, body);
		}

		static void sendNotModified(NetSocket socket, const Request& request, std::time_t modified) {
			Response response;
			response.status = 304;
			response.keep_alive = request.keep_alive;
			response.headers.push_back("Last-Modified: " + httpDate(modified));
			sendRaw(socket, renderHead(response, 0));
		}

		static void sendListing(NetSocket socket, const fs::path& dir, const Request& request) {
			const std::string body = listingBody(dir, request.local_path);
			Response response;
			response.keep_alive = request.keep_alive;
			response.headers.push_back("Content-Type: text/html");
			sendRaw(socket, renderHead(response, body.size()));
			if (request.method != "HEAD") sendRaw(socket, body);
		}

		static fs::path indexWithin(const HttpdOptions& options, const fs::path& dir) {
			std::vector<std::string> names = options.config.index_names;
			if (names.empty()) names.push_back("index.html");

			std::error_code ec;
			for (const std::string& name : names) {
				const fs::path index = dir / name;
				if (fs::is_regular_file(index, ec)) return index;
			}

			return fs::path();
		}

		static void serveFileEntry(NetSocket socket, const HttpdOptions& options, const fs::path& target,
		                           const Request& request) {
			std::error_code ec;
			const std::uint64_t size = fs::file_size(target, ec);
			if (ec) {
				sendError(socket, 403, request.keep_alive, request.method == "HEAD");
				return;
			}

			FileTimes times;
			readFileTimes(target, true, times);
			const std::time_t modified = static_cast<std::time_t>(times.modify.seconds);
			if (notModified(request, modified)) {
				sendNotModified(socket, request, modified);
				return;
			}

			sendFile(socket, options, target, request, size, modified);
		}

		static void redirectToDir(NetSocket socket, const Request& request) {
			Response redirect;
			redirect.status = 301;
			redirect.keep_alive = request.keep_alive;
			redirect.headers.push_back("Location: " + request.path + "/");
			sendRaw(socket, renderHead(redirect, 0));
		}

		static void serveExisting(NetSocket socket, const HttpdOptions& options, const fs::path& target,
		                          const Request& request) {
			std::error_code ec;
			if (!fs::is_directory(target, ec)) {
				serveFileEntry(socket, options, target, request);
				return;
			}

			if (request.path.empty() || request.path.back() != '/') {
				redirectToDir(socket, request);
				return;
			}

			const fs::path index = indexWithin(options, target);
			if (!index.empty()) serveFileEntry(socket, options, index, request);
			else sendListing(socket, target, request);
		}

		static int base64Value(char c) {
			if (c >= 'A' && c <= 'Z') return c - 'A';
			if (c >= 'a' && c <= 'z') return c - 'a' + 26;
			if (c >= '0' && c <= '9') return c - '0' + 52;
			if (c == '+') return 62;
			if (c == '/') return 63;
			return -1;
		}

		static std::string base64Decode(const std::string& text) {
			std::string out;
			int bits = 0;
			int accumulated = 0;
			for (const char c : text) {
				const int value = base64Value(c);
				if (value < 0) continue;
				accumulated = (accumulated << 6) | value;
				bits += 6;
				if (bits >= 8) {
					bits -= 8;
					out.push_back(static_cast<char>((accumulated >> bits) & 0xFF));
				}
			}

			return out;
		}

		static bool prefixMatches(const std::string& prefix, const std::string& address) {
			if (prefix.empty()) return true;

			const std::size_t slash = prefix.find('/');
			if (slash == std::string::npos) {
				return address == prefix || address.compare(0, prefix.size(), prefix) == 0;
			}

			return address.compare(0, slash, prefix.substr(0, slash)) == 0;
		}

		static bool accessAllowed(const HttpdConfig& config, const std::string& address) {
			for (const AccessRule& rule : config.access) {
				if (prefixMatches(rule.prefix, address)) return rule.allow;
			}

			return true;
		}

		// NTFS names are case-insensitive, so the rule must be too.
		static const AuthRule* authFor(const HttpdConfig& config, const std::string& path) {
			const std::string folded = lowerAscii(path);
			const AuthRule* match = nullptr;
			for (const AuthRule& rule : config.auth) {
				const std::string rule_path = lowerAscii(rule.path);
				if (folded.compare(0, rule_path.size(), rule_path) != 0) continue;
				if (match == nullptr || rule.path.size() > match->path.size()) match = &rule;
			}

			return match;
		}

		static bool authorized(const AuthRule& rule, const std::string& header) {
			const std::size_t space = header.find(' ');
			if (space == std::string::npos || lowerAscii(header.substr(0, space)) != "basic") return false;

			const std::string decoded = base64Decode(header.substr(space + 1));
			const std::size_t colon = decoded.find(':');
			if (colon == std::string::npos) return false;

			return decoded.substr(0, colon) == rule.user && decoded.substr(colon + 1) == rule.password;
		}

		static std::string errorPageBody(const HttpdOptions& options, int status, bool& found) {
			found = false;
			for (const auto& entry : options.config.error_pages) {
				if (entry.first != status) continue;

				std::string local_path;
				fs::path page;
				if (!normalizeRequestPath(entry.second, local_path)) continue;
				if (!resolveTarget(options, local_path, page)) continue;
				std::FILE* file = _wfopen(page.wstring().c_str(), L"rb");
				if (file == nullptr) continue;

				std::string body;
				char buffer[4096];
				std::size_t got = 0;
				while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) body.append(buffer, got);
				std::fclose(file);
				found = true;
				return body;
			}

			return std::string();
		}

		static void sendFailure(NetSocket socket, const HttpdOptions& options, int status,
		                        const Request& request) {
			bool custom = false;
			const std::string body = errorPageBody(options, status, custom);
			if (!custom) {
				sendError(socket, status, request.keep_alive, request.method == "HEAD");
				return;
			}

			Response response;
			response.status = status;
			response.keep_alive = request.keep_alive;
			response.headers.push_back("Content-Type: text/html");
			sendRaw(socket, renderHead(response, body.size()));
			if (request.method != "HEAD") sendRaw(socket, body);
		}

		static void sendUnauthorized(NetSocket socket, const Request& request) {
			const std::string body = "<html><body><h1>401 Unauthorized</h1></body></html>\r\n";
			Response response;
			response.status = 401;
			response.keep_alive = request.keep_alive;
			response.headers.push_back("WWW-Authenticate: Basic realm=\"wbsh-httpd\"");
			response.headers.push_back("Content-Type: text/html");
			sendRaw(socket, renderHead(response, body.size()));
			if (request.method != "HEAD") sendRaw(socket, body);
		}

		static bool isDirectlyExecutable(const fs::path& script) {
			const std::string ext = lowerAscii(pathToUtf8(script.extension()));
			return ext == ".exe" || ext == ".bat" || ext == ".cmd" || ext == ".com";
		}

		static bool hasShebang(const fs::path& script) {
			std::FILE* file = _wfopen(script.wstring().c_str(), L"rb");
			if (file == nullptr) return false;

			char head[2] = { 0, 0 };
			const std::size_t got = std::fread(head, 1, sizeof(head), file);
			std::fclose(file);
			return got == 2 && head[0] == '#' && head[1] == '!';
		}

		// Windows has no execute bit, so the rule busybox gets from it is
		// spelled out: programs (.exe .com .bat .cmd) run as they are,
		// scripts (.sh, .cgi, or a #! first line) run through wbsh, and any
		// other file under cgi-bin is refused rather than run or served.
		static bool isRunnableCgi(const fs::path& script) {
			if (isDirectlyExecutable(script)) return true;

			const std::string ext = lowerAscii(pathToUtf8(script.extension()));
			return ext == ".sh" || ext == ".cgi" || hasShebang(script);
		}

		static std::wstring quoteArg(const std::wstring& arg) {
			return L"\"" + arg + L"\"";
		}

		static std::wstring cgiCommandLine(const fs::path& script) {
			if (isDirectlyExecutable(script)) return quoteArg(script.wstring());
			return quoteArg(utf8ToWide(selfExecutablePath())) + L" -r " + quoteArg(script.wstring());
		}

		struct CgiPaths {
			fs::path script;
			std::string script_name;
			std::string path_info;
			bool is_cgi = false;
			bool found = false;
			bool runnable = false;
		};

		static const std::string kCgiPrefix = "/cgi-bin/";

		static bool isCgiPath(const std::string& path) {
			return lowerAscii(path).compare(0, kCgiPrefix.size(), kCgiPrefix) == 0;
		}

		static bool scriptContained(const HttpdOptions& options, const fs::path& script) {
			std::error_code ec;
			const fs::path bin = fs::weakly_canonical(options.home / "cgi-bin", ec);
			if (ec) return false;

			const fs::path canonical = fs::weakly_canonical(script, ec);
			return !ec && isWithin(bin, canonical) && foldedWide(bin) != foldedWide(canonical);
		}

		// @p path is already decoded and normalised (no "..", ':' or '\\'), so
		// the first segment after /cgi-bin/ is a plain file name.
		static CgiPaths locateCgi(const HttpdOptions& options, const std::string& path) {
			CgiPaths out;
			if (!isCgiPath(path)) return out;

			out.is_cgi = true;
			const std::string rest = path.substr(kCgiPrefix.size());
			const std::size_t slash = rest.find('/');
			const std::string name = rest.substr(0, slash);
			if (name.empty()) return out;

			out.script = (options.home / "cgi-bin" / utf8ToPath(name)).lexically_normal();
			out.script_name = kCgiPrefix + name;
			if (slash != std::string::npos) out.path_info = rest.substr(slash);

			std::error_code ec;
			out.found = scriptContained(options, out.script) && fs::is_regular_file(out.script, ec);
			out.runnable = out.found && isRunnableCgi(out.script);
			return out;
		}

		static std::string headerEnvName(const std::string& name) {
			std::string out = "HTTP_";
			for (const char c : name) out.push_back(c == '-' ? '_'
				: static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
			return out;
		}

		static void addCgiVar(std::vector<std::pair<std::string, std::string>>& env,
		                      const std::string& name, const std::string& value) {
			env.emplace_back(name, value);
		}

		static std::vector<std::pair<std::string, std::string>> cgiVariables(const HttpdOptions& options,
				const Request& request, const CgiPaths& cgi, const std::string& remote) {
			std::vector<std::pair<std::string, std::string>> env;
			addCgiVar(env, "GATEWAY_INTERFACE", "CGI/1.1");
			addCgiVar(env, "SERVER_SOFTWARE", "wbsh-httpd");
			addCgiVar(env, "SERVER_PROTOCOL", "HTTP/1.1");
			addCgiVar(env, "SERVER_NAME", options.bind_addr);
			addCgiVar(env, "SERVER_PORT", options.port);
			addCgiVar(env, "REQUEST_METHOD", request.method);
			addCgiVar(env, "REQUEST_URI", request.target);
			addCgiVar(env, "SCRIPT_NAME", cgi.script_name);
			addCgiVar(env, "PATH_INFO", cgi.path_info);
			addCgiVar(env, "QUERY_STRING", request.query);
			addCgiVar(env, "REMOTE_ADDR", remote);
			if (!request.content_type.empty()) addCgiVar(env, "CONTENT_TYPE", request.content_type);
			if (request.content_length > 0) {
				addCgiVar(env, "CONTENT_LENGTH", std::to_string(request.content_length));
			}

			for (const auto& header : request.headers) {
				if (header.first != "content-type" && header.first != "content-length") {
					addCgiVar(env, headerEnvName(header.first), header.second);
				}
			}

			return env;
		}

		static std::wstring cgiEnvironmentBlock(const std::vector<std::pair<std::string, std::string>>& env) {
			std::wstring block;
			wchar_t* existing = ::GetEnvironmentStringsW();
			for (const wchar_t* scan = existing; existing != nullptr && *scan != L'\0';) {
				const std::wstring entry = scan;
				block += entry;
				block.push_back(L'\0');
				scan += entry.size() + 1;
			}

			if (existing != nullptr) ::FreeEnvironmentStringsW(existing);
			for (const auto& pair : env) {
				block += utf8ToWide(pair.first + "=" + pair.second);
				block.push_back(L'\0');
			}

			block.push_back(L'\0');
			return block;
		}

		struct CgiPipes {
			HANDLE child_stdin_read = nullptr;
			HANDLE our_stdin_write = nullptr;
			HANDLE child_stdout_write = nullptr;
			HANDLE our_stdout_read = nullptr;
		};

		static bool makeCgiPipes(CgiPipes& pipes) {
			SECURITY_ATTRIBUTES attrs{};
			attrs.nLength = sizeof(attrs);
			attrs.bInheritHandle = TRUE;
			if (::CreatePipe(&pipes.child_stdin_read, &pipes.our_stdin_write, &attrs, 0) == 0) return false;
			if (::CreatePipe(&pipes.our_stdout_read, &pipes.child_stdout_write, &attrs, 0) == 0) return false;

			::SetHandleInformation(pipes.our_stdin_write, HANDLE_FLAG_INHERIT, 0);
			::SetHandleInformation(pipes.our_stdout_read, HANDLE_FLAG_INHERIT, 0);
			return true;
		}

		static std::string drainHandle(HANDLE handle) {
			std::string out;
			char buffer[16384];
			DWORD got = 0;
			while (::ReadFile(handle, buffer, sizeof(buffer), &got, nullptr) != 0 && got > 0) {
				out.append(buffer, got);
			}

			return out;
		}

		static void feedChild(HANDLE handle, const std::string& body) {
			std::size_t sent = 0;
			while (sent < body.size()) {
				DWORD put = 0;
				const DWORD chunk = static_cast<DWORD>((std::min)(body.size() - sent,
					static_cast<std::size_t>(16384)));
				if (::WriteFile(handle, body.data() + sent, chunk, &put, nullptr) == 0 || put == 0) break;
				sent += put;
			}

			::CloseHandle(handle);
		}

		static bool launchCgi(const fs::path& script, CgiPipes& pipes, const std::wstring& env, HANDLE& process) {
			std::wstring cmdline = cgiCommandLine(script);
			std::vector<wchar_t> mutable_line(cmdline.begin(), cmdline.end());
			mutable_line.push_back(L'\0');

			STARTUPINFOW startup{};
			startup.cb = sizeof(startup);
			startup.dwFlags = STARTF_USESTDHANDLES;
			startup.hStdInput = pipes.child_stdin_read;
			startup.hStdOutput = pipes.child_stdout_write;
			startup.hStdError = pipes.child_stdout_write;

			const std::wstring workdir = script.parent_path().wstring();
			PROCESS_INFORMATION info{};
			const BOOL ok = ::CreateProcessW(nullptr, mutable_line.data(), nullptr, nullptr, TRUE,
				CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, const_cast<wchar_t*>(env.c_str()),
				workdir.c_str(), &startup, &info);
			if (ok == 0) return false;

			::CloseHandle(info.hThread);
			process = info.hProcess;
			return true;
		}

		// Kills a CGI child that outruns the timeout so no script can spin forever.
		static std::string collectCgi(HANDLE process, CgiPipes& pipes, const std::string& body) {
			::CloseHandle(pipes.child_stdin_read);
			::CloseHandle(pipes.child_stdout_write);

			std::thread writer(feedChild, pipes.our_stdin_write, body);
			std::thread killer([process] {
				if (::WaitForSingleObject(process, kCgiTimeoutMs) == WAIT_TIMEOUT) {
					::TerminateProcess(process, 1);
				}
			});

			const std::string out = drainHandle(pipes.our_stdout_read);
			writer.join();
			::WaitForSingleObject(process, INFINITE);
			killer.join();
			::CloseHandle(pipes.our_stdout_read);
			::CloseHandle(process);
			return out;
		}

		static bool runCgiScript(const HttpdOptions& options, const Request& request, const CgiPaths& cgi,
		                         const std::string& remote, std::string& output) {
			CgiPipes pipes;
			if (!makeCgiPipes(pipes)) return false;

			const std::wstring env = cgiEnvironmentBlock(cgiVariables(options, request, cgi, remote));
			HANDLE process = nullptr;
			if (!launchCgi(cgi.script, pipes, env, process)) {
				::CloseHandle(pipes.child_stdin_read);
				::CloseHandle(pipes.our_stdin_write);
				::CloseHandle(pipes.child_stdout_write);
				::CloseHandle(pipes.our_stdout_read);
				return false;
			}

			output = collectCgi(process, pipes, request.body);
			return true;
		}

		struct CgiOutput {
			int status = 200;
			std::vector<std::string> headers;
			std::string body;
		};

		static void parseCgiHeader(const std::string& line, CgiOutput& out) {
			const std::size_t colon = line.find(':');
			if (colon == std::string::npos) return;

			const std::string name = lowerAscii(line.substr(0, colon));
			std::string value = line.substr(colon + 1);
			while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
			while (!value.empty() && (value.back() == '\r')) value.pop_back();

			if (name == "status") parseInt(value.substr(0, value.find(' ')), out.status);
			else {
				if (name == "location") out.status = out.status == 200 ? 302 : out.status;
				out.headers.push_back(line.substr(0, colon) + ": " + value);
			}
		}

		static std::size_t headerBodySplit(const std::string& raw) {
			const std::size_t crlf = raw.find("\r\n\r\n");
			const std::size_t lf = raw.find("\n\n");
			if (crlf != std::string::npos && (lf == std::string::npos || crlf < lf)) return crlf + 4;
			if (lf != std::string::npos) return lf + 2;
			return 0;
		}

		static CgiOutput parseCgiOutput(const std::string& raw) {
			CgiOutput out;
			const std::size_t split = headerBodySplit(raw);
			const std::string head = split == 0 ? std::string() : raw.substr(0, split);
			out.body = raw.substr(split);

			std::size_t start = 0;
			while (start < head.size()) {
				const std::size_t newline = head.find('\n', start);
				const std::string line = head.substr(start, newline - start);
				if (!line.empty() && line != "\r") parseCgiHeader(line, out);
				if (newline == std::string::npos) break;
				start = newline + 1;
			}

			return out;
		}

		static bool hasContentType(const CgiOutput& out) {
			for (const std::string& header : out.headers) {
				if (lowerAscii(header).compare(0, 13, "content-type:") == 0) return true;
			}

			return false;
		}

		static void sendCgi(NetSocket socket, const Request& request, const CgiOutput& cgi) {
			Response response;
			response.status = cgi.status;
			response.keep_alive = request.keep_alive;
			response.headers = cgi.headers;
			if (!hasContentType(cgi)) response.headers.push_back("Content-Type: text/plain");
			sendRaw(socket, renderHead(response, cgi.body.size()));
			if (request.method != "HEAD") sendRaw(socket, cgi.body);
		}

		static void serveCgi(NetSocket socket, const HttpdOptions& options, const Request& request,
		                     const CgiPaths& cgi, const std::string& remote) {
			std::string output;
			if (!runCgiScript(options, request, cgi, remote, output)) {
				sendFailure(socket, options, 500, request);
				return;
			}

			sendCgi(socket, request, parseCgiOutput(output));
		}

		// Checked against both the normalised request path and its on-disk
		// spelling, so neither %-escapes, case, "./" nor short names slip past.
		static bool passesAuth(NetSocket socket, const HttpdOptions& options, const Request& request,
		                       const std::string& disk_path) {
			const AuthRule* rule = authFor(options.config, request.local_path);
			if (rule == nullptr) rule = authFor(options.config, disk_path);
			if (rule == nullptr) return true;
			if (!request.authorization.empty() && authorized(*rule, request.authorization)) return true;

			sendUnauthorized(socket, request);
			return false;
		}

		static void dispatchCgi(NetSocket socket, const HttpdOptions& options, const Request& request,
		                        const CgiPaths& cgi, const std::string& remote) {
			if (!cgi.found) sendFailure(socket, options, 404, request);
			else if (!cgi.runnable) sendFailure(socket, options, 403, request);
			else serveCgi(socket, options, request, cgi, remote);
		}

		static void serveStatic(NetSocket socket, const HttpdOptions& options, const Request& request) {
			if (request.method != "GET" && request.method != "HEAD") {
				sendFailure(socket, options, 501, request);
				return;
			}

			fs::path target;
			if (!resolveTarget(options, request.local_path, target)) {
				sendFailure(socket, options, 403, request);
				return;
			}

			std::error_code ec;
			if (!fs::exists(target, ec)) sendFailure(socket, options, 404, request);
			else serveExisting(socket, options, target, request);
		}

		static void serveRequest(NetSocket socket, const HttpdOptions& options, const Request& request,
		                         const std::string& remote) {
			if (!accessAllowed(options.config, remote)) {
				sendFailure(socket, options, 403, request);
				return;
			}

			const std::string disk_path = canonicalRelative(options, request.local_path);
			if (!passesAuth(socket, options, request, disk_path)) return;

			if (isCgiPath(request.local_path) || isCgiPath(disk_path)) {
				const std::string& cgi_path = isCgiPath(disk_path) ? disk_path : request.local_path;
				dispatchCgi(socket, options, request, locateCgi(options, cgi_path), remote);
				return;
			}

			serveStatic(socket, options, request);
		}

		static void logRequest(const HttpdOptions& options, const Request& request, const std::string& peer) {
			if (!options.verbose) return;
			std::fprintf(stderr, "httpd: %s \"%s %s\"\n", peer.c_str(), request.method.c_str(),
				request.target.c_str());
			std::fflush(stderr);
		}

		static bool readBody(NetReader& reader, Request& request) {
			if (request.content_length <= 0) return true;
			if (static_cast<std::size_t>(request.content_length) > kBodyLimit) return false;

			while (request.body.size() < static_cast<std::size_t>(request.content_length)) {
				std::string chunk;
				const std::size_t want = static_cast<std::size_t>(request.content_length) - request.body.size();
				if (netReadSome(reader, want, chunk, kRequestTimeoutMs) != NetWait::Ready) return false;
				request.body += chunk;
			}

			return true;
		}

		static std::string remoteAddress(NetSocket socket) {
			std::string host;
			int port = 0;
			return netPeerAddress(socket, host, port) ? host : std::string("127.0.0.1");
		}

		static void handleClient(NetSocket socket, const HttpdOptions& options) {
			const std::string peer = netPeerName(socket);
			const std::string remote = remoteAddress(socket);
			NetReader reader{ socket };
			for (;;) {
				std::string block;
				const NetWait wait = readHeaderBlock(reader, block);
				if (wait != NetWait::Ready && block.empty()) break;

				Request request = parseRequest(block);
				if (!request.valid || !readBody(reader, request)) {
					sendError(socket, 400, false, false);
					break;
				}

				logRequest(options, request, peer);
				if (normalizeRequestPath(request.path, request.local_path)) {
					serveRequest(socket, options, request, remote);
				} else {
					sendFailure(socket, options, 403, request);
				}
				if (!request.keep_alive || wait != NetWait::Ready) break;
			}

			netClose(socket);
		}

		static int runServer(const HttpdOptions& options) {
			NetEndpoint local;
			local.host = options.bind_addr == "0.0.0.0" ? "" : options.bind_addr;
			local.port = options.port;
			local.numeric = !local.host.empty();
			NetError error;
			const NetSocket listener = netListen(local, error);
			if (listener == kNoSocket) {
				perr("httpd", "cannot bind " + options.bind_addr + ":" + options.port + ": "
					+ netErrorText(error.code));
				return 1;
			}

			const ScopedCtrlCCapture capture;
			for (;;) {
				NetWait wait = NetWait::Ready;
				const NetSocket client = netAccept(listener, kAcceptPollMs, wait);
				if (wait == NetWait::Interrupted) break;
				if (wait == NetWait::Timeout) continue;
				if (client == kNoSocket) break;

				std::thread(handleClient, client, options).detach();
			}

			netClose(listener);
			return 0;
		}

		static int daemonize(Executor& exec, const std::vector<std::string>& args) {
			std::vector<std::string> child = { "httpd", "-f" };
			child.insert(child.end(), args.begin(), args.end());

			const HANDLE process = exec.spawnCommand(child, detachedPlacement(false));
			if (process == INVALID_HANDLE_VALUE) return 1;

			::CloseHandle(process);
			return 0;
		}

	}  // namespace httpd_detail

	static int builtin_httpd(Executor& exec, const std::vector<std::string>& args) {
		httpd_detail::HttpdOptions options;
		if (!httpd_detail::parse(exec, args, options)) return 1;

		if (!options.foreground) return httpd_detail::daemonize(exec, args);
		return httpd_detail::runServer(options);
	}

	void registerHttpdBuiltin(Executor& exec) {
		exec.registerBuiltin("httpd", builtin_httpd);
	}

#else

	void registerHttpdBuiltin(Executor&) {}

#endif /* _WIN32 */

}  // namespace wbsh

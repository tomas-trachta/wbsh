#include "curl_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "coreutils_internal.h"
#include "executor.h"
#include "numparse.h"

namespace wbsh {

	namespace curl_detail {

		static std::string lowercase(std::string text) {
			for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return text;
		}

		static bool equalsIgnoreCase(const std::string& a, const std::string& b) {
			return lowercase(a) == lowercase(b);
		}

		static std::string percentDecode(const std::string& text) {
			std::string out;
			for (std::size_t i = 0; i < text.size(); ++i) {
				unsigned long value = 0;
				const bool escape = text[i] == '%' && i + 2 < text.size()
					&& std::isxdigit(static_cast<unsigned char>(text[i + 1]))
					&& std::isxdigit(static_cast<unsigned char>(text[i + 2]));
				if (escape && parseUL(text.substr(i + 1, 2), value, 16)) {
					out.push_back(static_cast<char>(value));
					i += 2;
				} else {
					out.push_back(text[i]);
				}
			}

			return out;
		}

		static int defaultPort(const std::string& scheme) {
			return scheme == "https" ? 443 : 80;
		}

		static void splitUserInfo(const std::string& info, UrlParts& parts) {
			const std::size_t colon = info.find(':');
			parts.user = percentDecode(info.substr(0, colon));
			if (colon != std::string::npos) parts.password = percentDecode(info.substr(colon + 1));
		}

		static bool splitHostPort(const std::string& authority, UrlParts& parts) {
			std::size_t host_end = authority.size();
			std::size_t port_start = std::string::npos;
			if (!authority.empty() && authority[0] == '[') {
				host_end = authority.find(']');
				if (host_end == std::string::npos) return false;
				parts.host = authority.substr(0, host_end + 1);
				if (host_end + 1 < authority.size() && authority[host_end + 1] == ':') port_start = host_end + 2;
			} else {
				const std::size_t colon = authority.rfind(':');
				parts.host = authority.substr(0, colon);
				if (colon != std::string::npos) port_start = colon + 1;
			}

			parts.port = defaultPort(parts.scheme);
			if (port_start == std::string::npos || port_start >= authority.size()) return !parts.host.empty();

			long long port = 0;
			std::size_t consumed = 0;
			const std::string text = authority.substr(port_start);
			if (!parseLL(text, port, 10, &consumed) || consumed != text.size()) return false;
			if (port < 1 || port > 65535) return false;
			parts.port = static_cast<int>(port);
			return !parts.host.empty();
		}

		bool splitUrl(const std::string& url, UrlParts& parts) {
			const std::size_t scheme_end = url.find("://");
			if (scheme_end == std::string::npos) return false;

			parts = UrlParts();
			parts.scheme = lowercase(url.substr(0, scheme_end));
			const std::size_t authority_start = scheme_end + 3;
			const std::size_t authority_end = (std::min)(url.find_first_of("/?#", authority_start), url.size());
			std::string authority = url.substr(authority_start, authority_end - authority_start);

			const std::size_t at = authority.rfind('@');
			if (at != std::string::npos) {
				splitUserInfo(authority.substr(0, at), parts);
				authority = authority.substr(at + 1);
			}

			std::string path = url.substr(authority_end);
			path = path.substr(0, path.find('#'));
			if (path.empty() || path[0] != '/') path.insert(0, "/");
			parts.path = path;
			return splitHostPort(authority, parts);
		}

		static std::string urlOrigin(const std::string& url) {
			const std::size_t scheme_end = url.find("://");
			if (scheme_end == std::string::npos) return url;

			const std::size_t authority_end = url.find_first_of("/?#", scheme_end + 3);
			return url.substr(0, authority_end == std::string::npos ? url.size() : authority_end);
		}

		static bool hasScheme(const std::string& location) {
			const std::size_t scheme_end = location.find("://");
			return scheme_end != std::string::npos && location.find_first_of("/?#") > scheme_end;
		}

		std::string resolveUrl(const std::string& base, const std::string& location) {
			if (hasScheme(location)) return location;
			if (location.rfind("//", 0) == 0) return base.substr(0, base.find("://") + 1) + location;

			const std::string origin = urlOrigin(base);
			if (!location.empty() && location[0] == '/') return origin + location;

			std::string path = base.substr(origin.size());
			path = path.substr(0, path.find_first_of("?#"));
			if (path.empty()) path = "/";
			if (!location.empty() && location[0] == '?') return origin + path + location;
			return origin + path.substr(0, path.rfind('/') + 1) + location;
		}

		std::string urlEncode(const std::string& text) {
			static const char kHex[] = "0123456789ABCDEF";
			std::string out;
			for (unsigned char c : text) {
				if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
					out.push_back(static_cast<char>(c));
					continue;
				}

				out.push_back('%');
				out.push_back(kHex[c >> 4]);
				out.push_back(kHex[c & 15]);
			}

			return out;
		}

		std::string base64Encode(const std::string& text) {
			static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			std::string out;
			for (std::size_t i = 0; i < text.size(); i += 3) {
				unsigned value = static_cast<unsigned char>(text[i]) << 16;
				if (i + 1 < text.size()) value |= static_cast<unsigned char>(text[i + 1]) << 8;
				if (i + 2 < text.size()) value |= static_cast<unsigned char>(text[i + 2]);

				out.push_back(kAlphabet[(value >> 18) & 63]);
				out.push_back(kAlphabet[(value >> 12) & 63]);
				out.push_back(i + 1 < text.size() ? kAlphabet[(value >> 6) & 63] : '=');
				out.push_back(i + 2 < text.size() ? kAlphabet[value & 63] : '=');
			}

			return out;
		}

		std::string lastPathSegment(const std::string& url) {
			UrlParts parts;
			std::string path = splitUrl(url, parts) ? parts.path : url;
			path = path.substr(0, path.find('?'));
			return path.substr(path.rfind('/') + 1);
		}

		static bool headerLine(const std::string& line, const std::string& name, std::string& value) {
			const std::size_t colon = line.find(':');
			if (colon == std::string::npos || !equalsIgnoreCase(line.substr(0, colon), name)) return false;

			const std::size_t start = line.find_first_not_of(" \t", colon + 1);
			value = start == std::string::npos ? std::string() : line.substr(start);
			return true;
		}

		std::vector<std::string> findAllHeaders(const std::string& raw, const std::string& name) {
			std::vector<std::string> values;
			std::size_t start = raw.find("\r\n");
			while (start != std::string::npos && start + 2 < raw.size()) {
				start += 2;
				const std::size_t end = raw.find("\r\n", start);
				const std::string line = raw.substr(start, end == std::string::npos ? std::string::npos : end - start);
				std::string value;
				if (headerLine(line, name, value)) values.push_back(value);
				start = end;
			}

			return values;
		}

		bool findHeader(const std::string& raw, const std::string& name, std::string& value) {
			const std::vector<std::string> values = findAllHeaders(raw, name);
			if (values.empty()) return false;

			value = values.back();
			return true;
		}

		bool readDataSource(Executor& exec, const std::string& name, std::string& data) {
			std::FILE* file = name == "-" ? stdin : fopenNative(exec, name, "rb");
			if (file == nullptr) return false;

			char buffer[16384];
			std::size_t got = 0;
			while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) data.append(buffer, got);

			const bool ok = std::ferror(file) == 0;
			if (file != stdin) std::fclose(file);
			return ok;
		}

		static std::vector<std::string> splitTabs(const std::string& line) {
			std::vector<std::string> fields;
			std::size_t start = 0;
			for (;;) {
				const std::size_t tab = line.find('\t', start);
				fields.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
				if (tab == std::string::npos) return fields;
				start = tab + 1;
			}
		}

		static bool parseCookieLine(std::string line, Cookie& cookie) {
			static const std::string kHttpOnly = "#HttpOnly_";
			cookie.http_only = line.rfind(kHttpOnly, 0) == 0;
			if (cookie.http_only) line = line.substr(kHttpOnly.size());
			if (line.empty() || line[0] == '#') return false;

			const std::vector<std::string> fields = splitTabs(line);
			if (fields.size() < 7) return false;

			cookie.domain = fields[0][0] == '.' ? fields[0].substr(1) : fields[0];
			cookie.subdomains = fields[1] == "TRUE";
			cookie.path = fields[2];
			cookie.secure = fields[3] == "TRUE";
			parseLL(fields[4], cookie.expires);
			cookie.name = fields[5];
			cookie.value = fields[6];
			return true;
		}

		bool loadCookieFile(Executor& exec, const std::string& path, CookieJar& jar) {
			jar.enabled = true;
			std::vector<std::string> lines;
			if (!readAllLines(exec, path, lines)) return false;

			for (const std::string& line : lines) {
				Cookie cookie;
				if (parseCookieLine(line, cookie)) jar.cookies.push_back(cookie);
			}

			return true;
		}

		static std::string cookieFileLine(const Cookie& cookie) {
			std::string line = cookie.http_only ? "#HttpOnly_" : "";
			line += (cookie.subdomains ? "." : "") + cookie.domain + "\t";
			line += std::string(cookie.subdomains ? "TRUE" : "FALSE") + "\t" + cookie.path + "\t";
			line += std::string(cookie.secure ? "TRUE" : "FALSE") + "\t" + std::to_string(cookie.expires) + "\t";
			return line + cookie.name + "\t" + cookie.value + "\n";
		}

		bool saveCookieFile(Executor& exec, const std::string& path, const CookieJar& jar) {
			std::FILE* file = path == "-" ? stdout : fopenNative(exec, path, "wb");
			if (file == nullptr) return false;

			std::fputs("# Netscape HTTP Cookie File\n", file);
			std::fputs("# https://curl.se/docs/http-cookies.html\n", file);
			std::fputs("# This file was generated by libcurl! Edit at your own risk.\n\n", file);
			for (const Cookie& cookie : jar.cookies) std::fputs(cookieFileLine(cookie).c_str(), file);

			const bool ok = std::ferror(file) == 0;
			if (file != stdout) return std::fclose(file) == 0 && ok;
			std::fflush(file);
			return ok;
		}

		static int monthIndex(const std::string& name) {
			static const char* const kMonths[] = {
				"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec",
			};
			for (int i = 0; i < 12; ++i) {
				if (lowercase(name) == kMonths[i]) return i;
			}

			return -1;
		}

		static long long parseHttpDate(const std::string& text) {
			const std::size_t comma = text.find(',');
			const std::string rest = comma == std::string::npos ? text : text.substr(comma + 1);

			int day = 0;
			int year = 0;
			char month[4] = {};
			std::tm tm{};
			const int fields = std::sscanf(rest.c_str(), " %d%*[ -]%3s%*[ -]%d %d:%d:%d",
				&day, month, &year, &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
			if (fields < 3 || monthIndex(month) < 0) return 0;

			tm.tm_mday = day;
			tm.tm_mon = monthIndex(month);
			tm.tm_year = (year < 100 ? year + 2000 : year) - 1900;
			return static_cast<long long>(_mkgmtime(&tm));
		}

		static std::string trimmed(const std::string& text) {
			const std::size_t first = text.find_first_not_of(" \t");
			if (first == std::string::npos) return std::string();
			return text.substr(first, text.find_last_not_of(" \t") - first + 1);
		}

		static void applyCookieAttribute(Cookie& cookie, const std::string& attribute, bool& expired) {
			const std::size_t equals = attribute.find('=');
			const std::string name = lowercase(trimmed(attribute.substr(0, equals)));
			const std::string value = equals == std::string::npos ? std::string()
				: trimmed(attribute.substr(equals + 1));
			long long seconds = 0;

			if (name == "domain" && !value.empty()) {
				cookie.domain = lowercase(value[0] == '.' ? value.substr(1) : value);
				cookie.subdomains = true;
			} else if (name == "path" && !value.empty()) {
				cookie.path = value;
			} else if (name == "secure") {
				cookie.secure = true;
			} else if (name == "httponly") {
				cookie.http_only = true;
			} else if (name == "max-age" && parseLL(value, seconds)) {
				cookie.expires = static_cast<long long>(std::time(nullptr)) + seconds;
				expired = seconds <= 0;
			} else if (name == "expires" && cookie.expires == 0) {
				cookie.expires = parseHttpDate(value);
				expired = cookie.expires != 0 && cookie.expires < static_cast<long long>(std::time(nullptr));
			}
		}

		void storeSetCookie(CookieJar& jar, const std::string& host, const std::string& header_value) {
			std::vector<std::string> parts;
			std::size_t start = 0;
			for (;;) {
				const std::size_t semi = header_value.find(';', start);
				const std::size_t length = semi == std::string::npos ? std::string::npos : semi - start;
				parts.push_back(header_value.substr(start, length));
				if (semi == std::string::npos) break;
				start = semi + 1;
			}

			const std::size_t equals = parts[0].find('=');
			if (equals == std::string::npos) return;

			Cookie cookie;
			cookie.name = trimmed(parts[0].substr(0, equals));
			cookie.value = trimmed(parts[0].substr(equals + 1));
			cookie.domain = lowercase(host);
			bool expired = false;
			for (std::size_t i = 1; i < parts.size(); ++i) applyCookieAttribute(cookie, parts[i], expired);

			auto same = [&cookie](const Cookie& other) {
				return other.name == cookie.name && other.domain == cookie.domain && other.path == cookie.path;
			};
			jar.cookies.erase(std::remove_if(jar.cookies.begin(), jar.cookies.end(), same), jar.cookies.end());
			if (!expired) jar.cookies.push_back(cookie);
		}

		static bool domainMatches(const Cookie& cookie, const std::string& host) {
			const std::string name = lowercase(host);
			if (name == cookie.domain) return true;
			if (!cookie.subdomains || name.size() <= cookie.domain.size()) return false;
			return name.compare(name.size() - cookie.domain.size() - 1, std::string::npos, "." + cookie.domain) == 0;
		}

		std::string cookieHeaderFor(const CookieJar& jar, const std::string& host, const std::string& path,
				bool https) {
			const long long now = static_cast<long long>(std::time(nullptr));
			std::string header;
			for (const Cookie& cookie : jar.cookies) {
				if (!domainMatches(cookie, host) || path.rfind(cookie.path, 0) != 0) continue;
				if ((cookie.secure && !https) || (cookie.expires != 0 && cookie.expires < now)) continue;

				if (!header.empty()) header += "; ";
				header += cookie.name + "=" + cookie.value;
			}

			return header;
		}

	}  // namespace curl_detail

}  // namespace wbsh

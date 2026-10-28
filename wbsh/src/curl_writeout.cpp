#include "curl_internal.h"

#include <cstdio>
#include <string>

namespace wbsh {

	namespace curl_detail {

		struct WriteOutContext {
			const ResponseInfo& info;
			int exit_code;
			const std::string& filename;
			std::FILE* stream;
		};

		static std::string seconds(double value) {
			char text[32];
			std::snprintf(text, sizeof(text), "%.6f", value);
			return text;
		}

		static std::string threeDigits(long value) {
			char text[16];
			std::snprintf(text, sizeof(text), "%03ld", value);
			return text;
		}

		static std::string upper(std::string text) {
			for (char& c : text) {
				if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
			}

			return text;
		}

		static unsigned long long countHeaders(const std::string& raw) {
			unsigned long long count = 0;
			for (std::size_t pos = raw.find("\r\n"); pos != std::string::npos; pos = raw.find("\r\n", pos + 2)) ++count;
			return count >= 2 ? count - 2 : 0;
		}

		static bool timeVariable(const std::string& name, const ResponseInfo& info, std::string& value) {
			if (name == "time_total") value = seconds(info.time_total);
			else if (name == "time_starttransfer") value = seconds(info.time_starttransfer);
			else if (name == "time_namelookup" || name == "time_connect" || name == "time_appconnect"
				|| name == "time_pretransfer" || name == "time_redirect") value = seconds(0);
			else if (name == "speed_download") value = std::to_string(info.time_total > 0
				? static_cast<unsigned long long>(static_cast<double>(info.size_download) / info.time_total) : 0);
			else if (name == "speed_upload") value = std::to_string(info.time_total > 0
				? static_cast<unsigned long long>(static_cast<double>(info.size_upload) / info.time_total) : 0);
			else return false;
			return true;
		}

		static bool sizeVariable(const std::string& name, const ResponseInfo& info, std::string& value) {
			if (name == "size_download") value = std::to_string(info.size_download);
			else if (name == "size_header") value = std::to_string(info.size_header);
			else if (name == "size_upload") value = std::to_string(info.size_upload);
			else if (name == "size_request") value = "0";
			else if (name == "num_redirects") value = std::to_string(info.num_redirects);
			else if (name == "num_headers") value = std::to_string(countHeaders(info.last_headers));
			else return false;
			return true;
		}

		static bool textVariable(const std::string& name, const WriteOutContext& context, std::string& value) {
			const ResponseInfo& info = context.info;
			if (name == "http_code" || name == "response_code") value = threeDigits(info.status);
			else if (name == "http_connect") value = "000";
			else if (name == "url_effective") value = info.effective_url.empty() ? info.url : info.effective_url;
			else if (name == "url") value = info.url;
			else if (name == "redirect_url") value = info.redirect_url;
			else if (name == "content_type") value = info.content_type;
			else if (name == "method") value = info.method;
			else if (name == "scheme") value = upper(info.scheme);
			else if (name == "http_version") value = info.status != 0 ? "1.1" : "0";
			else if (name == "remote_ip") value = info.remote_host;
			else if (name == "remote_port") value = info.remote_port != 0 ? std::to_string(info.remote_port) : "";
			else if (name == "exitcode") value = std::to_string(context.exit_code);
			else if (name == "errormsg") value = info.error;
			else if (name == "filename_effective") value = context.filename;
			else return false;
			return true;
		}

		static bool variableValue(const std::string& name, const WriteOutContext& context, std::string& value) {
			return textVariable(name, context, value) || sizeVariable(name, context.info, value)
				|| timeVariable(name, context.info, value);
		}

		static char escapeChar(char c) {
			switch (c) {
				case 'n':
					return '\n';
				case 'r':
					return '\r';
				case 't':
					return '\t';
				case '\\':
					return '\\';
				default:
					return '\0';
			}
		}

		// Returns false once %{onerror} stops the output.
		static bool expandVariable(const std::string& name, WriteOutContext& context) {
			if (name == "stdout" || name == "stderr") {
				std::fflush(context.stream);
				context.stream = name == "stdout" ? stdout : stderr;
				return true;
			}

			if (name == "onerror") return context.exit_code != 0;

			std::string value;
			if (variableValue(name, context, value)) std::fputs(value.c_str(), context.stream);
			else std::fprintf(stderr, "curl: unknown --write-out variable: '%s'\n", name.c_str());
			return true;
		}

		static void expandHeader(const std::string& name, const WriteOutContext& context) {
			std::string value;
			if (findHeader(context.info.last_headers, name, value)) std::fputs(value.c_str(), context.stream);
		}

		// Expands the %-sequence at @p i; false once the output must stop.
		static bool expandPercent(const std::string& format, std::size_t& i, WriteOutContext& context) {
			if (format.compare(i, 2, "%%") == 0) {
				std::fputc('%', context.stream);
				++i;
				return true;
			}

			const bool header = format.compare(i, 8, "%header{") == 0;
			const std::size_t open = i + (header ? 7 : 1);
			const std::size_t close = format.find('}', open);
			if ((!header && format.compare(i, 2, "%{") != 0) || close == std::string::npos) {
				std::fputc('%', context.stream);
				return true;
			}

			const std::string name = format.substr(open + 1, close - open - 1);
			i = close;
			if (header) {
				expandHeader(name, context);
				return true;
			}

			return expandVariable(name, context);
		}

		static std::string formatText(Executor& exec, const CurlOptions& options) {
			const std::string& format = options.write_out;
			if (format.empty() || format[0] != '@') return format;

			std::string text;
			return readDataSource(exec, format.substr(1), text) ? text : std::string();
		}

		void writeOut(Executor& exec, const CurlOptions& options, const ResponseInfo& info, int exit_code,
				const std::string& filename) {
			const std::string format = formatText(exec, options);
			WriteOutContext context{ info, exit_code, filename, stdout };
			for (std::size_t i = 0; i < format.size(); ++i) {
				const char c = format[i];
				if (c == '%') {
					if (!expandPercent(format, i, context)) break;
					continue;
				}

				const char escaped = c == '\\' && i + 1 < format.size() ? escapeChar(format[i + 1]) : '\0';
				if (escaped != '\0') ++i;
				std::fputc(escaped != '\0' ? escaped : c, context.stream);
			}

			std::fflush(stdout);
			std::fflush(stderr);
		}

	}  // namespace curl_detail

}  // namespace wbsh

#include "curl_internal.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include "coreutils_internal.h"
#include "executor.h"

namespace wbsh {

	namespace curl_detail {

		struct FormPart {
			std::string name;
			std::string value;
			std::string file;
			std::string filename;
			std::string type;
			bool attach = false;
			bool content_from_file = false;
		};

		static int readFailure(const std::string& name, std::string& error) {
			error = "Failed to open/read local data from file/application: " + name;
			return kExitRead;
		}

		static std::string stripNewlines(const std::string& text) {
			std::string out;
			for (char c : text) {
				if (c != '\r' && c != '\n') out.push_back(c);
			}

			return out;
		}

		static bool loadAt(Executor& exec, const std::string& text, std::string& out) {
			return readDataSource(exec, text.substr(1), out);
		}

		static int urlEncodedPart(Executor& exec, const std::string& text, std::string& out, std::string& error) {
			const std::size_t at = text.find('@');
			const std::size_t equals = text.find('=');
			const bool from_file = at != std::string::npos && (equals == std::string::npos || at < equals);
			if (from_file) {
				std::string content;
				if (!readDataSource(exec, text.substr(at + 1), content)) return readFailure(text.substr(at + 1), error);
				out = (at > 0 ? text.substr(0, at) + "=" : std::string()) + urlEncode(content);
				return 0;
			}

			if (equals == std::string::npos) out = urlEncode(text);
			else out = (equals > 0 ? text.substr(0, equals) + "=" : std::string()) + urlEncode(text.substr(equals + 1));
			return 0;
		}

		static int dataPart(Executor& exec, const DataArg& arg, std::string& out, std::string& error) {
			const bool from_file = !arg.text.empty() && arg.text[0] == '@' && arg.kind != DataKind::Raw;
			switch (arg.kind) {
				case DataKind::UrlEncode:
					return urlEncodedPart(exec, arg.text, out, error);
				case DataKind::Ascii:
					if (from_file && !loadAt(exec, arg.text, out)) return readFailure(arg.text.substr(1), error);
					out = stripNewlines(from_file ? out : arg.text);
					return 0;
				default:
					if (from_file && !loadAt(exec, arg.text, out)) return readFailure(arg.text.substr(1), error);
					if (!from_file) out = arg.text;
					return 0;
			}
		}

		static int joinData(Executor& exec, const CurlOptions& options, std::string& body, std::string& error) {
			for (const DataArg& arg : options.data) {
				std::string part;
				const int status = dataPart(exec, arg, part, error);
				if (status != 0) return status;

				if (!body.empty()) body.push_back('&');
				body += part;
			}

			return 0;
		}

		static std::string mimeTypeFor(const std::string& name) {
			static const std::pair<const char*, const char*> kTypes[] = {
				{ ".txt", "text/plain" }, { ".html", "text/html" }, { ".htm", "text/html" },
				{ ".json", "application/json" }, { ".xml", "application/xml" }, { ".gif", "image/gif" },
				{ ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" }, { ".png", "image/png" },
				{ ".svg", "image/svg+xml" }, { ".pdf", "application/pdf" },
			};
			const std::size_t dot = name.rfind('.');
			if (dot == std::string::npos) return "application/octet-stream";

			std::string extension = name.substr(dot);
			for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			for (const auto& entry : kTypes) {
				if (extension == entry.first) return entry.second;
			}

			return "application/octet-stream";
		}

		static void applyFormModifier(const std::string& modifier, FormPart& part) {
			if (modifier.rfind("type=", 0) == 0) part.type = modifier.substr(5);
			else if (modifier.rfind("filename=", 0) == 0) part.filename = modifier.substr(9);
		}

		static void splitFormValue(const std::string& text, FormPart& part, std::string& value) {
			std::size_t end = text.find(';');
			if (!text.empty() && text[0] == '"') {
				const std::size_t quote = text.find('"', 1);
				value = text.substr(1, quote == std::string::npos ? std::string::npos : quote - 1);
				end = quote == std::string::npos ? std::string::npos : text.find(';', quote);
			} else {
				value = text.substr(0, end);
			}

			while (end != std::string::npos) {
				const std::size_t next = text.find(';', end + 1);
				const std::size_t length = next == std::string::npos ? std::string::npos : next - end - 1;
				applyFormModifier(text.substr(end + 1, length), part);
				end = next;
			}
		}

		static bool parseFormPart(const DataArg& arg, FormPart& part) {
			const std::size_t equals = arg.text.find('=');
			if (equals == std::string::npos) return false;

			part.name = arg.text.substr(0, equals);
			const std::string rest = arg.text.substr(equals + 1);
			if (arg.kind == DataKind::Raw) {
				part.value = rest;
				return true;
			}

			part.attach = !rest.empty() && rest[0] == '@';
			part.content_from_file = !rest.empty() && rest[0] == '<';
			std::string value;
			splitFormValue(part.attach || part.content_from_file ? rest.substr(1) : rest, part, value);
			if (part.attach || part.content_from_file) part.file = value;
			else part.value = value;
			return true;
		}

		static std::string randomBoundary() {
			static const char kHex[] = "0123456789abcdef";
			std::random_device device;
			std::string boundary(24, '-');
			for (int i = 0; i < 16; ++i) boundary.push_back(kHex[device() & 15u]);
			return boundary;
		}

		static std::string partHeader(const FormPart& part) {
			std::string header = "Content-Disposition: form-data; name=\"" + part.name + "\"";
			if (part.attach) {
				const std::string filename = part.filename.empty()
					? std::filesystem::u8path(part.file).filename().u8string() : part.filename;
				header += "; filename=\"" + filename + "\"";
			}

			header += "\r\n";
			if (!part.type.empty()) header += "Content-Type: " + part.type + "\r\n";
			else if (part.attach) header += "Content-Type: " + mimeTypeFor(part.file) + "\r\n";
			return header;
		}

		static int appendFormPart(Executor& exec, const DataArg& arg, const std::string& boundary,
				std::string& body, std::string& error) {
			FormPart part;
			if (!parseFormPart(arg, part)) {
				error = "Illegally formatted input field";
				return kExitUsage;
			}

			if (!part.file.empty() && !readDataSource(exec, part.file, part.value)) {
				return readFailure(part.file, error);
			}

			body += "--" + boundary + "\r\n" + partHeader(part) + "\r\n";
			body += part.value + "\r\n";
			return 0;
		}

		static int buildMultipart(Executor& exec, const CurlOptions& options, Request& request, std::string& error) {
			const std::string boundary = randomBoundary();
			for (const DataArg& arg : options.form) {
				const int status = appendFormPart(exec, arg, boundary, request.body, error);
				if (status != 0) return status;
			}

			request.body += "--" + boundary + "--\r\n";
			request.headers.push_back("Content-Type: multipart/form-data; boundary=" + boundary);
			return 0;
		}

		static bool hasHeader(const CurlOptions& options, const char* name) {
			const std::size_t length = std::char_traits<char>::length(name);
			for (const std::string& header : options.headers) {
				const bool named = header.size() > length && _strnicmp(header.c_str(), name, length) == 0;
				if (named && header[length] == ':') return true;
			}

			return false;
		}

		static bool anyJson(const CurlOptions& options) {
			for (const DataArg& arg : options.data) {
				if (arg.kind == DataKind::Json) return true;
			}

			return false;
		}

		static void addDataHeaders(const CurlOptions& options, Request& request) {
			const bool json = anyJson(options);
			if (!hasHeader(options, "Content-Type")) {
				request.headers.push_back(json ? "Content-Type: application/json"
					: "Content-Type: application/x-www-form-urlencoded");
			}

			if (json && !hasHeader(options, "Accept")) request.headers.push_back("Accept: application/json");
		}

		static int prepareUpload(Executor& exec, const CurlOptions& options, Request& request, std::string& error) {
			const std::string& file = options.upload_file;
			if (!request.url.empty() && request.url.back() == '/') {
				request.url += std::filesystem::u8path(file).filename().u8string();
			}

			request.has_body = true;
			if (file == "-") return readDataSource(exec, file, request.body) ? 0 : readFailure(file, error);

			std::error_code ec;
			const auto size = std::filesystem::file_size(toNative(exec, file), ec);
			if (ec) return readFailure(file, error);

			request.upload_path = file;
			request.upload_size = static_cast<unsigned long long>(size);
			return 0;
		}

		static std::string defaultMethod(const CurlOptions& options, const Request& request) {
			if (options.head) return "HEAD";
			if (!options.upload_file.empty()) return "PUT";
			return request.has_body ? "POST" : "GET";
		}

		static int attachBody(Executor& exec, const CurlOptions& options, Request& request, std::string& error) {
			if (!options.upload_file.empty()) return prepareUpload(exec, options, request, error);
			if (!options.form.empty()) {
				request.has_body = true;
				return buildMultipart(exec, options, request, error);
			}

			if (options.data.empty()) return 0;

			std::string data;
			const int status = joinData(exec, options, data, error);
			if (status != 0) return status;

			if (options.get_mode) {
				request.url += (request.url.find('?') == std::string::npos ? "?" : "&") + data;
				return 0;
			}

			request.body = data;
			request.has_body = true;
			addDataHeaders(options, request);
			return 0;
		}

		int buildRequest(Executor& exec, const CurlOptions& options, const std::string& url, Request& request,
				std::string& error) {
			request.url = url;
			const int status = attachBody(exec, options, request, error);
			if (status != 0) return status;

			request.method = options.method.empty() ? defaultMethod(options, request) : options.method;
			request.headers.insert(request.headers.end(), options.headers.begin(), options.headers.end());
			const bool literal_cookie = options.cookie.find('=') != std::string::npos;
			if (literal_cookie) request.cookie = options.cookie;
			return 0;
		}

	}  // namespace curl_detail

}  // namespace wbsh

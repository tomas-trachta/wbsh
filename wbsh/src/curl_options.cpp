#include "curl_internal.h"

#include <cstring>
#include <string>
#include <vector>

#include "coreutils_internal.h"
#include "executor.h"
#include "numparse.h"

namespace wbsh {

	namespace curl_detail {

		enum class Opt {
			Silent, ShowError, Verbose, Include, Head, Fail, FailWithBody, CreateDirs, RemoteName,
			RemoteNameAll, Location, Insecure, Compressed, Get, RetryAllErrors, RetryConnrefused,
			Output, OutputDir, DumpHeader, WriteOut, Request, UserAgent, Referer, User, Cookie,
			CookieJar, Range, Proxy, Noproxy, UploadFile, ContinueAt, Header, Data, DataAscii,
			DataBinary, DataRaw, DataUrlencode, Json, Form, FormString, Url, MaxRedirs, MaxTime,
			ConnectTimeout, Retry, RetryDelay, RetryMaxTime, Config, Help, Version, Ignored, IgnoredValue,
		};

		struct OptionSpec {
			char short_name;
			const char* long_name;
			Opt id;
			bool takes_value;
		};

		static const OptionSpec kOptions[] = {
			{ 's', "silent", Opt::Silent, false },
			{ 'S', "show-error", Opt::ShowError, false },
			{ 'v', "verbose", Opt::Verbose, false },
			{ 'i', "include", Opt::Include, false },
			{ 'I', "head", Opt::Head, false },
			{ 'f', "fail", Opt::Fail, false },
			{ 0, "fail-with-body", Opt::FailWithBody, false },
			{ 0, "create-dirs", Opt::CreateDirs, false },
			{ 'O', "remote-name", Opt::RemoteName, false },
			{ 0, "remote-name-all", Opt::RemoteNameAll, false },
			{ 'L', "location", Opt::Location, false },
			{ 0, "location-trusted", Opt::Location, false },
			{ 'k', "insecure", Opt::Insecure, false },
			{ 0, "compressed", Opt::Compressed, false },
			{ 'G', "get", Opt::Get, false },
			{ 0, "retry-all-errors", Opt::RetryAllErrors, false },
			{ 0, "retry-connrefused", Opt::RetryConnrefused, false },
			{ 'o', "output", Opt::Output, true },
			{ 0, "output-dir", Opt::OutputDir, true },
			{ 'D', "dump-header", Opt::DumpHeader, true },
			{ 'w', "write-out", Opt::WriteOut, true },
			{ 'X', "request", Opt::Request, true },
			{ 'A', "user-agent", Opt::UserAgent, true },
			{ 'e', "referer", Opt::Referer, true },
			{ 'u', "user", Opt::User, true },
			{ 'b', "cookie", Opt::Cookie, true },
			{ 'c', "cookie-jar", Opt::CookieJar, true },
			{ 'r', "range", Opt::Range, true },
			{ 'x', "proxy", Opt::Proxy, true },
			{ 0, "noproxy", Opt::Noproxy, true },
			{ 'T', "upload-file", Opt::UploadFile, true },
			{ 'C', "continue-at", Opt::ContinueAt, true },
			{ 'H', "header", Opt::Header, true },
			{ 'd', "data", Opt::Data, true },
			{ 0, "data-ascii", Opt::DataAscii, true },
			{ 0, "data-binary", Opt::DataBinary, true },
			{ 0, "data-raw", Opt::DataRaw, true },
			{ 0, "data-urlencode", Opt::DataUrlencode, true },
			{ 0, "json", Opt::Json, true },
			{ 'F', "form", Opt::Form, true },
			{ 0, "form-string", Opt::FormString, true },
			{ 0, "url", Opt::Url, true },
			{ 0, "max-redirs", Opt::MaxRedirs, true },
			{ 'm', "max-time", Opt::MaxTime, true },
			{ 0, "connect-timeout", Opt::ConnectTimeout, true },
			{ 0, "retry", Opt::Retry, true },
			{ 0, "retry-delay", Opt::RetryDelay, true },
			{ 0, "retry-max-time", Opt::RetryMaxTime, true },
			{ 'K', "config", Opt::Config, true },
			{ 'h', "help", Opt::Help, false },
			{ 'V', "version", Opt::Version, false },
			{ '#', "progress-bar", Opt::Ignored, false },
			{ 0, "no-progress-meter", Opt::Ignored, false },
			{ 'N', "no-buffer", Opt::Ignored, false },
			{ 'g', "globoff", Opt::Ignored, false },
			{ '4', "ipv4", Opt::Ignored, false },
			{ '6', "ipv6", Opt::Ignored, false },
			{ '0', "http1.0", Opt::Ignored, false },
			{ 0, "http1.1", Opt::Ignored, false },
			{ 0, "http2", Opt::Ignored, false },
			{ 0, "tlsv1", Opt::Ignored, false },
			{ 0, "tlsv1.0", Opt::Ignored, false },
			{ 0, "tlsv1.1", Opt::Ignored, false },
			{ 0, "tlsv1.2", Opt::Ignored, false },
			{ 0, "tlsv1.3", Opt::Ignored, false },
			{ 0, "ssl-reqd", Opt::Ignored, false },
			{ 0, "tcp-nodelay", Opt::Ignored, false },
			{ 0, "path-as-is", Opt::Ignored, false },
			{ 0, "raw", Opt::Ignored, false },
			{ 'Z', "parallel", Opt::Ignored, false },
			{ 0, "fail-early", Opt::Ignored, false },
			{ 0, "styled-output", Opt::Ignored, false },
			{ 'q', "disable", Opt::Ignored, false },
			{ 0, "cacert", Opt::IgnoredValue, true },
			{ 0, "capath", Opt::IgnoredValue, true },
			{ 0, "parallel-max", Opt::IgnoredValue, true },
			{ 0, "keepalive-time", Opt::IgnoredValue, true },
			{ 0, "expect100-timeout", Opt::IgnoredValue, true },
		};

		static const char kHelp[] =
			"Usage: curl [options...] <url>...\n"
			" -A, --user-agent <name>   Send User-Agent <name> to server\n"
			" -b, --cookie <data|file>  Send cookies from string/file\n"
			" -c, --cookie-jar <file>   Write cookies to <file> after operation\n"
			" -C, --continue-at <off>   Resumed transfer offset (- = from the output size)\n"
			" -d, --data <data>         HTTP POST data (@file, @- for stdin)\n"
			"     --data-binary <data>  HTTP POST binary data\n"
			"     --data-raw <data>     HTTP POST data, '@' allowed\n"
			"     --data-urlencode <d>  HTTP POST data URL encoded\n"
			" -D, --dump-header <file>  Write the received headers to <file>\n"
			" -f, --fail                Fail fast with no output on HTTP errors\n"
			" -F, --form <name=content> Specify multipart MIME data\n"
			" -G, --get                 Put the post data in the URL and use GET\n"
			" -H, --header <header>     Pass custom header(s) to server\n"
			" -i, --include             Include response headers in the output\n"
			" -I, --head                Show document info only\n"
			"     --json <data>         HTTP POST JSON\n"
			" -k, --insecure            Allow insecure server connections\n"
			" -L, --location            Follow redirects\n"
			" -m, --max-time <seconds>  Maximum time allowed for each transfer\n"
			"     --connect-timeout <s> Maximum time allowed for connection\n"
			" -o, --output <file>       Write to file instead of stdout\n"
			" -O, --remote-name         Write output to a file named as the remote file\n"
			" -r, --range <range>       Retrieve only the bytes within RANGE\n"
			"     --retry <num>         Retry request if transient problems occur\n"
			" -s, --silent              Silent mode\n"
			" -S, --show-error          Show error even when -s is used\n"
			" -T, --upload-file <file>  Transfer local FILE to destination\n"
			" -u, --user <user:pass>    Server user and password\n"
			" -v, --verbose             Make the operation more talkative\n"
			" -w, --write-out <format>  Use output FORMAT after completion\n"
			" -x, --proxy <host:port>   Use this proxy\n"
			" -X, --request <method>    Specify request method to use\n";

		static const char kVersion[] =
			"curl 8.10.1 (wbsh) WinHTTP\n"
			"Protocols: file http https\n"
			"Features: IPv6 Largefile libz SSL\n";

		static int usageFailure(const std::string& message) {
			std::fprintf(stderr, "curl: %s\n", message.c_str());
			std::fprintf(stderr, "curl: try 'curl --help' or 'curl --manual' for more information\n");
			return kExitUsage;
		}

		static const OptionSpec* findLong(const std::string& name) {
			for (const OptionSpec& spec : kOptions) {
				if (name == spec.long_name) return &spec;
			}

			return nullptr;
		}

		static const OptionSpec* findShort(char c) {
			for (const OptionSpec& spec : kOptions) {
				if (spec.short_name == c) return &spec;
			}

			return nullptr;
		}

		static bool parseSeconds(const std::string& text, double& out) {
			std::size_t consumed = 0;
			return parseDouble(text, out, &consumed) && consumed == text.size() && out >= 0;
		}

		static bool parseCount(const std::string& text, long long& out) {
			std::size_t consumed = 0;
			return parseLL(text, out, 10, &consumed) && consumed == text.size();
		}

		static bool readHeaderFile(Executor& exec, const std::string& path, std::vector<std::string>& headers) {
			std::vector<std::string> lines;
			if (!readAllLines(exec, path, lines)) return false;

			for (const std::string& line : lines) {
				if (!line.empty()) headers.push_back(line);
			}

			return true;
		}

		static void applyFlag(Opt id, bool on, CurlOptions& options) {
			switch (id) {
				case Opt::Silent:
					options.silent = on;
					break;
				case Opt::ShowError:
					options.show_error = on;
					break;
				case Opt::Verbose:
					options.verbose = on;
					break;
				case Opt::Include:
					options.include = on;
					break;
				case Opt::Head:
					options.head = on;
					break;
				case Opt::Fail:
					options.fail = on;
					break;
				case Opt::FailWithBody:
					options.fail_with_body = on;
					break;
				case Opt::CreateDirs:
					options.create_dirs = on;
					break;
				case Opt::RemoteName:
					options.outputs.push_back({ std::string(), true });
					break;
				case Opt::RemoteNameAll:
					options.remote_name_all = on;
					break;
				case Opt::Location:
					options.location = on;
					break;
				case Opt::Insecure:
					options.insecure = on;
					break;
				case Opt::Compressed:
					options.compressed = on;
					break;
				case Opt::Get:
					options.get_mode = on;
					break;
				case Opt::RetryAllErrors:
					options.retry_all_errors = on;
					break;
				case Opt::RetryConnrefused:
					options.retry_connrefused = on;
					break;
				default:
					break;
			}
		}

		static bool applyText(Opt id, const std::string& value, CurlOptions& options) {
			switch (id) {
				case Opt::Output:
					options.outputs.push_back({ value, false });
					return true;
				case Opt::OutputDir:
					options.output_dir = value;
					return true;
				case Opt::DumpHeader:
					options.dump_header = value;
					return true;
				case Opt::WriteOut:
					options.write_out = value;
					return true;
				case Opt::Request:
					options.method = value;
					return true;
				case Opt::Referer:
					options.referer = value;
					return true;
				case Opt::User:
					options.user = value;
					return true;
				case Opt::Cookie:
					options.cookie = value;
					return true;
				case Opt::CookieJar:
					options.cookie_jar = value;
					return true;
				case Opt::Range:
					options.range = value;
					return true;
				case Opt::UploadFile:
					options.upload_file = value;
					return true;
				case Opt::Url:
					options.urls.push_back(value);
					return true;
				case Opt::IgnoredValue:
					return true;
				default:
					return false;
			}
		}

		static bool applyData(Opt id, const std::string& value, CurlOptions& options) {
			switch (id) {
				case Opt::Data:
					options.data.push_back({ DataKind::Ascii, value });
					return true;
				case Opt::DataAscii:
					options.data.push_back({ DataKind::Ascii, value });
					return true;
				case Opt::DataBinary:
					options.data.push_back({ DataKind::Binary, value });
					return true;
				case Opt::DataRaw:
					options.data.push_back({ DataKind::Raw, value });
					return true;
				case Opt::DataUrlencode:
					options.data.push_back({ DataKind::UrlEncode, value });
					return true;
				case Opt::Json:
					options.data.push_back({ DataKind::Json, value });
					return true;
				case Opt::Form:
					options.form.push_back({ DataKind::Binary, value });
					return true;
				case Opt::FormString:
					options.form.push_back({ DataKind::Raw, value });
					return true;
				default:
					return false;
			}
		}

		static bool applyNumber(Opt id, const std::string& value, CurlOptions& options) {
			long long count = 0;
			switch (id) {
				case Opt::MaxTime:
					return parseSeconds(value, options.max_time);
				case Opt::ConnectTimeout:
					return parseSeconds(value, options.connect_timeout);
				case Opt::RetryDelay:
					return parseSeconds(value, options.retry_delay);
				case Opt::RetryMaxTime:
					return parseSeconds(value, options.retry_max_time);
				case Opt::MaxRedirs:
					if (!parseCount(value, count)) return false;
					options.max_redirs = count;
					return true;
				case Opt::Retry:
					if (!parseCount(value, count) || count < 0 || count > 100000) return false;
					options.retry = static_cast<int>(count);
					return true;
				default:
					return false;
			}
		}

		static bool validContinueAt(const std::string& value) {
			long long offset = 0;
			return value == "-" || (parseCount(value, offset) && offset >= 0);
		}

		struct ParseState {
			Executor& exec;
			CurlOptions& options;
			int depth = 0;
		};

		static int parseArgList(ParseState& state, const std::vector<std::string>& args);

		static int applyConfig(ParseState& state, const std::string& path);

		static int applySpecial(ParseState& state, const OptionSpec& spec, const std::string& name,
				const std::string& value) {
			CurlOptions& options = state.options;
			switch (spec.id) {
				case Opt::Header:
					if (value.empty() || value[0] != '@') options.headers.push_back(value);
					else if (!readHeaderFile(state.exec, value.substr(1), options.headers)) {
						return usageFailure("option " + name + ": error reading " + value.substr(1));
					}
					return -1;
				case Opt::UserAgent:
					options.user_agent = value;
					options.user_agent_set = true;
					return -1;
				case Opt::Proxy:
					options.proxy = value;
					options.proxy_set = true;
					return -1;
				case Opt::Noproxy:
					options.noproxy = value;
					options.noproxy_set = true;
					return -1;
				case Opt::ContinueAt:
					if (!validContinueAt(value)) {
						return usageFailure("option " + name + ": expected a proper numerical parameter");
					}

					options.continue_at = value;
					return -1;
				case Opt::Config:
					return applyConfig(state, value);
				default:
					break;
			}

			if (applyText(spec.id, value, options) || applyData(spec.id, value, options)) return -1;
			if (applyNumber(spec.id, value, options)) return -1;
			return usageFailure("option " + name + ": expected a proper numerical parameter");
		}

		static int applyOption(ParseState& state, const OptionSpec& spec, bool on, const std::string& name,
				const std::string& value) {
			if (spec.id == Opt::Help) {
				std::fputs(kHelp, stdout);
				return 0;
			}

			if (spec.id == Opt::Version) {
				std::fputs(kVersion, stdout);
				return 0;
			}

			if (!spec.takes_value) {
				applyFlag(spec.id, on, state.options);
				return -1;
			}

			return applySpecial(state, spec, name, value);
		}

		static int parseLongOption(ParseState& state, const std::vector<std::string>& args, std::size_t& i) {
			const std::string name = args[i];
			const std::string bare = name.substr(2);
			bool on = true;
			const OptionSpec* spec = findLong(bare);
			if (spec == nullptr && bare.rfind("no-", 0) == 0) {
				spec = findLong(bare.substr(3));
				on = false;
				if (spec != nullptr && spec->takes_value) spec = nullptr;
			}

			if (spec == nullptr) return usageFailure("option " + name + ": is unknown");
			if (!spec->takes_value) return applyOption(state, *spec, on, name, std::string());
			if (i + 1 >= args.size()) return usageFailure("option " + name + ": requires parameter");
			return applyOption(state, *spec, on, name, args[++i]);
		}

		static int parseShortOptions(ParseState& state, const std::vector<std::string>& args, std::size_t& i) {
			const std::string arg = args[i];
			for (std::size_t k = 1; k < arg.size(); ++k) {
				const OptionSpec* spec = findShort(arg[k]);
				const std::string name = std::string("-") + arg[k];
				if (spec == nullptr) return usageFailure("option " + name + ": is unknown");
				if (!spec->takes_value) {
					const int status = applyOption(state, *spec, true, name, std::string());
					if (status >= 0) return status;
					continue;
				}

				if (k + 1 < arg.size()) return applyOption(state, *spec, true, name, arg.substr(k + 1));
				if (i + 1 >= args.size()) return usageFailure("option " + name + ": requires parameter");
				return applyOption(state, *spec, true, name, args[++i]);
			}

			return -1;
		}

		static int parseArgList(ParseState& state, const std::vector<std::string>& args) {
			for (std::size_t i = 0; i < args.size(); ++i) {
				const std::string& arg = args[i];
				int status = -1;
				if (arg.size() > 2 && arg.compare(0, 2, "--") == 0) status = parseLongOption(state, args, i);
				else if (arg.size() > 1 && arg[0] == '-') status = parseShortOptions(state, args, i);
				else state.options.urls.push_back(arg);
				if (status >= 0) return status;
			}

			return -1;
		}

		static std::string trimBlank(const std::string& text) {
			const std::size_t first = text.find_first_not_of(" \t\r");
			if (first == std::string::npos) return std::string();
			const std::size_t last = text.find_last_not_of(" \t\r");
			return text.substr(first, last - first + 1);
		}

		static std::string unquoteConfigValue(const std::string& text) {
			if (text.size() < 2 || text.front() != '"' || text.back() != '"') return text;

			std::string value;
			for (std::size_t i = 1; i + 1 < text.size(); ++i) {
				if (text[i] == '\\' && i + 2 < text.size()) ++i;
				value.push_back(text[i]);
			}

			return value;
		}

		// A config line is "option value", "option = value" or "option: value",
		// with or without the leading dashes.
		static void configLineArgs(const std::string& raw, std::vector<std::string>& args) {
			const std::string line = trimBlank(raw);
			if (line.empty() || line[0] == '#') return;

			const std::size_t split = line.find_first_of(" \t=:");
			std::string name = line.substr(0, split);
			if (name[0] != '-') name = (name.size() == 1 ? "-" : "--") + name;
			args.push_back(name);
			if (split == std::string::npos) return;

			std::string value = trimBlank(line.substr(split));
			if (!value.empty() && (value[0] == '=' || value[0] == ':')) value = trimBlank(value.substr(1));
			if (!value.empty()) args.push_back(unquoteConfigValue(value));
		}

		static int applyConfig(ParseState& state, const std::string& path) {
			std::vector<std::string> lines;
			if (state.depth > 8 || !readAllLines(state.exec, path, lines)) {
				return usageFailure("cannot read config from '" + path + "'");
			}

			std::vector<std::string> args;
			for (const std::string& line : lines) configLineArgs(line, args);

			++state.depth;
			const int status = parseArgList(state, args);
			--state.depth;
			return status;
		}

		int parseCurlArgs(Executor& exec, const std::vector<std::string>& args, CurlOptions& options) {
			ParseState state{ exec, options };
			const int status = parseArgList(state, args);
			if (status >= 0) return status;
			if (options.urls.empty()) return usageFailure("no URL specified!");
			return -1;
		}

	}  // namespace curl_detail

}  // namespace wbsh

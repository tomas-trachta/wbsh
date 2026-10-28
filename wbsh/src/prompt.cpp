/**
 * @file prompt.cpp
 * @brief Prompt escapes, the git segment, and promptvars expansion.
 */

#include "prompt.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>

#  include <io.h>
#endif /* _WIN32 */

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>

#include "environment.h"
#include "executor.h"
#include "gitprompt.h"
#include "pathconv.h"

#if !defined(WBSH_VERSION_MAJOR) || !defined(WBSH_VERSION_MINOR) || !defined(WBSH_VERSION_PATCH)
#define WBSH_VERSION_MAJOR 0
#define WBSH_VERSION_MINOR 0
#define WBSH_VERSION_PATCH 0
#endif
#define WBSH_PROMPT_VSTR_(x) #x
#define WBSH_PROMPT_VSTR(x)  WBSH_PROMPT_VSTR_(x)

namespace wbsh {

	namespace fs = std::filesystem;

	namespace prompt_detail {

		static const char* const kResetStyle = "\x1b[0m";
		static const char* const kBranchStyle = "\x1b[33;1m";
		static const char* const kShortVersion =
			WBSH_PROMPT_VSTR(WBSH_VERSION_MAJOR) "." WBSH_PROMPT_VSTR(WBSH_VERSION_MINOR);
		static const char* const kLongVersion =
			WBSH_PROMPT_VSTR(WBSH_VERSION_MAJOR) "." WBSH_PROMPT_VSTR(WBSH_VERSION_MINOR) "."
			WBSH_PROMPT_VSTR(WBSH_VERSION_PATCH);

		// readline's marks around the prompt bytes that take no cells,
		// which the line editor measures the prompt by.
		static const char kStartIgnore = '\x01';
		static const char kEndIgnore = '\x02';

		static bool stdoutIsTty() {
#ifdef _WIN32
			return _isatty(_fileno(stdout)) != 0;
#else
			return false;
#endif /* _WIN32 */
		}

		static bool stdinIsTty() {
#ifdef _WIN32
			return _isatty(_fileno(stdin)) != 0;
#else
			return false;
#endif /* _WIN32 */
		}

		static const char* gitStateColor(const std::string& state) {
			if (state == "merging" ||
			    state == "rebasing" ||
			    state == "cherry-picking" ||
			    state == "reverting")  return "\x1b[31;1m";
			if (state == "bisecting")  return "\x1b[35;1m";
			if (state == "unstaged")   return "\x1b[31m";
			if (state == "staged")     return "\x1b[32m";
			if (state == "untracked")  return "\x1b[36m";
			return "";
		}

		static std::string currentCwdAsUtf8() {
			std::error_code ec;
			const fs::path here = fs::current_path(ec);
			return ec ? std::string(".") : pathToUtf8(here);
		}

		static std::string promptUser(const Environment& env) {
			std::string user = env.get("USER");
			if (user.empty()) user = env.get("USERNAME");
			return user;
		}

		static std::string promptHost(const Environment& env, char form) {
			std::string host = env.get("HOSTNAME");
			if (host.empty()) host = env.get("COMPUTERNAME");
			if (form == 'h') {
				const std::size_t dot = host.find('.');
				if (dot != std::string::npos) host.resize(dot);
			}

			return host;
		}

		static bool underHome(const std::string& posix, const std::string& home) {
			return !home.empty() && posix.size() >= home.size()
			    && posix.compare(0, home.size(), home) == 0
			    && (posix.size() == home.size() || posix[home.size()] == '/');
		}

		static std::string promptCwdHome(const Environment& env, const PathConv& conv) {
			const std::string posix = conv.toPosix(currentCwdAsUtf8());
			const std::string home = env.get("HOME");
			if (underHome(posix, home)) return "~" + posix.substr(home.size());

			return posix;
		}

		static std::string promptCwdBasename(const PathConv& conv) {
			const std::string posix = conv.toPosix(currentCwdAsUtf8());
			const std::size_t slash = posix.rfind('/');
			return slash == std::string::npos ? posix : posix.substr(slash + 1);
		}

		static std::string plainGitSegment(const GitInfo& info) {
			std::string out = " (" + info.branch;
			if (!info.state.empty()) out += " | " + info.state;
			out += ")";
			return out;
		}

		static std::string coloredGitSegment(const GitInfo& info) {
			std::string out = " ";
			out += kBranchStyle;
			out += "(" + info.branch;
			if (!info.state.empty()) {
				out += " | ";
				const char* state_color = gitStateColor(info.state);
				if (*state_color != '\0') out += state_color;
				out += info.state;
				if (*state_color != '\0') out += kBranchStyle;
			}

			out += ")";
			out += kResetStyle;
			return out;
		}

		static std::string promptGitBranch(const Environment& env, const PathConv& conv,
		                                   bool can_redraw) {
			const GitInfo info = gitPromptInfo(env, conv, can_redraw);
			if (info.branch.empty()) return {};
			if (!stdoutIsTty()) return plainGitSegment(info);

			return coloredGitSegment(info);
		}

		static std::string timeText(const std::string& format) {
			const std::time_t now = std::time(nullptr);
			char text[256];
			const std::size_t length = std::strftime(text, sizeof(text), format.c_str(),
				std::localtime(&now));
			return std::string(text, length);
		}

		// The console has no device name to give; bash says "tty" when
		// stdin is not a terminal.
		static std::string terminalName() {
			return stdinIsTty() ? "console" : "tty";
		}

		static std::string counted(std::size_t count) {
			return std::to_string(count);
		}

		// The escapes that put text in, which a later expansion must not
		// read as `$`, backquote or backslash syntax.
		static bool insertedText(char code, const Environment& env, const PathConv& conv,
		                         const PromptRequest& request, std::string& text) {
			switch (code) {
			case 's': text = "wbsh";                                        break;
			case 'u': text = promptUser(env);                               break;
			case 'h':
			case 'H': text = promptHost(env, code);                         break;
			case 'w': text = promptCwdHome(env, conv);                      break;
			case 'W': text = promptCwdBasename(conv);                       break;
			case 'g': text = promptGitBranch(env, conv, request.can_redraw); break;
			case 'd': text = timeText("%a %b %d");                          break;
			case 't': text = timeText("%H:%M:%S");                          break;
			case 'T': text = timeText("%I:%M:%S");                          break;
			case '@': text = timeText("%I:%M %p");                          break;
			case 'A': text = timeText("%H:%M");                             break;
			case 'v': text = kShortVersion;                                 break;
			case 'V': text = kLongVersion;                                  break;
			case 'l': text = terminalName();                                break;
			case 'j': text = counted(request.counts.jobs);                  break;
			case '!': text = counted(request.counts.history_number);        break;
			case '#': text = counted(request.counts.command_number);        break;
			default:  return false;
			}

			return true;
		}

		static bool controlText(char code, std::string& text) {
			switch (code) {
			case 'n': text = "\n";                          break;
			case 'r': text = "\r";                          break;
			case 'a': text = "\a";                          break;
			case 'e': text = "\x1b";                        break;
			case '[': text = std::string(1, kStartIgnore);  break;
			case ']': text = std::string(1, kEndIgnore);    break;
			default:  return false;
			}

			return true;
		}

		static std::string quotedForExpansion(const std::string& text) {
			std::string quoted;
			for (const char letter : text) {
				if (letter == '$' || letter == '`' || letter == '\\' || letter == '"') {
					quoted.push_back('\\');
				}

				quoted.push_back(letter);
			}

			return quoted;
		}

		static bool isOctal(char letter) {
			return letter >= '0' && letter <= '7';
		}

		// `\nnn`: up to three octal digits starting at `i`.
		static std::string octalText(const std::string& ps, std::size_t& i) {
			int value = 0;
			std::size_t used = 0;
			while (used < 3 && i < ps.size() && isOctal(ps[i])) {
				value = value * 8 + (ps[i] - '0');
				++used;
				++i;
			}

			return std::string(1, static_cast<char>(value & 0xff));
		}

		// `\D{format}`: strftime; an empty format is the locale's time.
		static bool formattedTime(const std::string& ps, std::size_t& i, std::string& text) {
			if (i >= ps.size() || ps[i] != '{') return false;

			const std::size_t close = ps.find('}', i);
			if (close == std::string::npos) return false;

			const std::string format = ps.substr(i + 1, close - i - 1);
			text = timeText(format.empty() ? "%X" : format);
			i = close + 1;
			return true;
		}

		// `\$` is `#` when elevated; before an expansion it stays `\$`, which
		// the expansion turns into `$`.
		static std::string dollarSign(const PromptRequest& request) {
			if (runsElevated()) return "#";

			return request.for_expansion ? "\\$" : "$";
		}

		static std::string inserted(const std::string& text, const PromptRequest& request) {
			return request.for_expansion ? quotedForExpansion(text) : text;
		}

		// One escape whose letter is at `i`; `i` ends past it.
		static std::string decodeEscape(const std::string& ps, std::size_t& i,
		                                const Environment& env, const PathConv& conv,
		                                const PromptRequest& request) {
			const char code = ps[i++];
			std::string text;
			if (code == '$') return dollarSign(request);
			if (code == '\\') return inserted("\\", request);
			if (controlText(code, text)) return text;
			if (insertedText(code, env, conv, request, text)) return inserted(text, request);
			if (isOctal(code)) {
				--i;
				return inserted(octalText(ps, i), request);
			}

			if (code == 'D' && formattedTime(ps, i, text)) return inserted(text, request);

			return std::string{ '\\', code };
		}

		static std::string expandedKeepingStatus(Executor& exec, const std::string& text) {
			const int status = exec.lastStatus();
			Expander& expander = exec.expander();
			std::string expanded = expander.expandPromptText(text);
			if (expander.failed()) {
				std::fprintf(stderr, "wbsh: %s\n", expander.takeError().c_str());
				expanded = text;
			}

			expander.takeFatalError();
			exec.setLastStatus(status);
			return expanded;
		}

	}  // namespace prompt_detail

	bool runsElevated() {
#ifdef _WIN32
		HANDLE token = nullptr;
		if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

		TOKEN_ELEVATION elevation{};
		DWORD size = 0;
		const bool queried = ::GetTokenInformation(token, TokenElevation, &elevation,
			sizeof(elevation), &size) != 0;
		::CloseHandle(token);
		return queried && elevation.TokenIsElevated != 0;
#else
		return false;
#endif /* _WIN32 */
	}

	std::string decodePromptEscapes(const std::string& ps, const Environment& env,
	                                const PathConv& conv, const PromptRequest& request) {
		std::string out;
		std::size_t i = 0;
		while (i < ps.size()) {
			if (ps[i] != '\\' || i + 1 >= ps.size()) {
				out.push_back(ps[i++]);
				continue;
			}

			++i;
			out += prompt_detail::decodeEscape(ps, i, env, conv, request);
		}

		return out;
	}

	std::string expandPromptString(Executor& exec, const std::string& ps, bool can_redraw) {
		PromptRequest request;
		request.counts = exec.promptCounts();
		request.can_redraw = can_redraw;
		request.for_expansion = exec.env().shopts().promptvars;

		const std::string decoded = decodePromptEscapes(ps, exec.env(), exec.pathConv(), request);
		if (!request.for_expansion) return decoded;

		return prompt_detail::expandedKeepingStatus(exec, decoded);
	}

}  // namespace wbsh

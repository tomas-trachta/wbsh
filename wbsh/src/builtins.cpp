/**
 * @file builtins.cpp
 * @brief Shell builtins (`cd`, `export`, `read`, `declare`, `local`,
 *        `set`, `shopt`, `trap`, `getopts`, `compgen`, `complete`,
 *        `jobs`, `wait`, …) and registerCoreBuiltins().
 */

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>

#  include <fcntl.h>
#  include <io.h>
#endif /* _WIN32 */

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "coreutils_internal.h"
#include "diagnostics.h"
#include "executor.h"
#include "funcprint.h"
#include "exitstatus.h"
#include "interrupt.h"
#ifdef _WIN32
#  include "jobcontrol.h"
#endif /* _WIN32 */
#include "keymap.h"
#include "lexer.h"
#include "lineedit.h"
#include "numparse.h"
#include "parser.h"
#include "sockets.h"
#include "timefmt.h"
#include "utf8.h"

namespace wbsh {

	static const char* const kDefaultUmask    = "0022";
	static const char* const kHistoryFileName = "/.wbsh_history";
	static const std::size_t kHelpColumns     = 5;

	static long long toIntSafe(const std::string& text, bool& ok) {
		ok = false;
		if (text.empty()) return 0;

		long long value = 0;
		std::size_t parsed = 0;
		if (!parseLL(text, value, 10, &parsed) || parsed != text.size()) return 0;

		ok = true;
		return value;
	}

	static bool isLongInteger(const std::string& text) {
		bool ok = false;
		toIntSafe(text, ok);
		return ok;
	}

	static void printerr(const std::string& message) {
		shellError(message);
	}

	// bash prints a builtin's usage line without the script and line.
	static void printUsage(const std::string& usage) {
		std::fprintf(stderr, "%s\n", usage.c_str());
	}

	static void writeStdout(const std::string& text) {
		std::fwrite(text.data(), 1, text.size(), stdout);
	}

	static long long toIntOrZero(const std::string& text) {
		bool ok = false;
		return toIntSafe(text, ok);
	}

	static double toDoubleOrZero(const std::string& text) {
		char* end = nullptr;
		const double value = std::strtod(text.c_str(), &end);
		return end == text.c_str() ? 0.0 : value;
	}

	static std::string joinWithSpaces(std::vector<std::string>::const_iterator first,
			std::vector<std::string>::const_iterator last) {
		std::string joined;
		for (auto it = first; it != last; ++it) {
			if (it != first) joined.push_back(' ');
			joined += *it;
		}

		return joined;
	}

	static bool splitAssignment(const std::string& spec, std::string& out_name,
			std::string& out_value) {
		const std::size_t eq = spec.find('=');
		out_name = spec.substr(0, eq);
		if (eq == std::string::npos) {
			out_value.clear();
			return false;
		}

		out_value = spec.substr(eq + 1);
		return true;
	}

	template <typename Map>
	static std::vector<std::string> keysOf(const Map& map) {
		std::vector<std::string> keys;
		for (const auto& entry : map) keys.push_back(entry.first);
		return keys;
	}

	static std::vector<std::pair<std::string, std::string>> sortedEntries(
			const std::unordered_map<std::string, std::string>& map) {
		std::vector<std::pair<std::string, std::string>> entries(map.begin(), map.end());
		std::sort(entries.begin(), entries.end());
		return entries;
	}

	static int builtin_true(Executor&, const std::vector<std::string>&) { return 0; }
	static int builtin_false(Executor&, const std::vector<std::string>&) { return 1; }
	static int builtin_colon(Executor&, const std::vector<std::string>&) { return 0; }

	static void appendOctalEscape(const std::string& text, std::size_t& i, std::string& out) {
		int value = 0;
		int digits = 0;
		while (digits < 3 && i + 1 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '7') {
			value = value * 8 + (text[++i] - '0');
			++digits;
		}

		out.push_back(static_cast<char>(value));
	}

	static int hexDigitValue(char digit) {
		if (std::isdigit(static_cast<unsigned char>(digit))) return digit - '0';
		return std::tolower(static_cast<unsigned char>(digit)) - 'a' + 10;
	}

	static void appendHexEscape(const std::string& text, std::size_t& i, std::string& out) {
		int value = 0;
		int digits = 0;
		while (digits < 2 && i + 1 < text.size()
				&& std::isxdigit(static_cast<unsigned char>(text[i + 1]))) {
			value = value * 16 + hexDigitValue(text[++i]);
			++digits;
		}

		out.push_back(static_cast<char>(value));
	}

	namespace escape_detail {

		/// printf, unlike echo, complains about a `\u` with no digits.
		struct EscapeStyle {
			bool utf8_locale = true;
			bool printf_style = false;
		};

	}  // namespace escape_detail

	using escape_detail::EscapeStyle;

	// @p i is at the `u` or `U`; without a hex digit after it the escape
	// stays as written.
	static void appendEchoUnicode(const std::string& text, std::size_t& i, std::string& out,
			const EscapeStyle& style) {
		std::size_t at = i + 1;
		char32_t code = 0;
		if (!readUnicodeEscape(text, at, text[i], code)) {
			if (style.printf_style) {
				printerr(std::string("printf: missing unicode digit for \\") + text[i]);
			}

			out.push_back('\\');
			out.push_back(text[i]);
			return;
		}

		appendUnicodeEscape(out, code, style.utf8_locale);
		i = at - 1;
	}

	static void appendEchoEscape(const std::string& text, std::size_t& i, std::string& out,
			const EscapeStyle& style) {
		const char next = text[++i];
		switch (next) {
		case 'a':  out.push_back('\a'); break;
		case 'b':  out.push_back('\b'); break;
		case 'e':  out.push_back('\x1b'); break;
		case 'E':  out.push_back('\x1b'); break;
		case 'u':  appendEchoUnicode(text, i, out, style); break;
		case 'U':  appendEchoUnicode(text, i, out, style); break;
		case 'f':  out.push_back('\f'); break;
		case 'n':  out.push_back('\n'); break;
		case 'r':  out.push_back('\r'); break;
		case 't':  out.push_back('\t'); break;
		case 'v':  out.push_back('\v'); break;
		case '\\': out.push_back('\\'); break;
		case '0':  appendOctalEscape(text, i, out); break;
		case 'x':  appendHexEscape(text, i, out); break;
		default:
			out.push_back('\\');
			out.push_back(next);
			break;
		}
	}

	static bool isStopEscape(const std::string& text, std::size_t i) {
		return text[i] == '\\' && i + 1 < text.size() && text[i + 1] == 'c';
	}

	// Returns false when the text held `\c`: everything after it, the
	// trailing newline included, is suppressed.
	static bool interpretEcho(const std::string& text, std::string& out, const EscapeStyle& style) {
		for (std::size_t i = 0; i < text.size(); ++i) {
			const char c = text[i];
			if (isStopEscape(text, i)) return false;

			if (c == '\\' && i + 1 < text.size()) {
				appendEchoEscape(text, i, out, style);
				continue;
			}

			out.push_back(c);
		}

		return true;
	}

	// Only a word made entirely of option letters is an option: `-n: x`
	// is an argument to print.
	static bool isEchoOptionWord(const std::string& word) {
		if (word.size() < 2 || word[0] != '-') return false;

		return word.find_first_not_of("neE", 1) == std::string::npos;
	}

	static bool applyEchoFlags(const std::string& flag, bool& newline, bool& interpret) {
		if (!isEchoOptionWord(flag)) return false;

		for (std::size_t k = 1; k < flag.size(); ++k) {
			switch (flag[k]) {
			case 'n': newline = false;  break;
			case 'e': interpret = true;  break;
			default:  interpret = false; break;
			}
		}

		return true;
	}

	static std::size_t parseEchoFlags(const std::vector<std::string>& args, bool& newline,
			bool& interpret) {
		std::size_t i = 0;
		while (i < args.size() && !args[i].empty() && args[i][0] == '-') {
			if (!applyEchoFlags(args[i], newline, interpret)) break;
			++i;
		}

		return i;
	}

	static std::string echoText(const std::vector<std::string>& args, std::size_t first,
			bool newline, bool interpret, const EscapeStyle& style) {
		std::string out;
		for (std::size_t i = first; i < args.size(); ++i) {
			if (i > first) out.push_back(' ');
			if (!interpret) out += args[i];
			else if (!interpretEcho(args[i], out, style)) return out;
		}

		if (newline) out.push_back('\n');
		return out;
	}

	static int builtin_echo(Executor& exec, const std::vector<std::string>& args) {
		bool newline = true;
		bool interpret = exec.env().shopts().xpg_echo;
		const std::size_t first = parseEchoFlags(args, newline, interpret);

		EscapeStyle style;
		style.utf8_locale = exec.env().utf8Locale();
		writeStdout(echoText(args, first, newline, interpret, style));
		if (std::fflush(stdout) == 0 && !std::ferror(stdout)) return 0;

		// A reader that went away is SIGPIPE, which bash reports with no
		// message; any other failure (a closed stdout) is bash's write error.
		const int error = errno;
		const int status = stdoutFailureStatus();
		if (status != 1) return status;

		shellErrorf("echo: write error: %s\n", std::strerror(error));
		return 1;
	}

	namespace printf_detail {

		struct PrintfRun {
			std::vector<std::string> args;
			std::size_t next_arg = 0;
			std::string out;
			bool bad_number = false;
			bool bad_format = false;
			bool stopped = false;
			EscapeStyle escapes;
			bool has_time_format = false;
			bool long_form = false;
			bool wide_locale = false;
			std::string time_format;
			std::time_t shell_start = 0;
		};

	}  // namespace printf_detail

	using printf_detail::PrintfRun;

	static void appendFormatEscape(const std::string& fmt, std::size_t& i, PrintfRun& run) {
		const char next = fmt[i + 1];
		if (next >= '0' && next <= '7') {
			appendOctalEscape(fmt, i, run.out);
			return;
		}

		if (next == '"' || next == '\'' || next == '?') {
			run.out.push_back(next);
			++i;
			return;
		}

		appendEchoEscape(fmt, i, run.out, run.escapes);
	}

	// bash stops at a bad conversion: what came before it is printed and
	// the status is 1.
	static void rejectFormat(PrintfRun& run, const std::string& message) {
		printerr("printf: " + message);
		run.bad_format = true;
		run.stopped = true;
	}

	static std::string takePrintfArg(PrintfRun& run) {
		if (run.next_arg >= run.args.size()) return std::string();
		return run.args[run.next_arg++];
	}

	static bool isQuotedCharArg(const std::string& text) {
		return !text.empty() && (text[0] == '\'' || text[0] == '"');
	}

	static long long quotedCharCode(const std::string& text) {
		return text.size() > 1 ? static_cast<unsigned char>(text[1]) : 0;
	}

	static void reportBadNumber(const std::string& text, PrintfRun& run) {
		shellErrorf("printf: %s: invalid number\n", text.c_str());
		run.bad_number = true;
	}

	static long long printfIntegerArg(const std::string& text, PrintfRun& run) {
		if (text.empty()) return 0;
		if (isQuotedCharArg(text)) return quotedCharCode(text);

		long long value = 0;
		std::size_t parsed = 0;
		if (!parseLL(text, value, 0, &parsed) || parsed != text.size()) reportBadNumber(text, run);
		return value;
	}

	static double printfDoubleArg(const std::string& text, PrintfRun& run) {
		if (text.empty()) return 0.0;
		if (isQuotedCharArg(text)) return static_cast<double>(quotedCharCode(text));

		double value = 0.0;
		std::size_t parsed = 0;
		if (!parseDouble(text, value, &parsed) || parsed != text.size()) reportBadNumber(text, run);
		return value;
	}

	static void appendPrintfDigits(const std::string& fmt, std::size_t& i, std::string& out_spec) {
		while (i < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i]))) {
			out_spec.push_back(fmt[i++]);
		}
	}

	static void appendPrintfWidth(const std::string& fmt, std::size_t& i, PrintfRun& run,
			std::string& out_spec) {
		if (i < fmt.size() && fmt[i] == '*') {
			out_spec += std::to_string(printfIntegerArg(takePrintfArg(run), run));
			++i;
			return;
		}

		appendPrintfDigits(fmt, i, out_spec);
	}

	static bool skipLengthModifiers(const std::string& fmt, std::size_t& i) {
		bool long_form = false;
		while (i < fmt.size() && std::strchr("hlLjzt", fmt[i]) != nullptr) {
			long_form = long_form || fmt[i] == 'l';
			++i;
		}

		return long_form;
	}

	// `%(datefmt)T`: the strftime format between the parentheses.
	static bool takeTimeFormat(const std::string& fmt, std::size_t& i, PrintfRun& run) {
		run.has_time_format = false;
		if (i >= fmt.size() || fmt[i] != '(') return true;

		const std::size_t close = fmt.find(')', i + 1);
		if (close == std::string::npos) {
			printerr("printf: warning: `(': invalid time format specification");
			run.bad_number = true;
			run.stopped = true;
			return false;
		}

		run.time_format = fmt.substr(i + 1, close - i - 1);
		run.has_time_format = true;
		i = close + 1;
		return true;
	}

	static bool parsePrintfConversionSpec(const std::string& fmt, std::size_t& i, PrintfRun& run,
			std::string& out_spec) {
		const std::size_t start = i;
		out_spec = "%";
		++i;
		while (i < fmt.size() && std::strchr("-+ #0", fmt[i]) != nullptr) {
			out_spec.push_back(fmt[i++]);
		}

		appendPrintfWidth(fmt, i, run, out_spec);
		if (i < fmt.size() && fmt[i] == '.') {
			out_spec.push_back(fmt[i++]);
			appendPrintfWidth(fmt, i, run, out_spec);
		}

		if (!takeTimeFormat(fmt, i, run)) return false;
		run.long_form = skipLengthModifiers(fmt, i);
		if (i >= fmt.size()) {
			rejectFormat(run, "`" + fmt.substr(start) + "': missing format character");
			return false;
		}

		out_spec.push_back(fmt[i]);
		return true;
	}

	static std::string specWithConversion(const std::string& spec, const std::string& conversion) {
		std::string result = spec;
		result.pop_back();
		return result + conversion;
	}

	template <typename Value>
	static std::string formatValue(const std::string& spec, Value value) {
		const int length = std::snprintf(nullptr, 0, spec.c_str(), value);
		if (length <= 0) return std::string();

		std::vector<char> buffer(static_cast<std::size_t>(length) + 1);
		std::snprintf(buffer.data(), buffer.size(), spec.c_str(), value);
		return std::string(buffer.data(), static_cast<std::size_t>(length));
	}

	static std::string formatString(const std::string& spec, const std::string& text) {
		if (spec.size() == 2) return text;
		return formatValue(specWithConversion(spec, "s"), text.c_str());
	}

	static bool isShellSafeChar(char c) {
		return std::isalnum(static_cast<unsigned char>(c)) != 0
			|| std::strchr("_-./:=@%+,", c) != nullptr;
	}

	static std::string shellQuoteBackslash(const std::string& text) {
		if (text.empty()) return "''";

		std::string quoted;
		for (const char c : text) {
			if (!isShellSafeChar(c) && static_cast<unsigned char>(c) < 0x80) quoted.push_back('\\');
			quoted.push_back(c);
		}

		return quoted;
	}

	static bool hasControlChar(const std::string& text) {
		for (const char c : text) {
			const unsigned char code = static_cast<unsigned char>(c);
			if (code < 0x20 || code == 0x7f) return true;
		}

		return false;
	}

	// Control characters make bash quote the whole word as `$'...'`.
	static std::string shellQuoteForPrintf(const std::string& text) {
		if (hasControlChar(text)) return shellQuoteAlways(text);
		return shellQuoteBackslash(text);
	}

	static std::string formatEscapedArg(const std::string& spec, PrintfRun& run) {
		std::string expanded;
		if (!interpretEcho(takePrintfArg(run), expanded, run.escapes)) run.stopped = true;
		return formatString(spec, expanded);
	}

	static std::string formatUnsigned(const std::string& spec, char conversion, PrintfRun& run) {
		const long long value = printfIntegerArg(takePrintfArg(run), run);
		return formatValue(specWithConversion(spec, std::string("ll") + conversion),
			static_cast<unsigned long long>(value));
	}

	namespace printf_detail {

		struct WideSpec {
			bool left = false;
			std::size_t width = 0;
			bool has_precision = false;
			std::size_t precision = 0;
		};

		static std::size_t readSpecNumber(const std::string& spec, std::size_t& i) {
			std::size_t value = 0;
			while (i < spec.size() && std::isdigit(static_cast<unsigned char>(spec[i]))) {
				value = value * 10 + static_cast<std::size_t>(spec[i] - '0');
				++i;
			}

			return value;
		}

		// A `*` width already became digits, a negative one with its `-`,
		// which reads here as the left-justify flag.
		static WideSpec parseWideSpec(const std::string& spec) {
			WideSpec wide;
			std::size_t i = 1;
			while (i < spec.size() && std::strchr("-+ #0", spec[i]) != nullptr) {
				wide.left = wide.left || spec[i] == '-';
				++i;
			}

			wide.width = readSpecNumber(spec, i);
			if (i < spec.size() && spec[i] == '.') {
				++i;
				wide.has_precision = true;
				wide.precision = readSpecNumber(spec, i);
			}

			return wide;
		}

		static std::string padWide(const WideSpec& wide, std::string text, std::size_t chars) {
			if (chars >= wide.width) return text;

			const std::string padding(wide.width - chars, ' ');
			return wide.left ? text + padding : padding + text;
		}

		// bash 5.3's `%ls` / `%S` count width and precision in characters.
		static std::string formatWideString(const std::string& spec, const std::string& text) {
			const WideSpec wide = parseWideSpec(spec);
			std::size_t chars = utf8Count(text);
			std::string shown = text;
			if (wide.has_precision && wide.precision < chars) {
				shown = text.substr(0, utf8Advance(text, 0, wide.precision));
				chars = wide.precision;
			}

			return padWide(wide, std::move(shown), chars);
		}

		static std::string firstCharacter(const std::string& text) {
			return text.substr(0, utf8CharLength(text, 0));
		}

	}  // namespace printf_detail

	static bool wideConversion(const PrintfRun& run, char conversion) {
		const bool wide_letter = conversion == 'S' || conversion == 'C';
		return run.wide_locale && (run.long_form || wide_letter);
	}

	static std::string formatWideArg(const std::string& spec, char conversion, PrintfRun& run) {
		const std::string text = takePrintfArg(run);
		if (conversion == 's' || conversion == 'S') {
			return printf_detail::formatWideString(spec, text);
		}

		if (text.empty()) return formatValue(specWithConversion(spec, "c"), 0);
		return printf_detail::formatWideString(spec, printf_detail::firstCharacter(text));
	}

	static std::string formatCharArg(const std::string& spec, PrintfRun& run) {
		const std::string text = takePrintfArg(run);
		return formatValue(spec, text.empty() ? 0 : static_cast<int>(text[0]));
	}

	// No argument means now (-1); -2 is the time the shell started.
	static std::time_t printfTimeArg(PrintfRun& run) {
		const bool missing = run.next_arg >= run.args.size();
		const long long value = missing ? -1 : printfIntegerArg(takePrintfArg(run), run);
		if (value == -1) return std::time(nullptr);
		if (value == -2) return run.shell_start;
		return static_cast<std::time_t>(value);
	}

	static std::string formatTimeArg(const std::string& spec, PrintfRun& run) {
		const std::string format = run.time_format.empty() ? "%X" : run.time_format;
		const std::string text = formatLocalTime(format, printfTimeArg(run));
		return formatString(specWithConversion(spec, "s"), text);
	}

	static std::string formatPrintfConversion(const std::string& spec, char conversion,
			PrintfRun& run) {
		if (run.has_time_format && conversion == 'T') return formatTimeArg(spec, run);
		if (wideConversion(run, conversion)) return formatWideArg(spec, conversion, run);

		switch (conversion) {
		case 's':
		case 'S':
			return formatString(specWithConversion(spec, "s"), takePrintfArg(run));
		case 'b':
			return formatEscapedArg(spec, run);
		case 'q':
			return formatString(spec, shellQuoteForPrintf(takePrintfArg(run)));
		case 'd':
		case 'i':
			return formatValue(specWithConversion(spec, "lld"),
				printfIntegerArg(takePrintfArg(run), run));
		case 'u':
		case 'x':
		case 'X':
		case 'o':
			return formatUnsigned(spec, conversion, run);
		case 'f':
		case 'F':
		case 'e':
		case 'E':
		case 'g':
		case 'G':
		case 'a':
		case 'A':
			return formatValue(spec, printfDoubleArg(takePrintfArg(run), run));
		case 'c':
		case 'C':
			return formatCharArg(specWithConversion(spec, "c"), run);
		case '%':
			return "%";
		default:
			rejectFormat(run, std::string("`") + conversion + "': invalid format character");
			return std::string();
		}
	}

	static void appendFormatOnce(const std::string& fmt, PrintfRun& run) {
		for (std::size_t i = 0; i < fmt.size() && !run.stopped; ++i) {
			const char c = fmt[i];
			if (c == '\\' && i + 1 < fmt.size()) {
				appendFormatEscape(fmt, i, run);
				continue;
			}

			if (c != '%') {
				run.out.push_back(c);
				continue;
			}

			const std::size_t spec_start = i;
			std::string spec;
			if (!parsePrintfConversionSpec(fmt, i, run, spec)) {
				if (!run.bad_format) run.out.append(fmt, spec_start, i + 1 - spec_start);
				return;
			}

			run.out += formatPrintfConversion(spec, fmt[i], run);
		}
	}

	// Bash printf semantics: emit the format once, then keep cycling
	// while there are unconsumed args. If a pass consumes nothing
	// (a literal-only format), stop to avoid an infinite loop.
	static void appendFormatCycles(const std::string& fmt, PrintfRun& run) {
		std::size_t before = 0;
		do {
			before = run.next_arg;
			appendFormatOnce(fmt, run);
		} while (!run.stopped && run.next_arg < run.args.size() && run.next_arg != before);
	}

	static std::size_t parsePrintfTarget(const std::vector<std::string>& args,
			std::string& target) {
		std::size_t first = 0;
		if (args.size() >= 2 && args[0] == "-v") {
			target = args[1];
			first = 2;
		}

		if (first < args.size() && args[first] == "--") ++first;
		return first;
	}

	static int storePrintfResult(Executor& exec, const std::string& target,
			const std::string& text) {
		if (target.empty()) {
			writeStdout(text);
			std::fflush(stdout);
			return 0;
		}

		if (exec.env().isReadonly(target)) {
			printerr("printf: " + target + ": readonly variable");
			return 1;
		}

		exec.env().set(target, text);
		return 0;
	}

	static int builtin_printf(Executor& exec, const std::vector<std::string>& args) {
		std::string target;
		const std::size_t first = parsePrintfTarget(args, target);
		if (first >= args.size()) {
			printUsage("printf: usage: printf [-v var] format [arguments]");
			return 2;
		}

		PrintfRun run;
		run.args.assign(args.begin() + static_cast<std::ptrdiff_t>(first) + 1, args.end());
		run.shell_start = exec.env().startEpoch();
		run.escapes.utf8_locale = exec.env().utf8Locale();
		run.wide_locale = run.escapes.utf8_locale;
		run.escapes.printf_style = true;
		appendFormatCycles(args[first], run);

		const int store_status = storePrintfResult(exec, target, run.out);
		if (store_status != 0) return store_status;
		return run.bad_number || run.bad_format ? 1 : 0;
	}

	static std::filesystem::path resolvedPath(const std::filesystem::path& path) {
		std::error_code ec;
		const std::filesystem::path resolved = std::filesystem::canonical(path, ec);
		return ec ? path : resolved;
	}

	static int builtin_pwd(Executor& exec, const std::vector<std::string>& args) {
		bool windows_form = false;
		bool physical = exec.env().options().physical;
		for (const std::string& arg : args) {
			if (arg == "-W") windows_form = true;
			if (arg == "-P") physical = true;
			if (arg == "-L") physical = false;
		}

		std::error_code ec;
		std::filesystem::path here = expandShortNames(std::filesystem::current_path(ec));
		if (!ec && physical) here = resolvedPath(here);
		std::string shown = ec ? exec.env().get("PWD") : pathToUtf8(here);
		if (!windows_form) shown = exec.pathConv().toPosix(shown);

		writeStdout(shown);
		std::fputc('\n', stdout);
		std::fflush(stdout);
		return 0;
	}

	static bool resolveCdTarget(Executor& exec, const std::vector<std::string>& args,
			std::size_t first, std::string& out_target) {
		if (first >= args.size()) {
			out_target = exec.env().get("HOME");
			return true;
		}

		if (args[first] != "-") {
			out_target = args[first];
			return true;
		}

		out_target = exec.env().get("OLDPWD");
		if (out_target.empty()) {
			printerr("cd: OLDPWD not set");
			return false;
		}

		std::printf("%s\n", exec.pathConv().toPosix(out_target).c_str());
		return true;
	}

	static bool isCdOptionWord(const std::string& arg) {
		return arg.size() > 1 && arg[0] == '-';
	}

	static bool parseCdOptions(const std::vector<std::string>& args, std::size_t& first,
			bool& physical) {
		first = 0;
		while (first < args.size() && isCdOptionWord(args[first])) {
			const std::string& arg = args[first++];
			if (arg == "--") return true;

			for (std::size_t k = 1; k < arg.size(); ++k) {
				if (arg[k] == 'P') physical = true;
				else if (arg[k] == 'L') physical = false;
				else if (arg[k] != 'e' && arg[k] != '@') {
					shellErrorf("cd: -%c: invalid option\n", arg[k]);
					std::fprintf(stderr, "cd: usage: cd [-L|[-P [-e]] [-@]] [dir]\n");
					return false;
				}
			}
		}

		return true;
	}

	static int builtin_cd(Executor& exec, const std::vector<std::string>& args) {
		if (exec.env().restricted()) {
			printerr("cd: restricted");
			return 1;
		}

		bool physical = exec.env().options().physical;
		std::size_t first = 0;
		if (!parseCdOptions(args, first, physical)) return 2;

		std::string target;
		if (!resolveCdTarget(exec, args, first, target)) return 1;

		std::string error;
		if (!changeDirectory(exec, target, error, physical)) {
			shellErrorf("cd: %s: %s\n", target.c_str(), error.c_str());
			return 1;
		}

		return 0;
	}

	static const int kStatusBadUsage = 2;

	// The one optional integer operand of exit, return, shift, break and
	// continue. bash 5.3 rejects a word that is not a number, and extra
	// operands, with status 2 and does not act.
	static bool readNumericOperand(const std::string& builtin, const std::vector<std::string>& args,
			long long& value) {
		if (args.empty()) return true;

		bool ok = false;
		value = toIntSafe(args[0], ok);
		if (!ok) {
			printerr(builtin + ": " + args[0] + ": numeric argument required");
			return false;
		}

		if (args.size() == 1) return true;

		printerr(builtin + ": too many arguments");
		return false;
	}

	static int builtin_exit(Executor& exec, const std::vector<std::string>& args) {
		long long value = exec.defaultExitStatus();
		if (!readNumericOperand("exit", args, value)) return kStatusBadUsage;

		const int status = shellStatusFromInt(static_cast<int>(value));
		exec.raiseExit(status);
		return status;
	}

	static bool isInvalidSuspendOption(const std::string& arg) {
		return arg.size() > 1 && arg[0] == '-' && arg != "-f" && arg != "--";
	}

	// Stopping the shell needs job control, which wbsh does not have, so
	// this is always bash's answer for a shell without it.
	static int builtin_suspend(Executor&, const std::vector<std::string>& args) {
		if (!args.empty() && isInvalidSuspendOption(args[0])) {
			printerr("suspend: " + args[0].substr(0, 2) + ": invalid option");
			std::fprintf(stderr, "suspend: usage: suspend [-f]\n");
			return 2;
		}

		printerr("suspend: cannot suspend: no job control");
		return 1;
	}

	// `logout` ends a login shell (`wbsh -l`) like `exit`. Typed at the
	// prompt it also ends the session; anywhere else it is bash's error.
	static int builtin_logout(Executor& exec, const std::vector<std::string>& args) {
		const bool login = exec.env().shopts().login_shell;
		if (login || exec.env().interactive()) return builtin_exit(exec, args);

		printerr("logout: not login shell: use `exit'");
		return 1;
	}

	static int builtin_return(Executor& exec, const std::vector<std::string>& args) {
		if (exec.funcDepth() == 0 && exec.sourceDepth() == 0) {
			printerr("return: can only `return' from a function or sourced script");
			return 1;
		}

		long long value = exec.lastStatus();
		if (args.size() > 1) return readNumericOperand("return", args, value) ? 0 : kStatusBadUsage;
		if (!readNumericOperand("return", args, value)) value = kStatusBadUsage;

		const int status = shellStatusFromInt(static_cast<int>(value));
		exec.raiseReturn(status);
		return status;
	}

	// A count that is not a number abandons the whole command line, as bash's
	// top-level jump does; a count below 1 is only an error.
	static bool readLoopCount(Executor& exec, const std::string& builtin,
			const std::vector<std::string>& args, long long& count, int& status) {
		count = 1;
		const bool number = args.empty() || isLongInteger(args[0]);
		if (!readNumericOperand(builtin, args, count)) {
			status = kStatusBadUsage;
			if (!number) exec.raiseDiscard();
			return false;
		}

		if (count >= 1) return true;

		printerr(builtin + ": " + args[0] + ": loop count out of range");
		status = 1;
		return false;
	}

	static int builtin_break(Executor& exec, const std::vector<std::string>& args) {
		if (exec.loopDepth() == 0) {
			printerr("break: only meaningful in a `for', `while', or `until' loop");
			return 0;
		}

		long long count = 1;
		int status = 0;
		if (!readLoopCount(exec, "break", args, count, status)) return status;

		exec.raiseBreak(static_cast<int>(count));
		return 0;
	}

	static int builtin_continue(Executor& exec, const std::vector<std::string>& args) {
		if (exec.loopDepth() == 0) {
			printerr("continue: only meaningful in a `for', `while', or `until' loop");
			return 0;
		}

		long long count = 1;
		int status = 0;
		if (!readLoopCount(exec, "continue", args, count, status)) return status;

		exec.raiseContinue(static_cast<int>(count));
		return 0;
	}

	// A negative count is always reported; one past the parameters only
	// under shift_verbose (on in POSIX mode).
	static bool shiftCountInRange(Executor& exec, long long count, std::size_t available) {
		const bool too_many = count > static_cast<long long>(available);
		if (count >= 0 && !too_many) return true;

		if (count < 0 || exec.env().shopts().shift_verbose) {
			printerr("shift: " + std::to_string(count) + ": shift count out of range");
		}

		return false;
	}

	static int builtin_shift(Executor& exec, const std::vector<std::string>& args) {
		long long count = 1;
		if (!readNumericOperand("shift", args, count)) return kStatusBadUsage;

		std::vector<std::string> positional = exec.env().positional();
		if (!shiftCountInRange(exec, count, positional.size())) return 1;

		positional.erase(positional.begin(), positional.begin() + count);
		exec.env().setPositional(std::move(positional));
		return 0;
	}

	namespace set_detail {

		struct SetOperands {
			std::size_t first = 0;
			bool        replace_positional = false;
		};

	}  // namespace set_detail

	using set_detail::SetOperands;

	// The line editor follows `set -o vi` and `set -o emacs`; with line
	// editing always on, `set +o vi` goes back to emacs keys.
	static void applyEditingMode(const SetOptionSpec& spec, bool on) {
		const bool vi = spec.flag == &SetOptions::vi;
		const bool emacs = spec.flag == &SetOptions::emacs;
		if (!vi && !emacs) return;
		if (!on && !(vi && viEditing())) return;

		shellKeymap().setVariable("editing-mode", vi && on ? "vi" : "emacs");
	}

	static void applySetOption(Environment& env, const SetOptionSpec& spec, bool on) {
		if (spec.flag == &SetOptions::posix) {
			env.setPosixMode(on);
			return;
		}

		SetOptions& options = env.options();
		options.*(spec.flag) = on;

		if (on && spec.flag == &SetOptions::vi)    options.emacs = false;
		if (on && spec.flag == &SetOptions::emacs) options.vi = false;
		applyEditingMode(spec, on);
	}

	static void listSetOptions(const Environment& env, bool as_commands) {
		for (const SetOptionSpec& spec : setOptionTable()) {
			const bool on = env.options().*(spec.flag);
			if (as_commands) std::printf("set %co %s\n", on ? '-' : '+', spec.name);
			else             std::printf("%-15s\t%s\n", spec.name, on ? "on" : "off");
		}
	}

	static int applyNamedSetOption(Environment& env, const std::string& name, bool on) {
		const SetOptionSpec* spec = findSetOption(name);
		if (spec == nullptr) {
			shellErrorf("set: %s: invalid option name\n", name.c_str());
			return 2;
		}

		applySetOption(env, *spec, on);
		return 0;
	}

	// Each `o` in a cluster takes the next unused word as its option name;
	// with no word left it lists the options instead, as bash does.
	static int applySetOptionWord(Environment& env, const std::vector<std::string>& args,
			std::size_t& next, bool on) {
		if (next >= args.size()) {
			listSetOptions(env, !on);
			return 0;
		}

		return applyNamedSetOption(env, args[next++], on);
	}

	// `-r` is not a `set -o` option and, once on, cannot be turned off.
	static bool isRestrictFlag(char sign, char letter) {
		return sign == '-' && letter == 'r';
	}

	static int reportInvalidSetFlag(char sign, char letter) {
		shellErrorf("set: %c%c: invalid option\n", sign, letter);
		std::fprintf(stderr,
			"set: usage: set [-abefhkmnptuvxBCEHPT] [-o option-name] [--] [-] [arg ...]\n");
		return 2;
	}

	static int applySetCluster(Environment& env, const std::vector<std::string>& args,
			std::size_t& i) {
		const std::string& cluster = args[i];
		const bool on = cluster[0] == '-';
		std::size_t next = i + 1;

		for (std::size_t k = 1; k < cluster.size(); ++k) {
			const char letter = cluster[k];
			if (letter == 'o') {
				const int status = applySetOptionWord(env, args, next, on);
				if (status != 0) return status;

				continue;
			}

			if (isRestrictFlag(cluster[0], letter)) {
				env.enterRestrictedMode();
				continue;
			}

			if (letter == 'r') {
				if (!env.restricted()) continue;

				reportInvalidSetFlag(cluster[0], letter);
				return 1;
			}

			const SetOptionSpec* spec = findSetFlag(letter);
			if (spec == nullptr) return reportInvalidSetFlag(cluster[0], letter);

			applySetOption(env, *spec, on);
		}

		i = next;
		return 0;
	}

	static void endSetOptions(Environment& env, const std::string& marker, std::size_t next,
			std::size_t argc, SetOperands& out) {
		if (marker == "-") {
			env.options().xtrace = false;
			env.options().verbose = false;
		}

		out.first = next;
		out.replace_positional = marker == "--" || next < argc;
	}

	static bool isSetOptionWord(const std::string& arg) {
		return !arg.empty() && (arg[0] == '-' || arg[0] == '+');
	}

	static bool endsSetOptions(const std::string& arg) {
		return arg == "--" || arg == "-" || !isSetOptionWord(arg);
	}

	// Like bash, an unknown letter anywhere rejects the whole command before
	// any option changes; an unknown `-o` name only stops it where it occurs.
	static int validateSetCluster(const std::string& cluster, std::size_t& out_option_names) {
		out_option_names = 0;
		for (std::size_t k = 1; k < cluster.size(); ++k) {
			const char letter = cluster[k];
			if (letter == 'o') {
				++out_option_names;
				continue;
			}

			if (letter == 'r') continue;
			if (findSetFlag(letter) == nullptr) return reportInvalidSetFlag(cluster[0], letter);
		}

		return 0;
	}

	static int validateSetFlags(const std::vector<std::string>& args) {
		std::size_t i = 0;
		while (i < args.size() && !endsSetOptions(args[i])) {
			std::size_t option_names = 0;
			const int status = validateSetCluster(args[i], option_names);
			if (status != 0) return status;

			i += 1 + option_names;
		}

		return 0;
	}

	static int parseSetFlags(Executor& exec, const std::vector<std::string>& args,
			SetOperands& out) {
		const int invalid = validateSetFlags(args);
		if (invalid != 0) return invalid;

		std::size_t i = 0;
		while (i < args.size()) {
			const std::string& arg = args[i];
			if (arg == "--" || arg == "-") {
				endSetOptions(exec.env(), arg, i + 1, args.size(), out);
				return 0;
			}

			if (!isSetOptionWord(arg)) break;

			const int status = applySetCluster(exec.env(), args, i);
			if (status != 0) return status;
		}

		out.first = i;
		out.replace_positional = i < args.size();
		return 0;
	}

	static int builtin_set(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) return listShellVariables(exec);

		SetOperands operands;
		const int status = parseSetFlags(exec, args, operands);
		if (status != 0) return status;

		if (operands.replace_positional) {
			std::vector<std::string> positional(args.begin() + operands.first, args.end());
			exec.env().setPositional(std::move(positional));
		}

		return 0;
	}

	static std::string quoteSingle(const std::string& word) {
		std::string quoted = "'";
		for (const char c : word) {
			if (c == '\'') {
				quoted += "'\\''";
				continue;
			}

			quoted.push_back(c);
		}

		quoted.push_back('\'');
		return quoted;
	}

	static std::string joinQuoted(const std::vector<std::string>& words,
			std::string (*quote)(const std::string&)) {
		std::string joined;
		for (std::size_t i = 0; i < words.size(); ++i) {
			if (i != 0) joined.push_back(' ');
			joined += quote(words[i]);
		}

		return joined;
	}

	static int runExecCommand(Executor& exec, const std::vector<std::string>& args) {
		const std::string& command = args[0];
		const std::vector<std::string> rest(args.begin() + 1, args.end());
		if (exec.isBuiltin(command))  return exec.callBuiltin(command, rest);
		if (exec.isFunction(command)) return exec.callFunction(command, rest);

		return exec.executeText(joinQuoted(args, quoteSingle), "<exec>");
	}

#ifdef _WIN32
	static const int kStatusNotExecutable = 126;

	static bool isExternalProgram(Executor& exec, const std::string& name) {
		if (exec.isBuiltin(name) || exec.isFunction(name) || exec.isAlias(name)) return false;
		return !exec.findExecutable(name).empty();
	}

	// Ctrl+C and Ctrl+Break reach the program on the console the two share;
	// the shell only goes on waiting, so the program decides what they mean.
	static DWORD awaitExecProgram(HANDLE process) {
		const ScopedCtrlCCapture capture;
		::WaitForSingleObject(process, INFINITE);
		takeCtrlC();

		DWORD code = 0;
		::GetExitCodeProcess(process, &code);
		::CloseHandle(process);
		return code;
	}

	// Windows has no execve, so the program runs as a child that the shell
	// waits for and ends with: in a job that dies with the shell, so a
	// parent that kills the shell kills the program too, and with the
	// program's own exit code. Its PID is not the shell's.
	static int execProgram(Executor& exec, const std::vector<std::string>& argv) {
		std::fflush(stdout);
		std::fflush(stderr);
		const HANDLE job = createExecJob();
		SpawnPlacement placement;
		placement.job = job;
		const HANDLE process = exec.spawnCommand(argv, placement);
		if (process == INVALID_HANDLE_VALUE) {
			releaseExecJob(job);
			return kStatusNotExecutable;
		}

		const DWORD code = awaitExecProgram(process);
		releaseExecJob(job);
		rememberExecExitCode(code);
		return shellStatusFromExitCode(code);
	}
#else
	static bool isExternalProgram(Executor&, const std::string&) {
		return false;
	}

	static int execProgram(Executor& exec, const std::vector<std::string>& argv) {
		return runExecCommand(exec, argv);
	}
#endif /* _WIN32 */

	// The command replaces the shell: the shell ends with it, and its EXIT
	// trap never runs.
	static int builtin_exec(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) return 0;
		if (exec.env().restricted()) {
			shellErrorf("exec: restricted\n");
			return 1;
		}

		exec.clearTrap("EXIT");
		const int status = isExternalProgram(exec, args[0]) ? execProgram(exec, args)
			: runExecCommand(exec, args);
		if (!exec.flowPending()) exec.raiseExit(status);
		return status;
	}

	// Windows programs print CRLF, and `eval "$(tool completion)"` is how
	// their shell code is loaded; Git Bash reads that text as if the CRs
	// before each newline, and the one `$( )` left at the end, were absent.
	static std::string evalSourceText(const Executor& exec, const std::vector<std::string>& args) {
		std::string text = exec.protectProcSubstPaths(joinWithSpaces(args.begin(), args.end()));
		normalizeCrlf(text);
		if (!text.empty() && text.back() == '\r') text.pop_back();
		return text;
	}

	static int builtin_eval(Executor& exec, const std::vector<std::string>& args) {
		const std::string joined = evalSourceText(exec, args);
		if (joined.empty()) return 0;

		const int line = (std::max)(exec.env().currentLineno(), exec.commandEndLine());
		return exec.executeInputText(joined, "eval", static_cast<std::size_t>(line > 0 ? line : 1));
	}

	static bool readScriptFile(const std::filesystem::path& path, std::string& out_body) {
		std::ifstream file(path, std::ios::binary);
		if (!file) return false;

		std::stringstream buffer;
		buffer << file.rdbuf();
		out_body = buffer.str();
		stripUtf8Bom(out_body);
		normalizeCrlf(out_body);
		return true;
	}

	namespace source_detail {

		struct Request {
			std::string command;
			bool has_path = false;
			std::string path;
			std::string file;
			std::vector<std::string> args;
		};

		static int reportUsage(const Request& request, const char* message) {
			if (message != nullptr) printerr(request.command + ": " + message);
			printUsage(request.command + ": usage: " + request.command
				+ " [-p path] filename [arguments]");
			return 2;
		}

		static int parseRequest(const std::vector<std::string>& args, Request& request) {
			std::size_t i = 0;
			while (i < args.size() && args[i].size() > 1 && args[i][0] == '-') {
				if (args[i] == "--") {
					++i;
					break;
				}

				if (args[i].compare(0, 2, "-p") != 0) {
					printerr(request.command + ": " + args[i].substr(0, 2) + ": invalid option");
					return reportUsage(request, nullptr);
				}

				if (args[i].size() > 2) request.path = args[i].substr(2);
				else if (i + 1 < args.size()) request.path = args[++i];
				else return reportUsage(request, "-p: option requires an argument");

				request.has_path = true;
				++i;
			}

			if (i >= args.size()) return reportUsage(request, "filename argument required");

			request.file = args[i];
			request.args.assign(args.begin() + static_cast<std::ptrdiff_t>(i) + 1, args.end());
			return 0;
		}

		static bool isPathLike(const std::string& name) {
			if (name.empty()) return false;
			if (name[0] == '/' || name[0] == '\\') return true;
			if (name.compare(0, 2, "./") == 0 || name.compare(0, 3, "../") == 0) return true;
			return name.size() > 1 && name[1] == ':';
		}

		static bool isReadableFile(Executor& exec, const std::string& path) {
			std::error_code ec;
			const std::filesystem::path native = utf8ToPath(exec.pathConv().toWin32(path));
			return std::filesystem::is_regular_file(native, ec);
		}

		static std::string findInDirectories(Executor& exec, const std::string& name,
				const std::string& directories) {
			for (std::string dir : splitPathList(directories)) {
				if (dir.empty()) dir = ".";
				const std::string candidate = dir + "/" + name;
				if (isReadableFile(exec, candidate)) return candidate;
			}

			return std::string();
		}

		// With -p only the given directories are searched; without it a name
		// with no slash is looked up in PATH (shopt sourcepath) and then in
		// the current directory.
		static bool resolveFile(Executor& exec, const Request& request, std::string& out) {
			if (isPathLike(request.file)) {
				out = request.file;
				return true;
			}

			if (request.has_path) {
				out = findInDirectories(exec, request.file,
					request.path.empty() ? std::string(".") : request.path);
				if (!out.empty()) return true;

				printerr(request.command + ": " + request.file + ": file not found");
				exec.markSpecialBuiltinFailure();
				return false;
			}

			const bool searches_path = exec.env().shopts().sourcepath
				&& request.file.find('/') == std::string::npos;
			if (searches_path) out = findInDirectories(exec, request.file, exec.env().get("PATH"));
			if (!out.empty() || isPathLike(request.file) || !exec.env().options().posix) {
				if (out.empty()) out = request.file;
				return true;
			}

			printerr(request.command + ": " + request.file + ": file not found");
			exec.markSpecialBuiltinFailure();
			return false;
		}

	}  // namespace source_detail

	// `/dev/stdin` would map to the console; `. /dev/stdin` reads the
	// shell's own standard input, a pipe as often as not.
	static bool namesStandardInput(const std::string& file) {
		return file == "/dev/stdin" || file == "/dev/fd/0" || file == "/proc/self/fd/0";
	}

	static bool readStandardInput(std::string& out_body) {
		char buffer[65536];
		int got = 0;
		while ((got = _read(0, buffer, sizeof buffer)) > 0) {
			out_body.append(buffer, static_cast<std::size_t>(got));
		}

		stripUtf8Bom(out_body);
		normalizeCrlf(out_body);
		return got == 0;
	}

	static bool readSourcedText(Executor& exec, const std::string& file, std::string& out_body) {
		if (namesStandardInput(file)) return readStandardInput(out_body);

		return readScriptFile(utf8ToPath(exec.pathConv().toWin32(file)), out_body);
	}

	static int runSourcedFile(Executor& exec, const std::string& file,
			const std::vector<std::string>& args) {
		std::string body;
		if (!readSourcedText(exec, file, body)) {
			shellErrorf("%s: %s\n", file.c_str(), std::strerror(errno));
			exec.markSpecialBuiltinFailure();
			return 1;
		}

		std::vector<std::string> saved = exec.env().positional();
		if (!args.empty()) exec.env().setPositional(args);

		exec.beginSource(file);
		int status = exec.executeInputText(body, file);
		exec.consumeFlow(FlowSignal::Kind::Return, &status);
		exec.fireReturnTrap();
		exec.endSource();
		exec.env().setPositional(std::move(saved));
		return status;
	}

	static int runSourceCommand(Executor& exec, const char* command,
			const std::vector<std::string>& args) {
		source_detail::Request request;
		request.command = command;
		const int usage = source_detail::parseRequest(args, request);
		if (usage != 0) return usage;
		if (exec.env().restricted() && request.file.find('/') != std::string::npos) {
			shellErrorf("%s: %s: restricted\n", command, request.file.c_str());
			return 1;
		}

		std::string file;
		if (!source_detail::resolveFile(exec, request, file)) return 1;
		return runSourcedFile(exec, file, request.args);
	}

	static int builtin_source(Executor& exec, const std::vector<std::string>& args) {
		return runSourceCommand(exec, "source", args);
	}

	static int builtin_dot(Executor& exec, const std::vector<std::string>& args) {
		return runSourceCommand(exec, ".", args);
	}

	namespace type_detail {
		enum class Kind { Alias, Keyword, Function, Builtin, File };

		struct Match {
			Kind kind;
			std::string path;
		};

		struct Options {
			bool all          = false;
			bool no_functions = false;
			bool path_only    = false;
			bool force_path   = false;
			bool type_only    = false;
		};
	}  // namespace type_detail

	static const char* const kReservedWords[] = {
		"!", "case", "coproc", "do", "done", "elif", "else", "esac", "fi", "for", "function",
		"if", "in", "select", "then", "time", "until", "while", "{", "}", "[[", "]]",
	};

	static bool isReservedWord(const std::string& name) {
		for (const char* word : kReservedWords) {
			if (name == word) return true;
		}

		return false;
	}

	static std::vector<type_detail::Match> shellMatches(Executor& exec, const std::string& name,
			const type_detail::Options& opts) {
		using type_detail::Kind;
		std::vector<type_detail::Match> matches;
		if (opts.force_path) return matches;

		if (exec.env().expand_aliases() && exec.isAlias(name)) matches.push_back({ Kind::Alias, {} });
		if (isReservedWord(name)) matches.push_back({ Kind::Keyword, {} });
		if (!opts.no_functions && exec.isFunction(name)) matches.push_back({ Kind::Function, {} });
		if (exec.isBuiltin(name)) matches.push_back({ Kind::Builtin, {} });
		return matches;
	}

	static std::vector<type_detail::Match> commandMatches(Executor& exec, const std::string& name,
			const type_detail::Options& opts) {
		std::vector<type_detail::Match> matches = shellMatches(exec, name, opts);
		if (!opts.all && !matches.empty()) return matches;

		// A missing `/usr/bin/NAME` runs NAME.
		const std::string command = exec.systemBinCommand(name);
		if (command != name && !opts.force_path && exec.isBuiltin(command)) {
			matches.push_back({ type_detail::Kind::Builtin, {} });
			if (!opts.all) return matches;
		}

		std::vector<std::string> paths = exec.findAllExecutables(command);
		if (!opts.all && paths.size() > 1) paths.resize(1);
		for (const std::string& path : paths) {
			matches.push_back({ type_detail::Kind::File, exec.pathConv().toPosix(path) });
		}

		return matches;
	}

	static const char* kindWord(type_detail::Kind kind) {
		switch (kind) {
			case type_detail::Kind::Alias:    return "alias";
			case type_detail::Kind::Keyword:  return "keyword";
			case type_detail::Kind::Function: return "function";
			case type_detail::Kind::Builtin:  return "builtin";
			case type_detail::Kind::File:     return "file";
			default:                          return "";
		}
	}

	static void printFunctionDescription(Executor& exec, const std::string& name) {
		std::printf("%s is a function\n", name.c_str());
		const FunctionDef* def = exec.findFunction(name);
		if (def != nullptr) std::printf("%s\n", functionDefinitionText(*def).c_str());
	}

	static void describeMatch(Executor& exec, const std::string& name,
			const type_detail::Match& match) {
		switch (match.kind) {
			case type_detail::Kind::Alias:
				std::printf("%s is aliased to `%s'\n", name.c_str(), exec.aliasValue(name).c_str());
				break;
			case type_detail::Kind::Keyword:
				std::printf("%s is a shell keyword\n", name.c_str());
				break;
			case type_detail::Kind::Function:
				printFunctionDescription(exec, name);
				break;
			case type_detail::Kind::Builtin:
				std::printf("%s is a shell builtin\n", name.c_str());
				break;
			default:
				std::printf("%s is %s\n", name.c_str(), match.path.c_str());
				break;
		}
	}

	static void printMatch(Executor& exec, const std::string& name,
			const type_detail::Match& match, const type_detail::Options& opts) {
		if (opts.type_only) {
			std::printf("%s\n", kindWord(match.kind));
			return;
		}

		if (opts.path_only || opts.force_path) {
			if (match.kind == type_detail::Kind::File) std::printf("%s\n", match.path.c_str());
			return;
		}

		describeMatch(exec, name, match);
	}

	static bool printCommandType(Executor& exec, const std::string& name,
			const type_detail::Options& opts, const char* builtin_name) {
		const std::vector<type_detail::Match> matches = commandMatches(exec, name, opts);
		if (matches.empty()) {
			const bool quiet = opts.type_only || opts.path_only || opts.force_path;
			if (!quiet) shellErrorf("%s: %s: not found\n", builtin_name, name.c_str());
			return false;
		}

		for (const type_detail::Match& match : matches) printMatch(exec, name, match, opts);
		return true;
	}

	static bool applyTypeFlag(char flag, type_detail::Options& opts) {
		switch (flag) {
			case 'a': opts.all = true; break;
			case 'f': opts.no_functions = true; break;
			case 'p': opts.path_only = true; break;
			case 'P': opts.force_path = true; break;
			case 't': opts.type_only = true; break;
			default:  return false;
		}

		return true;
	}

	static bool parseTypeFlags(const std::vector<std::string>& args, std::size_t& first,
			type_detail::Options& opts) {
		first = 0;
		while (first < args.size() && args[first].size() > 1 && args[first][0] == '-') {
			const std::string& arg = args[first++];
			if (arg == "--") return true;

			for (std::size_t k = 1; k < arg.size(); ++k) {
				if (applyTypeFlag(arg[k], opts)) continue;

				shellErrorf("type: -%c: invalid option\n", arg[k]);
				std::fprintf(stderr, "type: usage: type [-afptP] name [name ...]\n");
				return false;
			}
		}

		return true;
	}

	static std::size_t describeCommands(Executor& exec, const std::vector<std::string>& names,
			std::size_t first, const type_detail::Options& opts, const char* builtin_name) {
		std::size_t found = 0;
		for (std::size_t i = first; i < names.size(); ++i) {
			if (printCommandType(exec, names[i], opts, builtin_name)) ++found;
		}

		std::fflush(stdout);
		return found;
	}

	static int builtin_type(Executor& exec, const std::vector<std::string>& args) {
		type_detail::Options opts;
		std::size_t first = 0;
		if (!parseTypeFlags(args, first, opts)) return 2;

		const std::size_t found = describeCommands(exec, args, first, opts, "type");
		return found == args.size() - first ? 0 : 1;
	}

	static bool printCommandName(Executor& exec, const std::string& name) {
		const std::vector<type_detail::Match> matches = commandMatches(exec, name, {});
		if (matches.empty()) return false;

		const type_detail::Match& match = matches.front();
		if (match.kind == type_detail::Kind::Alias) {
			std::printf("alias %s=%s\n", name.c_str(), quoteSingle(exec.aliasValue(name)).c_str());
		} else {
			std::printf("%s\n", match.kind == type_detail::Kind::File ? match.path.c_str() : name.c_str());
		}

		return true;
	}

	static std::size_t printCommandNames(Executor& exec, const std::vector<std::string>& names,
			std::size_t first) {
		std::size_t found = 0;
		for (std::size_t i = first; i < names.size(); ++i) {
			if (printCommandName(exec, names[i])) ++found;
		}

		std::fflush(stdout);
		return found;
	}

	static int describeForCommand(Executor& exec, const std::vector<std::string>& args,
			std::size_t first, char mode) {
		const std::size_t found = mode == 'v'
			? printCommandNames(exec, args, first)
			: describeCommands(exec, args, first, {}, "command");
		return found != 0 ? 0 : 1;
	}

	static std::size_t parseCommandFlags(const std::vector<std::string>& args, char& mode) {
		std::size_t i = 0;
		while (i < args.size() && args[i].size() > 1 && args[i][0] == '-') {
			const std::string& flag = args[i++];
			if (flag == "--") return i;

			for (std::size_t k = 1; k < flag.size(); ++k) {
				if (flag[k] == 'v' || flag[k] == 'V') mode = flag[k];
				else if (flag[k] != 'p') return i - 1;
			}
		}

		return i;
	}

	static bool usesDefaultPath(const std::vector<std::string>& args, std::size_t flag_count) {
		for (std::size_t i = 0; i < flag_count; ++i) {
			if (args[i] != "--" && args[i].find('p') != std::string::npos) return true;
		}

		return false;
	}

	// `command` skips functions and aliases: a builtin runs in place,
	// anything else is looked up on PATH.
	static int builtin_command(Executor& exec, const std::vector<std::string>& args) {
		char mode = 0;
		const std::size_t first = parseCommandFlags(args, mode);
		if (exec.env().restricted() && usesDefaultPath(args, first)) {
			shellErrorf("command: -p: restricted\n");
			return 1;
		}

		if (first >= args.size()) return 0;

		if (mode != 0) return describeForCommand(exec, args, first, mode);

		const std::vector<std::string> rest(args.begin() + first, args.end());
		const std::string name = exec.systemBinCommand(rest[0]);
		if (exec.isBuiltin(name)) {
			const std::vector<std::string> sub(rest.begin() + 1, rest.end());
			return exec.callBuiltin(name, sub);
		}

		return exec.runExternal(rest, {});
	}

	static bool isBuiltinOptionWord(const std::string& arg) {
		return arg.size() > 1 && arg[0] == '-';
	}

	static void reportBuiltinInvalidOption(const std::string& arg) {
		shellErrorf("builtin: %s: invalid option\n", arg.substr(0, 2).c_str());
		std::fprintf(stderr, "builtin: usage: builtin [shell-builtin [arg ...]]\n");
	}

	static int builtin_builtin(Executor& exec, const std::vector<std::string>& args) {
		std::size_t first = 0;
		if (first < args.size() && args[first] == "--") ++first;
		else if (first < args.size() && isBuiltinOptionWord(args[first])) {
			reportBuiltinInvalidOption(args[first]);
			return 2;
		}

		if (first >= args.size()) return 0;

		const std::string& name = args[first];
		if (!exec.isBuiltin(name)) {
			shellErrorf("builtin: %s: not a shell builtin\n", name.c_str());
			return 1;
		}

		return exec.callBuiltin(name, { args.begin() + first + 1, args.end() });
	}

	namespace shopt_detail {
		enum class Mode { ListAll, Set, Unset, Query };

		/// Which flags a listing shows: `shopt -s` alone lists those on.
		enum class Show { All, On, Off };

		struct ShoptArgs {
			Mode mode = Mode::ListAll;
			bool reusable = false;
			bool set_options = false;
			std::vector<std::string> names;
		};
	}  // namespace shopt_detail

	using shopt_detail::ShoptArgs;

	static bool shoptFlagOn(const Environment& env, const ShoptSpec& flag) {
		return env.shopts().*(flag.flag);
	}

	static void printShoptFlag(const Environment& env, const ShoptSpec& flag, bool reusable) {
		const bool on = shoptFlagOn(env, flag);
		if (reusable) std::printf("shopt %s %s\n", on ? "-s" : "-u", flag.name);
		else          std::printf("%-20s\t%s\n", flag.name, on ? "on" : "off");
	}

	static bool applyShoptOption(char letter, ShoptArgs& parsed) {
		using shopt_detail::Mode;
		switch (letter) {
		case 's': parsed.mode = Mode::Set;     return true;
		case 'u': parsed.mode = Mode::Unset;   return true;
		case 'q': parsed.mode = Mode::Query;   return true;
		case 'p': parsed.reusable = true;      return true;
		case 'o': parsed.set_options = true;   return true;
		default:  return false;
		}
	}

	static bool parseShoptArgs(const std::vector<std::string>& args, ShoptArgs& parsed) {
		for (const std::string& arg : args) {
			if (arg == "--") continue;

			const bool option = arg.size() > 1 && arg[0] == '-' && parsed.names.empty();
			if (!option) {
				parsed.names.push_back(arg);
				continue;
			}

			for (std::size_t k = 1; k < arg.size(); ++k) {
				if (applyShoptOption(arg[k], parsed)) continue;

				shellErrorf("shopt: -%c: invalid option\n", arg[k]);
				std::fprintf(stderr, "shopt: usage: shopt [-pqsu] [-o] [optname ...]\n");
				return false;
			}
		}

		return true;
	}

	static int shoptSetOrUnset(Executor& exec, const std::vector<std::string>& names, bool on) {
		int status = 0;
		for (const std::string& name : names) {
			const ShoptSpec* flag = findShopt(name);
			if (flag == nullptr) {
				shellErrorf("shopt: %s: invalid option name\n", name.c_str());
				status = 1;
				continue;
			}

			// Only the command line makes a login shell; bash ignores the
			// change without an error.
			if (flag->flag == &ShoptFlags::login_shell) continue;

			exec.env().shopts().*(flag->flag) = on;
		}

		return status;
	}

	static int shoptQuery(Executor& exec, const std::vector<std::string>& names) {
		for (const std::string& name : names) {
			const ShoptSpec* flag = findShopt(name);
			if (flag == nullptr) return 1;
			if (!(exec.env().shopts().*(flag->flag))) return 1;
		}

		return 0;
	}

	static void listAllShopts(const Environment& env, shopt_detail::Show show, bool reusable) {
		using shopt_detail::Show;
		for (const ShoptSpec& flag : shoptTable()) {
			const bool on = shoptFlagOn(env, flag);
			if ((show == Show::On && !on) || (show == Show::Off && on)) continue;

			printShoptFlag(env, flag, reusable);
		}
	}

	// As in bash, listing named flags fails when one of them is off.
	static int shoptListNamed(Executor& exec, const std::vector<std::string>& names,
			bool reusable) {
		int status = 0;
		for (const std::string& name : names) {
			const ShoptSpec* flag = findShopt(name);
			if (flag == nullptr) {
				shellErrorf("shopt: %s: invalid option name\n", name.c_str());
				status = 1;
				continue;
			}

			printShoptFlag(exec.env(), *flag, reusable);
			if (!shoptFlagOn(exec.env(), *flag)) status = 1;
		}

		return status;
	}

	static int shoptSetMode(Executor& exec, const ShoptArgs& parsed, bool on) {
		using shopt_detail::Show;
		if (!parsed.names.empty()) return shoptSetOrUnset(exec, parsed.names, on);

		listAllShopts(exec.env(), on ? Show::On : Show::Off, parsed.reusable);
		return 0;
	}

	// `shopt -o`: the same operations on the `set -o` options.
	namespace shopt_detail {

		static const SetOptionSpec* findNamedSetOption(const std::string& name) {
			const SetOptionSpec* spec = findSetOption(name);
			if (spec == nullptr) shellErrorf("shopt: %s: invalid shell option name\n", name.c_str());
			return spec;
		}

		static bool setOptionOn(const Environment& env, const SetOptionSpec& spec) {
			return env.options().*(spec.flag);
		}

		// bash pads a name to 20 columns when it was asked for by name and
		// to 15 when every option is listed (set -o's format).
		static void printSetOption(const Environment& env, const SetOptionSpec& spec,
				bool reusable, int width) {
			const bool on = setOptionOn(env, spec);
			if (reusable) std::printf("set %co %s\n", on ? '-' : '+', spec.name);
			else          std::printf("%-*s\t%s\n", width, spec.name, on ? "on" : "off");
		}

		static int changeSetOptions(Environment& env, const std::vector<std::string>& names,
				bool on) {
			int status = 0;
			for (const std::string& name : names) {
				const SetOptionSpec* spec = findNamedSetOption(name);
				if (spec == nullptr) status = 1;
				else applySetOption(env, *spec, on);
			}

			return status;
		}

		static int querySetOptions(const Environment& env, const std::vector<std::string>& names) {
			for (const std::string& name : names) {
				const SetOptionSpec* spec = findSetOption(name);
				if (spec == nullptr || !setOptionOn(env, *spec)) return 1;
			}

			return 0;
		}

		static int listSetOptions(const Environment& env, const ShoptArgs& parsed, Show show) {
			int status = 0;
			for (const std::string& name : parsed.names) {
				const SetOptionSpec* spec = findNamedSetOption(name);
				if (spec == nullptr || !setOptionOn(env, *spec)) status = 1;
				if (spec != nullptr) printSetOption(env, *spec, parsed.reusable, 20);
			}

			if (!parsed.names.empty()) return status;

			for (const SetOptionSpec& spec : setOptionTable()) {
				const bool on = setOptionOn(env, spec);
				if ((show == Show::On && !on) || (show == Show::Off && on)) continue;

				printSetOption(env, spec, parsed.reusable, 15);
			}

			return 0;
		}

		static int runOnSetOptions(Executor& exec, const ShoptArgs& parsed) {
			Environment& env = exec.env();
			const bool changes = parsed.mode == Mode::Set || parsed.mode == Mode::Unset;
			if (changes && !parsed.names.empty()) {
				return changeSetOptions(env, parsed.names, parsed.mode == Mode::Set);
			}

			if (parsed.mode == Mode::Query) return querySetOptions(env, parsed.names);
			if (parsed.mode == Mode::Set) return listSetOptions(env, parsed, Show::On);
			if (parsed.mode == Mode::Unset) return listSetOptions(env, parsed, Show::Off);
			return listSetOptions(env, parsed, Show::All);
		}

	}  // namespace shopt_detail

	static int builtin_shopt(Executor& exec, const std::vector<std::string>& args) {
		using shopt_detail::Mode;
		using shopt_detail::Show;
		ShoptArgs parsed;
		if (!parseShoptArgs(args, parsed)) return 2;
		if (parsed.set_options) return shopt_detail::runOnSetOptions(exec, parsed);

		switch (parsed.mode) {
		case Mode::Set:     return shoptSetMode(exec, parsed, true);
		case Mode::Unset:   return shoptSetMode(exec, parsed, false);
		case Mode::Query:   return shoptQuery(exec, parsed.names);
		case Mode::ListAll: break;
		}

		if (!parsed.names.empty()) return shoptListNamed(exec, parsed.names, parsed.reusable);

		listAllShopts(exec.env(), Show::All, parsed.reusable);
		return 0;
	}

	namespace mapfile_detail {
		struct MapfileOptions {
			bool strip_delimiter = false;
			char delimiter = '\n';
			long long max_lines = -1;
			long long origin = 0;
			long long skip = 0;
			long long quantum = 5000;
			std::string callback;
			std::string array_name = "MAPFILE";
		};
	}  // namespace mapfile_detail

	static bool parseMapfileArgs(const std::vector<std::string>& args,
			mapfile_detail::MapfileOptions& options) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			const bool has_value = i + 1 < args.size();
			if (arg == "-t") {
				options.strip_delimiter = true;
			} else if (arg == "-d" && has_value) {
				options.delimiter = args[++i].empty() ? '\0' : args[i][0];
			} else if (arg.size() > 2 && arg.compare(0, 2, "-d") == 0) {
				options.delimiter = arg[2];
			} else if (arg == "-n" && has_value) {
				parseLL(args[++i], options.max_lines);
			} else if (arg == "-O" && has_value) {
				parseLL(args[++i], options.origin);
			} else if (arg == "-s" && has_value) {
				parseLL(args[++i], options.skip);
			} else if (arg == "-u" && has_value) {
				++i;
			} else if (arg == "-C" && has_value) {
				options.callback = args[++i];
			} else if (arg == "-c" && has_value) {
				if (!parseLL(args[++i], options.quantum) || options.quantum <= 0) {
					shellErrorf("mapfile: %s: invalid callback quantum\n", args[i].c_str());
					return false;
				}
			} else if (!arg.empty() && arg[0] == '-' && arg != "-" && arg != "--") {
				shellErrorf("mapfile: unknown option: %s\n", arg.c_str());
				return false;
			} else {
				options.array_name = arg;
			}
		}

		return true;
	}

	static bool mapfileHasRoom(const mapfile_detail::MapfileOptions& options, std::size_t count) {
		return options.max_lines <= 0 || static_cast<long long>(count) < options.max_lines;
	}

	// A last line with no delimiter still counts, unless it falls inside
	// the skipped range.
	static std::vector<std::string> readMapfileLines(
			const mapfile_detail::MapfileOptions& options) {
		std::vector<std::string> lines;
		std::string current;
		long long skipped = 0;
		for (int ch = std::fgetc(stdin); ch != EOF; ch = std::fgetc(stdin)) {
			if (static_cast<char>(ch) != options.delimiter) {
				current.push_back(static_cast<char>(ch));
				continue;
			}

			if (skipped < options.skip) {
				++skipped;
				current.clear();
				continue;
			}

			if (!options.strip_delimiter && options.delimiter != '\0') {
				current.push_back(options.delimiter);
			}
			lines.push_back(std::move(current));
			current.clear();
			if (!mapfileHasRoom(options, lines.size())) break;
		}

		if (!current.empty() && skipped >= options.skip && mapfileHasRoom(options, lines.size())) {
			lines.push_back(std::move(current));
		}

		return lines;
	}

	static bool callbackDue(const mapfile_detail::MapfileOptions& options, std::size_t line) {
		const long long count = static_cast<long long>(line) + 1;
		return !options.callback.empty() && count % options.quantum == 0;
	}

	// -C: every quantum lines, `callback INDEX LINE` runs before the line
	// is stored.
	static void runMapfileCallback(Executor& exec, const mapfile_detail::MapfileOptions& options,
			long long index, const std::string& line) {
		exec.executeText(options.callback + " " + std::to_string(index) + " " + quoteSingle(line),
			"mapfile");
	}

	static int builtin_mapfile(Executor& exec, const std::vector<std::string>& args) {
		mapfile_detail::MapfileOptions options;
		if (!parseMapfileArgs(args, options)) return 1;

		std::vector<std::string> lines = readMapfileLines(options);
		std::map<long long, std::string> elements;
		for (std::size_t i = 0; i < lines.size(); ++i) {
			const long long index = options.origin + static_cast<long long>(i);
			if (callbackDue(options, i)) runMapfileCallback(exec, options, index, lines[i]);
			elements[index] = std::move(lines[i]);
		}

		exec.env().setIndexedArraySparse(options.array_name, std::move(elements));
		return 0;
	}

	namespace getopts_detail {
		static bool isVariableName(const std::string& name) {
			if (name.empty() || std::isdigit(static_cast<unsigned char>(name[0]))) return false;

			for (const char c : name) {
				if (c != '_' && !std::isalnum(static_cast<unsigned char>(c))) return false;
			}

			return true;
		}

		struct GetoptsCtx {
			Executor& exec;
			std::string opts;
			const std::string& name;
			std::vector<std::string> source;
			bool silent;
			int& sub;
			int optind;

			void setName(const std::string& value) const {
				if (isVariableName(name)) exec.env().set(name, value);
			}
			void storeOptind() const {
				exec.env().set("OPTIND", std::to_string(optind));
				exec.noteGetoptsWroteOptind();
			}
		};

		static int loadOptind(Executor& exec) {
			int value = 1;
			const std::string text = exec.env().get("OPTIND");
			if (!text.empty() && !parseInt(text, value)) value = 1;
			return value < 1 ? 1 : value;
		}

		static int finishWithoutOption(GetoptsCtx& ctx) {
			ctx.setName("?");
			ctx.exec.env().unsetNameref("OPTARG");
			ctx.exec.resetGetopts();
			ctx.storeOptind();
			return 1;
		}

		static int finishOnDoubleDash(GetoptsCtx& ctx) {
			++ctx.optind;
			ctx.exec.resetGetopts();
			ctx.setName("?");
			ctx.storeOptind();
			return 1;
		}

		static void advanceOneOptionChar(GetoptsCtx& ctx, const std::string& current) {
			++ctx.sub;
			if (ctx.sub >= static_cast<int>(current.size())) {
				++ctx.optind;
				ctx.sub = 1;
			}
		}

		// OPTERR=0 silences the messages, as a leading `:` does.
		static bool reportsErrors(const GetoptsCtx& ctx) {
			int opterr = 1;
			const std::string text = ctx.exec.env().get("OPTERR");
			return text.empty() || !parseInt(text, opterr) || opterr != 0;
		}

		static int handleIllegalOption(GetoptsCtx& ctx, const std::string& current, char opt) {
			if (ctx.silent) {
				ctx.setName("?");
				ctx.exec.env().set("OPTARG", std::string(1, opt));
			} else {
				if (reportsErrors(ctx)) {
					std::fprintf(stderr, "%sillegal option -- %c\n", scriptNamePrefix().c_str(), opt);
				}

				ctx.setName("?");
				ctx.exec.env().unsetNameref("OPTARG");
			}

			advanceOneOptionChar(ctx, current);
			ctx.storeOptind();
			return 0;
		}

		static int handleMissingOptionArg(GetoptsCtx& ctx, char opt) {
			if (ctx.silent) {
				ctx.setName(":");
				ctx.exec.env().set("OPTARG", std::string(1, opt));
			} else {
				if (reportsErrors(ctx)) {
					std::fprintf(stderr, "%soption requires an argument -- %c\n",
						scriptNamePrefix().c_str(), opt);
				}

				ctx.setName("?");
				ctx.exec.env().unsetNameref("OPTARG");
			}

			++ctx.optind;
			ctx.sub = 1;
			ctx.storeOptind();
			return 0;
		}

		static int handleOptionWithArg(GetoptsCtx& ctx, const std::string& current, char opt) {
			if (ctx.sub + 1 < static_cast<int>(current.size())) {
				const std::string optarg = current.substr(ctx.sub + 1);
				++ctx.optind;
				ctx.sub = 1;
				ctx.setName(std::string(1, opt));
				ctx.exec.env().set("OPTARG", optarg);
				ctx.storeOptind();
				return 0;
			}

			if (ctx.optind >= static_cast<int>(ctx.source.size())) {
				return handleMissingOptionArg(ctx, opt);
			}

			const std::string optarg = ctx.source[ctx.optind];
			ctx.optind += 2;
			ctx.sub = 1;
			ctx.setName(std::string(1, opt));
			ctx.exec.env().set("OPTARG", optarg);
			ctx.storeOptind();
			return 0;
		}

		static int handleFlagOption(GetoptsCtx& ctx, const std::string& current, char opt) {
			ctx.setName(std::string(1, opt));
			ctx.exec.env().unsetNameref("OPTARG");
			advanceOneOptionChar(ctx, current);
			ctx.storeOptind();
			return 0;
		}
	}  // namespace getopts_detail

	static int parseNextOption(Executor& exec, const std::vector<std::string>& args);

	// As in bash, the option is consumed (OPTIND moves on) before a bad
	// variable name is reported.
	static int builtin_getopts(Executor& exec, const std::vector<std::string>& args) {
		const int status = parseNextOption(exec, args);
		if (status == 2 || getopts_detail::isVariableName(args[1])) return status;

		shellErrorf("getopts: `%s': not a valid identifier\n", args[1].c_str());
		return 1;
	}

	static int parseNextOption(Executor& exec, const std::vector<std::string>& args) {
		if (!args.empty() && args[0].size() > 1 && args[0][0] == '-' && args[0] != "--") {
			shellErrorf("getopts: %s: invalid option\n", args[0].substr(0, 2).c_str());
			printUsage("getopts: usage: getopts optstring name [arg ...]");
			return 2;
		}

		if (args.size() < 2) {
			printUsage("getopts: usage: getopts optstring name [arg ...]");
			return 2;
		}

		std::string opts = args[0];
		const bool silent = !opts.empty() && opts[0] == ':';
		if (silent) opts.erase(0, 1);

		std::vector<std::string> source = args.size() > 2
			? std::vector<std::string>(args.begin() + 2, args.end())
			: exec.env().positional();

		exec.syncGetoptsWithOptind();
		int& sub = exec.getoptsSubindex();
		if (sub < 1) sub = 1;

		getopts_detail::GetoptsCtx ctx{
			exec, std::move(opts), args[1], std::move(source),
			silent, sub, getopts_detail::loadOptind(exec),
		};

		for (;;) {
			if (ctx.optind > static_cast<int>(ctx.source.size())) {
				return getopts_detail::finishWithoutOption(ctx);
			}

			const std::string& current = ctx.source[ctx.optind - 1];
			if (current.size() < 2 || current[0] != '-' || current == "-") {
				return getopts_detail::finishWithoutOption(ctx);
			}

			if (current == "--") return getopts_detail::finishOnDoubleDash(ctx);

			if (ctx.sub >= static_cast<int>(current.size())) {
				++ctx.optind;
				ctx.sub = 1;
				continue;
			}

			const char opt = current[ctx.sub];
			const std::size_t pos = ctx.opts.find(opt);
			if (pos == std::string::npos || opt == ':') {
				return getopts_detail::handleIllegalOption(ctx, current, opt);
			}

			if (pos + 1 < ctx.opts.size() && ctx.opts[pos + 1] == ':') {
				return getopts_detail::handleOptionWithArg(ctx, current, opt);
			}

			return getopts_detail::handleFlagOption(ctx, current, opt);
		}
	}

	static void printJob(const Executor& exec, const Executor::Job& job, char format) {
		if (format == 'p') {
			std::printf("%lld\n", job.pid);
			return;
		}

		std::string line = exec.jobLine(job);
		if (format == 'l') {
			char pid[32];
			std::snprintf(pid, sizeof(pid), "%5lld ", job.pid);
			line.replace(line.find(']') + 3, 1, pid);
		}

		std::printf("%s\n", line.c_str());
	}

	// `jobs`, `jobs -l` (with PIDs), `jobs -p` (PIDs only). Jobs shown as
	// finished leave the table, as bash's do once reported.
	static int builtin_jobs(Executor& exec, const std::vector<std::string>& args) {
		char format = ' ';
		for (const std::string& arg : args) {
			if (arg == "-l") format = 'l';
			else if (arg == "-p") format = 'p';
		}

		exec.reapJobs();
		std::vector<int> finished;
		for (const Executor::Job& job : exec.jobsTable()) {
			printJob(exec, job, format);
			if (!job.running) finished.push_back(job.id);
		}

		for (const int id : finished) exec.forgetJob(id);
		return 0;
	}

	static bool resolveJobSpec(Executor& exec, const std::string& spec, int& out_id) {
		out_id = -1;
		if (!spec.empty() && spec[0] == '%') {
			const Executor::Job* job = exec.findJob(spec);
			if (job != nullptr) out_id = job->id;
			return job != nullptr;
		}

		long long pid = 0;
		if (!parseLL(spec, pid)) return false;

		for (const Executor::Job& job : exec.jobsTable()) {
			if (job.pid != pid) continue;

			out_id = job.id;
			return true;
		}

		return true;
	}

	// A PID that is no job can be a `<(...)` / `>(...)` process (`wait $!`)
	// or a job that already finished and left the table.
	static bool waitForProcSubstSpec(Executor& exec, const std::string& spec, int& status) {
		long long pid = 0;
		if (!parseLL(spec, pid)) return false;

		return exec.waitForProcSubstChild(pid, status) || exec.finishedJobStatus(pid, status);
	}

	static int builtinWaitAny(Executor& exec) {
		const int status = exec.waitForAnyJob();
		return status < 0 ? 127 : status;
	}

	static int waitForAll(Executor& exec) {
		const int interrupted = exec.waitForAllJobs(true);
		if (interrupted != 0) return interrupted;

		exec.waitForProcSubstChildren();
		return 0;
	}

	// A signal with a trap, or one that ends the shell, cuts the wait short
	// with 128 + its number, as in bash; the trap runs once `wait` returns.
	static int builtin_wait(Executor& exec, const std::vector<std::string>& args) {
		if (!args.empty() && args[0] == "-n") return builtinWaitAny(exec);
		if (args.empty()) return waitForAll(exec);

		int status = 0;
		for (const std::string& spec : args) {
			int id = -1;
			if (!resolveJobSpec(exec, spec, id) || id < 0) {
				if (!waitForProcSubstSpec(exec, spec, status)) status = 127;
				continue;
			}

			const int job_status = exec.waitForJob(id, true);
			if (job_status >= 0) status = job_status;
			if (exec.interruptingSignal() != 0) return status;
		}

		return status;
	}

	static void removeJob(Executor& exec, int id) {
		std::vector<Executor::Job>& jobs = exec.jobsTable();
		jobs.erase(std::remove_if(jobs.begin(), jobs.end(),
			[id](const Executor::Job& job) { return job.id == id; }), jobs.end());
	}

	static int builtin_disown(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) {
			exec.jobsTable().clear();
			return 0;
		}

		for (const std::string& spec : args) {
			int id = -1;
			if (resolveJobSpec(exec, spec, id) && id >= 0) removeJob(exec, id);
		}

		return 0;
	}

	static int builtin_fg(Executor& exec, const std::vector<std::string>& args) {
		exec.reapJobs();
		const std::string spec = args.empty() ? std::string("%+") : args[0];
		const Executor::Job* job = exec.findJob(spec);
		if (job == nullptr) {
			if (args.empty()) printerr("fg: current: no such job");
			else shellErrorf("fg: %s: no such job\n", spec.c_str());
			return 1;
		}

		std::printf("%s\n", job->cmd_text.c_str());
		std::fflush(stdout);
		const int status = exec.foregroundJob(job->id);
		return status < 0 ? 1 : status;
	}

	// Every job wbsh starts already runs in the background, so `bg` only
	// confirms that, the way bash answers for a job that is not stopped.
	static int builtin_bg(Executor& exec, const std::vector<std::string>& args) {
		if (!exec.env().interactive()) {
			printerr("bg: no job control");
			return 1;
		}

		exec.reapJobs();
		const std::string spec = args.empty() ? std::string("%+") : args[0];
		const Executor::Job* job = exec.findJob(spec);
		if (job == nullptr) {
			shellErrorf("bg: %s: no such job\n", spec.c_str());
			return 1;
		}

		if (exec.continueJobInBackground(job->id)) return 0;

		if (job->running) shellErrorf("bg: job %d already in background\n", job->id);
		return 0;
	}

	// The form bash prints, which reads back as the same alias.
	static void printAliasDefinition(Executor& exec, const std::string& name) {
		const char* end_of_options = (!name.empty() && name[0] == '-') ? "-- " : "";
		std::printf("alias %s%s=%s\n", end_of_options, name.c_str(),
			quoteSingle(exec.aliasValue(name)).c_str());
	}

	static void printAllAliases(Executor& exec) {
		for (const auto& entry : sortedEntries(exec.aliases())) {
			printAliasDefinition(exec, entry.first);
		}
	}

	static bool printAlias(Executor& exec, const std::string& name) {
		if (!exec.isAlias(name)) {
			shellErrorf("alias: %s: not found\n", name.c_str());
			return false;
		}

		printAliasDefinition(exec, name);
		return true;
	}

	static bool parseAliasFlags(const std::string& arg, bool& print_all) {
		for (std::size_t k = 1; k < arg.size(); ++k) {
			if (arg[k] == 'p') {
				print_all = true;
				continue;
			}

			shellErrorf("alias: -%c: invalid option\n", arg[k]);
			std::fprintf(stderr, "alias: usage: alias [-p] [name[=value] ... ]\n");
			return false;
		}

		return true;
	}

	static bool parseAliasOptions(const std::vector<std::string>& args, std::size_t& first,
			bool& print_all) {
		for (; first < args.size(); ++first) {
			const std::string& arg = args[first];
			if (arg == "--") {
				++first;
				return true;
			}

			if (arg.size() < 2 || arg[0] != '-') return true;
			if (!parseAliasFlags(arg, print_all)) return false;
		}

		return true;
	}

	// `-p` lists every alias before the named ones.
	static int builtin_alias(Executor& exec, const std::vector<std::string>& args) {
		std::size_t first = 0;
		bool print_all = false;
		if (!parseAliasOptions(args, first, print_all)) return 2;
		if (print_all || first == args.size()) printAllAliases(exec);

		int status = 0;
		for (std::size_t i = first; i < args.size(); ++i) {
			const std::string& arg = args[i];
			std::string name;
			std::string value;
			if (splitAssignment(arg, name, value)) {
				exec.setAlias(name, std::move(value));
				continue;
			}

			if (!printAlias(exec, name)) status = 1;
		}

		return status;
	}

	// Options end at `--` or the first name; only -a is one.
	static bool parseUnaliasArgs(const std::vector<std::string>& args, bool& all,
			std::vector<std::string>& names) {
		std::size_t i = 0;
		for (; i < args.size() && args[i].size() > 1 && args[i][0] == '-'; ++i) {
			if (args[i] == "--") {
				++i;
				break;
			}

			if (args[i] == "-a") {
				all = true;
				continue;
			}

			printerr("unalias: " + args[i].substr(0, 2) + ": invalid option");
			printUsage("unalias: usage: unalias [-a] name [name ...]");
			return false;
		}

		names.assign(args.begin() + static_cast<std::ptrdiff_t>(i), args.end());
		return true;
	}

	static int builtin_unalias(Executor& exec, const std::vector<std::string>& args) {
		bool all = false;
		std::vector<std::string> names;
		if (!parseUnaliasArgs(args, all, names)) return 2;

		if (all) {
			for (const std::string& name : keysOf(exec.aliases())) exec.unsetAlias(name);
			return 0;
		}

		int status = 0;
		for (const std::string& name : names) {
			if (!exec.isAlias(name)) {
				shellErrorf("unalias: %s: not found\n", name.c_str());
				status = 1;
				continue;
			}

			exec.unsetAlias(name);
		}

		return status;
	}

	static std::string historyFilePath(Executor& exec) {
		std::string path = exec.env().get("HISTFILE");
		if (path.empty()) {
			const std::string home = exec.env().get("HOME");
			if (home.empty()) return std::string();

			path = home + kHistoryFileName;
		}

		return exec.pathConv().toWin32(path);
	}

	static int writeHistoryFile(Executor& exec) {
		const std::string path = historyFilePath(exec);
		if (path.empty() || !exec.writeHistory(path)) {
			printerr("history: cannot write history file");
			return 1;
		}

		return 0;
	}

	static int appendHistoryFileEntries(Executor& exec) {
		const std::string path = historyFilePath(exec);
		if (path.empty() || !exec.appendNewHistory(path)) {
			printerr("history: cannot append to history file");
			return 1;
		}

		return 0;
	}

	static int readHistoryFile(Executor& exec) {
		const std::string path = historyFilePath(exec);
		if (path.empty() || !exec.loadHistoryFromFile(path)) {
			printerr("history: cannot read history file");
			return 1;
		}

		return 0;
	}

	static int appendHistoryEntry(Executor& exec, const std::vector<std::string>& args) {
		if (args.size() < 2) return 0;

		exec.addHistoryEntry(joinWithSpaces(args.begin() + 1, args.end()));
		return 0;
	}

	static std::size_t historyStartIndex(const std::vector<std::string>& args, std::size_t total) {
		long count = -1;
		if (!args.empty()) {
			bool ok = false;
			const long long value = toIntSafe(args[0], ok);
			if (ok) count = static_cast<long>(value);
		}

		if (count > 0 && static_cast<std::size_t>(count) < total) {
			return total - static_cast<std::size_t>(count);
		}

		return 0;
	}

	// HISTTIMEFORMAT, when set, is the strftime format of the time shown
	// before each entry; an entry with no time shows ??, as in bash.
	static std::string historyTimeText(Executor& exec, std::size_t index) {
		if (!exec.env().has("HISTTIMEFORMAT")) return {};

		const std::vector<long long>& times = exec.historyTimes();
		const long long time = index < times.size() ? times[index] : 0;
		if (time <= 0) return "??";

		const std::time_t when = static_cast<std::time_t>(time);
		const std::tm* local = std::localtime(&when);
		const std::string format = exec.env().get("HISTTIMEFORMAT");
		char text[256];
		const std::size_t length = local == nullptr
			? 0 : std::strftime(text, sizeof(text), format.c_str(), local);
		return std::string(text, length);
	}

	static int printHistory(Executor& exec, const std::vector<std::string>& args) {
		const std::vector<std::string>& history = exec.history();
		const std::size_t start = historyStartIndex(args, history.size());
		for (std::size_t i = start; i < history.size(); ++i) {
			const std::string stamp = historyTimeText(exec, i);
			std::printf("%5zu  %s%s\n", i + 1, stamp.c_str(), history[i].c_str());
		}

		return 0;
	}

	static int builtin_history(Executor& exec, const std::vector<std::string>& args) {
		if (!args.empty()) {
			if (args[0] == "-c") {
				exec.clearHistory();
				return 0;
			}

			if (args[0] == "-w") return writeHistoryFile(exec);
			if (args[0] == "-a") return appendHistoryFileEntries(exec);
			if (args[0] == "-r") return readHistoryFile(exec);
			if (args[0] == "-s") return appendHistoryEntry(exec, args);
		}

		return printHistory(exec, args);
	}

	namespace revsearch_detail {
		struct RevsearchOptions {
			bool forward = false;
			long long start_one_based = -1;
			std::string query;
		};
	}  // namespace revsearch_detail

	static int parseRevsearchArgs(const std::vector<std::string>& args,
			revsearch_detail::RevsearchOptions& options) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg == "-f") {
				options.forward = true;
				continue;
			}

			if (arg != "-c") {
				options.query = arg;
				continue;
			}

			if (i + 1 >= args.size()) {
				printerr("__revsearch: -c requires an argument");
				return 2;
			}

			bool ok = false;
			options.start_one_based = toIntSafe(args[++i], ok);
			if (!ok || options.start_one_based < 1) {
				printerr("__revsearch: -c expects a positive integer");
				return 2;
			}
		}

		return 0;
	}

	static std::size_t revsearchStart(const std::vector<std::string>& history,
			long long start_one_based) {
		if (history.empty()) return 0;
		if (start_one_based < 0) return history.size() - 1;
		return static_cast<std::size_t>(start_one_based) - 1;
	}

	// Test hook driving findReverseSearchMatch without a TTY:
	//     __revsearch [-c START_INDEX] [-f(orward)] QUERY
	// prints "INDEX MATCHED_LINE" (1-based) or "no match".
	static int builtin_revsearch(Executor& exec, const std::vector<std::string>& args) {
		revsearch_detail::RevsearchOptions options;
		const int status = parseRevsearchArgs(args, options);
		if (status != 0) return status;

		const std::vector<std::string>& history = exec.history();
		const std::size_t start = revsearchStart(history, options.start_one_based);
		const std::size_t found = findReverseSearchMatch(history, options.query, start,
			options.forward);
		if (found >= history.size()) {
			std::printf("no match\n");
			return 1;
		}

		std::printf("%zu %s\n", found + 1, history[found].c_str());
		return 0;
	}

	// Test hook driving findInlinePrediction without a TTY:
	//     __predict PREFIX
	// prints the predicted suffix verbatim or "no prediction".
	static int builtin_predict(Executor& exec, const std::vector<std::string>& args) {
		const std::string prefix = args.empty() ? std::string() : args.front();
		const std::string suffix = findInlinePrediction(exec.history(), exec.historyStatus(),
			prefix);
		if (suffix.empty()) {
			std::printf("no prediction\n");
			return 1;
		}

		std::printf("%s\n", suffix.c_str());
		return 0;
	}

	// Test hook recording a history entry's exit status (what the REPL
	// does after each command):
	//     __histstat INDEX STATUS    (INDEX is 1-based)
	static int builtin_ctrlc(Executor&, const std::vector<std::string>& args) {
		int milliseconds = 0;
		if (args.size() != 1 || !parseInt(args[0], milliseconds) || milliseconds < 0) {
			printUsage("__ctrlc: usage: __ctrlc MILLISECONDS");
			return 2;
		}

		pressCtrlCAfter(static_cast<unsigned long>(milliseconds));
		return 0;
	}

	// Test hook: leaves an AF_UNIX socket file at each PATH, as a server
	// that has bound one would.
	static int builtin_mksock(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) {
			printUsage("__mksock: usage: __mksock PATH...");
			return 2;
		}

		int status = 0;
		for (const std::string& path : args) {
			NetError error;
			if (netCreateUnixSocketFile(exec.pathConv().toWin32(path), error)) continue;

			printerr("__mksock: " + path + ": " + netErrorText(error.code));
			status = 1;
		}

		return status;
	}

	static int builtin_histstat(Executor& exec, const std::vector<std::string>& args) {
		if (args.size() != 2) {
			printUsage("__histstat: usage: __histstat INDEX STATUS");
			return 2;
		}

		bool ok = false;
		const long long index = toIntSafe(args[0], ok);
		if (!ok || index < 1) {
			printerr("__histstat: INDEX must be a positive integer");
			return 2;
		}

		const long long status = toIntSafe(args[1], ok);
		if (!ok) {
			printerr("__histstat: STATUS must be an integer");
			return 2;
		}

		exec.setHistoryEntryStatus(static_cast<std::size_t>(index - 1), static_cast<int>(status));
		return 0;
	}

	static int builtin_let(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) return 1;

		Expander& expander = exec.expander();
		long long last = 0;
		for (const std::string& expression : args) {
			last = expander.evalArith(expression);
			if (expander.failed()) {
				const char* label = expander.errorLabel("let: ");
				shellErrorf("%s%s\n", label, expander.takeError().c_str());
				return 1;
			}
		}

		return last != 0 ? 0 : 1;
	}

	// bash lists only the permissions the mask lets through: `g=rx`.
	static std::string umaskTriplet(int mask, int shift) {
		std::string bits;
		if ((mask & (0400 >> shift)) == 0) bits.push_back('r');
		if ((mask & (0200 >> shift)) == 0) bits.push_back('w');
		if ((mask & (0100 >> shift)) == 0) bits.push_back('x');
		return bits;
	}

	static int printUmask(Executor& exec, bool symbolic) {
		std::string current = exec.env().get("_WBSH_UMASK");
		if (current.empty()) current = kDefaultUmask;
		if (!symbolic) {
			std::printf("%s\n", current.c_str());
			return 0;
		}

		int mask = 0;
		if (!parseInt(current, mask, 8)) mask = 022;
		std::printf("u=%s,g=%s,o=%s\n", umaskTriplet(mask, 0).c_str(),
			umaskTriplet(mask, 3).c_str(), umaskTriplet(mask, 6).c_str());
		return 0;
	}

	static int setUmask(Executor& exec, const std::string& text) {
		int mask = 0;
		if (!parseInt(text, mask, 8)) return 1;

		char stored[16];
		std::snprintf(stored, sizeof(stored), "%04o", mask & 0777);
		exec.env().set("_WBSH_UMASK", stored);
#ifdef _WIN32
		_umask(mask & 0700);
#endif
		return 0;
	}

	static int builtin_umask(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) return printUmask(exec, false);
		if (args.size() != 1) return 1;
		if (args[0] == "-S") return printUmask(exec, true);
		if (args[0].empty() || args[0][0] == '-') return 1;

		return setUmask(exec, args[0]);
	}

	namespace hash_detail {

		struct Request {
			bool reset = false;
			bool show_paths = false;
			bool reusable = false;
			bool forget = false;
			bool has_path = false;
			std::string path;
			std::vector<std::string> names;
		};

		static bool reportUsage(const std::string& option) {
			printerr("hash: " + option + ": invalid option");
			printUsage("hash: usage: hash [-lr] [-p pathname] [-dt] [name ...]");
			return false;
		}

		static bool applyLetter(const std::vector<std::string>& args, std::size_t& i,
				std::size_t k, Request& request) {
			switch (args[i][k]) {
			case 'r': request.reset = true;      return true;
			case 't': request.show_paths = true; return true;
			case 'l': request.reusable = true;   return true;
			case 'd': request.forget = true;     return true;
			case 'p':
				if (i + 1 >= args.size()) {
					printerr("hash: -p: option requires an argument");
					return false;
				}

				request.has_path = true;
				request.path = args[++i];
				return true;
			default:
				return reportUsage(std::string("-") + args[i][k]);
			}
		}

		static bool parse(const std::vector<std::string>& args, Request& request) {
			std::size_t i = 0;
			for (; i < args.size() && args[i].size() > 1 && args[i][0] == '-'; ++i) {
				if (args[i] == "--") {
					++i;
					break;
				}

				const std::size_t option_word = i;
				for (std::size_t k = 1; k < args[option_word].size(); ++k) {
					if (!applyLetter(args, i, k, request)) return false;
				}
			}

			request.names.assign(args.begin() + static_cast<std::ptrdiff_t>(i), args.end());
			return true;
		}

		static void list(const Executor& exec, bool reusable) {
			const auto& hashed = exec.hashedCommands();
			if (hashed.empty()) {
				std::printf("hash: hash table empty\n");
				return;
			}

			if (!reusable) std::printf("hits\tcommand\n");
			for (const auto& entry : hashed) {
				if (reusable) std::printf("builtin hash -p %s %s\n", entry.second.c_str(), entry.first.c_str());
				else std::printf("   0\t%s\n", entry.second.c_str());
			}
		}

		static int showPaths(const Executor& exec, const std::vector<std::string>& names) {
			int status = 0;
			for (const std::string& name : names) {
				const auto found = exec.hashedCommands().find(name);
				if (found == exec.hashedCommands().end()) {
					printerr("hash: " + name + ": not found");
					status = 1;
					continue;
				}

				if (names.size() > 1) std::printf("%s\t", name.c_str());
				std::printf("%s\n", found->second.c_str());
			}

			return status;
		}

		static int forget(Executor& exec, const std::vector<std::string>& names) {
			int status = 0;
			for (const std::string& name : names) {
				if (exec.unhashCommand(name)) continue;

				printerr("hash: " + name + ": not found");
				status = 1;
			}

			return status;
		}

		static int remember(Executor& exec, const std::vector<std::string>& names) {
			int status = 0;
			for (const std::string& name : names) {
				if (exec.isBuiltin(name) || exec.isFunction(name)) continue;

				const std::string found = exec.findExecutable(name);
				if (found.empty()) {
					printerr("hash: " + name + ": not found");
					status = 1;
					continue;
				}

				exec.hashCommand(name, exec.pathConv().toPosix(found));
			}

			return status;
		}

	}  // namespace hash_detail

	static int runHashRequest(Executor& exec, const hash_detail::Request& request) {
		if (request.reset) exec.clearExecutablePathCache();
		if (request.has_path && !exec.restrictedHashTargetAllowed(request.path, "hash: ")) return 1;

		if (request.has_path) {
			for (const std::string& name : request.names) exec.hashCommand(name, request.path);
			return 0;
		}

		if (request.forget) return hash_detail::forget(exec, request.names);
		if (request.show_paths) return hash_detail::showPaths(exec, request.names);
		if (!request.names.empty()) return hash_detail::remember(exec, request.names);
		if (!request.reset) hash_detail::list(exec, request.reusable);
		return 0;
	}

	static int builtin_hash(Executor& exec, const std::vector<std::string>& args) {
		hash_detail::Request request;
		if (!hash_detail::parse(args, request)) return 2;
		return runHashRequest(exec, request);
	}

#ifdef _WIN32
	static const double kFileTimeTicksPerSecond = 10000000.0;

	static double fileTimeSeconds(const FILETIME& time) {
		const unsigned long long ticks =
			(static_cast<unsigned long long>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
		return ticks / kFileTimeTicksPerSecond;
	}

	static void printTimesLine(double user, double kernel) {
		const int user_minutes = static_cast<int>(user / 60);
		const int kernel_minutes = static_cast<int>(kernel / 60);
		std::printf("%dm%.3fs %dm%.3fs\n", user_minutes, user - user_minutes * 60,
			kernel_minutes, kernel - kernel_minutes * 60);
	}
#endif /* _WIN32 */

	static int builtin_times(Executor&, const std::vector<std::string>&) {
#ifdef _WIN32
		FILETIME created;
		FILETIME exited;
		FILETIME kernel;
		FILETIME user;
		if (::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user) != 0) {
			printTimesLine(fileTimeSeconds(user), fileTimeSeconds(kernel));
		} else {
			printTimesLine(0.0, 0.0);
		}

		printTimesLine(0.0, 0.0);
#endif /* _WIN32 */
		return 0;
	}

	static const std::string* callStackElement(const Environment& env, const char* name,
			long long index) {
		const Environment::IndexedArray* array = env.getIndexedArray(name);
		if (array == nullptr) return nullptr;

		const auto found = array->find(index);
		return found == array->end() ? nullptr : &found->second;
	}

	static int printCallerLine(const Environment& env) {
		const std::string* line = callStackElement(env, "BASH_LINENO", 0);
		if (line == nullptr) return 1;

		const std::string* source = callStackElement(env, "BASH_SOURCE", 1);
		std::printf("%s %s\n", line->c_str(), source != nullptr ? source->c_str() : "NULL");
		return 0;
	}

	static int printCallerFrame(const Environment& env, long long level) {
		const std::string* line = callStackElement(env, "BASH_LINENO", level);
		const std::string* name = callStackElement(env, "FUNCNAME", level + 1);
		const std::string* source = callStackElement(env, "BASH_SOURCE", level + 1);
		if (line == nullptr || name == nullptr || source == nullptr) return 1;

		std::printf("%s %s %s\n", line->c_str(), name->c_str(), source->c_str());
		return 0;
	}

	// The frames come from the call-stack arrays, which a pipeline stage
	// running in a child process inherits.
	static int builtin_caller(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) return printCallerLine(exec.env());

		bool ok = false;
		const long long level = toIntSafe(args[0], ok);
		if (!ok || level < 0) {
			printerr("caller: " + args[0] + ": invalid number");
			return 2;
		}

		return printCallerFrame(exec.env(), level);
	}

	static void printUtilCommandIndex(Executor& exec) {
		std::vector<std::string> names = exec.pluginCommandNames();
		if (names.empty()) return;

		std::sort(names.begin(), names.end());
		std::printf("\nadded by utils:\n\n");
		for (const std::string& name : names) {
			const PluginCommand* plugin = exec.pluginCommand(name);
			std::printf("  %-14s %s\n", name.c_str(), plugin->summary.c_str());
		}
	}

	static void printBuiltinIndex(Executor& exec) {
		std::printf("wbsh built-in commands:\n\n");

		std::vector<std::string> names = exec.builtinNames();
		std::sort(names.begin(), names.end());
		for (std::size_t i = 0; i < names.size(); ++i) {
			std::printf("  %-14s", names[i].c_str());
			if (i % kHelpColumns == kHelpColumns - 1) std::printf("\n");
		}

		if (names.size() % kHelpColumns != 0) std::printf("\n");
		printUtilCommandIndex(exec);
		std::printf("\nUse `help NAME` for more on a specific builtin.\n");
	}

	// A util's command knows its own usage; a bundled one is documented by
	// bash(1), which is where the reader is sent.
	static void printCommandHelp(Executor& exec, const std::string& name) {
		const PluginCommand* plugin = exec.pluginCommand(name);
		if (plugin == nullptr) {
			std::printf("%s: %s — see bash(1) for full semantics\n", name.c_str(), name.c_str());
			return;
		}

		std::printf("%s: %s\n", name.c_str(),
			plugin->summary.empty() ? "a command added by a util" : plugin->summary.c_str());
		if (!plugin->usage.empty()) std::printf("usage: %s\n", plugin->usage.c_str());
	}

	static int builtin_help(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) {
			printBuiltinIndex(exec);
			return 0;
		}

		int status = 0;
		for (const std::string& name : args) {
			if (!exec.isBuiltin(name)) {
				shellErrorf("help: no help topics match '%s'\n", name.c_str());
				status = 1;
				continue;
			}

			printCommandHelp(exec, name);
		}

		return status;
	}

	void registerCoreBuiltins(Executor& exec) {
		exec.registerBuiltin(":",        builtin_colon);
		exec.registerBuiltin("true",     builtin_true);
		exec.registerBuiltin("false",    builtin_false);
		exec.registerBuiltin("echo",     builtin_echo);
		exec.registerBuiltin("printf",   builtin_printf);
		exec.registerBuiltin("exec",     builtin_exec);
		exec.registerBuiltin("pwd",      builtin_pwd);
		exec.registerBuiltin("cd",       builtin_cd);
		exec.registerBuiltin("exit",     builtin_exit);
		exec.registerBuiltin("suspend",  builtin_suspend);
		exec.registerBuiltin("logout",   builtin_logout);
		exec.registerBuiltin("return",   builtin_return);
		exec.registerBuiltin("break",    builtin_break);
		exec.registerBuiltin("continue", builtin_continue);
		exec.registerBuiltin("shift",    builtin_shift);
		exec.registerBuiltin("set",      builtin_set);
		exec.registerBuiltin("eval",     builtin_eval);
		exec.registerBuiltin("source",   builtin_source);
		exec.registerBuiltin(".",        builtin_dot);
		exec.registerBuiltin("type",     builtin_type);
		exec.registerBuiltin("command",  builtin_command);
		exec.registerBuiltin("builtin",  builtin_builtin);
		exec.registerBuiltin("alias",    builtin_alias);
		exec.registerBuiltin("unalias",  builtin_unalias);
		exec.registerBuiltin("history",  builtin_history);
		exec.registerBuiltin("__revsearch", builtin_revsearch);
		exec.registerBuiltin("__predict",   builtin_predict);
		exec.registerBuiltin("__lineedit",  lineEditBuiltin);
		exec.registerBuiltin("bind",        bindBuiltin);
		exec.registerBuiltin("__histstat",  builtin_histstat);
		exec.registerBuiltin("__ctrlc",     builtin_ctrlc);
		exec.registerBuiltin("__mksock",    builtin_mksock);
		exec.registerBuiltin("getopts",  builtin_getopts);
		exec.registerBuiltin("mapfile",  builtin_mapfile);
		exec.registerBuiltin("readarray",builtin_mapfile);
		exec.registerBuiltin("shopt",    builtin_shopt);
		exec.registerBuiltin("let",      builtin_let);
		exec.registerBuiltin("umask",    builtin_umask);
		exec.registerBuiltin("hash",     builtin_hash);
		exec.registerBuiltin("times",    builtin_times);
		exec.registerBuiltin("caller",   builtin_caller);
		exec.registerBuiltin("help",     builtin_help);
		registerCompletionBuiltins(exec);
		exec.registerBuiltin("jobs",     builtin_jobs);
		exec.registerBuiltin("wait",     builtin_wait);
		exec.registerBuiltin("fg",       builtin_fg);
		exec.registerBuiltin("bg",       builtin_bg);
		exec.registerBuiltin("disown",   builtin_disown);
		registerDeclarationBuiltins(exec);
		registerReadBuiltin(exec);
		registerUlimitBuiltin(exec);
		registerEnableBuiltin(exec);
		registerTrapBuiltin(exec);
		registerFcBuiltin(exec);
		registerTestBuiltins(exec);
	}

}  // namespace wbsh

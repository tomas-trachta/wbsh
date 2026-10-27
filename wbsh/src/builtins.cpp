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
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "executor.h"
#include "exitstatus.h"
#include "lexer.h"
#include "lineedit.h"
#include "numparse.h"
#include "parser.h"

namespace wbsh {

	static const char* const kDefaultUmask    = "0022";
	static const char* const kHistoryFileName = "/.wbsh_history";
	static const std::size_t kHelpColumns     = 5;

	static const char* const kExecutableSuffixes[] = { ".exe", ".cmd", ".bat", ".com" };

	static const char* const kShellKeywords[] = {
		"if", "then", "else", "elif", "fi", "case", "esac", "for",
		"while", "until", "do", "done", "function", "in", "select",
		"time", "[[", "]]", "return", "break", "continue",
	};

	static long long toIntSafe(const std::string& text, bool& ok) {
		ok = false;
		if (text.empty()) return 0;

		long long value = 0;
		std::size_t parsed = 0;
		if (!parseLL(text, value, 10, &parsed) || parsed != text.size()) return 0;

		ok = true;
		return value;
	}

	static int firstArgAsInt(const std::vector<std::string>& args, int fallback,
			bool require_positive = false) {
		if (args.empty()) return fallback;

		bool ok = false;
		const long long value = toIntSafe(args[0], ok);
		if (!ok) return fallback;
		if (require_positive && value <= 0) return fallback;

		return static_cast<int>(value);
	}

	static void printerr(const std::string& message) {
		std::fprintf(stderr, "wbsh: %s\n", message.c_str());
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

	static void appendEchoEscape(const std::string& text, std::size_t& i, std::string& out) {
		const char next = text[++i];
		switch (next) {
		case 'a':  out.push_back('\a'); break;
		case 'b':  out.push_back('\b'); break;
		case 'e':  out.push_back('\x1b'); break;
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
	static bool interpretEcho(const std::string& text, std::string& out) {
		for (std::size_t i = 0; i < text.size(); ++i) {
			const char c = text[i];
			if (isStopEscape(text, i)) return false;

			if (c == '\\' && i + 1 < text.size()) {
				appendEchoEscape(text, i, out);
				continue;
			}

			out.push_back(c);
		}

		return true;
	}

	static bool applyEchoFlags(const std::string& flag, bool& newline, bool& interpret) {
		if (flag.size() < 2) return false;

		for (std::size_t k = 1; k < flag.size(); ++k) {
			switch (flag[k]) {
			case 'n': newline = false;  break;
			case 'e': interpret = true;  break;
			case 'E': interpret = false; break;
			default:  return false;
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
			bool newline, bool interpret) {
		std::string out;
		for (std::size_t i = first; i < args.size(); ++i) {
			if (i > first) out.push_back(' ');
			if (!interpret) out += args[i];
			else if (!interpretEcho(args[i], out)) return out;
		}

		if (newline) out.push_back('\n');
		return out;
	}

	static int builtin_echo(Executor&, const std::vector<std::string>& args) {
		bool newline = true;
		bool interpret = false;
		const std::size_t first = parseEchoFlags(args, newline, interpret);

		writeStdout(echoText(args, first, newline, interpret));
		std::fflush(stdout);
		return 0;
	}

	namespace printf_detail {

		struct PrintfRun {
			std::vector<std::string> args;
			std::size_t next_arg = 0;
			std::string out;
			bool bad_number = false;
			bool stopped = false;
		};

	}  // namespace printf_detail

	using printf_detail::PrintfRun;

	static void appendFormatEscape(const std::string& fmt, std::size_t& i, std::string& out) {
		const char next = fmt[i + 1];
		if (next >= '0' && next <= '7') {
			appendOctalEscape(fmt, i, out);
			return;
		}

		if (next == '"') {
			out.push_back('"');
			++i;
			return;
		}

		appendEchoEscape(fmt, i, out);
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
		std::fprintf(stderr, "wbsh: printf: %s: invalid number\n", text.c_str());
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

	static void skipLengthModifiers(const std::string& fmt, std::size_t& i) {
		while (i < fmt.size() && std::strchr("hlLjzt", fmt[i]) != nullptr) ++i;
	}

	static bool parsePrintfConversionSpec(const std::string& fmt, std::size_t& i, PrintfRun& run,
			std::string& out_spec) {
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

		skipLengthModifiers(fmt, i);
		if (i >= fmt.size()) return false;

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

	static std::string formatEscapedArg(const std::string& spec, PrintfRun& run) {
		std::string expanded;
		if (!interpretEcho(takePrintfArg(run), expanded)) run.stopped = true;
		return formatString(spec, expanded);
	}

	static std::string formatUnsigned(const std::string& spec, char conversion, PrintfRun& run) {
		const long long value = printfIntegerArg(takePrintfArg(run), run);
		return formatValue(specWithConversion(spec, std::string("ll") + conversion),
			static_cast<unsigned long long>(value));
	}

	static std::string formatCharArg(const std::string& spec, PrintfRun& run) {
		const std::string text = takePrintfArg(run);
		return formatValue(spec, text.empty() ? 0 : static_cast<int>(text[0]));
	}

	static std::string formatPrintfConversion(const std::string& spec, char conversion,
			PrintfRun& run) {
		switch (conversion) {
		case 's':
			return formatString(spec, takePrintfArg(run));
		case 'b':
			return formatEscapedArg(spec, run);
		case 'q':
			return formatString(spec, shellQuoteBackslash(takePrintfArg(run)));
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
			return formatCharArg(spec, run);
		case '%':
			return "%";
		default:
			return spec;
		}
	}

	static void appendFormatOnce(const std::string& fmt, PrintfRun& run) {
		for (std::size_t i = 0; i < fmt.size() && !run.stopped; ++i) {
			const char c = fmt[i];
			if (c == '\\' && i + 1 < fmt.size()) {
				appendFormatEscape(fmt, i, run.out);
				continue;
			}

			if (c != '%') {
				run.out.push_back(c);
				continue;
			}

			const std::size_t spec_start = i;
			std::string spec;
			if (!parsePrintfConversionSpec(fmt, i, run, spec)) {
				run.out.append(fmt, spec_start, std::string::npos);
				return;
			}

			run.out += formatPrintfConversion(spec, fmt[i], run);
		}
	}

	// Bash printf semantics: emit the format once, then keep cycling
	// while there are unconsumed args. If a pass consumes nothing
	// (a literal-only format), stop to avoid an infinite loop.
	static void appendFormatCycles(const std::string& fmt, PrintfRun& run) {
		appendFormatOnce(fmt, run);
		while (!run.stopped && run.next_arg < run.args.size()) {
			const std::size_t before = run.next_arg;
			appendFormatOnce(fmt, run);
			if (run.next_arg == before) break;
		}
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
			printerr("printf: usage: printf [-v var] format [arguments]");
			return 2;
		}

		PrintfRun run;
		run.args.assign(args.begin() + static_cast<std::ptrdiff_t>(first) + 1, args.end());
		appendFormatCycles(args[first], run);

		const int store_status = storePrintfResult(exec, target, run.out);
		if (store_status != 0) return store_status;
		return run.bad_number ? 1 : 0;
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
		std::filesystem::path here = std::filesystem::current_path(ec);
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
					std::fprintf(stderr, "wbsh: cd: -%c: invalid option\n", arg[k]);
					std::fprintf(stderr, "cd: usage: cd [-L|[-P [-e]] [-@]] [dir]\n");
					return false;
				}
			}
		}

		return true;
	}

	static int builtin_cd(Executor& exec, const std::vector<std::string>& args) {
		bool physical = exec.env().options().physical;
		std::size_t first = 0;
		if (!parseCdOptions(args, first, physical)) return 2;

		std::string target;
		if (!resolveCdTarget(exec, args, first, target)) return 1;

		std::string error;
		if (!changeDirectory(exec, target, error, physical)) {
			std::fprintf(stderr, "wbsh: cd: %s: %s\n", target.c_str(), error.c_str());
			return 1;
		}

		return 0;
	}

	static int builtin_exit(Executor& exec, const std::vector<std::string>& args) {
		const int status = shellStatusFromInt(firstArgAsInt(args, exec.defaultExitStatus()));
		exec.raiseExit(status);
		return status;
	}

	static int builtin_return(Executor& exec, const std::vector<std::string>& args) {
		if (exec.funcDepth() == 0 && exec.sourceDepth() == 0) {
			printerr("return: can only `return' from a function or sourced script");
			return 1;
		}

		const int status = shellStatusFromInt(firstArgAsInt(args, exec.lastStatus()));
		exec.raiseReturn(status);
		return status;
	}

	static int builtin_break(Executor& exec, const std::vector<std::string>& args) {
		if (exec.loopDepth() == 0) {
			printerr("break: only meaningful in a `for', `while', or `until' loop");
			return 0;
		}

		exec.raiseBreak(firstArgAsInt(args, 1, /*require_positive=*/true));
		return 0;
	}

	static int builtin_continue(Executor& exec, const std::vector<std::string>& args) {
		if (exec.loopDepth() == 0) {
			printerr("continue: only meaningful in a `for', `while', or `until' loop");
			return 0;
		}

		exec.raiseContinue(firstArgAsInt(args, 1, /*require_positive=*/true));
		return 0;
	}

	static int builtin_shift(Executor& exec, const std::vector<std::string>& args) {
		const int count = firstArgAsInt(args, 1);
		std::vector<std::string> positional = exec.env().positional();
		if (count < 0 || static_cast<std::size_t>(count) > positional.size()) return 1;

		positional.erase(positional.begin(), positional.begin() + count);
		exec.env().setPositional(std::move(positional));
		return 0;
	}

	static int dumpAllShellVars(Executor& exec) {
		for (const auto& entry : sortedEntries(exec.env().vars())) {
			std::printf("%s=%s\n", entry.first.c_str(), entry.second.c_str());
		}

		return 0;
	}

	namespace set_detail {

		struct SetOperands {
			std::size_t first = 0;
			bool        replace_positional = false;
		};

	}  // namespace set_detail

	using set_detail::SetOperands;

	static void applySetOption(Environment& env, const SetOptionSpec& spec, bool on) {
		SetOptions& options = env.options();
		options.*(spec.flag) = on;

		if (on && spec.flag == &SetOptions::vi)    options.emacs = false;
		if (on && spec.flag == &SetOptions::emacs) options.vi = false;
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
			std::fprintf(stderr, "wbsh: set: %s: invalid option name\n", name.c_str());
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

	static int reportInvalidSetFlag(char sign, char letter) {
		std::fprintf(stderr, "wbsh: set: %c%c: invalid option\n", sign, letter);
		std::fprintf(stderr,
			"set: usage: set [-abefhkmnptuvxBCHP] [-o option-name] [--] [arg ...]\n");
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
		if (args.empty()) return dumpAllShellVars(exec);

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

	// The command replaces the shell: the shell ends with it, and its EXIT
	// trap never runs.
	static int builtin_exec(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) return 0;

		exec.clearTrap("EXIT");
		const int status = runExecCommand(exec, args);
		if (!exec.flowPending()) exec.raiseExit(status);
		return status;
	}

	static int builtin_eval(Executor& exec, const std::vector<std::string>& args) {
		const std::string joined = joinWithSpaces(args.begin(), args.end());
		if (joined.empty()) return 0;

		return exec.executeInputText(joined, "eval");
	}

	static bool readScriptFile(const std::filesystem::path& path, std::string& out_body) {
		std::ifstream file(path, std::ios::binary);
		if (!file) return false;

		std::stringstream buffer;
		buffer << file.rdbuf();
		out_body = buffer.str();
		normalizeCrlf(out_body);
		return true;
	}

	static int builtin_source(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) {
			printerr("source: filename required");
			return 2;
		}

		std::string body;
		const std::filesystem::path path = utf8ToPath(exec.pathConv().toWin32(args[0]));
		if (!readScriptFile(path, body)) {
			std::fprintf(stderr, "wbsh: source: %s: %s\n", args[0].c_str(), std::strerror(errno));
			return 1;
		}

		std::vector<std::string> saved = exec.env().positional();
		if (args.size() > 1) exec.env().setPositional({ args.begin() + 1, args.end() });

		exec.beginSource();
		int status = exec.executeInputText(body, args[0]);
		exec.endSource();
		exec.consumeFlow(FlowSignal::Kind::Return, &status);
		exec.env().setPositional(std::move(saved));
		return status;
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

		std::vector<std::string> paths = exec.findAllExecutables(name);
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
		if (def != nullptr) std::printf("%s () \n%s\n", name.c_str(), def->body_text.c_str());
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
			if (!quiet) std::fprintf(stderr, "wbsh: %s: %s: not found\n", builtin_name, name.c_str());
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

				std::fprintf(stderr, "wbsh: type: -%c: invalid option\n", arg[k]);
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

	// `command` skips functions and aliases: a builtin runs in place,
	// anything else is looked up on PATH.
	static int builtin_command(Executor& exec, const std::vector<std::string>& args) {
		char mode = 0;
		const std::size_t first = parseCommandFlags(args, mode);
		if (first >= args.size()) return 0;

		if (mode != 0) return describeForCommand(exec, args, first, mode);

		const std::vector<std::string> rest(args.begin() + first, args.end());
		if (exec.isBuiltin(rest[0])) {
			const std::vector<std::string> sub(rest.begin() + 1, rest.end());
			return exec.callBuiltin(rest[0], sub);
		}

		return exec.runExternal(rest, {});
	}

	static int builtin_builtin(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) return 0;

		if (!exec.isBuiltin(args[0])) {
			std::fprintf(stderr, "wbsh: builtin: %s: not a shell builtin\n", args[0].c_str());
			return 1;
		}

		return exec.callBuiltin(args[0], { args.begin() + 1, args.end() });
	}

	static int evalUnaryFileTest(char op, const std::string& raw_path, const PathConv& pc) {
		const std::string path = pc.toWin32(raw_path);
		struct stat info {};
		const bool exists = statPath(path, info);
		switch (op) {
		case 'e': return exists ? 0 : 1;
		case 'f': return (exists && (info.st_mode & S_IFMT) == S_IFREG) ? 0 : 1;
		case 'd': return (exists && (info.st_mode & S_IFMT) == S_IFDIR) ? 0 : 1;
		case 's': return (exists && info.st_size > 0) ? 0 : 1;
		case 'r': return (exists && (info.st_mode & 0444) != 0) ? 0 : 1;
		case 'w': return (exists && (info.st_mode & 0222) != 0) ? 0 : 1;
		case 'x': return (exists && (info.st_mode & 0111) != 0) ? 0 : 1;
		default:  return 2;
		}
	}

	static int evalUnaryTest(const std::vector<std::string>& args, const PathConv& pc) {
		if (args[0].size() != 2 || args[0][0] != '-') return 2;

		const char op = args[0][1];
		if (op == 'z') return args[1].empty() ? 0 : 1;
		if (op == 'n') return args[1].empty() ? 1 : 0;
		return evalUnaryFileTest(op, args[1], pc);
	}

	static int evalNumericTest(const std::string& op, long long left, long long right) {
		if (op == "-eq") return left == right ? 0 : 1;
		if (op == "-ne") return left != right ? 0 : 1;
		if (op == "-lt") return left <  right ? 0 : 1;
		if (op == "-le") return left <= right ? 0 : 1;
		if (op == "-gt") return left >  right ? 0 : 1;
		if (op == "-ge") return left >= right ? 0 : 1;
		return 2;
	}

	static int evalBinaryTest(const std::vector<std::string>& args) {
		const std::string& left  = args[0];
		const std::string& op    = args[1];
		const std::string& right = args[2];
		if (op == "=" || op == "==") return left == right ? 0 : 1;
		if (op == "!=")              return left != right ? 0 : 1;
		if (op == "<")               return left <  right ? 0 : 1;
		if (op == ">")               return left >  right ? 0 : 1;

		bool left_ok = false;
		bool right_ok = false;
		const long long left_value  = toIntSafe(left, left_ok);
		const long long right_value = toIntSafe(right, right_ok);
		if (!left_ok || !right_ok) return 2;

		return evalNumericTest(op, left_value, right_value);
	}

	static int evalTest(const std::vector<std::string>& args, const PathConv& pc) {
		if (args.empty()) return 1;

		if (args[0] == "!" && args.size() > 1) {
			const std::vector<std::string> rest(args.begin() + 1, args.end());
			const int result = evalTest(rest, pc);
			if (result == 2) return 2;
			return result == 0 ? 1 : 0;
		}

		switch (args.size()) {
		case 1:  return args[0].empty() ? 1 : 0;
		case 2:  return evalUnaryTest(args, pc);
		case 3:  return evalBinaryTest(args);
		default: return 2;
		}
	}

	static int builtin_test(Executor& exec, const std::vector<std::string>& args) {
		return evalTest(args, exec.pathConv());
	}

	namespace shopt_detail {
		enum class Mode { ListAll, Set, Unset, Query };
	}  // namespace shopt_detail

	static void printShoptFlag(const Environment& env, const ShoptSpec& flag) {
		const bool on = env.shopts().*(flag.flag);
		std::printf("%-15s %s\n", flag.name, on ? "on" : "off");
	}

	static bool parseShoptArgs(const std::vector<std::string>& args, shopt_detail::Mode& mode,
			std::vector<std::string>& names) {
		using shopt_detail::Mode;
		for (const std::string& arg : args) {
			if (arg == "-s") { mode = Mode::Set;   continue; }
			if (arg == "-u") { mode = Mode::Unset; continue; }
			if (arg == "-q") { mode = Mode::Query; continue; }
			if (arg == "-p") continue;
			if (arg == "--") continue;

			if (!arg.empty() && arg[0] == '-') {
				std::fprintf(stderr, "wbsh: shopt: unknown option: %s\n", arg.c_str());
				return false;
			}

			names.push_back(arg);
		}

		return true;
	}

	static int shoptSetOrUnset(Executor& exec, const std::vector<std::string>& names, bool on) {
		int status = 0;
		for (const std::string& name : names) {
			const ShoptSpec* flag = findShopt(name);
			if (flag == nullptr) {
				std::fprintf(stderr, "wbsh: shopt: %s: invalid option name\n", name.c_str());
				status = 1;
				continue;
			}

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

	static int shoptListMode(Executor& exec, const std::vector<std::string>& names) {
		if (names.empty()) {
			for (const ShoptSpec& flag : shoptTable()) {
				printShoptFlag(exec.env(), flag);
			}

			return 0;
		}

		int status = 0;
		for (const std::string& name : names) {
			const ShoptSpec* flag = findShopt(name);
			if (flag == nullptr) {
				std::fprintf(stderr, "wbsh: shopt: %s: invalid option name\n", name.c_str());
				status = 1;
				continue;
			}

			printShoptFlag(exec.env(), *flag);
		}

		return status;
	}

	static int builtin_shopt(Executor& exec, const std::vector<std::string>& args) {
		using shopt_detail::Mode;
		Mode mode = Mode::ListAll;
		std::vector<std::string> names;
		if (!parseShoptArgs(args, mode, names)) return 1;

		switch (mode) {
		case Mode::Set:     return shoptSetOrUnset(exec, names, true);
		case Mode::Unset:   return shoptSetOrUnset(exec, names, false);
		case Mode::Query:   return shoptQuery(exec, names);
		case Mode::ListAll: return shoptListMode(exec, names);
		}

		return 0;
	}

	namespace mapfile_detail {
		struct MapfileOptions {
			bool strip_newline = false;
			long long max_lines = -1;
			long long origin = 0;
			long long skip = 0;
			std::string array_name = "MAPFILE";
		};
	}  // namespace mapfile_detail

	static bool parseMapfileArgs(const std::vector<std::string>& args,
			mapfile_detail::MapfileOptions& options) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			const bool has_value = i + 1 < args.size();
			if (arg == "-t") {
				options.strip_newline = true;
			} else if (arg == "-n" && has_value) {
				parseLL(args[++i], options.max_lines);
			} else if (arg == "-O" && has_value) {
				parseLL(args[++i], options.origin);
			} else if (arg == "-s" && has_value) {
				parseLL(args[++i], options.skip);
			} else if (arg == "-u" && has_value) {
				++i;
			} else if (!arg.empty() && arg[0] == '-' && arg != "-" && arg != "--") {
				std::fprintf(stderr, "wbsh: mapfile: unknown option: %s\n", arg.c_str());
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

	// A last line with no newline still counts, unless it falls inside
	// the skipped range.
	static std::vector<std::string> readMapfileLines(
			const mapfile_detail::MapfileOptions& options) {
		std::vector<std::string> lines;
		std::string current;
		long long skipped = 0;
		for (int ch = std::fgetc(stdin); ch != EOF; ch = std::fgetc(stdin)) {
			if (ch != '\n') {
				current.push_back(static_cast<char>(ch));
				continue;
			}

			if (skipped < options.skip) {
				++skipped;
				current.clear();
				continue;
			}

			if (!options.strip_newline) current.push_back('\n');
			lines.push_back(std::move(current));
			current.clear();
			if (!mapfileHasRoom(options, lines.size())) break;
		}

		if (!current.empty() && skipped >= options.skip && mapfileHasRoom(options, lines.size())) {
			lines.push_back(std::move(current));
		}

		return lines;
	}

	static int builtin_mapfile(Executor& exec, const std::vector<std::string>& args) {
		mapfile_detail::MapfileOptions options;
		if (!parseMapfileArgs(args, options)) return 1;

		std::vector<std::string> lines = readMapfileLines(options);
		std::map<long long, std::string> elements;
		for (std::size_t i = 0; i < lines.size(); ++i) {
			elements[options.origin + static_cast<long long>(i)] = std::move(lines[i]);
		}

		exec.env().setIndexedArraySparse(options.array_name, std::move(elements));
		return 0;
	}

	static std::string canonicalSignalName(std::string name) {
		const bool prefixed = name.size() > 3
			&& (name.compare(0, 3, "SIG") == 0 || name.compare(0, 3, "sig") == 0);
		if (prefixed) name = name.substr(3);

		for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		if (name == "0") name = "EXIT";
		return name;
	}

	static int builtin_trap(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty() || (args.size() == 1 && args[0] == "-p")) {
			for (const auto& entry : exec.listedTraps()) {
				std::printf("trap -- '%s' %s\n", entry.second.c_str(), entry.first.c_str());
			}

			return 0;
		}

		if (args[0] == "-l") {
			std::printf("EXIT INT TERM HUP QUIT\n");
			return 0;
		}

		if (args.size() < 2) {
			printerr("trap: usage: trap [-lp] [[ARG] SIG ...]");
			return 2;
		}

		const std::string& action = args[0];
		for (std::size_t i = 1; i < args.size(); ++i) {
			const std::string signal = canonicalSignalName(args[i]);
			if (action == "-") exec.clearTrap(signal);
			else exec.setTrap(signal, action);
		}

		return 0;
	}

	namespace getopts_detail {
		struct GetoptsCtx {
			Executor& exec;
			std::string opts;
			const std::string& name;
			std::vector<std::string> source;
			bool silent;
			int& sub;
			int optind;

			void setName(const std::string& value) const { exec.env().set(name, value); }
			void storeOptind() const { exec.env().set("OPTIND", std::to_string(optind)); }
		};

		static int loadOptind(Executor& exec) {
			int value = 1;
			const std::string text = exec.env().get("OPTIND");
			if (!text.empty() && !parseInt(text, value)) value = 1;
			return value < 1 ? 1 : value;
		}

		static int finishWithoutOption(GetoptsCtx& ctx) {
			ctx.setName("?");
			ctx.exec.env().unset("OPTARG");
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

		static int handleIllegalOption(GetoptsCtx& ctx, const std::string& current, char opt) {
			if (ctx.silent) {
				ctx.setName("?");
				ctx.exec.env().set("OPTARG", std::string(1, opt));
			} else {
				std::fprintf(stderr, "getopts: illegal option -- %c\n", opt);
				ctx.setName("?");
				ctx.exec.env().unset("OPTARG");
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
				std::fprintf(stderr, "getopts: option requires an argument -- %c\n", opt);
				ctx.setName("?");
				ctx.exec.env().unset("OPTARG");
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
			ctx.exec.env().unset("OPTARG");
			advanceOneOptionChar(ctx, current);
			ctx.storeOptind();
			return 0;
		}
	}  // namespace getopts_detail

	static int builtin_getopts(Executor& exec, const std::vector<std::string>& args) {
		if (args.size() < 2) {
			printerr("getopts: usage: getopts OPTSTRING NAME [ARG ...]");
			return 2;
		}

		std::string opts = args[0];
		const bool silent = !opts.empty() && opts[0] == ':';
		if (silent) opts.erase(0, 1);

		std::vector<std::string> source = args.size() > 2
			? std::vector<std::string>(args.begin() + 2, args.end())
			: exec.env().positional();

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

	static int builtin_jobs(Executor& exec, const std::vector<std::string>&) {
		exec.reapJobs();
		for (const Executor::Job& job : exec.jobsTable()) {
			std::printf("[%d]  %s   %s\n", job.id, job.running ? "Running" : "Done",
				job.cmd_text.empty() ? "<command>" : job.cmd_text.c_str());
		}

		return 0;
	}

	static bool resolveJobSpec(Executor& exec, const std::string& spec, int& out_id) {
		out_id = -1;
		if (!spec.empty() && spec[0] == '%') return parseInt(spec.substr(1), out_id);

		long long pid = 0;
		if (!parseLL(spec, pid)) return false;

		for (const Executor::Job& job : exec.jobsTable()) {
			if (job.pid != pid) continue;

			out_id = job.id;
			return true;
		}

		return true;
	}

	static int builtin_wait(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) {
			exec.waitForAllJobs();
			return 0;
		}

		int status = 0;
		for (const std::string& spec : args) {
			int id = -1;
			if (!resolveJobSpec(exec, spec, id) || id < 0) {
				status = 1;
				continue;
			}

			const int job_status = exec.waitForJob(id);
			if (job_status >= 0) status = job_status;
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
			if (!spec.empty() && spec[0] == '%' && !parseInt(spec.substr(1), id)) continue;
			removeJob(exec, id);
		}

		return 0;
	}

	static int newestRunningJobId(Executor& exec) {
		const std::vector<Executor::Job>& jobs = exec.jobsTable();
		for (auto it = jobs.rbegin(); it != jobs.rend(); ++it) {
			if (it->running) return it->id;
		}

		return -1;
	}

	static int jobIdFromFgArg(const std::string& spec) {
		int id = -1;
		if (!spec.empty() && spec[0] == '%' && !parseInt(spec.substr(1), id)) id = -1;
		return id;
	}

	static int builtin_fg(Executor& exec, const std::vector<std::string>& args) {
		exec.reapJobs();
		const int id = args.empty() ? newestRunningJobId(exec) : jobIdFromFgArg(args[0]);
		if (id < 0) {
			printerr("fg: no current job");
			return 1;
		}

		const int status = exec.waitForJob(id);
		return status < 0 ? 1 : status;
	}

	static int builtin_bg(Executor& exec, const std::vector<std::string>&) {
		return builtin_jobs(exec, {});
	}

	static void printAllAliases(Executor& exec) {
		for (const auto& entry : sortedEntries(exec.aliases())) {
			std::printf("alias %s='%s'\n", entry.first.c_str(), entry.second.c_str());
		}
	}

	static bool printAlias(Executor& exec, const std::string& name) {
		if (!exec.isAlias(name)) {
			std::fprintf(stderr, "wbsh: alias: %s: not found\n", name.c_str());
			return false;
		}

		std::printf("alias %s='%s'\n", name.c_str(), exec.aliasValue(name).c_str());
		return true;
	}

	static int builtin_alias(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) {
			printAllAliases(exec);
			return 0;
		}

		int status = 0;
		for (const std::string& arg : args) {
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

	static int builtin_unalias(Executor& exec, const std::vector<std::string>& args) {
		bool all = false;
		std::vector<std::string> names;
		for (const std::string& arg : args) {
			if (arg == "-a") all = true;
			else if (!arg.empty() && arg[0] == '-') continue;
			else names.push_back(arg);
		}

		if (all) {
			for (const std::string& name : keysOf(exec.aliases())) exec.unsetAlias(name);
			return 0;
		}

		int status = 0;
		for (const std::string& name : names) {
			if (!exec.isAlias(name)) {
				std::fprintf(stderr, "wbsh: unalias: %s: not found\n", name.c_str());
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
		if (path.empty() || !exec.saveHistoryToFile(path)) {
			printerr("history: cannot write history file");
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

	static int printHistory(Executor& exec, const std::vector<std::string>& args) {
		const std::vector<std::string>& history = exec.history();
		const std::size_t start = historyStartIndex(args, history.size());
		for (std::size_t i = start; i < history.size(); ++i) {
			std::printf("%5zu  %s\n", i + 1, history[i].c_str());
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
	static int builtin_histstat(Executor& exec, const std::vector<std::string>& args) {
		if (args.size() != 2) {
			printerr("__histstat: usage: __histstat INDEX STATUS");
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

	static bool endsWithNoCase(const std::string& name, const std::string& suffix) {
		if (name.size() < suffix.size()) return false;

		std::string tail = name.substr(name.size() - suffix.size());
		for (char& c : tail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return tail == suffix;
	}

	static std::string commandBaseName(const std::string& file_name) {
		for (const char* suffix : kExecutableSuffixes) {
			if (endsWithNoCase(file_name, suffix)) {
				return file_name.substr(0, file_name.size() - std::strlen(suffix));
			}
		}

		return file_name;
	}

	static void collectCommandsFromPath(Executor& exec, std::vector<std::string>& out) {
		const std::string path = exec.env().get("PATH");
		if (path.empty()) return;

		std::set<std::string> seen;
		for (const std::string& dir : splitPathList(path)) {
			if (dir.empty()) continue;

			std::error_code ec;
			const std::filesystem::path native = utf8ToPath(exec.pathConv().toWin32(dir));
			std::filesystem::directory_iterator it(native, ec);
			if (ec) continue;

			for (const auto& entry : it) {
				const std::string base = commandBaseName(pathToUtf8(entry.path().filename()));
				if (seen.insert(base).second) out.push_back(base);
			}
		}
	}

	static std::vector<std::string> filterByPrefix(const std::vector<std::string>& values,
			const std::string& prefix) {
		std::vector<std::string> out;
		for (const std::string& value : values) {
			if (value.compare(0, prefix.size(), prefix) == 0) out.push_back(value);
		}

		return out;
	}

	namespace compgen_detail {
		struct CompgenFlags {
			std::string action;
			std::string prefix;
			std::vector<std::string> wordlist;
			bool include_files    = false;
			bool include_dirs     = false;
			bool include_cmds     = false;
			bool include_builtins = false;
			bool include_funcs    = false;
			bool include_aliases  = false;
			bool include_vars     = false;
			bool include_keywords = false;
		};
	}  // namespace compgen_detail

	static std::vector<std::string> splitDashWWordList(const std::string& text) {
		std::vector<std::string> out;
		std::string current;
		for (const char c : text) {
			if (c != ' ' && c != '\t' && c != '\n') {
				current.push_back(c);
				continue;
			}

			if (!current.empty()) out.push_back(std::move(current));
			current.clear();
		}

		if (!current.empty()) out.push_back(std::move(current));
		return out;
	}

	static void applyCompgenAction(compgen_detail::CompgenFlags& flags) {
		if (flags.action == "function")  flags.include_funcs    = true;
		if (flags.action == "variable")  flags.include_vars     = true;
		if (flags.action == "alias")     flags.include_aliases  = true;
		if (flags.action == "builtin")   flags.include_builtins = true;
		if (flags.action == "command")   flags.include_cmds     = true;
		if (flags.action == "file")      flags.include_files    = true;
		if (flags.action == "directory") flags.include_dirs     = true;
	}

	static void parseCompgenFlags(const std::vector<std::string>& args,
			compgen_detail::CompgenFlags& flags) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			const bool has_value = i + 1 < args.size();
			if (arg == "-W" && has_value) {
				flags.wordlist = splitDashWWordList(args[++i]);
				continue;
			}

			if (arg == "-A" && has_value) { flags.action = args[++i]; continue; }
			if (arg == "-f") { flags.include_files    = true; continue; }
			if (arg == "-d") { flags.include_dirs     = true; continue; }
			if (arg == "-c") { flags.include_cmds     = true; continue; }
			if (arg == "-b") { flags.include_builtins = true; continue; }
			if (arg == "-a") { flags.include_aliases  = true; continue; }
			if (arg == "-v") { flags.include_vars     = true; continue; }
			if (arg == "-k") { flags.include_keywords = true; continue; }
			if (arg == "-u" || arg == "-g" || arg == "-s" || arg == "-e") continue;
			if (arg == "-o" && has_value) { ++i; continue; }
			if (arg == "-F" && has_value) { ++i; continue; }
			if (arg == "-C" && has_value) { ++i; continue; }
			if (!arg.empty() && arg[0] == '-' && arg != "-" && arg != "--") continue;
			if (arg == "--") continue;
			flags.prefix = arg;
		}

		applyCompgenAction(flags);
	}

	static void appendSortedFiltered(std::vector<std::string>& out, std::vector<std::string> source,
			const std::string& prefix) {
		std::sort(source.begin(), source.end());
		for (std::string& name : filterByPrefix(source, prefix)) out.push_back(std::move(name));
	}

	static void appendCommandCandidates(std::vector<std::string>& out, Executor& exec,
			const std::string& prefix) {
		std::vector<std::string> names = exec.builtinNames();
		const std::vector<std::string> functions = exec.functionNames();
		names.insert(names.end(), functions.begin(), functions.end());
		collectCommandsFromPath(exec, names);

		std::sort(names.begin(), names.end());
		names.erase(std::unique(names.begin(), names.end()), names.end());
		for (std::string& name : filterByPrefix(names, prefix)) out.push_back(std::move(name));
	}

	static void appendKeywordCandidates(std::vector<std::string>& out, const std::string& prefix) {
		std::vector<std::string> keywords;
		for (const char* keyword : kShellKeywords) keywords.push_back(keyword);
		for (std::string& name : filterByPrefix(keywords, prefix)) out.push_back(std::move(name));
	}

	static void splitCompletionPrefix(const std::string& prefix, std::string& out_dir,
			std::string& out_leaf) {
		out_dir = ".";
		out_leaf = prefix;

		const std::size_t slash = prefix.find_last_of('/');
		if (slash == std::string::npos) return;

		out_dir  = prefix.substr(0, slash);
		out_leaf = prefix.substr(slash + 1);
		if (out_dir.empty()) out_dir = "/";
	}

	static std::string joinCompletionPath(const std::string& dir, const std::string& name) {
		if (dir == ".") return name;
		if (dir == "/") return "/" + name;
		return dir + "/" + name;
	}

	static void appendFileDirCandidates(std::vector<std::string>& out, Executor& exec,
			const std::string& prefix, bool include_files, bool include_dirs) {
		std::string dir;
		std::string leaf;
		splitCompletionPrefix(prefix, dir, leaf);

		std::error_code ec;
		const std::filesystem::path list_dir = utf8ToPath(exec.pathConv().toWin32(dir));
		std::filesystem::directory_iterator it(list_dir, ec);
		if (ec) return;

		for (const auto& entry : it) {
			const std::string name = pathToUtf8(entry.path().filename());
			if (name.empty() || name[0] == '.') continue;
			if (name.compare(0, leaf.size(), leaf) != 0) continue;

			const bool is_dir = entry.is_directory(ec);
			if (include_dirs && !is_dir && !include_files) continue;

			std::string full = joinCompletionPath(dir, name);
			if (is_dir) full.push_back('/');
			out.push_back(std::move(full));
		}
	}

	static std::vector<std::string> generateCompletions(Executor& exec,
			const std::vector<std::string>& args, std::string& out_prefix) {
		compgen_detail::CompgenFlags flags;
		parseCompgenFlags(args, flags);

		std::vector<std::string> out;
		for (std::string& word : filterByPrefix(flags.wordlist, flags.prefix)) {
			out.push_back(std::move(word));
		}

		if (flags.include_funcs)    appendSortedFiltered(out, exec.functionNames(), flags.prefix);
		if (flags.include_builtins) appendSortedFiltered(out, exec.builtinNames(), flags.prefix);
		if (flags.include_aliases)  appendSortedFiltered(out, keysOf(exec.aliases()), flags.prefix);
		if (flags.include_vars) {
			appendSortedFiltered(out, keysOf(exec.env().vars()), flags.prefix);
		}

		if (flags.include_cmds)     appendCommandCandidates(out, exec, flags.prefix);
		if (flags.include_keywords) appendKeywordCandidates(out, flags.prefix);
		if (flags.include_files || flags.include_dirs) {
			appendFileDirCandidates(out, exec, flags.prefix, flags.include_files,
				flags.include_dirs);
		}

		out_prefix = flags.prefix;
		return out;
	}

	static int builtin_compgen(Executor& exec, const std::vector<std::string>& args) {
		std::string prefix;
		const std::vector<std::string> candidates = generateCompletions(exec, args, prefix);
		for (const std::string& candidate : candidates) std::printf("%s\n", candidate.c_str());
		return candidates.empty() ? 1 : 0;
	}

	namespace complete_detail {
		struct CompleteOptions {
			Executor::CompletionSpec spec;
			std::vector<std::string> commands;
			bool remove_mode = false;
			bool print_mode = false;
			// -D is parsed but not yet acted on.
			bool default_complete = false;
		};
	}  // namespace complete_detail

	static void applyCompleteBehaviour(const std::string& option, Executor::CompletionSpec& spec) {
		if (option == "default")  spec.default_fallback = true;
		if (option == "plusdirs") spec.plusdirs = true;
		if (option == "nospace")  spec.nospace = true;
	}

	static void parseCompleteArgs(const std::vector<std::string>& args,
			complete_detail::CompleteOptions& options) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			const bool has_value = i + 1 < args.size();
			if (arg == "-r") { options.remove_mode      = true; continue; }
			if (arg == "-p") { options.print_mode       = true; continue; }
			if (arg == "-D") { options.default_complete = true; continue; }
			if (arg == "-W" && has_value) {
				options.spec.words = splitDashWWordList(args[++i]);
				continue;
			}

			if (arg == "-o" && has_value) {
				applyCompleteBehaviour(args[++i], options.spec);
				continue;
			}

			if (arg == "-F" && has_value) { options.spec.function = args[++i]; continue; }
			if (arg == "-C" && has_value) { options.spec.command  = args[++i]; continue; }
			if (arg == "-f") { options.spec.include_files = true; continue; }
			if (arg == "-d") { options.spec.include_dirs  = true; continue; }
			if (!arg.empty() && arg[0] == '-' && arg != "-" && arg != "--") continue;
			if (arg == "--") continue;
			options.commands.push_back(arg);
		}
	}

	static int printCompleteSpecs(Executor& exec, const std::vector<std::string>& commands) {
		const auto& specs = exec.completionSpecs();
		if (commands.empty()) {
			for (const auto& entry : specs) std::printf("complete %s\n", entry.first.c_str());
			return 0;
		}

		int status = 0;
		for (const std::string& command : commands) {
			if (specs.count(command) == 0) {
				std::fprintf(stderr, "wbsh: complete: %s: no completion specification\n",
					command.c_str());
				status = 1;
				continue;
			}

			std::printf("complete %s\n", command.c_str());
		}

		return status;
	}

	static int clearCompleteSpecs(Executor& exec, const std::vector<std::string>& commands) {
		if (commands.empty()) {
			for (const std::string& name : keysOf(exec.completionSpecs())) {
				exec.removeCompletionSpec(name);
			}

			return 0;
		}

		for (const std::string& command : commands) exec.removeCompletionSpec(command);
		return 0;
	}

	static int builtin_complete(Executor& exec, const std::vector<std::string>& args) {
		complete_detail::CompleteOptions options;
		parseCompleteArgs(args, options);

		if (options.print_mode)  return printCompleteSpecs(exec, options.commands);
		if (options.remove_mode) return clearCompleteSpecs(exec, options.commands);
		if (options.commands.empty()) return 0;

		for (const std::string& command : options.commands) {
			exec.setCompletionSpec(command, options.spec);
		}

		return 0;
	}

	static int builtin_compopt(Executor&, const std::vector<std::string>&) {
		return 0;
	}

	static int builtin_let(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) return 1;

		Expander& expander = exec.expander();
		long long last = 0;
		for (const std::string& expression : args) {
			last = expander.evalArith(expression);
			if (expander.failed()) {
				const char* label = expander.fatalErrorPending() ? "" : "let: ";
				std::fprintf(stderr, "wbsh: %s%s\n", label, expander.takeError().c_str());
				return 1;
			}
		}

		return last != 0 ? 0 : 1;
	}

	static std::string umaskTriplet(int mask, int shift) {
		std::string bits;
		bits.push_back((mask & (0400 >> shift)) != 0 ? '-' : 'r');
		bits.push_back((mask & (0200 >> shift)) != 0 ? '-' : 'w');
		bits.push_back((mask & (0100 >> shift)) != 0 ? '-' : 'x');
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

	// Nothing is hashed, so every flag but -r is accepted and ignored.
	static int builtin_hash(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) {
			std::printf("hash: no commands hashed\n");
			return 0;
		}

		for (const std::string& arg : args) {
			if (arg == "-r") exec.clearExecutablePathCache();
		}

		return 0;
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

	static int builtin_caller(Executor& exec, const std::vector<std::string>&) {
		if (exec.funcDepth() == 0) return 1;

		std::printf("%d %s\n", exec.env().currentLineno(), exec.env().shellName().c_str());
		return 0;
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
				std::fprintf(stderr, "wbsh: help: no help topics match '%s'\n", name.c_str());
				status = 1;
				continue;
			}

			printCommandHelp(exec, name);
		}

		return status;
	}

	static int builtin_lbracket(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty() || args.back() != "]") {
			printerr("[: missing closing `]'");
			return 2;
		}

		const std::vector<std::string> body(args.begin(), args.end() - 1);
		return evalTest(body, exec.pathConv());
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
		exec.registerBuiltin("return",   builtin_return);
		exec.registerBuiltin("break",    builtin_break);
		exec.registerBuiltin("continue", builtin_continue);
		exec.registerBuiltin("shift",    builtin_shift);
		exec.registerBuiltin("set",      builtin_set);
		exec.registerBuiltin("eval",     builtin_eval);
		exec.registerBuiltin("source",   builtin_source);
		exec.registerBuiltin(".",        builtin_source);
		exec.registerBuiltin("type",     builtin_type);
		exec.registerBuiltin("command",  builtin_command);
		exec.registerBuiltin("builtin",  builtin_builtin);
		exec.registerBuiltin("test",     builtin_test);
		exec.registerBuiltin("[",        builtin_lbracket);
		exec.registerBuiltin("alias",    builtin_alias);
		exec.registerBuiltin("unalias",  builtin_unalias);
		exec.registerBuiltin("history",  builtin_history);
		exec.registerBuiltin("__revsearch", builtin_revsearch);
		exec.registerBuiltin("__predict",   builtin_predict);
		exec.registerBuiltin("__lineedit",  lineEditBuiltin);
		exec.registerBuiltin("__histstat",  builtin_histstat);
		exec.registerBuiltin("trap",     builtin_trap);
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
		exec.registerBuiltin("compgen",  builtin_compgen);
		exec.registerBuiltin("complete", builtin_complete);
		exec.registerBuiltin("compopt",  builtin_compopt);
		exec.registerBuiltin("jobs",     builtin_jobs);
		exec.registerBuiltin("wait",     builtin_wait);
		exec.registerBuiltin("fg",       builtin_fg);
		exec.registerBuiltin("bg",       builtin_bg);
		exec.registerBuiltin("disown",   builtin_disown);
		registerDeclarationBuiltins(exec);
		registerReadBuiltin(exec);
	}

}  // namespace wbsh

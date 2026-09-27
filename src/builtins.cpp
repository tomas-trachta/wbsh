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
#include "lexer.h"
#include "lineedit.h"
#include "numparse.h"
#include "parser.h"

namespace wbsh {

	static const char* const kDefaultIfs      = " \t\n";
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

	static std::string interpretEcho(const std::string& text) {
		std::string out;
		for (std::size_t i = 0; i < text.size(); ++i) {
			const char c = text[i];
			if (c == '\\' && i + 1 < text.size()) {
				appendEchoEscape(text, i, out);
				continue;
			}

			out.push_back(c);
		}

		return out;
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
			if (args[i] == "--") return i + 1;
			if (!applyEchoFlags(args[i], newline, interpret)) break;
			++i;
		}

		return i;
	}

	static int builtin_echo(Executor&, const std::vector<std::string>& args) {
		bool newline = true;
		bool interpret = false;
		const std::size_t first = parseEchoFlags(args, newline, interpret);

		for (std::size_t i = first; i < args.size(); ++i) {
			if (i > first) std::fputc(' ', stdout);
			if (interpret) writeStdout(interpretEcho(args[i]));
			else writeStdout(args[i]);
		}

		if (newline) std::fputc('\n', stdout);
		std::fflush(stdout);
		return 0;
	}

	static void emitBackslashEscape(char next) {
		switch (next) {
		case 'a':  std::fputc('\a', stdout); break;
		case 'b':  std::fputc('\b', stdout); break;
		case 'e':  std::fputc('\x1b', stdout); break;
		case 'f':  std::fputc('\f', stdout); break;
		case 'n':  std::fputc('\n', stdout); break;
		case 'r':  std::fputc('\r', stdout); break;
		case 't':  std::fputc('\t', stdout); break;
		case 'v':  std::fputc('\v', stdout); break;
		case '\\': std::fputc('\\', stdout); break;
		default:
			std::fputc('\\', stdout);
			std::fputc(next, stdout);
			break;
		}
	}

	static void appendPrintfDigits(const std::string& fmt, std::size_t& i, std::string& out_spec) {
		while (i < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i]))) {
			out_spec.push_back(fmt[i++]);
		}
	}

	static bool parsePrintfConversionSpec(const std::string& fmt, std::size_t& i,
			std::string& out_spec) {
		out_spec = "%";
		++i;
		while (i < fmt.size() && std::strchr("-+ #0", fmt[i]) != nullptr) {
			out_spec.push_back(fmt[i++]);
		}

		appendPrintfDigits(fmt, i, out_spec);
		if (i < fmt.size() && fmt[i] == '.') {
			out_spec.push_back(fmt[i++]);
			appendPrintfDigits(fmt, i, out_spec);
		}

		if (i >= fmt.size()) return false;

		out_spec.push_back(fmt[i]);
		return true;
	}

	static std::string specWithConversion(const std::string& spec, const std::string& conversion) {
		std::string result = spec;
		result.pop_back();
		return result + conversion;
	}

	static void emitPrintfConversion(const std::string& spec, char conv,
			const std::string& arg_text) {
		switch (conv) {
		case 's':
			std::fprintf(stdout, spec.c_str(), arg_text.c_str());
			break;
		case 'd':
		case 'i':
			std::fprintf(stdout, specWithConversion(spec, "lld").c_str(), toIntOrZero(arg_text));
			break;
		case 'u':
			std::fprintf(stdout, specWithConversion(spec, "llu").c_str(),
				static_cast<unsigned long long>(toIntOrZero(arg_text)));
			break;
		case 'x':
		case 'X':
		case 'o':
			std::fprintf(stdout, specWithConversion(spec, std::string("ll") + conv).c_str(),
				static_cast<unsigned long long>(toIntOrZero(arg_text)));
			break;
		case 'f':
		case 'F':
		case 'e':
		case 'E':
		case 'g':
		case 'G':
			std::fprintf(stdout, spec.c_str(), toDoubleOrZero(arg_text));
			break;
		case 'c':
			std::fputc(arg_text.empty() ? '\0' : arg_text[0], stdout);
			break;
		case '%':
			std::fputc('%', stdout);
			break;
		default:
			writeStdout(spec);
			break;
		}
	}

	static std::string takePrintfArg(const std::vector<std::string>& args, std::size_t& next) {
		if (next >= args.size()) return std::string();
		return args[next++];
	}

	static void emitPrintfFormatOnce(const std::string& fmt, const std::vector<std::string>& args,
			std::size_t& next_arg) {
		for (std::size_t i = 0; i < fmt.size(); ++i) {
			const char c = fmt[i];
			if (c == '\\' && i + 1 < fmt.size()) {
				emitBackslashEscape(fmt[++i]);
				continue;
			}

			if (c != '%') {
				std::fputc(c, stdout);
				continue;
			}

			const std::size_t spec_start = i;
			std::string spec;
			if (!parsePrintfConversionSpec(fmt, i, spec)) {
				std::fwrite(fmt.data() + spec_start, 1, fmt.size() - spec_start, stdout);
				return;
			}

			emitPrintfConversion(spec, fmt[i], takePrintfArg(args, next_arg));
		}
	}

	static int builtin_printf(Executor&, const std::vector<std::string>& args) {
		if (args.empty()) {
			printerr("printf: missing format");
			return 2;
		}

		const std::string& fmt = args[0];
		std::size_t next_arg = 1;

		// Bash printf semantics: emit the format once, then keep cycling
		// while there are unconsumed args. If a pass consumes nothing
		// (a literal-only format), stop to avoid an infinite loop.
		emitPrintfFormatOnce(fmt, args, next_arg);
		while (next_arg < args.size()) {
			const std::size_t before = next_arg;
			emitPrintfFormatOnce(fmt, args, next_arg);
			if (next_arg == before) break;
		}

		std::fflush(stdout);
		return 0;
	}

	// -L and -P are accepted and make no difference.
	static int builtin_pwd(Executor& exec, const std::vector<std::string>& args) {
		bool windows_form = false;
		for (const std::string& arg : args) {
			if (arg == "-W") windows_form = true;
		}

		std::error_code ec;
		const std::filesystem::path here = std::filesystem::current_path(ec);
		std::string shown = ec ? exec.env().get("PWD") : pathToUtf8(here);
		if (!windows_form) shown = exec.pathConv().toPosix(shown);

		writeStdout(shown);
		std::fputc('\n', stdout);
		std::fflush(stdout);
		return 0;
	}

	static bool resolveCdTarget(Executor& exec, const std::vector<std::string>& args,
			std::string& out_target) {
		if (args.empty()) {
			out_target = exec.env().get("HOME");
			return true;
		}

		if (args[0] != "-") {
			out_target = args[0];
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

	static int builtin_cd(Executor& exec, const std::vector<std::string>& args) {
		std::string target;
		if (!resolveCdTarget(exec, args, target)) return 1;

		std::string error;
		if (!changeDirectory(exec, target, error)) {
			std::fprintf(stderr, "wbsh: cd: %s: %s\n", target.c_str(), error.c_str());
			return 1;
		}

		return 0;
	}

	static int builtin_exit(Executor& exec, const std::vector<std::string>& args) {
		const int status = firstArgAsInt(args, exec.lastStatus());
		exec.raiseExit(status);
		return status;
	}

	static int builtin_return(Executor& exec, const std::vector<std::string>& args) {
		if (exec.funcDepth() == 0) {
			printerr("return: can only `return' from a function or sourced script");
			return 1;
		}

		const int status = firstArgAsInt(args, exec.lastStatus());
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

	static void printExportedVars(const Environment& env) {
		for (const auto& entry : env.vars()) {
			if (!env.isExported(entry.first)) continue;

			std::printf("declare -x %s=\"%s\"\n", entry.first.c_str(), entry.second.c_str());
		}
	}

	static int builtin_export(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) {
			printExportedVars(exec.env());
			return 0;
		}

		for (const std::string& arg : args) {
			std::string name;
			std::string value;
			if (splitAssignment(arg, name, value)) exec.env().set(name, value);
			exec.env().exportVar(name);
		}

		return 0;
	}

	static bool unsetArrayElement(Executor& exec, const std::string& arg) {
		const std::size_t bracket = arg.find('[');
		if (bracket == std::string::npos || arg.empty() || arg.back() != ']') return false;

		const std::string name = arg.substr(0, bracket);
		const std::string subscript = arg.substr(bracket + 1, arg.size() - bracket - 2);
		if (!exec.env().isIndexedArray(name) && !exec.env().isAssocArray(name)) return false;

		long long index = 0;
		if (!exec.env().isAssocArray(name)) exec.expander().tryEvalArith(subscript, index);
		exec.env().unsetElement(name, index, subscript);
		return true;
	}

	static int builtin_unset(Executor& exec, const std::vector<std::string>& args) {
		for (const std::string& arg : args) {
			if (!unsetArrayElement(exec, arg)) exec.env().unset(arg);
		}

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

	static void applyShortSetFlag(Environment& env, char ch, bool on) {
		switch (ch) {
		case 'e': env.setErrexit(on); break;
		case 'u': env.setNounset(on); break;
		case 'x': env.setXtrace(on);  break;
		case 'f': env.setNoglob(on);  break;
		default: break;
		}
	}

	static bool applyLongSetOption(Environment& env, const std::string& name, bool on) {
		if (name == "errexit")  { env.setErrexit(on);  return true; }
		if (name == "nounset")  { env.setNounset(on);  return true; }
		if (name == "xtrace")   { env.setXtrace(on);   return true; }
		if (name == "noglob")   { env.setNoglob(on);   return true; }
		if (name == "pipefail") { env.setPipefail(on); return true; }
		return false;
	}

	static int applyLongSetArg(Executor& exec, const std::vector<std::string>& args,
			std::size_t& i, bool on) {
		if (i + 1 >= args.size()) {
			++i;
			return 0;
		}

		const std::string& name = args[i + 1];
		if (!applyLongSetOption(exec.env(), name, on)) {
			std::fprintf(stderr, "wbsh: set: unknown option: %s\n", name.c_str());
			return 2;
		}

		i += 2;
		return 0;
	}

	static int parseSetFlags(Executor& exec, const std::vector<std::string>& args,
			std::size_t& out_index, bool& out_consumed) {
		std::size_t i = 0;
		bool consumed = false;
		while (i < args.size()) {
			const std::string& arg = args[i];
			if (arg == "--" || arg == "-") {
				++i;
				consumed = true;
				break;
			}

			if (arg.empty() || (arg[0] != '-' && arg[0] != '+')) break;

			const bool on = arg[0] == '-';
			if (arg.size() > 1 && arg[1] == 'o') {
				const int status = applyLongSetArg(exec, args, i, on);
				if (status != 0) return status;

				consumed = true;
				continue;
			}

			for (std::size_t k = 1; k < arg.size(); ++k) applyShortSetFlag(exec.env(), arg[k], on);
			++i;
			consumed = true;
		}

		out_index = i;
		out_consumed = consumed;
		return 0;
	}

	static int builtin_set(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) return dumpAllShellVars(exec);

		std::size_t first = 0;
		bool consumed_flags = false;
		const int status = parseSetFlags(exec, args, first, consumed_flags);
		if (status != 0) return status;

		if (first < args.size() || !consumed_flags) {
			std::vector<std::string> positional(args.begin() + first, args.end());
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

	static std::string quoteDouble(const std::string& word) {
		std::string quoted = "\"";
		for (const char c : word) {
			if (c == '"' || c == '\\' || c == '$' || c == '`') quoted.push_back('\\');
			quoted.push_back(c);
		}

		quoted.push_back('"');
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

	static int builtin_exec(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) return 0;

		const std::string command = args[0];
		const std::vector<std::string> rest(args.begin() + 1, args.end());
		if (exec.isBuiltin(command)) return exec.callBuiltin(command, rest);
		if (exec.isFunction(command)) return exec.callFunction(command, rest);

		const int status = exec.executeText(joinQuoted(args, quoteSingle), "<exec>");
		if (!exec.flowPending()) exec.raiseExit(status);
		return status;
	}

	static int builtin_eval(Executor& exec, const std::vector<std::string>& args) {
		const std::string joined = joinWithSpaces(args.begin(), args.end());
		if (joined.empty()) return 0;

		return exec.executeText(joined, "eval");
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

		int status = exec.executeText(body, args[0]);
		exec.consumeFlow(FlowSignal::Kind::Return, &status);
		exec.env().setPositional(std::move(saved));
		return status;
	}

	static bool printCommandType(Executor& exec, const std::string& name) {
		if (exec.isFunction(name)) {
			std::printf("%s is a function\n", name.c_str());
			return true;
		}

		if (exec.isBuiltin(name)) {
			std::printf("%s is a shell builtin\n", name.c_str());
			return true;
		}

		const std::string found = exec.findExecutable(name);
		if (found.empty()) {
			std::fprintf(stderr, "wbsh: type: %s: not found\n", name.c_str());
			return false;
		}

		std::printf("%s is %s\n", name.c_str(), found.c_str());
		return true;
	}

	static int builtin_type(Executor& exec, const std::vector<std::string>& args) {
		int status = 0;
		for (const std::string& name : args) {
			if (!printCommandType(exec, name)) status = 1;
		}

		return status;
	}

	static std::size_t parseCommandFlags(const std::vector<std::string>& args, bool& describe) {
		std::size_t i = 0;
		while (i < args.size() && !args[i].empty() && args[i][0] == '-') {
			const std::string& flag = args[i];
			if (flag == "--") return i + 1;
			if (flag != "-v" && flag != "-V") break;

			describe = true;
			++i;
		}

		return i;
	}

	static int builtin_command(Executor& exec, const std::vector<std::string>& args) {
		bool describe = false;
		const std::size_t first = parseCommandFlags(args, describe);
		if (first >= args.size()) return 0;

		const std::vector<std::string> rest(args.begin() + first, args.end());
		if (describe) return builtin_type(exec, rest);

		if (exec.isBuiltin(rest[0])) {
			const std::vector<std::string> sub(rest.begin() + 1, rest.end());
			return exec.callBuiltin(rest[0], sub);
		}

		return exec.executeText(joinQuoted(rest, quoteDouble), "command");
	}

	namespace read_detail {
		struct ReadOptions {
			bool raw = false;
			std::string prompt;
			std::string array_name;
		};
	}  // namespace read_detail

	static std::size_t parseReadFlags(const std::vector<std::string>& args,
			read_detail::ReadOptions& options) {
		std::size_t i = 0;
		while (i < args.size() && !args[i].empty() && args[i][0] == '-') {
			const std::string& flag = args[i];
			if (flag == "--") return i + 1;
			if (flag == "-r") {
				options.raw = true;
				++i;
				continue;
			}

			if (flag != "-p" && flag != "-a") break;

			const bool has_value = i + 1 < args.size();
			if (has_value && flag == "-p") options.prompt = args[i + 1];
			if (has_value && flag == "-a") options.array_name = args[i + 1];
			i += has_value ? 2 : 1;
		}

		return i;
	}

	static bool readOneLineFromStdin(bool raw, std::string& line) {
		for (;;) {
			const int ch = std::fgetc(stdin);
			if (ch == EOF) return !line.empty();
			if (ch == '\n') return true;

			if (!raw && ch == '\\') {
				const int next = std::fgetc(stdin);
				if (next == EOF) return true;
				if (next == '\n') continue;

				line.push_back(static_cast<char>(next));
				continue;
			}

			line.push_back(static_cast<char>(ch));
		}
	}

	static bool isIfsChar(char c, const std::string& ifs) {
		return ifs.find(c) != std::string::npos;
	}

	static bool isIfsWhitespace(char c, const std::string& ifs) {
		return (c == ' ' || c == '\t' || c == '\n') && isIfsChar(c, ifs);
	}

	// One non-whitespace IFS character ends a field on its own; the IFS
	// whitespace around it is swallowed with it, as in bash.
	static void skipFieldSeparator(const std::string& line, std::size_t& pos,
			const std::string& ifs) {
		bool saw_nonws = false;
		while (pos < line.size() && isIfsChar(line[pos], ifs)) {
			if (!isIfsWhitespace(line[pos], ifs)) {
				if (saw_nonws) break;
				saw_nonws = true;
			}

			++pos;
		}
	}

	static std::vector<std::string> splitReadLine(const std::string& line, const std::string& ifs) {
		std::vector<std::string> fields;
		std::size_t pos = 0;
		while (pos < line.size() && isIfsWhitespace(line[pos], ifs)) ++pos;

		while (pos < line.size()) {
			std::string field;
			while (pos < line.size() && !isIfsChar(line[pos], ifs)) field.push_back(line[pos++]);
			fields.push_back(std::move(field));

			skipFieldSeparator(line, pos, ifs);
		}

		return fields;
	}

	static void assignReadFields(Environment& env, const std::vector<std::string>& names,
			const std::vector<std::string>& fields) {
		for (std::size_t k = 0; k < names.size(); ++k) {
			const bool last_name = k + 1 == names.size();

			std::string value;
			if (k < fields.size()) {
				value = last_name ? joinWithSpaces(fields.begin() + k, fields.end()) : fields[k];
			}

			env.set(names[k], value);
		}
	}

	static int builtin_read(Executor& exec, const std::vector<std::string>& args) {
		read_detail::ReadOptions options;
		const std::size_t first_name = parseReadFlags(args, options);

		if (!options.prompt.empty()) {
			std::fwrite(options.prompt.data(), 1, options.prompt.size(), stderr);
			std::fflush(stderr);
		}

		std::string line;
		if (!readOneLineFromStdin(options.raw, line)) return 1;

		std::string ifs = exec.env().get("IFS");
		if (ifs.empty()) ifs = kDefaultIfs;
		std::vector<std::string> fields = splitReadLine(line, ifs);

		if (!options.array_name.empty()) {
			exec.env().setIndexedArrayFromList(options.array_name, std::move(fields));
			return 0;
		}

		std::vector<std::string> names(args.begin() + first_name, args.end());
		if (names.empty()) names.push_back("REPLY");
		assignReadFields(exec.env(), names, fields);
		return 0;
	}

	static int evalUnaryFileTest(char op, const std::string& raw_path, const PathConv& pc) {
		const std::string path = pc.toWin32(raw_path);
		struct stat info {};
		const bool exists = ::stat(path.c_str(), &info) == 0;
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

	static void printIndexedArrayEntry(const std::string& name,
			const Environment::IndexedArray& array) {
		std::printf("declare -a %s=(", name.c_str());
		bool first = true;
		for (const auto& element : array) {
			if (!first) std::printf(" ");
			std::printf("[%lld]=\"%s\"", element.first, element.second.c_str());
			first = false;
		}

		std::printf(")\n");
	}

	static void printAssocArrayEntry(const std::string& name,
			const Environment::AssocArray& array) {
		std::printf("declare -A %s=(", name.c_str());
		bool first = true;
		for (const auto& element : array) {
			if (!first) std::printf(" ");
			std::printf("[\"%s\"]=\"%s\"", element.first.c_str(), element.second.c_str());
			first = false;
		}

		std::printf(")\n");
	}

	static std::string declareAttributes(const Environment& env, const std::string& name) {
		std::string attributes = "-";
		if (env.isExported(name)) attributes += "x";
		if (env.isReadonly(name)) attributes += "r";
		return attributes;
	}

	static void printDeclareEntry(Executor& exec, const std::string& name) {
		const Environment& env = exec.env();
		if (const auto* indexed = env.getIndexedArray(name)) {
			printIndexedArrayEntry(name, *indexed);
			return;
		}

		if (const auto* assoc = env.getAssocArray(name)) {
			printAssocArrayEntry(name, *assoc);
			return;
		}

		std::printf("declare %s %s=\"%s\"\n", declareAttributes(env, name).c_str(), name.c_str(),
			env.get(name).c_str());
	}

	namespace declare_detail {
		struct DeclareFlags {
			bool export_it     = false;
			bool readonly      = false;
			bool print_only    = false;
			bool indexed_array = false;
			bool assoc_array   = false;
		};
	}  // namespace declare_detail

	static void applyDeclareFlagChars(const std::string& arg, declare_detail::DeclareFlags& flags) {
		for (std::size_t k = 1; k < arg.size(); ++k) {
			switch (arg[k]) {
			case 'x': flags.export_it     = true; break;
			case 'r': flags.readonly      = true; break;
			case 'p': flags.print_only    = true; break;
			case 'a': flags.indexed_array = true; break;
			case 'A': flags.assoc_array   = true; break;
			default: break;
			}
		}
	}

	static void parseDeclareFlags(const std::vector<std::string>& args,
			declare_detail::DeclareFlags& flags, std::vector<std::string>& names) {
		for (const std::string& arg : args) {
			if (arg == "--") continue;

			if (!arg.empty() && arg[0] == '-') {
				applyDeclareFlagChars(arg, flags);
				continue;
			}

			names.push_back(arg);
		}
	}

	static int dumpAllDeclareEntries(Executor& exec) {
		for (const auto& entry : sortedEntries(exec.env().vars())) {
			printDeclareEntry(exec, entry.first);
		}

		std::vector<std::string> array_names = keysOf(exec.env().indexedArrays());
		const std::vector<std::string> assoc_names = keysOf(exec.env().assocArrays());
		array_names.insert(array_names.end(), assoc_names.begin(), assoc_names.end());
		std::sort(array_names.begin(), array_names.end());
		for (const std::string& name : array_names) printDeclareEntry(exec, name);
		return 0;
	}

	static int printDeclareNamedEntries(Executor& exec, const std::vector<std::string>& specs) {
		int status = 0;
		for (const std::string& spec : specs) {
			const std::string name = spec.substr(0, spec.find('='));
			if (!exec.env().has(name)) {
				std::fprintf(stderr, "wbsh: declare: %s: not found\n", name.c_str());
				status = 1;
				continue;
			}

			printDeclareEntry(exec, name);
		}

		return status;
	}

	static void applyDeclareToName(Executor& exec, const declare_detail::DeclareFlags& flags,
			const std::string& spec) {
		std::string name;
		std::string value;
		const bool has_value = splitAssignment(spec, name, value);

		if (flags.assoc_array && !has_value) {
			exec.env().declareAssocArray(name);
		} else if (flags.indexed_array && !has_value) {
			exec.env().setIndexedArrayFromList(name, {});
		} else if (has_value) {
			exec.env().set(name, value);
		}

		if (flags.export_it) exec.env().exportVar(name);
		if (flags.readonly) exec.env().markReadonly(name);
	}

	static int builtin_declare(Executor& exec, const std::vector<std::string>& args) {
		declare_detail::DeclareFlags flags;
		std::vector<std::string> names;
		parseDeclareFlags(args, flags, names);

		if (names.empty())     return dumpAllDeclareEntries(exec);
		if (flags.print_only)  return printDeclareNamedEntries(exec, names);

		for (const std::string& spec : names) applyDeclareToName(exec, flags, spec);
		return 0;
	}

	namespace shopt_detail {
		struct ShoptFlag {
			const char* name;
			bool (Environment::*get)() const;
			void (Environment::*set)(bool);
		};

		enum class Mode { ListAll, Set, Unset, Query };
	}  // namespace shopt_detail

	static const shopt_detail::ShoptFlag kShoptFlags[] = {
		{ "nullglob",       &Environment::nullglob,       &Environment::setNullglob },
		{ "dotglob",        &Environment::dotglob,        &Environment::setDotglob },
		{ "extglob",        &Environment::extglob,        &Environment::setExtglob },
		{ "nocaseglob",     &Environment::nocaseglob,     &Environment::setNocaseglob },
		{ "nocasematch",    &Environment::nocasematch,    &Environment::setNocasematch },
		{ "globstar",       &Environment::globstar,       &Environment::setGlobstar },
		{ "lastpipe",       &Environment::lastpipe,       &Environment::setLastpipe },
		{ "huponexit",      &Environment::huponexit,      &Environment::setHuponexit },
		{ "expand_aliases", &Environment::expand_aliases, &Environment::setExpandAliases },
		{ "autocd",         &Environment::autocd,         &Environment::setAutocd },
		{ "checkwinsize",   &Environment::checkwinsize,   &Environment::setCheckwinsize },
	};

	static const shopt_detail::ShoptFlag* findShoptFlag(const std::string& name) {
		for (const shopt_detail::ShoptFlag& flag : kShoptFlags) {
			if (name == flag.name) return &flag;
		}

		return nullptr;
	}

	static void printShoptFlag(const Environment& env, const shopt_detail::ShoptFlag& flag) {
		const bool on = (env.*(flag.get))();
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
			const shopt_detail::ShoptFlag* flag = findShoptFlag(name);
			if (flag == nullptr) {
				std::fprintf(stderr, "wbsh: shopt: %s: invalid option name\n", name.c_str());
				status = 1;
				continue;
			}

			(exec.env().*(flag->set))(on);
		}

		return status;
	}

	static int shoptQuery(Executor& exec, const std::vector<std::string>& names) {
		for (const std::string& name : names) {
			const shopt_detail::ShoptFlag* flag = findShoptFlag(name);
			if (flag == nullptr) return 1;
			if (!(exec.env().*(flag->get))()) return 1;
		}

		return 0;
	}

	static int shoptListMode(Executor& exec, const std::vector<std::string>& names) {
		if (names.empty()) {
			for (const shopt_detail::ShoptFlag& flag : kShoptFlags) {
				printShoptFlag(exec.env(), flag);
			}

			return 0;
		}

		int status = 0;
		for (const std::string& name : names) {
			const shopt_detail::ShoptFlag* flag = findShoptFlag(name);
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

	static int builtin_readonly(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) {
			for (const std::string& name : exec.env().readonlySet()) {
				std::printf("declare -r %s=\"%s\"\n", name.c_str(), exec.env().get(name).c_str());
			}

			return 0;
		}

		for (const std::string& spec : args) {
			if (!spec.empty() && spec[0] == '-') continue;

			std::string name;
			std::string value;
			if (splitAssignment(spec, name, value)) exec.env().set(name, value);
			exec.env().markReadonly(name);
		}

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
			for (const auto& entry : exec.trapHandlers()) {
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

		long long last = 0;
		for (const std::string& expression : args) {
			if (!exec.expander().tryEvalArith(expression, last)) return 1;
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

	static void printBuiltinIndex(Executor& exec) {
		std::printf("wbsh built-in commands:\n\n");

		std::vector<std::string> names = exec.builtinNames();
		std::sort(names.begin(), names.end());
		for (std::size_t i = 0; i < names.size(); ++i) {
			std::printf("  %-14s", names[i].c_str());
			if (i % kHelpColumns == kHelpColumns - 1) std::printf("\n");
		}

		if (names.size() % kHelpColumns != 0) std::printf("\n");
		std::printf("\nUse `help NAME` for more on a specific builtin.\n");
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

			std::printf("%s: %s — see bash(1) for full semantics\n", name.c_str(), name.c_str());
		}

		return status;
	}

	static int builtin_local(Executor& exec, const std::vector<std::string>& args) {
		if (exec.funcDepth() == 0) {
			printerr("local: can only be used in a function");
			return 1;
		}

		for (const std::string& arg : args) {
			std::string name;
			std::string value;
			splitAssignment(arg, name, value);
			exec.declareLocal(name, value);
		}

		return 0;
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
		exec.registerBuiltin("export",   builtin_export);
		exec.registerBuiltin("unset",    builtin_unset);
		exec.registerBuiltin("shift",    builtin_shift);
		exec.registerBuiltin("set",      builtin_set);
		exec.registerBuiltin("eval",     builtin_eval);
		exec.registerBuiltin("source",   builtin_source);
		exec.registerBuiltin(".",        builtin_source);
		exec.registerBuiltin("type",     builtin_type);
		exec.registerBuiltin("command",  builtin_command);
		exec.registerBuiltin("read",     builtin_read);
		exec.registerBuiltin("test",     builtin_test);
		exec.registerBuiltin("[",        builtin_lbracket);
		exec.registerBuiltin("local",    builtin_local);
		exec.registerBuiltin("alias",    builtin_alias);
		exec.registerBuiltin("unalias",  builtin_unalias);
		exec.registerBuiltin("history",  builtin_history);
		exec.registerBuiltin("__revsearch", builtin_revsearch);
		exec.registerBuiltin("__predict",   builtin_predict);
		exec.registerBuiltin("__histstat",  builtin_histstat);
		exec.registerBuiltin("trap",     builtin_trap);
		exec.registerBuiltin("getopts",  builtin_getopts);
		exec.registerBuiltin("declare",  builtin_declare);
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
		exec.registerBuiltin("typeset",  builtin_declare);
		exec.registerBuiltin("readonly", builtin_readonly);
		exec.registerBuiltin("jobs",     builtin_jobs);
		exec.registerBuiltin("wait",     builtin_wait);
		exec.registerBuiltin("fg",       builtin_fg);
		exec.registerBuiltin("bg",       builtin_bg);
		exec.registerBuiltin("disown",   builtin_disown);
	}

}  // namespace wbsh

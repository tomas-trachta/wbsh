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
#include "parser.h"

namespace wbsh {

	// ---- Tiny helpers ----

	static long long toIntSafe(const std::string& s, bool& ok) {
		ok = false;
		if (s.empty()) return 0;
		try {
			std::size_t idx = 0;
			long long v = std::stoll(s, &idx, 10);
			if (idx != s.size()) return 0;
			ok = true;
			return v;
		} catch (...) { return 0; }
	}

	// Parse args[0] as a small int. Returns `fallback` when args is empty,
	// the value isn't a parseable integer, or (when require_positive)
	// it's <= 0. Centralises the "first arg is an optional count" idiom
	// used by exit/return/break/continue/shift.
	static int firstArgAsInt(const std::vector<std::string>& args, int fallback,
	                  bool require_positive = false) {
		if (args.empty()) return fallback;
		bool ok = false;
		long long v = toIntSafe(args[0], ok);
		if (!ok) return fallback;
		if (require_positive && v <= 0) return fallback;
		return static_cast<int>(v);
	}

	static void printerr(const std::string& msg) {
		std::fprintf(stderr, "wbsh: %s\n", msg.c_str());
	}

	// ---- Builtin: true/false/: ----

	static int builtin_true(Executor&, const std::vector<std::string>&) { return 0; }
	static int builtin_false(Executor&, const std::vector<std::string>&) { return 1; }
	static int builtin_colon(Executor&, const std::vector<std::string>&) { return 0; }

	// ---- echo ----

	static std::string interpretEcho(const std::string& s) {
		std::string out;
		for (std::size_t i = 0; i < s.size(); ++i) {
			char c = s[i];
			if (c != '\\' || i + 1 >= s.size()) { out.push_back(c); continue; }
			char nx = s[++i];
			switch (nx) {
			case 'a': out.push_back('\a'); break;
			case 'b': out.push_back('\b'); break;
			case 'e': out.push_back('\x1b'); break;
			case 'f': out.push_back('\f'); break;
			case 'n': out.push_back('\n'); break;
			case 'r': out.push_back('\r'); break;
			case 't': out.push_back('\t'); break;
			case 'v': out.push_back('\v'); break;
			case '\\': out.push_back('\\'); break;
			case '0': {
				int val = 0, cnt = 0;
				while (cnt < 3 && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7') {
					val = val * 8 + (s[++i] - '0');
					++cnt;
				}
				out.push_back(static_cast<char>(val));
				break;
			}
			case 'x': {
				int val = 0, cnt = 0;
				while (cnt < 2 && i + 1 < s.size()
				    && std::isxdigit(static_cast<unsigned char>(s[i + 1]))) {
					char h = s[++i];
					int d = std::isdigit(static_cast<unsigned char>(h))
					          ? h - '0'
					          : std::tolower(static_cast<unsigned char>(h)) - 'a' + 10;
					val = val * 16 + d;
					++cnt;
				}
				out.push_back(static_cast<char>(val));
				break;
			}
			default: out.push_back('\\'); out.push_back(nx); break;
			}
		}
		return out;
	}

	static int builtin_echo(Executor&, const std::vector<std::string>& args) {
		bool newline = true;
		bool interp = false;
		std::size_t i = 0;
		while (i < args.size() && !args[i].empty() && args[i][0] == '-') {
			const std::string& f = args[i];
			if (f == "--") { ++i; break; }
			bool ok = f.size() > 1;
			for (std::size_t k = 1; k < f.size(); ++k) {
				if (f[k] == 'n') newline = false;
				else if (f[k] == 'e') interp = true;
				else if (f[k] == 'E') interp = false;
				else { ok = false; break; }
			}
			if (!ok) break;
			++i;
		}
		bool first = true;
		for (; i < args.size(); ++i) {
			if (!first) std::fputc(' ', stdout);
			first = false;
			if (interp) {
				std::string s = interpretEcho(args[i]);
				std::fwrite(s.data(), 1, s.size(), stdout);
			} else {
				std::fwrite(args[i].data(), 1, args[i].size(), stdout);
			}
		}
		if (newline) std::fputc('\n', stdout);
		std::fflush(stdout);
		return 0;
	}

	// ---- printf (subset) ----

	// Emit the C-string equivalent of a `\X` escape (e.g. `\n` -> newline).
	// Unknown escapes pass through as `\X` literal (matching bash printf).
	static void emitBackslashEscape(char nx) {
		switch (nx) {
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
			std::fputc(nx, stdout);
			break;
		}
	}

	// Parse a `%[flags][width][.precision]<conv>` spec starting at fmt[i]
	// (which must be `%`). On return, `i` indexes the conversion char and
	// `out_spec` holds the full spec string (including the conv char).
	// Returns false if the format ended mid-spec (no conv char); the caller
	// should print the partial spec verbatim and stop emitting.
	static bool parsePrintfConversionSpec(const std::string& fmt, std::size_t& i,
	                                      std::string& out_spec) {
		out_spec = "%";
		++i;  // step past '%'
		while (i < fmt.size() && std::strchr("-+ #0", fmt[i])) out_spec.push_back(fmt[i++]);
		while (i < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i])))
			out_spec.push_back(fmt[i++]);
		if (i < fmt.size() && fmt[i] == '.') {
			out_spec.push_back(fmt[i++]);
			while (i < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i])))
				out_spec.push_back(fmt[i++]);
		}
		if (i >= fmt.size()) return false;
		out_spec.push_back(fmt[i]);   // conversion char
		return true;
	}

	// Emit one conversion. `spec` ends in `conv`; `arg_text` is the next
	// printf positional arg (already consumed by the caller).
	static void emitPrintfConversion(const std::string& spec, char conv,
	                                 const std::string& arg_text) {
		switch (conv) {
		case 's':
			std::fprintf(stdout, spec.c_str(), arg_text.c_str());
			break;
		case 'd':
		case 'i': {
			bool ok;
			const long long v = toIntSafe(arg_text, ok);
			std::string s2 = spec; s2.pop_back(); s2 += "lld";
			std::fprintf(stdout, s2.c_str(), v);
			break;
		}
		case 'u': {
			bool ok;
			const long long v = toIntSafe(arg_text, ok);
			std::string s2 = spec; s2.pop_back(); s2 += "llu";
			std::fprintf(stdout, s2.c_str(), static_cast<unsigned long long>(v));
			break;
		}
		case 'x':
		case 'X':
		case 'o': {
			bool ok;
			const long long v = toIntSafe(arg_text, ok);
			std::string s2 = spec; s2.pop_back(); s2 += "ll";
			s2.push_back(conv);
			std::fprintf(stdout, s2.c_str(), static_cast<unsigned long long>(v));
			break;
		}
		case 'c':
			std::fputc(arg_text.empty() ? '\0' : arg_text[0], stdout);
			break;
		case '%':
			std::fputc('%', stdout);
			break;
		default:
			// Unknown conversion: print the spec verbatim.
			std::fwrite(spec.data(), 1, spec.size(), stdout);
			break;
		}
	}

	// Walk `fmt` once, emitting literal text and consuming positional args
	// from `args` starting at `*ai`. On return `*ai` is updated to the
	// index of the next unconsumed arg.
	static void emitPrintfFormatOnce(const std::string& fmt,
	                                 const std::vector<std::string>& args,
	                                 std::size_t* ai) {
		auto next_arg = [&]() -> std::string {
			return (*ai < args.size()) ? args[(*ai)++] : std::string();
		};
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
				// Truncated spec at end of string: emit verbatim, stop.
				std::fwrite(fmt.data() + spec_start, 1, fmt.size() - spec_start, stdout);
				return;
			}
			emitPrintfConversion(spec, fmt[i], next_arg());
		}
	}

	static int builtin_printf(Executor&, const std::vector<std::string>& args) {
		if (args.empty()) {
			printerr("printf: missing format");
			return 2;
		}
		const std::string& fmt = args[0];
		std::size_t ai = 1;

		// Bash printf semantics: emit the format once, then keep cycling
		// while there are unconsumed args. If a pass consumes nothing
		// (a literal-only format), stop to avoid an infinite loop.
		emitPrintfFormatOnce(fmt, args, &ai);
		while (ai < args.size()) {
			const std::size_t before = ai;
			emitPrintfFormatOnce(fmt, args, &ai);
			if (ai == before) break;
		}
		std::fflush(stdout);
		return 0;
	}

	// ---- cd / pwd ----

	static int builtin_pwd(Executor& exec, const std::vector<std::string>& args) {
		namespace fs = std::filesystem;
		bool win = false;
		for (const auto& a : args) {
			if (a == "-W") win = true;
			else if (a == "-P" || a == "-L") { /* accept silently */ }
		}
		std::error_code ec;
		auto p = fs::current_path(ec);
		std::string s = ec ? exec.env().get("PWD") : pathToUtf8(p);
		if (!win) s = exec.pathConv().toPosix(s);
		std::fwrite(s.data(), 1, s.size(), stdout);
		std::fputc('\n', stdout);
		std::fflush(stdout);
		return 0;
	}

	static int builtin_cd(Executor& exec, const std::vector<std::string>& args) {
		namespace fs = std::filesystem;
		std::string target;
		if (args.empty()) target = exec.env().get("HOME");
		else if (args[0] == "-") {
			target = exec.env().get("OLDPWD");
			if (target.empty()) { printerr("cd: OLDPWD not set"); return 1; }
			std::printf("%s\n", exec.pathConv().toPosix(target).c_str());
		} else target = args[0];
		fs::path win_target = utf8ToPath(exec.pathConv().toWin32(target));
		std::error_code ec;
		fs::current_path(win_target, ec);
		if (ec) {
			std::fprintf(stderr, "wbsh: cd: %s: %s\n", target.c_str(), ec.message().c_str());
			return 1;
		}
		std::string old_pwd = exec.env().get("PWD");
		auto cwd = fs::current_path(ec);
		if (!ec) {
			if (!old_pwd.empty()) exec.env().set("OLDPWD", old_pwd);
			// Store PWD in POSIX form so $PWD looks bash-like.
			exec.env().set("PWD", exec.pathConv().toPosix(pathToUtf8(cwd)));
		}
		return 0;
	}

	// ---- exit / return / break / continue ----

	static int builtin_exit(Executor& exec, const std::vector<std::string>& args) {
		throw ShellExit{ firstArgAsInt(args, exec.lastStatus()) };
	}

	static int builtin_return(Executor& exec, const std::vector<std::string>& args) {
		if (exec.funcDepth() == 0) {
			printerr("return: can only `return' from a function or sourced script");
			return 1;
		}
		throw FunctionReturn{ firstArgAsInt(args, exec.lastStatus()) };
	}

	static int builtin_break(Executor& exec, const std::vector<std::string>& args) {
		if (exec.loopDepth() == 0) {
			printerr("break: only meaningful in a `for', `while', or `until' loop");
			return 0;
		}
		throw LoopBreak{ firstArgAsInt(args, 1, /*require_positive=*/true) };
	}

	static int builtin_continue(Executor& exec, const std::vector<std::string>& args) {
		if (exec.loopDepth() == 0) {
			printerr("continue: only meaningful in a `for', `while', or `until' loop");
			return 0;
		}
		throw LoopContinue{ firstArgAsInt(args, 1, /*require_positive=*/true) };
	}

	// ---- export / unset / shift ----

	static int builtin_export(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) {
			for (const auto& kv : exec.env().vars()) {
				if (exec.env().isExported(kv.first)) {
					std::printf("declare -x %s=\"%s\"\n", kv.first.c_str(), kv.second.c_str());
				}
			}
			return 0;
		}
		for (const auto& a : args) {
			auto eq = a.find('=');
			std::string name = (eq == std::string::npos) ? a : a.substr(0, eq);
			if (eq != std::string::npos) {
				exec.env().set(name, a.substr(eq + 1));
			}
			exec.env().exportVar(name);
		}
		return 0;
	}

	static int builtin_unset(Executor& exec, const std::vector<std::string>& args) {
		for (const auto& a : args) exec.env().unset(a);
		return 0;
	}

	static int builtin_shift(Executor& exec, const std::vector<std::string>& args) {
		int n = firstArgAsInt(args, 1);
		auto pos = exec.env().positional();
		if (n < 0 || static_cast<std::size_t>(n) > pos.size()) return 1;
		pos.erase(pos.begin(), pos.begin() + n);
		exec.env().setPositional(std::move(pos));
		return 0;
	}

	// Bare `set`: dump all shell vars (sorted, `name=value` per line).
	static int dumpAllShellVars(Executor& exec) {
		std::vector<std::pair<std::string, std::string>> v(
			exec.env().vars().begin(), exec.env().vars().end());
		std::sort(v.begin(), v.end());
		for (const auto& kv : v) std::printf("%s=%s\n", kv.first.c_str(), kv.second.c_str());
		return 0;
	}

	// Apply one short option char (`e`, `u`, `x`, `f`) to the shell flags.
	// Unknown chars are silently ignored, matching bash leniency.
	static void applyShortSetFlag(Environment& env, char ch, bool on) {
		switch (ch) {
		case 'e': env.setErrexit(on); break;
		case 'u': env.setNounset(on); break;
		case 'x': env.setXtrace(on);  break;
		case 'f': env.setNoglob(on);  break;
		default: break;
		}
	}

	// Apply one long option name (`-o errexit`, `+o pipefail`). Returns
	// false if the name is not a recognised option.
	static bool applyLongSetOption(Environment& env, const std::string& name, bool on) {
		if (name == "errexit")  { env.setErrexit(on);  return true; }
		if (name == "nounset")  { env.setNounset(on);  return true; }
		if (name == "xtrace")   { env.setXtrace(on);   return true; }
		if (name == "noglob")   { env.setNoglob(on);   return true; }
		if (name == "pipefail") { env.setPipefail(on); return true; }
		return false;
	}

	// Walk `args` consuming option-style entries (`-e`, `-o errexit`,
	// `+u`, `--`) until the first non-option. Returns the index of the
	// first non-option arg via `*out_idx` and writes whether any flags
	// were consumed via `*out_consumed`. Returns 0 on success or a
	// non-zero status mirroring bash for unknown `-o NAME` operands.
	static int parseSetFlags(Executor& exec, const std::vector<std::string>& args,
	                         std::size_t* out_idx, bool* out_consumed) {
		std::size_t i = 0;
		bool consumed = false;
		while (i < args.size()) {
			const std::string& a = args[i];
			if (a == "--" || a == "-") { ++i; consumed = true; break; }
			if (a.empty() || (a[0] != '-' && a[0] != '+')) break;

			const bool on = (a[0] == '-');
			// `-o NAME` / `+o NAME` — long option toggle.
			if (a.size() > 1 && a[1] == 'o') {
				if (i + 1 < args.size()) {
					if (!applyLongSetOption(exec.env(), args[i + 1], on)) {
						std::fprintf(stderr, "wbsh: set: unknown option: %s\n",
							args[i + 1].c_str());
						return 2;
					}
					i += 2;
				} else {
					++i;   // bare `-o` / `+o` — bash prints opts; we no-op.
				}
				consumed = true;
				continue;
			}
			// `-eXf` / `+xu` — cluster of short options.
			for (std::size_t k = 1; k < a.size(); ++k) {
				applyShortSetFlag(exec.env(), a[k], on);
			}
			++i;
			consumed = true;
		}
		*out_idx = i;
		*out_consumed = consumed;
		return 0;
	}

	static int builtin_set(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) return dumpAllShellVars(exec);

		std::size_t i = 0;
		bool consumed_flags = false;
		const int rc = parseSetFlags(exec, args, &i, &consumed_flags);
		if (rc != 0) return rc;

		// Positional arguments are reset only when explicit args remain
		// after flag parsing (otherwise `set -e` would erase $@).
		if (i < args.size() || !consumed_flags) {
			std::vector<std::string> pos(args.begin() + i, args.end());
			exec.env().setPositional(std::move(pos));
		}
		return 0;
	}

	// ---- eval / source ----

	static int builtin_exec(Executor& exec, const std::vector<std::string>& args) {
		// Bash semantics: with no command, redirections take effect on
		// the current shell (permanent for the rest of the session).
		// With a command, replaces the shell with that command — we
		// can't do the latter losslessly on Windows so we just run it
		// and exit with its status.
		if (args.empty()) return 0;   // redirections-only handled by caller
		// Run as if it were a regular external invocation.
		std::vector<std::string> argv = args;
		std::string cmd = argv[0];
		argv.erase(argv.begin());
		if (exec.isBuiltin(cmd)) return exec.callBuiltin(cmd, argv);
		if (exec.isFunction(cmd)) return exec.callFunction(cmd, argv);
		std::string joined;
		for (std::size_t i = 0; i < args.size(); ++i) {
			if (i) joined.push_back(' ');
			joined.push_back('\'');
			for (char c : args[i]) {
				if (c == '\'') joined += "'\\''";
				else joined.push_back(c);
			}
			joined.push_back('\'');
		}
		int rc = exec.executeText(joined, "<exec>");
		throw ShellExit{ rc };
	}

	static int builtin_eval(Executor& exec, const std::vector<std::string>& args) {
		std::string joined;
		for (std::size_t i = 0; i < args.size(); ++i) {
			if (i) joined.push_back(' ');
			joined += args[i];
		}
		if (joined.empty()) return 0;
		return exec.executeText(joined, "eval");
	}

	static int builtin_source(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) {
			printerr("source: filename required");
			return 2;
		}
		std::filesystem::path p = utf8ToPath(exec.pathConv().toWin32(args[0]));
		std::ifstream f(p, std::ios::binary);
		if (!f) {
			std::fprintf(stderr, "wbsh: source: %s: %s\n",
				args[0].c_str(), std::strerror(errno));
			return 1;
		}
		std::stringstream ss;
		ss << f.rdbuf();
		std::string body = ss.str();
		normalizeCrlf(body);
		// If extra args provided, set them as positional during the source.
		auto saved = exec.env().positional();
		if (args.size() > 1) {
			exec.env().setPositional({ args.begin() + 1, args.end() });
		}
		int r = 0;
		try { r = exec.executeText(body, args[0]); }
		catch (FunctionReturn& fr) { r = fr.status; }   // `return' from sourced top-level
		exec.env().setPositional(std::move(saved));
		return r;
	}

	// ---- type / command ----

	static int builtin_type(Executor& exec, const std::vector<std::string>& args) {
		int rc = 0;
		for (const auto& a : args) {
			if (exec.isFunction(a)) {
				std::printf("%s is a function\n", a.c_str());
			} else if (exec.isBuiltin(a)) {
				std::printf("%s is a shell builtin\n", a.c_str());
			} else {
				std::string found = exec.findExecutable(a);
				if (!found.empty()) {
					std::printf("%s is %s\n", a.c_str(), found.c_str());
				} else {
					std::fprintf(stderr, "wbsh: type: %s: not found\n", a.c_str());
					rc = 1;
				}
			}
		}
		return rc;
	}

	static int builtin_command(Executor& exec, const std::vector<std::string>& args) {
		std::size_t i = 0;
		bool show_path = false;
		bool show_verbose = false;
		while (i < args.size() && !args[i].empty() && args[i][0] == '-') {
			const std::string& f = args[i];
			if (f == "--") { ++i; break; }
			if (f == "-v") { show_path = true; ++i; continue; }
			if (f == "-V") { show_verbose = true; ++i; continue; }
			break;
		}
		if (i >= args.size()) return 0;
		if (show_path || show_verbose) {
			std::vector<std::string> rest(args.begin() + i, args.end());
			return builtin_type(exec, rest);
		}
		// command bypasses functions; for now just route to builtin/external lookup.
		std::vector<std::string> rest(args.begin() + i, args.end());
		if (exec.isBuiltin(rest[0])) {
			std::vector<std::string> sub(rest.begin() + 1, rest.end());
			return exec.callBuiltin(rest[0], sub);
		}
		// External: invoke through a fresh SimpleCommand-like flow.
		// Easiest path: use eval to re-tokenize the joined command. (Quoting limits apply.)
		std::string joined;
		for (std::size_t k = 0; k < rest.size(); ++k) {
			if (k) joined.push_back(' ');
			// Quote each arg defensively.
			joined.push_back('"');
			for (char c : rest[k]) {
				if (c == '"' || c == '\\' || c == '$' || c == '`') joined.push_back('\\');
				joined.push_back(c);
			}
			joined.push_back('"');
		}
		return exec.executeText(joined, "command");
	}

	// ---- read ----

	// Walk the leading `-r` / `-p prompt` / `--` options of a `read` invocation.
	// Returns the index of the first non-option arg (the variable list).
	static std::size_t parseReadFlags(const std::vector<std::string>& args,
	                                  bool* out_raw, std::string* out_prompt,
	                                  std::string* out_array_name) {
		*out_raw = false;
		out_prompt->clear();
		out_array_name->clear();
		std::size_t i = 0;
		while (i < args.size() && !args[i].empty() && args[i][0] == '-') {
			const std::string& f = args[i];
			if (f == "--") { ++i; break; }
			if (f == "-r") { *out_raw = true; ++i; continue; }
			if (f == "-p") {
				if (i + 1 < args.size()) { *out_prompt = args[i + 1]; i += 2; continue; }
				++i;
				continue;
			}
			if (f == "-a") {
				// `read -a NAME` — every input field becomes one element of
				// the indexed array NAME, replacing any prior value.
				if (i + 1 < args.size()) {
					*out_array_name = args[i + 1];
					i += 2;
					continue;
				}
				++i;
				continue;
			}
			break;
		}
		return i;
	}

	// Read one line from stdin honouring `read`'s `\` escapes (unless raw).
	// Returns false if EOF was hit before any input — the caller should
	// then return 1 from the builtin.
	static bool readOneLineFromStdin(bool raw, std::string& line) {
		while (true) {
			const int c = std::fgetc(stdin);
			if (c == EOF) {
				return !line.empty();
			}
			if (c == '\n') return true;
			if (!raw && c == '\\') {
				const int n = std::fgetc(stdin);
				if (n == EOF) return true;
				if (n == '\n') continue;   // line continuation
				line.push_back(static_cast<char>(n));
				continue;
			}
			line.push_back(static_cast<char>(c));
		}
	}

	// Split `line` into fields per the rules in `read` / POSIX field
	// splitting: leading IFS whitespace is skipped; then alternating
	// non-IFS fields and IFS runs (where each non-whitespace IFS char is
	// at most one field separator).
	static std::vector<std::string> splitReadLine(const std::string& line,
	                                              const std::string& ifs) {
		auto is_ifs_ws = [&](char c) {
			return (c == ' ' || c == '\t' || c == '\n')
			       && ifs.find(c) != std::string::npos;
		};
		auto is_ifs = [&](char c) { return ifs.find(c) != std::string::npos; };

		std::vector<std::string> fields;
		std::size_t pos = 0;
		while (pos < line.size() && is_ifs_ws(line[pos])) ++pos;
		while (pos < line.size()) {
			std::string cur;
			while (pos < line.size() && !is_ifs(line[pos]))
				cur.push_back(line[pos++]);
			fields.push_back(std::move(cur));
			bool saw_nonws = false;
			while (pos < line.size() && is_ifs(line[pos])) {
				if (!is_ifs_ws(line[pos])) {
					if (saw_nonws) break;
					saw_nonws = true;
				}
				++pos;
			}
		}
		return fields;
	}

	// Assign `fields` into the listed variable `names`. The last named var
	// receives all remaining fields joined by a single space (matching
	// bash's read).
	static void assignReadFields(Environment& env,
	                             const std::vector<std::string>& names,
	                             const std::vector<std::string>& fields) {
		for (std::size_t k = 0; k < names.size(); ++k) {
			std::string val;
			if (k + 1 == names.size()) {
				for (std::size_t m = k; m < fields.size(); ++m) {
					if (m > k) val.push_back(' ');
					val += fields[m];
				}
			} else if (k < fields.size()) {
				val = fields[k];
			}
			env.set(names[k], val);
		}
	}

	static int builtin_read(Executor& exec, const std::vector<std::string>& args) {
		bool raw = false;
		std::string prompt;
		std::string array_name;
		const std::size_t i = parseReadFlags(args, &raw, &prompt, &array_name);

		if (!prompt.empty()) {
			std::fwrite(prompt.data(), 1, prompt.size(), stderr);
			std::fflush(stderr);
		}

		std::string line;
		if (!readOneLineFromStdin(raw, line)) return 1;

		std::string ifs = exec.env().get("IFS");
		if (ifs.empty()) ifs = " \t\n";

		auto fields = splitReadLine(line, ifs);

		// `-a NAME` short-circuits the per-name distribution: every field
		// becomes one element of the indexed array NAME.
		if (!array_name.empty()) {
			exec.env().setIndexedArrayFromList(array_name, std::move(fields));
			return 0;
		}

		std::vector<std::string> names(args.begin() + i, args.end());
		if (names.empty()) names.push_back("REPLY");
		assignReadFields(exec.env(), names, fields);
		return 0;
	}

	// ---- test / [ ----

	static bool fileStat(const std::string& path, struct stat& st) {
		return ::stat(path.c_str(), &st) == 0;
	}

	static int evalUnaryFileTest(char op, const std::string& raw_path, const PathConv& pc) {
		std::string path = pc.toWin32(raw_path);
		struct stat st {};
		bool ok = fileStat(path, st);
		switch (op) {
		case 'e': return ok ? 0 : 1;
		case 'f': return (ok && (st.st_mode & S_IFMT) == S_IFREG) ? 0 : 1;
		case 'd': return (ok && (st.st_mode & S_IFMT) == S_IFDIR) ? 0 : 1;
		case 's': return (ok && st.st_size > 0) ? 0 : 1;
		case 'r': return (ok && (st.st_mode & 0444)) ? 0 : 1;
		case 'w': return (ok && (st.st_mode & 0222)) ? 0 : 1;
		case 'x': return (ok && (st.st_mode & 0111)) ? 0 : 1;
		default:  return 2;
		}
	}

	static int evalTest(const std::vector<std::string>& a, const PathConv& pc) {
		if (a.empty()) return 1;

		// Leading ! negates the rest.
		if (a[0] == "!" && a.size() > 1) {
			std::vector<std::string> rest(a.begin() + 1, a.end());
			int r = evalTest(rest, pc);
			if (r == 2) return 2;
			return r == 0 ? 1 : 0;
		}

		auto evalOne = [&](const std::string& s) {
			return s.empty() ? 1 : 0;
		};
		auto strNum = [&](const std::string& s, bool& ok) -> long long {
			return toIntSafe(s, ok);
		};

		if (a.size() == 1) return evalOne(a[0]);

		if (a.size() == 2) {
			if (a[0].size() == 2 && a[0][0] == '-') {
				char op = a[0][1];
				if (op == 'z') return a[1].empty() ? 0 : 1;
				if (op == 'n') return a[1].empty() ? 1 : 0;
				return evalUnaryFileTest(op, a[1], pc);
			}
			return 2;
		}

		if (a.size() == 3) {
			const std::string& l = a[0]; const std::string& op = a[1]; const std::string& r = a[2];
			if (op == "=" || op == "==") return l == r ? 0 : 1;
			if (op == "!=")              return l != r ? 0 : 1;
			if (op == "<")               return l < r  ? 0 : 1;
			if (op == ">")               return l > r  ? 0 : 1;
			bool okL, okR;
			long long li = strNum(l, okL); long long ri = strNum(r, okR);
			if (okL && okR) {
				if (op == "-eq") return li == ri ? 0 : 1;
				if (op == "-ne") return li != ri ? 0 : 1;
				if (op == "-lt") return li <  ri ? 0 : 1;
				if (op == "-le") return li <= ri ? 0 : 1;
				if (op == "-gt") return li >  ri ? 0 : 1;
				if (op == "-ge") return li >= ri ? 0 : 1;
			}
			return 2;
		}
		return 2;
	}

	static int builtin_test(Executor& exec, const std::vector<std::string>& args) {
		return evalTest(args, exec.pathConv());
	}

	// Print one declare entry. Arrays render as `declare -a/A name=(...)`;
	// scalars as `declare [-attrs] name="value"`.
	static void printDeclareEntry(Executor& exec, const std::string& n) {
		if (auto* ia = exec.env().getIndexedArray(n)) {
			std::printf("declare -a %s=(", n.c_str());
			bool first = true;
			for (const auto& kv : *ia) {
				if (!first) std::printf(" ");
				std::printf("[%lld]=\"%s\"", kv.first, kv.second.c_str());
				first = false;
			}
			std::printf(")\n");
			return;
		}
		if (auto* aa = exec.env().getAssocArray(n)) {
			std::printf("declare -A %s=(", n.c_str());
			bool first = true;
			for (const auto& kv : *aa) {
				if (!first) std::printf(" ");
				std::printf("[\"%s\"]=\"%s\"",
					kv.first.c_str(), kv.second.c_str());
				first = false;
			}
			std::printf(")\n");
			return;
		}
		std::string attrs;
		if (exec.env().isExported(n)) attrs += "x";
		if (exec.env().isReadonly(n)) attrs += "r";
		if (attrs.empty()) attrs = "-";
		else attrs.insert(0, "-");
		std::printf("declare %s %s=\"%s\"\n",
			attrs.c_str(), n.c_str(), exec.env().get(n).c_str());
	}

	namespace declare_internal {
		struct DeclareFlags {
			bool x = false;   ///< -x: mark exported
			bool r = false;   ///< -r: mark readonly
			bool p = false;   ///< -p: print one entry
			bool a = false;   ///< -a: indexed array
			bool A = false;   ///< -A: associative array
		};
	}  // namespace declare_internal

	// Walk `args`, splitting them into a flag bundle and the trailing list of
	// `name` or `name=value` operands. Unknown letters in a `-XYZ` cluster
	// are silently ignored (matching bash's lenient declare).
	static void parseDeclareFlags(const std::vector<std::string>& args,
	                              declare_internal::DeclareFlags& f,
	                              std::vector<std::string>& names) {
		for (const auto& a : args) {
			if (a == "--") continue;
			if (!a.empty() && a[0] == '-') {
				for (std::size_t k = 1; k < a.size(); ++k) {
					switch (a[k]) {
					case 'x': f.x = true; break;
					case 'r': f.r = true; break;
					case 'p': f.p = true; break;
					case 'a': f.a = true; break;
					case 'A': f.A = true; break;
					default: break;
					}
				}
				continue;
			}
			names.push_back(a);
		}
	}

	// Bare `declare`: dump the entire variable + array tables (sorted).
	static int dumpAllDeclareEntries(Executor& exec) {
		std::vector<std::pair<std::string, std::string>> v(
			exec.env().vars().begin(), exec.env().vars().end());
		std::sort(v.begin(), v.end());
		for (const auto& kv : v) printDeclareEntry(exec, kv.first);

		std::vector<std::string> array_names;
		for (const auto& kv : exec.env().indexedArrays()) array_names.push_back(kv.first);
		for (const auto& kv : exec.env().assocArrays())   array_names.push_back(kv.first);
		std::sort(array_names.begin(), array_names.end());
		for (const auto& n : array_names) printDeclareEntry(exec, n);
		return 0;
	}

	// `declare -p name [name ...]`: print each named entry, or a "not
	// found" error per missing one. Returns 1 if any name was missing.
	static int printDeclareNamedEntries(Executor& exec,
	                                    const std::vector<std::string>& names) {
		int rc = 0;
		for (const auto& nv : names) {
			const auto eq = nv.find('=');
			const std::string n = (eq == std::string::npos) ? nv : nv.substr(0, eq);
			if (!exec.env().has(n)) {
				std::fprintf(stderr, "wbsh: declare: %s: not found\n", n.c_str());
				rc = 1;
				continue;
			}
			printDeclareEntry(exec, n);
		}
		return rc;
	}

	// Apply `declare -aArx ...` to a single name (with optional =value).
	static void applyDeclareToName(Executor& exec,
	                               const declare_internal::DeclareFlags& f,
	                               const std::string& nv) {
		const auto eq = nv.find('=');
		const std::string n = (eq == std::string::npos) ? nv : nv.substr(0, eq);

		if (f.A && eq == std::string::npos) {
			exec.env().declareAssocArray(n);
		} else if (f.a && eq == std::string::npos) {
			// `declare -a name` initialises an empty indexed array.
			exec.env().setIndexedArrayFromList(n, {});
		} else if (eq != std::string::npos) {
			exec.env().set(n, nv.substr(eq + 1));
		}

		if (f.x) exec.env().exportVar(n);
		if (f.r) exec.env().markReadonly(n);
	}

	static int builtin_declare(Executor& exec, const std::vector<std::string>& args) {
		declare_internal::DeclareFlags f;
		std::vector<std::string> names;
		parseDeclareFlags(args, f, names);

		if (names.empty()) return dumpAllDeclareEntries(exec);
		if (f.p)            return printDeclareNamedEntries(exec, names);

		for (const auto& nv : names) applyDeclareToName(exec, f, nv);
		return 0;
	}

	struct ShoptFlag {
		const char* name;
		bool (Environment::*get)() const;
		void (Environment::*set)(bool);
	};
	const ShoptFlag* shoptTable() {
		static const ShoptFlag kFlags[] = {
			{ "nullglob",      &Environment::nullglob,      &Environment::setNullglob },
			{ "dotglob",       &Environment::dotglob,       &Environment::setDotglob },
			{ "extglob",       &Environment::extglob,       &Environment::setExtglob },
			{ "nocaseglob",    &Environment::nocaseglob,    &Environment::setNocaseglob },
			{ "nocasematch",   &Environment::nocasematch,   &Environment::setNocasematch },
			{ "globstar",      &Environment::globstar,      &Environment::setGlobstar },
			{ "lastpipe",      &Environment::lastpipe,      &Environment::setLastpipe },
			{ "huponexit",     &Environment::huponexit,     &Environment::setHuponexit },
			{ "expand_aliases",&Environment::expand_aliases,&Environment::setExpandAliases },
			{ "autocd",        &Environment::autocd,        &Environment::setAutocd },
			{ "checkwinsize",  &Environment::checkwinsize,  &Environment::setCheckwinsize },
			{ nullptr, nullptr, nullptr },
		};
		return kFlags;
	}

	namespace shopt_internal {
		enum class Mode { ListAll, Set, Unset, Query };
	}

	// Find an entry in the shopt option table by name. Returns null if the
	// name is not a known shopt option.
	static const ShoptFlag* findShoptFlag(const std::string& name) {
		for (const ShoptFlag* p = shoptTable(); p->name; ++p) {
			if (name == p->name) return p;
		}
		return nullptr;
	}

	static void printShoptFlag(const Environment& env, const ShoptFlag* f) {
		const bool on = (env.*(f->get))();
		std::printf("%-15s %s\n", f->name, on ? "on" : "off");
	}

	// Walk `args`, pulling out option flags into `mode` and pushing each
	// option name onto `names`. Returns false (with diagnostic) on an
	// unknown option spelling.
	static bool parseShoptArgs(const std::vector<std::string>& args,
	                           shopt_internal::Mode& mode,
	                           std::vector<std::string>& names) {
		using shopt_internal::Mode;
		for (const auto& a : args) {
			if (a == "-s") { mode = Mode::Set;   continue; }
			if (a == "-u") { mode = Mode::Unset; continue; }
			if (a == "-q") { mode = Mode::Query; continue; }
			if (a == "-p") { /* printable form: same as default list */ continue; }
			if (a == "--") continue;
			if (!a.empty() && a[0] == '-') {
				std::fprintf(stderr, "wbsh: shopt: unknown option: %s\n", a.c_str());
				return false;
			}
			names.push_back(a);
		}
		return true;
	}

	// `shopt -s name ...` / `shopt -u name ...`: toggle each flag, diagnose
	// invalid names. Returns 1 if any name was unknown.
	static int shoptSetOrUnset(Executor& exec,
	                           const std::vector<std::string>& names, bool on) {
		int rc = 0;
		for (const auto& n : names) {
			const ShoptFlag* f = findShoptFlag(n);
			if (!f) {
				std::fprintf(stderr, "wbsh: shopt: %s: invalid option name\n", n.c_str());
				rc = 1;
				continue;
			}
			(exec.env().*(f->set))(on);
		}
		return rc;
	}

	// `shopt -q name ...`: silently return 0 iff every named flag is set.
	static int shoptQuery(Executor& exec, const std::vector<std::string>& names) {
		for (const auto& n : names) {
			const ShoptFlag* f = findShoptFlag(n);
			if (!f) return 1;
			if (!(exec.env().*(f->get))()) return 1;
		}
		return 0;
	}

	// `shopt` / `shopt name ...`: print the current setting of all (or just
	// the listed) flags. Diagnoses invalid names with rc=1.
	static int shoptListMode(Executor& exec, const std::vector<std::string>& names) {
		if (names.empty()) {
			for (const ShoptFlag* p = shoptTable(); p->name; ++p) {
				printShoptFlag(exec.env(), p);
			}
			return 0;
		}
		int rc = 0;
		for (const auto& n : names) {
			const ShoptFlag* f = findShoptFlag(n);
			if (!f) {
				std::fprintf(stderr, "wbsh: shopt: %s: invalid option name\n", n.c_str());
				rc = 1;
				continue;
			}
			printShoptFlag(exec.env(), f);
		}
		return rc;
	}

	static int builtin_shopt(Executor& exec, const std::vector<std::string>& args) {
		shopt_internal::Mode mode = shopt_internal::Mode::ListAll;
		std::vector<std::string> names;
		if (!parseShoptArgs(args, mode, names)) return 1;

		switch (mode) {
		case shopt_internal::Mode::Set:     return shoptSetOrUnset(exec, names, true);
		case shopt_internal::Mode::Unset:   return shoptSetOrUnset(exec, names, false);
		case shopt_internal::Mode::Query:   return shoptQuery(exec, names);
		case shopt_internal::Mode::ListAll: return shoptListMode(exec, names);
		}
		return 0;
	}

	static int builtin_mapfile(Executor& exec, const std::vector<std::string>& args) {
		bool strip_newline = false;
		long long max_lines = -1;       // -n N: stop after N lines
		long long origin = 0;           // -O start: assign starting from index
		long long skip = 0;             // -s N: skip first N lines from input
		std::string array_name = "MAPFILE";
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& a = args[i];
			if (a == "-t") strip_newline = true;
			else if (a == "-n" && i + 1 < args.size()) {
				try { max_lines = std::stoll(args[++i]); } catch (...) {}
			}
			else if (a == "-O" && i + 1 < args.size()) {
				try { origin = std::stoll(args[++i]); } catch (...) {}
			}
			else if (a == "-s" && i + 1 < args.size()) {
				try { skip = std::stoll(args[++i]); } catch (...) {}
			}
			else if (a == "-u" && i + 1 < args.size()) {
				// File descriptor — we only support 0 (stdin) here.
				try { (void)std::stoi(args[++i]); } catch (...) {}
			}
			else if (!a.empty() && a[0] == '-' && a != "-" && a != "--") {
				std::fprintf(stderr, "wbsh: mapfile: unknown option: %s\n", a.c_str());
				return 1;
			}
			else array_name = a;
		}
		std::vector<std::string> lines;
		std::string buf;
		int c;
		long long skipped = 0;
		while ((c = std::fgetc(stdin)) != EOF) {
			if (c == '\n') {
				if (skipped < skip) { ++skipped; buf.clear(); continue; }
				if (!strip_newline) buf.push_back('\n');
				lines.push_back(std::move(buf));
				buf.clear();
				if (max_lines > 0 && (long long)lines.size() >= max_lines) break;
			} else {
				buf.push_back((char)c);
			}
		}
		if (!buf.empty()) {
			if (skipped < skip) { /* skip last partial too */ }
			else {
				if (max_lines <= 0 || (long long)lines.size() < max_lines)
					lines.push_back(std::move(buf));
			}
		}
		std::map<long long, std::string> elems;
		for (std::size_t i = 0; i < lines.size(); ++i) {
			elems[origin + (long long)i] = std::move(lines[i]);
		}
		exec.env().setIndexedArraySparse(array_name, std::move(elems));
		return 0;
	}

	static int builtin_readonly(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) {
			for (const auto& name : exec.env().readonlySet()) {
				std::printf("declare -r %s=\"%s\"\n",
					name.c_str(), exec.env().get(name).c_str());
			}
			return 0;
		}
		for (const auto& nv : args) {
			if (!nv.empty() && nv[0] == '-') continue;
			auto eq = nv.find('=');
			std::string n = (eq == std::string::npos) ? nv : nv.substr(0, eq);
			if (eq != std::string::npos) exec.env().set(n, nv.substr(eq + 1));
			exec.env().markReadonly(n);
		}
		return 0;
	}

	static int builtin_trap(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty() || (args.size() == 1 && args[0] == "-p")) {
			for (const auto& kv : exec.trapHandlers()) {
				std::printf("trap -- '%s' %s\n",
					kv.second.c_str(), kv.first.c_str());
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
			std::string sig = args[i];
			// Normalise: strip leading SIG and uppercase.
			if (sig.size() > 3 && (sig.compare(0, 3, "SIG") == 0
			    || sig.compare(0, 3, "sig") == 0)) {
				sig = sig.substr(3);
			}
			std::transform(sig.begin(), sig.end(), sig.begin(),
				[](char c) { return static_cast<char>(std::toupper((unsigned char)c)); });
			if (sig == "0") sig = "EXIT";
			if (action == "-") {
				exec.clearTrap(sig);
			} else {
				exec.setTrap(sig, action);
			}
		}
		return 0;
	}

	namespace getopts_internal {
		// Cursor state shared between the main loop and its helpers. `optind`
		// points one past the last consumed positional arg (1-based, like
		// bash's $OPTIND); `sub` is the index inside the current cluster
		// `-abc` of arg[optind-1].
		struct GetoptsCtx {
			Executor& exec;
			std::string opts;            ///< Spec without leading `:`.
			const std::string& name;     ///< Variable to assign the option char to.
			std::vector<std::string> source;
			bool silent;
			int& sub;
			int optind;

			void setName(const std::string& v) const { exec.env().set(name, v); }
			void storeOptind() const {
				exec.env().set("OPTIND", std::to_string(optind));
			}
		};

		// Read $OPTIND from the env and clamp to >= 1.
		static int loadOptind(Executor& exec) {
			int v = 1;
			try {
				const std::string s = exec.env().get("OPTIND");
				if (!s.empty()) v = std::stoi(s);
			} catch (...) {
				v = 1;
			}
			return v < 1 ? 1 : v;
		}

		// End-of-options markers: out of arguments, or a positional that
		// can't be an option (`-`, no leading `-`, or empty after `-`).
		// Resets state and returns 1, mirroring bash's getopts.
		static int finishWithoutOption(GetoptsCtx& ctx) {
			ctx.setName("?");
			ctx.exec.env().unset("OPTARG");
			ctx.exec.resetGetopts();
			ctx.storeOptind();
			return 1;
		}

		// Hit `--`: bump past it, reset, mark $? = 1 to terminate the
		// caller's getopts loop.
		static int finishOnDoubleDash(GetoptsCtx& ctx) {
			++ctx.optind;
			ctx.exec.resetGetopts();
			ctx.setName("?");
			ctx.storeOptind();
			return 1;
		}

		// Move past `cur[ctx.sub]`, advancing optind when the cluster is done.
		static void advanceOneOptionChar(GetoptsCtx& ctx, const std::string& cur) {
			++ctx.sub;
			if (ctx.sub >= static_cast<int>(cur.size())) {
				++ctx.optind;
				ctx.sub = 1;
			}
		}

		// Option char isn't in spec or is the placeholder `:`. Diagnose
		// (loud or silent) and return 0 with NAME set to "?".
		static int handleIllegalOption(GetoptsCtx& ctx,
		                               const std::string& cur, char opt) {
			if (ctx.silent) {
				ctx.setName("?");
				ctx.exec.env().set("OPTARG", std::string(1, opt));
			} else {
				std::fprintf(stderr, "getopts: illegal option -- %c\n", opt);
				ctx.setName("?");
				ctx.exec.env().unset("OPTARG");
			}
			advanceOneOptionChar(ctx, cur);
			ctx.storeOptind();
			return 0;
		}

		// Option `opt` takes an arg ("o:" in spec). Try to fetch it from
		// the rest of the current word, or from the next positional.
		// Returns 0 with NAME set to either the option char (success) or
		// `?` / `:` (missing-arg, depending on silent mode).
		static int handleOptionWithArg(GetoptsCtx& ctx,
		                               const std::string& cur, char opt) {
			std::string optarg;

			// Form 1: -ofoo — arg is glued to the option letter.
			if (ctx.sub + 1 < static_cast<int>(cur.size())) {
				optarg = cur.substr(ctx.sub + 1);
				++ctx.optind;
				ctx.sub = 1;
				ctx.setName(std::string(1, opt));
				ctx.exec.env().set("OPTARG", optarg);
				ctx.storeOptind();
				return 0;
			}

			// Form 2: -o foo — arg is the next positional.
			if (ctx.optind >= static_cast<int>(ctx.source.size())) {
				if (ctx.silent) {
					ctx.setName(":");
					ctx.exec.env().set("OPTARG", std::string(1, opt));
				} else {
					std::fprintf(stderr,
						"getopts: option requires an argument -- %c\n", opt);
					ctx.setName("?");
					ctx.exec.env().unset("OPTARG");
				}
				++ctx.optind;
				ctx.sub = 1;
				ctx.storeOptind();
				return 0;
			}
			optarg = ctx.source[ctx.optind];
			ctx.optind += 2;
			ctx.sub = 1;
			ctx.setName(std::string(1, opt));
			ctx.exec.env().set("OPTARG", optarg);
			ctx.storeOptind();
			return 0;
		}
	}  // namespace getopts_internal

	static int builtin_getopts(Executor& exec, const std::vector<std::string>& args) {
		if (args.size() < 2) {
			printerr("getopts: usage: getopts OPTSTRING NAME [ARG ...]");
			return 2;
		}

		// First char `:` switches getopts into silent mode (errors via OPTARG
		// instead of printed diagnostics, plus the `:` exit-name signal).
		std::string opts = args[0];
		const bool silent = !opts.empty() && opts[0] == ':';
		if (silent) opts.erase(0, 1);

		// `getopts spec name args...` overrides the positional list; with
		// fewer args the shell's positionals are scanned.
		std::vector<std::string> source = (args.size() > 2)
			? std::vector<std::string>(args.begin() + 2, args.end())
			: exec.env().positional();

		int& sub = exec.getoptsSubindex();
		if (sub < 1) sub = 1;

		getopts_internal::GetoptsCtx ctx{
			exec, std::move(opts), args[1], std::move(source),
			silent, sub, getopts_internal::loadOptind(exec),
		};

		while (true) {
			if (ctx.optind > static_cast<int>(ctx.source.size()))
				return getopts_internal::finishWithoutOption(ctx);

			const std::string& cur = ctx.source[ctx.optind - 1];
			if (cur.size() < 2 || cur[0] != '-' || cur == "-")
				return getopts_internal::finishWithoutOption(ctx);
			if (cur == "--")
				return getopts_internal::finishOnDoubleDash(ctx);

			// Cluster done; advance to next positional and re-enter.
			if (ctx.sub >= static_cast<int>(cur.size())) {
				++ctx.optind;
				ctx.sub = 1;
				continue;
			}

			const char opt = cur[ctx.sub];
			const auto pos = ctx.opts.find(opt);
			if (pos == std::string::npos || opt == ':')
				return getopts_internal::handleIllegalOption(ctx, cur, opt);
			if (pos + 1 < ctx.opts.size() && ctx.opts[pos + 1] == ':')
				return getopts_internal::handleOptionWithArg(ctx, cur, opt);

			// Plain flag (no argument).
			ctx.setName(std::string(1, opt));
			ctx.exec.env().unset("OPTARG");
			getopts_internal::advanceOneOptionChar(ctx, cur);
			ctx.storeOptind();
			return 0;
		}
	}

	static int builtin_jobs(Executor& exec, const std::vector<std::string>&) {
		exec.reapJobs();
		for (auto& j : exec.jobsTable()) {
			std::printf("[%d]  %s   %s\n",
				j.id,
				j.running ? "Running" : "Done",
				j.cmd_text.empty() ? "<command>" : j.cmd_text.c_str());
		}
		return 0;
	}

	static int builtin_wait(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) {
			exec.waitForAllJobs();
			return 0;
		}
		int rc = 0;
		for (const auto& a : args) {
			int id = -1;
			try {
				if (!a.empty() && a[0] == '%') id = std::stoi(a.substr(1));
				else {
					long long pid = std::stoll(a);
					for (auto& j : exec.jobsTable()) {
						if (j.pid == pid) { id = j.id; break; }
					}
				}
			} catch (...) { rc = 1; continue; }
			if (id < 0) { rc = 1; continue; }
			int s = exec.waitForJob(id);
			if (s >= 0) rc = s;
		}
		return rc;
	}

	static int builtin_disown(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) { exec.jobsTable().clear(); return 0; }
		for (const auto& a : args) {
			int id = -1;
			try {
				if (!a.empty() && a[0] == '%') id = std::stoi(a.substr(1));
			} catch (...) { continue; }
			auto& v = exec.jobsTable();
			v.erase(std::remove_if(v.begin(), v.end(),
				[&](const Executor::Job& j) { return j.id == id; }), v.end());
		}
		return 0;
	}

	static int builtin_fg(Executor& exec, const std::vector<std::string>& args) {
		// We don't support stop/cont so fg simply waits for the job.
		exec.reapJobs();
		int id = -1;
		if (args.empty()) {
			for (auto it = exec.jobsTable().rbegin(); it != exec.jobsTable().rend(); ++it) {
				if (it->running) { id = it->id; break; }
			}
		} else {
			try {
				const std::string& a = args[0];
				if (!a.empty() && a[0] == '%') id = std::stoi(a.substr(1));
			} catch (...) {}
		}
		if (id < 0) { printerr("fg: no current job"); return 1; }
		int s = exec.waitForJob(id);
		return s < 0 ? 1 : s;
	}

	static int builtin_bg(Executor& exec, const std::vector<std::string>&) {
		// No stopped-job concept on Windows; bg is a no-op that lists.
		return builtin_jobs(exec, {});
	}

	static int builtin_alias(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) {
			std::vector<std::pair<std::string, std::string>> v(
				exec.aliases().begin(), exec.aliases().end());
			std::sort(v.begin(), v.end());
			for (const auto& kv : v) {
				std::printf("alias %s='%s'\n",
					kv.first.c_str(), kv.second.c_str());
			}
			return 0;
		}
		int rc = 0;
		for (const auto& a : args) {
			auto eq = a.find('=');
			if (eq == std::string::npos) {
				if (exec.isAlias(a)) {
					std::printf("alias %s='%s'\n",
						a.c_str(), exec.aliasValue(a).c_str());
				} else {
					std::fprintf(stderr, "wbsh: alias: %s: not found\n", a.c_str());
					rc = 1;
				}
			} else {
				std::string name = a.substr(0, eq);
				std::string value = a.substr(eq + 1);
				exec.setAlias(name, std::move(value));
			}
		}
		return rc;
	}

	static int builtin_unalias(Executor& exec, const std::vector<std::string>& args) {
		bool all = false;
		std::vector<std::string> names;
		for (const auto& a : args) {
			if (a == "-a") all = true;
			else if (!a.empty() && a[0] == '-') continue;
			else names.push_back(a);
		}
		if (all) {
			std::vector<std::string> all_names;
			for (const auto& kv : exec.aliases()) all_names.push_back(kv.first);
			for (const auto& n : all_names) exec.unsetAlias(n);
			return 0;
		}
		int rc = 0;
		for (const auto& n : names) {
			if (!exec.isAlias(n)) {
				std::fprintf(stderr, "wbsh: unalias: %s: not found\n", n.c_str());
				rc = 1;
				continue;
			}
			exec.unsetAlias(n);
		}
		return rc;
	}

	static int builtin_history(Executor& exec, const std::vector<std::string>& args) {
		auto histfilePath = [&]() {
			std::string p = exec.env().get("HISTFILE");
			if (p.empty()) {
				std::string home = exec.env().get("HOME");
				if (home.empty()) return std::string();
				p = home + "/.wbsh_history";
			}
			return exec.pathConv().toWin32(p);
		};
		if (!args.empty()) {
			if (args[0] == "-c") { exec.clearHistory(); return 0; }
			if (args[0] == "-w") {
				std::string p = histfilePath();
				if (p.empty() || !exec.saveHistoryToFile(p)) {
					printerr("history: cannot write history file");
					return 1;
				}
				return 0;
			}
			if (args[0] == "-r") {
				std::string p = histfilePath();
				if (p.empty() || !exec.loadHistoryFromFile(p)) {
					printerr("history: cannot read history file");
					return 1;
				}
				return 0;
			}
			if (args[0] == "-s") {
				// Push the remaining args as a single history entry,
				// matching bash. `history -s` with no args is a no-op.
				if (args.size() < 2) return 0;
				std::string entry = args[1];
				for (std::size_t i = 2; i < args.size(); ++i) {
					entry.push_back(' ');
					entry += args[i];
				}
				exec.addHistoryEntry(std::move(entry));
				return 0;
			}
		}
		long n = -1;
		if (!args.empty()) {
			bool ok; long long v = toIntSafe(args[0], ok);
			if (ok) n = static_cast<long>(v);
		}
		const auto& h = exec.history();
		std::size_t start = (n > 0 && static_cast<std::size_t>(n) < h.size())
			? h.size() - static_cast<std::size_t>(n) : 0;
		for (std::size_t i = start; i < h.size(); ++i) {
			std::printf("%5zu  %s\n", i + 1, h[i].c_str());
		}
		return 0;
	}

	// Test hook for the reverse-incremental-search matcher. Wraps the pure
	// `findReverseSearchMatch` helper so a shell script can drive it
	// without needing a real TTY. Usage:
	//
	//     __revsearch [-c N] [-f] QUERY
	//
	// `-c N`   start searching from 1-based history index N (default: the
	//          last entry, matching the initial state of Ctrl-R).
	// `-f`     scan toward newer entries (default: backward toward older).
	//
	// Prints "INDEX MATCHED_LINE" on success (1-based index, to align with
	// the user-visible `history` output) or "no match" otherwise. Exit
	// status 0 on match, 1 on no-match, 2 on bad usage.
	static int builtin_revsearch(Executor& exec, const std::vector<std::string>& args) {
		bool forward = false;
		long long start_one_based = -1;
		std::string query;
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& a = args[i];
			if (a == "-f") { forward = true; continue; }
			if (a == "-c") {
				if (i + 1 >= args.size()) {
					printerr("__revsearch: -c requires an argument");
					return 2;
				}
				bool ok = false;
				start_one_based = toIntSafe(args[++i], ok);
				if (!ok || start_one_based < 1) {
					printerr("__revsearch: -c expects a positive integer");
					return 2;
				}
				continue;
			}
			query = a;
		}
		const auto& history = exec.history();
		std::size_t start = history.empty()
			? 0
			: (start_one_based < 0
				? history.size() - 1
				: static_cast<std::size_t>(start_one_based) - 1);
		std::size_t idx = findReverseSearchMatch(history, query, start, forward);
		if (idx >= history.size()) {
			std::printf("no match\n");
			return 1;
		}
		std::printf("%zu %s\n", idx + 1, history[idx].c_str());
		return 0;
	}

	// Append all PATH-resolvable executables to `out`. Paths are walked
	// in order; duplicates dropped.
	static void collectCommandsFromPath(Executor& exec, std::vector<std::string>& out) {
		std::string path = exec.env().get("PATH");
		if (path.empty()) return;
		std::set<std::string> seen;
		std::size_t i = 0;
		while (i <= path.size()) {
			auto sep = path.find(':', i);
			std::string dir = path.substr(i, sep == std::string::npos
				? std::string::npos : sep - i);
			i = (sep == std::string::npos) ? path.size() + 1 : sep + 1;
			if (dir.empty()) continue;
			std::filesystem::path win = utf8ToPath(exec.pathConv().toWin32(dir));
			std::error_code ec;
			std::filesystem::directory_iterator it(win, ec);
			if (ec) continue;
			for (auto& e : it) {
				std::string n;
				try { n = pathToUtf8(e.path().filename()); }
				catch (...) { continue; }
				// Strip .exe / .cmd / .bat for nicer suggestions.
				auto ends_with = [&](const std::string& suf) {
					if (n.size() < suf.size()) return false;
					std::string tail = n.substr(n.size() - suf.size());
					for (auto& c : tail) c = (char)std::tolower((unsigned char)c);
					return tail == suf;
				};
				std::string base = n;
				for (const char* s : { ".exe", ".cmd", ".bat", ".com" }) {
					if (ends_with(s)) {
						base = n.substr(0, n.size() - std::strlen(s));
						break;
					}
				}
				if (seen.insert(base).second) out.push_back(base);
			}
		}
	}

	// Apply prefix filtering to a list of candidates.
	static std::vector<std::string> filterByPrefix(const std::vector<std::string>& v,
	                                        const std::string& prefix) {
		std::vector<std::string> out;
		for (const auto& s : v) {
			if (s.compare(0, prefix.size(), prefix) == 0)
				out.push_back(s);
		}
		return out;
	}

	namespace compgen_internal {
		// Parsed `compgen` / `complete` flag set. Built up by parseCompgenFlags.
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
	}  // namespace compgen_internal

	// Split a `-W "word1 word2 ..."` argument on whitespace.
	static std::vector<std::string> splitDashWWordList(const std::string& s) {
		std::vector<std::string> out;
		std::string cur;
		for (char c : s) {
			if (c == ' ' || c == '\t' || c == '\n') {
				if (!cur.empty()) { out.push_back(std::move(cur)); cur.clear(); }
			} else {
				cur.push_back(c);
			}
		}
		if (!cur.empty()) out.push_back(std::move(cur));
		return out;
	}

	// Parse the option flags accepted by compgen / complete and capture the
	// trailing positional arg (the prefix) into `f`. Unknown options are
	// silently ignored, matching bash for forward compatibility.
	static void parseCompgenFlags(const std::vector<std::string>& args,
	                              compgen_internal::CompgenFlags& f) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& a = args[i];
			if (a == "-W" && i + 1 < args.size()) {
				f.wordlist = splitDashWWordList(args[++i]);
				continue;
			}
			if (a == "-A" && i + 1 < args.size()) { f.action = args[++i]; continue; }
			if (a == "-f") { f.include_files    = true; continue; }
			if (a == "-d") { f.include_dirs     = true; continue; }
			if (a == "-c") { f.include_cmds     = true; continue; }
			if (a == "-b") { f.include_builtins = true; continue; }
			if (a == "-a") { f.include_aliases  = true; continue; }
			if (a == "-v") { f.include_vars     = true; continue; }
			if (a == "-k") { f.include_keywords = true; continue; }
			// user / group / service / exported — not meaningful on Windows.
			if (a == "-u" || a == "-g" || a == "-s" || a == "-e") continue;
			// Options we recognise but don't act on at this layer.
			if (a == "-o" && i + 1 < args.size()) { ++i; continue; }
			if (a == "-F" && i + 1 < args.size()) { ++i; continue; }
			if (a == "-C" && i + 1 < args.size()) { ++i; continue; }
			if (!a.empty() && a[0] == '-' && a != "-" && a != "--") continue;
			if (a == "--") continue;
			f.prefix = a;
		}

		// `-A <name>` aliases for the boolean flags.
		if (f.action == "function")  f.include_funcs    = true;
		if (f.action == "variable")  f.include_vars     = true;
		if (f.action == "alias")     f.include_aliases  = true;
		if (f.action == "builtin")   f.include_builtins = true;
		if (f.action == "command")   f.include_cmds     = true;
		if (f.action == "file")      f.include_files    = true;
		if (f.action == "directory") f.include_dirs     = true;
	}

	// Append every prefix-matching item from `src` (sorted) to `out`.
	static void appendSortedFiltered(std::vector<std::string>& out,
	                                 std::vector<std::string> src,
	                                 const std::string& prefix) {
		std::sort(src.begin(), src.end());
		for (auto& n : filterByPrefix(src, prefix)) out.push_back(std::move(n));
	}

	// Append the (sorted, deduplicated) names of all `-c command` candidates
	// — builtins + functions + every executable on PATH — that begin with
	// `prefix`.
	static void appendCommandCandidates(std::vector<std::string>& out,
	                                    Executor& exec, const std::string& prefix) {
		std::vector<std::string> names = exec.builtinNames();
		auto fns = exec.functionNames();
		names.insert(names.end(), fns.begin(), fns.end());
		collectCommandsFromPath(exec, names);
		std::sort(names.begin(), names.end());
		names.erase(std::unique(names.begin(), names.end()), names.end());
		for (auto& n : filterByPrefix(names, prefix)) out.push_back(std::move(n));
	}

	// Append shell-keyword candidates matching `prefix`.
	static void appendKeywordCandidates(std::vector<std::string>& out,
	                                    const std::string& prefix) {
		static const char* kw[] = {
			"if","then","else","elif","fi","case","esac","for",
			"while","until","do","done","function","in","select",
			"time","[[","]]","return","break","continue", nullptr };
		std::vector<std::string> v;
		for (int k = 0; kw[k]; ++k) v.push_back(kw[k]);
		for (auto& n : filterByPrefix(v, prefix)) out.push_back(std::move(n));
	}

	// Append filename / directory candidates whose basename matches the
	// `prefix` argument's leaf component. Directories are suffixed with `/`.
	static void appendFileDirCandidates(std::vector<std::string>& out, Executor& exec,
	                                    const std::string& prefix,
	                                    bool include_files, bool include_dirs) {
		namespace fs = std::filesystem;
		std::string dir = ".";
		std::string leaf = prefix;
		const auto sl = prefix.find_last_of('/');
		if (sl != std::string::npos) {
			dir  = prefix.substr(0, sl);
			leaf = prefix.substr(sl + 1);
			if (dir.empty()) dir = "/";
		}
		std::error_code ec;
		fs::path list_dir = utf8ToPath(exec.pathConv().toWin32(dir));
		fs::directory_iterator it(list_dir, ec);
		if (ec) return;

		for (auto& e : it) {
			std::string n;
			try { n = pathToUtf8(e.path().filename()); }
			catch (...) { continue; }
			if (n.empty() || n[0] == '.') continue;
			if (n.compare(0, leaf.size(), leaf) != 0) continue;
			const bool isdir = e.is_directory(ec);
			if (include_dirs && !isdir && !include_files) continue;
			std::string full = (dir == ".")
				? n
				: (dir == "/" ? "/" + n : dir + "/" + n);
			if (isdir) full.push_back('/');
			out.push_back(std::move(full));
		}
	}

	// Shared candidate generator used by compgen and (later) the line
	// editor's programmable-completion path. Reads -A action / -W words /
	// -f / -d / -c flags from `args` and produces the prefix-filtered
	// candidate list.
	static std::vector<std::string> generateCompletions(
			Executor& exec,
			const std::vector<std::string>& args,
			/*out*/ std::string& prefix_out)
	{
		compgen_internal::CompgenFlags f;
		parseCompgenFlags(args, f);

		std::vector<std::string> out;

		if (!f.wordlist.empty()) {
			for (auto& w : filterByPrefix(f.wordlist, f.prefix))
				out.push_back(std::move(w));
		}
		if (f.include_funcs)    appendSortedFiltered(out, exec.functionNames(),  f.prefix);
		if (f.include_builtins) appendSortedFiltered(out, exec.builtinNames(),   f.prefix);
		if (f.include_aliases) {
			std::vector<std::string> names;
			for (const auto& kv : exec.aliases()) names.push_back(kv.first);
			appendSortedFiltered(out, std::move(names), f.prefix);
		}
		if (f.include_vars) {
			std::vector<std::string> names;
			for (const auto& kv : exec.env().vars()) names.push_back(kv.first);
			appendSortedFiltered(out, std::move(names), f.prefix);
		}
		if (f.include_cmds)     appendCommandCandidates(out, exec, f.prefix);
		if (f.include_keywords) appendKeywordCandidates(out, f.prefix);
		if (f.include_files || f.include_dirs) {
			appendFileDirCandidates(out, exec, f.prefix,
				f.include_files, f.include_dirs);
		}

		prefix_out = f.prefix;
		return out;
	}

	static int builtin_compgen(Executor& exec, const std::vector<std::string>& args) {
		std::string prefix;
		auto cands = generateCompletions(exec, args, prefix);
		for (const auto& s : cands) std::printf("%s\n", s.c_str());
		return cands.empty() ? 1 : 0;
	}

	namespace complete_internal {
		struct CompleteOptions {
			Executor::CompletionSpec spec;
			std::vector<std::string> commands;
			bool remove_mode = false;
			bool print_mode = false;
			bool default_complete = false;
		};
	}  // namespace complete_internal

	// Walk `args`, populating a CompletionSpec, the command-name list, and
	// mode flags. Unrecognised `-X` options are silently accepted to stay
	// forward-compatible with bash extensions.
	static void parseCompleteArgs(const std::vector<std::string>& args,
	                              complete_internal::CompleteOptions& o) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& a = args[i];
			if (a == "-r") { o.remove_mode = true; continue; }
			if (a == "-p") { o.print_mode  = true; continue; }
			if (a == "-D") { o.default_complete = true; continue; }
			if (a == "-W" && i + 1 < args.size()) {
				o.spec.words = splitDashWWordList(args[++i]);
				continue;
			}
			if (a == "-F" && i + 1 < args.size()) { o.spec.function = args[++i]; continue; }
			if (a == "-C" && i + 1 < args.size()) { o.spec.command  = args[++i]; continue; }
			if (a == "-f") { o.spec.include_files = true; continue; }
			if (a == "-d") { o.spec.include_dirs  = true; continue; }
			if (a == "-o" && i + 1 < args.size()) {
				const std::string& opt = args[++i];
				if      (opt == "default")  o.spec.default_fallback = true;
				else if (opt == "plusdirs") o.spec.plusdirs = true;
				else if (opt == "nospace")  o.spec.nospace = true;
				continue;
			}
			if (!a.empty() && a[0] == '-' && a != "-" && a != "--") continue;
			if (a == "--") continue;
			o.commands.push_back(a);
		}
	}

	// `complete -p [name ...]`: print one `complete <name>` line per
	// command with a registered spec. Returns 1 if any named command had
	// no spec.
	static int printCompleteSpecs(Executor& exec,
	                              const std::vector<std::string>& commands) {
		const auto& specs = exec.completionSpecs();
		if (commands.empty()) {
			for (const auto& kv : specs) {
				std::printf("complete %s\n", kv.first.c_str());
			}
			return 0;
		}
		int rc = 0;
		for (const auto& c : commands) {
			if (specs.count(c) == 0) {
				std::fprintf(stderr,
					"wbsh: complete: %s: no completion specification\n",
					c.c_str());
				rc = 1;
				continue;
			}
			std::printf("complete %s\n", c.c_str());
		}
		return rc;
	}

	// `complete -r [name ...]`: clear all specs (no args), or just the
	// named ones.
	static int clearCompleteSpecs(Executor& exec,
	                              const std::vector<std::string>& commands) {
		if (commands.empty()) {
			for (const auto& kv : exec.completionSpecs())
				exec.removeCompletionSpec(kv.first);
			return 0;
		}
		for (const auto& c : commands) exec.removeCompletionSpec(c);
		return 0;
	}

	static int builtin_complete(Executor& exec, const std::vector<std::string>& args) {
		complete_internal::CompleteOptions o;
		parseCompleteArgs(args, o);
		(void)o.default_complete;   // not yet acted on; stored for future use

		if (o.print_mode)  return printCompleteSpecs(exec, o.commands);
		if (o.remove_mode) return clearCompleteSpecs(exec, o.commands);
		if (o.commands.empty()) return 0;

		for (const auto& c : o.commands) exec.setCompletionSpec(c, o.spec);
		return 0;
	}

	static int builtin_compopt(Executor&, const std::vector<std::string>& args) {
		// Minimal implementation: accept and ignore. We don't track an
		// "active completion" so options have no place to attach. This
		// is sufficient for scripts that toggle, e.g., -o nospace.
		(void)args;
		return 0;
	}

	static int builtin_let(Executor& exec, const std::vector<std::string>& args) {
		// `let EXPR ...`: evaluate each arithmetic expression. Exit
		// status is 0 if the last expression evaluates to non-zero,
		// else 1. Side-effects (assignments) propagate via $((...)).
		if (args.empty()) return 1;
		long long last = 0;
		for (const auto& e : args) {
			try { last = exec.expander().evalArith(e); }
			catch (...) { return 1; }
		}
		return last != 0 ? 0 : 1;
	}

	static int builtin_umask(Executor& exec, const std::vector<std::string>& args) {
		// Windows has no real umask. Track a stored value in env so
		// scripts that read/write umask see consistent values, and
		// best-effort apply it via _umask (limited mask support).
		if (args.empty() || (args.size() == 1 && args[0] == "-S")) {
			std::string cur = exec.env().get("_WBSH_UMASK");
			if (cur.empty()) cur = "0022";
			if (!args.empty() && args[0] == "-S") {
				int v = std::stoi(cur, nullptr, 8);
				auto bits = [&](int shift) {
					std::string s;
					s.push_back((v & (0400 >> shift)) ? '-' : 'r');
					s.push_back((v & (0200 >> shift)) ? '-' : 'w');
					s.push_back((v & (0100 >> shift)) ? '-' : 'x');
					return s;
				};
				std::printf("u=%s,g=%s,o=%s\n",
					bits(0).c_str(), bits(3).c_str(), bits(6).c_str());
			} else {
				std::printf("%s\n", cur.c_str());
			}
			return 0;
		}
		if (args.size() == 1 && !args[0].empty() && args[0][0] != '-') {
			int v = 0;
			try { v = std::stoi(args[0], nullptr, 8); } catch (...) { return 1; }
			char buf[16];
			std::snprintf(buf, sizeof(buf), "%04o", v & 0777);
			exec.env().set("_WBSH_UMASK", buf);
#ifdef _WIN32
			_umask(v & 0700);
#endif
			return 0;
		}
		return 1;
	}

	static int builtin_hash(Executor&, const std::vector<std::string>& args) {
		// We don't keep a PATH hash; the file system check on each
		// lookup is fast enough on Windows. Implement -r (clear) and
		// -l (list) as no-ops for compatibility, treating positional
		// args as remembered names (printed back without lookup).
		if (args.empty()) {
			std::printf("hash: no commands hashed\n");
			return 0;
		}
		for (const auto& a : args) {
			if (a == "-r" || a == "-l" || a == "-d") continue;
			if (a == "-p" || a == "-t") continue;   // ignore option val
		}
		return 0;
	}

	static int builtin_times(Executor&, const std::vector<std::string>&) {
#ifdef _WIN32
		FILETIME create, exitT, kernel, user;
		if (GetProcessTimes(GetCurrentProcess(), &create, &exitT, &kernel, &user)) {
			auto toSec = [](const FILETIME& f) {
				unsigned long long t =
					((unsigned long long)f.dwHighDateTime << 32) | f.dwLowDateTime;
				return t / 10000000.0;
			};
			double u = toSec(user);
			double k = toSec(kernel);
			int um = (int)(u / 60); double us = u - um * 60;
			int km = (int)(k / 60); double ks = k - km * 60;
			std::printf("%dm%.3fs %dm%.3fs\n", um, us, km, ks);
			std::printf("0m0.000s 0m0.000s\n");   // children CPU not tracked
		} else {
			std::printf("0m0.000s 0m0.000s\n");
			std::printf("0m0.000s 0m0.000s\n");
		}
#endif /* _WIN32 */
		return 0;
	}

	static int builtin_caller(Executor& exec, const std::vector<std::string>& args) {
		// Minimal: when called from a function, print current LINENO
		// and the script "shell name" ($0). Without args.
		(void)args;
		if (exec.funcDepth() == 0) return 1;
		std::printf("%d %s\n",
			exec.env().currentLineno(),
			exec.env().shellName().c_str());
		return 0;
	}

	static int builtin_help(Executor& exec, const std::vector<std::string>& args) {
		auto print_index = [&]() {
			std::printf("wbsh built-in commands:\n\n");
			std::vector<std::string> names = exec.builtinNames();
			std::sort(names.begin(), names.end());
			for (std::size_t i = 0; i < names.size(); ++i) {
				std::printf("  %-14s", names[i].c_str());
				if (i % 5 == 4) std::printf("\n");
			}
			if (names.size() % 5 != 0) std::printf("\n");
			std::printf("\nUse `help NAME` for more on a specific builtin.\n");
		};
		if (args.empty()) { print_index(); return 0; }
		int rc = 0;
		for (const auto& n : args) {
			if (!exec.isBuiltin(n)) {
				std::fprintf(stderr, "wbsh: help: no help topics match '%s'\n",
					n.c_str());
				rc = 1;
				continue;
			}
			std::printf("%s: %s — see bash(1) for full semantics\n",
				n.c_str(), n.c_str());
		}
		return rc;
	}

	static int builtin_local(Executor& exec, const std::vector<std::string>& args) {
		if (exec.funcDepth() == 0) {
			printerr("local: can only be used in a function");
			return 1;
		}
		for (const auto& a : args) {
			auto eq = a.find('=');
			std::string name = (eq == std::string::npos) ? a : a.substr(0, eq);
			std::string val  = (eq == std::string::npos) ? std::string() : a.substr(eq + 1);
			exec.declareLocal(name, val);
		}
		return 0;
	}

	static int builtin_lbracket(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty() || args.back() != "]") {
			printerr("[: missing closing `]'");
			return 2;
		}
		std::vector<std::string> body(args.begin(), args.end() - 1);
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

#include "executor.h"
#include "lexer.h"
#include "parser.h"

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

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <io.h>
#  include <fcntl.h>
#endif

namespace wbsh {

	namespace {

		// ---- Tiny helpers ----

		long long toIntSafe(const std::string& s, bool& ok) {
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

		void printerr(const std::string& msg) {
			std::fprintf(stderr, "wbsh: %s\n", msg.c_str());
		}

		// ---- Builtin: true/false/: ----

		int builtin_true (Executor&, const std::vector<std::string>&) { return 0; }
		int builtin_false(Executor&, const std::vector<std::string>&) { return 1; }
		int builtin_colon(Executor&, const std::vector<std::string>&) { return 0; }

		// ---- echo ----

		std::string interpretEcho(const std::string& s) {
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
					while (cnt < 2 && i + 1 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1]))) {
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

		int builtin_echo(Executor&, const std::vector<std::string>& args) {
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

		int builtin_printf(Executor&, const std::vector<std::string>& args) {
			if (args.empty()) {
				printerr("printf: missing format");
				return 2;
			}
			const std::string& fmt = args[0];
			std::size_t ai = 1;
			auto arg = [&](bool& consumed) -> std::string {
				consumed = true;
				if (ai < args.size()) return args[ai++];
				consumed = false;
				return {};
			};
			auto emit_once = [&]() {
				for (std::size_t i = 0; i < fmt.size(); ++i) {
					char c = fmt[i];
					if (c == '\\' && i + 1 < fmt.size()) {
						char nx = fmt[++i];
						switch (nx) {
						case 'a': std::fputc('\a', stdout); break;
						case 'b': std::fputc('\b', stdout); break;
						case 'e': std::fputc('\x1b', stdout); break;
						case 'f': std::fputc('\f', stdout); break;
						case 'n': std::fputc('\n', stdout); break;
						case 'r': std::fputc('\r', stdout); break;
						case 't': std::fputc('\t', stdout); break;
						case 'v': std::fputc('\v', stdout); break;
						case '\\': std::fputc('\\', stdout); break;
						default: std::fputc('\\', stdout); std::fputc(nx, stdout); break;
						}
						continue;
					}
					if (c != '%') { std::fputc(c, stdout); continue; }
					// Parse conversion spec.
					std::size_t spec_start = i;
					++i;
					std::string spec = "%";
					while (i < fmt.size() && std::strchr("-+ #0", fmt[i])) spec.push_back(fmt[i++]);
					while (i < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i]))) spec.push_back(fmt[i++]);
					if (i < fmt.size() && fmt[i] == '.') {
						spec.push_back(fmt[i++]);
						while (i < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i]))) spec.push_back(fmt[i++]);
					}
					if (i >= fmt.size()) {
						std::fwrite(fmt.data() + spec_start, 1, fmt.size() - spec_start, stdout);
						break;
					}
					char conv = fmt[i];
					spec.push_back(conv);
					bool consumed;
					std::string a = arg(consumed);
					char buf[64];
					switch (conv) {
					case 's': std::fprintf(stdout, spec.c_str(), a.c_str()); break;
					case 'd': case 'i': {
						bool ok; long long v = toIntSafe(a, ok);
						std::string s2 = spec; s2.pop_back(); s2 += "lld";
						std::fprintf(stdout, s2.c_str(), v);
						break;
					}
					case 'u': {
						bool ok; long long v = toIntSafe(a, ok);
						unsigned long long uv = (unsigned long long)v;
						std::string s2 = spec; s2.pop_back(); s2 += "llu";
						std::fprintf(stdout, s2.c_str(), uv);
						break;
					}
					case 'x': case 'X': case 'o': {
						bool ok; long long v = toIntSafe(a, ok);
						unsigned long long uv = (unsigned long long)v;
						std::string s2 = spec; s2.pop_back(); s2 += "ll";
						s2.push_back(conv);
						std::fprintf(stdout, s2.c_str(), uv);
						break;
					}
					case 'c': {
						char ch = a.empty() ? '\0' : a[0];
						std::fputc(ch, stdout);
						(void)buf;
						break;
					}
					case '%': std::fputc('%', stdout); break;
					default:
						std::fwrite(spec.data(), 1, spec.size(), stdout);
						break;
					}
				}
			};
			emit_once();
			while (ai < args.size()) {
				std::size_t before = ai;
				emit_once();
				if (ai == before) break;
			}
			std::fflush(stdout);
			return 0;
		}

		// ---- cd / pwd ----

		int builtin_pwd(Executor& exec, const std::vector<std::string>& args) {
			namespace fs = std::filesystem;
			bool win = false;
			for (const auto& a : args) {
				if (a == "-W") win = true;
				else if (a == "-P" || a == "-L") { /* accept silently */ }
			}
			std::error_code ec;
			auto p = fs::current_path(ec);
			std::string s = ec ? exec.env().get("PWD") : p.string();
			if (!win) s = exec.pathConv().toPosix(s);
			std::fwrite(s.data(), 1, s.size(), stdout);
			std::fputc('\n', stdout);
			std::fflush(stdout);
			return 0;
		}

		int builtin_cd(Executor& exec, const std::vector<std::string>& args) {
			namespace fs = std::filesystem;
			std::string target;
			if (args.empty()) target = exec.env().get("HOME");
			else if (args[0] == "-") {
				target = exec.env().get("OLDPWD");
				if (target.empty()) { printerr("cd: OLDPWD not set"); return 1; }
				std::printf("%s\n", exec.pathConv().toPosix(target).c_str());
			} else target = args[0];
			std::string win_target = exec.pathConv().toWin32(target);
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
				exec.env().set("PWD", exec.pathConv().toPosix(cwd.string()));
			}
			return 0;
		}

		// ---- exit / return / break / continue ----

		int builtin_exit(Executor& exec, const std::vector<std::string>& args) {
			int s = exec.lastStatus();
			if (!args.empty()) { bool ok; long long v = toIntSafe(args[0], ok); if (ok) s = static_cast<int>(v); }
			throw ShellExit{ s };
		}

		int builtin_return(Executor& exec, const std::vector<std::string>& args) {
			if (exec.funcDepth() == 0) {
				printerr("return: can only `return' from a function or sourced script");
				return 1;
			}
			int s = exec.lastStatus();
			if (!args.empty()) { bool ok; long long v = toIntSafe(args[0], ok); if (ok) s = static_cast<int>(v); }
			throw FunctionReturn{ s };
		}

		int builtin_break(Executor& exec, const std::vector<std::string>& args) {
			if (exec.loopDepth() == 0) {
				printerr("break: only meaningful in a `for', `while', or `until' loop");
				return 0;
			}
			int n = 1;
			if (!args.empty()) { bool ok; long long v = toIntSafe(args[0], ok); if (ok && v > 0) n = static_cast<int>(v); }
			throw LoopBreak{ n };
		}

		int builtin_continue(Executor& exec, const std::vector<std::string>& args) {
			if (exec.loopDepth() == 0) {
				printerr("continue: only meaningful in a `for', `while', or `until' loop");
				return 0;
			}
			int n = 1;
			if (!args.empty()) { bool ok; long long v = toIntSafe(args[0], ok); if (ok && v > 0) n = static_cast<int>(v); }
			throw LoopContinue{ n };
		}

		// ---- export / unset / shift ----

		int builtin_export(Executor& exec, const std::vector<std::string>& args) {
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

		int builtin_unset(Executor& exec, const std::vector<std::string>& args) {
			for (const auto& a : args) exec.env().unset(a);
			return 0;
		}

		int builtin_shift(Executor& exec, const std::vector<std::string>& args) {
			int n = 1;
			if (!args.empty()) { bool ok; long long v = toIntSafe(args[0], ok); if (ok) n = static_cast<int>(v); }
			auto pos = exec.env().positional();
			if (n < 0 || static_cast<std::size_t>(n) > pos.size()) return 1;
			pos.erase(pos.begin(), pos.begin() + n);
			exec.env().setPositional(std::move(pos));
			return 0;
		}

		int builtin_set(Executor& exec, const std::vector<std::string>& args) {
			if (args.empty()) {
				std::vector<std::pair<std::string, std::string>> v(
					exec.env().vars().begin(), exec.env().vars().end());
				std::sort(v.begin(), v.end());
				for (const auto& kv : v) std::printf("%s=%s\n", kv.first.c_str(), kv.second.c_str());
				return 0;
			}
			auto applyShort = [&](char ch, bool on) {
				switch (ch) {
				case 'e': exec.env().setErrexit(on); break;
				case 'u': exec.env().setNounset(on); break;
				case 'x': exec.env().setXtrace(on);  break;
				case 'f': exec.env().setNoglob(on);  break;
				default: break;
				}
			};
			auto applyLong = [&](const std::string& name, bool on) -> bool {
				if (name == "errexit")       exec.env().setErrexit(on);
				else if (name == "nounset")  exec.env().setNounset(on);
				else if (name == "xtrace")   exec.env().setXtrace(on);
				else if (name == "noglob")   exec.env().setNoglob(on);
				else if (name == "pipefail") exec.env().setPipefail(on);
				else return false;
				return true;
			};
			std::size_t i = 0;
			bool consumed_flags = false;
			while (i < args.size()) {
				const std::string& a = args[i];
				if (a == "--") { ++i; consumed_flags = true; break; }
				if (a == "-")  { ++i; consumed_flags = true; break; }
				if (!a.empty() && (a[0] == '-' || a[0] == '+')) {
					bool on = (a[0] == '-');
					if (a.size() > 1 && a[1] == 'o') {
						if (i + 1 < args.size()) {
							if (!applyLong(args[i + 1], on)) {
								std::fprintf(stderr, "wbsh: set: unknown option: %s\n",
									args[i + 1].c_str());
								return 2;
							}
							i += 2;
							consumed_flags = true;
							continue;
						}
						++i;
						consumed_flags = true;
						continue;
					}
					for (std::size_t k = 1; k < a.size(); ++k) applyShort(a[k], on);
					++i;
					consumed_flags = true;
					continue;
				}
				break;
			}
			// Positional arguments only set when explicit args remain after flags.
			if (i < args.size() || !consumed_flags) {
				std::vector<std::string> pos(args.begin() + i, args.end());
				exec.env().setPositional(std::move(pos));
			}
			return 0;
		}

		// ---- eval / source ----

		int builtin_exec(Executor& exec, const std::vector<std::string>& args) {
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

		int builtin_eval(Executor& exec, const std::vector<std::string>& args) {
			std::string joined;
			for (std::size_t i = 0; i < args.size(); ++i) {
				if (i) joined.push_back(' ');
				joined += args[i];
			}
			if (joined.empty()) return 0;
			return exec.executeText(joined, "eval");
		}

		int builtin_source(Executor& exec, const std::vector<std::string>& args) {
			if (args.empty()) {
				printerr("source: filename required");
				return 2;
			}
			std::string p = exec.pathConv().toWin32(args[0]);
			std::ifstream f(p, std::ios::binary);
			if (!f) {
				std::fprintf(stderr, "wbsh: source: %s: %s\n",
					args[0].c_str(), std::strerror(errno));
				return 1;
			}
			std::stringstream ss;
			ss << f.rdbuf();
			// If extra args provided, set them as positional during the source.
			auto saved = exec.env().positional();
			if (args.size() > 1) {
				exec.env().setPositional({ args.begin() + 1, args.end() });
			}
			int r = 0;
			try { r = exec.executeText(ss.str(), args[0]); }
			catch (FunctionReturn& fr) { r = fr.status; }   // `return' from sourced top-level
			exec.env().setPositional(std::move(saved));
			return r;
		}

		// ---- type / command ----

		int builtin_type(Executor& exec, const std::vector<std::string>& args) {
			int rc = 0;
			for (const auto& a : args) {
				if (exec.isFunction(a)) {
					std::printf("%s is a function\n", a.c_str());
				} else if (exec.isBuiltin(a)) {
					std::printf("%s is a shell builtin\n", a.c_str());
				} else {
					// path lookup — duplicate the executor's logic loosely
					std::string path = exec.env().get("PATH");
					namespace fs = std::filesystem;
					std::vector<std::string> dirs;
					std::string cur;
#ifdef _WIN32
					const char sep = ';';
#else
					const char sep = ':';
#endif
					for (char c : path) { if (c == sep) { dirs.push_back(cur); cur.clear(); } else cur.push_back(c); }
					if (!cur.empty()) dirs.push_back(cur);
					std::string found;
					for (const auto& d : dirs) {
						if (d.empty()) continue;
						fs::path p(d); p /= a;
						std::error_code ec;
						if (fs::exists(p, ec) && !fs::is_directory(p, ec)) { found = p.string(); break; }
#ifdef _WIN32
						for (const char* e : { ".exe", ".cmd", ".bat" }) {
							fs::path q = p; q += e;
							if (fs::exists(q, ec) && !fs::is_directory(q, ec)) { found = q.string(); break; }
						}
						if (!found.empty()) break;
#endif
					}
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

		int builtin_command(Executor& exec, const std::vector<std::string>& args) {
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

		int builtin_read(Executor& exec, const std::vector<std::string>& args) {
			bool raw = false;
			std::string prompt;
			std::size_t i = 0;
			while (i < args.size() && !args[i].empty() && args[i][0] == '-') {
				const std::string& f = args[i];
				if (f == "--") { ++i; break; }
				if (f == "-r") { raw = true; ++i; continue; }
				if (f == "-p") {
					if (i + 1 < args.size()) { prompt = args[i + 1]; i += 2; continue; }
					++i; continue;
				}
				break;
			}
			if (!prompt.empty()) {
				std::fwrite(prompt.data(), 1, prompt.size(), stderr);
				std::fflush(stderr);
			}
			std::string line;
			while (true) {
				int c = std::fgetc(stdin);
				if (c == EOF) {
					if (line.empty()) return 1;
					break;
				}
				if (c == '\n') break;
				if (!raw && c == '\\') {
					int n = std::fgetc(stdin);
					if (n == EOF) break;
					if (n == '\n') continue;   // line continuation
					line.push_back(static_cast<char>(n));
					continue;
				}
				line.push_back(static_cast<char>(c));
			}

			std::vector<std::string> names(args.begin() + i, args.end());
			if (names.empty()) names.push_back("REPLY");

			std::string ifs = exec.env().get("IFS");
			if (ifs.empty()) ifs = " \t\n";
			auto isIfsWS = [&](char c) {
				return (c == ' ' || c == '\t' || c == '\n') && ifs.find(c) != std::string::npos;
			};
			auto isIfs = [&](char c) { return ifs.find(c) != std::string::npos; };

			std::vector<std::string> fields;
			std::size_t pos = 0;
			while (pos < line.size() && isIfsWS(line[pos])) ++pos;
			while (pos < line.size()) {
				std::string cur;
				while (pos < line.size() && !isIfs(line[pos])) cur.push_back(line[pos++]);
				fields.push_back(std::move(cur));
				bool saw_nonws = false;
				while (pos < line.size() && isIfs(line[pos])) {
					if (!isIfsWS(line[pos])) { if (saw_nonws) break; saw_nonws = true; }
					++pos;
				}
			}

			for (std::size_t k = 0; k < names.size(); ++k) {
				std::string val;
				if (k + 1 == names.size()) {
					for (std::size_t m = k; m < fields.size(); ++m) {
						if (m > k) val.push_back(' ');
						val += fields[m];
					}
				} else {
					if (k < fields.size()) val = fields[k];
				}
				exec.env().set(names[k], val);
			}
			return 0;
		}

		// ---- test / [ ----

		bool fileStat(const std::string& path, struct stat& st) {
			return ::stat(path.c_str(), &st) == 0;
		}

		int evalUnaryFileTest(char op, const std::string& raw_path, const PathConv& pc) {
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

		int evalTest(const std::vector<std::string>& a, const PathConv& pc) {
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

		int builtin_test(Executor& exec, const std::vector<std::string>& args) {
			return evalTest(args, exec.pathConv());
		}

		// Print one declare entry. Arrays render as `declare -a/A name=(...)`;
		// scalars as `declare [-attrs] name="value"`.
		void printDeclareEntry(Executor& exec, const std::string& n) {
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

		int builtin_declare(Executor& exec, const std::vector<std::string>& args) {
			bool flag_x = false, flag_r = false, flag_p = false;
			bool flag_a = false, flag_A = false;
			std::vector<std::string> names;
			for (const auto& a : args) {
				if (a == "--") continue;
				if (!a.empty() && a[0] == '-') {
					for (std::size_t k = 1; k < a.size(); ++k) {
						switch (a[k]) {
						case 'x': flag_x = true; break;
						case 'r': flag_r = true; break;
						case 'p': flag_p = true; break;
						case 'a': flag_a = true; break;
						case 'A': flag_A = true; break;
						default: break;
						}
					}
					continue;
				}
				names.push_back(a);
			}

			if (names.empty()) {
				std::vector<std::pair<std::string, std::string>> v(
					exec.env().vars().begin(), exec.env().vars().end());
				std::sort(v.begin(), v.end());
				for (const auto& kv : v) printDeclareEntry(exec, kv.first);
				std::vector<std::string> array_names;
				for (const auto& kv : exec.env().indexedArrays())
					array_names.push_back(kv.first);
				for (const auto& kv : exec.env().assocArrays())
					array_names.push_back(kv.first);
				std::sort(array_names.begin(), array_names.end());
				for (const auto& n : array_names) printDeclareEntry(exec, n);
				return 0;
			}
			if (flag_p) {
				int rc = 0;
				for (const auto& nv : names) {
					auto eq = nv.find('=');
					std::string n = (eq == std::string::npos) ? nv : nv.substr(0, eq);
					if (!exec.env().has(n)) {
						std::fprintf(stderr,
							"wbsh: declare: %s: not found\n", n.c_str());
						rc = 1;
						continue;
					}
					printDeclareEntry(exec, n);
				}
				return rc;
			}
			for (const auto& nv : names) {
				auto eq = nv.find('=');
				std::string n = (eq == std::string::npos) ? nv : nv.substr(0, eq);
				// `declare -A name` declares without assigning a value.
				if (flag_A && eq == std::string::npos) {
					exec.env().declareAssocArray(n);
				} else if (flag_a && eq == std::string::npos) {
					// `declare -a name` initialises an empty indexed array.
					exec.env().setIndexedArrayFromList(n, {});
				} else if (eq != std::string::npos) {
					exec.env().set(n, nv.substr(eq + 1));
				}
				if (flag_x) exec.env().exportVar(n);
				if (flag_r) exec.env().markReadonly(n);
			}
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

		int builtin_shopt(Executor& exec, const std::vector<std::string>& args) {
			enum class Mode { ListSet, ListUnset, ListAll, Set, Unset, Query };
			Mode mode = Mode::ListAll;
			bool printable = false;
			std::vector<std::string> names;
			for (const auto& a : args) {
				if (a == "-s") mode = Mode::Set;
				else if (a == "-u") mode = Mode::Unset;
				else if (a == "-q") mode = Mode::Query;
				else if (a == "-p") printable = true;
				else if (!a.empty() && a[0] == '-' && a != "--") {
					std::fprintf(stderr, "wbsh: shopt: unknown option: %s\n", a.c_str());
					return 1;
				}
				else if (a == "--") { /* end of opts */ }
				else names.push_back(a);
			}
			auto findFlag = [](const std::string& n) -> const ShoptFlag* {
				for (const ShoptFlag* p = shoptTable(); p->name; ++p) {
					if (n == p->name) return p;
				}
				return nullptr;
			};
			(void)printable;
			if (mode == Mode::Set || mode == Mode::Unset) {
				bool on = (mode == Mode::Set);
				int rc = 0;
				for (const auto& n : names) {
					auto* f = findFlag(n);
					if (!f) {
						std::fprintf(stderr, "wbsh: shopt: %s: invalid option name\n", n.c_str());
						rc = 1;
						continue;
					}
					(exec.env().*(f->set))(on);
				}
				return rc;
			}
			if (mode == Mode::Query) {
				for (const auto& n : names) {
					auto* f = findFlag(n);
					if (!f) return 1;
					if (!(exec.env().*(f->get))()) return 1;
				}
				return 0;
			}
			// List form. With names: print only those. Without names: all.
			auto print_one = [&](const ShoptFlag* f) {
				bool on = (exec.env().*(f->get))();
				std::printf("%-15s %s\n", f->name, on ? "on" : "off");
			};
			if (!names.empty()) {
				int rc = 0;
				for (const auto& n : names) {
					auto* f = findFlag(n);
					if (!f) {
						std::fprintf(stderr, "wbsh: shopt: %s: invalid option name\n", n.c_str());
						rc = 1;
						continue;
					}
					print_one(f);
				}
				return rc;
			}
			for (const ShoptFlag* p = shoptTable(); p->name; ++p) print_one(p);
			return 0;
		}

		int builtin_mapfile(Executor& exec, const std::vector<std::string>& args) {
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

		int builtin_readonly(Executor& exec, const std::vector<std::string>& args) {
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

		int builtin_trap(Executor& exec, const std::vector<std::string>& args) {
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

		int builtin_getopts(Executor& exec, const std::vector<std::string>& args) {
			if (args.size() < 2) {
				printerr("getopts: usage: getopts OPTSTRING NAME [ARG ...]");
				return 2;
			}
			std::string opts = args[0];
			const std::string& name = args[1];
			bool silent = !opts.empty() && opts[0] == ':';
			if (silent) opts.erase(0, 1);

			std::vector<std::string> source;
			if (args.size() > 2) {
				source.assign(args.begin() + 2, args.end());
			} else {
				source = exec.env().positional();
			}

			int optind = 1;
			try {
				std::string s = exec.env().get("OPTIND");
				if (!s.empty()) optind = std::stoi(s);
			} catch (...) {}
			if (optind < 1) optind = 1;
			int& sub = exec.getoptsSubindex();
			if (sub < 1) sub = 1;

			auto setName = [&](const std::string& v) { exec.env().set(name, v); };
			auto storeOptind = [&]() { exec.env().set("OPTIND", std::to_string(optind)); };

			while (true) {
				if (optind > static_cast<int>(source.size())) {
					setName("?");
					exec.env().unset("OPTARG");
					exec.resetGetopts();
					storeOptind();
					return 1;
				}
				const std::string& cur = source[optind - 1];
				if (cur.size() < 2 || cur[0] != '-' || cur == "-") {
					setName("?");
					exec.env().unset("OPTARG");
					exec.resetGetopts();
					storeOptind();
					return 1;
				}
				if (cur == "--") {
					++optind;
					exec.resetGetopts();
					setName("?");
					storeOptind();
					return 1;
				}
				if (sub >= static_cast<int>(cur.size())) {
					++optind;
					sub = 1;
					continue;
				}
				char opt = cur[sub];
				auto pos = opts.find(opt);
				if (pos == std::string::npos || opt == ':') {
					if (silent) {
						setName("?");
						exec.env().set("OPTARG", std::string(1, opt));
					} else {
						std::fprintf(stderr, "getopts: illegal option -- %c\n", opt);
						setName("?");
						exec.env().unset("OPTARG");
					}
					++sub;
					if (sub >= static_cast<int>(cur.size())) { ++optind; sub = 1; }
					storeOptind();
					return 0;
				}
				if (pos + 1 < opts.size() && opts[pos + 1] == ':') {
					std::string optarg;
					if (sub + 1 < static_cast<int>(cur.size())) {
						optarg = cur.substr(sub + 1);
						++optind;
						sub = 1;
					} else {
						if (optind >= static_cast<int>(source.size())) {
							if (silent) {
								setName(":");
								exec.env().set("OPTARG", std::string(1, opt));
							} else {
								std::fprintf(stderr,
									"getopts: option requires an argument -- %c\n", opt);
								setName("?");
								exec.env().unset("OPTARG");
							}
							++optind;
							sub = 1;
							storeOptind();
							return 0;
						}
						optarg = source[optind];
						optind += 2;
						sub = 1;
					}
					setName(std::string(1, opt));
					exec.env().set("OPTARG", optarg);
					storeOptind();
					return 0;
				}
				setName(std::string(1, opt));
				exec.env().unset("OPTARG");
				++sub;
				if (sub >= static_cast<int>(cur.size())) { ++optind; sub = 1; }
				storeOptind();
				return 0;
			}
		}

		int builtin_jobs(Executor& exec, const std::vector<std::string>&) {
			exec.reapJobs();
			for (auto& j : exec.jobsTable()) {
				std::printf("[%d]  %s   %s\n",
					j.id,
					j.running ? "Running" : "Done",
					j.cmd_text.empty() ? "<command>" : j.cmd_text.c_str());
			}
			return 0;
		}

		int builtin_wait(Executor& exec, const std::vector<std::string>& args) {
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

		int builtin_disown(Executor& exec, const std::vector<std::string>& args) {
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

		int builtin_fg(Executor& exec, const std::vector<std::string>& args) {
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

		int builtin_bg(Executor& exec, const std::vector<std::string>&) {
			// No stopped-job concept on Windows; bg is a no-op that lists.
			return builtin_jobs(exec, {});
		}

		int builtin_alias(Executor& exec, const std::vector<std::string>& args) {
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

		int builtin_unalias(Executor& exec, const std::vector<std::string>& args) {
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

		int builtin_history(Executor& exec, const std::vector<std::string>& args) {
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

		// Append all PATH-resolvable executables to `out`. Paths are walked
		// in order; duplicates dropped.
		void collectCommandsFromPath(Executor& exec, std::vector<std::string>& out) {
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
				std::string win = exec.pathConv().toWin32(dir);
				std::error_code ec;
				std::filesystem::directory_iterator it(win, ec);
				if (ec) continue;
				for (auto& e : it) {
					std::string n;
					try { n = e.path().filename().string(); }
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
		std::vector<std::string> filterByPrefix(const std::vector<std::string>& v,
		                                        const std::string& prefix) {
			std::vector<std::string> out;
			for (const auto& s : v) {
				if (s.compare(0, prefix.size(), prefix) == 0)
					out.push_back(s);
			}
			return out;
		}

		// Shared candidate generator used by compgen and (later) the line
		// editor's programmable-completion path. Reads -A action / -W words /
		// -f / -d / -c flags from `args` and produces the prefix-filtered
		// candidate list.
		std::vector<std::string> generateCompletions(
				Executor& exec,
				const std::vector<std::string>& args,
				/*out*/ std::string& prefix_out)
		{
			std::vector<std::string> out;
			std::string prefix;
			std::vector<std::string> wordlist;
			std::string action;
			bool include_files = false;
			bool include_dirs = false;
			bool include_cmds = false;
			bool include_builtins = false;
			bool include_funcs = false;
			bool include_aliases = false;
			bool include_vars = false;
			bool include_keywords = false;
			for (std::size_t i = 0; i < args.size(); ++i) {
				const std::string& a = args[i];
				if (a == "-W" && i + 1 < args.size()) {
					std::string s = args[++i];
					std::string cur;
					for (char c : s) {
						if (c == ' ' || c == '\t' || c == '\n') {
							if (!cur.empty()) { wordlist.push_back(std::move(cur)); cur.clear(); }
						} else cur.push_back(c);
					}
					if (!cur.empty()) wordlist.push_back(std::move(cur));
				}
				else if (a == "-A" && i + 1 < args.size()) action = args[++i];
				else if (a == "-f") include_files = true;
				else if (a == "-d") include_dirs = true;
				else if (a == "-c") include_cmds = true;
				else if (a == "-b") include_builtins = true;
				else if (a == "-a") include_aliases = true;
				else if (a == "-v") include_vars = true;
				else if (a == "-k") include_keywords = true;
				else if (a == "-u" || a == "-g" || a == "-s" || a == "-e") {
					/* user/group/service/exported — skip on Windows */
				}
				else if (a == "-o" && i + 1 < args.size()) ++i;  // ignore option
				else if (a == "-F" && i + 1 < args.size()) ++i;  // function: not invoked here
				else if (a == "-C" && i + 1 < args.size()) ++i;  // external cmd: not invoked here
				else if (!a.empty() && a[0] == '-' && a != "-" && a != "--") {
					// unknown option — ignore
				}
				else if (a == "--") { /* end of options */ }
				else prefix = a;
			}
			if (action == "function")  include_funcs = true;
			if (action == "variable")  include_vars = true;
			if (action == "alias")     include_aliases = true;
			if (action == "builtin")   include_builtins = true;
			if (action == "command")   include_cmds = true;
			if (action == "file")      include_files = true;
			if (action == "directory") include_dirs = true;

			if (!wordlist.empty()) {
				for (auto& w : filterByPrefix(wordlist, prefix)) out.push_back(w);
			}
			if (include_funcs) {
				auto names = exec.functionNames();
				std::sort(names.begin(), names.end());
				for (auto& n : filterByPrefix(names, prefix)) out.push_back(n);
			}
			if (include_builtins) {
				auto names = exec.builtinNames();
				std::sort(names.begin(), names.end());
				for (auto& n : filterByPrefix(names, prefix)) out.push_back(n);
			}
			if (include_aliases) {
				std::vector<std::string> names;
				for (const auto& kv : exec.aliases()) names.push_back(kv.first);
				std::sort(names.begin(), names.end());
				for (auto& n : filterByPrefix(names, prefix)) out.push_back(n);
			}
			if (include_vars) {
				std::vector<std::string> names;
				for (const auto& kv : exec.env().vars()) names.push_back(kv.first);
				std::sort(names.begin(), names.end());
				for (auto& n : filterByPrefix(names, prefix)) out.push_back(n);
			}
			if (include_cmds) {
				auto names = exec.builtinNames();
				auto fns = exec.functionNames();
				names.insert(names.end(), fns.begin(), fns.end());
				collectCommandsFromPath(exec, names);
				std::sort(names.begin(), names.end());
				names.erase(std::unique(names.begin(), names.end()), names.end());
				for (auto& n : filterByPrefix(names, prefix)) out.push_back(n);
			}
			if (include_keywords) {
				static const char* kw[] = {
					"if","then","else","elif","fi","case","esac","for",
					"while","until","do","done","function","in","select",
					"time","[[","]]","return","break","continue", nullptr };
				std::vector<std::string> v;
				for (int k = 0; kw[k]; ++k) v.push_back(kw[k]);
				for (auto& n : filterByPrefix(v, prefix)) out.push_back(n);
			}
			if (include_files || include_dirs) {
				namespace fs = std::filesystem;
				// Split prefix into dir + leaf.
				std::string dir = ".", leaf = prefix;
				auto sl = prefix.find_last_of('/');
				if (sl != std::string::npos) {
					dir = prefix.substr(0, sl);
					leaf = prefix.substr(sl + 1);
					if (dir.empty()) dir = "/";
				}
				std::error_code ec;
				std::string list_dir = exec.pathConv().toWin32(dir);
				fs::directory_iterator it(list_dir, ec);
				if (!ec) {
					for (auto& e : it) {
						std::string n;
						try { n = e.path().filename().string(); }
						catch (...) { continue; }
						if (n.empty() || n[0] == '.') continue;
						if (n.compare(0, leaf.size(), leaf) != 0) continue;
						bool isdir = e.is_directory(ec);
						if (include_dirs && !isdir && !include_files) continue;
						std::string full = (dir == ".") ? n
						    : (dir == "/" ? "/" + n : dir + "/" + n);
						if (isdir) full.push_back('/');
						out.push_back(full);
					}
				}
			}
			prefix_out = prefix;
			return out;
		}

		int builtin_compgen(Executor& exec, const std::vector<std::string>& args) {
			std::string prefix;
			auto cands = generateCompletions(exec, args, prefix);
			for (const auto& s : cands) std::printf("%s\n", s.c_str());
			return cands.empty() ? 1 : 0;
		}

		int builtin_complete(Executor& exec, const std::vector<std::string>& args) {
			Executor::CompletionSpec spec;
			std::vector<std::string> commands;
			bool remove_mode = false;
			bool print_mode = false;
			bool default_complete = false;
			(void)default_complete;
			for (std::size_t i = 0; i < args.size(); ++i) {
				const std::string& a = args[i];
				if (a == "-r") remove_mode = true;
				else if (a == "-p") print_mode = true;
				else if (a == "-D") default_complete = true;
				else if (a == "-W" && i + 1 < args.size()) {
					std::string s = args[++i];
					std::string cur;
					for (char c : s) {
						if (c == ' ' || c == '\t' || c == '\n') {
							if (!cur.empty()) { spec.words.push_back(std::move(cur)); cur.clear(); }
						} else cur.push_back(c);
					}
					if (!cur.empty()) spec.words.push_back(std::move(cur));
				}
				else if (a == "-F" && i + 1 < args.size()) spec.function = args[++i];
				else if (a == "-C" && i + 1 < args.size()) spec.command = args[++i];
				else if (a == "-f") spec.include_files = true;
				else if (a == "-d") spec.include_dirs = true;
				else if (a == "-o" && i + 1 < args.size()) {
					const std::string& opt = args[++i];
					if (opt == "default") spec.default_fallback = true;
					else if (opt == "plusdirs") spec.plusdirs = true;
					else if (opt == "nospace") spec.nospace = true;
				}
				else if (!a.empty() && a[0] == '-' && a != "-" && a != "--") {
					// other flags — accept silently
				}
				else if (a == "--") { /* end of opts */ }
				else commands.push_back(a);
			}
			if (print_mode) {
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
					} else {
						std::printf("complete %s\n", c.c_str());
					}
				}
				return rc;
			}
			if (remove_mode) {
				if (commands.empty()) {
					for (const auto& kv : exec.completionSpecs())
						exec.removeCompletionSpec(kv.first);
					return 0;
				}
				for (const auto& c : commands) exec.removeCompletionSpec(c);
				return 0;
			}
			if (commands.empty()) return 0;
			for (const auto& c : commands) {
				exec.setCompletionSpec(c, spec);
			}
			return 0;
		}

		int builtin_compopt(Executor&, const std::vector<std::string>& args) {
			// Minimal implementation: accept and ignore. We don't track an
			// "active completion" so options have no place to attach. This
			// is sufficient for scripts that toggle, e.g., -o nospace.
			(void)args;
			return 0;
		}

		int builtin_let(Executor& exec, const std::vector<std::string>& args) {
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

		int builtin_umask(Executor& exec, const std::vector<std::string>& args) {
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

		int builtin_hash(Executor&, const std::vector<std::string>& args) {
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

		int builtin_times(Executor&, const std::vector<std::string>&) {
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
#endif
			return 0;
		}

		int builtin_caller(Executor& exec, const std::vector<std::string>& args) {
			// Minimal: when called from a function, print current LINENO
			// and the script "shell name" ($0). Without args.
			(void)args;
			if (exec.funcDepth() == 0) return 1;
			std::printf("%d %s\n",
				exec.env().currentLineno(),
				exec.env().shellName().c_str());
			return 0;
		}

		int builtin_help(Executor& exec, const std::vector<std::string>& args) {
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

		int builtin_local(Executor& exec, const std::vector<std::string>& args) {
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

		int builtin_lbracket(Executor& exec, const std::vector<std::string>& args) {
			if (args.empty() || args.back() != "]") {
				printerr("[: missing closing `]'");
				return 2;
			}
			std::vector<std::string> body(args.begin(), args.end() - 1);
			return evalTest(body, exec.pathConv());
		}

	}  // namespace

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

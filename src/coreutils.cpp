// Native implementations of the most-used coreutils, so wbsh is functional
// out of the box without depending on Git Bash / MSYS / Cygwin binaries.
//
// Scope is intentionally pragmatic: enough flags to cover everyday
// interactive use and common scripts. Power users with full GNU coreutils
// installed can still call them by absolute path (or remove these by
// `unalias` / dropping them in a future flag).

#include "awk.h"
#include "executor.h"
#include "inflate.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <io.h>
#  include <bcrypt.h>
#  include <winhttp.h>
#  pragma comment(lib, "bcrypt.lib")
#  pragma comment(lib, "winhttp.lib")
#endif

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace wbsh {

	namespace {

		namespace fs = std::filesystem;

		void perr(const std::string& cmd, const std::string& path, const std::error_code& ec) {
			std::fprintf(stderr, "wbsh: %s: %s: %s\n",
				cmd.c_str(), path.c_str(), ec.message().c_str());
		}

		void perr(const std::string& cmd, const std::string& msg) {
			std::fprintf(stderr, "wbsh: %s: %s\n", cmd.c_str(), msg.c_str());
		}

		// Translate an arg-style path (POSIX-or-Win32, UTF-8) to a native
		// filesystem::path. On Windows we route through wide strings so the
		// UTF-8 bytes don't get mojibake'd through the active codepage by
		// the default `path(string)` constructor (`Tomáš` -> `TomÃ¡Å¡`).
		fs::path toNative(Executor& exec, const std::string& p) {
			return utf8ToPath(exec.pathConv().toWin32(p));
		}

		// fopen on a UTF-8, POSIX-or-Win32 path. Routes through `_wfopen`
		// on Windows so non-ASCII filenames survive intact.
		std::FILE* fopenNative(Executor& exec, const std::string& p, const char* mode) {
			return openUtf8(exec.pathConv().toWin32(p), mode);
		}

		// ---- ls --------------------------------------------------------------

		struct LsOpts {
			bool all = false;        // -a / -A
			bool long_fmt = false;   // -l
			bool one = false;        // -1
			bool human = false;      // -h (with -l)
			bool reverse = false;    // -r
			bool sort_mtime = false; // -t
			bool sort_size = false;  // -S
			bool classify = false;   // -F
			enum { Auto, Always, Never } color = Auto;
		};

		bool stdoutIsTty() {
#ifdef _WIN32
			return _isatty(_fileno(stdout)) != 0;
#else
			return false;
#endif
		}

		int consoleWidth() {
#ifdef _WIN32
			HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
			CONSOLE_SCREEN_BUFFER_INFO info{};
			if (h != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(h, &info)) {
				int w = info.srWindow.Right - info.srWindow.Left + 1;
				if (w > 0) return w;
			}
#endif
			return 80;
		}

		struct LsEntry {
			std::string name;
			fs::path full;
			fs::file_status status{};
			std::uintmax_t size = 0;
			fs::file_time_type mtime{};
			bool is_dir = false;
			bool is_symlink = false;
			bool is_executable = false;
			bool is_hidden = false;
			bool valid = false;
		};

		bool windowsHidden(const fs::path& p) {
#ifdef _WIN32
			DWORD attr = GetFileAttributesW(p.wstring().c_str());
			if (attr == INVALID_FILE_ATTRIBUTES) return false;
			return (attr & FILE_ATTRIBUTE_HIDDEN) != 0;
#else
			(void)p;
			return false;
#endif
		}

		LsEntry collect(const fs::path& parent, const std::string& name) {
			LsEntry e;
			e.name = name;
			fs::path npath = utf8ToPath(name);
			e.full = parent.empty() ? npath : (parent / npath);
			std::error_code ec;
			e.status = fs::symlink_status(e.full, ec);
			e.is_symlink = !ec && fs::is_symlink(e.status);
			fs::file_status real = e.is_symlink
				? fs::status(e.full, ec)
				: e.status;
			e.is_dir = !ec && fs::is_directory(real);
			if (!ec && fs::is_regular_file(e.status)) {
				e.size = fs::file_size(e.full, ec);
				if (ec) e.size = 0;
			}
			std::error_code ec2;
			e.mtime = fs::last_write_time(e.full, ec2);
			// Hidden: leading dot OR Windows hidden attribute.
			if (!name.empty() && name[0] == '.') e.is_hidden = true;
			if (windowsHidden(e.full)) e.is_hidden = true;
			// Executable heuristic on Windows: extension.
			std::string ext = pathToUtf8(e.full.extension());
			std::transform(ext.begin(), ext.end(), ext.begin(),
				[](char c) { return static_cast<char>(std::tolower((unsigned char)c)); });
			if (ext == ".exe" || ext == ".bat" || ext == ".cmd" || ext == ".com" || ext == ".ps1") {
				e.is_executable = true;
			}
			e.valid = true;
			return e;
		}

		std::string colorize(const LsEntry& e, bool use_color) {
			if (!use_color) return e.name;
			const char* code = nullptr;
			if (e.is_symlink) code = "\x1b[36;1m";       // cyan
			else if (e.is_dir) code = "\x1b[34;1m";      // blue
			else if (e.is_executable) code = "\x1b[32;1m"; // green
			if (!code) return e.name;
			return std::string(code) + e.name + "\x1b[0m";
		}

		std::string classifySuffix(const LsEntry& e) {
			if (e.is_dir) return "/";
			if (e.is_symlink) return "@";
			if (e.is_executable) return "*";
			return "";
		}

		std::string humanSize(std::uintmax_t n) {
			static const char* units[] = { "", "K", "M", "G", "T", "P" };
			double v = static_cast<double>(n);
			int u = 0;
			while (v >= 1024.0 && u + 1 < static_cast<int>(sizeof(units) / sizeof(units[0]))) {
				v /= 1024.0;
				++u;
			}
			char buf[32];
			if (u == 0) {
				std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(n));
			} else if (v >= 10.0) {
				std::snprintf(buf, sizeof(buf), "%.0f%s", v, units[u]);
			} else {
				std::snprintf(buf, sizeof(buf), "%.1f%s", v, units[u]);
			}
			return buf;
		}

		std::string formatMtime(const fs::file_time_type& t) {
			using namespace std::chrono;
			auto sctp = time_point_cast<system_clock::duration>(
				t - fs::file_time_type::clock::now() + system_clock::now());
			std::time_t tt = system_clock::to_time_t(sctp);
			std::tm tm{};
#ifdef _WIN32
			localtime_s(&tm, &tt);
#else
			localtime_r(&tt, &tm);
#endif
			// Match GNU ls: month day time-or-year. For the past 6 months, show
			// time; otherwise show year. Cheap version: always show YYYY-MM-DD.
			char buf[32];
			std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
			return buf;
		}

		std::string permString(const LsEntry& e) {
			std::string s;
			s += e.is_symlink ? 'l' : (e.is_dir ? 'd' : '-');
			// Best-effort rwx — Windows lacks POSIX bits; we approximate.
			bool readonly = false;
#ifdef _WIN32
			DWORD attr = GetFileAttributesW(e.full.wstring().c_str());
			if (attr != INVALID_FILE_ATTRIBUTES) {
				readonly = (attr & FILE_ATTRIBUTE_READONLY) != 0;
			}
#endif
			s += "r";
			s += (readonly ? "-" : "w");
			s += (e.is_executable || e.is_dir) ? "x" : "-";
			s += "r-";
			s += (e.is_executable || e.is_dir) ? "x" : "-";
			s += "r-";
			s += (e.is_executable || e.is_dir) ? "x" : "-";
			return s;
		}

		void sortEntries(std::vector<LsEntry>& items, const LsOpts& opts) {
			auto cmp_name = [](const LsEntry& a, const LsEntry& b) {
				// Case-insensitive primary, case-sensitive tiebreak.
				std::string al = a.name, bl = b.name;
				std::transform(al.begin(), al.end(), al.begin(),
					[](char c) { return static_cast<char>(std::tolower((unsigned char)c)); });
				std::transform(bl.begin(), bl.end(), bl.begin(),
					[](char c) { return static_cast<char>(std::tolower((unsigned char)c)); });
				if (al != bl) return al < bl;
				return a.name < b.name;
			};
			if (opts.sort_mtime) {
				std::sort(items.begin(), items.end(),
					[&](const LsEntry& a, const LsEntry& b) {
						if (a.mtime != b.mtime) return a.mtime > b.mtime;
						return cmp_name(a, b);
					});
			} else if (opts.sort_size) {
				std::sort(items.begin(), items.end(),
					[&](const LsEntry& a, const LsEntry& b) {
						if (a.size != b.size) return a.size > b.size;
						return cmp_name(a, b);
					});
			} else {
				std::sort(items.begin(), items.end(), cmp_name);
			}
			if (opts.reverse) std::reverse(items.begin(), items.end());
		}

		void printColumns(const std::vector<LsEntry>& items, const LsOpts& opts, bool use_color) {
			if (items.empty()) return;
			std::vector<std::string> labels;
			labels.reserve(items.size());
			std::size_t maxlen = 0;
			for (const auto& e : items) {
				std::string label = e.name + classifySuffix(e);
				if (label.size() > maxlen) maxlen = label.size();
				labels.push_back(std::move(label));
			}
			if (opts.one || !stdoutIsTty()) {
				for (std::size_t i = 0; i < items.size(); ++i) {
					if (use_color) std::fputs(colorize(items[i], true).c_str(), stdout);
					else           std::fputs(items[i].name.c_str(), stdout);
					std::fputs(classifySuffix(items[i]).c_str(), stdout);
					std::fputc('\n', stdout);
				}
				return;
			}
			int width = consoleWidth();
			std::size_t pad = maxlen + 2;
			std::size_t cols = std::max<std::size_t>(1, width / pad);
			std::size_t rows = (items.size() + cols - 1) / cols;
			for (std::size_t r = 0; r < rows; ++r) {
				for (std::size_t c = 0; c < cols; ++c) {
					std::size_t idx = c * rows + r;
					if (idx >= items.size()) break;
					std::string disp = use_color ? colorize(items[idx], true) : items[idx].name;
					disp += classifySuffix(items[idx]);
					std::fputs(disp.c_str(), stdout);
					if (c + 1 < cols && (c + 1) * rows + r < items.size()) {
						// Pad to column width using the *visible* label length.
						std::size_t visible = labels[idx].size();
						for (std::size_t k = visible; k < pad; ++k) std::fputc(' ', stdout);
					}
				}
				std::fputc('\n', stdout);
			}
		}

		void printLong(const std::vector<LsEntry>& items, const LsOpts& opts, bool use_color, Executor& exec) {
			std::string user = exec.env().get("USER");
			if (user.empty()) user = exec.env().get("USERNAME");
			if (user.empty()) user = "user";
			// Compute column widths.
			std::size_t size_w = 1;
			for (const auto& e : items) {
				std::string s = opts.human ? humanSize(e.size) : std::to_string(e.size);
				if (s.size() > size_w) size_w = s.size();
			}
			for (const auto& e : items) {
				std::string p = permString(e);
				std::string s = opts.human ? humanSize(e.size) : std::to_string(e.size);
				std::string mt = formatMtime(e.mtime);
				std::string label = use_color ? colorize(e, true) : e.name;
				label += classifySuffix(e);
				// Pad size right-aligned.
				std::string spad(size_w - s.size(), ' ');
				std::fprintf(stdout, "%s 1 %s %s %s%s %s %s\n",
					p.c_str(), user.c_str(), user.c_str(),
					spad.c_str(), s.c_str(), mt.c_str(), label.c_str());
			}
		}

		int builtin_ls(Executor& exec, const std::vector<std::string>& args) {
			LsOpts opts;
			std::vector<std::string> paths;
			for (std::size_t i = 0; i < args.size(); ++i) {
				const std::string& a = args[i];
				if (a == "--") {
					for (++i; i < args.size(); ++i) paths.push_back(args[i]);
					break;
				}
				if (a == "--color" || a == "--color=auto") opts.color = LsOpts::Auto;
				else if (a == "--color=always" || a == "--color=yes") opts.color = LsOpts::Always;
				else if (a == "--color=never"  || a == "--color=no")  opts.color = LsOpts::Never;
				else if (a == "--all")         opts.all = true;
				else if (a == "--human-readable") opts.human = true;
				else if (a == "--reverse")     opts.reverse = true;
				else if (a == "--classify")    opts.classify = true;
				else if (a.size() > 1 && a[0] == '-' && a[1] != '-') {
					for (std::size_t k = 1; k < a.size(); ++k) {
						switch (a[k]) {
						case 'a': case 'A': opts.all = true; break;
						case 'l': opts.long_fmt = true; break;
						case '1': opts.one = true; break;
						case 'h': opts.human = true; break;
						case 'r': opts.reverse = true; break;
						case 't': opts.sort_mtime = true; break;
						case 'S': opts.sort_size = true; break;
						case 'F': opts.classify = true; break;
						default:
							std::fprintf(stderr, "wbsh: ls: unknown option -%c\n", a[k]);
							return 2;
						}
					}
				} else {
					paths.push_back(a);
				}
			}
			if (paths.empty()) paths.push_back(".");

			bool use_color = (opts.color == LsOpts::Always)
				|| (opts.color == LsOpts::Auto && stdoutIsTty());
			// classify is implicit with -F; otherwise off.
			bool classify_set = opts.classify;
			(void)classify_set;

			int rc = 0;
			bool show_headers = paths.size() > 1;
			for (std::size_t pi = 0; pi < paths.size(); ++pi) {
				const std::string& p = paths[pi];
				fs::path target = toNative(exec, p);
				std::error_code ec;
				fs::file_status st = fs::symlink_status(target, ec);
				if (ec) {
					perr("ls", p, ec);
					rc = 1;
					continue;
				}
				std::vector<LsEntry> items;
				if (fs::is_directory(st)) {
					if (show_headers) {
						if (pi) std::fputc('\n', stdout);
						std::fprintf(stdout, "%s:\n", p.c_str());
					}
					fs::directory_iterator it(target, ec);
					if (ec) { perr("ls", p, ec); rc = 1; continue; }
					for (const auto& de : it) {
						std::string name;
						try { name = pathToUtf8(de.path().filename()); }
						catch (...) { continue; }
						LsEntry e = collect(target, name);
						if (!opts.all && e.is_hidden) continue;
						items.push_back(std::move(e));
					}
				} else {
					LsEntry e = collect(fs::path(), p);
					e.name = p;   // preserve user-supplied spelling
					items.push_back(std::move(e));
				}
				sortEntries(items, opts);
				if (opts.long_fmt) {
					std::uintmax_t total_blocks = 0;
					(void)total_blocks;
					printLong(items, opts, use_color, exec);
				} else {
					printColumns(items, opts, use_color);
				}
			}
			std::fflush(stdout);
			return rc;
		}

		// ---- cat -------------------------------------------------------------

		int builtin_cat(Executor& exec, const std::vector<std::string>& args) {
			bool number = false;
			bool number_nonblank = false;
			std::vector<std::string> files;
			std::size_t i = 0;
			while (i < args.size()) {
				const std::string& a = args[i];
				if (a == "--") { ++i; while (i < args.size()) files.push_back(args[i++]); break; }
				if (a == "-n" || a == "--number") { number = true; ++i; continue; }
				if (a == "-b" || a == "--number-nonblank") { number_nonblank = true; ++i; continue; }
				if (a == "-")  { files.push_back("-"); ++i; continue; }
				if (a.size() > 1 && a[0] == '-' && a[1] != '-') {
					bool ok = true;
					for (std::size_t k = 1; k < a.size(); ++k) {
						if (a[k] == 'n') number = true;
						else if (a[k] == 'b') number_nonblank = true;
						else { ok = false; break; }
					}
					if (ok) { ++i; continue; }
				}
				files.push_back(a);
				++i;
			}
			if (files.empty()) files.push_back("-");

			int rc = 0;
			std::size_t lineno = 0;
			auto emit_chunk = [&](const char* data, std::size_t n) {
				if (!number && !number_nonblank) {
					std::fwrite(data, 1, n, stdout);
					return;
				}
				// Line-aware emit: track lines, prepend numbers when appropriate.
				static thread_local bool at_line_start = true;
				static thread_local std::string pending;
				for (std::size_t p = 0; p < n; ++p) {
					if (at_line_start) {
						bool blank_line = (p < n && (data[p] == '\n'));
						if (number_nonblank) {
							if (!blank_line) {
								++lineno;
								std::fprintf(stdout, "%6zu\t", lineno);
							}
						} else {
							++lineno;
							std::fprintf(stdout, "%6zu\t", lineno);
						}
						at_line_start = false;
					}
					std::fputc(data[p], stdout);
					if (data[p] == '\n') at_line_start = true;
				}
			};

			for (const auto& f : files) {
				if (f == "-") {
					char buf[4096];
					while (true) {
						std::size_t got = std::fread(buf, 1, sizeof(buf), stdin);
						if (got == 0) break;
						emit_chunk(buf, got);
					}
					continue;
				}
				fs::path native = toNative(exec, f);
				std::ifstream in(native, std::ios::binary);
				if (!in) {
					std::fprintf(stderr, "wbsh: cat: %s: %s\n",
						f.c_str(), std::strerror(errno));
					rc = 1;
					continue;
				}
				char buf[4096];
				while (in) {
					in.read(buf, sizeof(buf));
					std::streamsize got = in.gcount();
					if (got > 0) emit_chunk(buf, static_cast<std::size_t>(got));
				}
			}
			std::fflush(stdout);
			return rc;
		}

		// ---- clear -----------------------------------------------------------

		int builtin_clear(Executor&, const std::vector<std::string>&) {
			// Cursor home + clear screen + clear scrollback.
			std::fputs("\x1b[H\x1b[2J\x1b[3J", stdout);
			std::fflush(stdout);
			return 0;
		}

		// ---- which -----------------------------------------------------------

		int builtin_which(Executor& exec, const std::vector<std::string>& args) {
			int rc = 0;
			for (const auto& name : args) {
				if (exec.isFunction(name)) {
					std::printf("%s: shell function\n", name.c_str());
					continue;
				}
				if (exec.isBuiltin(name)) {
					std::printf("%s: shell builtin\n", name.c_str());
					continue;
				}
				std::string path = exec.env().get("PATH");
				bool found = false;
				std::vector<std::string> dirs;
				std::string cur;
				for (std::size_t k = 0; k < path.size(); ++k) {
					char c = path[k];
					if (c == ';') { dirs.push_back(cur); cur.clear(); }
					else if (c == ':') {
						if (cur.size() == 1 && std::isalpha((unsigned char)cur[0])) cur.push_back(c);
						else { dirs.push_back(cur); cur.clear(); }
					} else cur.push_back(c);
				}
				if (!cur.empty()) dirs.push_back(cur);
				for (const auto& d : dirs) {
					if (d.empty()) continue;
					fs::path base = utf8ToPath(exec.pathConv().toWin32(d));
#ifdef _WIN32
					static const char* exts[] = { "", ".exe", ".cmd", ".bat", nullptr };
#else
					static const char* exts[] = { "", nullptr };
#endif
					for (int e = 0; exts[e]; ++e) {
						fs::path q = base / utf8ToPath(name + exts[e]);
						std::error_code ec;
						if (fs::exists(q, ec) && !fs::is_directory(q, ec)) {
							std::printf("%s\n", exec.pathConv().toPosix(pathToUtf8(q)).c_str());
							found = true;
							break;
						}
					}
					if (found) break;
				}
				if (!found) {
					std::fprintf(stderr, "wbsh: which: %s: not found\n", name.c_str());
					rc = 1;
				}
			}
			return rc;
		}

		// ---- mkdir / rmdir / rm / cp / mv / touch ---------------------------

		int builtin_mkdir(Executor& exec, const std::vector<std::string>& args) {
			bool parents = false;
			std::vector<std::string> paths;
			for (const auto& a : args) {
				if (a == "-p" || a == "--parents") parents = true;
				else if (a.size() > 1 && a[0] == '-' && a[1] != '-') {
					for (std::size_t k = 1; k < a.size(); ++k) if (a[k] == 'p') parents = true;
				} else paths.push_back(a);
			}
			if (paths.empty()) { perr("mkdir", "missing operand"); return 1; }
			int rc = 0;
			for (const auto& p : paths) {
				std::error_code ec;
				fs::path nat(toNative(exec, p));
				bool ok = parents ? fs::create_directories(nat, ec)
				                  : fs::create_directory(nat, ec);
				if (ec) { perr("mkdir", p, ec); rc = 1; }
				else if (!ok && !parents) { perr("mkdir", p + ": already exists"); rc = 1; }
			}
			return rc;
		}

		int builtin_rmdir(Executor& exec, const std::vector<std::string>& args) {
			if (args.empty()) { perr("rmdir", "missing operand"); return 1; }
			int rc = 0;
			for (const auto& p : args) {
				std::error_code ec;
				fs::path nat(toNative(exec, p));
				if (!fs::remove(nat, ec)) {
					if (ec) perr("rmdir", p, ec);
					else perr("rmdir", p + ": failed");
					rc = 1;
				}
			}
			return rc;
		}

		int builtin_rm(Executor& exec, const std::vector<std::string>& args) {
			bool recursive = false, force = false;
			std::vector<std::string> paths;
			for (const auto& a : args) {
				if (a == "-r" || a == "-R" || a == "--recursive") recursive = true;
				else if (a == "-f" || a == "--force") force = true;
				else if (a == "-rf" || a == "-fr" || a == "-Rf" || a == "-fR") { recursive = true; force = true; }
				else if (a.size() > 1 && a[0] == '-' && a[1] != '-') {
					for (std::size_t k = 1; k < a.size(); ++k) {
						if (a[k] == 'r' || a[k] == 'R') recursive = true;
						else if (a[k] == 'f') force = true;
					}
				} else paths.push_back(a);
			}
			if (paths.empty()) {
				if (!force) { perr("rm", "missing operand"); return 1; }
				return 0;
			}
			int rc = 0;
			for (const auto& p : paths) {
				std::error_code ec;
				fs::path nat(toNative(exec, p));
				if (!fs::exists(nat, ec)) {
					if (!force) { perr("rm", p + ": no such file or directory"); rc = 1; }
					continue;
				}
				if (recursive) {
					std::uintmax_t n = fs::remove_all(nat, ec);
					if (ec && !force) { perr("rm", p, ec); rc = 1; }
					(void)n;
				} else {
					if (fs::is_directory(nat, ec)) {
						perr("rm", p + ": is a directory");
						rc = 1;
						continue;
					}
					if (!fs::remove(nat, ec)) {
						if (!force) { perr("rm", p, ec); rc = 1; }
					}
				}
			}
			return rc;
		}

		int builtin_cp(Executor& exec, const std::vector<std::string>& args) {
			bool recursive = false;
			std::vector<std::string> paths;
			for (const auto& a : args) {
				if (a == "-r" || a == "-R" || a == "--recursive") recursive = true;
				else if (a == "-a") recursive = true;   // archive (we treat as -r)
				else if (a.size() > 1 && a[0] == '-' && a[1] != '-') {
					for (std::size_t k = 1; k < a.size(); ++k) {
						if (a[k] == 'r' || a[k] == 'R') recursive = true;
					}
				} else paths.push_back(a);
			}
			if (paths.size() < 2) { perr("cp", "missing source/destination"); return 1; }
			fs::path dst(toNative(exec, paths.back()));
			std::error_code ec;
			bool dst_is_dir = fs::is_directory(dst, ec);
			int rc = 0;
			for (std::size_t i = 0; i + 1 < paths.size(); ++i) {
				fs::path src(toNative(exec, paths[i]));
				fs::path target = dst_is_dir ? (dst / src.filename()) : dst;
				std::error_code copy_ec;
				auto opts = fs::copy_options::overwrite_existing;
				if (recursive) opts = opts | fs::copy_options::recursive
					| fs::copy_options::copy_symlinks;
				fs::copy(src, target, opts, copy_ec);
				if (copy_ec) { perr("cp", paths[i], copy_ec); rc = 1; }
			}
			return rc;
		}

		int builtin_mv(Executor& exec, const std::vector<std::string>& args) {
			std::vector<std::string> paths;
			for (const auto& a : args) {
				if (a == "--") continue;
				if (a.size() > 1 && a[0] == '-' && a[1] != '-' && a != "-") continue;   // ignore unknown flags
				paths.push_back(a);
			}
			if (paths.size() < 2) { perr("mv", "missing source/destination"); return 1; }
			fs::path dst(toNative(exec, paths.back()));
			std::error_code ec;
			bool dst_is_dir = fs::is_directory(dst, ec);
			int rc = 0;
			for (std::size_t i = 0; i + 1 < paths.size(); ++i) {
				fs::path src(toNative(exec, paths[i]));
				fs::path target = dst_is_dir ? (dst / src.filename()) : dst;
				std::error_code mv_ec;
				fs::rename(src, target, mv_ec);
				if (mv_ec) {
					// Cross-device or other rename failure: fall back to copy + remove.
					std::error_code cp_ec;
					fs::copy(src, target, fs::copy_options::overwrite_existing
						| fs::copy_options::recursive | fs::copy_options::copy_symlinks, cp_ec);
					if (cp_ec) { perr("mv", paths[i], cp_ec); rc = 1; continue; }
					std::error_code rm_ec;
					fs::remove_all(src, rm_ec);
					if (rm_ec) { perr("mv", paths[i], rm_ec); rc = 1; }
				}
			}
			return rc;
		}

		int builtin_touch(Executor& exec, const std::vector<std::string>& args) {
			if (args.empty()) { perr("touch", "missing operand"); return 1; }
			int rc = 0;
			for (const auto& p : args) {
				if (!p.empty() && p[0] == '-' && p != "-") continue;   // skip flags
				fs::path nat = toNative(exec, p);
				std::error_code ec;
				if (!fs::exists(nat, ec)) {
					std::ofstream f(nat, std::ios::binary | std::ios::app);
					if (!f) { perr("touch", p + ": cannot create"); rc = 1; continue; }
				} else {
					auto now = fs::file_time_type::clock::now();
					fs::last_write_time(nat, now, ec);
					if (ec) { perr("touch", p, ec); rc = 1; }
				}
			}
			return rc;
		}

		// ---- head / tail / wc ------------------------------------------------

		int parseNumFlag(const std::vector<std::string>& args, const char* short_flag,
		                 std::size_t& i, long& out) {
			const std::string& a = args[i];
			if (a.size() > 2 && a[0] == '-' && a[1] == short_flag[0] && std::isdigit((unsigned char)a[2])) {
				try { out = std::stol(a.substr(2)); ++i; return 0; } catch (...) { return -1; }
			}
			if (a == short_flag || a == std::string("-") + short_flag) {
				if (i + 1 >= args.size()) return -1;
				try { out = std::stol(args[i + 1]); i += 2; return 0; } catch (...) { return -1; }
			}
			return 1;
		}

		int builtin_head(Executor& exec, const std::vector<std::string>& args) {
			long n = 10;
			std::vector<std::string> files;
			for (std::size_t i = 0; i < args.size(); ) {
				const std::string& a = args[i];
				if (a == "-n" || a.rfind("-n", 0) == 0) {
					std::size_t j = i;
					int r = parseNumFlag(args, "n", j, n);
					if (r == 0) { i = j; continue; }
					if (r == -1) { perr("head", "bad -n value"); return 1; }
				}
				if (a == "--") { for (++i; i < args.size(); ++i) files.push_back(args[i]); break; }
				if (a.size() > 1 && a[0] == '-' && std::isdigit((unsigned char)a[1])) {
					try { n = std::stol(a.substr(1)); } catch (...) { perr("head", "bad N"); return 1; }
					++i; continue;
				}
				files.push_back(a);
				++i;
			}
			if (files.empty()) files.push_back("-");
			int rc = 0;
			for (const auto& f : files) {
				FILE* fp = (f == "-") ? stdin : fopenNative(exec, f, "rb");
				if (!fp) { perr("head", f + ": " + std::strerror(errno)); rc = 1; continue; }
				long printed = 0;
				int c;
				while (printed < n && (c = std::fgetc(fp)) != EOF) {
					std::fputc(c, stdout);
					if (c == '\n') ++printed;
				}
				if (fp != stdin) std::fclose(fp);
			}
			std::fflush(stdout);
			return rc;
		}

		int builtin_tail(Executor& exec, const std::vector<std::string>& args) {
			long n = 10;
			std::vector<std::string> files;
			for (std::size_t i = 0; i < args.size(); ) {
				const std::string& a = args[i];
				if (a == "-n" || a.rfind("-n", 0) == 0) {
					std::size_t j = i;
					int r = parseNumFlag(args, "n", j, n);
					if (r == 0) { i = j; continue; }
					if (r == -1) { perr("tail", "bad -n value"); return 1; }
				}
				if (a == "--") { for (++i; i < args.size(); ++i) files.push_back(args[i]); break; }
				if (a.size() > 1 && a[0] == '-' && std::isdigit((unsigned char)a[1])) {
					try { n = std::stol(a.substr(1)); } catch (...) { perr("tail", "bad N"); return 1; }
					++i; continue;
				}
				files.push_back(a);
				++i;
			}
			if (files.empty()) files.push_back("-");
			int rc = 0;
			for (const auto& f : files) {
				std::vector<std::string> lines;
				FILE* fp = (f == "-") ? stdin : fopenNative(exec, f, "rb");
				if (!fp) { perr("tail", f + ": " + std::strerror(errno)); rc = 1; continue; }
				std::string cur;
				int c;
				while ((c = std::fgetc(fp)) != EOF) {
					if (c == '\n') {
						lines.push_back(std::move(cur));
						cur.clear();
					} else cur.push_back(static_cast<char>(c));
				}
				if (!cur.empty()) lines.push_back(std::move(cur));
				if (fp != stdin) std::fclose(fp);
				std::size_t start = (lines.size() > static_cast<std::size_t>(n))
					? lines.size() - static_cast<std::size_t>(n) : 0;
				for (std::size_t k = start; k < lines.size(); ++k) {
					std::fputs(lines[k].c_str(), stdout);
					std::fputc('\n', stdout);
				}
			}
			std::fflush(stdout);
			return rc;
		}

		int builtin_wc(Executor& exec, const std::vector<std::string>& args) {
			bool want_l = false, want_w = false, want_c = false, want_m = false;
			std::vector<std::string> files;
			for (const auto& a : args) {
				if (a == "--") continue;
				if (a == "-l" || a == "--lines")  { want_l = true; continue; }
				if (a == "-w" || a == "--words")  { want_w = true; continue; }
				if (a == "-c" || a == "--bytes")  { want_c = true; continue; }
				if (a == "-m" || a == "--chars")  { want_m = true; continue; }
				if (a.size() > 1 && a[0] == '-' && a[1] != '-') {
					for (std::size_t k = 1; k < a.size(); ++k) {
						if (a[k] == 'l') want_l = true;
						else if (a[k] == 'w') want_w = true;
						else if (a[k] == 'c') want_c = true;
						else if (a[k] == 'm') want_m = true;
					}
					continue;
				}
				files.push_back(a);
			}
			bool any = want_l || want_w || want_c || want_m;
			if (!any) { want_l = want_w = want_c = true; }
			if (files.empty()) files.push_back("-");

			std::uintmax_t total_l = 0, total_w = 0, total_c = 0, total_m = 0;
			int rc = 0;
			for (const auto& f : files) {
				FILE* fp = (f == "-") ? stdin : fopenNative(exec, f, "rb");
				if (!fp) { perr("wc", f + ": " + std::strerror(errno)); rc = 1; continue; }
				std::uintmax_t l = 0, w = 0, c = 0, m = 0;
				bool in_word = false;
				int ch;
				while ((ch = std::fgetc(fp)) != EOF) {
					++c;
					if (static_cast<unsigned char>(ch) < 0x80
					    || (static_cast<unsigned char>(ch) & 0xC0) != 0x80) ++m;
					if (ch == '\n') ++l;
					if (std::isspace(static_cast<unsigned char>(ch))) {
						in_word = false;
					} else if (!in_word) {
						in_word = true;
						++w;
					}
				}
				if (fp != stdin) std::fclose(fp);
				std::string out;
				char buf[64];
				if (want_l) { std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)l); out += buf; }
				if (want_w) { if (!out.empty()) out += " "; std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)w); out += buf; }
				if (want_m && !want_c) { if (!out.empty()) out += " "; std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)m); out += buf; }
				if (want_c) { if (!out.empty()) out += " "; std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)c); out += buf; }
				if (f != "-") { out += " "; out += f; }
				out.push_back('\n');
				std::fputs(out.c_str(), stdout);
				total_l += l; total_w += w; total_c += c; total_m += m;
			}
			if (files.size() > 1) {
				std::string out;
				char buf[64];
				if (want_l) { std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)total_l); out += buf; }
				if (want_w) { if (!out.empty()) out += " "; std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)total_w); out += buf; }
				if (want_m && !want_c) { if (!out.empty()) out += " "; std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)total_m); out += buf; }
				if (want_c) { if (!out.empty()) out += " "; std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)total_c); out += buf; }
				out += " total\n";
				std::fputs(out.c_str(), stdout);
			}
			std::fflush(stdout);
			return rc;
		}

		// ---- whoami / hostname / env / sleep / basename / dirname ------------

		int builtin_whoami(Executor& exec, const std::vector<std::string>&) {
			std::string u = exec.env().get("USER");
			if (u.empty()) u = exec.env().get("USERNAME");
			if (u.empty()) u = "user";
			std::printf("%s\n", u.c_str());
			return 0;
		}

		int builtin_hostname(Executor& exec, const std::vector<std::string>&) {
			std::string h = exec.env().get("HOSTNAME");
			if (h.empty()) h = exec.env().get("COMPUTERNAME");
#ifdef _WIN32
			if (h.empty()) {
				char buf[256];
				DWORD n = sizeof(buf);
				if (GetComputerNameA(buf, &n)) h.assign(buf, n);
			}
#endif
			std::printf("%s\n", h.c_str());
			return 0;
		}

		int builtin_env(Executor& exec, const std::vector<std::string>& args) {
			// `env` with no args: print env. With `NAME=val ...` followed by a
			// command: run command with overridden env. (Not implemented yet —
			// we just print the merged set.)
			std::vector<std::pair<std::string, std::string>> sets;
			std::vector<std::string> cmd;
			for (const auto& a : args) {
				if (cmd.empty() && a.find('=') != std::string::npos
				    && (std::isalpha((unsigned char)a[0]) || a[0] == '_')) {
					auto eq = a.find('=');
					sets.emplace_back(a.substr(0, eq), a.substr(eq + 1));
				} else {
					cmd.push_back(a);
				}
			}
			if (!cmd.empty()) {
				std::fprintf(stderr, "wbsh: env: running with overrides not yet implemented; "
					"set then call directly\n");
				return 1;
			}
			std::vector<std::pair<std::string, std::string>> all(
				exec.env().vars().begin(), exec.env().vars().end());
			for (const auto& s : sets) {
				bool found = false;
				for (auto& kv : all) if (kv.first == s.first) { kv.second = s.second; found = true; break; }
				if (!found) all.push_back(s);
			}
			std::sort(all.begin(), all.end());
			for (const auto& kv : all) {
				if (!exec.env().isExported(kv.first)) continue;
				std::printf("%s=%s\n", kv.first.c_str(), kv.second.c_str());
			}
			return 0;
		}

		int builtin_sleep(Executor&, const std::vector<std::string>& args) {
			if (args.empty()) { perr("sleep", "missing operand"); return 1; }
			double secs = 0.0;
			try {
				const std::string& a = args[0];
				char suffix = (!a.empty()) ? a.back() : '\0';
				std::string num = (suffix == 's' || suffix == 'm' || suffix == 'h')
					? a.substr(0, a.size() - 1) : a;
				secs = std::stod(num);
				if (suffix == 'm') secs *= 60.0;
				else if (suffix == 'h') secs *= 3600.0;
			} catch (...) {
				perr("sleep", args[0] + ": invalid time interval");
				return 1;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(
				static_cast<long long>(secs * 1000.0)));
			return 0;
		}

		int builtin_basename(Executor&, const std::vector<std::string>& args) {
			if (args.empty()) { perr("basename", "missing operand"); return 1; }
			std::string s = args[0];
			while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
			auto pos = s.find_last_of("/\\");
			std::string name = (pos == std::string::npos) ? s : s.substr(pos + 1);
			if (args.size() > 1) {
				const std::string& suf = args[1];
				if (name.size() > suf.size()
				    && name.compare(name.size() - suf.size(), suf.size(), suf) == 0) {
					name.resize(name.size() - suf.size());
				}
			}
			std::printf("%s\n", name.c_str());
			return 0;
		}

		int builtin_dirname(Executor&, const std::vector<std::string>& args) {
			if (args.empty()) { perr("dirname", "missing operand"); return 1; }
			std::string s = args[0];
			while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
			auto pos = s.find_last_of("/\\");
			if (pos == std::string::npos) { std::printf(".\n"); return 0; }
			if (pos == 0) { std::printf("/\n"); return 0; }
			std::printf("%s\n", s.substr(0, pos).c_str());
			return 0;
		}

		// ---- Common helpers for line-oriented builtins -----------------------

		// Read all lines from a file (or stdin if path is "-"). Lines do NOT
		// include the terminating newline. A final unterminated line is
		// emitted as-is.
		bool readAllLines(Executor& exec, const std::string& path,
		                  std::vector<std::string>& out) {
			FILE* fp = (path == "-") ? stdin : fopenNative(exec, path, "rb");
			if (!fp) return false;
			std::string cur;
			int c;
			while ((c = std::fgetc(fp)) != EOF) {
				if (c == '\n') { out.push_back(std::move(cur)); cur.clear(); }
				else cur.push_back(static_cast<char>(c));
			}
			if (!cur.empty()) out.push_back(std::move(cur));
			if (fp != stdin) std::fclose(fp);
			return true;
		}

		// ---- sort ------------------------------------------------------------

		int builtin_sort(Executor& exec, const std::vector<std::string>& args) {
			bool reverse = false, numeric = false, unique = false, fold = false;
			std::vector<std::string> files;
			for (const auto& a : args) {
				if (a == "--") continue;
				if (a == "-r" || a == "--reverse") reverse = true;
				else if (a == "-n" || a == "--numeric-sort") numeric = true;
				else if (a == "-u" || a == "--unique") unique = true;
				else if (a == "-f" || a == "--ignore-case") fold = true;
				else if (a.size() > 1 && a[0] == '-' && a[1] != '-') {
					for (std::size_t k = 1; k < a.size(); ++k) {
						switch (a[k]) {
						case 'r': reverse = true; break;
						case 'n': numeric = true; break;
						case 'u': unique = true; break;
						case 'f': fold = true; break;
						default:
							std::fprintf(stderr, "wbsh: sort: unknown -%c\n", a[k]);
							return 2;
						}
					}
				} else files.push_back(a);
			}
			if (files.empty()) files.push_back("-");
			std::vector<std::string> lines;
			for (const auto& f : files) {
				if (!readAllLines(exec, f, lines)) {
					perr("sort", f + ": " + std::strerror(errno));
					return 2;
				}
			}
			auto fold_lower = [](std::string s) {
				for (char& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
				return s;
			};
			auto cmp = [&](const std::string& a, const std::string& b) {
				if (numeric) {
					double da = 0, db = 0;
					try { da = std::stod(a); } catch (...) {}
					try { db = std::stod(b); } catch (...) {}
					if (da != db) return da < db;
					return a < b;
				}
				if (fold) return fold_lower(a) < fold_lower(b);
				return a < b;
			};
			std::sort(lines.begin(), lines.end(), cmp);
			if (reverse) std::reverse(lines.begin(), lines.end());
			if (unique) {
				auto eq = [&](const std::string& a, const std::string& b) {
					if (fold) return fold_lower(a) == fold_lower(b);
					return a == b;
				};
				lines.erase(std::unique(lines.begin(), lines.end(), eq), lines.end());
			}
			for (const auto& l : lines) {
				std::fwrite(l.data(), 1, l.size(), stdout);
				std::fputc('\n', stdout);
			}
			std::fflush(stdout);
			return 0;
		}

		// ---- uniq (only dedupes ADJACENT duplicates, like real uniq) --------

		int builtin_uniq(Executor& exec, const std::vector<std::string>& args) {
			bool count = false, dups_only = false, uniques_only = false, fold = false;
			std::vector<std::string> files;
			for (const auto& a : args) {
				if (a == "-c" || a == "--count") count = true;
				else if (a == "-d" || a == "--repeated") dups_only = true;
				else if (a == "-u" || a == "--unique") uniques_only = true;
				else if (a == "-i" || a == "--ignore-case") fold = true;
				else if (a.size() > 1 && a[0] == '-' && a[1] != '-') {
					for (std::size_t k = 1; k < a.size(); ++k) {
						switch (a[k]) {
						case 'c': count = true; break;
						case 'd': dups_only = true; break;
						case 'u': uniques_only = true; break;
						case 'i': fold = true; break;
						}
					}
				} else files.push_back(a);
			}
			if (files.empty()) files.push_back("-");

			auto fold_lower = [](std::string s) {
				for (char& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
				return s;
			};
			auto eq = [&](const std::string& a, const std::string& b) {
				if (fold) return fold_lower(a) == fold_lower(b);
				return a == b;
			};

			std::vector<std::string> lines;
			for (const auto& f : files) {
				if (!readAllLines(exec, f, lines)) {
					perr("uniq", f + ": " + std::strerror(errno));
					return 1;
				}
			}
			std::size_t i = 0;
			while (i < lines.size()) {
				std::size_t j = i + 1;
				while (j < lines.size() && eq(lines[j], lines[i])) ++j;
				int n = static_cast<int>(j - i);
				bool emit = true;
				if (dups_only && n < 2) emit = false;
				if (uniques_only && n > 1) emit = false;
				if (emit) {
					if (count) std::printf("%7d %s\n", n, lines[i].c_str());
					else      std::printf("%s\n", lines[i].c_str());
				}
				i = j;
			}
			std::fflush(stdout);
			return 0;
		}

		// ---- tr --------------------------------------------------------------

		std::string trExpandSet(const std::string& s) {
			std::string r;
			for (std::size_t i = 0; i < s.size(); ++i) {
				if (s[i] == '\\' && i + 1 < s.size()) {
					char nx = s[++i];
					switch (nx) {
					case 'n': r.push_back('\n'); break;
					case 't': r.push_back('\t'); break;
					case 'r': r.push_back('\r'); break;
					case '\\': r.push_back('\\'); break;
					case 'a': r.push_back('\a'); break;
					case 'b': r.push_back('\b'); break;
					case '0': r.push_back('\0'); break;
					default: r.push_back(nx); break;
					}
				} else if (i + 2 < s.size() && s[i + 1] == '-') {
					char a = s[i], b = s[i + 2];
					if (a <= b) for (char c = a; c <= b; ++c) r.push_back(c);
					else        for (char c = a; c >= b; --c) r.push_back(c);
					i += 2;
				} else {
					r.push_back(s[i]);
				}
			}
			return r;
		}

		int builtin_tr(Executor&, const std::vector<std::string>& args) {
			bool delete_mode = false, squeeze = false, complement = false;
			std::vector<std::string> sets;
			for (const auto& a : args) {
				if (a == "-d") delete_mode = true;
				else if (a == "-s") squeeze = true;
				else if (a == "-c" || a == "-C" || a == "--complement") complement = true;
				else if (a.size() > 1 && a[0] == '-' && a != "-") {
					for (std::size_t k = 1; k < a.size(); ++k) {
						if (a[k] == 'd') delete_mode = true;
						else if (a[k] == 's') squeeze = true;
						else if (a[k] == 'c' || a[k] == 'C') complement = true;
					}
				} else sets.push_back(a);
			}
			if (sets.empty() || (!delete_mode && !squeeze && sets.size() < 2)) {
				perr("tr", "usage: tr [-cds] SET1 [SET2]");
				return 2;
			}
			std::string s1 = trExpandSet(sets[0]);
			std::string s2 = (sets.size() > 1) ? trExpandSet(sets[1]) : std::string();
			std::vector<bool> in_s1(256, false);
			for (unsigned char c : s1) in_s1[c] = true;
			if (complement) {
				std::vector<bool> inv(256, true);
				for (int k = 0; k < 256; ++k) if (in_s1[k]) inv[k] = false;
				in_s1 = std::move(inv);
			}
			std::vector<unsigned char> map(256);
			for (int k = 0; k < 256; ++k) map[k] = static_cast<unsigned char>(k);
			if (!delete_mode && !s2.empty()) {
				// Map each byte in s1 (or in complement) to the corresponding
				// byte in s2; pad s2 with its last char.
				if (complement) {
					unsigned char repl = static_cast<unsigned char>(s2.back());
					for (int k = 0; k < 256; ++k) if (in_s1[k]) map[k] = repl;
				} else {
					for (std::size_t k = 0; k < s1.size(); ++k) {
						unsigned char src = static_cast<unsigned char>(s1[k]);
						unsigned char dst = (k < s2.size())
							? static_cast<unsigned char>(s2[k])
							: static_cast<unsigned char>(s2.back());
						map[src] = dst;
					}
				}
			}
			int prev = -1;
			int c;
			while ((c = std::fgetc(stdin)) != EOF) {
				unsigned char uc = static_cast<unsigned char>(c);
				if (delete_mode && in_s1[uc]) continue;
				unsigned char out = map[uc];
				if (squeeze && static_cast<int>(out) == prev) continue;
				std::fputc(out, stdout);
				prev = out;
			}
			std::fflush(stdout);
			return 0;
		}

		// ---- cut -------------------------------------------------------------

		struct CutSpec {
			std::vector<std::pair<int, int>> ranges;  // [start,end], 1-indexed; -1 means "to end"
			bool parse(const std::string& spec) {
				std::size_t i = 0;
				while (i < spec.size()) {
					std::string tok;
					while (i < spec.size() && spec[i] != ',') tok.push_back(spec[i++]);
					if (i < spec.size() && spec[i] == ',') ++i;
					if (tok.empty()) continue;
					int a = 0, b = 0;
					auto dash = tok.find('-');
					try {
						if (dash == std::string::npos) {
							a = std::stoi(tok); b = a;
						} else if (dash == 0) {
							a = 1;
							b = std::stoi(tok.substr(1));
						} else if (dash + 1 == tok.size()) {
							a = std::stoi(tok.substr(0, dash)); b = -1;
						} else {
							a = std::stoi(tok.substr(0, dash));
							b = std::stoi(tok.substr(dash + 1));
						}
					} catch (...) { return false; }
					ranges.emplace_back(a, b);
				}
				return !ranges.empty();
			}
			bool contains(int n) const {
				for (auto& r : ranges) {
					if (n >= r.first && (r.second == -1 || n <= r.second)) return true;
				}
				return false;
			}
		};

		int builtin_cut(Executor& exec, const std::vector<std::string>& args) {
			char delim = '\t';
			std::string field_spec, char_spec;
			bool only_delim_lines = false;
			std::vector<std::string> files;
			for (std::size_t i = 0; i < args.size(); ++i) {
				const std::string& a = args[i];
				if (a == "-d" && i + 1 < args.size()) {
					if (!args[i + 1].empty()) delim = args[i + 1][0];
					++i;
				} else if (a.size() > 2 && a.compare(0, 2, "-d") == 0) {
					delim = a[2];
				} else if (a == "-f" && i + 1 < args.size()) {
					field_spec = args[++i];
				} else if (a.size() > 2 && a.compare(0, 2, "-f") == 0) {
					field_spec = a.substr(2);
				} else if (a == "-c" && i + 1 < args.size()) {
					char_spec = args[++i];
				} else if (a.size() > 2 && a.compare(0, 2, "-c") == 0) {
					char_spec = a.substr(2);
				} else if (a == "-s") only_delim_lines = true;
				else if (a == "--") {
					for (++i; i < args.size(); ++i) files.push_back(args[i]);
				} else if (!a.empty() && a[0] != '-') files.push_back(a);
			}
			if (field_spec.empty() && char_spec.empty()) {
				perr("cut", "specify -f or -c");
				return 1;
			}
			CutSpec spec;
			if (!spec.parse(field_spec.empty() ? char_spec : field_spec)) {
				perr("cut", "bad field/char spec");
				return 1;
			}
			bool by_field = !field_spec.empty();
			if (files.empty()) files.push_back("-");
			int rc = 0;
			for (const auto& f : files) {
				std::vector<std::string> lines;
				if (!readAllLines(exec, f, lines)) {
					perr("cut", f + ": " + std::strerror(errno));
					rc = 1;
					continue;
				}
				for (auto& line : lines) {
					if (by_field) {
						if (line.find(delim) == std::string::npos) {
							if (!only_delim_lines) std::printf("%s\n", line.c_str());
							continue;
						}
						std::vector<std::string> fields;
						std::string cur;
						for (char c : line) {
							if (c == delim) { fields.push_back(std::move(cur)); cur.clear(); }
							else cur.push_back(c);
						}
						fields.push_back(std::move(cur));
						std::string out;
						bool first = true;
						for (std::size_t k = 0; k < fields.size(); ++k) {
							if (spec.contains(static_cast<int>(k + 1))) {
								if (!first) out.push_back(delim);
								out += fields[k];
								first = false;
							}
						}
						std::printf("%s\n", out.c_str());
					} else {
						std::string out;
						for (std::size_t k = 0; k < line.size(); ++k) {
							if (spec.contains(static_cast<int>(k + 1))) out.push_back(line[k]);
						}
						std::printf("%s\n", out.c_str());
					}
				}
			}
			std::fflush(stdout);
			return rc;
		}

		// ---- tee -------------------------------------------------------------

		int builtin_tee(Executor& exec, const std::vector<std::string>& args) {
			bool append = false;
			std::vector<std::string> files;
			for (const auto& a : args) {
				if (a == "-a" || a == "--append") append = true;
				else if (a.size() > 1 && a[0] == '-' && a != "-") continue;
				else files.push_back(a);
			}
			std::vector<FILE*> outs;
			for (const auto& f : files) {
				FILE* fp = fopenNative(exec, f, append ? "ab" : "wb");
				if (!fp) {
					perr("tee", f + ": " + std::strerror(errno));
				} else {
					outs.push_back(fp);
				}
			}
			char buf[4096];
			while (true) {
				std::size_t got = std::fread(buf, 1, sizeof(buf), stdin);
				if (got == 0) break;
				std::fwrite(buf, 1, got, stdout);
				for (FILE* fp : outs) std::fwrite(buf, 1, got, fp);
			}
			for (FILE* fp : outs) std::fclose(fp);
			std::fflush(stdout);
			return 0;
		}

		// ---- paste -----------------------------------------------------------

		int builtin_paste(Executor& exec, const std::vector<std::string>& args) {
			std::string delims = "\t";
			std::vector<std::string> files;
			bool serial = false;
			for (std::size_t i = 0; i < args.size(); ++i) {
				const std::string& a = args[i];
				if (a == "-d" && i + 1 < args.size()) {
					delims = args[++i];
				} else if (a.size() > 2 && a.compare(0, 2, "-d") == 0) {
					delims = a.substr(2);
				} else if (a == "-s") serial = true;
				else if (a == "--") { for (++i; i < args.size(); ++i) files.push_back(args[i]); }
				else if (!a.empty() && a[0] != '-') files.push_back(a);
			}
			if (files.empty()) files.push_back("-");
			if (serial) {
				int rc = 0;
				for (const auto& f : files) {
					std::vector<std::string> lines;
					if (!readAllLines(exec, f, lines)) { perr("paste", f); rc = 1; continue; }
					for (std::size_t k = 0; k < lines.size(); ++k) {
						if (k) std::fputc(delims.empty() ? '\t' : delims[k % delims.size()], stdout);
						std::fputs(lines[k].c_str(), stdout);
					}
					std::fputc('\n', stdout);
				}
				std::fflush(stdout);
				return rc;
			}
			std::vector<std::vector<std::string>> all;
			std::size_t maxlen = 0;
			for (const auto& f : files) {
				std::vector<std::string> v;
				if (!readAllLines(exec, f, v)) { perr("paste", f); return 1; }
				if (v.size() > maxlen) maxlen = v.size();
				all.push_back(std::move(v));
			}
			for (std::size_t r = 0; r < maxlen; ++r) {
				for (std::size_t c = 0; c < all.size(); ++c) {
					if (c) std::fputc(delims.empty() ? '\t' : delims[(c - 1) % delims.size()], stdout);
					if (r < all[c].size()) std::fputs(all[c][r].c_str(), stdout);
				}
				std::fputc('\n', stdout);
			}
			std::fflush(stdout);
			return 0;
		}

		// ---- tac / rev / nl --------------------------------------------------

		int builtin_tac(Executor& exec, const std::vector<std::string>& args) {
			std::vector<std::string> files;
			for (const auto& a : args) if (!a.empty() && a[0] != '-') files.push_back(a);
			if (files.empty()) files.push_back("-");
			int rc = 0;
			for (const auto& f : files) {
				std::vector<std::string> lines;
				if (!readAllLines(exec, f, lines)) { perr("tac", f); rc = 1; continue; }
				for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
					std::fputs(it->c_str(), stdout);
					std::fputc('\n', stdout);
				}
			}
			std::fflush(stdout);
			return rc;
		}

		int builtin_rev(Executor& exec, const std::vector<std::string>& args) {
			std::vector<std::string> files;
			for (const auto& a : args) if (!a.empty() && a[0] != '-') files.push_back(a);
			if (files.empty()) files.push_back("-");
			int rc = 0;
			for (const auto& f : files) {
				std::vector<std::string> lines;
				if (!readAllLines(exec, f, lines)) { perr("rev", f); rc = 1; continue; }
				for (auto& l : lines) {
					std::reverse(l.begin(), l.end());
					std::fputs(l.c_str(), stdout);
					std::fputc('\n', stdout);
				}
			}
			std::fflush(stdout);
			return rc;
		}

		int builtin_nl(Executor& exec, const std::vector<std::string>& args) {
			bool number_blank = false;
			std::vector<std::string> files;
			for (const auto& a : args) {
				if (a == "-ba") number_blank = true;
				else if (a == "-bt") number_blank = false;
				else if (!a.empty() && a[0] != '-') files.push_back(a);
			}
			if (files.empty()) files.push_back("-");
			int rc = 0;
			int n = 0;
			for (const auto& f : files) {
				std::vector<std::string> lines;
				if (!readAllLines(exec, f, lines)) { perr("nl", f); rc = 1; continue; }
				for (auto& l : lines) {
					if (l.empty() && !number_blank) {
						std::fputc('\n', stdout);
					} else {
						++n;
						std::fprintf(stdout, "%6d\t%s\n", n, l.c_str());
					}
				}
			}
			std::fflush(stdout);
			return rc;
		}

		// ---- date / seq ------------------------------------------------------

		int builtin_date(Executor&, const std::vector<std::string>& args) {
			std::string fmt = "%a %b %e %H:%M:%S %Y";
			bool utc = false;
			for (const auto& a : args) {
				if (a == "-u" || a == "--utc") utc = true;
				else if (!a.empty() && a[0] == '+') fmt = a.substr(1);
			}
			std::time_t t = std::time(nullptr);
			std::tm tm{};
#ifdef _WIN32
			if (utc) gmtime_s(&tm, &t); else localtime_s(&tm, &t);
#else
			if (utc) gmtime_r(&t, &tm); else localtime_r(&t, &tm);
#endif
			char buf[256];
			std::strftime(buf, sizeof(buf), fmt.c_str(), &tm);
			std::printf("%s\n", buf);
			return 0;
		}

		int builtin_seq(Executor&, const std::vector<std::string>& args) {
			double first = 1.0, inc = 1.0, last = 1.0;
			std::string sep = "\n";
			std::vector<std::string> nums;
			for (std::size_t i = 0; i < args.size(); ++i) {
				const std::string& a = args[i];
				if (a == "-s" && i + 1 < args.size()) sep = args[++i];
				else if (a.size() > 2 && a.compare(0, 2, "-s") == 0) sep = a.substr(2);
				else if (a == "--") {
					for (++i; i < args.size(); ++i) nums.push_back(args[i]);
				} else nums.push_back(a);
			}
			try {
				if (nums.size() == 1) { last = std::stod(nums[0]); }
				else if (nums.size() == 2) { first = std::stod(nums[0]); last = std::stod(nums[1]); }
				else if (nums.size() == 3) { first = std::stod(nums[0]); inc = std::stod(nums[1]); last = std::stod(nums[2]); }
				else { perr("seq", "usage: seq [LAST | FIRST LAST | FIRST INC LAST]"); return 1; }
			} catch (...) { perr("seq", "invalid number"); return 1; }
			if (inc == 0) { perr("seq", "increment must be non-zero"); return 1; }
			bool integer = std::floor(first) == first && std::floor(inc) == inc && std::floor(last) == last;
			bool first_out = true;
			auto emit = [&](double v) {
				if (!first_out) std::fputs(sep.c_str(), stdout);
				if (integer) std::fprintf(stdout, "%lld", static_cast<long long>(v));
				else         std::fprintf(stdout, "%g", v);
				first_out = false;
			};
			if (inc > 0) {
				for (double v = first; v <= last + 1e-12; v += inc) emit(v);
			} else {
				for (double v = first; v >= last - 1e-12; v += inc) emit(v);
			}
			std::fputc('\n', stdout);
			std::fflush(stdout);
			return 0;
		}

		// ---- uname / id ------------------------------------------------------

		int builtin_uname(Executor& exec, const std::vector<std::string>& args) {
			bool s = false, n = false, r = false, v = false, m = false, o = false, all = false;
			for (const auto& a : args) {
				if (a == "-a" || a == "--all") all = true;
				else if (a == "-s" || a == "--kernel-name") s = true;
				else if (a == "-n" || a == "--nodename")    n = true;
				else if (a == "-r" || a == "--kernel-release") r = true;
				else if (a == "-v" || a == "--kernel-version") v = true;
				else if (a == "-m" || a == "--machine")     m = true;
				else if (a == "-o" || a == "--operating-system") o = true;
				else if (a.size() > 1 && a[0] == '-' && a[1] != '-') {
					for (std::size_t k = 1; k < a.size(); ++k) {
						switch (a[k]) {
						case 'a': all = true; break;
						case 's': s = true; break;
						case 'n': n = true; break;
						case 'r': r = true; break;
						case 'v': v = true; break;
						case 'm': m = true; break;
						case 'o': o = true; break;
						}
					}
				}
			}
			if (!all && !s && !n && !r && !v && !m && !o) s = true;
			if (all) s = n = r = v = m = o = true;

			std::string node = exec.env().get("COMPUTERNAME");
			if (node.empty()) node = exec.env().get("HOSTNAME");
#ifdef _WIN32
			std::string arch = exec.env().get("PROCESSOR_ARCHITECTURE");
			if (arch.empty()) arch = "x86_64";
			OSVERSIONINFOA vi{};
			vi.dwOSVersionInfoSize = sizeof(vi);
#  pragma warning(push)
#  pragma warning(disable : 4996)
			GetVersionExA(&vi);
#  pragma warning(pop)
			std::string release = std::to_string(vi.dwMajorVersion) + "."
				+ std::to_string(vi.dwMinorVersion);
			std::string version = std::to_string(vi.dwBuildNumber);
#else
			std::string arch = "x86_64", release = "0", version = "0";
#endif
			std::string kernel = "wbsh";
			std::string opsys  = "Windows";

			std::string out;
			auto add = [&](const std::string& x) { if (!out.empty()) out += " "; out += x; };
			if (s) add(kernel);
			if (n) add(node);
			if (r) add(release);
			if (v) add(version);
			if (m) add(arch);
			if (o) add(opsys);
			std::printf("%s\n", out.c_str());
			return 0;
		}

		int builtin_id(Executor& exec, const std::vector<std::string>& args) {
			bool name_only = false, user_only = false, group_only = false;
			for (const auto& a : args) {
				if (a == "-n") name_only = true;
				else if (a == "-u") user_only = true;
				else if (a == "-g") group_only = true;
			}
			std::string user = exec.env().get("USER");
			if (user.empty()) user = exec.env().get("USERNAME");
			if (user.empty()) user = "user";
			// Windows lacks POSIX uid/gid; print stable placeholders.
			int uid = 1000, gid = 1000;
			if (user_only) {
				if (name_only) std::printf("%s\n", user.c_str());
				else           std::printf("%d\n", uid);
			} else if (group_only) {
				if (name_only) std::printf("%s\n", user.c_str());
				else           std::printf("%d\n", gid);
			} else {
				std::printf("uid=%d(%s) gid=%d(%s) groups=%d(%s)\n",
					uid, user.c_str(), gid, user.c_str(), gid, user.c_str());
			}
			return 0;
		}

		// ---- realpath / readlink --------------------------------------------

		int builtin_realpath(Executor& exec, const std::vector<std::string>& args) {
			if (args.empty()) { perr("realpath", "missing operand"); return 1; }
			int rc = 0;
			for (const auto& p : args) {
				if (!p.empty() && p[0] == '-') continue;
				std::error_code ec;
				fs::path nat = fs::weakly_canonical(toNative(exec, p), ec);
				if (ec) { perr("realpath", p, ec); rc = 1; continue; }
				std::printf("%s\n", exec.pathConv().toPosix(pathToUtf8(nat)).c_str());
			}
			return rc;
		}

		int builtin_readlink(Executor& exec, const std::vector<std::string>& args) {
			bool canonical = false;
			std::vector<std::string> paths;
			for (const auto& a : args) {
				if (a == "-f" || a == "-e" || a == "-m" || a == "--canonicalize") canonical = true;
				else if (!a.empty() && a[0] != '-') paths.push_back(a);
			}
			if (paths.empty()) { perr("readlink", "missing operand"); return 1; }
			int rc = 0;
			for (const auto& p : paths) {
				std::error_code ec;
				if (canonical) {
					fs::path q = fs::weakly_canonical(toNative(exec, p), ec);
					if (ec) { rc = 1; continue; }
					std::printf("%s\n", exec.pathConv().toPosix(pathToUtf8(q)).c_str());
				} else {
					fs::path q = fs::read_symlink(toNative(exec, p), ec);
					if (ec) { rc = 1; continue; }
					std::printf("%s\n", exec.pathConv().toPosix(pathToUtf8(q)).c_str());
				}
			}
			return rc;
		}

		// ---- expr (simple) ---------------------------------------------------

		int builtin_expr(Executor&, const std::vector<std::string>& args) {
			// Tiny `expr`: integer arithmetic + string `length`, `substr`, `index`.
			// Not full POSIX, but handles common scripts.
			if (args.empty()) { perr("expr", "missing operand"); return 2; }
			if (args.size() == 2 && args[0] == "length") {
				std::printf("%zu\n", args[1].size());
				return 0;
			}
			if (args.size() == 4 && args[0] == "substr") {
				try {
					int p = std::stoi(args[2]);
					int n = std::stoi(args[3]);
					if (p < 1) p = 1;
					std::size_t start = static_cast<std::size_t>(p - 1);
					if (start >= args[1].size()) { std::printf("\n"); return 1; }
					std::printf("%s\n", args[1].substr(start, n).c_str());
					return 0;
				} catch (...) { return 2; }
			}
			if (args.size() == 3) {
				const std::string& l = args[0]; const std::string& op = args[1]; const std::string& r = args[2];
				try {
					long long li = std::stoll(l), ri = std::stoll(r);
					if (op == "+") { std::printf("%lld\n", li + ri); return 0; }
					if (op == "-") { std::printf("%lld\n", li - ri); return 0; }
					if (op == "*") { std::printf("%lld\n", li * ri); return 0; }
					if (op == "/") { if (ri == 0) return 2; std::printf("%lld\n", li / ri); return 0; }
					if (op == "%") { if (ri == 0) return 2; std::printf("%lld\n", li % ri); return 0; }
					if (op == "<")  { std::printf("%d\n", li <  ri); return (li <  ri) ? 0 : 1; }
					if (op == "<=") { std::printf("%d\n", li <= ri); return (li <= ri) ? 0 : 1; }
					if (op == ">")  { std::printf("%d\n", li >  ri); return (li >  ri) ? 0 : 1; }
					if (op == ">=") { std::printf("%d\n", li >= ri); return (li >= ri) ? 0 : 1; }
					if (op == "=" || op == "==") { std::printf("%d\n", li == ri); return (li == ri) ? 0 : 1; }
					if (op == "!=") { std::printf("%d\n", li != ri); return (li != ri) ? 0 : 1; }
				} catch (...) {
					if (op == "=" || op == "==") { std::printf("%d\n", l == r); return (l == r) ? 0 : 1; }
					if (op == "!=")              { std::printf("%d\n", l != r); return (l != r) ? 0 : 1; }
				}
			}
			// Fallback: print joined with spaces.
			std::string out;
			for (std::size_t i = 0; i < args.size(); ++i) {
				if (i) out.push_back(' ');
				out += args[i];
			}
			std::printf("%s\n", out.c_str());
			return 0;
		}

		// ---- grep ------------------------------------------------------------

		int builtin_grep(Executor& exec, const std::vector<std::string>& args) {
			bool icase = false, invert = false, line_no = false, count_only = false;
			bool fixed = false, list_only = false, quiet = false, recursive = false;
			std::string pattern;
			std::vector<std::string> files;
			for (std::size_t i = 0; i < args.size(); ++i) {
				const std::string& a = args[i];
				if (a == "--") { for (++i; i < args.size(); ++i) {
					if (pattern.empty()) pattern = args[i];
					else files.push_back(args[i]);
				} break; }
				if (a == "-e" && i + 1 < args.size()) { pattern = args[++i]; continue; }
				if (a == "-i" || a == "--ignore-case") { icase = true; continue; }
				if (a == "-v" || a == "--invert-match") { invert = true; continue; }
				if (a == "-n" || a == "--line-number") { line_no = true; continue; }
				if (a == "-c" || a == "--count") { count_only = true; continue; }
				if (a == "-F" || a == "--fixed-strings") { fixed = true; continue; }
				if (a == "-E" || a == "--extended-regexp") { continue; }
				if (a == "-l") { list_only = true; continue; }
				if (a == "-q" || a == "--quiet" || a == "--silent") { quiet = true; continue; }
				if (a == "-r" || a == "-R" || a == "--recursive") { recursive = true; continue; }
				if (a.size() > 1 && a[0] == '-' && a[1] != '-') {
					for (std::size_t k = 1; k < a.size(); ++k) {
						switch (a[k]) {
						case 'i': icase = true; break;
						case 'v': invert = true; break;
						case 'n': line_no = true; break;
						case 'c': count_only = true; break;
						case 'F': fixed = true; break;
						case 'E': break;
						case 'l': list_only = true; break;
						case 'q': quiet = true; break;
						case 'r': case 'R': recursive = true; break;
						default:
							std::fprintf(stderr, "wbsh: grep: unknown option -%c\n", a[k]);
							return 2;
						}
					}
					continue;
				}
				if (pattern.empty()) pattern = a;
				else files.push_back(a);
			}
			if (pattern.empty()) { perr("grep", "missing pattern"); return 2; }
			if (files.empty()) files.push_back(recursive ? "." : "-");

			// -r: expand any directory arg into the files it contains.
			if (recursive) {
				std::vector<std::string> expanded;
				for (const auto& f : files) {
					if (f == "-") { expanded.push_back(f); continue; }
					fs::path nat = toNative(exec, f);
					std::error_code ec;
					if (!fs::is_directory(nat, ec)) {
						expanded.push_back(f);
						continue;
					}
					fs::recursive_directory_iterator it(nat,
						fs::directory_options::skip_permission_denied, ec);
					if (ec) continue;
					for (auto cur = it; cur != fs::recursive_directory_iterator(); cur.increment(ec)) {
						if (ec) break;
						std::error_code fec;
						if (cur->is_regular_file(fec)) {
							std::string p = pathToUtf8(cur->path());
							std::replace(p.begin(), p.end(), '\\', '/');
							expanded.push_back(p);
						}
					}
				}
				files = std::move(expanded);
			}

			std::regex::flag_type flags = std::regex::ECMAScript;
			if (icase) flags |= std::regex::icase;
			std::regex re;
			if (!fixed) {
				try { re = std::regex(pattern, flags); }
				catch (const std::regex_error& e) {
					perr("grep", std::string("bad pattern: ") + e.what());
					return 2;
				}
			}
			std::string lower_pat;
			if (fixed && icase) {
				lower_pat = pattern;
				for (char& c : lower_pat) c = static_cast<char>(std::tolower((unsigned char)c));
			}

			auto match_line = [&](const std::string& line) {
				if (fixed) {
					if (!icase) return line.find(pattern) != std::string::npos;
					std::string lo = line;
					for (char& c : lo) c = static_cast<char>(std::tolower((unsigned char)c));
					return lo.find(lower_pat) != std::string::npos;
				}
				return std::regex_search(line, re);
			};

			bool any_match = false;
			bool show_filename = files.size() > 1;
			int rc = 1;   // default: no matches
			for (const auto& f : files) {
				std::vector<std::string> lines;
				if (!readAllLines(exec, f, lines)) {
					perr("grep", f + ": " + std::strerror(errno));
					rc = 2;
					continue;
				}
				int file_count = 0;
				bool file_any = false;
				for (std::size_t k = 0; k < lines.size(); ++k) {
					bool m = match_line(lines[k]);
					if (invert) m = !m;
					if (!m) continue;
					++file_count;
					file_any = true;
					any_match = true;
					if (quiet) goto next_file;
					if (list_only) { std::printf("%s\n", f.c_str()); break; }
					if (count_only) continue;
					if (show_filename) std::printf("%s:", f.c_str());
					if (line_no) std::printf("%zu:", k + 1);
					std::printf("%s\n", lines[k].c_str());
				}
				if (count_only && !list_only && !quiet) {
					if (show_filename) std::printf("%s:", f.c_str());
					std::printf("%d\n", file_count);
				}
			next_file:
				(void)file_any;
			}
			std::fflush(stdout);
			if (quiet) return any_match ? 0 : 1;
			return any_match ? 0 : rc;
		}

		// ---- find ------------------------------------------------------------

		int builtin_find(Executor& exec, const std::vector<std::string>& args) {
			std::vector<std::string> roots;
			std::string name_pat;
			char type_filter = 0;   // 0 = any, 'f' = file, 'd' = dir, 'l' = symlink
			int max_depth = -1;
			std::size_t i = 0;
			while (i < args.size()) {
				const std::string& a = args[i];
				if (a == "-name" && i + 1 < args.size()) { name_pat = args[++i]; ++i; continue; }
				if (a == "-type" && i + 1 < args.size()) {
					if (!args[i + 1].empty()) type_filter = args[i + 1][0];
					i += 2; continue;
				}
				if (a == "-maxdepth" && i + 1 < args.size()) {
					try { max_depth = std::stoi(args[++i]); ++i; continue; }
					catch (...) { perr("find", "bad -maxdepth"); return 1; }
				}
				if (!a.empty() && a[0] == '-') {
					std::fprintf(stderr, "wbsh: find: option %s not implemented\n", a.c_str());
					return 1;
				}
				roots.push_back(a);
				++i;
			}
			if (roots.empty()) roots.push_back(".");

			auto match_name = [&](const std::string& name) {
				if (name_pat.empty()) return true;
				// Lightweight glob: treat * and ? like fnmatch.
				std::regex_constants::syntax_option_type fl = std::regex::ECMAScript;
				std::string r;
				for (char c : name_pat) {
					switch (c) {
					case '*': r += ".*"; break;
					case '?': r += "."; break;
					case '.': case '+': case '(': case ')': case '|': case '^':
					case '$': case '{': case '}': case '\\':
						r.push_back('\\'); r.push_back(c); break;
					default: r.push_back(c); break;
					}
				}
				try { return std::regex_match(name, std::regex(r, fl)); }
				catch (...) { return false; }
			};

			auto pass_type = [&](const fs::path& p) {
				std::error_code ec;
				if (type_filter == 0) return true;
				auto st = fs::symlink_status(p, ec);
				if (ec) return false;
				if (type_filter == 'l') return fs::is_symlink(st);
				if (type_filter == 'd') return fs::is_directory(st);
				if (type_filter == 'f') return fs::is_regular_file(st);
				return true;
			};

			int rc = 0;
			for (const auto& r : roots) {
				std::error_code ec;
				fs::path nat(toNative(exec, r));
				if (!fs::exists(nat, ec)) {
					std::fprintf(stderr, "wbsh: find: %s: no such file or directory\n", r.c_str());
					rc = 1;
					continue;
				}
				// Print root itself if it matches.
				if (pass_type(nat) && match_name(pathToUtf8(nat.filename()).empty() ? r : pathToUtf8(nat.filename()))) {
					std::printf("%s\n", exec.pathConv().toPosix(pathToUtf8(nat)).c_str());
				}
				if (!fs::is_directory(nat, ec)) continue;
				fs::recursive_directory_iterator it(nat,
					fs::directory_options::skip_permission_denied, ec);
				if (ec) continue;
				for (auto cur = it; cur != fs::recursive_directory_iterator(); cur.increment(ec)) {
					if (ec) break;
					if (max_depth >= 0 && cur.depth() >= max_depth) cur.disable_recursion_pending();
					fs::path p = cur->path();
					if (!pass_type(p)) continue;
					if (!match_name(pathToUtf8(p.filename()))) continue;
					std::printf("%s\n", exec.pathConv().toPosix(pathToUtf8(p)).c_str());
				}
			}
			std::fflush(stdout);
			return rc;
		}

		// ---- tar (ustar, uncompressed) -------------------------------------

		struct TarHeader {
			char name[100];
			char mode[8];
			char uid[8];
			char gid[8];
			char size[12];
			char mtime[12];
			char chksum[8];
			char typeflag;
			char linkname[100];
			char magic[6];     // "ustar\0"
			char version[2];   // "00"
			char uname[32];
			char gname[32];
			char devmajor[8];
			char devminor[8];
			char prefix[155];
			char pad[12];
		};
		static_assert(sizeof(TarHeader) == 512, "tar header must be 512 bytes");

		void tarOctal(char* dst, std::size_t width, std::uintmax_t v) {
			std::string s;
			while (v > 0) { s.insert(s.begin(), char('0' + (v & 7))); v >>= 3; }
			if (s.empty()) s = "0";
			while (s.size() < width - 1) s.insert(s.begin(), '0');
			std::memset(dst, 0, width);
			std::memcpy(dst, s.data(), (std::min)(s.size(), width - 1));
		}

		std::uintmax_t tarParseOctal(const char* p, std::size_t n) {
			std::uintmax_t v = 0;
			for (std::size_t i = 0; i < n && p[i] && p[i] != ' '; ++i) {
				if (p[i] < '0' || p[i] > '7') break;
				v = (v << 3) | (p[i] - '0');
			}
			return v;
		}

		void tarFillChecksum(TarHeader* h) {
			std::memset(h->chksum, ' ', sizeof(h->chksum));
			std::uintmax_t s = 0;
			const unsigned char* p = reinterpret_cast<const unsigned char*>(h);
			for (std::size_t i = 0; i < sizeof(*h); ++i) s += p[i];
			tarOctal(h->chksum, 7, s);
			h->chksum[6] = '\0';
			h->chksum[7] = ' ';
		}

		bool tarWriteEntry(FILE* out, const fs::path& src, const std::string& rel,
		                   bool verbose) {
			std::error_code ec;
			fs::file_status st = fs::symlink_status(src, ec);
			if (ec) return false;
			TarHeader h{};
			std::string name = rel;
			std::replace(name.begin(), name.end(), '\\', '/');
			if (fs::is_directory(st)) {
				if (!name.empty() && name.back() != '/') name.push_back('/');
				h.typeflag = '5';
			} else {
				h.typeflag = '0';
			}
			if (name.size() >= sizeof(h.name)) {
				std::fprintf(stderr, "wbsh: tar: name too long: %s\n", name.c_str());
				return false;
			}
			std::strncpy(h.name, name.c_str(), sizeof(h.name) - 1);
			tarOctal(h.mode, 8, 0644);
			tarOctal(h.uid,  8, 0);
			tarOctal(h.gid,  8, 0);
			std::uintmax_t size = (h.typeflag == '5') ? 0 : fs::file_size(src, ec);
			if (ec) size = 0;
			tarOctal(h.size, 12, size);
			std::time_t mt = 0;
			{
				auto t = fs::last_write_time(src, ec);
				if (!ec) {
					mt = std::chrono::system_clock::to_time_t(
						std::chrono::system_clock::now()
						+ std::chrono::duration_cast<std::chrono::system_clock::duration>(
							t - fs::file_time_type::clock::now()));
				}
			}
			tarOctal(h.mtime, 12, static_cast<std::uintmax_t>(mt));
			std::memcpy(h.magic, "ustar\0", 6);
			std::memcpy(h.version, "00", 2);
			tarFillChecksum(&h);
			std::fwrite(&h, 1, sizeof(h), out);
			if (verbose) std::fprintf(stderr, "%s\n", name.c_str());
			if (h.typeflag != '5' && size > 0) {
				FILE* in = openUtf8(pathToUtf8(src), "rb");
				if (!in) return false;
				char buf[512];
				std::uintmax_t left = size;
				while (left > 0) {
					std::size_t want = (std::min)(static_cast<std::uintmax_t>(512), left);
					std::size_t got = std::fread(buf, 1, want, in);
					if (got == 0) break;
					std::fwrite(buf, 1, got, out);
					if (got < 512) {
						char zero[512] = {};
						std::fwrite(zero, 1, 512 - got, out);
					}
					left -= got;
				}
				std::fclose(in);
			}
			return true;
		}

		int tarCreate(Executor& exec, const std::string& archive,
		              const std::vector<std::string>& items, bool verbose) {
			FILE* out = fopenNative(exec, archive, "wb");
			if (!out) { perr("tar", archive + ": " + std::strerror(errno)); return 1; }
			int rc = 0;
			for (const auto& item : items) {
				fs::path nat(toNative(exec, item));
				std::error_code ec;
				if (!fs::exists(nat, ec)) {
					perr("tar", item + ": not found");
					rc = 1;
					continue;
				}
				if (fs::is_directory(nat, ec)) {
					tarWriteEntry(out, nat, item, verbose);
					fs::recursive_directory_iterator it(nat,
						fs::directory_options::skip_permission_denied, ec);
					if (ec) continue;
					for (auto cur = it; cur != fs::recursive_directory_iterator(); cur.increment(ec)) {
						if (ec) break;
						std::error_code rec;
						std::string rel = item + "/" + pathToUtf8(fs::relative(cur->path(), nat, rec));
						tarWriteEntry(out, cur->path(), rel, verbose);
					}
				} else {
					tarWriteEntry(out, nat, item, verbose);
				}
			}
			char zero[1024] = {};
			std::fwrite(zero, 1, 1024, out);
			std::fclose(out);
			return rc;
		}

		int tarExtract(Executor& exec, const std::string& archive, bool verbose, bool list_only) {
			FILE* in = fopenNative(exec, archive, "rb");
			if (!in) { perr("tar", archive + ": " + std::strerror(errno)); return 1; }
			int rc = 0;
			while (true) {
				TarHeader h{};
				std::size_t got = std::fread(&h, 1, sizeof(h), in);
				if (got != sizeof(h)) break;
				bool all_zero = true;
				const unsigned char* p = reinterpret_cast<const unsigned char*>(&h);
				for (std::size_t k = 0; k < sizeof(h); ++k) {
					if (p[k]) { all_zero = false; break; }
				}
				if (all_zero) break;
				std::string name(h.name, ::strnlen(h.name, sizeof(h.name)));
				std::uintmax_t size = tarParseOctal(h.size, sizeof(h.size));
				bool is_dir = (h.typeflag == '5')
					|| (!name.empty() && name.back() == '/');
				if (verbose || list_only) std::printf("%s\n", name.c_str());
				if (list_only) {
					std::uintmax_t skip = (size + 511) & ~static_cast<std::uintmax_t>(511);
					std::fseek(in, static_cast<long>(skip), SEEK_CUR);
					continue;
				}
				if (is_dir) {
					std::error_code ec;
					fs::create_directories(toNative(exec, name), ec);
					std::uintmax_t skip = (size + 511) & ~static_cast<std::uintmax_t>(511);
					std::fseek(in, static_cast<long>(skip), SEEK_CUR);
					continue;
				}
				// Ensure parent dir.
				{
					fs::path p(toNative(exec, name));
					std::error_code ec;
					fs::create_directories(p.parent_path(), ec);
				}
				FILE* out = fopenNative(exec, name, "wb");
				if (!out) {
					perr("tar", name + ": " + std::strerror(errno));
					rc = 1;
					std::uintmax_t skip = (size + 511) & ~static_cast<std::uintmax_t>(511);
					std::fseek(in, static_cast<long>(skip), SEEK_CUR);
					continue;
				}
				char buf[512];
				std::uintmax_t left = size;
				while (left > 0) {
					std::size_t want = 512;
					std::size_t r = std::fread(buf, 1, want, in);
					if (r == 0) break;
					std::size_t writeN = (left >= 512) ? 512 : static_cast<std::size_t>(left);
					std::fwrite(buf, 1, writeN, out);
					left -= writeN;
				}
				std::fclose(out);
			}
			std::fclose(in);
			return rc;
		}

		int builtin_tar(Executor& exec, const std::vector<std::string>& args) {
			char mode = 0;
			bool verbose = false;
			std::string archive;
			std::vector<std::string> items;
			for (std::size_t i = 0; i < args.size(); ++i) {
				const std::string& a = args[i];
				if (!a.empty() && a[0] == '-' && a.size() > 1) {
					for (std::size_t k = 1; k < a.size(); ++k) {
						char c = a[k];
						if (c == 'c' || c == 'x' || c == 't') mode = c;
						else if (c == 'v') verbose = true;
						else if (c == 'f') {
							if (k + 1 < a.size()) archive = a.substr(k + 1);
							else if (i + 1 < args.size()) archive = args[++i];
							k = a.size();
						}
					}
					continue;
				}
				items.push_back(a);
			}
			if (mode == 0) { perr("tar", "specify -c, -x, or -t"); return 2; }
			if (archive.empty()) { perr("tar", "missing -f ARCHIVE"); return 2; }
			switch (mode) {
			case 'c': return tarCreate(exec, archive, items, verbose);
			case 'x': return tarExtract(exec, archive, verbose, false);
			case 't': return tarExtract(exec, archive, false, true);
			}
			return 2;
		}

		// ---- curl (basic HTTP/HTTPS GET via WinHTTP) -----------------------

#ifdef _WIN32
		std::wstring utf8ToWideLocal(const std::string& s) {
			if (s.empty()) return {};
			int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
			std::wstring r(n, L'\0');
			MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), r.data(), n);
			return r;
		}
#endif

		int builtin_curl(Executor& exec, const std::vector<std::string>& args) {
#ifndef _WIN32
			(void)exec; (void)args;
			perr("curl", "not supported");
			return 1;
#else
			bool silent = false;
			bool include_headers = false;
			bool head_only = false;
			bool save_remote = false;
			std::string output_file;
			std::string method = "GET";
			std::string body_data;
			std::vector<std::string> headers;
			std::string url;

			for (std::size_t i = 0; i < args.size(); ++i) {
				const std::string& a = args[i];
				// Long forms.
				if (a == "--silent") { silent = true; continue; }
				if (a == "--include") { include_headers = true; continue; }
				if (a == "--head") { head_only = true; method = "HEAD"; continue; }
				if (a == "--location") continue;
				if (a == "--remote-name") { save_remote = true; continue; }
				if (a == "--output" && i + 1 < args.size()) { output_file = args[++i]; continue; }
				if (a == "--request" && i + 1 < args.size()) { method = args[++i]; continue; }
				if (a == "--header" && i + 1 < args.size()) { headers.push_back(args[++i]); continue; }
				if (a == "--data" && i + 1 < args.size()) {
					body_data = args[++i];
					if (method == "GET") method = "POST";
					continue;
				}
				// Short flags, possibly bundled. The value-taking ones (-o/-X/-H/-d)
				// must be the last char in the cluster.
				if (a.size() > 1 && a[0] == '-' && a[1] != '-') {
					bool consumed_next = false;
					for (std::size_t k = 1; k < a.size(); ++k) {
						char c = a[k];
						bool last = (k + 1 == a.size());
						switch (c) {
						case 's': silent = true; break;
						case 'i': include_headers = true; break;
						case 'I': head_only = true; method = "HEAD"; break;
						case 'L': break;
						case 'O': save_remote = true; break;
						case 'o':
							if (last && i + 1 < args.size()) {
								output_file = args[++i];
								consumed_next = true;
							}
							break;
						case 'X':
							if (last && i + 1 < args.size()) {
								method = args[++i];
								consumed_next = true;
							}
							break;
						case 'H':
							if (last && i + 1 < args.size()) {
								headers.push_back(args[++i]);
								consumed_next = true;
							}
							break;
						case 'd':
							if (last && i + 1 < args.size()) {
								body_data = args[++i];
								if (method == "GET") method = "POST";
								consumed_next = true;
							}
							break;
						default: break;
						}
					}
					(void)consumed_next;
					continue;
				}
				if (!a.empty() && a[0] != '-') url = a;
			}

			if (url.empty()) {
				if (!silent) perr("curl", "no URL specified");
				return 2;
			}

			// Default to https:// if no scheme.
			if (url.find("://") == std::string::npos) url = "https://" + url;

			URL_COMPONENTS uc{};
			uc.dwStructSize = sizeof(uc);
			wchar_t hostBuf[256] = {};
			wchar_t pathBuf[2048] = {};
			uc.lpszHostName = hostBuf;
			uc.dwHostNameLength = sizeof(hostBuf) / sizeof(hostBuf[0]);
			uc.lpszUrlPath = pathBuf;
			uc.dwUrlPathLength = sizeof(pathBuf) / sizeof(pathBuf[0]);

			std::wstring wurl = utf8ToWideLocal(url);
			if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
				if (!silent) perr("curl", "bad URL");
				return 1;
			}
			bool is_https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
			std::wstring path = uc.dwUrlPathLength
				? std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength)
				: std::wstring(L"/");

			HINTERNET hSession = WinHttpOpen(L"wbsh-curl/0.1",
				WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
				WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
			if (!hSession) { if (!silent) perr("curl", "WinHttpOpen failed"); return 1; }

			HINTERNET hConnect = WinHttpConnect(hSession, hostBuf, uc.nPort, 0);
			if (!hConnect) {
				WinHttpCloseHandle(hSession);
				if (!silent) perr("curl", "connect failed");
				return 1;
			}

			std::wstring wmethod = utf8ToWideLocal(method);
			HINTERNET hRequest = WinHttpOpenRequest(hConnect, wmethod.c_str(),
				path.c_str(), nullptr, WINHTTP_NO_REFERER,
				WINHTTP_DEFAULT_ACCEPT_TYPES,
				is_https ? WINHTTP_FLAG_SECURE : 0);
			if (!hRequest) {
				WinHttpCloseHandle(hConnect);
				WinHttpCloseHandle(hSession);
				if (!silent) perr("curl", "open request failed");
				return 1;
			}

			// Add custom headers.
			for (const auto& h : headers) {
				std::wstring wh = utf8ToWideLocal(h + "\r\n");
				WinHttpAddRequestHeaders(hRequest, wh.c_str(),
					(DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
			}

			BOOL r = WinHttpSendRequest(hRequest,
				WINHTTP_NO_ADDITIONAL_HEADERS, 0,
				body_data.empty() ? WINHTTP_NO_REQUEST_DATA
				                  : const_cast<char*>(body_data.data()),
				static_cast<DWORD>(body_data.size()),
				static_cast<DWORD>(body_data.size()), 0);
			if (!r || !WinHttpReceiveResponse(hRequest, nullptr)) {
				DWORD err = GetLastError();
				WinHttpCloseHandle(hRequest);
				WinHttpCloseHandle(hConnect);
				WinHttpCloseHandle(hSession);
				if (!silent) std::fprintf(stderr,
					"wbsh: curl: request failed (err=%lu)\n", err);
				return 1;
			}

			DWORD status_code = 0;
			DWORD sz = sizeof(status_code);
			WinHttpQueryHeaders(hRequest,
				WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX,
				&status_code, &sz, WINHTTP_NO_HEADER_INDEX);

			// Decide where to write. If -O, derive from URL path.
			FILE* out = stdout;
			std::string opened_path;
			if (save_remote) {
				std::string p(path.begin(), path.end());
				auto slash = p.find_last_of('/');
				std::string name = (slash == std::string::npos) ? p : p.substr(slash + 1);
				if (name.empty() || name == "/") name = "index.html";
				output_file = name;
			}
			if (!output_file.empty()) {
				out = fopenNative(exec, output_file, "wb");
				if (!out) {
					WinHttpCloseHandle(hRequest);
					WinHttpCloseHandle(hConnect);
					WinHttpCloseHandle(hSession);
					if (!silent) perr("curl", output_file + ": cannot open");
					return 1;
				}
				opened_path = output_file;
			}

			if (include_headers || head_only) {
				DWORD hdr_size = 0;
				WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF,
					WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &hdr_size,
					WINHTTP_NO_HEADER_INDEX);
				if (hdr_size > 0) {
					std::vector<wchar_t> hbuf(hdr_size / sizeof(wchar_t) + 1);
					if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF,
						WINHTTP_HEADER_NAME_BY_INDEX,
						hbuf.data(), &hdr_size, WINHTTP_NO_HEADER_INDEX)) {
						int n = WideCharToMultiByte(CP_UTF8, 0, hbuf.data(), -1,
							nullptr, 0, nullptr, nullptr);
						std::string utf(n, '\0');
						WideCharToMultiByte(CP_UTF8, 0, hbuf.data(), -1,
							utf.data(), n, nullptr, nullptr);
						std::fwrite(utf.data(), 1, utf.size() ? utf.size() - 1 : 0, out);
					}
				}
			}

			if (!head_only) {
				while (true) {
					DWORD avail = 0;
					if (!WinHttpQueryDataAvailable(hRequest, &avail)) break;
					if (avail == 0) break;
					std::vector<char> buf(avail);
					DWORD got = 0;
					if (!WinHttpReadData(hRequest, buf.data(), avail, &got)) break;
					if (got == 0) break;
					std::fwrite(buf.data(), 1, got, out);
				}
			}

			WinHttpCloseHandle(hRequest);
			WinHttpCloseHandle(hConnect);
			WinHttpCloseHandle(hSession);
			if (out != stdout) std::fclose(out);
			std::fflush(stdout);
			return (status_code >= 400) ? 22 : 0;
#endif
		}

		// ---- Hashes (md5sum / sha1sum / sha256sum / sha512sum) -------------

#ifdef _WIN32
		bool computeHashHex(LPCWSTR algId, FILE* fp, std::string& hex_out) {
			BCRYPT_ALG_HANDLE hAlg = nullptr;
			if (BCryptOpenAlgorithmProvider(&hAlg, algId, nullptr, 0) != 0) return false;
			DWORD hashLen = 0, dummy = 0;
			BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH,
				reinterpret_cast<PUCHAR>(&hashLen), sizeof(hashLen), &dummy, 0);
			BCRYPT_HASH_HANDLE hHash = nullptr;
			if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) != 0) {
				BCryptCloseAlgorithmProvider(hAlg, 0);
				return false;
			}
			unsigned char buf[8192];
			while (true) {
				std::size_t n = std::fread(buf, 1, sizeof(buf), fp);
				if (n == 0) break;
				BCryptHashData(hHash, buf, static_cast<ULONG>(n), 0);
			}
			std::vector<unsigned char> bytes(hashLen);
			BCryptFinishHash(hHash, bytes.data(), hashLen, 0);
			BCryptDestroyHash(hHash);
			BCryptCloseAlgorithmProvider(hAlg, 0);
			char hex[3];
			hex_out.reserve(hashLen * 2);
			for (DWORD i = 0; i < hashLen; ++i) {
				std::snprintf(hex, sizeof(hex), "%02x", bytes[i]);
				hex_out += hex;
			}
			return true;
		}
#endif

		int builtinHashImpl(Executor& exec, const std::vector<std::string>& args,
		                    const wchar_t* algId, const char* cmd) {
			std::vector<std::string> files;
			for (const auto& a : args) {
				if (!a.empty() && a[0] == '-' && a != "-") continue;
				files.push_back(a);
			}
			if (files.empty()) files.push_back("-");
			int rc = 0;
			for (const auto& f : files) {
				FILE* fp = (f == "-") ? stdin : fopenNative(exec, f, "rb");
				if (!fp) {
					std::fprintf(stderr, "wbsh: %s: %s: %s\n",
						cmd, f.c_str(), std::strerror(errno));
					rc = 1;
					continue;
				}
				std::string hex;
#ifdef _WIN32
				bool ok = computeHashHex(algId, fp, hex);
#else
				bool ok = false;
				(void)algId;
#endif
				if (fp != stdin) std::fclose(fp);
				if (!ok) {
					std::fprintf(stderr, "wbsh: %s: hash failed\n", cmd);
					rc = 1;
					continue;
				}
				std::printf("%s  %s\n", hex.c_str(), f.c_str());
			}
			std::fflush(stdout);
			return rc;
		}

		int builtin_md5sum(Executor& e, const std::vector<std::string>& a) {
			return builtinHashImpl(e, a, BCRYPT_MD5_ALGORITHM, "md5sum");
		}
		int builtin_sha1sum(Executor& e, const std::vector<std::string>& a) {
			return builtinHashImpl(e, a, BCRYPT_SHA1_ALGORITHM, "sha1sum");
		}
		int builtin_sha256sum(Executor& e, const std::vector<std::string>& a) {
			return builtinHashImpl(e, a, BCRYPT_SHA256_ALGORITHM, "sha256sum");
		}
		int builtin_sha512sum(Executor& e, const std::vector<std::string>& a) {
			return builtinHashImpl(e, a, BCRYPT_SHA512_ALGORITHM, "sha512sum");
		}

		// ---- base64 ---------------------------------------------------------

		const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

		std::string base64Encode(const std::vector<unsigned char>& in) {
			std::string out;
			std::size_t n = in.size();
			out.reserve(((n + 2) / 3) * 4);
			for (std::size_t i = 0; i < n; i += 3) {
				unsigned int v = static_cast<unsigned int>(in[i]) << 16;
				if (i + 1 < n) v |= static_cast<unsigned int>(in[i + 1]) << 8;
				if (i + 2 < n) v |= static_cast<unsigned int>(in[i + 2]);
				out.push_back(kB64[(v >> 18) & 0x3F]);
				out.push_back(kB64[(v >> 12) & 0x3F]);
				out.push_back((i + 1 < n) ? kB64[(v >> 6) & 0x3F] : '=');
				out.push_back((i + 2 < n) ? kB64[v & 0x3F] : '=');
			}
			return out;
		}

		std::vector<unsigned char> base64Decode(const std::string& in, bool& ok) {
			ok = true;
			int dec[256];
			for (int i = 0; i < 256; ++i) dec[i] = -1;
			for (int i = 0; i < 64; ++i) dec[static_cast<unsigned char>(kB64[i])] = i;
			std::vector<unsigned char> out;
			int v = 0, bits = 0;
			for (char c : in) {
				if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
				if (c == '=') break;
				int d = dec[static_cast<unsigned char>(c)];
				if (d < 0) { ok = false; return out; }
				v = (v << 6) | d;
				bits += 6;
				if (bits >= 8) {
					bits -= 8;
					out.push_back(static_cast<unsigned char>((v >> bits) & 0xFF));
				}
			}
			return out;
		}

		int builtin_base64(Executor& exec, const std::vector<std::string>& args) {
			bool decode = false;
			std::string file = "-";
			std::size_t wrap = 76;
			for (std::size_t i = 0; i < args.size(); ++i) {
				const std::string& a = args[i];
				if (a == "-d" || a == "--decode") { decode = true; continue; }
				if (a == "-w" && i + 1 < args.size()) {
					try { wrap = std::stoul(args[++i]); } catch (...) {}
					continue;
				}
				if (!a.empty() && a[0] == '-' && a != "-") continue;
				file = a;
			}
			FILE* fp = (file == "-") ? stdin : fopenNative(exec, file, "rb");
			if (!fp) {
				std::fprintf(stderr, "wbsh: base64: %s: %s\n",
					file.c_str(), std::strerror(errno));
				return 1;
			}
			std::vector<unsigned char> bytes;
			int c;
			while ((c = std::fgetc(fp)) != EOF) bytes.push_back(static_cast<unsigned char>(c));
			if (fp != stdin) std::fclose(fp);
			if (decode) {
				std::string s(bytes.begin(), bytes.end());
				bool ok;
				auto out = base64Decode(s, ok);
				if (!ok) { perr("base64", "invalid input"); return 1; }
				std::fwrite(out.data(), 1, out.size(), stdout);
			} else {
				std::string out = base64Encode(bytes);
				if (wrap > 0) {
					for (std::size_t i = 0; i < out.size(); i += wrap) {
						std::size_t take = (std::min)(wrap, out.size() - i);
						std::fwrite(out.data() + i, 1, take, stdout);
						std::fputc('\n', stdout);
					}
				} else {
					std::fwrite(out.data(), 1, out.size(), stdout);
					std::fputc('\n', stdout);
				}
			}
			std::fflush(stdout);
			return 0;
		}

		// ---- cmp ------------------------------------------------------------

		int builtin_cmp(Executor& exec, const std::vector<std::string>& args) {
			std::vector<std::string> files;
			bool quiet = false;
			for (const auto& a : args) {
				if (a == "-s" || a == "--silent" || a == "--quiet") { quiet = true; continue; }
				if (!a.empty() && a[0] == '-') continue;
				files.push_back(a);
			}
			if (files.size() < 2) {
				perr("cmp", "usage: cmp [-s] FILE1 FILE2");
				return 2;
			}
			FILE* f1 = fopenNative(exec, files[0], "rb");
			if (!f1) { if (!quiet) perr("cmp", files[0] + ": " + std::strerror(errno)); return 2; }
			FILE* f2 = fopenNative(exec, files[1], "rb");
			if (!f2) { std::fclose(f1); if (!quiet) perr("cmp", files[1] + ": " + std::strerror(errno)); return 2; }
			long long byte = 0, line = 1;
			int rc = 0;
			while (true) {
				int a = std::fgetc(f1);
				int b = std::fgetc(f2);
				if (a == EOF && b == EOF) break;
				++byte;
				if (a == EOF || b == EOF) {
					if (!quiet) {
						std::fprintf(stderr, "cmp: EOF on %s\n",
							(a == EOF ? files[0].c_str() : files[1].c_str()));
					}
					rc = 1;
					break;
				}
				if (a != b) {
					if (!quiet) {
						std::printf("%s %s differ: byte %lld, line %lld\n",
							files[0].c_str(), files[1].c_str(), byte, line);
					}
					rc = 1;
					break;
				}
				if (a == '\n') ++line;
			}
			std::fclose(f1);
			std::fclose(f2);
			return rc;
		}

		// ---- diff (line-LCS, normal output) --------------------------------

		void diffEmitRange(int a, int b) {
			if (a == b) std::printf("%d", a);
			else        std::printf("%d,%d", a, b);
		}

		int builtin_diff(Executor& exec, const std::vector<std::string>& args) {
			bool brief = false;
			bool unified = false;
			int context = 3;
			std::vector<std::string> files;
			for (std::size_t i = 0; i < args.size(); ++i) {
				const std::string& a = args[i];
				if (a == "-q" || a == "--brief") { brief = true; continue; }
				if (a == "-u" || a == "--unified") { unified = true; continue; }
				if (a.size() > 2 && a.compare(0, 2, "-U") == 0) {
					unified = true;
					try { context = std::stoi(a.substr(2)); } catch (...) {}
					continue;
				}
				if (a == "-U" && i + 1 < args.size()) {
					unified = true;
					try { context = std::stoi(args[++i]); } catch (...) {}
					continue;
				}
				if (!a.empty() && a[0] == '-') continue;
				files.push_back(a);
			}
			if (files.size() < 2) {
				perr("diff", "usage: diff [-q] FILE1 FILE2");
				return 2;
			}
			std::vector<std::string> A, B;
			if (!readAllLines(exec, files[0], A) || !readAllLines(exec, files[1], B)) {
				return 2;
			}
			if (A == B) return 0;
			if (brief) {
				std::printf("Files %s and %s differ\n",
					files[0].c_str(), files[1].c_str());
				return 1;
			}
			// LCS table.
			std::size_t m = A.size(), n = B.size();
			std::vector<std::vector<int>> L(m + 1, std::vector<int>(n + 1, 0));
			for (std::size_t i = 1; i <= m; ++i) {
				for (std::size_t j = 1; j <= n; ++j) {
					if (A[i - 1] == B[j - 1]) L[i][j] = L[i - 1][j - 1] + 1;
					else                      L[i][j] = (std::max)(L[i - 1][j], L[i][j - 1]);
				}
			}
			// Unified-diff path: walk LCS into ' '/'-'/'+' items and group
			// runs of changes (with `context` lines on each side) into hunks.
			if (unified) {
				struct Item { char kind; int a, b; std::string text; };
				std::vector<Item> items;
				int ai = static_cast<int>(m), bj = static_cast<int>(n);
				while (ai > 0 || bj > 0) {
					if (ai > 0 && bj > 0 && A[ai - 1] == B[bj - 1]) {
						items.push_back({ ' ', ai, bj, A[ai - 1] });
						--ai; --bj;
					} else if (bj > 0 && (ai == 0 || L[ai][bj - 1] >= L[ai - 1][bj])) {
						items.push_back({ '+', 0, bj, B[bj - 1] });
						--bj;
					} else {
						items.push_back({ '-', ai, 0, A[ai - 1] });
						--ai;
					}
				}
				std::reverse(items.begin(), items.end());

				std::printf("--- %s\n+++ %s\n",
					files[0].c_str(), files[1].c_str());

				int N = static_cast<int>(items.size());
				int i = 0;
				while (i < N) {
					while (i < N && items[i].kind == ' ') ++i;
					if (i >= N) break;
					int change_start = i;
					int last_change = i;
					int j = i + 1;
					while (j < N) {
						if (items[j].kind != ' ') {
							last_change = j;
							++j;
						} else {
							int run = 0, k = j;
							while (k < N && items[k].kind == ' ' && run < 2 * context) {
								++run; ++k;
							}
							if (k < N && items[k].kind != ' ') {
								j = k;
							} else {
								break;
							}
						}
					}
					int hstart = (std::max)(0, change_start - context);
					int hend   = (std::min)(N - 1, last_change + context);
					int a_start = 0, a_count = 0, b_start = 0, b_count = 0;
					for (int k = hstart; k <= hend; ++k) {
						if (items[k].kind != '+') {
							if (a_count == 0) a_start = items[k].a;
							++a_count;
						}
						if (items[k].kind != '-') {
							if (b_count == 0) b_start = items[k].b;
							++b_count;
						}
					}
					std::printf("@@ -%d,%d +%d,%d @@\n",
						a_count == 0 ? 0 : a_start, a_count,
						b_count == 0 ? 0 : b_start, b_count);
					for (int k = hstart; k <= hend; ++k) {
						std::putchar(items[k].kind);
						std::printf("%s\n", items[k].text.c_str());
					}
					i = hend + 1;
				}
				return 1;
			}

			// Normal-format path: backtrack to produce a hunk list.
			struct Hunk { int a1, a2, b1, b2; char kind; };
			std::vector<Hunk> hunks;
			std::size_t i = m, j = n;
			while (i > 0 || j > 0) {
				if (i > 0 && j > 0 && A[i - 1] == B[j - 1]) {
					--i; --j;
					continue;
				}
				// Walk a contiguous mismatch run.
				std::size_t ei = i, ej = j;
				while (i > 0 && j > 0 && A[i - 1] != B[j - 1]) {
					if (L[i - 1][j] >= L[i][j - 1]) --i;
					else                            --j;
				}
				while (i > 0 && (j == 0 || L[i - 1][j] >= L[i][j])) --i;
				while (j > 0 && (i == 0 || L[i][j - 1] >  L[i][j])) --j;
				int a1 = static_cast<int>(i + 1), a2 = static_cast<int>(ei);
				int b1 = static_cast<int>(j + 1), b2 = static_cast<int>(ej);
				char kind;
				if (a1 > a2 && b1 <= b2)      { kind = 'a'; --a1; }   // pure addition
				else if (b1 > b2 && a1 <= a2) { kind = 'd'; --b1; }   // pure deletion
				else                          { kind = 'c'; }
				hunks.push_back({ a1, a2, b1, b2, kind });
			}
			std::reverse(hunks.begin(), hunks.end());
			// Emit normal-format diff: `<` lines (from A) come first, then
			// `---` separator on `c`, then `>` lines (from B).
			for (const auto& h : hunks) {
				diffEmitRange(h.a1, h.a2);
				std::putchar(h.kind);
				diffEmitRange(h.b1, h.b2);
				std::putchar('\n');
				if (h.kind == 'd' || h.kind == 'c') {
					for (int k = h.a1; k <= h.a2; ++k) {
						if (k - 1 < static_cast<int>(A.size())) {
							std::printf("< %s\n", A[k - 1].c_str());
						}
					}
				}
				if (h.kind == 'c') std::puts("---");
				if (h.kind == 'a' || h.kind == 'c') {
					for (int k = h.b1; k <= h.b2; ++k) {
						if (k - 1 < static_cast<int>(B.size())) {
							std::printf("> %s\n", B[k - 1].c_str());
						}
					}
				}
			}
			return 1;
		}

		// ---- du -------------------------------------------------------------

		std::uintmax_t walkSize(const fs::path& p, std::error_code& ec) {
			std::uintmax_t total = 0;
			if (fs::is_regular_file(p, ec)) {
				return fs::file_size(p, ec);
			}
			if (!fs::is_directory(p, ec)) return 0;
			fs::recursive_directory_iterator it(p,
				fs::directory_options::skip_permission_denied, ec);
			if (ec) return 0;
			for (auto cur = it; cur != fs::recursive_directory_iterator(); cur.increment(ec)) {
				if (ec) break;
				std::error_code fec;
				if (cur->is_regular_file(fec)) {
					std::uintmax_t s = cur->file_size(fec);
					if (!fec) total += s;
				}
			}
			return total;
		}

		int builtin_du(Executor& exec, const std::vector<std::string>& args) {
			bool summary = false, human = false, all = false;
			std::vector<std::string> paths;
			for (const auto& a : args) {
				if (a == "-s" || a == "--summarize") summary = true;
				else if (a == "-h" || a == "--human-readable") human = true;
				else if (a == "-a" || a == "--all") all = true;
				else if (!a.empty() && a[0] == '-') continue;
				else paths.push_back(a);
			}
			if (paths.empty()) paths.push_back(".");
			auto fmt = [&](std::uintmax_t bytes) -> std::string {
				if (human) return humanSize(bytes);
				// Default: 1024-byte blocks, like GNU du.
				return std::to_string((bytes + 1023) / 1024);
			};
			int rc = 0;
			for (const auto& p : paths) {
				fs::path nat = toNative(exec, p);
				std::error_code ec;
				if (!fs::exists(nat, ec)) {
					std::fprintf(stderr, "wbsh: du: %s: %s\n",
						p.c_str(), ec.message().c_str());
					rc = 1;
					continue;
				}
				if (summary) {
					std::uintmax_t total = walkSize(nat, ec);
					std::printf("%s\t%s\n", fmt(total).c_str(), p.c_str());
					continue;
				}
				if (fs::is_regular_file(nat, ec)) {
					std::printf("%s\t%s\n",
						fmt(fs::file_size(nat, ec)).c_str(), p.c_str());
					continue;
				}
				// Walk each entry; print directory totals as we ascend.
				std::uintmax_t grand = 0;
				fs::recursive_directory_iterator it(nat,
					fs::directory_options::skip_permission_denied, ec);
				if (ec) { std::fprintf(stderr, "wbsh: du: %s\n", ec.message().c_str()); rc = 1; continue; }
				for (auto cur = it; cur != fs::recursive_directory_iterator(); cur.increment(ec)) {
					if (ec) break;
					std::error_code fec;
					if (cur->is_regular_file(fec)) {
						std::uintmax_t s = cur->file_size(fec);
						if (!fec) grand += s;
						if (all) {
							std::printf("%s\t%s\n",
								fmt(s).c_str(), pathToUtf8(cur->path()).c_str());
						}
					}
				}
				std::printf("%s\t%s\n", fmt(grand).c_str(), p.c_str());
			}
			return rc;
		}

		// ---- df -------------------------------------------------------------

		int builtin_df(Executor& exec, const std::vector<std::string>& args) {
			bool human = false;
			std::vector<std::string> paths;
			for (const auto& a : args) {
				if (a == "-h" || a == "--human-readable") human = true;
				else if (!a.empty() && a[0] == '-') continue;
				else paths.push_back(a);
			}
			std::vector<std::string> drives;
			if (paths.empty()) {
#ifdef _WIN32
				DWORD mask = GetLogicalDrives();
				for (int i = 0; i < 26; ++i) {
					if (mask & (1u << i)) {
						char d[4] = { static_cast<char>('A' + i), ':', '\\', 0 };
						drives.push_back(d);
					}
				}
#endif
			} else {
				for (const auto& p : paths) drives.push_back(pathToUtf8(toNative(exec, p)));
			}
			auto fmt = [&](std::uintmax_t b) -> std::string {
				if (human) return humanSize(b);
				return std::to_string(b / 1024);
			};
			std::printf("%-20s %12s %12s %12s %5s %s\n",
				"Filesystem", "1K-blocks", "Used", "Available", "Use%", "Mounted on");
			int rc = 0;
			for (const auto& d : drives) {
#ifdef _WIN32
				ULARGE_INTEGER avail{}, total{}, free_{};
				std::string root = d;
				if (!root.empty() && root.back() != '\\' && root.back() != '/') root.push_back('\\');
				if (!GetDiskFreeSpaceExA(root.c_str(), &avail, &total, &free_)) {
					rc = 1;
					continue;
				}
				std::uintmax_t t = total.QuadPart;
				std::uintmax_t a = avail.QuadPart;
				std::uintmax_t u = (t > a) ? (t - a) : 0;
				int pct = (t == 0) ? 0 : static_cast<int>((u * 100) / t);
				std::printf("%-20s %12s %12s %12s %4d%% %s\n",
					root.c_str(), fmt(t).c_str(), fmt(u).c_str(),
					fmt(a).c_str(), pct, root.c_str());
#else
				(void)d;
#endif
			}
			return rc;
		}

		// ---- sed (s/PAT/REPL/[g] only) --------------------------------------

		struct SedSubst {
			std::regex re;
			std::string repl;
			bool global = false;
		};

		bool sedParseSubst(const std::string& cmd, SedSubst& out, std::string& err) {
			if (cmd.size() < 4 || cmd[0] != 's') {
				err = "only s/PAT/REPL/[g] is supported";
				return false;
			}
			char delim = cmd[1];
			std::size_t i = 2;
			std::string pat;
			while (i < cmd.size() && cmd[i] != delim) {
				if (cmd[i] == '\\' && i + 1 < cmd.size()) {
					pat.push_back(cmd[i]);
					pat.push_back(cmd[i + 1]);
					i += 2;
					continue;
				}
				pat.push_back(cmd[i++]);
			}
			if (i >= cmd.size()) { err = "missing closing delimiter"; return false; }
			++i;   // skip delim
			std::string rep;
			while (i < cmd.size() && cmd[i] != delim) {
				if (cmd[i] == '\\' && i + 1 < cmd.size()) {
					char nx = cmd[i + 1];
					// Interpret common backslash escapes in the replacement.
					if (nx == 'n') { rep.push_back('\n'); i += 2; continue; }
					if (nx == 't') { rep.push_back('\t'); i += 2; continue; }
					if (nx >= '0' && nx <= '9') {
						// Backreference \N -> std::regex uses $N.
						rep.push_back('$');
						rep.push_back(nx);
						i += 2;
						continue;
					}
					rep.push_back(nx);
					i += 2;
					continue;
				}
				if (cmd[i] == '$') { rep += "$$"; ++i; continue; }
				rep.push_back(cmd[i++]);
			}
			if (i < cmd.size()) ++i;   // skip closing delim
			std::string flags = (i < cmd.size()) ? cmd.substr(i) : std::string();
			try {
				out.re = std::regex(pat);
			} catch (const std::regex_error& e) {
				err = std::string("regex: ") + e.what();
				return false;
			}
			out.repl = rep;
			out.global = flags.find('g') != std::string::npos;
			return true;
		}

		int builtin_sed(Executor& exec, const std::vector<std::string>& args) {
			bool quiet = false;
			std::string script;
			std::vector<std::string> files;
			for (std::size_t i = 0; i < args.size(); ++i) {
				const std::string& a = args[i];
				if (a == "-n" || a == "--quiet" || a == "--silent") { quiet = true; continue; }
				if (a == "-E" || a == "-r" || a == "--regexp-extended") continue;
				if (a == "--") { ++i; while (i < args.size()) files.push_back(args[i++]); break; }
				if (a == "-e" && i + 1 < args.size()) {
					if (!script.empty()) script.push_back(';');
					script += args[++i];
					continue;
				}
				if (a.size() > 2 && a.compare(0, 2, "-e") == 0) {
					if (!script.empty()) script.push_back(';');
					script += a.substr(2);
					continue;
				}
				if (script.empty() && (a.empty() || a[0] != '-')) {
					script = a;
					continue;
				}
				files.push_back(a);
			}
			if (script.empty()) { perr("sed", "no script provided"); return 2; }

			std::vector<SedSubst> cmds;
			{
				std::size_t s = 0;
				while (s <= script.size()) {
					std::size_t e = script.find(';', s);
					if (e == std::string::npos) e = script.size();
					std::string sub = script.substr(s, e - s);
					if (!sub.empty()) {
						SedSubst c;
						std::string err;
						if (!sedParseSubst(sub, c, err)) {
							std::fprintf(stderr, "wbsh: sed: %s\n", err.c_str());
							return 2;
						}
						cmds.push_back(std::move(c));
					}
					if (e == script.size()) break;
					s = e + 1;
				}
			}
			if (files.empty()) files.push_back("-");

			auto applyAll = [&](const std::string& line) -> std::string {
				std::string out = line;
				for (const auto& c : cmds) {
					if (c.global) {
						out = std::regex_replace(out, c.re, c.repl);
					} else {
						out = std::regex_replace(out, c.re, c.repl,
							std::regex_constants::format_first_only);
					}
				}
				return out;
			};

			int rc = 0;
			for (const auto& f : files) {
				FILE* fp = (f == "-") ? stdin : fopenNative(exec, f, "rb");
				if (!fp) { perr("sed", f + ": " + std::strerror(errno)); rc = 1; continue; }
				std::string line;
				int c;
				while ((c = std::fgetc(fp)) != EOF) {
					if (c == '\n') {
						std::string out = applyAll(line);
						if (!quiet) {
							std::fwrite(out.data(), 1, out.size(), stdout);
							std::fputc('\n', stdout);
						}
						line.clear();
					} else line.push_back(static_cast<char>(c));
				}
				if (!line.empty()) {
					std::string out = applyAll(line);
					if (!quiet) std::fwrite(out.data(), 1, out.size(), stdout);
				}
				if (fp != stdin) std::fclose(fp);
			}
			std::fflush(stdout);
			return rc;
		}

		// ---- stat -----------------------------------------------------------

		int builtin_stat(Executor& exec, const std::vector<std::string>& args) {
			if (args.empty()) { perr("stat", "missing operand"); return 1; }
			int rc = 0;
			for (const auto& p : args) {
				if (!p.empty() && p[0] == '-') continue;
				std::string nat = exec.pathConv().toWin32(p);
				struct stat st {};
				if (::stat(nat.c_str(), &st) != 0) {
					std::fprintf(stderr, "wbsh: stat: %s: %s\n",
						p.c_str(), std::strerror(errno));
					rc = 1;
					continue;
				}
				const char* type =
#ifdef S_ISDIR
					(S_ISDIR(st.st_mode) ? "directory" :
					 S_ISREG(st.st_mode) ? "regular file" : "special file");
#else
					((st.st_mode & S_IFMT) == S_IFDIR ? "directory" :
					 (st.st_mode & S_IFMT) == S_IFREG ? "regular file" : "special file");
#endif
				char tbuf[64];
				std::tm tm{};
#ifdef _WIN32
				localtime_s(&tm, &st.st_mtime);
#else
				localtime_r(&st.st_mtime, &tm);
#endif
				std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);
				std::printf("  File: %s\n", p.c_str());
				std::printf("  Size: %lld\tType: %s\n",
					static_cast<long long>(st.st_size), type);
				std::printf("Access: (%04o)\n",
					static_cast<unsigned int>(st.st_mode & 0777));
				std::printf("Modify: %s\n", tbuf);
			}
			return rc;
		}

		// ---- chmod (best-effort: only the readonly bit on Windows) ----------

		int builtin_chmod(Executor& exec, const std::vector<std::string>& args) {
			if (args.size() < 2) {
				perr("chmod", "usage: chmod MODE FILE...");
				return 1;
			}
			const std::string& mode = args[0];
			int rc = 0;
			// Map a small set of common modes to a "writable" bool.
			// +w / -w / u+w / u-w / numeric (NNN where owner has w) all
			// drive the read-only NTFS attribute.
			bool writable = true;
			if (mode == "-w" || mode == "u-w" || mode == "a-w" || mode == "go-w") {
				writable = false;
			} else if (mode == "+w" || mode == "u+w" || mode == "a+w" || mode == "go+w") {
				writable = true;
			} else if (mode.size() == 3
			    && std::isdigit((unsigned char)mode[0])
			    && std::isdigit((unsigned char)mode[1])
			    && std::isdigit((unsigned char)mode[2])) {
				// Owner-write bit (octal): mode[0] & 2.
				writable = ((mode[0] - '0') & 2) != 0;
			}
			for (std::size_t i = 1; i < args.size(); ++i) {
				std::string nat = exec.pathConv().toWin32(args[i]);
#ifdef _WIN32
				DWORD attr = GetFileAttributesA(nat.c_str());
				if (attr == INVALID_FILE_ATTRIBUTES) {
					std::fprintf(stderr, "wbsh: chmod: %s: not found\n", args[i].c_str());
					rc = 1;
					continue;
				}
				if (writable) attr &= ~FILE_ATTRIBUTE_READONLY;
				else          attr |=  FILE_ATTRIBUTE_READONLY;
				if (!SetFileAttributesA(nat.c_str(), attr)) {
					std::fprintf(stderr, "wbsh: chmod: %s: cannot change\n", args[i].c_str());
					rc = 1;
				}
#else
				(void)nat;
#endif
			}
			return rc;
		}

		// ---- ln [-s] [-f] TARGET LINK ---------------------------------------

		int builtin_ln(Executor& exec, const std::vector<std::string>& args) {
			bool symbolic = false;
			bool force = false;
			std::vector<std::string> rest;
			for (const auto& a : args) {
				if (a == "-s" || a == "--symbolic") symbolic = true;
				else if (a == "-f" || a == "--force") force = true;
				else if (a == "-sf" || a == "-fs") { symbolic = true; force = true; }
				else if (!a.empty() && a[0] == '-') continue;
				else rest.push_back(a);
			}
			if (rest.size() < 2) {
				perr("ln", "usage: ln [-s] [-f] TARGET LINK");
				return 1;
			}
			std::string target = rest[0];
			std::string link   = rest[1];
			std::string nat_target = exec.pathConv().toWin32(target);
			std::string nat_link   = exec.pathConv().toWin32(link);
#ifdef _WIN32
			if (force) DeleteFileA(nat_link.c_str());
			BOOL ok = FALSE;
			if (symbolic) {
				DWORD flags = 0;
				std::error_code ec;
				if (fs::is_directory(nat_target, ec)) flags |= 0x1;   // SYMBOLIC_LINK_FLAG_DIRECTORY
				flags |= 0x2;   // SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
				ok = CreateSymbolicLinkA(nat_link.c_str(), nat_target.c_str(), flags);
			} else {
				ok = CreateHardLinkA(nat_link.c_str(), nat_target.c_str(), nullptr);
			}
			if (!ok) {
				DWORD err = GetLastError();
				std::fprintf(stderr, "wbsh: ln: %s -> %s failed (err=%lu)\n",
					link.c_str(), target.c_str(), err);
				return 1;
			}
			return 0;
#else
			(void)nat_target; (void)nat_link;
			perr("ln", "not supported");
			return 1;
#endif
		}

		// ---- pushd / popd / dirs --------------------------------------------

		std::string currentCwdPosix(Executor& exec) {
			std::error_code ec;
			auto cwd = fs::current_path(ec);
			if (ec) return exec.env().get("PWD");
			return exec.pathConv().toPosix(pathToUtf8(cwd));
		}

		void printDirStack(Executor& exec, bool numbered) {
			const auto& s = exec.dirStack();
			if (s.empty()) {
				std::printf("%s\n", currentCwdPosix(exec).c_str());
				return;
			}
			if (numbered) {
				for (std::size_t i = 0; i < s.size(); ++i) {
					std::printf("%2zu  %s\n", i, s[i].c_str());
				}
			} else {
				for (std::size_t i = 0; i < s.size(); ++i) {
					if (i) std::fputc(' ', stdout);
					std::fputs(s[i].c_str(), stdout);
				}
				std::fputc('\n', stdout);
			}
		}

		int builtin_pushd(Executor& exec, const std::vector<std::string>& args) {
			auto& stack = exec.dirStack();
			std::string cur = currentCwdPosix(exec);
			if (stack.empty()) stack.push_back(cur);

			if (args.empty()) {
				if (stack.size() < 2) {
					perr("pushd", "no other directory");
					return 1;
				}
				std::swap(stack[0], stack[1]);
			} else {
				const std::string& target = args[0];
				fs::path nat = toNative(exec, target);
				std::error_code ec;
				fs::current_path(nat, ec);
				if (ec) { perr("pushd", target, ec); return 1; }
				stack.insert(stack.begin(), exec.pathConv().toPosix(
					pathToUtf8(fs::current_path(ec))));
			}
			// After pushd, the front of the stack must reflect new cwd.
			std::error_code ec;
			fs::current_path(toNative(exec, stack[0]), ec);
			if (!ec) {
				exec.env().set("OLDPWD", cur);
				exec.env().set("PWD", stack[0]);
			}
			printDirStack(exec, false);
			return 0;
		}

		int builtin_popd(Executor& exec, const std::vector<std::string>&) {
			auto& stack = exec.dirStack();
			if (stack.size() < 2) {
				perr("popd", "directory stack empty");
				return 1;
			}
			std::string old = stack.front();
			stack.erase(stack.begin());
			std::error_code ec;
			fs::current_path(exec.pathConv().toWin32(stack[0]), ec);
			if (ec) { perr("popd", stack[0], ec); return 1; }
			exec.env().set("OLDPWD", old);
			exec.env().set("PWD", stack[0]);
			printDirStack(exec, false);
			return 0;
		}

		int builtin_dirs(Executor& exec, const std::vector<std::string>& args) {
			bool numbered = false;
			bool clear = false;
			for (const auto& a : args) {
				if (a == "-v") numbered = true;
				else if (a == "-c") clear = true;
				else if (a == "-l") {}    // long form — not implemented
				else if (a == "-p") numbered = false;
			}
			if (clear) {
				exec.dirStack().clear();
				return 0;
			}
			if (exec.dirStack().empty()) {
				exec.dirStack().push_back(currentCwdPosix(exec));
			}
			printDirStack(exec, numbered);
			return 0;
		}

		// (`time` is now a parser-level reserved word; see parser.cpp /
		//  Executor::execPipeline.)

		// ---- gzip / gunzip / zcat -------------------------------------------

		// CRC-32/IEEE 802.3 (poly 0xEDB88320). Required for gzip footer.
		std::uint32_t crc32Update(std::uint32_t crc, const std::uint8_t* buf, std::size_t n) {
			static std::uint32_t table[256];
			static bool inited = false;
			if (!inited) {
				for (std::uint32_t i = 0; i < 256; ++i) {
					std::uint32_t c = i;
					for (int k = 0; k < 8; ++k)
						c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
					table[i] = c;
				}
				inited = true;
			}
			crc ^= 0xFFFFFFFFu;
			for (std::size_t i = 0; i < n; ++i)
				crc = table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
			return crc ^ 0xFFFFFFFFu;
		}

		// Read all bytes from a FILE*.
		std::vector<std::uint8_t> readAllBytesFromFile(FILE* f) {
			std::vector<std::uint8_t> out;
			std::uint8_t buf[4096];
			while (true) {
				std::size_t n = std::fread(buf, 1, sizeof(buf), f);
				if (n == 0) break;
				out.insert(out.end(), buf, buf + n);
			}
			return out;
		}

		// Strip the gzip wrapper and return the raw DEFLATE stream offset.
		// `compressed_end` is set to the byte just past the DEFLATE stream
		// (where CRC32 + ISIZE live).
		bool gzipParseHeader(const std::vector<std::uint8_t>& bytes,
		                     std::size_t& deflate_start,
		                     std::size_t& deflate_end) {
			if (bytes.size() < 18) return false;
			if (bytes[0] != 0x1F || bytes[1] != 0x8B) return false;
			if (bytes[2] != 8) return false;   // CM = deflate
			std::uint8_t flg = bytes[3];
			std::size_t i = 10;   // skip MTIME, XFL, OS
			if (flg & 0x04) {     // FEXTRA
				if (i + 2 > bytes.size()) return false;
				std::size_t xlen = bytes[i] | (bytes[i + 1] << 8);
				i += 2 + xlen;
			}
			if (flg & 0x08) {     // FNAME
				while (i < bytes.size() && bytes[i] != 0) ++i;
				if (i < bytes.size()) ++i;
			}
			if (flg & 0x10) {     // FCOMMENT
				while (i < bytes.size() && bytes[i] != 0) ++i;
				if (i < bytes.size()) ++i;
			}
			if (flg & 0x02) i += 2;   // FHCRC
			if (i + 8 > bytes.size()) return false;
			deflate_start = i;
			deflate_end   = bytes.size() - 8;
			return true;
		}

		// Encode `data` as gzip (using stored DEFLATE blocks — valid format,
		// no compression). Output written to `out`.
		void gzipEncodeStored(const std::vector<std::uint8_t>& data,
		                      std::vector<std::uint8_t>& out) {
			// Header.
			out.push_back(0x1F);
			out.push_back(0x8B);
			out.push_back(0x08);   // CM = deflate
			out.push_back(0x00);   // FLG = 0
			for (int k = 0; k < 4; ++k) out.push_back(0x00);   // MTIME
			out.push_back(0x00);   // XFL
			out.push_back(0xFF);   // OS = unknown
			// Stored blocks: each up to 65535 bytes.
			std::size_t i = 0;
			std::size_t n = data.size();
			if (n == 0) {
				// Empty stored final block.
				out.push_back(0x01);
				out.push_back(0x00); out.push_back(0x00);
				out.push_back(0xFF); out.push_back(0xFF);
			}
			while (i < n) {
				std::size_t take = n - i;
				if (take > 65535) take = 65535;
				bool last = (i + take >= n);
				out.push_back(last ? 0x01 : 0x00);
				out.push_back((std::uint8_t)(take & 0xFF));
				out.push_back((std::uint8_t)((take >> 8) & 0xFF));
				std::uint16_t nlen = (std::uint16_t)~take;
				out.push_back((std::uint8_t)(nlen & 0xFF));
				out.push_back((std::uint8_t)((nlen >> 8) & 0xFF));
				out.insert(out.end(), data.begin() + i, data.begin() + i + take);
				i += take;
			}
			// Footer: CRC32 + ISIZE (mod 2^32), little-endian.
			std::uint32_t crc = crc32Update(0, data.data(), data.size());
			std::uint32_t isize = (std::uint32_t)(data.size() & 0xFFFFFFFFu);
			for (int k = 0; k < 4; ++k) out.push_back((std::uint8_t)((crc >> (8 * k)) & 0xFF));
			for (int k = 0; k < 4; ++k) out.push_back((std::uint8_t)((isize >> (8 * k)) & 0xFF));
		}

		int builtin_gzip(Executor& exec, const std::vector<std::string>& args) {
			bool decompress = false;
			bool to_stdout = false;
			bool keep = false;
			bool force = false;
			std::vector<std::string> files;
			for (const auto& a : args) {
				if (a == "-d" || a == "--decompress") decompress = true;
				else if (a == "-c" || a == "--stdout") to_stdout = true;
				else if (a == "-k" || a == "--keep") keep = true;
				else if (a == "-f" || a == "--force") force = true;
				else if (!a.empty() && a[0] == '-' && a != "-") { /* ignore */ }
				else files.push_back(a);
			}
			(void)force;
			auto runOne = [&](const std::string& fname) -> int {
				std::vector<std::uint8_t> input;
				if (fname == "-" || fname.empty()) {
					input = readAllBytesFromFile(stdin);
				} else {
					FILE* f = fopenNative(exec, fname, "rb");
					if (!f) {
						perr(decompress ? "gunzip" : "gzip", fname,
						     std::error_code(errno, std::system_category()));
						return 1;
					}
					input = readAllBytesFromFile(f);
					std::fclose(f);
				}
				std::vector<std::uint8_t> output;
				if (decompress) {
					std::size_t s, e;
					if (!gzipParseHeader(input, s, e)) {
						std::fprintf(stderr,
							"wbsh: gunzip: not in gzip format: %s\n", fname.c_str());
						return 1;
					}
					if (!inflateRaw(input.data() + s, e - s, output)) {
						std::fprintf(stderr,
							"wbsh: gunzip: invalid compressed data: %s\n", fname.c_str());
						return 1;
					}
				} else {
					gzipEncodeStored(input, output);
				}
				std::string out_path;
				FILE* of = stdout;
				if (!to_stdout && !fname.empty() && fname != "-") {
					if (decompress) {
						if (fname.size() > 3
						    && fname.substr(fname.size() - 3) == ".gz")
							out_path = fname.substr(0, fname.size() - 3);
						else out_path = fname + ".out";
					} else {
						out_path = fname + ".gz";
					}
					of = fopenNative(exec, out_path, "wb");
					if (!of) {
						perr(decompress ? "gunzip" : "gzip", out_path,
						     std::error_code(errno, std::system_category()));
						return 1;
					}
				}
				std::fwrite(output.data(), 1, output.size(), of);
				if (of != stdout) std::fclose(of);
				if (!to_stdout && !keep && !fname.empty() && fname != "-") {
					std::error_code ec;
					std::filesystem::remove(toNative(exec, fname), ec);
				}
				return 0;
			};
			if (files.empty()) {
				to_stdout = true;
				return runOne("");
			}
			int rc = 0;
			for (const auto& f : files) {
				int r = runOne(f);
				if (r) rc = r;
			}
			return rc;
		}

		int builtin_gunzip(Executor& exec, const std::vector<std::string>& args) {
			std::vector<std::string> a = { "-d" };
			a.insert(a.end(), args.begin(), args.end());
			return builtin_gzip(exec, a);
		}

		int builtin_zcat(Executor& exec, const std::vector<std::string>& args) {
			std::vector<std::string> a = { "-d", "-c" };
			a.insert(a.end(), args.begin(), args.end());
			return builtin_gzip(exec, a);
		}

		// ---- zip / unzip ----------------------------------------------------
		//
		// Read/write ZIP archives. Uses our DEFLATE decoder for unzip and
		// stored entries (no compression) for zip — both produce valid
		// archives compatible with other tools.

		std::uint16_t zipR16(const std::uint8_t* p) {
			return (std::uint16_t)(p[0] | (p[1] << 8));
		}
		std::uint32_t zipR32(const std::uint8_t* p) {
			return (std::uint32_t)p[0]
			    | ((std::uint32_t)p[1] << 8)
			    | ((std::uint32_t)p[2] << 16)
			    | ((std::uint32_t)p[3] << 24);
		}
		void zipW16(std::vector<std::uint8_t>& out, std::uint16_t v) {
			out.push_back((std::uint8_t)(v & 0xFF));
			out.push_back((std::uint8_t)((v >> 8) & 0xFF));
		}
		void zipW32(std::vector<std::uint8_t>& out, std::uint32_t v) {
			out.push_back((std::uint8_t)(v & 0xFF));
			out.push_back((std::uint8_t)((v >> 8) & 0xFF));
			out.push_back((std::uint8_t)((v >> 16) & 0xFF));
			out.push_back((std::uint8_t)((v >> 24) & 0xFF));
		}

		// Find ZIP end-of-central-directory record by scanning the last 64KB.
		bool zipFindEOCD(const std::vector<std::uint8_t>& bytes, std::size_t& pos) {
			if (bytes.size() < 22) return false;
			std::size_t max_back = bytes.size() < 65557 ? bytes.size() : 65557;
			for (std::size_t i = bytes.size() - 22; i + 22 >= bytes.size() - max_back; --i) {
				if (zipR32(bytes.data() + i) == 0x06054B50u) { pos = i; return true; }
				if (i == 0) break;
			}
			return false;
		}

		int builtin_unzip(Executor& exec, const std::vector<std::string>& args) {
			bool list_only = false;
			bool to_stdout = false;
			bool overwrite = true;
			std::string archive;
			std::vector<std::string> select;
			std::string outdir;
			for (std::size_t i = 0; i < args.size(); ++i) {
				const std::string& a = args[i];
				if (a == "-l") list_only = true;
				else if (a == "-p") to_stdout = true;
				else if (a == "-o") overwrite = true;
				else if (a == "-n") overwrite = false;
				else if (a == "-d" && i + 1 < args.size()) outdir = args[++i];
				else if (!a.empty() && a[0] == '-' && a != "-") {
					/* ignore */
				}
				else if (archive.empty()) archive = a;
				else select.push_back(a);
			}
			if (archive.empty()) {
				perr("unzip", "missing archive name"); return 1;
			}
			(void)overwrite;
			FILE* f = fopenNative(exec, archive, "rb");
			if (!f) {
				perr("unzip", archive,
				    std::error_code(errno, std::system_category()));
				return 1;
			}
			auto bytes = readAllBytesFromFile(f);
			std::fclose(f);
			std::size_t eocd;
			if (!zipFindEOCD(bytes, eocd)) {
				perr("unzip", "not a zip archive: " + archive); return 1;
			}
			std::uint16_t total = zipR16(bytes.data() + eocd + 10);
			std::uint32_t cd_size  = zipR32(bytes.data() + eocd + 12);
			std::uint32_t cd_off   = zipR32(bytes.data() + eocd + 16);
			(void)cd_size;
			std::size_t p = cd_off;
			if (list_only) {
				std::printf("Archive:  %s\n", archive.c_str());
				std::printf("  Length      Date    Time    Name\n");
				std::printf("---------  ---------- -----   ----\n");
			}
			std::size_t total_bytes = 0;
			for (std::size_t k = 0; k < total; ++k) {
				if (p + 46 > bytes.size()) break;
				if (zipR32(bytes.data() + p) != 0x02014B50u) break;
				std::uint16_t method = zipR16(bytes.data() + p + 10);
				std::uint32_t crc    = zipR32(bytes.data() + p + 16);
				std::uint32_t csize  = zipR32(bytes.data() + p + 20);
				std::uint32_t usize  = zipR32(bytes.data() + p + 24);
				std::uint16_t nlen   = zipR16(bytes.data() + p + 28);
				std::uint16_t xlen   = zipR16(bytes.data() + p + 30);
				std::uint16_t clen   = zipR16(bytes.data() + p + 32);
				std::uint32_t lfh    = zipR32(bytes.data() + p + 42);
				(void)crc;
				std::string name((const char*)(bytes.data() + p + 46), nlen);
				p += 46 + nlen + xlen + clen;
				if (!select.empty()) {
					bool found = false;
					for (const auto& s : select) if (s == name) { found = true; break; }
					if (!found) continue;
				}
				if (list_only) {
					std::printf("%9u  ----------- ------  %s\n",
						(unsigned)usize, name.c_str());
					total_bytes += usize;
					continue;
				}
				if (lfh + 30 > bytes.size()) continue;
				if (zipR32(bytes.data() + lfh) != 0x04034B50u) continue;
				std::uint16_t l_nlen = zipR16(bytes.data() + lfh + 26);
				std::uint16_t l_xlen = zipR16(bytes.data() + lfh + 28);
				std::size_t data_off = lfh + 30 + l_nlen + l_xlen;
				if (data_off + csize > bytes.size()) continue;
				std::vector<std::uint8_t> data;
				if (method == 0) {
					data.assign(bytes.begin() + data_off,
					            bytes.begin() + data_off + csize);
				} else if (method == 8) {
					if (!inflateRaw(bytes.data() + data_off, csize, data)) {
						std::fprintf(stderr, "wbsh: unzip: inflate failed: %s\n",
							name.c_str());
						continue;
					}
				} else {
					std::fprintf(stderr,
						"wbsh: unzip: unsupported method %d for %s\n",
						(int)method, name.c_str());
					continue;
				}
				if (to_stdout) {
					std::fwrite(data.data(), 1, data.size(), stdout);
					continue;
				}
				std::string out_path = outdir.empty() ? name : (outdir + "/" + name);
				if (!name.empty() && name.back() == '/') {
					std::error_code ec;
					std::filesystem::create_directories(toNative(exec, out_path), ec);
					continue;
				}
				std::filesystem::path pp = toNative(exec, out_path);
				std::error_code ec;
				if (pp.has_parent_path()) {
					std::filesystem::create_directories(pp.parent_path(), ec);
				}
				FILE* of = openUtf8(pathToUtf8(pp), "wb");
				if (!of) {
					perr("unzip", out_path,
					    std::error_code(errno, std::system_category()));
					continue;
				}
				std::fwrite(data.data(), 1, data.size(), of);
				std::fclose(of);
				if (!list_only) std::printf("  inflating: %s\n", name.c_str());
			}
			if (list_only) {
				std::printf("---------                     -------\n");
				std::printf("%9zu                     %u files\n",
					total_bytes, (unsigned)total);
			}
			return 0;
		}

		int builtin_zip(Executor& exec, const std::vector<std::string>& args) {
			bool recurse = false;
			std::string archive;
			std::vector<std::string> inputs;
			for (const auto& a : args) {
				if (a == "-r" || a == "--recurse-paths") recurse = true;
				else if (!a.empty() && a[0] == '-' && a != "-") { /* ignore */ }
				else if (archive.empty()) archive = a;
				else inputs.push_back(a);
			}
			if (archive.empty()) { perr("zip", "missing archive name"); return 1; }
			if (inputs.empty())  { perr("zip", "no input files"); return 1; }
			namespace fs = std::filesystem;
			std::vector<std::string> paths;
			for (const auto& in : inputs) {
				fs::path win = toNative(exec, in);
				std::error_code ec;
				if (fs::is_directory(win, ec) && recurse) {
					for (auto it = fs::recursive_directory_iterator(win, ec);
					     it != fs::recursive_directory_iterator(); it.increment(ec))
					{
						if (ec) break;
						if (it->is_regular_file(ec)) {
							std::string rel = pathToUtf8(fs::relative(it->path(), fs::current_path(ec)));
							std::replace(rel.begin(), rel.end(), '\\', '/');
							paths.push_back(rel);
						}
					}
				} else if (fs::is_regular_file(win, ec)) {
					paths.push_back(in);
				}
			}
			std::vector<std::uint8_t> out;
			struct CDEntry {
				std::string name;
				std::uint32_t crc;
				std::uint32_t size;
				std::uint32_t lfh_off;
			};
			std::vector<CDEntry> cd;
			for (const auto& path : paths) {
				FILE* f = fopenNative(exec, path, "rb");
				if (!f) {
					perr("zip", path,
					    std::error_code(errno, std::system_category()));
					continue;
				}
				auto data = readAllBytesFromFile(f);
				std::fclose(f);
				std::uint32_t crc = crc32Update(0, data.data(), data.size());
				std::uint32_t lfh_off = (std::uint32_t)out.size();
				// Local file header.
				zipW32(out, 0x04034B50u);
				zipW16(out, 20);                       // version needed
				zipW16(out, 0);                        // flags
				zipW16(out, 0);                        // method = stored
				zipW16(out, 0);                        // mod time
				zipW16(out, 0);                        // mod date
				zipW32(out, crc);
				zipW32(out, (std::uint32_t)data.size());   // comp size
				zipW32(out, (std::uint32_t)data.size());   // uncomp size
				zipW16(out, (std::uint16_t)path.size());
				zipW16(out, 0);                        // extra
				out.insert(out.end(), path.begin(), path.end());
				out.insert(out.end(), data.begin(), data.end());
				cd.push_back({ path, crc, (std::uint32_t)data.size(), lfh_off });
				std::printf("  adding: %s (stored)\n", path.c_str());
			}
			std::uint32_t cd_off = (std::uint32_t)out.size();
			for (const auto& e : cd) {
				zipW32(out, 0x02014B50u);
				zipW16(out, 20);                       // version made
				zipW16(out, 20);                       // version needed
				zipW16(out, 0);                        // flags
				zipW16(out, 0);                        // method
				zipW16(out, 0);                        // mod time
				zipW16(out, 0);                        // mod date
				zipW32(out, e.crc);
				zipW32(out, e.size);                   // comp size
				zipW32(out, e.size);                   // uncomp size
				zipW16(out, (std::uint16_t)e.name.size());
				zipW16(out, 0);                        // extra
				zipW16(out, 0);                        // comment len
				zipW16(out, 0);                        // disk
				zipW16(out, 0);                        // int attr
				zipW32(out, 0);                        // ext attr
				zipW32(out, e.lfh_off);
				out.insert(out.end(), e.name.begin(), e.name.end());
			}
			std::uint32_t cd_size = (std::uint32_t)(out.size() - cd_off);
			zipW32(out, 0x06054B50u);
			zipW16(out, 0);
			zipW16(out, 0);
			zipW16(out, (std::uint16_t)cd.size());
			zipW16(out, (std::uint16_t)cd.size());
			zipW32(out, cd_size);
			zipW32(out, cd_off);
			zipW16(out, 0);
			FILE* of = fopenNative(exec, archive, "wb");
			if (!of) {
				perr("zip", archive,
				    std::error_code(errno, std::system_category()));
				return 1;
			}
			std::fwrite(out.data(), 1, out.size(), of);
			std::fclose(of);
			return 0;
		}

		// ---- bc -------------------------------------------------------------

		// Pragmatic bc(1): parses arithmetic expressions, assignments, and
		// simple control flow. Uses double precision; `scale` controls
		// fraction display. Math library (-l) provides sqrt, s, c, e, l, a.

		struct BcState {
			std::map<std::string, double> vars;
			int scale = 0;
			bool with_lib = false;
			bool quiet = false;
			bool exit_now = false;
		};

		struct BcLex {
			const std::string& src;
			std::size_t i = 0;
			std::vector<std::string> pushback;
			explicit BcLex(const std::string& s) : src(s) {}
			void skip() {
				while (i < src.size() && (src[i] == ' ' || src[i] == '\t')) ++i;
				while (i + 1 < src.size() && src[i] == '\\' && src[i + 1] == '\n') i += 2;
				if (i < src.size() && src[i] == '#') {
					while (i < src.size() && src[i] != '\n') ++i;
				}
			}
			std::string next() {
				if (!pushback.empty()) {
					std::string t = pushback.back();
					pushback.pop_back();
					return t;
				}
				skip();
				if (i >= src.size()) return "";
				char c = src[i];
				if (std::isdigit((unsigned char)c) || c == '.') {
					std::size_t s = i;
					while (i < src.size()
					    && (std::isdigit((unsigned char)src[i]) || src[i] == '.')) ++i;
					return src.substr(s, i - s);
				}
				if (std::isalpha((unsigned char)c) || c == '_') {
					std::size_t s = i;
					while (i < src.size()
					    && (std::isalnum((unsigned char)src[i]) || src[i] == '_')) ++i;
					return src.substr(s, i - s);
				}
				if (c == '<' || c == '>' || c == '=' || c == '!') {
					if (i + 1 < src.size() && src[i + 1] == '=') {
						std::string r = src.substr(i, 2);
						i += 2; return r;
					}
				}
				if (c == '\n') { ++i; return ";"; }
				char r = src[i++]; return std::string(1, r);
			}
			std::string peek() {
				if (pushback.empty()) {
					std::string t = next();
					if (t.empty()) return t;
					pushback.push_back(t);
				}
				return pushback.back();
			}
			void unread(std::string t) { pushback.push_back(std::move(t)); }
		};

		bool bcIsName(const std::string& t) {
			if (t.empty()) return false;
			char c = t[0];
			if (!(std::isalpha((unsigned char)c) || c == '_')) return false;
			for (char x : t) if (!std::isalnum((unsigned char)x) && x != '_') return false;
			return true;
		}
		bool bcIsNumber(const std::string& t) {
			if (t.empty()) return false;
			for (char c : t) {
				if (!std::isdigit((unsigned char)c) && c != '.') return false;
			}
			return true;
		}

		// Recursive-descent expression parser/evaluator. Operates directly
		// on lex stream and BcState; emits the value of each top-level
		// expression to stdout.
		struct BcEval {
			BcLex& lex;
			BcState& st;
			BcEval(BcLex& l, BcState& s) : lex(l), st(s) {}

			double parseExpr()    { return parseAssign(); }
			double parseAssign() {
				// lookahead: NAME = expr
				std::string t = lex.peek();
				if (bcIsName(t)) {
					std::string n = lex.next();
					std::string op = lex.peek();
					if (op == "=" || op == "+=" || op == "-=" || op == "*="
					    || op == "/=" || op == "%=" || op == "^=") {
						lex.next();
						double rhs = parseAssign();
						double cur = st.vars.count(n) ? st.vars[n] : 0.0;
						double v = rhs;
						if (op == "+=") v = cur + rhs;
						else if (op == "-=") v = cur - rhs;
						else if (op == "*=") v = cur * rhs;
						else if (op == "/=") v = rhs == 0 ? 0 : cur / rhs;
						else if (op == "%=") v = rhs == 0 ? 0 : std::fmod(cur, rhs);
						else if (op == "^=") v = std::pow(cur, rhs);
						if (n == "scale") st.scale = (int)v;
						else st.vars[n] = v;
						return v;
					}
					lex.unread(std::move(n));
				}
				return parseOr();
			}
			double parseOr() {
				double a = parseAnd();
				while (lex.peek() == "||") { lex.next(); double b = parseAnd(); a = (a != 0 || b != 0) ? 1 : 0; }
				return a;
			}
			double parseAnd() {
				double a = parseCmp();
				while (lex.peek() == "&&") { lex.next(); double b = parseCmp(); a = (a != 0 && b != 0) ? 1 : 0; }
				return a;
			}
			double parseCmp() {
				double a = parseAddSub();
				while (true) {
					std::string p = lex.peek();
					if (p == "==" || p == "!=" || p == "<" || p == "<="
					    || p == ">" || p == ">=") {
						lex.next();
						double b = parseAddSub();
						bool r = false;
						if (p == "==") r = a == b;
						else if (p == "!=") r = a != b;
						else if (p == "<") r = a < b;
						else if (p == "<=") r = a <= b;
						else if (p == ">") r = a > b;
						else                r = a >= b;
						a = r ? 1.0 : 0.0;
					} else break;
				}
				return a;
			}
			double parseAddSub() {
				double a = parseMul();
				while (true) {
					std::string p = lex.peek();
					if (p == "+") { lex.next(); a += parseMul(); }
					else if (p == "-") { lex.next(); a -= parseMul(); }
					else break;
				}
				return a;
			}
			double parseMul() {
				double a = parseExp();
				while (true) {
					std::string p = lex.peek();
					if (p == "*") { lex.next(); a *= parseExp(); }
					else if (p == "/") {
						lex.next();
						double b = parseExp();
						a = b == 0 ? 0 : a / b;
					} else if (p == "%") {
						lex.next();
						double b = parseExp();
						a = b == 0 ? 0 : std::fmod(a, b);
					} else break;
				}
				return a;
			}
			double parseExp() {
				double a = parseUnary();
				if (lex.peek() == "^") {
					lex.next();
					a = std::pow(a, parseExp());
				}
				return a;
			}
			double parseUnary() {
				std::string p = lex.peek();
				if (p == "-") { lex.next(); return -parseUnary(); }
				if (p == "+") { lex.next(); return  parseUnary(); }
				if (p == "!") { lex.next(); return parseUnary() == 0 ? 1 : 0; }
				if (p == "++") {
					lex.next();
					std::string n = lex.next();
					double v = (st.vars.count(n) ? st.vars[n] : 0.0) + 1;
					st.vars[n] = v;
					return v;
				}
				if (p == "--") {
					lex.next();
					std::string n = lex.next();
					double v = (st.vars.count(n) ? st.vars[n] : 0.0) - 1;
					st.vars[n] = v;
					return v;
				}
				return parsePrimary();
			}
			double parsePrimary() {
				std::string t = lex.next();
				if (t.empty()) return 0;
				if (t == "(") {
					double v = parseExpr();
					if (lex.peek() == ")") lex.next();
					return v;
				}
				if (bcIsNumber(t)) {
					try { return std::stod(t); } catch (...) { return 0; }
				}
				if (bcIsName(t)) {
					if (lex.peek() == "(") {
						// Function call
						lex.next();
						std::vector<double> args;
						if (lex.peek() != ")") {
							args.push_back(parseExpr());
							while (lex.peek() == ",") { lex.next(); args.push_back(parseExpr()); }
						}
						if (lex.peek() == ")") lex.next();
						return callFunc(t, args);
					}
					// Postfix ++/--
					if (lex.peek() == "++" || lex.peek() == "--") {
						double cur = st.vars.count(t) ? st.vars[t] : 0.0;
						double nv = lex.next() == "++" ? cur + 1 : cur - 1;
						st.vars[t] = nv;
						return cur;
					}
					if (t == "scale") return st.scale;
					return st.vars.count(t) ? st.vars[t] : 0.0;
				}
				return 0;
			}
			double callFunc(const std::string& n, const std::vector<double>& a) {
				auto get0 = [&](){ return a.empty() ? 0 : a[0]; };
				if (n == "sqrt") return std::sqrt(get0());
				if (st.with_lib) {
					if (n == "s") return std::sin(get0());
					if (n == "c") return std::cos(get0());
					if (n == "e") return std::exp(get0());
					if (n == "l") return std::log(get0());
					if (n == "a") return std::atan(get0());
				}
				if (n == "length") {
					char buf[64];
					std::snprintf(buf, sizeof(buf), "%.20g", get0());
					std::string s = buf;
					int c = 0;
					for (char x : s) if (std::isdigit((unsigned char)x)) ++c;
					return (double)c;
				}
				return 0;
			}
		};

		// Suppress-output check: an expression starting with `NAME = ...`
		// (or `NAME op= ...`, or `++NAME` / `--NAME`) is an assignment and
		// bc does not print its value. We do a cheap lookahead on the
		// token stream before eval to decide.
		bool bcStartsAssign(BcLex& lex) {
			// Peek up to two tokens.
			std::string t1 = lex.next();
			if (t1.empty()) { return false; }
			std::string t2 = lex.next();
			lex.unread(t2);
			lex.unread(t1);
			if (t1 == "++" || t1 == "--") return true;
			if (!bcIsName(t1)) return false;
			return t2 == "=" || t2 == "+=" || t2 == "-=" || t2 == "*="
			    || t2 == "/=" || t2 == "%=" || t2 == "^=";
		}

		void bcEvalLine(BcLex& lex, BcState& st) {
			while (true) {
				std::string p = lex.peek();
				if (p.empty()) return;
				if (p == ";") { lex.next(); continue; }
				if (p == "quit" || p == "halt") {
					lex.next(); st.exit_now = true; return;
				}
				if (p == "if") {
					lex.next();
					if (lex.peek() == "(") lex.next();
					BcEval e(lex, st);
					double cond = e.parseExpr();
					if (lex.peek() == ")") lex.next();
					if ((bool)cond) {
						bcEvalLine(lex, st);
					} else {
						// Skip the controlled statement.
						std::string nx = lex.next();
						if (nx == "{") {
							int d = 1;
							while (d > 0) {
								std::string t = lex.next();
								if (t.empty()) return;
								if (t == "{") ++d;
								else if (t == "}") --d;
							}
						} else {
							while (!nx.empty() && nx != ";" && nx != "\n") nx = lex.next();
						}
					}
					continue;
				}
				if (p == "{") {
					lex.next();
					while (lex.peek() != "}" && !lex.peek().empty()) bcEvalLine(lex, st);
					if (lex.peek() == "}") lex.next();
					continue;
				}
				if (p == "}") return;
				bool suppress = bcStartsAssign(lex);
				BcEval e(lex, st);
				double v = e.parseExpr();
				if (!suppress) {
					if (st.scale == 0) std::printf("%lld\n", (long long)v);
					else std::printf("%.*f\n", st.scale, v);
				}
				if (lex.peek() == ";") lex.next();
				if (lex.peek().empty() || lex.peek() == "}") return;
			}
		}

		int builtin_bc(Executor& exec, const std::vector<std::string>& args) {
			BcState st;
			std::vector<std::string> files;
			for (std::size_t i = 0; i < args.size(); ++i) {
				const std::string& a = args[i];
				if (a == "-l" || a == "--mathlib") { st.with_lib = true; st.scale = 20; }
				else if (a == "-q" || a == "--quiet") st.quiet = true;
				else if (a == "-e" && i + 1 < args.size()) {
					BcLex lex(args[++i]);
					while (!lex.peek().empty() && !st.exit_now)
						bcEvalLine(lex, st);
				}
				else if (!a.empty() && a[0] == '-' && a != "-") { /* ignore */ }
				else files.push_back(a);
			}
			auto runStream = [&](FILE* f) {
				std::string buf;
				int c;
				while ((c = std::fgetc(f)) != EOF && !st.exit_now) {
					buf.push_back((char)c);
					if (c == '\n') {
						BcLex lex(buf);
						while (!lex.peek().empty() && !st.exit_now)
							bcEvalLine(lex, st);
						buf.clear();
					}
				}
				if (!buf.empty() && !st.exit_now) {
					BcLex lex(buf);
					while (!lex.peek().empty() && !st.exit_now) bcEvalLine(lex, st);
				}
			};
			if (files.empty()) runStream(stdin);
			else {
				for (const auto& fn : files) {
					FILE* f = fopenNative(exec, fn, "rb");
					if (!f) { perr("bc", fn, std::error_code(errno, std::system_category())); continue; }
					runStream(f);
					std::fclose(f);
				}
			}
			return 0;
		}

		// ---- xxd ------------------------------------------------------------

		int builtin_xxd(Executor& exec, const std::vector<std::string>& args) {
			bool plain = false, reverse = false;
			int cols = 16;
			std::string path;
			for (std::size_t i = 0; i < args.size(); ++i) {
				const std::string& a = args[i];
				if (a == "-p" || a == "--plain")    plain = true;
				else if (a == "-r" || a == "--revert") reverse = true;
				else if (a == "-c" && i + 1 < args.size()) {
					try { cols = std::stoi(args[++i]); } catch (...) {}
				}
				else if (!a.empty() && a[0] == '-' && a != "-") {
					perr("xxd", "unknown option: " + a); return 1;
				}
				else if (path.empty()) path = a;
			}
			std::vector<unsigned char> bytes;
			auto readAllBytes = [&](FILE* f) {
				int c; while ((c = std::fgetc(f)) != EOF) bytes.push_back((unsigned char)c);
			};
			if (path.empty() || path == "-") readAllBytes(stdin);
			else {
				FILE* f = fopenNative(exec, path, "rb");
				if (!f) { perr("xxd", path, std::error_code(errno, std::system_category())); return 1; }
				readAllBytes(f);
				std::fclose(f);
			}
			if (reverse) {
				// Plain: just hex bytes back to binary (any whitespace allowed).
				// Default xxd format: "<addr>: <hex pairs>  <ascii>" — we
				// extract the hex pairs only.
				std::vector<unsigned char> out;
				std::string line;
				auto flushLine = [&]() {
					std::string hex;
					if (plain) hex = line;
					else {
						auto colon = line.find(':');
						auto start = (colon == std::string::npos) ? 0 : colon + 1;
						auto two = line.find("  ", start);
						hex = line.substr(start, (two == std::string::npos) ? std::string::npos : two - start);
					}
					unsigned cur = 0; int half = 0;
					for (char c : hex) {
						unsigned v;
						if      (c >= '0' && c <= '9') v = c - '0';
						else if (c >= 'a' && c <= 'f') v = 10 + c - 'a';
						else if (c >= 'A' && c <= 'F') v = 10 + c - 'A';
						else continue;
						cur = (cur << 4) | v; half++;
						if (half == 2) { out.push_back((unsigned char)cur); cur = 0; half = 0; }
					}
				};
				for (unsigned char c : bytes) {
					if (c == '\n') { flushLine(); line.clear(); }
					else line.push_back((char)c);
				}
				if (!line.empty()) flushLine();
				std::fwrite(out.data(), 1, out.size(), stdout);
				return 0;
			}
			// Forward dump.
			if (plain) {
				int per_line = (cols > 0) ? cols * 2 : 60;
				int n = 0;
				for (unsigned char c : bytes) {
					std::printf("%02x", c);
					n += 2;
					if (n >= per_line) { std::printf("\n"); n = 0; }
				}
				if (n) std::printf("\n");
				return 0;
			}
			std::size_t addr = 0;
			while (addr < bytes.size()) {
				std::size_t end = (std::min)(bytes.size(), addr + (std::size_t)cols);
				std::printf("%08zx:", addr);
				for (std::size_t i = addr; i < addr + (std::size_t)cols; ++i) {
					if ((i - addr) % 2 == 0) std::printf(" ");
					if (i < end) std::printf("%02x", bytes[i]);
					else         std::printf("  ");
				}
				std::printf("  ");
				for (std::size_t i = addr; i < end; ++i) {
					unsigned char c = bytes[i];
					std::putchar((c >= 32 && c < 127) ? c : '.');
				}
				std::printf("\n");
				addr = end;
			}
			return 0;
		}

		// ---- od -------------------------------------------------------------

		int builtin_od(Executor& exec, const std::vector<std::string>& args) {
			char fmt = 'o';        // o (octal-bytes), x (hex-bytes), c (char), d (decimal)
			char addr_fmt = 'o';
			std::string path;
			for (std::size_t i = 0; i < args.size(); ++i) {
				const std::string& a = args[i];
				if (a == "-c") fmt = 'c';
				else if (a == "-x" || a == "-h") fmt = 'x';
				else if (a == "-d") fmt = 'd';
				else if (a == "-o") fmt = 'o';
				else if (a == "-A" && i + 1 < args.size()) {
					const std::string& w = args[++i];
					if (!w.empty()) addr_fmt = w[0];
				}
				else if (a == "-t" && i + 1 < args.size()) {
					const std::string& t = args[++i];
					if (!t.empty()) {
						char c = t[0];
						if (c == 'x' || c == 'd' || c == 'o' || c == 'c') fmt = c;
					}
				}
				else if (!a.empty() && a[0] == '-' && a != "-") {
					perr("od", "unknown option: " + a); return 1;
				}
				else if (path.empty()) path = a;
			}
			std::vector<unsigned char> bytes;
			if (path.empty() || path == "-") {
				int c; while ((c = std::fgetc(stdin)) != EOF) bytes.push_back((unsigned char)c);
			} else {
				FILE* f = fopenNative(exec, path, "rb");
				if (!f) { perr("od", path, std::error_code(errno, std::system_category())); return 1; }
				int c; while ((c = std::fgetc(f)) != EOF) bytes.push_back((unsigned char)c);
				std::fclose(f);
			}
			constexpr std::size_t per_row = 16;
			std::size_t addr = 0;
			while (addr < bytes.size()) {
				switch (addr_fmt) {
				case 'd': std::printf("%07zu", addr); break;
				case 'x': std::printf("%07zx", addr); break;
				case 'n': break;
				default:  std::printf("%07zo", addr); break;
				}
				std::size_t end = (std::min)(bytes.size(), addr + per_row);
				if (fmt == 'c') {
					for (std::size_t i = addr; i < end; ++i) {
						unsigned char b = bytes[i];
						const char* esc = nullptr;
						switch (b) {
						case '\0': esc = "\\0"; break;
						case '\a': esc = "\\a"; break;
						case '\b': esc = "\\b"; break;
						case '\t': esc = "\\t"; break;
						case '\n': esc = "\\n"; break;
						case '\v': esc = "\\v"; break;
						case '\f': esc = "\\f"; break;
						case '\r': esc = "\\r"; break;
						}
						if (esc) std::printf("  %s", esc);
						else if (b >= 32 && b < 127) std::printf("   %c", b);
						else std::printf(" %03o", b);
					}
				} else if (fmt == 'x') {
					for (std::size_t i = addr; i < end; ++i) std::printf(" %02x", bytes[i]);
				} else if (fmt == 'd') {
					for (std::size_t i = addr; i < end; ++i) std::printf(" %3u", bytes[i]);
				} else { // octal
					for (std::size_t i = addr; i < end; ++i) std::printf(" %03o", bytes[i]);
				}
				std::printf("\n");
				addr = end;
			}
			// Final address (GNU od convention).
			switch (addr_fmt) {
			case 'd': std::printf("%07zu\n", bytes.size()); break;
			case 'x': std::printf("%07zx\n", bytes.size()); break;
			case 'n': break;
			default:  std::printf("%07zo\n", bytes.size()); break;
			}
			return 0;
		}

		// ---- fold -----------------------------------------------------------

		int builtin_fold(Executor& exec, const std::vector<std::string>& args) {
			int width = 80;
			bool by_bytes = true;
			bool wrap_spaces = false;
			std::vector<std::string> files;
			for (std::size_t i = 0; i < args.size(); ++i) {
				const std::string& a = args[i];
				if (a == "-b" || a == "--bytes") by_bytes = true;
				else if (a == "-c" || a == "--characters") by_bytes = false;
				else if (a == "-s" || a == "--spaces") wrap_spaces = true;
				else if (a == "-w" && i + 1 < args.size()) {
					try { width = std::stoi(args[++i]); } catch (...) {}
				}
				else if (a.size() > 2 && a.compare(0, 2, "-w") == 0) {
					try { width = std::stoi(a.substr(2)); } catch (...) {}
				}
				else if (!a.empty() && a[0] == '-' && a != "-" && std::isdigit((unsigned char)a[1])) {
					try { width = std::stoi(a.substr(1)); } catch (...) {}
				}
				else if (!a.empty() && a[0] == '-' && a != "-") {
					perr("fold", "unknown option: " + a); return 1;
				}
				else files.push_back(a);
			}
			(void)by_bytes;   // bytes vs chars: same in the ASCII path
			if (width <= 0) { perr("fold", "width must be > 0"); return 1; }
			auto wrapLine = [&](const std::string& line) {
				std::size_t pos = 0;
				while (pos < line.size()) {
					std::size_t take = (std::min<std::size_t>)(width, line.size() - pos);
					if (wrap_spaces && take < line.size() - pos) {
						auto sp = line.rfind(' ', pos + take - 1);
						if (sp != std::string::npos && sp >= pos) take = sp - pos + 1;
					}
					std::fwrite(line.data() + pos, 1, take, stdout);
					std::putchar('\n');
					pos += take;
				}
				if (line.empty()) std::putchar('\n');
			};
			auto runOn = [&](FILE* f) {
				std::string buf;
				int c;
				while ((c = std::fgetc(f)) != EOF) {
					if (c == '\n') { wrapLine(buf); buf.clear(); }
					else buf.push_back((char)c);
				}
				if (!buf.empty()) wrapLine(buf);
			};
			if (files.empty() || files[0] == "-") runOn(stdin);
			else {
				for (const auto& p : files) {
					FILE* f = fopenNative(exec, p, "rb");
					if (!f) { perr("fold", p, std::error_code(errno, std::system_category())); return 1; }
					runOn(f);
					std::fclose(f);
				}
			}
			return 0;
		}

		// ---- column ---------------------------------------------------------

		int builtin_column(Executor& exec, const std::vector<std::string>& args) {
			bool table = false;
			std::string sep = " \t";
			std::string out_sep = "  ";
			std::vector<std::string> files;
			for (std::size_t i = 0; i < args.size(); ++i) {
				const std::string& a = args[i];
				if (a == "-t" || a == "--table") table = true;
				else if ((a == "-s" || a == "--separator") && i + 1 < args.size()) sep = args[++i];
				else if (a.size() > 2 && a.compare(0, 2, "-s") == 0) sep = a.substr(2);
				else if ((a == "-o" || a == "--output-separator") && i + 1 < args.size()) out_sep = args[++i];
				else if (!a.empty() && a[0] == '-' && a != "-") {
					perr("column", "unknown option: " + a); return 1;
				}
				else files.push_back(a);
			}
			std::vector<std::vector<std::string>> rows;
			auto splitFields = [&](const std::string& line) {
				std::vector<std::string> fields;
				std::string cur;
				for (char c : line) {
					if (sep.find(c) != std::string::npos) {
						fields.push_back(std::move(cur)); cur.clear();
						// Treat consecutive whitespace separators as one field break.
						if (sep == " \t") {
							while (!fields.empty() && fields.back().empty())
								fields.pop_back();
						}
					} else cur.push_back(c);
				}
				fields.push_back(std::move(cur));
				return fields;
			};
			auto runOn = [&](FILE* f) {
				std::string buf; int c;
				while ((c = std::fgetc(f)) != EOF) {
					if (c == '\n') { rows.push_back(splitFields(buf)); buf.clear(); }
					else buf.push_back((char)c);
				}
				if (!buf.empty()) rows.push_back(splitFields(buf));
			};
			if (files.empty() || files[0] == "-") runOn(stdin);
			else {
				for (const auto& p : files) {
					FILE* f = fopenNative(exec, p, "rb");
					if (!f) { perr("column", p, std::error_code(errno, std::system_category())); return 1; }
					runOn(f);
					std::fclose(f);
				}
			}
			if (!table) {
				for (const auto& row : rows) {
					for (std::size_t i = 0; i < row.size(); ++i) {
						if (i) std::fputs(" ", stdout);
						std::fputs(row[i].c_str(), stdout);
					}
					std::putchar('\n');
				}
				return 0;
			}
			// Table mode: pad each column to the widest entry.
			std::size_t cols = 0;
			for (const auto& r : rows) cols = (std::max)(cols, r.size());
			std::vector<std::size_t> widths(cols, 0);
			for (const auto& r : rows) {
				for (std::size_t i = 0; i < r.size(); ++i)
					widths[i] = (std::max)(widths[i], r[i].size());
			}
			for (const auto& r : rows) {
				for (std::size_t i = 0; i < r.size(); ++i) {
					if (i) std::fputs(out_sep.c_str(), stdout);
					std::fputs(r[i].c_str(), stdout);
					if (i + 1 < r.size()) {
						for (std::size_t k = r[i].size(); k < widths[i]; ++k)
							std::putchar(' ');
					}
				}
				std::putchar('\n');
			}
			return 0;
		}

		// ---- expand / unexpand ----------------------------------------------

		int builtin_expand(Executor& exec, const std::vector<std::string>& args) {
			int tabstop = 8;
			std::vector<std::string> files;
			for (std::size_t i = 0; i < args.size(); ++i) {
				const std::string& a = args[i];
				if ((a == "-t" || a == "--tabs") && i + 1 < args.size()) {
					try { tabstop = std::stoi(args[++i]); } catch (...) {}
				}
				else if (a.size() > 2 && a.compare(0, 2, "-t") == 0) {
					try { tabstop = std::stoi(a.substr(2)); } catch (...) {}
				}
				else if (!a.empty() && a[0] == '-' && a != "-" && std::isdigit((unsigned char)a[1])) {
					try { tabstop = std::stoi(a.substr(1)); } catch (...) {}
				}
				else if (!a.empty() && a[0] == '-' && a != "-") {
					perr("expand", "unknown option: " + a); return 1;
				}
				else files.push_back(a);
			}
			if (tabstop <= 0) tabstop = 8;
			auto runOn = [&](FILE* f) {
				int col = 0; int c;
				while ((c = std::fgetc(f)) != EOF) {
					if (c == '\t') {
						int spaces = tabstop - (col % tabstop);
						for (int i = 0; i < spaces; ++i) std::putchar(' ');
						col += spaces;
					}
					else if (c == '\n') { std::putchar('\n'); col = 0; }
					else { std::putchar(c); ++col; }
				}
			};
			if (files.empty() || files[0] == "-") runOn(stdin);
			else {
				for (const auto& p : files) {
					FILE* f = fopenNative(exec, p, "rb");
					if (!f) { perr("expand", p, std::error_code(errno, std::system_category())); return 1; }
					runOn(f);
					std::fclose(f);
				}
			}
			return 0;
		}

		int builtin_unexpand(Executor& exec, const std::vector<std::string>& args) {
			int tabstop = 8;
			bool all = false;
			std::vector<std::string> files;
			for (std::size_t i = 0; i < args.size(); ++i) {
				const std::string& a = args[i];
				if (a == "-a" || a == "--all") all = true;
				else if ((a == "-t" || a == "--tabs") && i + 1 < args.size()) {
					try { tabstop = std::stoi(args[++i]); } catch (...) {}
				}
				else if (a.size() > 2 && a.compare(0, 2, "-t") == 0) {
					try { tabstop = std::stoi(a.substr(2)); } catch (...) {}
				}
				else if (!a.empty() && a[0] == '-' && a != "-") {
					perr("unexpand", "unknown option: " + a); return 1;
				}
				else files.push_back(a);
			}
			if (tabstop <= 0) tabstop = 8;
			auto runOn = [&](FILE* f) {
				std::string buf; int c;
				auto flush = [&]() {
					// Compress runs of leading spaces (or, with -a, anywhere)
					// into tabs at tabstop boundaries.
					std::size_t i = 0;
					int col = 0;
					bool past_indent = false;
					while (i < buf.size()) {
						if (!past_indent || all) {
							std::size_t run_start = i;
							int run_col = col;
							while (i < buf.size() && buf[i] == ' ') { ++i; ++col; }
							int run_len = col - run_col;
							if (run_len > 0) {
								int next_tab = ((run_col / tabstop) + 1) * tabstop;
								while (run_col + (next_tab - run_col) <= col) {
									int gap = next_tab - run_col;
									if (gap == 0) break;
									std::putchar('\t');
									run_col = next_tab;
									next_tab += tabstop;
								}
								while (run_col < col) { std::putchar(' '); ++run_col; }
								(void)run_start;
								continue;
							}
						}
						char ch = buf[i++];
						if (ch != ' ' && ch != '\t') past_indent = true;
						std::putchar(ch);
						if (ch == '\t') col = ((col / tabstop) + 1) * tabstop;
						else ++col;
					}
				};
				while ((c = std::fgetc(f)) != EOF) {
					if (c == '\n') { flush(); std::putchar('\n'); buf.clear(); }
					else buf.push_back((char)c);
				}
				if (!buf.empty()) flush();
			};
			if (files.empty() || files[0] == "-") runOn(stdin);
			else {
				for (const auto& p : files) {
					FILE* f = fopenNative(exec, p, "rb");
					if (!f) { perr("unexpand", p, std::error_code(errno, std::system_category())); return 1; }
					runOn(f);
					std::fclose(f);
				}
			}
			return 0;
		}

		// ---- comm -----------------------------------------------------------

		int builtin_comm(Executor& exec, const std::vector<std::string>& args) {
			bool suppress[3] = { false, false, false };  // -1 -2 -3
			std::vector<std::string> files;
			for (const auto& a : args) {
				if (a == "-1") suppress[0] = true;
				else if (a == "-2") suppress[1] = true;
				else if (a == "-3") suppress[2] = true;
				else if (a == "-12" || a == "-21") { suppress[0] = suppress[1] = true; }
				else if (a == "-13" || a == "-31") { suppress[0] = suppress[2] = true; }
				else if (a == "-23" || a == "-32") { suppress[1] = suppress[2] = true; }
				else if (a == "-123") { suppress[0] = suppress[1] = suppress[2] = true; }
				else if (!a.empty() && a[0] == '-' && a != "-") {
					perr("comm", "unknown option: " + a); return 1;
				}
				else files.push_back(a);
			}
			if (files.size() != 2) { perr("comm", "usage: comm [opts] FILE1 FILE2"); return 1; }
			auto loadLines = [&](const std::string& p) -> std::vector<std::string> {
				std::vector<std::string> out;
				FILE* f = (p == "-") ? stdin
				    : fopenNative(exec, p, "rb");
				if (!f) { perr("comm", p, std::error_code(errno, std::system_category())); return out; }
				std::string buf; int c;
				while ((c = std::fgetc(f)) != EOF) {
					if (c == '\n') { out.push_back(std::move(buf)); buf.clear(); }
					else buf.push_back((char)c);
				}
				if (!buf.empty()) out.push_back(std::move(buf));
				if (f != stdin) std::fclose(f);
				return out;
			};
			auto la = loadLines(files[0]);
			auto lb = loadLines(files[1]);
			auto emit = [&](int col, const std::string& s) {
				if (suppress[col]) return;
				for (int k = 0; k < col; ++k) std::putchar('\t');
				std::fputs(s.c_str(), stdout);
				std::putchar('\n');
			};
			std::size_t i = 0, j = 0;
			while (i < la.size() && j < lb.size()) {
				if (la[i] == lb[j])      { emit(2, la[i]); ++i; ++j; }
				else if (la[i] < lb[j])  { emit(0, la[i]); ++i; }
				else                     { emit(1, lb[j]); ++j; }
			}
			while (i < la.size()) { emit(0, la[i++]); }
			while (j < lb.size()) { emit(1, lb[j++]); }
			return 0;
		}

		// ---- yes ------------------------------------------------------------

		int builtin_yes(Executor&, const std::vector<std::string>& args) {
			std::string line;
			if (args.empty()) line = "y";
			else {
				for (std::size_t i = 0; i < args.size(); ++i) {
					if (i) line.push_back(' ');
					line += args[i];
				}
			}
			line.push_back('\n');
			while (true) {
				if (std::fwrite(line.data(), 1, line.size(), stdout) != line.size()) {
					return 1;   // pipe closed (downstream done) — exit cleanly
				}
			}
		}

		// ---- nproc ----------------------------------------------------------

		int builtin_nproc(Executor&, const std::vector<std::string>& args) {
			(void)args;   // ignore --all / --ignore=N for this minimal impl
			unsigned n = std::thread::hardware_concurrency();
			if (n == 0) n = 1;
			std::printf("%u\n", n);
			return 0;
		}

		// ---- tput -----------------------------------------------------------

		int builtin_tput(Executor&, const std::vector<std::string>& args) {
			if (args.empty()) return 0;
			auto consoleSize = [](int& cols, int& lines) -> bool {
#ifdef _WIN32
				HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
				CONSOLE_SCREEN_BUFFER_INFO info{};
				if (h == INVALID_HANDLE_VALUE) return false;
				if (!GetConsoleScreenBufferInfo(h, &info)) return false;
				cols = info.srWindow.Right - info.srWindow.Left + 1;
				lines = info.srWindow.Bottom - info.srWindow.Top + 1;
				return true;
#else
				cols = 80; lines = 24;
				return true;
#endif
			};
			const std::string& cap = args[0];
			if (cap == "cols" || cap == "columns") {
				int c = 80, l = 24;
				consoleSize(c, l);
				std::printf("%d\n", c);
				return 0;
			}
			if (cap == "lines") {
				int c = 80, l = 24;
				consoleSize(c, l);
				std::printf("%d\n", l);
				return 0;
			}
			// VT-100 escape sequences for the common attributes / motions.
			if (cap == "clear")  { std::printf("\x1b[2J\x1b[H"); return 0; }
			if (cap == "reset")  { std::printf("\x1b" "c");       return 0; }
			if (cap == "bold")   { std::printf("\x1b[1m");        return 0; }
			if (cap == "dim")    { std::printf("\x1b[2m");        return 0; }
			if (cap == "smul")   { std::printf("\x1b[4m");        return 0; }
			if (cap == "rmul")   { std::printf("\x1b[24m");       return 0; }
			if (cap == "rev")    { std::printf("\x1b[7m");        return 0; }
			if (cap == "blink")  { std::printf("\x1b[5m");        return 0; }
			if (cap == "sgr0" || cap == "op") { std::printf("\x1b[0m"); return 0; }
			if (cap == "civis")  { std::printf("\x1b[?25l");      return 0; }
			if (cap == "cnorm")  { std::printf("\x1b[?25h");      return 0; }
			if (cap == "el")     { std::printf("\x1b[K");         return 0; }
			if (cap == "ed")     { std::printf("\x1b[J");         return 0; }
			if (cap == "home")   { std::printf("\x1b[H");         return 0; }
			if (cap == "cup" && args.size() >= 3) {
				int row = 0, col = 0;
				try { row = std::stoi(args[1]); col = std::stoi(args[2]); }
				catch (...) { return 1; }
				std::printf("\x1b[%d;%dH", row + 1, col + 1);
				return 0;
			}
			if ((cap == "setaf" || cap == "setf") && args.size() >= 2) {
				int n = 0;
				try { n = std::stoi(args[1]); } catch (...) { return 1; }
				if (n >= 0 && n < 8) std::printf("\x1b[%dm", 30 + n);
				else std::printf("\x1b[39m");
				return 0;
			}
			if ((cap == "setab" || cap == "setb") && args.size() >= 2) {
				int n = 0;
				try { n = std::stoi(args[1]); } catch (...) { return 1; }
				if (n >= 0 && n < 8) std::printf("\x1b[%dm", 40 + n);
				else std::printf("\x1b[49m");
				return 0;
			}
			std::fprintf(stderr, "wbsh: tput: unknown capability: %s\n", cap.c_str());
			return 1;
		}

		// ---- mktemp ---------------------------------------------------------

		int builtin_mktemp(Executor& exec, const std::vector<std::string>& args) {
			bool make_dir = false;
			bool dry_run = false;
			bool quiet = false;
			std::string template_arg;
			std::string tmpdir_override;
			bool template_from_p = false;
			(void)template_from_p;
			for (std::size_t i = 0; i < args.size(); ++i) {
				const std::string& a = args[i];
				if (a == "-d" || a == "--directory") make_dir = true;
				else if (a == "-u" || a == "--dry-run") dry_run = true;
				else if (a == "-q" || a == "--quiet") quiet = true;
				else if (a == "-t") {
					// legacy "use TMPDIR" — already our default; ignore
				}
				else if (a == "-p" || a == "--tmpdir") {
					if (i + 1 < args.size()) tmpdir_override = args[++i];
				}
				else if (a.size() > 9 && a.compare(0, 9, "--tmpdir=") == 0) {
					tmpdir_override = a.substr(9);
				}
				else if (!a.empty() && a[0] == '-' && a != "-") {
					std::fprintf(stderr, "wbsh: mktemp: unknown option: %s\n", a.c_str());
					return 1;
				}
				else if (template_arg.empty()) {
					template_arg = a;
				}
			}
			if (template_arg.empty()) {
				template_arg = make_dir ? "tmp.XXXXXXXXXX" : "tmp.XXXXXXXXXX";
			}
			// Resolve TMPDIR: explicit override > $TMPDIR > $TEMP > $TMP > /tmp.
			std::string base;
			if (!tmpdir_override.empty()) base = tmpdir_override;
			else if (!exec.env().get("TMPDIR").empty()) base = exec.env().get("TMPDIR");
			else if (!exec.env().get("TEMP").empty())   base = exec.env().get("TEMP");
			else if (!exec.env().get("TMP").empty())    base = exec.env().get("TMP");
			else base = "/tmp";
			// If template contains a slash, treat it as path relative to CWD.
			fs::path full;
			if (template_arg.find('/') != std::string::npos
			    || template_arg.find('\\') != std::string::npos) {
				full = fs::path(toNative(exec, template_arg));
			} else {
				full = fs::path(toNative(exec, base)) / template_arg;
			}
			std::string s = pathToUtf8(full);
			// Find trailing run of X (must be at least 3).
			std::size_t end = s.size();
			std::size_t start = end;
			while (start > 0 && s[start - 1] == 'X') --start;
			std::size_t xcount = end - start;
			if (xcount < 3) {
				if (!quiet) std::fprintf(stderr, "wbsh: mktemp: too few X's in template '%s'\n",
					template_arg.c_str());
				return 1;
			}
			// Try up to a few attempts with random suffixes.
			static const char* alpha = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
			constexpr std::size_t alpha_n = 62;
			std::error_code ec;
			for (int attempt = 0; attempt < 200; ++attempt) {
				std::string suffix(xcount, 'X');
				auto t = std::chrono::steady_clock::now().time_since_epoch().count();
				unsigned long long mix = static_cast<unsigned long long>(t)
				    ^ (static_cast<unsigned long long>(GetCurrentProcessId()) << 32)
				    ^ static_cast<unsigned long long>(attempt) * 0x9E3779B97F4A7C15ULL;
				for (std::size_t k = 0; k < xcount; ++k) {
					suffix[k] = alpha[mix % alpha_n];
					mix = mix * 6364136223846793005ULL + 1442695040888963407ULL;
				}
				std::string candidate = s.substr(0, start) + suffix;
				if (dry_run) {
					std::printf("%s\n", exec.pathConv().toPosix(candidate).c_str());
					return 0;
				}
				if (make_dir) {
					if (fs::create_directory(candidate, ec) && !ec) {
						std::printf("%s\n", exec.pathConv().toPosix(candidate).c_str());
						return 0;
					}
				} else {
					HANDLE h = CreateFileA(candidate.c_str(),
						GENERIC_WRITE, 0, nullptr,
						CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
					if (h != INVALID_HANDLE_VALUE) {
						CloseHandle(h);
						std::printf("%s\n", exec.pathConv().toPosix(candidate).c_str());
						return 0;
					}
				}
			}
			if (!quiet) std::fprintf(stderr,
				"wbsh: mktemp: failed to create unique file from '%s'\n",
				template_arg.c_str());
			return 1;
		}

		// ---- kill -----------------------------------------------------------

		int builtin_kill(Executor&, const std::vector<std::string>& args) {
			int signum = 15;   // SIGTERM
			std::vector<int> pids;
			for (std::size_t i = 0; i < args.size(); ++i) {
				const std::string& a = args[i];
				if (a == "-l") {
					std::printf(" 1) HUP   2) INT   3) QUIT  4) ILL   "
					            "5) TRAP  6) ABRT  7) BUS   8) FPE\n"
					            " 9) KILL 10) USR1 11) SEGV 12) USR2 "
					            "13) PIPE 14) ALRM 15) TERM\n");
					return 0;
				}
				if (a == "-s" && i + 1 < args.size()) {
					try { signum = std::stoi(args[++i]); } catch (...) {}
					continue;
				}
				if (a.size() > 1 && a[0] == '-' && std::isdigit((unsigned char)a[1])) {
					try { signum = std::stoi(a.substr(1)); } catch (...) {}
					continue;
				}
				if (a == "-9"   || a == "-KILL") { signum = 9;  continue; }
				if (a == "-15"  || a == "-TERM") { signum = 15; continue; }
				if (a == "-2"   || a == "-INT")  { signum = 2;  continue; }
				if (a == "-1"   || a == "-HUP")  { signum = 1;  continue; }
				try { pids.push_back(std::stoi(a)); } catch (...) {
					std::fprintf(stderr, "wbsh: kill: %s: arguments must be PIDs\n", a.c_str());
					return 1;
				}
			}
			if (pids.empty()) {
				perr("kill", "usage: kill [-SIG] PID...");
				return 2;
			}
			int rc = 0;
#ifdef _WIN32
			(void)signum;
			for (int pid : pids) {
				HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
				if (!h) {
					std::fprintf(stderr, "wbsh: kill: %d: no such process\n", pid);
					rc = 1;
					continue;
				}
				if (!TerminateProcess(h, static_cast<UINT>(128 + signum))) {
					std::fprintf(stderr, "wbsh: kill: %d: cannot terminate\n", pid);
					rc = 1;
				}
				CloseHandle(h);
			}
#else
			(void)pids; (void)signum;
#endif
			return rc;
		}

		// ---- xargs (minimal) ------------------------------------------------

		int builtin_xargs(Executor& exec, const std::vector<std::string>& args) {
			int n_per = -1;   // -1 means "all in one batch"
			std::vector<std::string> cmd;
			for (std::size_t i = 0; i < args.size(); ++i) {
				const std::string& a = args[i];
				if (a == "-n" && i + 1 < args.size()) {
					try { n_per = std::stoi(args[++i]); } catch (...) { return 1; }
					continue;
				}
				if (a.size() > 2 && a.compare(0, 2, "-n") == 0) {
					try { n_per = std::stoi(a.substr(2)); } catch (...) { return 1; }
					continue;
				}
				if (a == "--") { for (++i; i < args.size(); ++i) cmd.push_back(args[i]); break; }
				cmd.push_back(a);
			}
			if (cmd.empty()) cmd.push_back("echo");

			// Split stdin on whitespace into items.
			std::vector<std::string> items;
			std::string cur;
			int c;
			while ((c = std::fgetc(stdin)) != EOF) {
				if (std::isspace(static_cast<unsigned char>(c))) {
					if (!cur.empty()) { items.push_back(std::move(cur)); cur.clear(); }
				} else cur.push_back(static_cast<char>(c));
			}
			if (!cur.empty()) items.push_back(std::move(cur));

			int rc = 0;
			auto invoke_batch = [&](std::vector<std::string> batch) {
				if (batch.empty() && n_per < 0) return;   // empty in single-batch mode: no-op
				std::vector<std::string> argv = cmd;
				for (auto& it : batch) argv.push_back(std::move(it));
				if (argv.empty()) return;
				if (exec.isFunction(argv[0]) || exec.isBuiltin(argv[0])) {
					std::vector<std::string> a(argv.begin() + 1, argv.end());
					int r = exec.isBuiltin(argv[0])
						? exec.callBuiltin(argv[0], a)
						: exec.callFunction(argv[0], a);
					if (r != 0) rc = r;
				} else {
					// Re-run via wbsh's executeText: build a quoted string. Quote
					// each arg conservatively (single-quote, escape inner ').
					std::string line;
					for (std::size_t k = 0; k < argv.size(); ++k) {
						if (k) line.push_back(' ');
						line.push_back('\'');
						for (char ch : argv[k]) {
							if (ch == '\'') line += "'\\''";
							else line.push_back(ch);
						}
						line.push_back('\'');
					}
					int r = exec.executeText(line, "<xargs>");
					if (r != 0) rc = r;
				}
			};

			if (n_per > 0) {
				for (std::size_t k = 0; k < items.size(); k += static_cast<std::size_t>(n_per)) {
					std::vector<std::string> batch;
					for (int j = 0; j < n_per && k + j < items.size(); ++j) {
						batch.push_back(items[k + j]);
					}
					invoke_batch(std::move(batch));
				}
			} else {
				invoke_batch(items);
			}
			return rc;
		}

	}  // namespace

	void registerCoreutils(Executor& exec) {
		exec.registerBuiltin("ls",       builtin_ls);
		exec.registerBuiltin("cat",      builtin_cat);
		exec.registerBuiltin("clear",    builtin_clear);
		exec.registerBuiltin("which",    builtin_which);
		exec.registerBuiltin("mkdir",    builtin_mkdir);
		exec.registerBuiltin("rmdir",    builtin_rmdir);
		exec.registerBuiltin("rm",       builtin_rm);
		exec.registerBuiltin("cp",       builtin_cp);
		exec.registerBuiltin("mv",       builtin_mv);
		exec.registerBuiltin("touch",    builtin_touch);
		exec.registerBuiltin("head",     builtin_head);
		exec.registerBuiltin("tail",     builtin_tail);
		exec.registerBuiltin("wc",       builtin_wc);
		exec.registerBuiltin("whoami",   builtin_whoami);
		exec.registerBuiltin("hostname", builtin_hostname);
		exec.registerBuiltin("env",      builtin_env);
		exec.registerBuiltin("sleep",    builtin_sleep);
		exec.registerBuiltin("basename", builtin_basename);
		exec.registerBuiltin("dirname",  builtin_dirname);
		exec.registerBuiltin("sort",     builtin_sort);
		exec.registerBuiltin("uniq",     builtin_uniq);
		exec.registerBuiltin("tr",       builtin_tr);
		exec.registerBuiltin("cut",      builtin_cut);
		exec.registerBuiltin("tee",      builtin_tee);
		exec.registerBuiltin("paste",    builtin_paste);
		exec.registerBuiltin("tac",      builtin_tac);
		exec.registerBuiltin("rev",      builtin_rev);
		exec.registerBuiltin("nl",       builtin_nl);
		exec.registerBuiltin("date",     builtin_date);
		exec.registerBuiltin("seq",      builtin_seq);
		exec.registerBuiltin("uname",    builtin_uname);
		exec.registerBuiltin("id",       builtin_id);
		exec.registerBuiltin("realpath", builtin_realpath);
		exec.registerBuiltin("readlink", builtin_readlink);
		exec.registerBuiltin("expr",     builtin_expr);
		exec.registerBuiltin("grep",     builtin_grep);
		exec.registerBuiltin("find",     builtin_find);
		exec.registerBuiltin("xargs",    builtin_xargs);
		exec.registerBuiltin("pushd",    builtin_pushd);
		exec.registerBuiltin("popd",     builtin_popd);
		exec.registerBuiltin("dirs",     builtin_dirs);
		exec.registerBuiltin("xxd",      builtin_xxd);
		exec.registerBuiltin("od",       builtin_od);
		exec.registerBuiltin("fold",     builtin_fold);
		exec.registerBuiltin("column",   builtin_column);
		exec.registerBuiltin("expand",   builtin_expand);
		exec.registerBuiltin("unexpand", builtin_unexpand);
		exec.registerBuiltin("comm",     builtin_comm);
		exec.registerBuiltin("yes",      builtin_yes);
		exec.registerBuiltin("nproc",    builtin_nproc);
		exec.registerBuiltin("tput",     builtin_tput);
		exec.registerBuiltin("mktemp",   builtin_mktemp);
		exec.registerBuiltin("kill",     builtin_kill);
		exec.registerBuiltin("sed",      builtin_sed);
		exec.registerBuiltin("awk",      builtin_awk);
		exec.registerBuiltin("gawk",     builtin_awk);
		exec.registerBuiltin("bc",       builtin_bc);
		exec.registerBuiltin("gzip",     builtin_gzip);
		exec.registerBuiltin("gunzip",   builtin_gunzip);
		exec.registerBuiltin("zcat",     builtin_zcat);
		exec.registerBuiltin("zip",      builtin_zip);
		exec.registerBuiltin("unzip",    builtin_unzip);
		exec.registerBuiltin("stat",     builtin_stat);
		exec.registerBuiltin("chmod",    builtin_chmod);
		exec.registerBuiltin("ln",       builtin_ln);
		exec.registerBuiltin("cmp",      builtin_cmp);
		exec.registerBuiltin("diff",     builtin_diff);
		exec.registerBuiltin("du",       builtin_du);
		exec.registerBuiltin("df",       builtin_df);
		exec.registerBuiltin("md5sum",   builtin_md5sum);
		exec.registerBuiltin("sha1sum",  builtin_sha1sum);
		exec.registerBuiltin("sha256sum",builtin_sha256sum);
		exec.registerBuiltin("sha512sum",builtin_sha512sum);
		exec.registerBuiltin("base64",   builtin_base64);
		exec.registerBuiltin("curl",     builtin_curl);
		exec.registerBuiltin("tar",      builtin_tar);
	}

}  // namespace wbsh

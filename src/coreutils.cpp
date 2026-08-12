/**
 * @file coreutils.cpp
 * @brief File / system coreutils plus the helpers shared by the
 *        coreutils_*.cpp family. Scope is pragmatic: enough flags to
 *        cover everyday interactive use and common scripts.
 */

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>

#  include <io.h>
#endif /* _WIN32 */

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

#include "awk.h"
#include "coreutils_internal.h"
#include "executor.h"
#include "inflate.h"
#include "numparse.h"
#include "regexutil.h"

namespace wbsh {

	namespace fs = std::filesystem;

	void perr(const std::string& cmd, const std::string& path, const std::error_code& ec) {
		std::fprintf(stderr, "wbsh: %s: %s: %s\n",
			cmd.c_str(), path.c_str(), ec.message().c_str());
	}

	void perr(const std::string& cmd, const std::string& msg) {
		std::fprintf(stderr, "wbsh: %s: %s\n", cmd.c_str(), msg.c_str());
	}

	fs::path toNative(Executor& exec, const std::string& p) {
		return utf8ToPath(exec.pathConv().toWin32(p));
	}

	std::FILE* fopenNative(Executor& exec, const std::string& p, const char* mode) {
		return openUtf8(exec.pathConv().toWin32(p), mode);
	}

	struct LsOpts {
		bool all = false;
		bool long_fmt = false;
		bool one = false;
		bool human = false;
		bool reverse = false;
		bool sort_mtime = false;
		bool sort_size = false;
		bool classify = false;
		enum { Auto, Always, Never } color = Auto;
	};

	static bool stdoutIsTty() {
#ifdef _WIN32
		return _isatty(_fileno(stdout)) != 0;
#else
		return false;
#endif
	}

	static int consoleWidth() {
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

	static bool windowsHidden(const fs::path& p) {
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
		if (!name.empty() && name[0] == '.') e.is_hidden = true;
		if (windowsHidden(e.full)) e.is_hidden = true;
		std::string ext = pathToUtf8(e.full.extension());
		std::transform(ext.begin(), ext.end(), ext.begin(),
			[](char c) { return static_cast<char>(std::tolower((unsigned char)c)); });
		if (ext == ".exe" || ext == ".bat" || ext == ".cmd" || ext == ".com" || ext == ".ps1") {
			e.is_executable = true;
		}

		e.valid = true;
		return e;
	}

	static std::string colorize(const LsEntry& e, bool use_color) {
		if (!use_color) return e.name;
		const char* code = nullptr;
		if (e.is_symlink) code = "\x1b[36;1m";
		else if (e.is_dir) code = "\x1b[34;1m";
		else if (e.is_executable) code = "\x1b[32;1m";
		if (!code) return e.name;
		return std::string(code) + e.name + "\x1b[0m";
	}

	static std::string classifySuffix(const LsEntry& e) {
		if (e.is_dir) return "/";
		if (e.is_symlink) return "@";
		if (e.is_executable) return "*";
		return "";
	}

	static std::string humanSize(std::uintmax_t n) {
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
		}
		else if (v >= 10.0) {
			std::snprintf(buf, sizeof(buf), "%.0f%s", v, units[u]);
		}
		else {
			std::snprintf(buf, sizeof(buf), "%.1f%s", v, units[u]);
		}

		return buf;
	}

	static std::string formatMtime(const fs::file_time_type& t) {
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
		char buf[32];
		std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
		return buf;
	}

	static std::string permString(const LsEntry& e) {
		std::string s;
		s += e.is_symlink ? 'l' : (e.is_dir ? 'd' : '-');
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

	static bool lsNameLess(const LsEntry& a, const LsEntry& b) {
		const std::size_t n = a.name.size() < b.name.size() ? a.name.size() : b.name.size();
		for (std::size_t i = 0; i < n; ++i) {
			const int ca = std::tolower(static_cast<unsigned char>(a.name[i]));
			const int cb = std::tolower(static_cast<unsigned char>(b.name[i]));
			if (ca != cb) return ca < cb;
		}

		if (a.name.size() != b.name.size()) return a.name.size() < b.name.size();
		return a.name < b.name;
	}

	static void sortEntries(std::vector<LsEntry>& items, const LsOpts& opts) {
		if (opts.sort_mtime) {
			std::sort(items.begin(), items.end(),
				[](const LsEntry& a, const LsEntry& b) {
					if (a.mtime != b.mtime) return a.mtime > b.mtime;
					return lsNameLess(a, b);
				});
		}
		else if (opts.sort_size) {
			std::sort(items.begin(), items.end(),
				[](const LsEntry& a, const LsEntry& b) {
					if (a.size != b.size) return a.size > b.size;
					return lsNameLess(a, b);
				});
		}
		else {
			std::sort(items.begin(), items.end(), lsNameLess);
		}

		if (opts.reverse) std::reverse(items.begin(), items.end());
	}

	static void printColumns(const std::vector<LsEntry>& items, const LsOpts& opts,
	                         bool use_color) {
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
					std::size_t visible = labels[idx].size();
					for (std::size_t k = visible; k < pad; ++k) std::fputc(' ', stdout);
				}
			}

			std::fputc('\n', stdout);
		}
	}

	static void printLong(const std::vector<LsEntry>& items, const LsOpts& opts,
	                      bool use_color, Executor& exec) {
		std::string user = exec.env().get("USER");
		if (user.empty()) user = exec.env().get("USERNAME");
		if (user.empty()) user = "user";
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
			std::string spad(size_w - s.size(), ' ');
			std::fprintf(stdout, "%s 1 %s %s %s%s %s %s\n",
				p.c_str(), user.c_str(), user.c_str(),
				spad.c_str(), s.c_str(), mt.c_str(), label.c_str());
		}
	}

	static int parseLsArgs(const std::vector<std::string>& args,
	                       LsOpts& opts, std::vector<std::string>& paths) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& a = args[i];
			if (a == "--") {
				for (++i; i < args.size(); ++i) paths.push_back(args[i]);
				break;
			}

			if      (a == "--color" || a == "--color=auto")  opts.color = LsOpts::Auto;
			else if (a == "--color=always" || a == "--color=yes") opts.color = LsOpts::Always;
			else if (a == "--color=never"  || a == "--color=no")  opts.color = LsOpts::Never;
			else if (a == "--all")            opts.all = true;
			else if (a == "--human-readable") opts.human = true;
			else if (a == "--reverse")        opts.reverse = true;
			else if (a == "--classify")       opts.classify = true;
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
			}
			else {
				paths.push_back(a);
			}
		}

		if (paths.empty()) paths.push_back(".");
		return 0;
	}

	static bool collectLsDirectoryEntries(const fs::path& target, const LsOpts& opts,
	                                      const std::string& source_path,
	                                      std::vector<LsEntry>& items) {
		std::error_code ec;
		fs::directory_iterator it(target, ec);
		if (ec) {
			perr("ls", source_path, ec);
			return false;
		}

		for (const auto& de : it) {
			std::string name = pathToUtf8(de.path().filename());
			LsEntry e = collect(target, name);
			if (!opts.all && e.is_hidden) continue;
			items.push_back(std::move(e));
		}

		return true;
	}

	static void emitLsItems(const std::vector<LsEntry>& items, const LsOpts& opts,
	                        bool use_color, Executor& exec) {
		if (opts.long_fmt) printLong(items, opts, use_color, exec);
		else               printColumns(items, opts, use_color);
	}

	static int builtin_ls(Executor& exec, const std::vector<std::string>& args) {
		LsOpts opts;
		std::vector<std::string> paths;
		const int parse_rc = parseLsArgs(args, opts, paths);
		if (parse_rc != 0) return parse_rc;

		const bool use_color = (opts.color == LsOpts::Always)
			|| (opts.color == LsOpts::Auto && stdoutIsTty());

		int rc = 0;
		const bool show_headers = paths.size() > 1;
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

				if (!collectLsDirectoryEntries(target, opts, p, items)) {
					rc = 1;
					continue;
				}
			} else {
				LsEntry e = collect(fs::path(), p);
				e.name = p;
				items.push_back(std::move(e));
			}

			sortEntries(items, opts);
			emitLsItems(items, opts, use_color, exec);
		}

		std::fflush(stdout);
		return rc;
	}

	struct CatOptions {
		bool number = false;
		bool number_nonblank = false;
		std::vector<std::string> files;
	};

	struct CatEmitState {
		bool at_line_start = true;
		std::size_t lineno = 0;
	};

	static CatOptions parseCatArgs(const std::vector<std::string>& args) {
		CatOptions o;
		std::size_t i = 0;
		while (i < args.size()) {
			const std::string& a = args[i];
			if (a == "--") { ++i; while (i < args.size()) o.files.push_back(args[i++]); break; }
			if (a == "-n" || a == "--number") { o.number = true; ++i; continue; }
			if (a == "-b" || a == "--number-nonblank") { o.number_nonblank = true; ++i; continue; }
			if (a == "-") { o.files.push_back("-"); ++i; continue; }
			if (a.size() > 1 && a[0] == '-' && a[1] != '-') {
				bool ok = true;
				for (std::size_t k = 1; k < a.size(); ++k) {
					if (a[k] == 'n') o.number = true;
					else if (a[k] == 'b') o.number_nonblank = true;
					else { ok = false; break; }
				}

				if (ok) { ++i; continue; }
			}

			o.files.push_back(a);
			++i;
		}

		if (o.files.empty()) o.files.push_back("-");
		return o;
	}

	static void catEmitChunk(const char* data, std::size_t n,
	                         const CatOptions& opts, CatEmitState& st) {
		if (!opts.number && !opts.number_nonblank) {
			std::fwrite(data, 1, n, stdout);
			return;
		}

		for (std::size_t p = 0; p < n; ++p) {
			if (st.at_line_start) {
				bool blank_line = (data[p] == '\n');
				if (!opts.number_nonblank || !blank_line) {
					++st.lineno;
					std::fprintf(stdout, "%6zu\t", st.lineno);
				}

				st.at_line_start = false;
			}

			std::fputc(data[p], stdout);
			if (data[p] == '\n') st.at_line_start = true;
		}
	}

	static int catStreamFile(Executor& exec, const std::string& name,
	                         const CatOptions& opts, CatEmitState& st) {
		if (name == "-") {
			char buf[4096];
			while (true) {
				std::size_t got = std::fread(buf, 1, sizeof(buf), stdin);
				if (got == 0) break;
				catEmitChunk(buf, got, opts, st);
			}

			return 0;
		}

		fs::path native = toNative(exec, name);
		std::ifstream in(native, std::ios::binary);
		if (!in) {
			std::fprintf(stderr, "wbsh: cat: %s: %s\n",
				name.c_str(), std::strerror(errno));
			return 1;
		}

		char buf[4096];
		while (in) {
			in.read(buf, sizeof(buf));
			std::streamsize got = in.gcount();
			if (got > 0) catEmitChunk(buf, static_cast<std::size_t>(got), opts, st);
		}

		return 0;
	}

	static int builtin_cat(Executor& exec, const std::vector<std::string>& args) {
		CatOptions opts = parseCatArgs(args);
		CatEmitState st;
		int rc = 0;
		for (const auto& f : opts.files) {
			if (catStreamFile(exec, f, opts, st) != 0) rc = 1;
		}

		std::fflush(stdout);
		return rc;
	}

	static int builtin_clear(Executor&, const std::vector<std::string>&) {
		std::fputs("\x1b[H\x1b[2J\x1b[3J", stdout);
		std::fflush(stdout);
		return 0;
	}

	static int builtin_which(Executor& exec, const std::vector<std::string>& args) {
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
				}
				else cur.push_back(c);
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

	static int builtin_mkdir(Executor& exec, const std::vector<std::string>& args) {
		bool parents = false;
		std::vector<std::string> paths;
		for (const auto& a : args) {
			if (a == "-p" || a == "--parents") parents = true;
			else if (a.size() > 1 && a[0] == '-' && a[1] != '-') {
				for (std::size_t k = 1; k < a.size(); ++k) if (a[k] == 'p') parents = true;
			}
			else paths.push_back(a);
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

	static int builtin_rmdir(Executor& exec, const std::vector<std::string>& args) {
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

	static int builtin_rm(Executor& exec, const std::vector<std::string>& args) {
		bool recursive = false, force = false;
		std::vector<std::string> paths;
		for (const auto& a : args) {
			if (a == "-r" || a == "-R" || a == "--recursive") recursive = true;
			else if (a == "-f" || a == "--force") force = true;
			else if (a == "-rf" || a == "-fr" || a == "-Rf" || a == "-fR") {
				recursive = true; force = true;
			}
			else if (a.size() > 1 && a[0] == '-' && a[1] != '-') {
				for (std::size_t k = 1; k < a.size(); ++k) {
					if (a[k] == 'r' || a[k] == 'R') recursive = true;
					else if (a[k] == 'f') force = true;
				}
			}
			else paths.push_back(a);
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
			}
			else {
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

	static int builtin_cp(Executor& exec, const std::vector<std::string>& args) {
		bool recursive = false;
		std::vector<std::string> paths;
		for (const auto& a : args) {
			if (a == "-r" || a == "-R" || a == "--recursive") recursive = true;
			else if (a == "-a") recursive = true;
			else if (a.size() > 1 && a[0] == '-' && a[1] != '-') {
				for (std::size_t k = 1; k < a.size(); ++k) {
					if (a[k] == 'r' || a[k] == 'R') recursive = true;
				}
			}
			else paths.push_back(a);
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

	static int builtin_mv(Executor& exec, const std::vector<std::string>& args) {
		std::vector<std::string> paths;
		for (const auto& a : args) {
			if (a == "--") continue;
			if (a.size() > 1 && a[0] == '-' && a[1] != '-' && a != "-") continue;
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

	static int builtin_touch(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) { perr("touch", "missing operand"); return 1; }
		int rc = 0;
		for (const auto& p : args) {
			if (!p.empty() && p[0] == '-' && p != "-") continue;
			fs::path nat = toNative(exec, p);
			std::error_code ec;
			if (!fs::exists(nat, ec)) {
				std::ofstream f(nat, std::ios::binary | std::ios::app);
				if (!f) { perr("touch", p + ": cannot create"); rc = 1; continue; }
			}
			else {
				auto now = fs::file_time_type::clock::now();
				fs::last_write_time(nat, now, ec);
				if (ec) { perr("touch", p, ec); rc = 1; }
			}
		}

		return rc;
	}

	static int parseNumFlag(const std::vector<std::string>& args, const char* short_flag,
		std::size_t& i, long& out) {
		const std::string& a = args[i];
		if (a.size() > 2 && a[0] == '-' && a[1] == short_flag[0]
		    && std::isdigit((unsigned char)a[2])) {
			int v = 0;
			if (!parseInt(a.substr(2), v)) return -1;
			out = v; ++i; return 0;
		}

		if (a == short_flag || a == std::string("-") + short_flag) {
			if (i + 1 >= args.size()) return -1;
			int v = 0;
			if (!parseInt(args[i + 1], v)) return -1;
			out = v; i += 2; return 0;
		}

		return 1;
	}

	static int builtin_head(Executor& exec, const std::vector<std::string>& args) {
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
				int v = 0;
				if (!parseInt(a.substr(1), v)) { perr("head", "bad N"); return 1; }
				n = v;
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

	static int builtin_tail(Executor& exec, const std::vector<std::string>& args) {
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
				int v = 0;
				if (!parseInt(a.substr(1), v)) { perr("tail", "bad N"); return 1; }
				n = v;
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
				}
				else cur.push_back(static_cast<char>(c));
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

	static void appendWcCount(std::string& out, std::uintmax_t v) {
		if (!out.empty()) out += " ";
		char buf[64];
		std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
		out += buf;
	}

	struct WcOptions {
		bool want_l = false, want_w = false, want_c = false, want_m = false;
		std::vector<std::string> files;
	};

	struct WcCounts {
		std::uintmax_t l = 0, w = 0, c = 0, m = 0;
	};

	static WcOptions parseWcArgs(const std::vector<std::string>& args) {
		WcOptions o;
		for (const auto& a : args) {
			if (a == "--") continue;
			if (a == "-l" || a == "--lines") { o.want_l = true; continue; }
			if (a == "-w" || a == "--words") { o.want_w = true; continue; }
			if (a == "-c" || a == "--bytes") { o.want_c = true; continue; }
			if (a == "-m" || a == "--chars") { o.want_m = true; continue; }
			if (a.size() > 1 && a[0] == '-' && a[1] != '-') {
				for (std::size_t k = 1; k < a.size(); ++k) {
					if (a[k] == 'l') o.want_l = true;
					else if (a[k] == 'w') o.want_w = true;
					else if (a[k] == 'c') o.want_c = true;
					else if (a[k] == 'm') o.want_m = true;
				}
				continue;
			}

			o.files.push_back(a);
		}

		if (!(o.want_l || o.want_w || o.want_c || o.want_m)) {
			o.want_l = o.want_w = o.want_c = true;
		}

		if (o.files.empty()) o.files.push_back("-");
		return o;
	}

	static WcCounts wcCountStream(FILE* fp) {
		WcCounts r;
		bool in_word = false;
		int ch;
		while ((ch = std::fgetc(fp)) != EOF) {
			++r.c;
			if (static_cast<unsigned char>(ch) < 0x80
				|| (static_cast<unsigned char>(ch) & 0xC0) != 0x80) ++r.m;
			if (ch == '\n') ++r.l;
			if (std::isspace(static_cast<unsigned char>(ch))) {
				in_word = false;
			} else if (!in_word) {
				in_word = true;
				++r.w;
			}
		}

		return r;
	}

	static std::string formatWcLine(const WcOptions& opts, const WcCounts& cnt,
	                                const std::string& label) {
		std::string out;
		if (opts.want_l) appendWcCount(out, cnt.l);
		if (opts.want_w) appendWcCount(out, cnt.w);
		if (opts.want_m && !opts.want_c) appendWcCount(out, cnt.m);
		if (opts.want_c) appendWcCount(out, cnt.c);
		if (!label.empty()) { out += " "; out += label; }
		out.push_back('\n');
		return out;
	}

	static int builtin_wc(Executor& exec, const std::vector<std::string>& args) {
		WcOptions opts = parseWcArgs(args);
		WcCounts totals;
		int rc = 0;
		for (const auto& f : opts.files) {
			FILE* fp = (f == "-") ? stdin : fopenNative(exec, f, "rb");
			if (!fp) { perr("wc", f + ": " + std::strerror(errno)); rc = 1; continue; }
			WcCounts cnt = wcCountStream(fp);
			if (fp != stdin) std::fclose(fp);
			std::fputs(formatWcLine(opts, cnt, f == "-" ? "" : f).c_str(), stdout);
			totals.l += cnt.l; totals.w += cnt.w; totals.c += cnt.c; totals.m += cnt.m;
		}

		if (opts.files.size() > 1) {
			std::fputs(formatWcLine(opts, totals, "total").c_str(), stdout);
		}

		std::fflush(stdout);
		return rc;
	}

	static int builtin_whoami(Executor& exec, const std::vector<std::string>&) {
		std::string u = exec.env().get("USER");
		if (u.empty()) u = exec.env().get("USERNAME");
		if (u.empty()) u = "user";
		std::printf("%s\n", u.c_str());
		return 0;
	}

	static int builtin_hostname(Executor& exec, const std::vector<std::string>&) {
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

	static int builtin_env(Executor& exec, const std::vector<std::string>& args) {
		std::vector<std::pair<std::string, std::string>> sets;
		std::vector<std::string> cmd;
		for (const auto& a : args) {
			if (cmd.empty() && a.find('=') != std::string::npos
				&& (std::isalpha((unsigned char)a[0]) || a[0] == '_')) {
				auto eq = a.find('=');
				sets.emplace_back(a.substr(0, eq), a.substr(eq + 1));
			}
			else {
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
			for (auto& kv : all) {
				if (kv.first == s.first) {
					kv.second = s.second; found = true; break;
				}
			}

			if (!found) all.push_back(s);
		}

		std::sort(all.begin(), all.end());
		for (const auto& kv : all) {
			if (!exec.env().isExported(kv.first)) continue;
			std::printf("%s=%s\n", kv.first.c_str(), kv.second.c_str());
		}

		return 0;
	}

	static int builtin_sleep(Executor&, const std::vector<std::string>& args) {
		if (args.empty()) { perr("sleep", "missing operand"); return 1; }
		double secs = 0.0;
		const std::string& a = args[0];
		char suffix = (!a.empty()) ? a.back() : '\0';
		std::string num = (suffix == 's' || suffix == 'm' || suffix == 'h')
			? a.substr(0, a.size() - 1) : a;
		if (!parseDouble(num, secs)) {
			perr("sleep", args[0] + ": invalid time interval");
			return 1;
		}

		if (suffix == 'm') secs *= 60.0;
		else if (suffix == 'h') secs *= 3600.0;
		std::this_thread::sleep_for(std::chrono::milliseconds(
			static_cast<long long>(secs * 1000.0)));
		return 0;
	}

	static int builtin_basename(Executor&, const std::vector<std::string>& args) {
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

	static int builtin_dirname(Executor&, const std::vector<std::string>& args) {
		if (args.empty()) { perr("dirname", "missing operand"); return 1; }
		std::string s = args[0];
		while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
		auto pos = s.find_last_of("/\\");
		if (pos == std::string::npos) { std::printf(".\n"); return 0; }
		if (pos == 0) { std::printf("/\n"); return 0; }
		std::printf("%s\n", s.substr(0, pos).c_str());
		return 0;
	}

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

	static int builtin_date(Executor&, const std::vector<std::string>& args) {
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

	static int builtin_seq(Executor&, const std::vector<std::string>& args) {
		double first = 1.0, inc = 1.0, last = 1.0;
		std::string sep = "\n";
		std::vector<std::string> nums;
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& a = args[i];
			if (a == "-s" && i + 1 < args.size()) sep = args[++i];
			else if (a.size() > 2 && a.compare(0, 2, "-s") == 0) sep = a.substr(2);
			else if (a == "--") {
				for (++i; i < args.size(); ++i) nums.push_back(args[i]);
			}
			else nums.push_back(a);
		}

		bool num_ok = true;
		if (nums.size() == 1) { num_ok = parseDouble(nums[0], last); }
		else if (nums.size() == 2) {
			num_ok = parseDouble(nums[0], first) && parseDouble(nums[1], last);
		}
		else if (nums.size() == 3) {
			num_ok = parseDouble(nums[0], first) && parseDouble(nums[1], inc)
			    && parseDouble(nums[2], last);
		}
		else { perr("seq", "usage: seq [LAST | FIRST LAST | FIRST INC LAST]"); return 1; }
		if (!num_ok) { perr("seq", "invalid number"); return 1; }
		if (inc == 0) { perr("seq", "increment must be non-zero"); return 1; }
		bool integer = std::floor(first) == first && std::floor(inc) == inc
		    && std::floor(last) == last;
		bool first_out = true;
		auto emit = [&](double v) {
			if (!first_out) std::fputs(sep.c_str(), stdout);
			if (integer) std::fprintf(stdout, "%lld", static_cast<long long>(v));
			else         std::fprintf(stdout, "%g", v);
			first_out = false;
			};
		if (inc > 0) {
			for (double v = first; v <= last + 1e-12; v += inc) emit(v);
		}
		else {
			for (double v = first; v >= last - 1e-12; v += inc) emit(v);
		}

		std::fputc('\n', stdout);
		std::fflush(stdout);
		return 0;
	}

	static int builtin_uname(Executor& exec, const std::vector<std::string>& args) {
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
		std::string opsys = "Windows";

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

	static int builtin_id(Executor& exec, const std::vector<std::string>& args) {
		bool name_only = false, user_only = false, group_only = false;
		for (const auto& a : args) {
			if (a == "-n") name_only = true;
			else if (a == "-u") user_only = true;
			else if (a == "-g") group_only = true;
		}

		std::string user = exec.env().get("USER");
		if (user.empty()) user = exec.env().get("USERNAME");
		if (user.empty()) user = "user";
		int uid = 1000, gid = 1000;
		if (user_only) {
			if (name_only) std::printf("%s\n", user.c_str());
			else           std::printf("%d\n", uid);
		}
		else if (group_only) {
			if (name_only) std::printf("%s\n", user.c_str());
			else           std::printf("%d\n", gid);
		}
		else {
			std::printf("uid=%d(%s) gid=%d(%s) groups=%d(%s)\n",
				uid, user.c_str(), gid, user.c_str(), gid, user.c_str());
		}

		return 0;
	}

	static int builtin_realpath(Executor& exec, const std::vector<std::string>& args) {
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

	static int builtin_readlink(Executor& exec, const std::vector<std::string>& args) {
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
			}
			else {
				fs::path q = fs::read_symlink(toNative(exec, p), ec);
				if (ec) { rc = 1; continue; }
				std::printf("%s\n", exec.pathConv().toPosix(pathToUtf8(q)).c_str());
			}
		}

		return rc;
	}

	static int exprBinaryIntOp(long long li, long long ri, const std::string& op) {
		if (op == "+") { std::printf("%lld\n", li + ri); return 0; }
		if (op == "-") { std::printf("%lld\n", li - ri); return 0; }
		if (op == "*") { std::printf("%lld\n", li * ri); return 0; }
		if (op == "/") {
			if (ri == 0) return 2;
			std::printf("%lld\n", li / ri); return 0;
		}

		if (op == "%") {
			if (ri == 0) return 2;
			std::printf("%lld\n", li % ri); return 0;
		}

		if (op == "<")  { std::printf("%d\n", li <  ri); return (li <  ri) ? 0 : 1; }
		if (op == "<=") { std::printf("%d\n", li <= ri); return (li <= ri) ? 0 : 1; }
		if (op == ">")  { std::printf("%d\n", li >  ri); return (li >  ri) ? 0 : 1; }
		if (op == ">=") { std::printf("%d\n", li >= ri); return (li >= ri) ? 0 : 1; }
		if (op == "=" || op == "==") {
			std::printf("%d\n", li == ri); return (li == ri) ? 0 : 1;
		}

		if (op == "!=") {
			std::printf("%d\n", li != ri); return (li != ri) ? 0 : 1;
		}

		return -1;
	}

	static int exprBinaryStringOp(const std::string& l, const std::string& r,
	                              const std::string& op) {
		if (op == "=" || op == "==") {
			std::printf("%d\n", l == r); return (l == r) ? 0 : 1;
		}

		if (op == "!=") {
			std::printf("%d\n", l != r); return (l != r) ? 0 : 1;
		}

		return -1;
	}

	static int builtin_expr(Executor&, const std::vector<std::string>& args) {
		if (args.empty()) { perr("expr", "missing operand"); return 2; }
		if (args.size() == 2 && args[0] == "length") {
			std::printf("%zu\n", args[1].size());
			return 0;
		}

		if (args.size() == 4 && args[0] == "substr") {
			int p = 0, n = 0;
			if (!parseInt(args[2], p) || !parseInt(args[3], n)) return 2;
			if (p < 1) p = 1;
			std::size_t start = static_cast<std::size_t>(p - 1);
			if (start >= args[1].size()) { std::printf("\n"); return 1; }
			std::printf("%s\n", args[1].substr(start, n).c_str());
			return 0;
		}

		if (args.size() == 3) {
			const std::string& l  = args[0];
			const std::string& op = args[1];
			const std::string& r  = args[2];
			long long li = 0, ri = 0;
			if (parseLL(l, li) && parseLL(r, ri)) {
				int rc = exprBinaryIntOp(li, ri, op);
				if (rc >= 0) return rc;
			}
			else {
				int rc = exprBinaryStringOp(l, r, op);
				if (rc >= 0) return rc;
			}
		}

		std::string out;
		for (std::size_t i = 0; i < args.size(); ++i) {
			if (i) out.push_back(' ');
			out += args[i];
		}

		std::printf("%s\n", out.c_str());
		return 0;
	}

	static int builtin_cmp(Executor& exec, const std::vector<std::string>& args) {
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
		if (!f2) {
			std::fclose(f1);
			if (!quiet) perr("cmp", files[1] + ": " + std::strerror(errno));
			return 2;
		}

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

	static void diffEmitRange(int a, int b) {
		if (a == b) std::printf("%d", a);
		else        std::printf("%d,%d", a, b);
	}

	namespace diff_internal {
		struct DiffOptions {
			bool brief = false;
			bool unified = false;
			int context = 3;
			std::vector<std::string> files;
		};

		struct UnifiedItem {
			char kind;
			int a, b;
			std::string text;
		};

		struct NormalHunk {
			int a1, a2;
			int b1, b2;
			char kind;
		};
	}  // namespace diff_internal

	static diff_internal::DiffOptions parseDiffArgs(const std::vector<std::string>& args) {
		diff_internal::DiffOptions o;
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& a = args[i];
			if (a == "-q" || a == "--brief")   { o.brief = true; continue; }
			if (a == "-u" || a == "--unified") { o.unified = true; continue; }
			if (a.size() > 2 && a.compare(0, 2, "-U") == 0) {
				o.unified = true;
				parseInt(a.substr(2), o.context);
				continue;
			}

			if (a == "-U" && i + 1 < args.size()) {
				o.unified = true;
				parseInt(args[++i], o.context);
				continue;
			}

			if (!a.empty() && a[0] == '-') continue;
			o.files.push_back(a);
		}

		return o;
	}

	static std::vector<std::vector<int>>
	buildLcsTable(const std::vector<std::string>& A, const std::vector<std::string>& B) {
		const std::size_t m = A.size();
		const std::size_t n = B.size();
		std::vector<std::vector<int>> L(m + 1, std::vector<int>(n + 1, 0));
		for (std::size_t i = 1; i <= m; ++i) {
			for (std::size_t j = 1; j <= n; ++j) {
				if (A[i - 1] == B[j - 1]) L[i][j] = L[i - 1][j - 1] + 1;
				else                      L[i][j] = (std::max)(L[i - 1][j], L[i][j - 1]);
			}
		}

		return L;
	}

	static std::vector<diff_internal::UnifiedItem>
	walkLcsToUnifiedItems(const std::vector<std::string>& A,
	                      const std::vector<std::string>& B,
	                      const std::vector<std::vector<int>>& L) {
		std::vector<diff_internal::UnifiedItem> items;
		int ai = static_cast<int>(A.size());
		int bj = static_cast<int>(B.size());
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
		return items;
	}

	static int findHunkLastChange(const std::vector<diff_internal::UnifiedItem>& items,
	                              int i, int context) {
		const int N = static_cast<int>(items.size());
		int last_change = i;
		int j = i + 1;
		while (j < N) {
			if (items[j].kind != ' ') {
				last_change = j;
				++j;
				continue;
			}

			int run = 0;
			int k = j;
			while (k < N && items[k].kind == ' ' && run < 2 * context) {
				++run;
				++k;
			}

			if (k < N && items[k].kind != ' ') {
				j = k;
				continue;
			}
			break;
		}

		return last_change;
	}

	static void emitUnifiedHunk(const std::vector<diff_internal::UnifiedItem>& items,
	                            int hstart, int hend) {
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
	}

	static int emitUnifiedDiff(const std::vector<std::string>& A,
	                           const std::vector<std::string>& B,
	                           const std::vector<std::vector<int>>& L,
	                           const std::string& fa, const std::string& fb,
	                           int context) {
		const auto items = walkLcsToUnifiedItems(A, B, L);
		std::printf("--- %s\n+++ %s\n", fa.c_str(), fb.c_str());

		const int N = static_cast<int>(items.size());
		int i = 0;
		while (i < N) {
			while (i < N && items[i].kind == ' ') ++i;
			if (i >= N) break;
			const int change_start = i;
			const int last_change = findHunkLastChange(items, i, context);
			const int hstart = (std::max)(0, change_start - context);
			const int hend = (std::min)(N - 1, last_change + context);
			emitUnifiedHunk(items, hstart, hend);
			i = hend + 1;
		}

		return 1;
	}

	static std::vector<diff_internal::NormalHunk>
	buildNormalHunks(const std::vector<std::string>& A,
	                 const std::vector<std::string>& B,
	                 const std::vector<std::vector<int>>& L) {
		std::vector<diff_internal::NormalHunk> hunks;
		std::size_t i = A.size();
		std::size_t j = B.size();
		while (i > 0 || j > 0) {
			if (i > 0 && j > 0 && A[i - 1] == B[j - 1]) {
				--i; --j;
				continue;
			}

			const std::size_t ei = i;
			const std::size_t ej = j;
			while (i > 0 && j > 0 && A[i - 1] != B[j - 1]) {
				if (L[i - 1][j] >= L[i][j - 1]) --i;
				else                            --j;
			}

			while (i > 0 && (j == 0 || L[i - 1][j] >= L[i][j])) --i;
			while (j > 0 && (i == 0 || L[i][j - 1] >  L[i][j])) --j;

			int a1 = static_cast<int>(i + 1);
			const int a2 = static_cast<int>(ei);
			int b1 = static_cast<int>(j + 1);
			const int b2 = static_cast<int>(ej);
			char kind;
			if      (a1 > a2 && b1 <= b2) { kind = 'a'; --a1; }
			else if (b1 > b2 && a1 <= a2) { kind = 'd'; --b1; }
			else                          { kind = 'c'; }
			hunks.push_back({ a1, a2, b1, b2, kind });
		}

		std::reverse(hunks.begin(), hunks.end());
		return hunks;
	}

	static int emitNormalDiff(const std::vector<std::string>& A,
	                          const std::vector<std::string>& B,
	                          const std::vector<std::vector<int>>& L) {
		const auto hunks = buildNormalHunks(A, B, L);
		for (const auto& h : hunks) {
			diffEmitRange(h.a1, h.a2);
			std::putchar(h.kind);
			diffEmitRange(h.b1, h.b2);
			std::putchar('\n');
			if (h.kind == 'd' || h.kind == 'c') {
				for (int k = h.a1; k <= h.a2; ++k) {
					if (k - 1 < static_cast<int>(A.size()))
						std::printf("< %s\n", A[k - 1].c_str());
				}
			}

			if (h.kind == 'c') std::puts("---");
			if (h.kind == 'a' || h.kind == 'c') {
				for (int k = h.b1; k <= h.b2; ++k) {
					if (k - 1 < static_cast<int>(B.size()))
						std::printf("> %s\n", B[k - 1].c_str());
				}
			}
		}

		return 1;
	}

	static int builtin_diff(Executor& exec, const std::vector<std::string>& args) {
		diff_internal::DiffOptions o = parseDiffArgs(args);
		if (o.files.size() < 2) {
			perr("diff", "usage: diff [-q] FILE1 FILE2");
			return 2;
		}

		std::vector<std::string> A;
		std::vector<std::string> B;
		if (!readAllLines(exec, o.files[0], A) || !readAllLines(exec, o.files[1], B))
			return 2;
		if (A == B) return 0;

		if (o.brief) {
			std::printf("Files %s and %s differ\n",
				o.files[0].c_str(), o.files[1].c_str());
			return 1;
		}

		const auto L = buildLcsTable(A, B);
		if (o.unified) {
			return emitUnifiedDiff(A, B, L, o.files[0], o.files[1], o.context);
		}

		return emitNormalDiff(A, B, L);
	}

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

	static std::string duFormatSize(std::uintmax_t bytes, bool human) {
		if (human) return humanSize(bytes);
		return std::to_string((bytes + 1023) / 1024);
	}

	static int duEmitDirectory(const fs::path& nat, const std::string& label,
	                           bool human, bool all) {
		std::error_code ec;
		fs::recursive_directory_iterator it(nat,
			fs::directory_options::skip_permission_denied, ec);
		if (ec) {
			std::fprintf(stderr, "wbsh: du: %s\n", ec.message().c_str());
			return 1;
		}

		std::uintmax_t grand = 0;
		for (auto cur = it; cur != fs::recursive_directory_iterator(); cur.increment(ec)) {
			if (ec) break;
			std::error_code fec;
			if (cur->is_regular_file(fec)) {
				std::uintmax_t s = cur->file_size(fec);
				if (!fec) grand += s;
				if (all) {
					std::printf("%s\t%s\n",
						duFormatSize(s, human).c_str(), pathToUtf8(cur->path()).c_str());
				}
			}
		}

		std::printf("%s\t%s\n", duFormatSize(grand, human).c_str(), label.c_str());
		return 0;
	}

	static int builtin_du(Executor& exec, const std::vector<std::string>& args) {
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
				std::printf("%s\t%s\n", duFormatSize(total, human).c_str(), p.c_str());
				continue;
			}

			if (fs::is_regular_file(nat, ec)) {
				std::printf("%s\t%s\n",
					duFormatSize(fs::file_size(nat, ec), human).c_str(), p.c_str());
				continue;
			}

			rc |= duEmitDirectory(nat, p, human, all);
		}

		return rc;
	}

	static int builtin_df(Executor& exec, const std::vector<std::string>& args) {
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
		}
		else {
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

	static int builtin_stat(Executor& exec, const std::vector<std::string>& args) {
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

	static int builtin_chmod(Executor& exec, const std::vector<std::string>& args) {
		if (args.size() < 2) {
			perr("chmod", "usage: chmod MODE FILE...");
			return 1;
		}

		const std::string& mode = args[0];
		int rc = 0;
		bool writable = true;
		if (mode == "-w" || mode == "u-w" || mode == "a-w" || mode == "go-w") {
			writable = false;
		}
		else if (mode == "+w" || mode == "u+w" || mode == "a+w" || mode == "go+w") {
			writable = true;
		}
		else if (mode.size() == 3
			&& std::isdigit((unsigned char)mode[0])
			&& std::isdigit((unsigned char)mode[1])
			&& std::isdigit((unsigned char)mode[2])) {
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
			else          attr |= FILE_ATTRIBUTE_READONLY;
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

	static int builtin_ln(Executor& exec, const std::vector<std::string>& args) {
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
		std::string link = rest[1];
		std::string nat_target = exec.pathConv().toWin32(target);
		std::string nat_link = exec.pathConv().toWin32(link);
#ifdef _WIN32
		if (force) DeleteFileA(nat_link.c_str());
		BOOL ok = FALSE;
		if (symbolic) {
			DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
			std::error_code ec;
			if (fs::is_directory(nat_target, ec)) flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
			ok = CreateSymbolicLinkA(nat_link.c_str(), nat_target.c_str(), flags);
		}
		else {
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

	static std::string currentCwdPosix(Executor& exec) {
		std::error_code ec;
		auto cwd = fs::current_path(ec);
		if (ec) return exec.env().get("PWD");
		return exec.pathConv().toPosix(pathToUtf8(cwd));
	}

	static void printDirStack(Executor& exec, bool numbered) {
		const auto& s = exec.dirStack();
		if (s.empty()) {
			std::printf("%s\n", currentCwdPosix(exec).c_str());
			return;
		}

		if (numbered) {
			for (std::size_t i = 0; i < s.size(); ++i) {
				std::printf("%2zu  %s\n", i, s[i].c_str());
			}
		}
		else {
			for (std::size_t i = 0; i < s.size(); ++i) {
				if (i) std::fputc(' ', stdout);
				std::fputs(s[i].c_str(), stdout);
			}

			std::fputc('\n', stdout);
		}
	}

	static int builtin_pushd(Executor& exec, const std::vector<std::string>& args) {
		auto& stack = exec.dirStack();
		std::string cur = currentCwdPosix(exec);
		if (stack.empty()) stack.push_back(cur);

		if (args.empty()) {
			if (stack.size() < 2) {
				perr("pushd", "no other directory");
				return 1;
			}

			std::swap(stack[0], stack[1]);
		}
		else {
			const std::string& target = args[0];
			fs::path nat = toNative(exec, target);
			std::error_code ec;
			fs::current_path(nat, ec);
			if (ec) { perr("pushd", target, ec); return 1; }
			stack.insert(stack.begin(), exec.pathConv().toPosix(
				pathToUtf8(fs::current_path(ec))));
		}

		std::error_code ec;
		fs::current_path(toNative(exec, stack[0]), ec);
		if (!ec) {
			exec.env().set("OLDPWD", cur);
			exec.env().set("PWD", stack[0]);
		}

		printDirStack(exec, false);
		return 0;
	}

	static int builtin_popd(Executor& exec, const std::vector<std::string>&) {
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

	static int builtin_dirs(Executor& exec, const std::vector<std::string>& args) {
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

	static int builtin_yes(Executor&, const std::vector<std::string>& args) {
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

	static int builtin_nproc(Executor&, const std::vector<std::string>& args) {
		(void)args;   // ignore --all / --ignore=N for this minimal impl
		unsigned n = std::thread::hardware_concurrency();
		if (n == 0) n = 1;
		std::printf("%u\n", n);
		return 0;
	}

	static void tputConsoleSize(int& cols, int& lines) {
#ifdef _WIN32
		HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
		CONSOLE_SCREEN_BUFFER_INFO info{};
		if (h != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(h, &info)) {
			cols = info.srWindow.Right - info.srWindow.Left + 1;
			lines = info.srWindow.Bottom - info.srWindow.Top + 1;
			return;
		}
#endif
		cols = 80;
		lines = 24;
	}

	static const char* tputStaticEscape(const std::string& cap) {
		static const std::unordered_map<std::string, std::string> table = {
			{ "clear", "\x1b[2J\x1b[H" },
			{ "reset", "\033c"          },
			{ "bold",  "\x1b[1m"        },
			{ "dim",   "\x1b[2m"        },
			{ "smul",  "\x1b[4m"        },
			{ "rmul",  "\x1b[24m"       },
			{ "rev",   "\x1b[7m"        },
			{ "blink", "\x1b[5m"        },
			{ "sgr0",  "\x1b[0m"        },
			{ "op",    "\x1b[0m"        },
			{ "civis", "\x1b[?25l"      },
			{ "cnorm", "\x1b[?25h"      },
			{ "el",    "\x1b[K"         },
			{ "ed",    "\x1b[J"         },
			{ "home",  "\x1b[H"         },
		};
		auto it = table.find(cap);
		return (it == table.end()) ? nullptr : it->second.c_str();
	}

	static int tputParameterizedCap(const std::string& cap, const std::vector<std::string>& args) {
		if (cap == "cup" && args.size() >= 3) {
			int row = 0, col = 0;
			if (!parseInt(args[1], row) || !parseInt(args[2], col)) return 1;
			std::printf("\x1b[%d;%dH", row + 1, col + 1);
			return 0;
		}

		if ((cap == "setaf" || cap == "setf") && args.size() >= 2) {
			int n = 0;
			if (!parseInt(args[1], n)) return 1;
			if (n >= 0 && n < 8) std::printf("\x1b[%dm", 30 + n);
			else                 std::printf("\x1b[39m");
			return 0;
		}

		if ((cap == "setab" || cap == "setb") && args.size() >= 2) {
			int n = 0;
			if (!parseInt(args[1], n)) return 1;
			if (n >= 0 && n < 8) std::printf("\x1b[%dm", 40 + n);
			else                 std::printf("\x1b[49m");
			return 0;
		}

		return -1;
	}

	static int builtin_tput(Executor&, const std::vector<std::string>& args) {
		if (args.empty()) return 0;
		const std::string& cap = args[0];
		if (cap == "cols" || cap == "columns") {
			int c, l; tputConsoleSize(c, l);
			std::printf("%d\n", c);
			return 0;
		}

		if (cap == "lines") {
			int c, l; tputConsoleSize(c, l);
			std::printf("%d\n", l);
			return 0;
		}

		if (const char* esc = tputStaticEscape(cap); esc) {
			std::fputs(esc, stdout);
			return 0;
		}

		int rc = tputParameterizedCap(cap, args);
		if (rc >= 0) return rc;
		std::fprintf(stderr, "wbsh: tput: unknown capability: %s\n", cap.c_str());
		return 1;
	}

	namespace mktemp_internal {
		struct MktempOptions {
			bool make_dir = false;
			bool dry_run = false;
			bool quiet = false;
			std::string template_arg;
			std::string tmpdir_override;
		};
	}  // namespace mktemp_internal

	static int parseMktempArgs(const std::vector<std::string>& args,
	                           mktemp_internal::MktempOptions& o) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& a = args[i];
			if      (a == "-d" || a == "--directory") o.make_dir = true;
			else if (a == "-u" || a == "--dry-run")   o.dry_run = true;
			else if (a == "-q" || a == "--quiet")     o.quiet = true;
			else if (a == "-t") { /* legacy: use TMPDIR — already default */ }
			else if (a == "-p" || a == "--tmpdir") {
				if (i + 1 < args.size()) o.tmpdir_override = args[++i];
			}
			else if (a.size() > 9 && a.compare(0, 9, "--tmpdir=") == 0) {
				o.tmpdir_override = a.substr(9);
			}
			else if (!a.empty() && a[0] == '-' && a != "-") {
				std::fprintf(stderr, "wbsh: mktemp: unknown option: %s\n", a.c_str());
				return 1;
			}
			else if (o.template_arg.empty()) {
				o.template_arg = a;
			}
		}

		if (o.template_arg.empty()) o.template_arg = "tmp.XXXXXXXXXX";
		return 0;
	}

	static std::string resolveMktempBaseDir(Executor& exec,
	                                        const std::string& tmpdir_override) {
		if (!tmpdir_override.empty()) return tmpdir_override;
		if (!exec.env().get("TMPDIR").empty()) return exec.env().get("TMPDIR");
		if (!exec.env().get("TEMP").empty())   return exec.env().get("TEMP");
		if (!exec.env().get("TMP").empty())    return exec.env().get("TMP");
		return "/tmp";
	}

	static fs::path resolveMktempFullPath(Executor& exec,
	                                      const std::string& template_arg,
	                                      const std::string& base_dir) {
		if (template_arg.find('/') != std::string::npos
		    || template_arg.find('\\') != std::string::npos) {
			return fs::path(toNative(exec, template_arg));
		}

		return fs::path(toNative(exec, base_dir)) / template_arg;
	}

	static std::string randomMktempSuffix(std::size_t xcount, int attempt) {
		static const char* alpha =
			"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
		constexpr std::size_t alpha_n = 62;
		const auto t = std::chrono::steady_clock::now().time_since_epoch().count();
		unsigned long long mix = static_cast<unsigned long long>(t)
			^ (static_cast<unsigned long long>(GetCurrentProcessId()) << 32)
			^ static_cast<unsigned long long>(attempt) * 0x9E3779B97F4A7C15ULL;
		std::string suffix(xcount, 'X');
		for (std::size_t k = 0; k < xcount; ++k) {
			suffix[k] = alpha[mix % alpha_n];
			mix = mix * 6364136223846793005ULL + 1442695040888963407ULL;
		}

		return suffix;
	}

	static bool tryClaimMktempCandidate(const std::string& candidate, bool make_dir) {
		std::error_code ec;
		if (make_dir) {
			return fs::create_directory(candidate, ec) && !ec;
		}

		HANDLE h = CreateFileA(candidate.c_str(),
			GENERIC_WRITE, 0, nullptr,
			CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE) return false;
		CloseHandle(h);
		return true;
	}

	static int builtin_mktemp(Executor& exec, const std::vector<std::string>& args) {
		mktemp_internal::MktempOptions o;
		const int parse_rc = parseMktempArgs(args, o);
		if (parse_rc != 0) return parse_rc;

		const std::string base = resolveMktempBaseDir(exec, o.tmpdir_override);
		const fs::path full = resolveMktempFullPath(exec, o.template_arg, base);

		std::string s = pathToUtf8(full);
		std::size_t end = s.size();
		std::size_t start = end;
		while (start > 0 && s[start - 1] == 'X') --start;
		const std::size_t xcount = end - start;
		if (xcount < 3) {
			if (!o.quiet) std::fprintf(stderr,
				"wbsh: mktemp: too few X's in template '%s'\n",
				o.template_arg.c_str());
			return 1;
		}

		for (int attempt = 0; attempt < 200; ++attempt) {
			const std::string candidate =
				s.substr(0, start) + randomMktempSuffix(xcount, attempt);
			if (o.dry_run) {
				std::printf("%s\n", exec.pathConv().toPosix(candidate).c_str());
				return 0;
			}

			if (tryClaimMktempCandidate(candidate, o.make_dir)) {
				std::printf("%s\n", exec.pathConv().toPosix(candidate).c_str());
				return 0;
			}
		}

		if (!o.quiet) std::fprintf(stderr,
			"wbsh: mktemp: failed to create unique file from '%s'\n",
			o.template_arg.c_str());
		return 1;
	}

	static void killPrintSignalList() {
		std::printf(" 1) HUP   2) INT   3) QUIT  4) ILL   "
			"5) TRAP  6) ABRT  7) BUS   8) FPE\n"
			" 9) KILL 10) USR1 11) SEGV 12) USR2 "
			"13) PIPE 14) ALRM 15) TERM\n");
	}

	static int killTerminate(const std::vector<int>& pids, int signum) {
		int rc = 0;
#ifdef _WIN32
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

	static int builtin_kill(Executor&, const std::vector<std::string>& args) {
		int signum = 15;
		std::vector<int> pids;
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& a = args[i];
			if (a == "-l") {
				killPrintSignalList();
				return 0;
			}

			if (a == "-s" && i + 1 < args.size()) {
				parseInt(args[++i], signum);
				continue;
			}

			if (a.size() > 1 && a[0] == '-' && std::isdigit((unsigned char)a[1])) {
				parseInt(a.substr(1), signum);
				continue;
			}

			if (a == "-9" || a == "-KILL") { signum = 9;  continue; }
			if (a == "-15" || a == "-TERM") { signum = 15; continue; }
			if (a == "-2" || a == "-INT") { signum = 2;  continue; }
			if (a == "-1" || a == "-HUP") { signum = 1;  continue; }
			int pid = 0;
			if (!parseInt(a, pid)) {
				std::fprintf(stderr, "wbsh: kill: %s: arguments must be PIDs\n", a.c_str());
				return 1;
			}

			pids.push_back(pid);
		}

		if (pids.empty()) {
			perr("kill", "usage: kill [-SIG] PID...");
			return 2;
		}

		return killTerminate(pids, signum);
	}

	static void registerFileBuiltins(Executor& exec) {
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
		exec.registerBuiltin("stat",     builtin_stat);
		exec.registerBuiltin("chmod",    builtin_chmod);
		exec.registerBuiltin("ln",       builtin_ln);
		exec.registerBuiltin("cmp",      builtin_cmp);
		exec.registerBuiltin("diff",     builtin_diff);
		exec.registerBuiltin("du",       builtin_du);
		exec.registerBuiltin("df",       builtin_df);
		exec.registerBuiltin("realpath", builtin_realpath);
		exec.registerBuiltin("readlink", builtin_readlink);
		exec.registerBuiltin("basename", builtin_basename);
		exec.registerBuiltin("dirname",  builtin_dirname);
		exec.registerBuiltin("pushd",    builtin_pushd);
		exec.registerBuiltin("popd",     builtin_popd);
		exec.registerBuiltin("dirs",     builtin_dirs);
	}

	static void registerSystemBuiltins(Executor& exec) {
		exec.registerBuiltin("whoami",   builtin_whoami);
		exec.registerBuiltin("hostname", builtin_hostname);
		exec.registerBuiltin("env",      builtin_env);
		exec.registerBuiltin("sleep",    builtin_sleep);
		exec.registerBuiltin("date",     builtin_date);
		exec.registerBuiltin("seq",      builtin_seq);
		exec.registerBuiltin("uname",    builtin_uname);
		exec.registerBuiltin("id",       builtin_id);
		exec.registerBuiltin("expr",     builtin_expr);
		exec.registerBuiltin("yes",      builtin_yes);
		exec.registerBuiltin("nproc",    builtin_nproc);
		exec.registerBuiltin("tput",     builtin_tput);
		exec.registerBuiltin("mktemp",   builtin_mktemp);
		exec.registerBuiltin("kill",     builtin_kill);
	}

	void registerCoreutils(Executor& exec) {
		registerFileBuiltins(exec);
		registerTextBuiltins(exec);
		registerEncodingBuiltins(exec);
		registerArchiveBuiltins(exec);
		registerSystemBuiltins(exec);
		registerBcBuiltin(exec);
		registerHashBuiltins(exec);
		registerCurlBuiltin(exec);
		registerFzfBuiltin(exec);
	}

}  // namespace wbsh

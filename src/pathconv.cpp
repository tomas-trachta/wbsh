#include "pathconv.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <string>

namespace wbsh {

	namespace {

		std::string getTempDir() {
#ifdef _WIN32
			char buf[MAX_PATH];
			DWORD n = GetTempPathA(MAX_PATH, buf);
			if (n == 0 || n > MAX_PATH) return {};
			std::string s(buf, n);
			while (!s.empty() && (s.back() == '\\' || s.back() == '/')) s.pop_back();
			return s;
#else
			return "/tmp";
#endif
		}

		void slashesToBackslashes(std::string& s) {
			for (char& c : s) {
				if (c == '/') c = '\\';
			}
		}

		void backslashesToSlashes(std::string& s) {
			for (char& c : s) {
				if (c == '\\') c = '/';
			}
		}

	}  // namespace

	PathConv::PathConv() {
		// Drive-letter mounts: /a..z -> A:\ ..Z:\ (one entry per drive letter).
		for (char c = 'a'; c <= 'z'; ++c) {
			Mount m;
			m.posix = std::string("/") + c;
			std::string w; w += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			w += ":";
			m.win32 = std::move(w);
			m.exact = false;
			mounts_.push_back(std::move(m));
		}

		// /tmp -> %TEMP%
		std::string tmp = getTempDir();
		if (!tmp.empty()) {
			Mount m{ "/tmp", tmp, false };
			mounts_.push_back(std::move(m));
		}

		// /dev/null -> NUL (Windows special device that always exists).
		mounts_.push_back({ "/dev/null", "NUL", true });

		// Console aliases. /dev/tty resolves to CON, which Win32 supports as
		// a path that opens the console.
		mounts_.push_back({ "/dev/tty",   "CON",     true });
		mounts_.push_back({ "/dev/stdin", "CONIN$",  true });
		mounts_.push_back({ "/dev/stdout","CONOUT$", true });
		mounts_.push_back({ "/dev/stderr","CONOUT$", true });
	}

	bool PathConv::isPosixAbsolute(const std::string& p) {
		return !p.empty() && p[0] == '/';
	}

	bool PathConv::isWin32Absolute(const std::string& p) {
		if (p.size() >= 2 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':') return true;
		if (p.size() >= 2 && (p[0] == '\\' || p[0] == '/') && (p[1] == '\\' || p[1] == '/')) return true;
		return false;
	}

	bool PathConv::looksLikeWin32(const std::string& s) {
		if (isWin32Absolute(s)) return true;
		for (char c : s) if (c == '\\') return true;
		return false;
	}

	bool PathConv::applyMount(const std::string& p, std::string& out) const {
		// Find the longest matching mount.
		const Mount* best = nullptr;
		std::size_t best_len = 0;
		for (const auto& m : mounts_) {
			if (m.exact) {
				if (p == m.posix) { best = &m; best_len = m.posix.size(); break; }
				continue;
			}
			if (p.size() < m.posix.size()) continue;
			if (p.compare(0, m.posix.size(), m.posix) != 0) continue;
			if (p.size() > m.posix.size() && p[m.posix.size()] != '/') continue;
			if (m.posix.size() > best_len) {
				best = &m;
				best_len = m.posix.size();
			}
		}
		if (!best) return false;
		if (best->exact) {
			out = best->win32;
			return true;
		}
		// Prefix match. Splice mount.win32 + (rest of p with `/` -> `\`).
		std::string tail = p.substr(best->posix.size());
		// Drop leading separator if present (we'll re-add via mount).
		if (!tail.empty() && tail.front() == '/') tail.erase(0, 1);
		std::string w = best->win32;
		if (!tail.empty()) {
			if (!w.empty() && w.back() != '\\' && w.back() != '/') w.push_back('\\');
			w += tail;
		} else {
			// "/c" alone -> "C:\"
			if (!w.empty() && w.back() == ':') w.push_back('\\');
		}
		slashesToBackslashes(w);
		out = std::move(w);
		return true;
	}

	std::string PathConv::toWin32(const std::string& p) const {
		if (p.empty()) return p;
		// Already a Win32 path? Pass through (just normalise / -> \).
		if (looksLikeWin32(p)) {
			std::string s = p;
			slashesToBackslashes(s);
			return s;
		}
		// Special-case bare `/` — root of current drive.
		if (p == "/") {
#ifdef _WIN32
			char buf[MAX_PATH];
			DWORD n = GetCurrentDirectoryA(MAX_PATH, buf);
			if (n >= 3 && buf[1] == ':') {
				return std::string(1, buf[0]) + ":\\";
			}
			return "C:\\";
#else
			return "/";
#endif
		}
		if (isPosixAbsolute(p)) {
			std::string out;
			if (applyMount(p, out)) return out;
			// Fallback: treat unknown /foo as relative-to-current-drive root.
			std::string s = p.substr(1);
			slashesToBackslashes(s);
#ifdef _WIN32
			char buf[MAX_PATH];
			DWORD n = GetCurrentDirectoryA(MAX_PATH, buf);
			if (n >= 3 && buf[1] == ':') {
				return std::string(1, buf[0]) + ":\\" + s;
			}
#endif
			return "\\" + s;
		}
		// Relative path — leave as-is (forward slashes are accepted by Win32 APIs).
		return p;
	}

	std::string PathConv::toPosix(const std::string& p) const {
		if (p.empty()) return p;
		std::string s = p;
		// Drive-letter prefix: X:\... or X:/... -> /x/...
		if (s.size() >= 2 && std::isalpha(static_cast<unsigned char>(s[0])) && s[1] == ':') {
			char drive = static_cast<char>(std::tolower(static_cast<unsigned char>(s[0])));
			std::string rest = s.substr(2);
			backslashesToSlashes(rest);
			if (rest.empty() || rest[0] != '/') rest.insert(rest.begin(), '/');
			return std::string("/") + drive + rest;
		}
		// UNC: \\host\share\... -> //host/share/...
		if (s.size() >= 2 && (s[0] == '\\' || s[0] == '/') && (s[1] == '\\' || s[1] == '/')) {
			backslashesToSlashes(s);
			return s;
		}
		// Otherwise convert backslashes only.
		backslashesToSlashes(s);
		return s;
	}

	std::string PathConv::pathListWin32ToPosix(const std::string& list) const {
		if (list.empty()) return list;
		std::string out;
		std::string cur;
		auto flush = [&]() {
			if (!cur.empty()) {
				if (!out.empty()) out.push_back(':');
				out += toPosix(cur);
				cur.clear();
			}
		};
		for (char c : list) {
			if (c == ';') flush();
			else cur.push_back(c);
		}
		flush();
		return out;
	}

	std::string PathConv::pathListPosixToWin32(const std::string& list) const {
		if (list.empty()) return list;
		std::string out;
		std::string cur;
		auto flush = [&]() {
			if (!cur.empty()) {
				if (!out.empty()) out.push_back(';');
				out += toWin32(cur);
				cur.clear();
			}
		};
		// Splitter that treats ':' as separator unless it's a drive-letter colon
		// (i.e., second char of a token after a single alpha char).
		std::size_t i = 0;
		while (i < list.size()) {
			char c = list[i];
			if (c == ':') {
				if (cur.size() == 1 && std::isalpha(static_cast<unsigned char>(cur[0]))) {
					// Drive-letter colon — part of the entry.
					cur.push_back(c);
				} else {
					flush();
				}
				++i;
				continue;
			}
			cur.push_back(c);
			++i;
		}
		flush();
		return out;
	}

	bool PathConv::argLooksTranslatable(const std::string& arg) const {
		if (arg.empty()) return false;
		if (arg == "/") return false;
		if (arg.find("://") != std::string::npos) return false;
		// `--opt=path` and `-x=path` get separately translated by translateArg.
		if (arg[0] == '-') return false;
		if (arg[0] != '/') return false;
		// Single-letter `/x` is almost always a Win32-style switch (`cmd /c`,
		// `cmd /k`, `where /q`, etc.). Require a deeper path before we
		// translate, OR an exact match against a known mount.
		if (arg.size() < 3) return false;
		// `/x/...` shape is a clear translatable path.
		if (arg.size() >= 4 && arg[2] == '/') return true;
		// `/tmp`, `/dev/null`, etc. — exact prefix matches against the
		// mount table count as translatable.
		std::string out;
		return applyMount(arg, out);
	}

	std::string PathConv::translateArg(const std::string& arg) const {
		// Plain absolute path?
		if (argLooksTranslatable(arg)) return toWin32(arg);
		// `--opt=PATH` or `-X=PATH` form: translate value.
		if (!arg.empty() && arg[0] == '-') {
			std::size_t eq = arg.find('=');
			if (eq != std::string::npos && eq + 1 < arg.size()) {
				std::string val = arg.substr(eq + 1);
				if (argLooksTranslatable(val)) {
					return arg.substr(0, eq + 1) + toWin32(val);
				}
			}
		}
		return arg;
	}

}  // namespace wbsh

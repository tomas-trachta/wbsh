#include "pathconv.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif /* _WIN32 */

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

namespace wbsh {

	std::wstring utf8ToWide(const std::string& s) {
#ifdef _WIN32
		if (s.empty()) return {};
		int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
									   nullptr, 0);
		if (n <= 0) return {};
		std::wstring out(static_cast<std::size_t>(n), L'\0');
		::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
							  out.data(), n);
		return out;
#else
		return std::wstring(s.begin(), s.end());
#endif
	}

	std::string wideToUtf8(const std::wstring& w) {
#ifdef _WIN32
		if (w.empty()) return {};
		int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
									   nullptr, 0, nullptr, nullptr);
		if (n <= 0) return {};
		std::string out(static_cast<std::size_t>(n), '\0');
		::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
							  out.data(), n, nullptr, nullptr);
		return out;
#else
		return std::string(w.begin(), w.end());
#endif
	}

	std::filesystem::path utf8ToPath(const std::string& s) {
#ifdef _WIN32
		return std::filesystem::path(utf8ToWide(s));
#else
		return std::filesystem::path(s);
#endif
	}

	std::string pathToUtf8(const std::filesystem::path& p) {
#ifdef _WIN32
		return wideToUtf8(p.wstring());
#else
		return p.string();
#endif
	}

	std::FILE* openUtf8(const std::string& utf8_path, const char* mode) {
#ifdef _WIN32
		std::wstring wpath = utf8ToWide(utf8_path);
		std::wstring wmode;
		for (const char* m = mode; *m; ++m) {
			wmode.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*m)));
		}

		return ::_wfopen(wpath.c_str(), wmode.c_str());
#else
		return std::fopen(utf8_path.c_str(), mode);
#endif
	}

	static std::string getTempDir() {
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

	static void slashesToBackslashes(std::string& s) {
		for (char& c : s) {
			if (c == '/') c = '\\';
		}
	}

	static void backslashesToSlashes(std::string& s) {
		for (char& c : s) {
			if (c == '\\') c = '/';
		}
	}

	PathConv::PathConv() {
		for (char c = 'a'; c <= 'z'; ++c) {
			Mount m;
			m.posix = std::string("/") + c;
			std::string w; w += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			w += ":";
			m.win32 = std::move(w);
			m.exact = false;
			mounts_.push_back(std::move(m));
		}

		std::string tmp = getTempDir();
		if (!tmp.empty()) {
			Mount m{ "/tmp", tmp, false };
			mounts_.push_back(std::move(m));
		}

		mounts_.push_back({ "/dev/null", "NUL", true });

		mounts_.push_back({ "/dev/tty",   "CON",     true });
		mounts_.push_back({ "/dev/stdin", "CONIN$",  true });
		mounts_.push_back({ "/dev/stdout","CONOUT$", true });
		mounts_.push_back({ "/dev/stderr","CONOUT$", true });
	}

	bool PathConv::isPosixAbsolute(const std::string& p) {
		return !p.empty() && p[0] == '/';
	}

	bool PathConv::isWin32Absolute(const std::string& p) {
		if (p.size() >= 2
		    && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':') {
			return true;
		}

		if (p.size() >= 2
		    && (p[0] == '\\' || p[0] == '/') && (p[1] == '\\' || p[1] == '/')) {
			return true;
		}

		return false;
	}

	bool PathConv::looksLikeWin32(const std::string& s) {
		if (isWin32Absolute(s)) return true;
		for (char c : s) if (c == '\\') return true;
		return false;
	}

	bool PathConv::applyMount(const std::string& p, std::string& out) const {
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

		std::string tail = p.substr(best->posix.size());
		if (!tail.empty() && tail.front() == '/') tail.erase(0, 1);
		std::string w = best->win32;
		if (!tail.empty()) {
			if (!w.empty() && w.back() != '\\' && w.back() != '/') w.push_back('\\');
			w += tail;
		} else {
			if (!w.empty() && w.back() == ':') w.push_back('\\');
		}

		slashesToBackslashes(w);
		out = std::move(w);
		return true;
	}

	std::string PathConv::toWin32(const std::string& p) const {
		if (p.empty()) return p;
		if (looksLikeWin32(p)) {
			std::string s = p;
			slashesToBackslashes(s);
			return s;
		}

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

		return p;
	}

	std::string PathConv::toPosix(const std::string& p) const {
		if (p.empty()) return p;
		std::string s = p;
		if (s.size() >= 2 && std::isalpha(static_cast<unsigned char>(s[0])) && s[1] == ':') {
			char drive = static_cast<char>(std::tolower(static_cast<unsigned char>(s[0])));
			std::string rest = s.substr(2);
			backslashesToSlashes(rest);
			if (rest.empty() || rest[0] != '/') rest.insert(rest.begin(), '/');
			return std::string("/") + drive + rest;
		}

		if (s.size() >= 2 && (s[0] == '\\' || s[0] == '/') && (s[1] == '\\' || s[1] == '/')) {
			backslashesToSlashes(s);
			return s;
		}

		backslashesToSlashes(s);
		return s;
	}

	std::string PathConv::toWin32Short(const std::string& p) const {
		std::string longw = toWin32(p);
#ifdef _WIN32
		if (longw.empty()) return longw;
		bool has_non_ascii = false;
		for (unsigned char c : longw) {
			if (c >= 0x80) { has_non_ascii = true; break; }
		}

		if (!has_non_ascii) return longw;

		int wn = ::MultiByteToWideChar(CP_UTF8, 0, longw.data(), (int)longw.size(),
									   nullptr, 0);
		if (wn <= 0) return longw;
		std::wstring wlong(static_cast<std::size_t>(wn), L'\0');
		::MultiByteToWideChar(CP_UTF8, 0, longw.data(), (int)longw.size(),
							  wlong.data(), wn);

		DWORD need = ::GetShortPathNameW(wlong.c_str(), nullptr, 0);
		if (need == 0) return longw;
		std::wstring wshort(need, L'\0');
		DWORD got = ::GetShortPathNameW(wlong.c_str(), wshort.data(), need);
		if (got == 0 || got >= need) return longw;
		wshort.resize(got);

		int n = ::WideCharToMultiByte(CP_UTF8, 0, wshort.data(), (int)wshort.size(),
									  nullptr, 0, nullptr, nullptr);
		if (n <= 0) return longw;
		std::string out(static_cast<std::size_t>(n), '\0');
		::WideCharToMultiByte(CP_UTF8, 0, wshort.data(), (int)wshort.size(),
							  out.data(), n, nullptr, nullptr);

		// If 8.3 generation is off on this volume, GetShortPathName returns
		// the long form unchanged — still non-ASCII, still mangleable.
		// Fall back to the long form (caller is no worse off than before).
		for (unsigned char c : out) {
			if (c >= 0x80) return longw;
		}

		return out;
#else
		return longw;
#endif /* _WIN32 */
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
		std::size_t i = 0;
		while (i < list.size()) {
			char c = list[i];
			if (c == ':') {
				if (cur.size() == 1 && std::isalpha(static_cast<unsigned char>(cur[0]))) {
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
		if (arg[0] == '-') return false;
		if (arg[0] != '/') return false;
		// Single-letter `/x` is almost always a Win32-style switch (`cmd /c`,
		// `cmd /k`, `where /q`, etc.). Require a deeper path before we
		// translate, OR an exact match against a known mount.
		if (arg.size() < 3) return false;
		if (arg.size() >= 4 && arg[2] == '/') return true;
		// /dev/std{in,out,err} and /dev/tty: pass through verbatim. Mapping
		// to CONIN$/CONOUT$/CON breaks cross-process uses like
		// `docker exec ... -i /dev/stdin`, where the path is meant to be
		// interpreted by the callee (here: sqlcmd inside a Linux container).
		if (arg == "/dev/stdin" || arg == "/dev/stdout"
		    || arg == "/dev/stderr" || arg == "/dev/tty") return false;
		std::string out;
		return applyMount(arg, out);
	}

	std::string PathConv::translateArg(const std::string& arg) const {
		if (argLooksTranslatable(arg)) return toWin32(arg);
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

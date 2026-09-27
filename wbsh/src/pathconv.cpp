/**
 * @file pathconv.cpp
 * @brief UTF-8 ↔ native path conversion and POSIX ↔ Win32 path translation.
 */

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

	static const char kWin32ListSeparator = ';';
	static const char kPosixListSeparator = ':';

	std::wstring utf8ToWide(const std::string& s) {
#ifdef _WIN32
		if (s.empty()) return {};

		const int size = static_cast<int>(s.size());
		const int length = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), size, nullptr, 0);
		if (length <= 0) return {};

		std::wstring out(static_cast<std::size_t>(length), L'\0');
		::MultiByteToWideChar(CP_UTF8, 0, s.data(), size, out.data(), length);
		return out;
#else
		return std::wstring(s.begin(), s.end());
#endif /* _WIN32 */
	}

	std::string wideToUtf8(const std::wstring& w) {
#ifdef _WIN32
		if (w.empty()) return {};

		const int size = static_cast<int>(w.size());
		const int length = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), size,
			nullptr, 0, nullptr, nullptr);
		if (length <= 0) return {};

		std::string out(static_cast<std::size_t>(length), '\0');
		::WideCharToMultiByte(CP_UTF8, 0, w.data(), size, out.data(), length, nullptr, nullptr);
		return out;
#else
		return std::string(w.begin(), w.end());
#endif /* _WIN32 */
	}

	std::filesystem::path utf8ToPath(const std::string& s) {
#ifdef _WIN32
		return std::filesystem::path(utf8ToWide(s));
#else
		return std::filesystem::path(s);
#endif /* _WIN32 */
	}

	std::string pathToUtf8(const std::filesystem::path& p) {
#ifdef _WIN32
		return wideToUtf8(p.wstring());
#else
		return p.string();
#endif /* _WIN32 */
	}

#ifdef _WIN32
	static bool statFromAttributes(const std::string& win32_path, struct stat& info) {
		const DWORD attributes = ::GetFileAttributesW(utf8ToWide(win32_path).c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES) return false;

		const bool is_dir = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		const bool read_only = (attributes & FILE_ATTRIBUTE_READONLY) != 0;
		const unsigned short kind = is_dir ? S_IFDIR : S_IFREG;
		const unsigned short perms = read_only ? 0555 : 0777;
		info = {};
		info.st_mode = static_cast<unsigned short>(kind | perms);
		info.st_nlink = 1;
		return true;
	}
#endif /* _WIN32 */

	bool statPath(const std::string& win32_path, struct stat& info) {
		if (::stat(win32_path.c_str(), &info) == 0) return true;
#ifdef _WIN32
		return statFromAttributes(win32_path, info);
#else
		return false;
#endif /* _WIN32 */
	}

	std::FILE* openUtf8(const std::string& utf8_path, const char* mode) {
#ifdef _WIN32
		const std::wstring wide_path = utf8ToWide(utf8_path);
		std::wstring wide_mode;
		for (const char* m = mode; *m != '\0'; ++m) {
			wide_mode.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*m)));
		}

		return ::_wfopen(wide_path.c_str(), wide_mode.c_str());
#else
		return std::fopen(utf8_path.c_str(), mode);
#endif /* _WIN32 */
	}

	static std::string getTempDir() {
#ifdef _WIN32
		char buffer[MAX_PATH];
		const DWORD length = ::GetTempPathA(MAX_PATH, buffer);
		if (length == 0 || length > MAX_PATH) return {};

		std::string dir(buffer, length);
		while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
		return dir;
#else
		return "/tmp";
#endif /* _WIN32 */
	}

#ifdef _WIN32
	// "X:\" of the current drive, or empty when it cannot be told.
	static std::string currentDriveRoot() {
		char buffer[MAX_PATH];
		const DWORD length = ::GetCurrentDirectoryA(MAX_PATH, buffer);
		if (length >= 3 && buffer[1] == ':') return std::string(1, buffer[0]) + ":\\";
		return {};
	}
#endif /* _WIN32 */

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

	static bool isAscii(const std::string& s) {
		for (unsigned char c : s) {
			if (c >= 0x80) return false;
		}

		return true;
	}

	static bool isDriveLetterPath(const std::string& p) {
		return p.size() >= 2 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':';
	}

	static bool isUncPath(const std::string& p) {
		return p.size() >= 2
			&& (p[0] == '\\' || p[0] == '/')
			&& (p[1] == '\\' || p[1] == '/');
	}

	PathConv::PathConv() {
		for (char letter = 'a'; letter <= 'z'; ++letter) {
			const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(letter)));
			Mount mount;
			mount.posix = std::string("/") + letter;
			mount.win32 = std::string(1, upper) + ":";
			mount.exact = false;
			mounts_.push_back(std::move(mount));
		}

		const std::string temp_dir = getTempDir();
		if (!temp_dir.empty()) mounts_.push_back({ "/tmp", temp_dir, false });

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
		return isDriveLetterPath(p) || isUncPath(p);
	}

	bool PathConv::looksLikeWin32(const std::string& s) {
		if (isWin32Absolute(s)) return true;
		return s.find('\\') != std::string::npos;
	}

	const PathConv::Mount* PathConv::findMount(const std::string& p) const {
		const Mount* best = nullptr;
		std::size_t best_length = 0;

		for (const auto& mount : mounts_) {
			if (mount.exact) {
				if (p == mount.posix) return &mount;
				continue;
			}

			const std::size_t prefix_length = mount.posix.size();
			if (p.size() < prefix_length) continue;
			if (p.compare(0, prefix_length, mount.posix) != 0) continue;
			if (p.size() > prefix_length && p[prefix_length] != '/') continue;

			if (prefix_length > best_length) {
				best = &mount;
				best_length = prefix_length;
			}
		}

		return best;
	}

	static std::string joinMountTail(const std::string& win32_root, std::string tail) {
		if (!tail.empty() && tail.front() == '/') tail.erase(0, 1);

		std::string out = win32_root;
		if (tail.empty()) {
			if (!out.empty() && out.back() == ':') out.push_back('\\');
		} else {
			if (!out.empty() && out.back() != '\\' && out.back() != '/') out.push_back('\\');
			out += tail;
		}

		slashesToBackslashes(out);
		return out;
	}

	bool PathConv::applyMount(const std::string& p, std::string& out) const {
		const Mount* best = findMount(p);
		if (best == nullptr) return false;

		if (best->exact) {
			out = best->win32;
			return true;
		}

		out = joinMountTail(best->win32, p.substr(best->posix.size()));
		return true;
	}

	std::string PathConv::toWin32(const std::string& p) const {
		if (p.empty()) return p;

		if (looksLikeWin32(p)) {
			std::string native = p;
			slashesToBackslashes(native);
			return native;
		}

		if (p == "/") {
#ifdef _WIN32
			const std::string root = currentDriveRoot();
			if (!root.empty()) return root;
			return "C:\\";
#else
			return "/";
#endif /* _WIN32 */
		}

		if (!isPosixAbsolute(p)) return p;

		std::string mounted;
		if (applyMount(p, mounted)) return mounted;

		std::string rest = p.substr(1);
		slashesToBackslashes(rest);
#ifdef _WIN32
		const std::string root = currentDriveRoot();
		if (!root.empty()) return root + rest;
#endif /* _WIN32 */
		return "\\" + rest;
	}

	std::string PathConv::toPosix(const std::string& p) const {
		if (p.empty()) return p;

		if (isDriveLetterPath(p)) {
			const char drive = static_cast<char>(std::tolower(static_cast<unsigned char>(p[0])));
			std::string rest = p.substr(2);
			backslashesToSlashes(rest);
			if (rest.empty() || rest[0] != '/') rest.insert(rest.begin(), '/');
			return std::string("/") + drive + rest;
		}

		std::string posix = p;
		backslashesToSlashes(posix);
		return posix;
	}

#ifdef _WIN32
	static bool toWideStrict(const std::string& utf8, std::wstring& out) {
		const int size = static_cast<int>(utf8.size());
		const int length = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), size, nullptr, 0);
		if (length <= 0) return false;

		out.assign(static_cast<std::size_t>(length), L'\0');
		::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), size, out.data(), length);
		return true;
	}

	static bool toUtf8Strict(const std::wstring& wide, std::string& out) {
		const int size = static_cast<int>(wide.size());
		const int length = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), size,
			nullptr, 0, nullptr, nullptr);
		if (length <= 0) return false;

		out.assign(static_cast<std::size_t>(length), '\0');
		::WideCharToMultiByte(CP_UTF8, 0, wide.data(), size, out.data(), length, nullptr, nullptr);
		return true;
	}

	static bool queryShortPathName(const std::wstring& long_form, std::wstring& out) {
		const DWORD needed = ::GetShortPathNameW(long_form.c_str(), nullptr, 0);
		if (needed == 0) return false;

		out.assign(needed, L'\0');
		const DWORD got = ::GetShortPathNameW(long_form.c_str(), out.data(), needed);
		if (got == 0 || got >= needed) return false;

		out.resize(got);
		return true;
	}
#endif /* _WIN32 */

	std::string PathConv::toWin32Short(const std::string& p) const {
		const std::string long_form = toWin32(p);
#ifdef _WIN32
		if (long_form.empty() || isAscii(long_form)) return long_form;

		std::wstring wide_long;
		if (!toWideStrict(long_form, wide_long)) return long_form;

		std::wstring wide_short;
		if (!queryShortPathName(wide_long, wide_short)) return long_form;

		std::string short_form;
		if (!toUtf8Strict(wide_short, short_form)) return long_form;

		// If 8.3 generation is off on this volume, GetShortPathName returns
		// the long form unchanged — still non-ASCII, still mangleable.
		// Fall back to the long form (caller is no worse off than before).
		if (!isAscii(short_form)) return long_form;
		return short_form;
#else
		return long_form;
#endif /* _WIN32 */
	}

	static void appendListEntry(std::string& out, char separator, const std::string& entry) {
		if (!out.empty()) out.push_back(separator);
		out += entry;
	}

	static std::vector<std::string> splitWin32PathList(const std::string& list) {
		std::vector<std::string> entries;
		std::string current;
		for (char c : list) {
			if (c != kWin32ListSeparator) {
				current.push_back(c);
				continue;
			}

			entries.push_back(current);
			current.clear();
		}

		entries.push_back(current);
		return entries;
	}

	// A colon right after a lone letter is a drive separator, not a
	// list separator, so `C:\x:/usr/bin` splits into two entries.
	static std::vector<std::string> splitPosixPathList(const std::string& list) {
		std::vector<std::string> entries;
		std::string current;
		for (char c : list) {
			if (c != kPosixListSeparator) {
				current.push_back(c);
				continue;
			}

			const bool drive_colon = current.size() == 1
				&& std::isalpha(static_cast<unsigned char>(current[0]));
			if (drive_colon) {
				current.push_back(c);
				continue;
			}

			entries.push_back(current);
			current.clear();
		}

		entries.push_back(current);
		return entries;
	}

	std::string PathConv::pathListWin32ToPosix(const std::string& list) const {
		if (list.empty()) return list;

		std::string out;
		for (const std::string& entry : splitWin32PathList(list)) {
			if (entry.empty()) continue;
			appendListEntry(out, kPosixListSeparator, toPosix(entry));
		}

		return out;
	}

	std::string PathConv::pathListPosixToWin32(const std::string& list) const {
		if (list.empty()) return list;

		std::string out;
		for (const std::string& entry : splitPosixPathList(list)) {
			if (entry.empty()) continue;
			appendListEntry(out, kWin32ListSeparator, toWin32(entry));
		}

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

		std::string mounted;
		return applyMount(arg, mounted);
	}

	std::string PathConv::translateArg(const std::string& arg) const {
		if (argLooksTranslatable(arg)) return toWin32(arg);
		if (arg.empty() || arg[0] != '-') return arg;

		const std::size_t equals = arg.find('=');
		if (equals == std::string::npos || equals + 1 >= arg.size()) return arg;

		const std::string value = arg.substr(equals + 1);
		if (!argLooksTranslatable(value)) return arg;
		return arg.substr(0, equals + 1) + toWin32(value);
	}

}  // namespace wbsh

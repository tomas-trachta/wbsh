/**
 * @file setup.cpp
 * @brief Implementation of the startup environment-seeding helpers.
 */

#include "setup.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif /* _WIN32 */

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "pathconv.h"

namespace wbsh {

#ifdef _WIN32
	// Read a REG_SZ / REG_EXPAND_SZ value as UTF-8, expanding env refs.
	// Returns empty if the key/value isn't present or the type is wrong.
	static std::string readRegistryString(HKEY root, const wchar_t* subkey,
	                                      const wchar_t* value) {
		HKEY hk;
		if (RegOpenKeyExW(root, subkey, 0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS)
			return {};
		DWORD type = 0, size = 0;
		LONG r = RegQueryValueExW(hk, value, nullptr, &type, nullptr, &size);
		if (r != ERROR_SUCCESS
		    || (type != REG_SZ && type != REG_EXPAND_SZ)
		    || size == 0) {
			RegCloseKey(hk);
			return {};
		}
		std::wstring buf(size / sizeof(wchar_t) + 1, L'\0');
		r = RegQueryValueExW(hk, value, nullptr, &type,
			reinterpret_cast<LPBYTE>(buf.data()), &size);
		RegCloseKey(hk);
		if (r != ERROR_SUCCESS) return {};
		while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
		if (type == REG_EXPAND_SZ) {
			DWORD n = ExpandEnvironmentStringsW(buf.c_str(), nullptr, 0);
			if (n) {
				std::wstring exp(n, L'\0');
				ExpandEnvironmentStringsW(buf.c_str(), exp.data(), n);
				while (!exp.empty() && exp.back() == L'\0') exp.pop_back();
				buf = std::move(exp);
			}
		}
		int u = WideCharToMultiByte(CP_UTF8, 0, buf.data(), (int)buf.size(),
			nullptr, 0, nullptr, nullptr);
		std::string out(u, '\0');
		WideCharToMultiByte(CP_UTF8, 0, buf.data(), (int)buf.size(),
			out.data(), u, nullptr, nullptr);
		return out;
	}

	// Returns the entries of the user + system PATH from the registry, in
	// Win32 form. Used to repair PATH when wbsh is launched from a parent
	// (Explorer started at login) whose own environment predates a tool
	// install. Both cmd.exe and PowerShell get a fresh, registry-merged
	// PATH from Windows Terminal / login; wbsh, launched from a stale
	// Explorer process, otherwise wouldn't.
	static std::vector<std::string> registryPathDirs() {
		std::vector<std::string> dirs;
		auto split = [&](const std::string& list) {
			std::string cur;
			for (char c : list) {
				if (c == ';') {
					if (!cur.empty()) { dirs.push_back(cur); cur.clear(); }
				} else {
					cur.push_back(c);
				}
			}
			if (!cur.empty()) dirs.push_back(cur);
		};
		// User PATH first so user-installed tools win over system-shadowed ones.
		split(readRegistryString(HKEY_CURRENT_USER, L"Environment", L"Path"));
		split(readRegistryString(HKEY_LOCAL_MACHINE,
			L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment",
			L"Path"));
		return dirs;
	}
#endif /* _WIN32 */

	// Probe well-known install locations for git.exe and return the
	// directories that contain it. wbsh ships its own coreutils, but git
	// itself is an external the user installs separately and absolutely
	// expects to find on PATH. When wbsh is launched from Explorer the
	// inherited PATH may miss it; this fills the gap.
	static std::vector<std::string> findGitDirs() {
		namespace fs = std::filesystem;
		std::vector<std::string> candidates;
		auto add = [&](const char* var, const char* suffix) {
			if (const char* v = std::getenv(var)) {
				candidates.push_back(std::string(v) + suffix);
			}
		};
		add("ProgramFiles",      "\\Git\\cmd");
		add("ProgramFiles(x86)", "\\Git\\cmd");
		add("ProgramW6432",      "\\Git\\cmd");
		add("LOCALAPPDATA",      "\\Programs\\Git\\cmd");
		if (const char* up = std::getenv("USERPROFILE")) {
			candidates.push_back(std::string(up)
				+ "\\scoop\\apps\\git\\current\\cmd");
			candidates.push_back(std::string(up)
				+ "\\scoop\\shims");
		}
		if (const char* pd = std::getenv("ProgramData")) {
			candidates.push_back(std::string(pd)
				+ "\\chocolatey\\bin");
		}

		std::vector<std::string> hits;
		for (const auto& d : candidates) {
			std::error_code ec;
			fs::path exe = fs::path(d) / "git.exe";
			if (fs::exists(exe, ec) && !fs::is_directory(exe, ec)) {
				hits.push_back(d);
			}
		}
		return hits;
	}

	// Locate Git for Windows' bundled MSYS tree at `{root}\usr\bin` (sh.exe,
	// env.exe, awk, sed, ...). Git launches its own sh via a hardcoded path,
	// but scripts that sh runs as the actual editor -- VS Code's `code`
	// wrapper invoked by `git pull` for the merge message, hooks, etc. --
	// have a `#!/usr/bin/env sh` shebang that re-resolves sh through PATH.
	// Without `\Git\usr\bin` on PATH, that fails with
	// `/usr/bin/env: 'sh': No such file or directory`. findGitDirs only
	// adds `\Git\cmd` (where git.exe lives), so this is a separate probe.
	static std::vector<std::string> findGitUnixDirs() {
		namespace fs = std::filesystem;
		std::vector<std::string> candidates;
		auto add = [&](const char* var, const char* suffix) {
			if (const char* v = std::getenv(var)) {
				candidates.push_back(std::string(v) + suffix);
			}
		};
		add("ProgramFiles",      "\\Git\\usr\\bin");
		add("ProgramFiles(x86)", "\\Git\\usr\\bin");
		add("ProgramW6432",      "\\Git\\usr\\bin");
		add("LOCALAPPDATA",      "\\Programs\\Git\\usr\\bin");
		if (const char* up = std::getenv("USERPROFILE")) {
			candidates.push_back(std::string(up)
				+ "\\scoop\\apps\\git\\current\\usr\\bin");
		}

		std::vector<std::string> hits;
		for (const auto& d : candidates) {
			std::error_code ec;
			fs::path exe = fs::path(d) / "sh.exe";
			if (fs::exists(exe, ec) && !fs::is_directory(exe, ec)) {
				hits.push_back(d);
			}
		}
		return hits;
	}

	// Same idea as findGitDirs, for Docker. Docker Desktop installs its CLI
	// under `Program Files\Docker\Docker\resources\bin`, which isn't on the
	// PATH a non-shell parent (Explorer, VS debugger) inherits.
	static std::vector<std::string> findDockerDirs() {
		namespace fs = std::filesystem;
		std::vector<std::string> candidates;
		auto add = [&](const char* var, const char* suffix) {
			if (const char* v = std::getenv(var)) {
				candidates.push_back(std::string(v) + suffix);
			}
		};
		add("ProgramFiles",      "\\Docker\\Docker\\resources\\bin");
		add("ProgramFiles(x86)", "\\Docker\\Docker\\resources\\bin");
		add("ProgramW6432",      "\\Docker\\Docker\\resources\\bin");
		if (const char* up = std::getenv("USERPROFILE")) {
			candidates.push_back(std::string(up)
				+ "\\scoop\\apps\\docker\\current");
			candidates.push_back(std::string(up)
				+ "\\scoop\\shims");
		}
		if (const char* pd = std::getenv("ProgramData")) {
			candidates.push_back(std::string(pd)
				+ "\\chocolatey\\bin");
		}

		std::vector<std::string> hits;
		for (const auto& d : candidates) {
			std::error_code ec;
			fs::path exe = fs::path(d) / "docker.exe";
			if (fs::exists(exe, ec) && !fs::is_directory(exe, ec)) {
				hits.push_back(d);
			}
		}
		return hits;
	}

	static void prependDirsToPath(Environment& env, const PathConv& pc,
	                              const std::vector<std::string>& dirs) {
		if (dirs.empty()) return;
		std::string path = env.get("PATH");
		std::string prepend;
		for (const auto& d : dirs) {
			std::string posix = pc.toPosix(d);
			// Crude dedup: skip if PATH already contains this entry.
			if (!path.empty() && path.find(posix) != std::string::npos) continue;
			if (!prepend.empty()) prepend.push_back(':');
			prepend += posix;
		}
		if (prepend.empty()) return;
		env.set("PATH", path.empty() ? prepend : (prepend + ":" + path));
	}

	void prepareEnv(Environment& env) {
		env.loadFromProcessEnv();
		PathConv pc;
		std::string p = env.get("PATH");
		if (!p.empty()) env.set("PATH", pc.pathListWin32ToPosix(p));
#ifdef _WIN32
		// Merge user + system PATH from the registry. Without this, wbsh
		// launched from a long-running Explorer can't find tools installed
		// after that Explorer started -- e.g. VS Code's `code.cmd`.
		prependDirsToPath(env, pc, registryPathDirs());
#endif /* _WIN32 */
		// Make `git` and `docker` discoverable when launched standalone (from
		// Explorer or via a non-shell parent). Doesn't affect users whose
		// system PATH already includes them — the dedup check is a no-op there.
		// `findGitUnixDirs` adds `\Git\usr\bin` so script-as-editor shebangs
		// like `#!/usr/bin/env sh` (used by VS Code's `code` wrapper, picked up
		// by git as merge-message editor) can resolve sh through PATH.
		prependDirsToPath(env, pc, findGitDirs());
		prependDirsToPath(env, pc, findGitUnixDirs());
		prependDirsToPath(env, pc, findDockerDirs());
		std::string home = env.get("HOME");
		if (home.empty()) home = env.get("USERPROFILE");
		if (!home.empty()) {
			env.set("HOME", pc.toPosix(home));
			env.exportVar("HOME");
		}
		std::error_code ec;
		auto cwd = std::filesystem::current_path(ec);
		if (!ec) {
			env.set("PWD", pc.toPosix(pathToUtf8(cwd)));
			env.exportVar("PWD");
		}
	}

	void absorbInheritedState(Environment& env, Executor& exec) {
		// Drop any local-name marker from the parent (we already loaded all
		// values; we just need to fix isExported() before any new spawn).
		std::string locals_marker = env.get("WBSH_LOCAL_NAMES");
		if (!locals_marker.empty()) {
			env.unset("WBSH_LOCAL_NAMES");
			std::size_t i = 0;
			while (i < locals_marker.size()) {
				while (i < locals_marker.size() && locals_marker[i] == ' ') ++i;
				std::size_t start = i;
				while (i < locals_marker.size() && locals_marker[i] != ' ') ++i;
				if (i > start) env.unexportVar(locals_marker.substr(start, i - start));
			}
		}
		std::string inherited_fns = env.get("WBSH_FUNCTIONS");
		if (!inherited_fns.empty()) {
			env.unset("WBSH_FUNCTIONS");
			exec.executeText(inherited_fns, "<inherited functions>");
		}
		std::string inherited_aliases = env.get("WBSH_ALIASES");
		if (!inherited_aliases.empty()) {
			env.unset("WBSH_ALIASES");
			exec.executeText(inherited_aliases, "<inherited aliases>");
		}
		std::string inherited_arrays = env.get("WBSH_ARRAYS");
		if (!inherited_arrays.empty()) {
			env.unset("WBSH_ARRAYS");
			exec.executeText(inherited_arrays, "<inherited arrays>");
		}
	}

}  // namespace wbsh

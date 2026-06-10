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
		split(readRegistryString(HKEY_CURRENT_USER, L"Environment", L"Path"));
		split(readRegistryString(HKEY_LOCAL_MACHINE,
			L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment",
			L"Path"));
		return dirs;
	}
#endif /* _WIN32 */

	struct ToolDirProbe {
		const char* env_var;
		const char* suffix;
	};

	static std::vector<std::string> probeToolDirs(const ToolDirProbe* probes,
	                                              const char* marker_exe) {
		namespace fs = std::filesystem;
		std::vector<std::string> hits;
		for (int i = 0; probes[i].env_var; ++i) {
			const char* v = std::getenv(probes[i].env_var);
			if (!v) continue;
			std::string d = std::string(v) + probes[i].suffix;
			std::error_code ec;
			fs::path exe = fs::path(d) / marker_exe;
			if (fs::exists(exe, ec) && !fs::is_directory(exe, ec)) {
				hits.push_back(std::move(d));
			}
		}

		return hits;
	}

	static std::vector<std::string> findGitDirs() {
		static const ToolDirProbe kProbes[] = {
			{ "ProgramFiles",      "\\Git\\cmd" },
			{ "ProgramFiles(x86)", "\\Git\\cmd" },
			{ "ProgramW6432",      "\\Git\\cmd" },
			{ "LOCALAPPDATA",      "\\Programs\\Git\\cmd" },
			{ "USERPROFILE",       "\\scoop\\apps\\git\\current\\cmd" },
			{ "USERPROFILE",       "\\scoop\\shims" },
			{ "ProgramData",       "\\chocolatey\\bin" },
			{ nullptr, nullptr },
		};
		return probeToolDirs(kProbes, "git.exe");
	}

	// Not redundant with findGitDirs: git hooks and editor wrappers use
	// `#!/usr/bin/env sh` shebangs that resolve sh through PATH, and
	// sh.exe lives in `\Git\usr\bin`, not `\Git\cmd`.
	static std::vector<std::string> findGitUnixDirs() {
		static const ToolDirProbe kProbes[] = {
			{ "ProgramFiles",      "\\Git\\usr\\bin" },
			{ "ProgramFiles(x86)", "\\Git\\usr\\bin" },
			{ "ProgramW6432",      "\\Git\\usr\\bin" },
			{ "LOCALAPPDATA",      "\\Programs\\Git\\usr\\bin" },
			{ "USERPROFILE",       "\\scoop\\apps\\git\\current\\usr\\bin" },
			{ nullptr, nullptr },
		};
		return probeToolDirs(kProbes, "sh.exe");
	}

	static std::vector<std::string> findDockerDirs() {
		static const ToolDirProbe kProbes[] = {
			{ "ProgramFiles",      "\\Docker\\Docker\\resources\\bin" },
			{ "ProgramFiles(x86)", "\\Docker\\Docker\\resources\\bin" },
			{ "ProgramW6432",      "\\Docker\\Docker\\resources\\bin" },
			{ "USERPROFILE",       "\\scoop\\apps\\docker\\current" },
			{ "USERPROFILE",       "\\scoop\\shims" },
			{ "ProgramData",       "\\chocolatey\\bin" },
			{ nullptr, nullptr },
		};
		return probeToolDirs(kProbes, "docker.exe");
	}

	static void prependDirsToPath(Environment& env, const PathConv& pc,
	                              const std::vector<std::string>& dirs) {
		if (dirs.empty()) return;
		std::string path = env.get("PATH");
		std::string prepend;
		for (const auto& d : dirs) {
			std::string posix = pc.toPosix(d);
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
		prependDirsToPath(env, pc, registryPathDirs());
#endif /* _WIN32 */
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

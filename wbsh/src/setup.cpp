/**
 * @file setup.cpp
 * @brief Process-environment seeding and child-shell state inheritance.
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
	static void trimTrailingNuls(std::wstring& text) {
		while (!text.empty() && text.back() == L'\0') text.pop_back();
	}

	static std::wstring expandEnvironmentStrings(const std::wstring& text) {
		const DWORD needed = ::ExpandEnvironmentStringsW(text.c_str(), nullptr, 0);
		if (needed == 0) return text;

		std::wstring expanded(needed, L'\0');
		::ExpandEnvironmentStringsW(text.c_str(), expanded.data(), needed);
		trimTrailingNuls(expanded);
		return expanded;
	}

	static std::string wideToUtf8(const std::wstring& text) {
		const int length = static_cast<int>(text.size());
		const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), length,
			nullptr, 0, nullptr, nullptr);
		std::string utf8(needed, '\0');
		::WideCharToMultiByte(CP_UTF8, 0, text.data(), length,
			utf8.data(), needed, nullptr, nullptr);
		return utf8;
	}

	static bool readRegistryValue(HKEY key, const wchar_t* value, std::wstring& text,
	                              DWORD& type) {
		DWORD size = 0;
		const LONG probed = ::RegQueryValueExW(key, value, nullptr, &type, nullptr, &size);
		if (probed != ERROR_SUCCESS) return false;
		if (type != REG_SZ && type != REG_EXPAND_SZ) return false;
		if (size == 0) return false;

		text.assign(size / sizeof(wchar_t) + 1, L'\0');
		const LONG read = ::RegQueryValueExW(key, value, nullptr, &type,
			reinterpret_cast<LPBYTE>(text.data()), &size);
		return read == ERROR_SUCCESS;
	}

	static std::string readRegistryString(HKEY root, const wchar_t* subkey,
	                                      const wchar_t* value) {
		HKEY key = nullptr;
		if (::RegOpenKeyExW(root, subkey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return {};

		std::wstring text;
		DWORD type = 0;
		const bool read = readRegistryValue(key, value, text, type);
		::RegCloseKey(key);
		if (!read) return {};

		trimTrailingNuls(text);
		if (type == REG_EXPAND_SZ) text = expandEnvironmentStrings(text);
		return wideToUtf8(text);
	}

	static void appendSemicolonList(const std::string& list, std::vector<std::string>& dirs) {
		std::string current;
		for (char c : list) {
			if (c != ';') {
				current.push_back(c);
				continue;
			}

			if (!current.empty()) dirs.push_back(current);
			current.clear();
		}

		if (!current.empty()) dirs.push_back(current);
	}

	static std::vector<std::string> registryPathDirs() {
		std::vector<std::string> dirs;
		appendSemicolonList(readRegistryString(HKEY_CURRENT_USER, L"Environment", L"Path"), dirs);
		appendSemicolonList(readRegistryString(HKEY_LOCAL_MACHINE,
			L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment",
			L"Path"), dirs);
		return dirs;
	}
#endif /* _WIN32 */

	struct ToolDirProbe {
		const char* env_var;
		const char* suffix;
	};

	static bool fileExists(const std::filesystem::path& path) {
		std::error_code ec;
		return std::filesystem::exists(path, ec) && !std::filesystem::is_directory(path, ec);
	}

	static std::vector<std::string> probeToolDirs(const ToolDirProbe* probes,
	                                              const char* marker_exe) {
		std::vector<std::string> hits;
		for (int i = 0; probes[i].env_var != nullptr; ++i) {
			const char* base = std::getenv(probes[i].env_var);
			if (base == nullptr) continue;

			std::string dir = std::string(base) + probes[i].suffix;
			if (fileExists(std::filesystem::path(dir) / marker_exe)) hits.push_back(std::move(dir));
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

	static std::string missingPosixDirs(const PathConv& path_conv, const std::string& path,
	                                    const std::vector<std::string>& dirs) {
		std::string prepend;
		for (const auto& dir : dirs) {
			const std::string posix = path_conv.toPosix(dir);
			if (!path.empty() && path.find(posix) != std::string::npos) continue;
			if (!prepend.empty()) prepend.push_back(':');
			prepend += posix;
		}

		return prepend;
	}

	static void prependDirsToPath(Environment& env, const PathConv& path_conv,
	                              const std::vector<std::string>& dirs) {
		if (dirs.empty()) return;

		const std::string path = env.get("PATH");
		const std::string prepend = missingPosixDirs(path_conv, path, dirs);
		if (prepend.empty()) return;

		env.set("PATH", path.empty() ? prepend : (prepend + ":" + path));
	}

	static void seedHome(Environment& env, const PathConv& path_conv) {
		std::string home = env.get("HOME");
		if (home.empty()) home = env.get("USERPROFILE");
		if (home.empty()) return;

		env.set("HOME", path_conv.toPosix(home));
		env.exportVar("HOME");
	}

	static void seedPwd(Environment& env, const PathConv& path_conv) {
		std::error_code ec;
		const std::filesystem::path cwd = std::filesystem::current_path(ec);
		if (ec) return;

		env.set("PWD", path_conv.toPosix(pathToUtf8(cwd)));
		env.exportVar("PWD");
	}

	void prepareEnv(Environment& env) {
		env.loadFromProcessEnv();

		const PathConv path_conv;
		const std::string path = env.get("PATH");
		if (!path.empty()) env.set("PATH", path_conv.pathListWin32ToPosix(path));

#ifdef _WIN32
		prependDirsToPath(env, path_conv, registryPathDirs());
#endif /* _WIN32 */
		prependDirsToPath(env, path_conv, findGitDirs());
		prependDirsToPath(env, path_conv, findGitUnixDirs());
		prependDirsToPath(env, path_conv, findDockerDirs());

		seedHome(env, path_conv);
		seedPwd(env, path_conv);
	}

	static void unexportInheritedLocals(Environment& env, const std::string& names) {
		std::size_t i = 0;
		while (i < names.size()) {
			while (i < names.size() && names[i] == ' ') ++i;

			const std::size_t start = i;
			while (i < names.size() && names[i] != ' ') ++i;
			if (i > start) env.unexportVar(names.substr(start, i - start));
		}
	}

	static void replayInheritedText(Environment& env, Executor& exec, const char* var,
	                                const char* label) {
		const std::string text = env.get(var);
		if (text.empty()) return;

		env.unset(var);
		exec.executeText(text, label);
	}

	void absorbInheritedState(Environment& env, Executor& exec) {
		const std::string local_names = env.get("WBSH_LOCAL_NAMES");
		if (!local_names.empty()) {
			env.unset("WBSH_LOCAL_NAMES");
			unexportInheritedLocals(env, local_names);
		}

		replayInheritedText(env, exec, "WBSH_FUNCTIONS", "<inherited functions>");
		replayInheritedText(env, exec, "WBSH_ALIASES",   "<inherited aliases>");
		replayInheritedText(env, exec, "WBSH_ARRAYS",    "<inherited arrays>");
	}

}  // namespace wbsh

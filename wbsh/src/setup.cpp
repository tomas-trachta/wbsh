/**
 * @file setup.cpp
 * @brief Process-environment seeding and child-shell state inheritance.
 */

#include "setup.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <tlhelp32.h>
#else
#  include <unistd.h>
#endif /* _WIN32 */

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <filesystem>
#include <string>
#include <vector>

#include "numparse.h"
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

	static std::string environmentUtf8(const char* name) {
		const wchar_t* value = ::_wgetenv(utf8ToWide(name).c_str());
		return value == nullptr ? std::string() : wideToUtf8(value);
	}

	static std::vector<std::string> probeToolDirs(const ToolDirProbe* probes,
	                                              const char* marker_exe) {
		std::vector<std::string> hits;
		for (int i = 0; probes[i].env_var != nullptr; ++i) {
			const std::string base = environmentUtf8(probes[i].env_var);
			if (base.empty()) continue;

			std::string dir = base + probes[i].suffix;
			if (fileExists(utf8ToPath(dir) / marker_exe)) hits.push_back(std::move(dir));
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
			if (prepend.find(posix) != std::string::npos) continue;
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

		env.set("PWD", path_conv.toPosix(pathToUtf8(expandShortNames(cwd))));
		env.exportVar("PWD");
	}

	static void addDiscoveredToolDirs(Environment& env, const PathConv& path_conv) {
#ifdef _WIN32
		prependDirsToPath(env, path_conv, registryPathDirs());
#endif /* _WIN32 */
		prependDirsToPath(env, path_conv, findGitDirs());
		prependDirsToPath(env, path_conv, findGitUnixDirs());
		prependDirsToPath(env, path_conv, findDockerDirs());
	}

	bool isChildShell(const Environment& env) {
		return env.get("WBSH_SUBSHELL") == "1";
	}

#if !defined(WBSH_VERSION_MAJOR) || !defined(WBSH_VERSION_MINOR) || !defined(WBSH_VERSION_PATCH)
#define WBSH_VERSION_MAJOR 0
#define WBSH_VERSION_MINOR 0
#define WBSH_VERSION_PATCH 0
#endif
#define WBSH_SETUP_VSTR2(x) #x
#define WBSH_SETUP_VSTR(x) WBSH_SETUP_VSTR2(x)

	// Scripts gate on BASH_VERSION / BASH_VERSINFO, so wbsh reports the bash
	// release it is compatible with; WBSH_VERSION is its own version. OSTYPE
	// and MACHTYPE are Git Bash's, which scripts already treat as Windows.
	static const char* const kBashVersInfo[] = { "5", "2", "37", "1", "release" };
	static const char* const kWbshVersion = WBSH_SETUP_VSTR(WBSH_VERSION_MAJOR) "."
		WBSH_SETUP_VSTR(WBSH_VERSION_MINOR) "." WBSH_SETUP_VSTR(WBSH_VERSION_PATCH);

	static std::string hostType() {
#if defined(_M_ARM64) || defined(__aarch64__)
		return "aarch64";
#else
		return "x86_64";
#endif
	}

	static std::string selfExecutablePath(const PathConv& path_conv) {
#ifdef _WIN32
		wchar_t buffer[MAX_PATH] = {};
		const DWORD length = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
		if (length == 0 || length >= MAX_PATH) return "wbsh";

		return path_conv.toPosix(wideToUtf8(std::wstring(buffer, length)));
#else
		(void)path_conv;
		return "wbsh";
#endif /* _WIN32 */
	}

	static long long parentProcessId() {
#ifdef _WIN32
		const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE) return 0;

		const DWORD self = ::GetCurrentProcessId();
		PROCESSENTRY32W entry{};
		entry.dwSize = sizeof(entry);
		long long parent = 0;
		BOOL more = ::Process32FirstW(snapshot, &entry);
		while (more && parent == 0) {
			if (entry.th32ProcessID == self) parent = entry.th32ParentProcessID;
			more = ::Process32NextW(snapshot, &entry);
		}

		::CloseHandle(snapshot);
		return parent;
#else
		return static_cast<long long>(::getppid());
#endif /* _WIN32 */
	}

	// A self-spawned subshell keeps its parent shell's `$PPID`.
	static std::string shellParentPid(const Environment& env) {
		long long pid = 0;
		if (parseLL(env.get("WBSH_PPID"), pid)) return std::to_string(pid);

		return std::to_string(parentProcessId());
	}

	static void setReadonlyInteger(Environment& env, const std::string& name, std::string value) {
		env.unexportVar(name);
		env.set(name, std::move(value));
		env.setAttributes(name, kAttrInteger);
		env.markReadonly(name);
	}

	static void seedBashVariables(Environment& env, const PathConv& path_conv) {
		const std::string machine = hostType() + "-pc-msys";
		env.set("BASH_VERSION", "5.2.37(1)-release");
		env.set("WBSH_VERSION", kWbshVersion);
		env.set("BASH", selfExecutablePath(path_conv));
		env.set("OSTYPE", "msys");
		env.set("HOSTTYPE", hostType());
		env.set("MACHTYPE", machine);

		std::vector<std::string> versinfo(std::begin(kBashVersInfo), std::end(kBashVersInfo));
		versinfo.push_back(machine);
		env.setIndexedArrayFromList("BASH_VERSINFO", std::move(versinfo));
		env.markReadonly("BASH_VERSINFO");

		setReadonlyInteger(env, "PPID", shellParentPid(env));
	}

	void prepareEnv(Environment& env) {
		env.loadFromProcessEnv();

		const PathConv path_conv;
		const std::string path = env.get("PATH");
		if (!path.empty()) env.set("PATH", path_conv.pathListWin32ToPosix(path));

		if (!isChildShell(env)) addDiscoveredToolDirs(env, path_conv);

		seedHome(env, path_conv);
		seedPwd(env, path_conv);
		seedBashVariables(env, path_conv);
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

	static void applyInheritedDisabledBuiltins(Environment& env, Executor& exec) {
		const std::string names = env.get("WBSH_DISABLED_BUILTINS");
		if (names.empty()) return;

		env.unset("WBSH_DISABLED_BUILTINS");
		std::size_t start = 0;
		while (start < names.size()) {
			std::size_t end = names.find(' ', start);
			if (end == std::string::npos) end = names.size();
			if (end > start) exec.setBuiltinEnabled(names.substr(start, end - start), false);
			start = end + 1;
		}
	}

	static void applyInheritedFunctionFiles(Environment& env, Executor& exec) {
		const std::string text = env.get("WBSH_FUNCTION_FILES");
		if (text.empty()) return;

		env.unset("WBSH_FUNCTION_FILES");
		exec.adoptInheritedFunctionFiles(text);
	}

	static void applyInheritedOption(Environment& env, const std::string& word) {
		if (word.size() < 2) return;

		const std::string name = word.substr(1);
		const bool on = word[0] == '-';
		const SetOptionSpec* set_spec = findSetOption(name);
		if (set_spec != nullptr) env.options().*(set_spec->flag) = on;

		const ShoptSpec* shopt_spec = findShopt(name);
		if (shopt_spec != nullptr) env.shopts().*(shopt_spec->flag) = on;
	}

	static void applyInheritedOptions(Environment& env) {
		const std::string words = env.get("WBSH_OPTIONS");
		if (words.empty()) return;

		env.unset("WBSH_OPTIONS");
		std::size_t i = 0;
		while (i < words.size()) {
			while (i < words.size() && words[i] == ' ') ++i;

			const std::size_t start = i;
			while (i < words.size() && words[i] != ' ') ++i;
			applyInheritedOption(env, words.substr(start, i - start));
		}
	}

	static void applyInheritedShellParameters(Environment& env, Executor& exec) {
		long long pid = 0;
		if (parseLL(env.get("WBSH_SHELL_PID"), pid)) env.setShellPid(pid);
		env.unset("WBSH_SHELL_PID");

		int status = 0;
		if (parseInt(env.get("WBSH_STATUS"), status)) exec.setLastStatus(status);
		env.unset("WBSH_STATUS");
		env.unset("WBSH_PPID");
	}

	static void applyInheritedHistory(Environment& env, Executor& exec) {
		const std::string path = env.get("WBSH_HISTORY");
		if (path.empty()) return;

		env.unset("WBSH_HISTORY");
		exec.loadHistoryFromFile(path);

		std::error_code ec;
		std::filesystem::remove(utf8ToPath(path), ec);
	}

	static std::vector<std::string> splitFields(const std::string& line, std::size_t count) {
		std::vector<std::string> fields;
		std::size_t start = 0;
		while (fields.size() + 1 < count) {
			const std::size_t space = line.find(' ', start);
			if (space == std::string::npos) break;

			fields.push_back(line.substr(start, space - start));
			start = space + 1;
		}

		fields.push_back(line.substr(std::min<std::size_t>(start, line.size())));
		return fields;
	}

	// A running job stays visible to `jobs` and `wait` here through a
	// handle of our own on its process.
	static void* openJobProcess(long long pid) {
#ifdef _WIN32
		const DWORD access = SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION;
		return ::OpenProcess(access, FALSE, static_cast<DWORD>(pid));
#else
		(void)pid;
		return nullptr;
#endif /* _WIN32 */
	}

	static void applyInheritedJob(Executor& exec, const std::string& line) {
		const std::vector<std::string> fields = splitFields(line, 5);
		if (fields.size() != 5) return;

		Executor::Job job;
		if (!parseInt(fields[0], job.id) || !parseLL(fields[1], job.pid)) return;
		if (!parseInt(fields[3], job.exit_code)) return;

		job.running = fields[2] == "1";
		job.cmd_text = fields[4];
		if (job.running) job.handle = openJobProcess(job.pid);
		if (job.running && job.handle == nullptr) return;

		exec.adoptInheritedJob(job);
	}

	static void applyInheritedJobs(Environment& env, Executor& exec) {
		const std::string text = env.get("WBSH_JOBS");
		if (text.empty()) return;

		env.unset("WBSH_JOBS");
		std::size_t start = 0;
		while (start < text.size()) {
			std::size_t end = text.find('\n', start);
			if (end == std::string::npos) end = text.size();

			applyInheritedJob(exec, text.substr(start, end - start));
			start = end + 1;
		}
	}

	void absorbInheritedState(Environment& env, Executor& exec) {
		env.unset("WBSH_SUBSHELL");
		env.unset("WBSH_UTIL_FILES");

		const std::string local_names = env.get("WBSH_LOCAL_NAMES");
		if (!local_names.empty()) {
			env.unset("WBSH_LOCAL_NAMES");
			unexportInheritedLocals(env, local_names);
		}

		replayInheritedText(env, exec, "WBSH_FUNCTIONS", "<inherited functions>");
		replayInheritedText(env, exec, "WBSH_ALIASES",   "<inherited aliases>");
		replayInheritedText(env, exec, "WBSH_ARRAYS",    "<inherited arrays>");
		exec.adoptInheritedCallStack();
		applyInheritedFunctionFiles(env, exec);
		applyInheritedDisabledBuiltins(env, exec);
		env.unset("WBSH_LINENO");
		replayInheritedText(env, exec, "WBSH_ATTRIBUTES", "<inherited attributes>");
		replayInheritedText(env, exec, "WBSH_BINDINGS", "<inherited bindings>");
		replayInheritedText(env, exec, "WBSH_COMPLETIONS", "<inherited completions>");
		applyInheritedOptions(env);
		applyInheritedShellParameters(env, exec);
		applyInheritedHistory(env, exec);
		applyInheritedJobs(env, exec);
	}

}  // namespace wbsh

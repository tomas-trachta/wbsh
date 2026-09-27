/**
 * @file utils.cpp
 * @brief Binding the shell to the SDK, and finding utils to load.
 *
 * The SDK is reached through LoadLibrary rather than linked, so wbsh.exe
 * keeps working on its own with no DLL beside it -- the whole point of a
 * single self-contained binary. No SDK simply means no utils.
 */

#include "utils.h"

#include "coreutils_internal.h"
#include "executor.h"
#include "interrupt.h"
#include "pathconv.h"
#include "wbshsdk.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif /* WIN32_LEAN_AND_MEAN */
#include <windows.h>

#include <cstdio>
#include <filesystem>

namespace wbsh {

	namespace utils_detail {

		struct SdkEntries {
			int  (*bind)(const WbshHostApi*) = nullptr;
			int  (*load_directory)(const wchar_t*, WbshLogFn, void*) = nullptr;
			int  (*load_file)(const wchar_t*, WbshLogFn, void*) = nullptr;
			void (*unload_all)(void) = nullptr;
			int  (*count)(void) = nullptr;
			const WbshUtilInfo* (*info_at)(int) = nullptr;
			const wchar_t* (*path_at)(int) = nullptr;
		};

		static SdkEntries                g_sdk;
		static bool                      g_bound = false;
		static std::vector<UtilInfo>     g_loaded;
		static std::vector<std::string>  g_failures;
		static std::vector<std::wstring> g_loaded_by_hand;
		static std::string               g_scratch;

		/* ---- what the SDK calls back into ------------------------------- */

		static int registerCommand(void* context, const WbshCommand* command) {
			auto* exec = static_cast<Executor*>(context);

			PluginCommand added;
			added.fn       = command->fn;
			added.complete = command->complete;
			added.user     = command->user;
			added.summary  = command->summary != nullptr ? command->summary : "";
			added.usage    = command->usage != nullptr ? command->usage : "";
			return exec->registerPluginCommand(command->name, added) ? WBSH_OK : WBSH_ERR_TAKEN;
		}

		static void writeOut(void*, const char* bytes, size_t length) {
			std::fwrite(bytes, 1, length, stdout);
			std::fflush(stdout);
		}

		static void writeErr(void*, const char* bytes, size_t length) {
			std::fwrite(bytes, 1, length, stderr);
			std::fflush(stderr);
		}

		// The string has to outlive the call and no further, so one buffer
		// per host is enough: a util that wants to keep it copies it.
		static const char* workingDirectory(void*) {
			std::error_code ec;
			const std::filesystem::path here = std::filesystem::current_path(ec);
			if (ec) return nullptr;

			g_scratch = pathToUtf8(here);
			return g_scratch.c_str();
		}

		static const char* variable(void* context, const char* name) {
			auto* exec = static_cast<Executor*>(context);
			if (name == nullptr) return nullptr;

			if (!exec->env().has(name)) return nullptr;

			g_scratch = exec->env().get(name);
			return g_scratch.c_str();
		}

		static int setVariable(void* context, const char* name, const char* value) {
			auto* exec = static_cast<Executor*>(context);

			if (value == nullptr) {
				exec->env().unset(name);
			} else {
				exec->env().set(name, value);
			}

			return WBSH_OK;
		}

		static int cancelled(void*) {
			return ctrlCPending() ? 1 : 0;
		}

		static void completeAdd(void*, WbshCompletion* completion, const char* text) {
			completion->items.emplace_back(text);
		}

		// A refusal is said once as it happens and kept, so `utils` can
		// show it again after it has scrolled off the screen.
		static void complain(void*, const char* message) {
			std::fprintf(stderr, "wbsh: util: %s\n", message);
			g_failures.emplace_back(message);
		}

		static WbshHostApi shellApi(Executor& exec) {
			WbshHostApi api{};
			api.size    = static_cast<uint32_t>(sizeof(api));
			api.abi     = WBSH_SDK_ABI;
			api.kind    = WBSH_HOST_SHELL;
			api.context = &exec;

			api.register_command  = registerCommand;
			api.register_segment  = nullptr;
			api.write_out         = writeOut;
			api.write_err         = writeErr;
			api.working_directory = workingDirectory;
			api.variable          = variable;
			api.set_variable      = setVariable;
			api.cancelled         = cancelled;
			api.complete_add      = completeAdd;
			return api;
		}

		/* ---- finding the SDK and the folders ---------------------------- */

		template <typename Fn>
		static Fn entry(HMODULE sdk, const char* name) {
			return reinterpret_cast<Fn>(reinterpret_cast<void*>(::GetProcAddress(sdk, name)));
		}

		static bool findSdk(HMODULE sdk, SdkEntries& out_entries) {
			out_entries.bind = entry<int (*)(const WbshHostApi*)>(sdk, "wbshSdkBindHost");
			out_entries.load_directory = entry<int (*)(const wchar_t*, WbshLogFn, void*)>(
				sdk, "wbshSdkLoadDirectory");
			out_entries.load_file = entry<int (*)(const wchar_t*, WbshLogFn, void*)>(
				sdk, "wbshSdkLoadFile");
			out_entries.unload_all = entry<void (*)(void)>(sdk, "wbshSdkUnloadAll");
			out_entries.count = entry<int (*)(void)>(sdk, "wbshSdkCount");
			out_entries.info_at = entry<const WbshUtilInfo* (*)(int)>(sdk, "wbshSdkInfoAt");
			out_entries.path_at = entry<const wchar_t* (*)(int)>(sdk, "wbshSdkPathAt");

			return out_entries.bind != nullptr && out_entries.load_directory != nullptr
				&& out_entries.load_file != nullptr && out_entries.unload_all != nullptr
				&& out_entries.count != nullptr && out_entries.info_at != nullptr
				&& out_entries.path_at != nullptr;
		}

		static std::wstring directoryOfThisExe() {
			wchar_t path[MAX_PATH] = {};
			const DWORD length = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
			const std::wstring full(path, length);

			const std::size_t cut = full.find_last_of(L'\\');
			return cut == std::wstring::npos ? std::wstring() : full.substr(0, cut + 1);
		}

		static std::wstring environmentValue(const wchar_t* name) {
			wchar_t* value = nullptr;
			std::size_t length = 0;
			if (_wdupenv_s(&value, &length, name) != 0 || value == nullptr) {
				return std::wstring();
			}

			const std::wstring copy(value);
			std::free(value);
			return copy;
		}

		// A user's own utils live beside their config rather than inside
		// Program Files, so installing one needs no administrator.
		static std::wstring userPluginDirectory() {
			const std::wstring roaming = environmentValue(L"APPDATA");
			return roaming.empty() ? std::wstring() : roaming + L"\\wbsh\\plugins";
		}

		// WBSH_PLUGINS is for the developer's own build output: a folder
		// list like PATH, scanned after the two folders an install owns.
		static void appendExtraDirectories(std::vector<std::wstring>& folders) {
			const std::wstring extra = environmentValue(L"WBSH_PLUGINS");

			std::size_t start = 0;
			while (start <= extra.size()) {
				std::size_t stop = extra.find(L';', start);
				if (stop == std::wstring::npos) stop = extra.size();

				const std::wstring folder = extra.substr(start, stop - start);
				if (!folder.empty()) folders.push_back(folder);
				start = stop + 1;
			}
		}

		static std::vector<std::wstring> pluginDirectories() {
			std::vector<std::wstring> folders;
			folders.push_back(directoryOfThisExe() + L"plugins");

			const std::wstring mine = userPluginDirectory();
			if (!mine.empty()) folders.push_back(mine);

			appendExtraDirectories(folders);
			return folders;
		}

		/* ---- loading ------------------------------------------------------ */

		static bool bindSdk(Executor& exec) {
			if (g_bound) return true;

			const HMODULE sdk = ::LoadLibraryW((directoryOfThisExe() + L"wbshsdk.dll").c_str());
			if (sdk == nullptr) return false;
			if (!findSdk(sdk, g_sdk)) return false;

			const WbshHostApi api = shellApi(exec);
			if (g_sdk.bind(&api) != WBSH_OK) return false;

			g_bound = true;
			return true;
		}

		static void rememberLoaded() {
			g_loaded.clear();

			for (int index = 0; index < g_sdk.count(); ++index) {
				const WbshUtilInfo* info = g_sdk.info_at(index);
				if (info == nullptr) continue;

				UtilInfo shown;
				shown.name    = info->name != nullptr ? info->name : "";
				shown.version = info->version != nullptr ? info->version : "";
				shown.summary = info->summary != nullptr ? info->summary : "";
				shown.path    = wideToUtf8(g_sdk.path_at(index));
				g_loaded.push_back(shown);
			}
		}

		static void loadEverything() {
			for (const std::wstring& folder : pluginDirectories()) {
				g_sdk.load_directory(folder.c_str(), complain, nullptr);
			}

			for (const std::wstring& file : g_loaded_by_hand) {
				g_sdk.load_file(file.c_str(), complain, nullptr);
			}

			rememberLoaded();
		}

		static std::wstring absoluteUtilPath(const std::string& given) {
			std::error_code ec;
			const std::filesystem::path full = std::filesystem::absolute(utf8ToPath(given), ec);
			return ec ? utf8ToPath(given).wstring() : full.wstring();
		}

		/* ---- the `utils` command ------------------------------------------ */

		static void printLoaded() {
			if (g_loaded.empty()) {
				std::fputs("no utils loaded\n", stdout);
			}

			for (const UtilInfo& util : g_loaded) {
				std::fprintf(stdout, "%-16s %-10s %s\n", util.name.c_str(), util.version.c_str(),
					util.summary.c_str());
			}
		}

		static void printFailures() {
			if (g_failures.empty()) return;

			std::fputs("\nnot loaded:\n", stdout);
			for (const std::string& why : g_failures) {
				std::fprintf(stdout, "  %s\n", why.c_str());
			}
		}

		static int utilsList() {
			printLoaded();
			printFailures();
			return 0;
		}

		static int utilsLoad(Executor& exec, const std::string& given) {
			if (!bindSdk(exec)) {
				perr("utils", "wbshsdk.dll is not beside wbsh.exe");
				return 1;
			}

			const std::wstring path = absoluteUtilPath(given);
			const int loaded = g_sdk.load_file(path.c_str(), complain, nullptr);
			if (loaded == 1) g_loaded_by_hand.push_back(path);

			rememberLoaded();
			return loaded == 1 ? 0 : 1;
		}

		// Everything a util registered points into its DLL, so the commands
		// go first and the DLLs after; then the same folders are walked
		// again, plus whatever was loaded by hand since the shell started.
		static int utilsReload(Executor& exec) {
			if (!bindSdk(exec)) {
				perr("utils", "wbshsdk.dll is not beside wbsh.exe");
				return 1;
			}

			exec.clearPluginCommands();
			g_sdk.unload_all();
			g_failures.clear();

			loadEverything();
			return utilsList();
		}

		static int utilsUsage() {
			perr("utils", "usage: utils | utils load <dll> | utils reload");
			return 2;
		}

	}  // namespace utils_detail

	// `utils` is how anyone finds out what a machine actually has loaded,
	// which matters more than usual when the answer came out of a folder
	// rather than out of this binary.
	static int builtin_utils(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) return utils_detail::utilsList();
		if (args[0] == "reload" && args.size() == 1) return utils_detail::utilsReload(exec);
		if (args[0] == "load" && args.size() == 2) return utils_detail::utilsLoad(exec, args[1]);

		return utils_detail::utilsUsage();
	}

	void registerUtilsBuiltin(Executor& exec) {
		exec.registerBuiltin("utils", builtin_utils);
	}

	const std::vector<UtilInfo>& loadedUtils() {
		return utils_detail::g_loaded;
	}

	const std::vector<std::string>& utilLoadFailures() {
		return utils_detail::g_failures;
	}

	void loadUtils(Executor& exec) {
		if (!utils_detail::bindSdk(exec)) return;

		utils_detail::loadEverything();
	}

}  // namespace wbsh

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

		static std::vector<UtilInfo> g_loaded;
		static std::string           g_scratch;

		static int registerCommand(void* context, const char* name, WbshCommandFn fn,
				void* user) {
			auto* exec = static_cast<Executor*>(context);

			PluginCommand command;
			command.fn = fn;
			command.user = user;
			return exec->registerPluginCommand(name, command) ? WBSH_OK : WBSH_ERR_TAKEN;
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

		static void complain(void*, const char* message) {
			std::fprintf(stderr, "wbsh: util: %s\n", message);
		}

		static std::wstring directoryOfThisExe() {
			wchar_t path[MAX_PATH] = {};
			const DWORD length = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
			const std::wstring full(path, length);

			const std::size_t cut = full.find_last_of(L'\\');
			return cut == std::wstring::npos ? std::wstring() : full.substr(0, cut + 1);
		}

		// A user's own utils live beside their config rather than inside
		// Program Files, so installing one needs no administrator.
		static std::wstring userPluginDirectory() {
			wchar_t* roaming = nullptr;
			std::size_t length = 0;
			if (_wdupenv_s(&roaming, &length, L"APPDATA") != 0 || roaming == nullptr) {
				return std::wstring();
			}

			const std::wstring folder = std::wstring(roaming) + L"\\wbsh\\plugins";
			std::free(roaming);
			return folder;
		}

		struct SdkEntries {
			int  (*bind)(const WbshHostApi*) = nullptr;
			int  (*load)(const wchar_t*, WbshLogFn, void*) = nullptr;
			int  (*count)(void) = nullptr;
			const WbshUtilInfo* (*info_at)(int) = nullptr;
		};

		static bool findSdk(HMODULE sdk, SdkEntries& out_entries) {
			out_entries.bind = reinterpret_cast<int (*)(const WbshHostApi*)>(
				reinterpret_cast<void*>(::GetProcAddress(sdk, "wbshSdkBindHost")));
			out_entries.load = reinterpret_cast<int (*)(const wchar_t*, WbshLogFn, void*)>(
				reinterpret_cast<void*>(::GetProcAddress(sdk, "wbshSdkLoadDirectory")));
			out_entries.count = reinterpret_cast<int (*)(void)>(
				reinterpret_cast<void*>(::GetProcAddress(sdk, "wbshSdkCount")));
			out_entries.info_at = reinterpret_cast<const WbshUtilInfo* (*)(int)>(
				reinterpret_cast<void*>(::GetProcAddress(sdk, "wbshSdkInfoAt")));

			return out_entries.bind != nullptr && out_entries.load != nullptr
				&& out_entries.count != nullptr && out_entries.info_at != nullptr;
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
			return api;
		}

		static void rememberLoaded(const SdkEntries& sdk) {
			g_loaded.clear();

			for (int index = 0; index < sdk.count(); ++index) {
				const WbshUtilInfo* info = sdk.info_at(index);
				if (info == nullptr) continue;

				UtilInfo shown;
				shown.name    = info->name != nullptr ? info->name : "";
				shown.version = info->version != nullptr ? info->version : "";
				shown.summary = info->summary != nullptr ? info->summary : "";
				g_loaded.push_back(shown);
			}
		}

	}  // namespace utils_detail

	// `utils` is how anyone finds out what a machine actually has loaded,
	// which matters more than usual when the answer came out of a folder
	// rather than out of this binary.
	static int builtin_utils(Executor&, const std::vector<std::string>& args) {
		if (!args.empty()) { perr("utils", "takes no arguments"); return 2; }

		const std::vector<UtilInfo>& loaded = loadedUtils();
		if (loaded.empty()) {
			std::fputs("no utils loaded\n", stdout);
			return 0;
		}

		for (const UtilInfo& util : loaded) {
			std::fprintf(stdout, "%-16s %-10s %s\n", util.name.c_str(), util.version.c_str(),
				util.summary.c_str());
		}

		return 0;
	}

	void registerUtilsBuiltin(Executor& exec) {
		exec.registerBuiltin("utils", builtin_utils);
	}

	const std::vector<UtilInfo>& loadedUtils() {
		return utils_detail::g_loaded;
	}

	void loadUtils(Executor& exec) {
		const std::wstring here = utils_detail::directoryOfThisExe();
		const HMODULE sdk = ::LoadLibraryW((here + L"wbshsdk.dll").c_str());
		if (sdk == nullptr) return;

		utils_detail::SdkEntries entries;
		if (!utils_detail::findSdk(sdk, entries)) return;

		const WbshHostApi api = utils_detail::shellApi(exec);
		if (entries.bind(&api) != WBSH_OK) return;

		entries.load((here + L"plugins").c_str(), utils_detail::complain, nullptr);

		const std::wstring mine = utils_detail::userPluginDirectory();
		if (!mine.empty()) entries.load(mine.c_str(), utils_detail::complain, nullptr);

		utils_detail::rememberLoaded(entries);
	}

}  // namespace wbsh

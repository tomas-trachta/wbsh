/**
 * @file wbshsdk.cpp
 * @brief The SDK library: it holds the host's functions on one side and
 *        the loaded utils on the other, and refuses anything that does
 *        not match the ABI it was built for.
 *
 * Everything here is single-threaded by contract. A host binds itself and
 * loads utils on one thread before anything else runs, and a util's
 * callbacks are made on the host's own thread: the shell's command thread
 * or the terminal's painting thread. Nothing below takes a lock.
 */

#include "wbshsdk.h"

#include "wbshsdk_internal.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif /* WIN32_LEAN_AND_MEAN */
#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

#ifndef WBSH_VERSION_MAJOR
#  define WBSH_VERSION_MAJOR 0
#  define WBSH_VERSION_MINOR 0
#  define WBSH_VERSION_PATCH 0
#endif /* WBSH_VERSION_MAJOR */

#define WBSH_SDK_QUOTE(text)  #text
#define WBSH_SDK_STRING(text) WBSH_SDK_QUOTE(text)

#define WBSH_SDK_VERSION_TEXT \
	WBSH_SDK_STRING(WBSH_VERSION_MAJOR) "." \
	WBSH_SDK_STRING(WBSH_VERSION_MINOR) "." \
	WBSH_SDK_STRING(WBSH_VERSION_PATCH)

namespace wbshsdk_detail {

	struct LoadedUtil {
		HMODULE              module = nullptr;
		const WbshUtilInfo*  info   = nullptr;
		std::wstring         path;
		void               (*unload)(void) = nullptr;
	};

	static WbshHostApi             g_host;
	static WbshApi                 g_api;
	static bool                    g_bound = false;
	static std::vector<LoadedUtil> g_utils;

	static void report(WbshLogFn log, void* context, const std::string& message) {
		if (log == nullptr) return;

		log(context, message.c_str());
	}

	static std::string narrow(const wchar_t* text) {
		if (text == nullptr) return std::string();

		const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0,
			nullptr, nullptr);
		if (needed <= 1) return std::string();

		std::string out(static_cast<std::size_t>(needed) - 1, '\0');
		::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
		return out;
	}

	// A name has to survive being typed at a prompt and being looked up in
	// a table, so anything with a space, a slash or a control byte in it is
	// refused before it can shadow something it should not.
	static bool nameIsUsable(const char* name) {
		if (name == nullptr || *name == '\0') return false;
		if (std::strlen(name) > 64) return false;

		for (const char* letter = name; *letter != '\0'; ++letter) {
			const unsigned char code = static_cast<unsigned char>(*letter);
			if (code <= ' ' || code == 0x7F) return false;
			if (*letter == '/' || *letter == '\\') return false;
		}

		return true;
	}

	/* ---- the table handed to utils ---------------------------------- */

	static const char* apiVersion(void) {
		return WBSH_SDK_VERSION_TEXT;
	}

	// The struct may grow at the end, so a util built against a shorter
	// one is read for as much as it says it filled in and zeroes stand in
	// for the rest.
	static WbshCommand paddedCommand(const WbshCommand* command) {
		WbshCommand padded;
		std::memset(&padded, 0, sizeof(padded));

		const std::size_t given = static_cast<std::size_t>(command->size);
		const std::size_t known = sizeof(WbshCommand);
		std::memcpy(&padded, command, given < known ? given : known);

		padded.size = static_cast<uint32_t>(known);
		return padded;
	}

	static int apiRegisterCommand(const WbshCommand* command) {
		if (!g_bound) return WBSH_ERR_NO_HOST;
		if (command == nullptr || command->size < offsetof(WbshCommand, user)) {
			return WBSH_ERR_BAD_NAME;
		}

		const WbshCommand padded = paddedCommand(command);
		if (!nameIsUsable(padded.name) || padded.fn == nullptr) return WBSH_ERR_BAD_NAME;
		if (g_host.register_command == nullptr) return WBSH_ERR_UNSUPPORTED;

		return g_host.register_command(g_host.context, &padded);
	}

	static int apiRegisterSegment(const char* name, WbshSegmentFn fn, void* user) {
		if (!g_bound) return WBSH_ERR_NO_HOST;
		if (!nameIsUsable(name) || fn == nullptr) return WBSH_ERR_BAD_NAME;
		if (g_host.register_segment == nullptr) return WBSH_ERR_UNSUPPORTED;

		return g_host.register_segment(g_host.context, name, fn, user);
	}

	static void apiWriteOut(const char* bytes, size_t length) {
		if (!g_bound || g_host.write_out == nullptr || bytes == nullptr) return;

		g_host.write_out(g_host.context, bytes, length);
	}

	static void apiWriteErr(const char* bytes, size_t length) {
		if (!g_bound || g_host.write_err == nullptr || bytes == nullptr) return;

		g_host.write_err(g_host.context, bytes, length);
	}

	static void apiPrint(const char* text) {
		if (text == nullptr) return;

		apiWriteOut(text, std::strlen(text));
	}

	static void apiPrintError(const char* text) {
		if (text == nullptr) return;

		apiWriteErr(text, std::strlen(text));
	}

	static const char* apiWorkingDirectory(void) {
		if (!g_bound || g_host.working_directory == nullptr) return nullptr;

		return g_host.working_directory(g_host.context);
	}

	static const char* apiVariable(const char* name) {
		if (!g_bound || g_host.variable == nullptr || name == nullptr) return nullptr;

		return g_host.variable(g_host.context, name);
	}

	static int apiSetVariable(const char* name, const char* value) {
		if (!g_bound) return WBSH_ERR_NO_HOST;
		if (name == nullptr || *name == '\0') return WBSH_ERR_BAD_NAME;
		if (g_host.set_variable == nullptr) return WBSH_ERR_UNSUPPORTED;

		return g_host.set_variable(g_host.context, name, value);
	}

	static int apiCancelled(void) {
		if (!g_bound || g_host.cancelled == nullptr) return 0;

		return g_host.cancelled(g_host.context);
	}

	static void apiCompleteAdd(WbshCompletion* completion, const char* text) {
		if (!g_bound || g_host.complete_add == nullptr) return;
		if (completion == nullptr || text == nullptr) return;

		g_host.complete_add(g_host.context, completion, text);
	}

	static void fillApi(WbshApi& api) {
		std::memset(&api, 0, sizeof(api));
		api.size = static_cast<uint32_t>(sizeof(api));
		api.abi  = WBSH_SDK_ABI;
		api.host = g_host.kind;

		api.version           = apiVersion;
		api.register_command  = apiRegisterCommand;
		api.register_segment  = apiRegisterSegment;
		api.write_out         = apiWriteOut;
		api.write_err         = apiWriteErr;
		api.print             = apiPrint;
		api.print_error       = apiPrintError;
		api.working_directory = apiWorkingDirectory;
		api.variable          = apiVariable;
		api.set_variable      = apiSetVariable;
		api.cancelled         = apiCancelled;
		api.complete_add      = apiCompleteAdd;

		fillTerminalApi(api);
	}

	/* ---- binding the host ------------------------------------------- */

	// The struct may grow at the end, so a host built against an older
	// header is taken at its word for as much as it says it filled in and
	// zeroes stand in for the rest.
	static void adoptHost(const WbshHostApi* api) {
		std::memset(&g_host, 0, sizeof(g_host));

		const std::size_t given = static_cast<std::size_t>(api->size);
		const std::size_t known = sizeof(WbshHostApi);
		std::memcpy(&g_host, api, given < known ? given : known);

		g_host.size = static_cast<uint32_t>(known);
		g_bound = true;
		fillApi(g_api);
	}

	/* ---- loading utils ---------------------------------------------- */

	struct UtilEntries {
		const WbshUtilInfo* (*describe)(void) = nullptr;
		int  (*load)(const WbshApi*) = nullptr;
		void (*unload)(void) = nullptr;
	};

	static bool findEntries(HMODULE module, UtilEntries& out_entries) {
		out_entries.describe = reinterpret_cast<const WbshUtilInfo* (*)(void)>(
			reinterpret_cast<void*>(::GetProcAddress(module, "wbshUtilDescribe")));
		out_entries.load = reinterpret_cast<int (*)(const WbshApi*)>(
			reinterpret_cast<void*>(::GetProcAddress(module, "wbshUtilLoad")));
		out_entries.unload = reinterpret_cast<void (*)(void)>(
			reinterpret_cast<void*>(::GetProcAddress(module, "wbshUtilUnload")));

		return out_entries.describe != nullptr && out_entries.load != nullptr;
	}

	static bool nameAlreadyLoaded(const char* name) {
		if (name == nullptr) return false;

		for (const LoadedUtil& util : g_utils) {
			if (util.info->name != nullptr && std::strcmp(util.info->name, name) == 0) return true;
		}

		return false;
	}

	// The reason a DLL is refused is the one thing a developer needs from
	// this function, so each refusal is spelled out rather than folded
	// into a single "could not load".
	static const char* refusal(HMODULE module, const UtilEntries& entries,
			const WbshUtilInfo* info) {
		if (module == nullptr) return "cannot be loaded as a DLL";
		if (info == nullptr) return "not a wbsh util: no wbshUtilDescribe";
		if (info->abi != WBSH_SDK_ABI) return "built for a different SDK ABI";
		if (!nameIsUsable(info->name)) return "describes itself with an unusable name";
		if (nameAlreadyLoaded(info->name)) return "a util with that name is already loaded";
		if (entries.load == nullptr) return "not a wbsh util: no wbshUtilLoad";

		return nullptr;
	}

	static std::string abiDetail(const WbshUtilInfo* info) {
		if (info == nullptr || info->abi == WBSH_SDK_ABI) return std::string();

		return " (util " + std::to_string(info->abi) + ", host "
			+ std::to_string(WBSH_SDK_ABI) + ")";
	}

	static bool loadOne(const std::wstring& path, WbshLogFn log, void* context) {
		const std::string shown = narrow(path.c_str());

		const HMODULE module = ::LoadLibraryW(path.c_str());
		UtilEntries entries;
		if (module != nullptr) findEntries(module, entries);
		const WbshUtilInfo* info = entries.describe != nullptr ? entries.describe() : nullptr;

		const char* why = refusal(module, entries, info);
		if (why != nullptr) {
			report(log, context, shown + ": " + why + abiDetail(info));
			if (module != nullptr) ::FreeLibrary(module);
			return false;
		}

		const int answer = entries.load(&g_api);
		if (answer != WBSH_OK) {
			report(log, context, shown + ": refused to load (returned "
				+ std::to_string(answer) + ")");
			if (entries.unload != nullptr) entries.unload();
			::FreeLibrary(module);
			return false;
		}

		LoadedUtil util;
		util.module = module;
		util.info   = info;
		util.path   = path;
		util.unload = entries.unload;
		g_utils.push_back(util);
		return true;
	}

} /* namespace wbshsdk_detail */

using namespace wbshsdk_detail;

extern "C" {

int wbshSdkBindHost(const WbshHostApi* api) {
	if (api == nullptr) return WBSH_ERR_NO_HOST;
	if (api->abi != WBSH_SDK_ABI) return WBSH_ERR_ABI;
	if (api->size < offsetof(WbshHostApi, register_command)) return WBSH_ERR_ABI;

	adoptHost(api);
	return WBSH_OK;
}

// A missing plugins folder is the ordinary case, not a failure: most
// installs have no utils, and saying so every time would be noise.
int wbshSdkLoadDirectory(const wchar_t* directory, WbshLogFn log, void* context) {
	if (!g_bound) return WBSH_ERR_NO_HOST;
	if (directory == nullptr || *directory == L'\0') return WBSH_ERR_BAD_NAME;

	const std::wstring folder(directory);
	WIN32_FIND_DATAW found{};
	const HANDLE search = ::FindFirstFileW((folder + L"\\*.dll").c_str(), &found);
	if (search == INVALID_HANDLE_VALUE) return 0;

	int loaded = 0;
	do {
		if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
		if (::lstrcmpiW(found.cFileName, L"wbshsdk.dll") == 0) continue;

		if (loadOne(folder + L"\\" + found.cFileName, log, context)) ++loaded;
	} while (::FindNextFileW(search, &found) != 0);

	::FindClose(search);
	return loaded;
}

int wbshSdkLoadFile(const wchar_t* path, WbshLogFn log, void* context) {
	if (!g_bound) return WBSH_ERR_NO_HOST;
	if (path == nullptr || *path == L'\0') return WBSH_ERR_BAD_NAME;

	return loadOne(path, log, context) ? 1 : 0;
}

// Callbacks registered by a util point into its DLL, so a host has to have
// dropped every one of them before this runs or the next call goes into
// freed pages.
void wbshSdkUnloadAll(void) {
	for (std::size_t left = g_utils.size(); left > 0; --left) {
		LoadedUtil& util = g_utils[left - 1];
		if (util.unload != nullptr) util.unload();

		::FreeLibrary(util.module);
	}

	g_utils.clear();
}

int wbshSdkCount(void) {
	return static_cast<int>(g_utils.size());
}

const WbshUtilInfo* wbshSdkInfoAt(int index) {
	if (index < 0 || index >= wbshSdkCount()) return nullptr;

	return g_utils[static_cast<std::size_t>(index)].info;
}

const wchar_t* wbshSdkPathAt(int index) {
	if (index < 0 || index >= wbshSdkCount()) return nullptr;

	return g_utils[static_cast<std::size_t>(index)].path.c_str();
}

}  /* extern "C" */

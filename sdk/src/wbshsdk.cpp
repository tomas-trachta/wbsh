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
		void               (*unload)(void) = nullptr;
	};

	static WbshHostApi             g_host;
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
	}

	struct UtilEntries {
		const WbshUtilInfo* (*describe)(void) = nullptr;
		int  (*load)(WbshHostKind) = nullptr;
		void (*unload)(void) = nullptr;
	};

	static bool findEntries(HMODULE module, UtilEntries& out_entries) {
		out_entries.describe = reinterpret_cast<const WbshUtilInfo* (*)(void)>(
			reinterpret_cast<void*>(::GetProcAddress(module, "wbshUtilDescribe")));
		out_entries.load = reinterpret_cast<int (*)(WbshHostKind)>(
			reinterpret_cast<void*>(::GetProcAddress(module, "wbshUtilLoad")));
		out_entries.unload = reinterpret_cast<void (*)(void)>(
			reinterpret_cast<void*>(::GetProcAddress(module, "wbshUtilUnload")));

		return out_entries.describe != nullptr && out_entries.load != nullptr;
	}

	static bool loadOne(const std::wstring& path, WbshLogFn log, void* context) {
		const std::string shown = narrow(path.c_str());

		const HMODULE module = ::LoadLibraryW(path.c_str());
		if (module == nullptr) {
			report(log, context, shown + ": cannot be loaded");
			return false;
		}

		UtilEntries entries;
		const WbshUtilInfo* info = findEntries(module, entries) ? entries.describe() : nullptr;

		if (info == nullptr) {
			report(log, context, shown + ": not a wbsh util");
			::FreeLibrary(module);
			return false;
		}

		if (info->abi != WBSH_SDK_ABI) {
			report(log, context, shown + ": built for a different SDK");
			::FreeLibrary(module);
			return false;
		}

		if (entries.load(g_host.kind) != WBSH_OK) {
			report(log, context, shown + ": refused to load");
			if (entries.unload != nullptr) entries.unload();
			::FreeLibrary(module);
			return false;
		}

		LoadedUtil util;
		util.module = module;
		util.info   = info;
		util.unload = entries.unload;
		g_utils.push_back(util);
		return true;
	}

} /* namespace wbshsdk_detail */

using namespace wbshsdk_detail;

extern "C" {

WbshHostKind wbshHost(void) {
	return g_bound ? g_host.kind : WBSH_HOST_NONE;
}

const char* wbshSdkVersion(void) {
	return WBSH_SDK_VERSION_TEXT;
}

int wbshRegisterCommand(const char* name, WbshCommandFn fn, void* user) {
	if (!g_bound) return WBSH_ERR_NO_HOST;
	if (!nameIsUsable(name) || fn == nullptr) return WBSH_ERR_BAD_NAME;
	if (g_host.register_command == nullptr) return WBSH_ERR_UNSUPPORTED;

	return g_host.register_command(g_host.context, name, fn, user);
}

int wbshRegisterStatusSegment(const char* name, WbshSegmentFn fn, void* user) {
	if (!g_bound) return WBSH_ERR_NO_HOST;
	if (!nameIsUsable(name) || fn == nullptr) return WBSH_ERR_BAD_NAME;
	if (g_host.register_segment == nullptr) return WBSH_ERR_UNSUPPORTED;

	return g_host.register_segment(g_host.context, name, fn, user);
}

void wbshWriteOut(const char* bytes, size_t length) {
	if (!g_bound || g_host.write_out == nullptr || bytes == nullptr) return;

	g_host.write_out(g_host.context, bytes, length);
}

void wbshWriteErr(const char* bytes, size_t length) {
	if (!g_bound || g_host.write_err == nullptr || bytes == nullptr) return;

	g_host.write_err(g_host.context, bytes, length);
}

void wbshPrint(const char* text) {
	if (text == nullptr) return;

	wbshWriteOut(text, std::strlen(text));
}

void wbshPrintError(const char* text) {
	if (text == nullptr) return;

	wbshWriteErr(text, std::strlen(text));
}

const char* wbshWorkingDirectory(void) {
	if (!g_bound || g_host.working_directory == nullptr) return nullptr;

	return g_host.working_directory(g_host.context);
}

const char* wbshVariable(const char* name) {
	if (!g_bound || g_host.variable == nullptr || name == nullptr) return nullptr;

	return g_host.variable(g_host.context, name);
}

}  /* extern "C" */

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

}  /* extern "C" */

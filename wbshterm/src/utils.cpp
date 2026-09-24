/**
 * @file utils.cpp
 * @brief Binding the terminal to the SDK, and the segments utils add.
 *
 * Reached through LoadLibrary rather than linked, so wbshterm still runs
 * with no SDK beside it; no SDK simply means no utils.
 */

#include "utils.h"

#include "wbshsdk.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif /* WIN32_LEAN_AND_MEAN */
#include <windows.h>

#include <cstdlib>
#include <vector>

namespace wbshterm {

	namespace utils_detail {

		struct Segment {
			std::string name;
			WbshSegmentFn fn = nullptr;
			void* user = nullptr;
		};

		static std::vector<Segment> g_segments;
		static std::string          g_scratch;
		static int                  g_loaded = 0;

		static int registerSegment(void*, const char* name, WbshSegmentFn fn, void* user) {
			for (const Segment& segment : g_segments) {
				if (segment.name == name) return WBSH_ERR_TAKEN;
			}

			Segment added;
			added.name = name;
			added.fn   = fn;
			added.user = user;
			g_segments.push_back(added);
			return WBSH_OK;
		}

		// The terminal has no stdout of its own -- a pane's output belongs
		// to the shell running in it -- so a util that prints here is
		// printing nowhere, and says so by finding no writer at all.
		static const char* workingDirectory(void*) {
			wchar_t here[MAX_PATH] = {};
			const DWORD length = ::GetCurrentDirectoryW(MAX_PATH, here);
			if (length == 0) return nullptr;

			const int needed = ::WideCharToMultiByte(CP_UTF8, 0, here,
				static_cast<int>(length), nullptr, 0, nullptr, nullptr);
			if (needed <= 0) return nullptr;

			g_scratch.assign(static_cast<std::size_t>(needed), '\0');
			::WideCharToMultiByte(CP_UTF8, 0, here, static_cast<int>(length),
				g_scratch.data(), needed, nullptr, nullptr);
			return g_scratch.c_str();
		}

		static const char* variable(void*, const char* name) {
			if (name == nullptr) return nullptr;

			const char* value = std::getenv(name);
			if (value == nullptr) return nullptr;

			g_scratch = value;
			return g_scratch.c_str();
		}

		static void complain(void*, const char*) {
		}

		static std::wstring directoryOfThisExe() {
			wchar_t path[MAX_PATH] = {};
			const DWORD length = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
			const std::wstring full(path, length);

			const std::size_t cut = full.find_last_of(L'\\');
			return cut == std::wstring::npos ? std::wstring() : full.substr(0, cut + 1);
		}

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
			int (*bind)(const WbshHostApi*) = nullptr;
			int (*load)(const wchar_t*, WbshLogFn, void*) = nullptr;
			int (*count)(void) = nullptr;
		};

		static bool findSdk(HMODULE sdk, SdkEntries& out_entries) {
			out_entries.bind = reinterpret_cast<int (*)(const WbshHostApi*)>(
				reinterpret_cast<void*>(::GetProcAddress(sdk, "wbshSdkBindHost")));
			out_entries.load = reinterpret_cast<int (*)(const wchar_t*, WbshLogFn, void*)>(
				reinterpret_cast<void*>(::GetProcAddress(sdk, "wbshSdkLoadDirectory")));
			out_entries.count = reinterpret_cast<int (*)(void)>(
				reinterpret_cast<void*>(::GetProcAddress(sdk, "wbshSdkCount")));

			return out_entries.bind != nullptr && out_entries.load != nullptr
				&& out_entries.count != nullptr;
		}

		static WbshHostApi terminalApi() {
			WbshHostApi api{};
			api.size    = static_cast<uint32_t>(sizeof(api));
			api.abi     = WBSH_SDK_ABI;
			api.kind    = WBSH_HOST_TERMINAL;
			api.context = nullptr;

			api.register_command  = nullptr;
			api.register_segment  = registerSegment;
			api.write_out         = nullptr;
			api.write_err         = nullptr;
			api.working_directory = workingDirectory;
			api.variable          = variable;
			return api;
		}

	}  /* namespace utils_detail */

	int loadedUtilCount() {
		return utils_detail::g_loaded;
	}

	void loadUtils() {
		const std::wstring here = utils_detail::directoryOfThisExe();
		const HMODULE sdk = ::LoadLibraryW((here + L"wbshsdk.dll").c_str());
		if (sdk == nullptr) return;

		utils_detail::SdkEntries entries;
		if (!utils_detail::findSdk(sdk, entries)) return;

		const WbshHostApi api = utils_detail::terminalApi();
		if (entries.bind(&api) != WBSH_OK) return;

		entries.load((here + L"plugins").c_str(), utils_detail::complain, nullptr);

		const std::wstring mine = utils_detail::userPluginDirectory();
		if (!mine.empty()) entries.load(mine.c_str(), utils_detail::complain, nullptr);

		utils_detail::g_loaded = entries.count();
	}

	// A segment that answers nothing is left out rather than shown as a
	// gap, so a util can go quiet without leaving a hole in the bar.
	std::string utilSegmentText() {
		std::string text;

		for (const utils_detail::Segment& segment : utils_detail::g_segments) {
			const char* shown = segment.fn != nullptr ? segment.fn(segment.user) : nullptr;
			if (shown == nullptr || *shown == '\0') continue;

			if (!text.empty()) text += "  ";
			text += shown;
		}

		return text;
	}

}  /* namespace wbshterm */

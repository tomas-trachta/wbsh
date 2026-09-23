/**
 * @file fetch.cpp
 * @brief Gathering the facts, and laying them out beside the logo.
 */

#include "fetch.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif /* WIN32_LEAN_AND_MEAN */
#include <windows.h>

#include <lmcons.h>

#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "version.lib")

namespace wbshterm {

	static const char* kLogo[] = {
		"  ██     ██ ██████  ███████ ██   ██",
		"  ██     ██ ██   ██ ██      ██   ██",
		"  ██  █  ██ ██████  ███████ ███████",
		"  ██ ███ ██ ██   ██      ██ ██   ██",
		"   ███ ███  ██████  ███████ ██   ██",
	};

	static const std::size_t kLogoWidth = 36;

	static std::string narrowText(const std::wstring& text) {
		if (text.empty()) return std::string();

		const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
			static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
		std::string out(static_cast<std::size_t>(needed), '\0');
		::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
			out.data(), needed, nullptr, nullptr);
		return out;
	}

	static std::string registryText(HKEY root, const wchar_t* path, const wchar_t* name) {
		wchar_t value[256] = {};
		DWORD size = sizeof(value);
		const LSTATUS status =
			::RegGetValueW(root, path, name, RRF_RT_REG_SZ, nullptr, value, &size);
		if (status != ERROR_SUCCESS) {
			return std::string();
		}

		std::string text = narrowText(value);
		while (!text.empty() && text.back() == ' ') text.pop_back();
		return text;
	}

	static std::string windowsName() {
		const std::string product = registryText(HKEY_LOCAL_MACHINE,
			L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ProductName");
		const std::string build = registryText(HKEY_LOCAL_MACHINE,
			L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuildNumber");

		if (product.empty()) return "Windows";
		if (build.empty()) return product;

		// Windows 11 still calls itself 10 in the registry; the build says which.
		const bool eleven = std::stoi(build) >= 22000;
		const std::string name = eleven && product.find("Windows 10") != std::string::npos
			? "Windows 11" + product.substr(10)
			: product;
		return name + " (build " + build + ")";
	}

	static std::string uptimeText() {
		const unsigned long long seconds = ::GetTickCount64() / 1000;
		const unsigned long long days = seconds / 86400;
		const unsigned long long hours = (seconds % 86400) / 3600;
		const unsigned long long minutes = (seconds % 3600) / 60;

		std::string text;
		if (days > 0) text += std::to_string(days) + "d ";
		if (days > 0 || hours > 0) text += std::to_string(hours) + "h ";
		text += std::to_string(minutes) + "m";
		return text;
	}

	static std::string memoryText() {
		MEMORYSTATUSEX status{};
		status.dwLength = sizeof(status);
		if (!::GlobalMemoryStatusEx(&status)) return std::string();

		const unsigned long long megabyte = 1024ull * 1024ull;
		const unsigned long long total = status.ullTotalPhys / megabyte;
		const unsigned long long used = total - (status.ullAvailPhys / megabyte);
		return std::to_string(used) + "MiB / " + std::to_string(total) + "MiB";
	}

	static std::string resolutionText() {
		const int width = ::GetSystemMetrics(SM_CXSCREEN);
		const int height = ::GetSystemMetrics(SM_CYSCREEN);
		if (width == 0 || height == 0) return std::string();

		return std::to_string(width) + "x" + std::to_string(height);
	}

	static std::string userName() {
		wchar_t name[UNLEN + 1] = {};
		DWORD size = UNLEN + 1;
		if (!::GetUserNameW(name, &size)) return "user";
		return narrowText(name);
	}

	static std::string hostName() {
		wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
		DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
		if (!::GetComputerNameW(name, &size)) return "windows";
		return narrowText(name);
	}

	static std::wstring executableDirectory() {
		wchar_t path[MAX_PATH] = {};
		const DWORD length = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
		const std::wstring full(path, length);

		const std::size_t cut = full.find_last_of(L'\\');
		return cut == std::wstring::npos ? std::wstring() : full.substr(0, cut + 1);
	}

	// The shell's version lives in its own file resource, so the terminal can
	// report it without running it.
	static std::wstring locateShellBinary() {
		const std::wstring here = executableDirectory();
		const std::wstring candidates[] = {
			here + L"wbsh.exe",
			here + L"..\\..\\x64\\Release\\wbsh.exe",
			here + L"..\\..\\x64\\Debug\\wbsh.exe",
		};

		for (const std::wstring& candidate : candidates) {
			if (::GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
				return candidate;
			}
		}

		wchar_t found[MAX_PATH] = {};
		if (::SearchPathW(nullptr, L"wbsh.exe", nullptr, MAX_PATH, found, nullptr) != 0) {
			return found;
		}

		return std::wstring();
	}

	static std::string shellVersion() {
		const std::wstring path = locateShellBinary();
		if (path.empty()) return "wbsh";

		DWORD ignored = 0;
		const DWORD size = ::GetFileVersionInfoSizeW(path.c_str(), &ignored);
		if (size == 0) return "wbsh";

		std::vector<char> block(size);
		if (!::GetFileVersionInfoW(path.c_str(), 0, size, block.data())) return "wbsh";

		VS_FIXEDFILEINFO* info = nullptr;
		UINT info_size = 0;
		if (!::VerQueryValueW(block.data(), L"\\", reinterpret_cast<void**>(&info), &info_size)
			|| info == nullptr) {
			return "wbsh";
		}

		return "wbsh " + std::to_string(HIWORD(info->dwFileVersionMS)) + "."
			+ std::to_string(LOWORD(info->dwFileVersionMS)) + "."
			+ std::to_string(HIWORD(info->dwFileVersionLS));
	}

	static std::string fontText(const Config& config) {
		return narrowText(config.font.family) + " "
			+ std::to_string(static_cast<int>(config.font.size)) + "pt";
	}

	FetchInfo gatherFetchInfo(const Config& config) {
		FetchInfo info;
		info.user = userName();
		info.host = hostName();

		info.rows.push_back({ "OS", windowsName() });
		info.rows.push_back({ "Uptime", uptimeText() });
		info.rows.push_back({ "Shell", shellVersion() });
		info.rows.push_back({ "Terminal", "wbshterm" });
		info.rows.push_back({ "CPU", registryText(HKEY_LOCAL_MACHINE,
			L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString") });
		info.rows.push_back({ "Memory", memoryText() });
		info.rows.push_back({ "Display", resolutionText() });
		info.rows.push_back({ "Theme", config.theme_name });
		info.rows.push_back({ "Font", fontText(config) });

		return info;
	}

	static std::string paletteStrip() {
		std::string first;
		std::string second;

		for (int color = 0; color < 8; ++color) {
			first += "\x1b[4" + std::to_string(color) + "m   ";
			second += "\x1b[10" + std::to_string(color) + "m   ";
		}

		return first + "\x1b[0m\n" + second + "\x1b[0m\n";
	}

	static std::string logoLine(std::size_t index) {
		const std::size_t count = sizeof(kLogo) / sizeof(kLogo[0]);
		if (index >= count) return std::string(kLogoWidth, ' ');

		return "\x1b[36;1m" + std::string(kLogo[index]) + "\x1b[0m";
	}

	static std::string headerLine(const FetchInfo& info) {
		return "\x1b[32;1m" + info.user + "\x1b[0m@\x1b[32;1m" + info.host + "\x1b[0m";
	}

	// Rows and logo lines are zipped together, whichever runs out first
	// padded, so the panel keeps its shape as either side changes.
	std::string renderFetchPanel(const FetchInfo& info) {
		std::vector<std::string> right;
		right.push_back(headerLine(info));
		right.push_back("\x1b[2m" + std::string(info.user.size() + info.host.size() + 1, '-')
			+ "\x1b[0m");

		for (const FetchRow& row : info.rows) {
			if (row.value.empty()) continue;
			right.push_back("\x1b[33;1m" + row.label + "\x1b[0m: " + row.value);
		}

		const std::size_t logo_lines = sizeof(kLogo) / sizeof(kLogo[0]);
		const std::size_t lines = right.size() > logo_lines ? right.size() : logo_lines;

		std::string panel = "\n";
		for (std::size_t i = 0; i < lines; ++i) {
			panel += logoLine(i);
			panel += "  ";
			panel += i < right.size() ? right[i] : std::string();
			panel += "\n";
		}

		panel += "\n" + paletteStrip();
		return panel;
	}

	static bool writeAll(HANDLE output, const std::string& text) {
		if (output == nullptr || output == INVALID_HANDLE_VALUE) return false;

		DWORD written = 0;
		if (!::WriteFile(output, text.data(), static_cast<DWORD>(text.size()), &written, nullptr)) {
			return false;
		}

		return written == text.size();
	}

	// A window-subsystem process is not attached to the console its parent
	// hands it, so the standard handles it inherited mean nothing here: a
	// redirect to a file works, a write to the pseudoconsole quietly does
	// not. Joining the parent's console and writing to CONOUT$ does.
	static bool writeToParentConsole(const std::string& text) {
		if (!::AttachConsole(ATTACH_PARENT_PROCESS)) return false;

		const HANDLE console = ::CreateFileW(L"CONOUT$", GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);

		DWORD mode = 0;
		if (::GetConsoleMode(console, &mode)) {
			::SetConsoleMode(console, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
		}

		const bool written = writeAll(console, text);
		if (console != INVALID_HANDLE_VALUE) ::CloseHandle(console);

		::FreeConsole();
		return written;
	}

	int printFetchPanel(const Config& config) {
		const std::string panel = renderFetchPanel(gatherFetchInfo(config));

		if (writeAll(::GetStdHandle(STD_OUTPUT_HANDLE), panel)) return 0;
		return writeToParentConsole(panel) ? 0 : 1;
	}

} /* namespace wbshterm */

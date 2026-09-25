/**
 * @file sysinfo.cpp
 * @brief Reading the machine, and putting the reading into words.
 */

#include "sysinfo.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif /* WIN32_LEAN_AND_MEAN */
#include <windows.h>

#include <lmcons.h>

#include <cstdio>

namespace wbshterm {

	static const unsigned long long kMegabyte = 1024ull * 1024ull;
	static const unsigned long long kGigabyte = kMegabyte * 1024ull;

	static unsigned long long fileTimeValue(const FILETIME& time) {
		return (static_cast<unsigned long long>(time.dwHighDateTime) << 32)
			| time.dwLowDateTime;
	}

	int SystemMonitor::cpuPercentSince(unsigned long long idle, unsigned long long total) {
		const unsigned long long idle_delta  = idle - last_idle_;
		const unsigned long long total_delta = total - last_total_;
		const bool ready = primed_ && total_delta > 0;

		last_idle_  = idle;
		last_total_ = total;
		primed_     = true;

		if (!ready) return -1;

		return static_cast<int>(100 - (idle_delta * 100) / total_delta);
	}

	// Kernel time already includes idle time, so the sum is the whole clock.
	void SystemMonitor::readCpu(SystemSample& sample) {
		FILETIME idle{};
		FILETIME kernel{};
		FILETIME user{};
		if (::GetSystemTimes(&idle, &kernel, &user) == 0) return;

		const unsigned long long idle_value = fileTimeValue(idle);
		const unsigned long long total = fileTimeValue(kernel) + fileTimeValue(user);
		sample.cpu_percent = cpuPercentSince(idle_value, total);
	}

	static void readMemory(SystemSample& sample) {
		MEMORYSTATUSEX status{};
		status.dwLength = sizeof(status);
		if (::GlobalMemoryStatusEx(&status) == 0) return;

		sample.memory_total_mb = status.ullTotalPhys / kMegabyte;
		sample.memory_used_mb  = sample.memory_total_mb - status.ullAvailPhys / kMegabyte;
	}

	static char systemDriveLetter() {
		wchar_t path[MAX_PATH] = {};
		if (::GetWindowsDirectoryW(path, MAX_PATH) == 0 || path[1] != L':') return 'C';

		return static_cast<char>(path[0]);
	}

	static void readDisk(SystemSample& sample) {
		const char letter = systemDriveLetter();
		const wchar_t root[] = { static_cast<wchar_t>(letter), L':', L'\\', L'\0' };

		ULARGE_INTEGER free_bytes{};
		ULARGE_INTEGER total_bytes{};
		if (::GetDiskFreeSpaceExW(root, &free_bytes, &total_bytes, nullptr) == 0) return;

		sample.disk_letter   = letter;
		sample.disk_free_gb  = free_bytes.QuadPart / kGigabyte;
		sample.disk_total_gb = total_bytes.QuadPart / kGigabyte;
	}

	static const BYTE kNoBattery      = 128;
	static const BYTE kUnknownPercent = 255;

	static void readBattery(SystemSample& sample) {
		SYSTEM_POWER_STATUS power{};
		if (::GetSystemPowerStatus(&power) == 0) return;
		if ((power.BatteryFlag & kNoBattery) != 0) return;
		if (power.BatteryLifePercent == kUnknownPercent) return;

		sample.battery_percent = power.BatteryLifePercent;
		sample.on_mains        = power.ACLineStatus == 1;
	}

	void SystemMonitor::sample() {
		SystemSample fresh;
		readCpu(fresh);
		readMemory(fresh);
		readDisk(fresh);
		readBattery(fresh);
		sample_ = fresh;
	}

	static std::string gigabytes(unsigned long long megabytes) {
		char text[32] = {};
		std::snprintf(text, sizeof(text), "%.1fG", static_cast<double>(megabytes) / 1024.0);
		return text;
	}

	static void appendPart(std::string& text, const std::string& part) {
		if (part.empty()) return;
		if (!text.empty()) text += "  ";

		text += part;
	}

	static std::string cpuPart(const SystemSample& sample) {
		if (sample.cpu_percent < 0) return std::string();

		return "cpu " + std::to_string(sample.cpu_percent) + "%";
	}

	static std::string memoryPart(const SystemSample& sample) {
		if (sample.memory_total_mb == 0) return std::string();

		return "mem " + gigabytes(sample.memory_used_mb) + "/" + gigabytes(sample.memory_total_mb);
	}

	static std::string diskPart(const SystemSample& sample) {
		if (sample.disk_letter == 0) return std::string();

		return std::string(1, sample.disk_letter) + ": " + std::to_string(sample.disk_free_gb)
			+ "G free";
	}

	static std::string batteryPart(const SystemSample& sample) {
		if (sample.battery_percent < 0) return std::string();

		return "bat " + std::to_string(sample.battery_percent) + "%"
			+ (sample.on_mains ? "+" : "");
	}

	std::string systemInfoText(const SystemSample& sample, const StatusBarSettings& settings) {
		std::string text;
		if (settings.cpu)     appendPart(text, cpuPart(sample));
		if (settings.memory)  appendPart(text, memoryPart(sample));
		if (settings.disk)    appendPart(text, diskPart(sample));
		if (settings.battery) appendPart(text, batteryPart(sample));
		return text;
	}

	static std::string narrowName(const wchar_t* name, DWORD length) {
		const int needed = ::WideCharToMultiByte(CP_UTF8, 0, name, static_cast<int>(length),
			nullptr, 0, nullptr, nullptr);
		if (needed <= 0) return std::string();

		std::string narrow(static_cast<std::size_t>(needed), '\0');
		::WideCharToMultiByte(CP_UTF8, 0, name, static_cast<int>(length), narrow.data(), needed,
			nullptr, nullptr);
		return narrow;
	}

	std::string userName() {
		wchar_t name[UNLEN + 1] = {};
		DWORD size = UNLEN + 1;
		if (::GetUserNameW(name, &size) == 0 || size == 0) return "user";

		return narrowName(name, size - 1);
	}

	std::string hostName() {
		wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
		DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
		if (::GetComputerNameW(name, &size) == 0) return "windows";

		return narrowName(name, size);
	}

} /* namespace wbshterm */

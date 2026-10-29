/**
 * @file procfs_text.cpp
 * @brief The texts of the /proc files, laid out as Linux lays them out.
 */

#include "procfs_internal.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <io.h>
#include <intrin.h>
#include <psapi.h>
#include <shellapi.h>
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shell32.lib")

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>
#include <string>
#include <vector>

#include "coreutils_internal.h"
#include "pathconv.h"

namespace wbsh::procfs_detail {

	struct CpuFlag {
		unsigned leaf;
		int reg;
		int bit;
		const char* name;
	};

	struct CpuIdentity {
		std::string vendor;
		std::string model_name;
		int family = 0;
		int model = 0;
		int stepping = 0;
		std::string flags;
	};

	struct CpuPlace {
		int package = 0;
		int core = 0;
	};

	struct CpuTopology {
		std::vector<CpuPlace> places;
		std::vector<int> siblings;
		std::vector<int> cores;
	};

	static const int kEax = 0;
	static const int kEbx = 1;
	static const int kEcx = 2;
	static const int kEdx = 3;
	static const unsigned kExtendedLeaf = 0x80000000u;
	static const unsigned kExtendedFeatures = 0x80000001u;
	static const int kProbedDescriptors = 1024;
	static const unsigned long long kKiB = 1024;

	// Linux's names, from the CPUID bits it reads them from.
	static const CpuFlag kCpuFlags[] = {
		{ 1, kEdx, 0, "fpu" },   { 1, kEdx, 1, "vme" },     { 1, kEdx, 2, "de" },
		{ 1, kEdx, 3, "pse" },   { 1, kEdx, 4, "tsc" },     { 1, kEdx, 5, "msr" },
		{ 1, kEdx, 6, "pae" },   { 1, kEdx, 7, "mce" },     { 1, kEdx, 8, "cx8" },
		{ 1, kEdx, 9, "apic" },  { 1, kEdx, 11, "sep" },    { 1, kEdx, 12, "mtrr" },
		{ 1, kEdx, 13, "pge" },  { 1, kEdx, 14, "mca" },    { 1, kEdx, 15, "cmov" },
		{ 1, kEdx, 16, "pat" },  { 1, kEdx, 17, "pse36" },  { 1, kEdx, 19, "clflush" },
		{ 1, kEdx, 23, "mmx" },  { 1, kEdx, 24, "fxsr" },   { 1, kEdx, 25, "sse" },
		{ 1, kEdx, 26, "sse2" }, { 1, kEdx, 28, "ht" },
		{ kExtendedFeatures, kEdx, 11, "syscall" }, { kExtendedFeatures, kEdx, 20, "nx" },
		{ kExtendedFeatures, kEdx, 26, "pdpe1gb" }, { kExtendedFeatures, kEdx, 27, "rdtscp" },
		{ kExtendedFeatures, kEdx, 29, "lm" },
		{ 1, kEcx, 0, "pni" },     { 1, kEcx, 1, "pclmulqdq" }, { 1, kEcx, 9, "ssse3" },
		{ 1, kEcx, 12, "fma" },    { 1, kEcx, 13, "cx16" },     { 1, kEcx, 19, "sse4_1" },
		{ 1, kEcx, 20, "sse4_2" }, { 1, kEcx, 22, "movbe" },    { 1, kEcx, 23, "popcnt" },
		{ 1, kEcx, 25, "aes" },    { 1, kEcx, 26, "xsave" },    { 1, kEcx, 28, "avx" },
		{ 1, kEcx, 29, "f16c" },   { 1, kEcx, 30, "rdrand" },   { 1, kEcx, 31, "hypervisor" },
		{ kExtendedFeatures, kEcx, 0, "lahf_lm" }, { kExtendedFeatures, kEcx, 5, "abm" },
		{ kExtendedFeatures, kEcx, 6, "sse4a" },   { kExtendedFeatures, kEcx, 8, "3dnowprefetch" },
		{ kExtendedFeatures, kEcx, 16, "fma4" },
		{ 7, kEbx, 0, "fsgsbase" },    { 7, kEbx, 3, "bmi1" },        { 7, kEbx, 4, "hle" },
		{ 7, kEbx, 5, "avx2" },        { 7, kEbx, 7, "smep" },        { 7, kEbx, 8, "bmi2" },
		{ 7, kEbx, 9, "erms" },        { 7, kEbx, 10, "invpcid" },    { 7, kEbx, 11, "rtm" },
		{ 7, kEbx, 16, "avx512f" },    { 7, kEbx, 17, "avx512dq" },   { 7, kEbx, 18, "rdseed" },
		{ 7, kEbx, 19, "adx" },        { 7, kEbx, 20, "smap" },       { 7, kEbx, 21, "avx512ifma" },
		{ 7, kEbx, 23, "clflushopt" }, { 7, kEbx, 24, "clwb" },       { 7, kEbx, 28, "avx512cd" },
		{ 7, kEbx, 29, "sha_ni" },     { 7, kEbx, 30, "avx512bw" },   { 7, kEbx, 31, "avx512vl" },
		{ 7, kEcx, 1, "avx512vbmi" },  { 7, kEcx, 2, "umip" },        { 7, kEcx, 3, "pku" },
		{ 7, kEcx, 8, "gfni" },        { 7, kEcx, 9, "vaes" },        { 7, kEcx, 10, "vpclmulqdq" },
		{ 7, kEcx, 11, "avx512_vnni" }, { 7, kEcx, 12, "avx512_bitalg" },
		{ 7, kEcx, 14, "avx512_vpopcntdq" }, { 7, kEcx, 22, "rdpid" },
	};

	static void cpuid(unsigned leaf, int registers[4]) {
		__cpuidex(registers, static_cast<int>(leaf), 0);
	}

	static unsigned highestLeaf(unsigned base) {
		int registers[4] = {};
		cpuid(base, registers);
		return static_cast<unsigned>(registers[kEax]);
	}

	static std::string registerText(const int* registers, std::size_t count) {
		std::string text(reinterpret_cast<const char*>(registers), count * sizeof(int));
		text.erase(std::find(text.begin(), text.end(), '\0'), text.end());
		return text;
	}

	static std::string trimmed(const std::string& text) {
		const std::size_t first = text.find_first_not_of(' ');
		if (first == std::string::npos) return std::string();
		return text.substr(first, text.find_last_not_of(' ') - first + 1);
	}

	static std::string brandString() {
		if (highestLeaf(kExtendedLeaf) < kExtendedLeaf + 4) return std::string();

		int brand[12] = {};
		for (unsigned i = 0; i < 3; ++i) cpuid(kExtendedLeaf + 2 + i, brand + i * 4);
		return trimmed(registerText(brand, 12));
	}

	static void readSignature(CpuIdentity& cpu) {
		int registers[4] = {};
		cpuid(0, registers);
		const int vendor[3] = { registers[kEbx], registers[kEdx], registers[kEcx] };
		cpu.vendor = registerText(vendor, 3);

		cpuid(1, registers);
		const int signature = registers[kEax];
		const int base_family = (signature >> 8) & 0xF;
		const int base_model = (signature >> 4) & 0xF;
		cpu.stepping = signature & 0xF;
		cpu.family = base_family == 0xF ? base_family + ((signature >> 20) & 0xFF) : base_family;
		const bool extended_model = base_family == 0x6 || base_family == 0xF;
		cpu.model = extended_model ? (((signature >> 16) & 0xF) << 4) + base_model : base_model;
	}

	static bool leafAvailable(unsigned leaf) {
		if (leaf >= kExtendedLeaf) return highestLeaf(kExtendedLeaf) >= leaf;
		return highestLeaf(0) >= leaf;
	}

	static std::string cpuFlags() {
		std::map<unsigned, std::vector<int>> leaves;
		std::string flags;
		for (const CpuFlag& flag : kCpuFlags) {
			if (!leafAvailable(flag.leaf)) continue;

			std::vector<int>& registers = leaves[flag.leaf];
			if (registers.empty()) {
				registers.assign(4, 0);
				cpuid(flag.leaf, registers.data());
			}

			if ((static_cast<unsigned>(registers[flag.reg]) & (1u << flag.bit)) == 0) continue;
			if (!flags.empty()) flags.push_back(' ');
			flags += flag.name;
		}

		return flags;
	}

	static CpuIdentity identifyCpu() {
		CpuIdentity cpu;
		readSignature(cpu);
		cpu.model_name = brandString();
		cpu.flags = cpuFlags();
		return cpu;
	}

	static DWORD processorMegahertz() {
		DWORD mhz = 0;
		DWORD size = sizeof(mhz);
		::RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
			L"~MHz", RRF_RT_REG_DWORD, nullptr, &mhz, &size);
		return mhz;
	}

	static std::vector<int> groupOffsets() {
		std::vector<int> offsets;
		int total = 0;
		const WORD groups = ::GetActiveProcessorGroupCount();
		for (WORD group = 0; group < groups; ++group) {
			offsets.push_back(total);
			total += static_cast<int>(::GetActiveProcessorCount(group));
		}

		return offsets;
	}

	static std::vector<int> maskMembers(const GROUP_AFFINITY& mask,
	                                    const std::vector<int>& offsets) {
		std::vector<int> members;
		if (mask.Group >= offsets.size()) return members;

		for (int bit = 0; bit < static_cast<int>(sizeof(KAFFINITY) * 8); ++bit) {
			if ((mask.Mask & (static_cast<KAFFINITY>(1) << bit)) != 0) {
				members.push_back(offsets[mask.Group] + bit);
			}
		}

		return members;
	}

	static std::vector<char> logicalProcessorInformation() {
		DWORD bytes = 0;
		::GetLogicalProcessorInformationEx(RelationAll, nullptr, &bytes);
		std::vector<char> buffer(bytes);
		auto* first = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());
		if (bytes == 0 || ::GetLogicalProcessorInformationEx(RelationAll, first, &bytes) == 0) {
			buffer.clear();
		}

		return buffer;
	}

	// Each processor package and core, as the lists of logical processors in it.
	static std::vector<std::vector<int>> relationMembers(const std::vector<char>& buffer,
	                                                     LOGICAL_PROCESSOR_RELATIONSHIP kind,
	                                                     int logical_count) {
		const std::vector<int> offsets = groupOffsets();
		std::vector<std::vector<int>> groups;
		for (std::size_t at = 0; at < buffer.size();) {
			const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
				buffer.data() + at);
			at += info->Size;
			if (info->Relationship != kind) continue;

			std::vector<int> members;
			for (const int logical : maskMembers(info->Processor.GroupMask[0], offsets)) {
				if (logical < logical_count) members.push_back(logical);
			}

			if (!members.empty()) groups.push_back(std::move(members));
		}

		return groups;
	}

	static void placePackages(CpuTopology& topology,
	                          const std::vector<std::vector<int>>& packages) {
		for (const std::vector<int>& members : packages) {
			const int package = static_cast<int>(topology.siblings.size());
			topology.siblings.push_back(static_cast<int>(members.size()));
			topology.cores.push_back(0);
			for (const int logical : members) topology.places[logical].package = package;
		}

		if (!topology.siblings.empty()) return;

		topology.siblings.push_back(static_cast<int>(topology.places.size()));
		topology.cores.push_back(0);
	}

	static void placeCores(CpuTopology& topology, const std::vector<std::vector<int>>& cores) {
		for (const std::vector<int>& members : cores) {
			const CpuPlace& first = topology.places[static_cast<std::size_t>(members[0])];
			const std::size_t package = static_cast<std::size_t>(first.package);
			const int core = topology.cores[package]++;
			for (const int logical : members) topology.places[logical].core = core;
		}
	}

	static CpuTopology cpuTopology(int logical_count) {
		CpuTopology topology;
		topology.places.resize(static_cast<std::size_t>(logical_count));

		const std::vector<char> buffer = logicalProcessorInformation();
		placePackages(topology, relationMembers(buffer, RelationProcessorPackage, logical_count));
		placeCores(topology, relationMembers(buffer, RelationProcessorCore, logical_count));
		return topology;
	}

	static std::string formatted(const char* format, ...) {
		char text[512];
		va_list args;
		va_start(args, format);
		std::vsnprintf(text, sizeof(text), format, args);
		va_end(args);
		return text;
	}

	static std::string processorBlock(int processor, const CpuIdentity& cpu, DWORD mhz,
	                                  const CpuTopology& topology) {
		const CpuPlace& place = topology.places[static_cast<std::size_t>(processor)];
		const std::size_t package = static_cast<std::size_t>(place.package);
		std::string block;
		block += formatted("processor\t: %d\n", processor);
		block += "vendor_id\t: " + cpu.vendor + "\n";
		block += formatted("cpu family\t: %d\nmodel\t\t: %d\n", cpu.family, cpu.model);
		block += "model name\t: " + cpu.model_name + "\n";
		block += formatted("stepping\t: %d\ncpu MHz\t\t: %lu.000\n", cpu.stepping, mhz);
		block += formatted("physical id\t: %d\nsiblings\t: %d\n", place.package,
			topology.siblings[package]);
		block += formatted("core id\t\t: %d\ncpu cores\t: %d\n", place.core,
			topology.cores[package]);
		block += "flags\t\t: " + cpu.flags + "\n\n";
		return block;
	}

	std::string cpuInfoText() {
		const int count = static_cast<int>(::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
		const CpuIdentity cpu = identifyCpu();
		const DWORD mhz = processorMegahertz();
		const CpuTopology topology = cpuTopology(count);

		std::string text;
		for (int processor = 0; processor < count; ++processor) {
			text += processorBlock(processor, cpu, mhz, topology);
		}

		return text;
	}

	static std::string memLine(const char* label, unsigned long long bytes) {
		return formatted("%-16s%8llu kB\n", label, bytes / kKiB);
	}

	std::string memInfoText() {
		MEMORYSTATUSEX status{};
		status.dwLength = sizeof(status);
		::GlobalMemoryStatusEx(&status);

		PERFORMANCE_INFORMATION performance{};
		::GetPerformanceInfo(&performance, sizeof(performance));
		const unsigned long long page = performance.PageSize;

		unsigned long long swap_total = 0;
		unsigned long long swap_free = 0;
		pageFileUsage(swap_total, swap_free);

		std::string text;
		text += memLine("MemTotal:", status.ullTotalPhys);
		text += memLine("MemFree:", status.ullAvailPhys);
		text += memLine("MemAvailable:", status.ullAvailPhys);
		text += memLine("Buffers:", 0);
		text += memLine("Cached:", performance.SystemCache * page);
		text += memLine("SwapCached:", 0);
		text += memLine("SwapTotal:", swap_total);
		text += memLine("SwapFree:", swap_free);
		text += memLine("CommitLimit:", performance.CommitLimit * page);
		text += memLine("Committed_AS:", performance.CommitTotal * page);
		return text;
	}

	static double filetimeSeconds(const FILETIME& time) {
		const unsigned long long ticks =
			(static_cast<unsigned long long>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
		return static_cast<double>(ticks) / 1e7;
	}

	// The second field is idle time summed over every processor, which is
	// what GetSystemTimes reports.
	std::string uptimeFileText() {
		FILETIME idle{};
		FILETIME kernel{};
		FILETIME user{};
		::GetSystemTimes(&idle, &kernel, &user);
		const double uptime = static_cast<double>(::GetTickCount64()) / 1000.0;
		return formatted("%.2f %.2f\n", uptime, filetimeSeconds(idle));
	}

	// Windows keeps no load average, so it reads 0 as uptime and top show
	// it; the running / total and last-PID fields are real.
	std::string loadAverageText() {
		unsigned long threads = 0;
		unsigned long newest = 0;
		for (const ProcessInfo& process : listProcesses()) {
			threads += process.threads;
			newest = std::max(newest, process.pid);
		}

		return formatted("0.00 0.00 0.00 1/%lu %lu\n", threads, newest);
	}

	// Never "Microsoft": scripts take that in /proc/version to mean WSL.
	std::string versionText() {
		const WindowsVersion os = windowsVersion();
		const std::string release = formatted("%lu.%lu.%lu", os.major, os.minor, os.build);
		const std::string shell = formatted("(wbsh %d.%d.%d)", WBSH_VERSION_MAJOR,
			WBSH_VERSION_MINOR, WBSH_VERSION_PATCH);
		const std::string windows = formatted("Windows NT %lu.%lu build %lu", os.major, os.minor,
			os.build);
		return "wbsh version " + release + " " + shell + " " + windows + "\n";
	}

	static bool findProcess(unsigned long pid, ProcessInfo& found) {
		for (const ProcessInfo& process : listProcesses()) {
			if (process.pid != pid) continue;

			found = process;
			return true;
		}

		return false;
	}

	static std::string commandName(const std::string& image) {
		const std::size_t length = image.size();
		const bool exe = length > 4 && _stricmp(image.c_str() + length - 4, ".exe") == 0;
		return exe ? image.substr(0, length - 4) : image;
	}

	static std::string nulSeparatedArguments(const std::string& command_line) {
		if (command_line.empty()) return std::string();

		int count = 0;
		LPWSTR* words = ::CommandLineToArgvW(utf8ToWide(command_line).c_str(), &count);
		if (words == nullptr) return command_line + '\0';

		std::string text;
		for (int i = 0; i < count; ++i) text += wideToUtf8(words[i]) + '\0';
		::LocalFree(words);
		return text;
	}

	static std::string statusText(unsigned long pid, const ProcessInfo& process) {
		const bool self = pid == ::GetCurrentProcessId();
		std::string text;
		text += "Name:\t" + commandName(process.name) + "\n";
		text += self ? "State:\tR (running)\n" : "State:\tS (sleeping)\n";
		text += formatted("Tgid:\t%lu\nPid:\t%lu\nPPid:\t%lu\n", pid, pid, process.parent_pid);

		const std::map<unsigned long, ProcessUsage> usage = collectProcessUsage();
		const auto found = usage.find(pid);
		if (found != usage.end()) {
			text += formatted("VmRSS:\t%8llu kB\n", found->second.working_set / kKiB);
		}

		text += formatted("Threads:\t%lu\n", process.threads);
		return text;
	}

	static std::string environmentText() {
		LPWCH block = ::GetEnvironmentStringsW();
		if (block == nullptr) return std::string();

		std::string text;
		for (const wchar_t* entry = block; *entry != L'\0'; entry += std::wcslen(entry) + 1) {
			if (entry[0] != L'=') text += wideToUtf8(entry) + '\0';
		}

		::FreeEnvironmentStringsW(block);
		return text;
	}

	std::string processFileText(unsigned long pid, const std::string& name) {
		if (name == "environ") return environmentText();
		if (name == "cmdline") return nulSeparatedArguments(processCommandLine(pid));

		ProcessInfo process;
		if (!findProcess(pid, process)) return std::string();
		if (name == "comm") return commandName(process.name) + "\n";
		return statusText(pid, process);
	}

	std::vector<unsigned long> processIds() {
		std::vector<unsigned long> pids;
		for (const ProcessInfo& process : listProcesses()) {
			if (process.pid != 0) pids.push_back(process.pid);
		}

		std::sort(pids.begin(), pids.end());
		return pids;
	}

	// A process that will not be opened still exists; only an id that names
	// none is refused as an invalid parameter.
	bool processExists(unsigned long pid) {
		const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (process == nullptr) return ::GetLastError() == ERROR_ACCESS_DENIED;

		DWORD code = 0;
		const bool running = ::GetExitCodeProcess(process, &code) != 0 && code == STILL_ACTIVE;
		::CloseHandle(process);
		return running;
	}

	std::string processImagePath(const PathConv& paths, unsigned long pid) {
		const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (process == nullptr) return std::string();

		std::vector<wchar_t> path(32768);
		DWORD length = static_cast<DWORD>(path.size());
		const bool known = ::QueryFullProcessImageNameW(process, 0, path.data(), &length) != 0;
		::CloseHandle(process);
		if (!known) return std::string();

		return paths.toPosix(wideToUtf8(std::wstring(path.data(), length)));
	}

	std::string currentDirectoryPath(const PathConv& paths) {
		const DWORD needed = ::GetCurrentDirectoryW(0, nullptr);
		std::wstring directory(needed, L'\0');
		const DWORD length = ::GetCurrentDirectoryW(needed, directory.data());
		directory.resize(length < needed ? length : 0);
		return directory.empty() ? std::string() : paths.toPosix(wideToUtf8(directory));
	}

	static HANDLE descriptorHandle(int fd) {
		const intptr_t raw = _get_osfhandle(fd);
		if (raw == -1 || raw == -2) return INVALID_HANDLE_VALUE;
		return reinterpret_cast<HANDLE>(raw);
	}

	std::vector<int> openDescriptors() {
		std::vector<int> fds;
		for (int fd = 0; fd < kProbedDescriptors; ++fd) {
			if (descriptorHandle(fd) != INVALID_HANDLE_VALUE) fds.push_back(fd);
		}

		return fds;
	}

	static std::wstring withoutVerbatimPrefix(const std::wstring& path) {
		static const std::wstring kUncPrefix = L"\\\\?\\UNC\\";
		static const std::wstring kPrefix = L"\\\\?\\";
		const bool unc = path.compare(0, kUncPrefix.size(), kUncPrefix) == 0;
		if (unc) return L"\\\\" + path.substr(kUncPrefix.size());
		if (path.compare(0, kPrefix.size(), kPrefix) == 0) return path.substr(kPrefix.size());
		return path;
	}

	static std::string diskFilePath(const PathConv& paths, HANDLE handle) {
		std::vector<wchar_t> path(32768);
		const DWORD length = ::GetFinalPathNameByHandleW(handle, path.data(),
			static_cast<DWORD>(path.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
		if (length == 0 || length >= path.size()) return std::string();

		const std::wstring native = withoutVerbatimPrefix(std::wstring(path.data(), length));
		return paths.toPosix(wideToUtf8(native));
	}

	std::string descriptorTarget(const PathConv& paths, int fd, std::string& disk_path) {
		disk_path.clear();
		const HANDLE handle = descriptorHandle(fd);
		if (handle == INVALID_HANDLE_VALUE) return std::string();

		DWORD mode = 0;
		switch (::GetFileType(handle)) {
			case FILE_TYPE_DISK:
				disk_path = diskFilePath(paths, handle);
				return disk_path.empty() ? "anon_inode:[file]" : disk_path;
			case FILE_TYPE_PIPE:
				return "pipe:[" + std::to_string(reinterpret_cast<std::uintptr_t>(handle)) + "]";
			case FILE_TYPE_CHAR:
				return ::GetConsoleMode(handle, &mode) != 0 ? "/dev/tty" : "/dev/null";
			default:
				return "anon_inode:[unknown]";
		}
	}

}  // namespace wbsh::procfs_detail

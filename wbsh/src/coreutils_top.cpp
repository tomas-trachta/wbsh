/**
 * @file coreutils_top.cpp
 * @brief top: a live process view.
 *
 * Every frame is a fresh Toolhelp process list and WTS usage table, so no
 * process has to be opened. %CPU is the CPU time a process used since the
 * previous frame over the wall time in between, per core as procps' Irix
 * mode counts it, and %Cpu(s) comes from GetSystemTimes the same way.
 * VIRT is the commit charge (ps' VSZ), RES the working set, NI the priority
 * class read back through the mapping nice uses, and PR is 20 + NI. A
 * process counts as running when it used CPU in the interval; Windows
 * keeps no load average and has no stopped or zombie processes to count.
 */

#include "coreutils_internal.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <psapi.h>
#  pragma comment(lib, "psapi.lib")
#  include <wtsapi32.h>
#  pragma comment(lib, "wtsapi32.lib")
#endif /* _WIN32 */

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include "consolekeys.h"
#include "executor.h"
#include "interrupt.h"
#include "numparse.h"
#include "optscan.h"
#include "textcells.h"

namespace wbsh {

#ifdef _WIN32

	namespace top_detail {

		enum class SortField { Pid, User, Priority, Nice, Virtual, Resident, Cpu, Memory, Time,
			Command };

		struct FieldName {
			const char* name;
			SortField field;
		};

		static const FieldName kFieldNames[] = {
			{ "PID",  SortField::Pid },     { "USER", SortField::User },
			{ "PR",   SortField::Priority }, { "NI",   SortField::Nice },
			{ "VIRT", SortField::Virtual }, { "RES",  SortField::Resident },
			{ "%CPU", SortField::Cpu },     { "%MEM", SortField::Memory },
			{ "TIME+", SortField::Time },   { "TIME", SortField::Time },
			{ "COMMAND", SortField::Command },
		};

		enum TopOptionId {
			kOptBatch = 1, kOptCommandLine, kOptDelay, kOptIterations, kOptPid, kOptUser,
			kOptSort, kOptIdle, kOptIgnored,
		};

		struct TopOptions {
			double delay = 3.0;
			long iterations = 0;
			bool batch = false;
			bool command_lines = false;
			bool hide_idle = false;
			SortField sort = SortField::Cpu;
			bool ascending = false;
			std::vector<unsigned long> pids;
			std::string user;
		};

		struct Snapshot {
			std::uint64_t taken = 0;
			std::uint64_t idle = 0;
			std::uint64_t kernel = 0;
			std::uint64_t user = 0;
			std::vector<ProcessInfo> processes;
			std::map<unsigned long, ProcessUsage> usage;
		};

		struct Task {
			ProcessInfo info;
			ProcessUsage usage;
			int niceness = 0;
			double cpu_percent = 0.0;
			double mem_percent = 0.0;
			bool running = false;
		};

		struct MemoryTotals {
			std::uint64_t physical_bytes = 0;
			double total = 0.0;
			double free = 0.0;
			double used = 0.0;
			double cache = 0.0;
			double available = 0.0;
			double swap_total = 0.0;
			double swap_free = 0.0;
		};

		struct Summary {
			std::size_t total = 0;
			std::size_t running = 0;
			double user_percent = 0.0;
			double system_percent = 0.0;
			double idle_percent = 100.0;
			MemoryTotals memory;
		};

		struct TopConsole {
			HANDLE in = INVALID_HANDLE_VALUE;
			HANDLE out = INVALID_HANDLE_VALUE;
			DWORD saved_in = 0;
			DWORD saved_out = 0;
			int columns = 80;
			int rows = 24;
		};

		struct Top {
			TopOptions options;
			Snapshot previous;
			Summary summary;
			std::vector<Task> tasks;
			std::string message;
			TopConsole console;
		};

		enum class InputEvent { Timeout, Refresh, Redraw, Quit };

		enum class PromptKey { Typing, Accepted, Cancelled };

	}  // namespace top_detail

	using top_detail::InputEvent;
	using top_detail::MemoryTotals;
	using top_detail::PromptKey;
	using top_detail::Snapshot;
	using top_detail::SortField;
	using top_detail::Summary;
	using top_detail::Task;
	using top_detail::Top;
	using top_detail::TopConsole;
	using top_detail::TopOptions;

	static const double        kBytesPerKibibyte  = 1024.0;
	static const double        kBytesPerMebibyte  = 1024.0 * 1024.0;
	static const double        kFirstSampleLimit  = 0.5;
	static const std::int64_t  kTicksPerCentisecond = 100000;
	static const std::size_t   kUserWidth         = 8;
	static const std::size_t   kTimeWidth         = 9;
	static const int           kVirtualWidth      = 7;
	static const int           kResidentWidth     = 6;
	static const int           kDefaultSignal     = 15;
	static const long          kRealtimeBase      = 16;
	static const char* const   kColumnHeader =
		"    PID USER      PR  NI    VIRT    RES S  %CPU  %MEM     TIME+ COMMAND";
	static const char* const   kMessageRow        = "\x1b[6;1H";

	static const char* const kHelpLines[] = {
		"Help for Interactive Commands - wbsh top",
		"",
		"  P,M,N,T   Sort by %CPU, %MEM, PID, TIME+",
		"  R         Reverse the sort order",
		"  c         Show command lines or program names",
		"  i         Hide or show idle processes",
		"  k         Kill (end) a process",
		"  r         Renice a process",
		"  d,s       Change the delay between updates",
		"  space     Update now",
		"  h,?       This help",
		"  q         Quit",
		"",
		"Press any key to continue",
	};

	static std::string lowerAscii(std::string text) {
		for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return text;
	}

	static std::string upperAscii(std::string text) {
		for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		return text;
	}

	static std::uint64_t fileTimeValue(const FILETIME& time) {
		ULARGE_INTEGER value;
		value.LowPart = time.dwLowDateTime;
		value.HighPart = time.dwHighDateTime;
		return value.QuadPart;
	}

	static DWORD delayMilliseconds(double seconds) {
		if (seconds <= 0.0) return 0;
		const double ms = seconds * 1000.0;
		if (ms >= static_cast<double>(INFINITE - 1)) return INFINITE - 1;
		return static_cast<DWORD>(ms);
	}

	static Snapshot takeSnapshot() {
		Snapshot snapshot;
		ULONGLONG now = 0;
		::QueryUnbiasedInterruptTime(&now);
		snapshot.taken = now;

		FILETIME idle{};
		FILETIME kernel{};
		FILETIME user{};
		if (::GetSystemTimes(&idle, &kernel, &user) != 0) {
			snapshot.idle = fileTimeValue(idle);
			snapshot.kernel = fileTimeValue(kernel);
			snapshot.user = fileTimeValue(user);
		}

		snapshot.processes = listProcesses();
		snapshot.usage = collectProcessUsage();
		return snapshot;
	}

	static std::int64_t previousCpuTicks(const Snapshot& previous, unsigned long pid) {
		const auto hit = previous.usage.find(pid);
		return hit == previous.usage.end() ? 0 : hit->second.cpu_ticks;
	}

	static Task makeTask(const ProcessInfo& info, const Snapshot& previous,
	                     const Snapshot& current, std::uint64_t physical_bytes) {
		Task task;
		task.info = info;
		const auto hit = current.usage.find(info.pid);
		if (hit != current.usage.end()) task.usage = hit->second;
		task.niceness = nicenessOfBasePriority(info.base_priority);

		const std::int64_t used = task.usage.cpu_ticks - previousCpuTicks(previous, info.pid);
		const double elapsed = static_cast<double>(current.taken - previous.taken);
		task.running = used > 0;
		if (task.running && elapsed > 0.0) {
			task.cpu_percent = static_cast<double>(used) * 100.0 / elapsed;
		}

		if (physical_bytes > 0) {
			task.mem_percent = static_cast<double>(task.usage.working_set) * 100.0
				/ static_cast<double>(physical_bytes);
		}

		return task;
	}

	static void summarizeCpu(const Snapshot& previous, const Snapshot& current, Summary& summary) {
		const double idle = static_cast<double>(current.idle - previous.idle);
		const double kernel = static_cast<double>(current.kernel - previous.kernel);
		const double user = static_cast<double>(current.user - previous.user);
		const double total = kernel + user;
		if (total <= 0.0) return;

		summary.user_percent = user * 100.0 / total;
		summary.system_percent = (kernel - idle) * 100.0 / total;
		summary.idle_percent = idle * 100.0 / total;
	}

	static double systemCacheBytes() {
		PERFORMANCE_INFORMATION performance{};
		performance.cb = sizeof(performance);
		if (::GetPerformanceInfo(&performance, sizeof(performance)) == 0) return 0.0;
		return static_cast<double>(performance.SystemCache)
			* static_cast<double>(performance.PageSize);
	}

	// procps splits available memory into free and buff/cache; Windows'
	// counterpart of the cache is the standby list, part of what is available.
	static MemoryTotals memoryTotals() {
		MemoryTotals totals;
		MEMORYSTATUSEX status{};
		status.dwLength = sizeof(status);
		if (::GlobalMemoryStatusEx(&status) == 0) return totals;

		const double available = static_cast<double>(status.ullAvailPhys);
		const double cache = std::min(systemCacheBytes(), available);
		totals.physical_bytes = status.ullTotalPhys;
		totals.total = static_cast<double>(status.ullTotalPhys) / kBytesPerMebibyte;
		totals.available = available / kBytesPerMebibyte;
		totals.cache = cache / kBytesPerMebibyte;
		totals.free = (available - cache) / kBytesPerMebibyte;
		totals.used = totals.total - totals.available;

		unsigned long long swap_total = 0;
		unsigned long long swap_free = 0;
		pageFileUsage(swap_total, swap_free);
		totals.swap_total = static_cast<double>(swap_total) / kBytesPerMebibyte;
		totals.swap_free = static_cast<double>(swap_free) / kBytesPerMebibyte;
		return totals;
	}

	static bool isSelected(const TopOptions& options, const Task& task) {
		const auto& pids = options.pids;
		if (!pids.empty() && std::find(pids.begin(), pids.end(), task.info.pid) == pids.end()) {
			return false;
		}

		if (!options.user.empty() && lowerAscii(task.usage.user) != lowerAscii(options.user)) {
			return false;
		}

		return !options.hide_idle || task.running;
	}

	static std::string commandName(const Task& task) {
		const std::string& name = task.info.name;
		const bool exe = name.size() > 4 && lowerAscii(name.substr(name.size() - 4)) == ".exe";
		return exe ? name.substr(0, name.size() - 4) : name;
	}

	static double numericKey(SortField field, const Task& task) {
		switch (field) {
			case SortField::Pid:      return static_cast<double>(task.info.pid);
			case SortField::Priority:
			case SortField::Nice:     return static_cast<double>(task.niceness);
			case SortField::Virtual:  return static_cast<double>(task.usage.pagefile);
			case SortField::Resident: return static_cast<double>(task.usage.working_set);
			case SortField::Cpu:      return task.cpu_percent;
			case SortField::Memory:   return task.mem_percent;
			case SortField::Time:     return static_cast<double>(task.usage.cpu_ticks);
			default:                  return 0.0;
		}
	}

	static int compareTasks(SortField field, const Task& a, const Task& b) {
		if (field == SortField::User) {
			return lowerAscii(a.usage.user).compare(lowerAscii(b.usage.user));
		}

		if (field == SortField::Command) {
			return lowerAscii(commandName(a)).compare(lowerAscii(commandName(b)));
		}

		const double x = numericKey(field, a);
		const double y = numericKey(field, b);
		if (x == y) return 0;
		return x < y ? -1 : 1;
	}

	// High to low unless reversed, as procps sorts; equal rows keep PID order.
	static void sortTasks(Top& top) {
		const SortField field = top.options.sort;
		const bool ascending = top.options.ascending;
		std::stable_sort(top.tasks.begin(), top.tasks.end(),
			[field, ascending](const Task& a, const Task& b) {
				const int order = compareTasks(field, a, b);
				if (order != 0) return ascending ? order < 0 : order > 0;
				return a.info.pid < b.info.pid;
			});
	}

	static void refresh(Top& top) {
		Snapshot current = takeSnapshot();
		Summary summary;
		summary.memory = memoryTotals();
		summarizeCpu(top.previous, current, summary);

		top.tasks.clear();
		for (const ProcessInfo& info : current.processes) {
			if (info.pid == 0) continue;

			const Task task = makeTask(info, top.previous, current, summary.memory.physical_bytes);
			++summary.total;
			if (task.running) ++summary.running;
			if (isSelected(top.options, task)) top.tasks.push_back(task);
		}

		top.summary = summary;
		top.previous = std::move(current);
		sortTasks(top);
	}

	static int activeSessions() {
		WTS_SESSION_INFOW* sessions = nullptr;
		DWORD count = 0;
		if (::WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count) == 0) {
			return 0;
		}

		int active = 0;
		for (DWORD i = 0; i < count; ++i) {
			if (sessions[i].State == WTSActive) ++active;
		}

		::WTSFreeMemory(sessions);
		return active;
	}

	static std::string clockText() {
		const std::time_t now = std::time(nullptr);
		std::tm local{};
		localtime_s(&local, &now);
		char text[16];
		std::strftime(text, sizeof(text), "%H:%M:%S", &local);
		return text;
	}

	static std::string formatted(const char* format, ...) {
		char text[256];
		va_list arguments;
		va_start(arguments, format);
		std::vsnprintf(text, sizeof(text), format, arguments);
		va_end(arguments);
		return text;
	}

	static std::vector<std::string> summaryLines(const Summary& summary) {
		const int users = activeSessions();
		const MemoryTotals& memory = summary.memory;
		const std::size_t sleeping = summary.total - summary.running;
		return {
			formatted("top - %s up %s, %2d user%s,  load average: 0.00, 0.00, 0.00",
				clockText().c_str(), uptimeText(::GetTickCount64()).c_str(), users,
				users == 1 ? "" : "s"),
			formatted("Tasks: %3zu total, %3zu running, %3zu sleeping,   0 stopped,   0 zombie",
				summary.total, summary.running, sleeping),
			formatted("%%Cpu(s): %4.1f us, %4.1f sy,  0.0 ni, %4.1f id,  0.0 wa,  0.0 hi, "
				" 0.0 si,  0.0 st", summary.user_percent, summary.system_percent,
				summary.idle_percent),
			formatted("MiB Mem : %8.1f total, %8.1f free, %8.1f used, %8.1f buff/cache",
				memory.total, memory.free, memory.used, memory.cache),
			formatted("MiB Swap: %8.1f total, %8.1f free, %8.1f used. %8.1f avail Mem",
				memory.swap_total, memory.swap_free, memory.swap_total - memory.swap_free,
				memory.available),
		};
	}

	// KiB while they fit, then the largest unit that does, as procps scales.
	static std::string scaledKibibytes(std::uint64_t bytes, int width) {
		const std::string exact = std::to_string(bytes / 1024);
		if (static_cast<int>(exact.size()) <= width) return exact;

		double value = static_cast<double>(bytes) / kBytesPerKibibyte;
		for (const char* unit = "mgtp"; *unit != '\0'; ++unit) {
			value /= kBytesPerKibibyte;
			for (int precision = 1; precision >= 0; --precision) {
				const std::string text = formatted("%.*f%c", precision, value, *unit);
				if (static_cast<int>(text.size()) <= width) return text;
			}
		}

		return exact;
	}

	static std::string cpuTimeText(std::int64_t ticks) {
		const long long centiseconds = ticks / kTicksPerCentisecond;
		const long long seconds = centiseconds / 100;
		const std::string precise = formatted("%lld:%02lld.%02lld", seconds / 60, seconds % 60,
			centiseconds % 100);
		if (precise.size() <= kTimeWidth) return precise;

		const std::string minutes = formatted("%lld:%02lld", seconds / 60, seconds % 60);
		if (minutes.size() <= kTimeWidth) return minutes;
		return formatted("%lld,%02lld", seconds / 3600, (seconds / 60) % 60);
	}

	static std::string fitCells(const std::string& text, std::size_t cells) {
		std::size_t pos = 0;
		std::size_t used = 0;
		while (pos < text.size()) {
			const TextCluster cluster = clusterAt(text, pos);
			if (used + cluster.width > cells) break;
			used += cluster.width;
			pos = cluster.end;
		}

		return text.substr(0, pos);
	}

	static std::string padCells(const std::string& text, std::size_t cells) {
		const std::size_t width = displayWidth(text, text.size());
		return width >= cells ? text : text + std::string(cells - width, ' ');
	}

	static std::string userCell(const std::string& user) {
		const std::string name = user.empty() ? "?" : user;
		if (displayWidth(name, name.size()) <= kUserWidth) return padCells(name, kUserWidth);
		return padCells(fitCells(name, kUserWidth - 1) + "+", kUserWidth);
	}

	static std::string commandCell(const Top& top, const Task& task) {
		if (!top.options.command_lines) return commandName(task);

		const std::string line = processCommandLine(task.info.pid);
		return line.empty() ? commandName(task) : line;
	}

	static std::string taskRow(const Top& top, const Task& task) {
		const std::string priority = task.info.base_priority >= kRealtimeBase ? "rt"
			: std::to_string(20 + task.niceness);
		const std::string head = formatted("%7lu ", task.info.pid);
		const std::string rest = formatted(" %3s %3d %7s %6s %c %5.1f %5.1f %9s ",
			priority.c_str(), task.niceness,
			scaledKibibytes(task.usage.pagefile, kVirtualWidth).c_str(),
			scaledKibibytes(task.usage.working_set, kResidentWidth).c_str(),
			task.running ? 'R' : 'S', task.cpu_percent, task.mem_percent,
			cpuTimeText(task.usage.cpu_ticks).c_str());
		return head + userCell(task.usage.user) + rest + commandCell(top, task);
	}

	static void printBatchFrame(const Top& top) {
		std::string frame;
		for (const std::string& line : summaryLines(top.summary)) frame += line + "\n";
		frame += "\n";
		frame += kColumnHeader;
		frame += "\n";
		for (const Task& task : top.tasks) frame += taskRow(top, task) + "\n";

		std::fputs(frame.c_str(), stdout);
		std::fflush(stdout);
	}

	static bool reachedLimit(const TopOptions& options, long shown) {
		return options.iterations > 0 && shown >= options.iterations;
	}

	static DWORD firstSampleMilliseconds(const TopOptions& options) {
		return delayMilliseconds(std::min(options.delay, kFirstSampleLimit));
	}

	static int runBatch(Top& top) {
		const ScopedCtrlCCapture capture;
		top.previous = takeSnapshot();
		bool proceed = sleepUnlessCtrlC(firstSampleMilliseconds(top.options));
		for (long shown = 0; proceed; ) {
			refresh(top);
			if (shown > 0) std::fputc('\n', stdout);
			printBatchFrame(top);
			if (stdoutFailed()) return stdoutFailureStatus();

			++shown;
			if (reachedLimit(top.options, shown)) break;
			proceed = sleepUnlessCtrlC(delayMilliseconds(top.options.delay));
		}

		takeCtrlC();
		return 0;
	}

	static void writeConsole(const TopConsole& console, const std::string& text) {
		DWORD written = 0;
		::WriteFile(console.out, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
	}

	static void closeTopConsole(TopConsole& console) {
		if (console.in != INVALID_HANDLE_VALUE) {
			::SetConsoleMode(console.in, console.saved_in);
			::CloseHandle(console.in);
		}

		if (console.out != INVALID_HANDLE_VALUE) {
			::SetConsoleMode(console.out, console.saved_out);
			::CloseHandle(console.out);
		}

		console.in = INVALID_HANDLE_VALUE;
		console.out = INVALID_HANDLE_VALUE;
	}

	static HANDLE openConsoleDevice(const wchar_t* name) {
		return ::CreateFileW(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr, OPEN_EXISTING, 0, nullptr);
	}

	static bool openTopConsole(TopConsole& console) {
		console.in = openConsoleDevice(L"CONIN$");
		console.out = openConsoleDevice(L"CONOUT$");
		const bool opened = console.in != INVALID_HANDLE_VALUE
			&& console.out != INVALID_HANDLE_VALUE
			&& ::GetConsoleMode(console.in, &console.saved_in) != 0
			&& ::GetConsoleMode(console.out, &console.saved_out) != 0;
		if (!opened) {
			closeTopConsole(console);
			return false;
		}

		const DWORD cooked = ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT
			| ENABLE_QUICK_EDIT_MODE | ENABLE_MOUSE_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT;
		::SetConsoleMode(console.in, (console.saved_in & ~cooked) | ENABLE_WINDOW_INPUT
			| ENABLE_EXTENDED_FLAGS);
		::SetConsoleMode(console.out, console.saved_out | ENABLE_VIRTUAL_TERMINAL_PROCESSING
			| ENABLE_PROCESSED_OUTPUT);
		return true;
	}

	static void measureConsole(TopConsole& console) {
		CONSOLE_SCREEN_BUFFER_INFO info{};
		if (::GetConsoleScreenBufferInfo(console.out, &info) == 0) return;

		console.columns = std::max(1, info.srWindow.Right - info.srWindow.Left + 1);
		console.rows = std::max(1, info.srWindow.Bottom - info.srWindow.Top + 1);
	}

	static std::vector<std::string> screenLines(Top& top) {
		const std::size_t width = static_cast<std::size_t>(top.console.columns);
		std::vector<std::string> lines;
		for (const std::string& line : summaryLines(top.summary)) {
			lines.push_back(fitCells(line, width));
		}

		lines.push_back(fitCells(top.message, width));
		top.message.clear();
		lines.push_back("\x1b[7m" + padCells(fitCells(kColumnHeader, width), width) + "\x1b[0m");
		for (const Task& task : top.tasks) lines.push_back(fitCells(taskRow(top, task), width));

		lines.resize(std::min(lines.size(), static_cast<std::size_t>(top.console.rows)));
		return lines;
	}

	// Erasing after a line that fills the row would erase its last cell,
	// where the cursor waits to wrap.
	static void renderScreen(Top& top) {
		measureConsole(top.console);
		const std::size_t width = static_cast<std::size_t>(top.console.columns);
		const std::vector<std::string> lines = screenLines(top);

		std::string frame = "\x1b[H";
		for (std::size_t i = 0; i < lines.size(); ++i) {
			if (i != 0) frame += "\r\n";
			frame += lines[i];
			if (displayWidth(lines[i], lines[i].size()) < width) frame += "\x1b[K";
		}

		frame += "\x1b[J";
		writeConsole(top.console, frame);
	}

	static bool readKeyPress(const TopConsole& console, KEY_EVENT_RECORD& key) {
		for (;;) {
			INPUT_RECORD record;
			DWORD read = 0;
			if (::ReadConsoleInputW(console.in, &record, 1, &read) == 0 || read == 0) return false;
			if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) continue;

			key = record.Event.KeyEvent;
			if (key.uChar.UnicodeChar != 0) return true;
		}
	}

	static void dropLastCharacter(std::string& text) {
		while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) {
			text.pop_back();
		}

		if (!text.empty()) text.pop_back();
	}

	static PromptKey applyPromptKey(const TopConsole& console, const KEY_EVENT_RECORD& key,
	                                std::string& entered) {
		const wchar_t ch = key.uChar.UnicodeChar;
		if (ch == L'\r') return PromptKey::Accepted;
		if (ch == 0x1B || ch == 0x03) return PromptKey::Cancelled;
		if (ch == L'\b') dropLastCharacter(entered);
		else if (ch >= 0x20) entered += readConsoleCharacter(console.in, ch);
		return PromptKey::Typing;
	}

	static bool promptLine(Top& top, const std::string& prompt, std::string& entered) {
		entered.clear();
		PromptKey state = PromptKey::Typing;
		while (state == PromptKey::Typing) {
			writeConsole(top.console, std::string(kMessageRow) + prompt + entered
				+ "\x1b[K\x1b[?25h");

			KEY_EVENT_RECORD key{};
			if (!readKeyPress(top.console, key)) state = PromptKey::Cancelled;
			else state = applyPromptKey(top.console, key, entered);
		}

		writeConsole(top.console, "\x1b[?25l");
		return state == PromptKey::Accepted;
	}

	static bool promptPid(Top& top, const char* what, unsigned long& pid) {
		const unsigned long fallback = top.tasks.empty() ? 0 : top.tasks.front().info.pid;
		std::string entered;
		if (!promptLine(top, formatted("%s [default pid = %lu] ", what, fallback), entered)) {
			return false;
		}

		if (entered.empty()) {
			pid = fallback;
			return pid != 0;
		}

		std::size_t used = 0;
		if (parseUL(entered, pid, 10, &used) && used == entered.size()) return true;

		top.message = "Unacceptable integer";
		return false;
	}

	static std::string lastErrorText() {
		return posixErrorText(std::error_code(static_cast<int>(::GetLastError()),
			std::system_category()));
	}

	static void killPrompted(Top& top) {
		unsigned long pid = 0;
		if (!promptPid(top, "PID to signal/kill", pid)) return;

		std::string entered;
		const std::string prompt = formatted("Send pid %lu signal [%d/sigterm] ", pid,
			kDefaultSignal);
		if (!promptLine(top, prompt, entered)) return;

		int signum = kDefaultSignal;
		if (!entered.empty() && !parseSignalName(entered, signum)) {
			top.message = "Invalid signal";
			return;
		}

		if (terminateProcessId(pid, signum)) return;
		top.message = formatted("Failed signal pid '%lu' with '%d': ", pid, signum)
			+ lastErrorText();
	}

	static void renicePrompted(Top& top) {
		unsigned long pid = 0;
		if (!promptPid(top, "PID to renice", pid)) return;

		std::string entered;
		if (!promptLine(top, formatted("Renice PID %lu to value ", pid), entered)) return;

		int niceness = 0;
		std::size_t used = 0;
		if (!parseInt(entered, niceness, 10, &used) || used != entered.size()) {
			top.message = "Unacceptable integer";
			return;
		}

		std::error_code ec;
		if (setProcessNiceness(pid, niceness, ec)) return;
		top.message = formatted("Failed renice of PID %lu to %d: ", pid, niceness)
			+ posixErrorText(ec);
	}

	static void delayPrompted(Top& top) {
		std::string entered;
		const std::string prompt = formatted("Change delay from %.1f to ", top.options.delay);
		if (!promptLine(top, prompt, entered) || entered.empty()) return;

		double seconds = 0.0;
		std::size_t used = 0;
		if (!parseDouble(entered, seconds, &used) || used != entered.size() || seconds < 0.0) {
			top.message = "Unacceptable floating point";
			return;
		}

		top.options.delay = seconds;
	}

	static void showHelp(Top& top) {
		std::string screen = "\x1b[H\x1b[2J";
		for (const char* line : kHelpLines) screen += std::string(line) + "\r\n";
		writeConsole(top.console, screen);

		KEY_EVENT_RECORD key{};
		readKeyPress(top.console, key);
	}

	static InputEvent sortBy(Top& top, SortField field) {
		top.options.sort = field;
		return InputEvent::Redraw;
	}

	static InputEvent toggle(bool& flag, InputEvent event) {
		flag = !flag;
		return event;
	}

	static InputEvent handleKey(Top& top, wchar_t key) {
		switch (key) {
			case L'q':
			case 0x03:
				return InputEvent::Quit;
			case L'P':
				return sortBy(top, SortField::Cpu);
			case L'M':
				return sortBy(top, SortField::Memory);
			case L'N':
				return sortBy(top, SortField::Pid);
			case L'T':
				return sortBy(top, SortField::Time);
			case L'R':
				return toggle(top.options.ascending, InputEvent::Redraw);
			case L'c':
				return toggle(top.options.command_lines, InputEvent::Redraw);
			case L'i':
				return toggle(top.options.hide_idle, InputEvent::Refresh);
			case L'k':
				killPrompted(top);
				return InputEvent::Refresh;
			case L'r':
				renicePrompted(top);
				return InputEvent::Refresh;
			case L'd':
			case L's':
				delayPrompted(top);
				return InputEvent::Redraw;
			case L'h':
			case L'?':
				showHelp(top);
				return InputEvent::Redraw;
			case L' ':
			case L'\r':
				return InputEvent::Refresh;
			default:
				return InputEvent::Redraw;
		}
	}

	static InputEvent waitForInput(Top& top) {
		const ULONGLONG deadline = ::GetTickCount64() + delayMilliseconds(top.options.delay);
		for (;;) {
			const ULONGLONG now = ::GetTickCount64();
			if (now >= deadline) return InputEvent::Timeout;

			const DWORD left = static_cast<DWORD>(deadline - now);
			if (::WaitForSingleObject(top.console.in, left) != WAIT_OBJECT_0) {
				return InputEvent::Timeout;
			}

			INPUT_RECORD record;
			DWORD read = 0;
			if (::ReadConsoleInputW(top.console.in, &record, 1, &read) == 0 || read == 0) {
				return InputEvent::Quit;
			}

			if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) return InputEvent::Redraw;
			const KEY_EVENT_RECORD& key = record.Event.KeyEvent;
			if (record.EventType != KEY_EVENT || !key.bKeyDown || key.uChar.UnicodeChar == 0) {
				continue;
			}

			return handleKey(top, key.uChar.UnicodeChar);
		}
	}

	static void runInteractive(Top& top) {
		std::fflush(stdout);
		writeConsole(top.console, "\x1b[?1049h\x1b[?25l");
		top.previous = takeSnapshot();
		::Sleep(firstSampleMilliseconds(top.options));
		refresh(top);

		long shown = 1;
		for (;;) {
			renderScreen(top);
			if (reachedLimit(top.options, shown)) break;

			const InputEvent event = waitForInput(top);
			if (event == InputEvent::Quit) break;
			if (event == InputEvent::Redraw) {
				sortTasks(top);
				continue;
			}

			refresh(top);
			++shown;
		}

		writeConsole(top.console, "\x1b[?25h\x1b[?1049l");
	}

	static bool topFail(const std::string& message) {
		perr("top", message);
		return false;
	}

	static bool parseSortField(const std::string& text, TopOptions& options) {
		std::string name = text;
		if (!name.empty() && (name[0] == '+' || name[0] == '-')) {
			options.ascending = name[0] == '-';
			name.erase(0, 1);
		}

		for (const auto& entry : top_detail::kFieldNames) {
			if (upperAscii(name) != entry.name) continue;
			options.sort = entry.field;
			return true;
		}

		return topFail("unrecognized field name '" + text + "'");
	}

	static bool parsePidList(const std::string& text, std::vector<unsigned long>& pids) {
		std::size_t start = 0;
		while (start <= text.size()) {
			const std::size_t comma = std::min(text.find(',', start), text.size());
			const std::string item = text.substr(start, comma - start);
			unsigned long pid = 0;
			std::size_t used = 0;
			if (!parseUL(item, pid, 10, &used) || used != item.size()) {
				return topFail("bad pid '" + item + "'");
			}

			pids.push_back(pid);
			start = comma + 1;
		}

		return true;
	}

	static bool parseDelay(const std::string& text, double& delay) {
		std::size_t used = 0;
		if (parseDouble(text, delay, &used) && used == text.size() && delay >= 0.0) return true;
		return topFail("bad delay interval '" + text + "'");
	}

	static bool parseIterations(const std::string& text, long& iterations) {
		long long value = 0;
		std::size_t used = 0;
		if (parseLL(text, value, 10, &used) && used == text.size() && value > 0) {
			iterations = static_cast<long>(std::min<long long>(value, 0x7FFFFFFF));
			return true;
		}

		return topFail("bad iterations argument '" + text + "'");
	}

	static const std::vector<OptDef>& topOptionDefs() {
		static const std::vector<OptDef> defs = {
			{ top_detail::kOptBatch,       'b', "batch-mode",        OptArg::None },
			{ top_detail::kOptCommandLine, 'c', "cmdline-toggle",    OptArg::None },
			{ top_detail::kOptDelay,       'd', "delay",             OptArg::Required },
			{ top_detail::kOptIterations,  'n', "iterations",        OptArg::Required },
			{ top_detail::kOptPid,         'p', "pid",               OptArg::Required },
			{ top_detail::kOptUser,        'u', "filter-only-euser", OptArg::Required },
			{ top_detail::kOptUser,        'U', "filter-any-user",   OptArg::Required },
			{ top_detail::kOptSort,        'o', "sort-override",     OptArg::Required },
			{ top_detail::kOptIdle,        'i', "idle-toggle",       OptArg::None },
			{ top_detail::kOptIgnored,     'S', "accum-time-toggle", OptArg::None },
			{ top_detail::kOptIgnored,     's', "secure-mode",       OptArg::None },
			{ top_detail::kOptIgnored,     'H', "threads-show",      OptArg::None },
			{ top_detail::kOptIgnored,     'w', "width",             OptArg::Optional },
		};
		return defs;
	}

	static bool applyTopHit(const OptHit& hit, TopOptions& options) {
		switch (hit.id) {
			case top_detail::kOptBatch:       options.batch = true;                    return true;
			case top_detail::kOptCommandLine: options.command_lines = true;            return true;
			case top_detail::kOptDelay:       return parseDelay(hit.value, options.delay);
			case top_detail::kOptIterations:  return parseIterations(hit.value, options.iterations);
			case top_detail::kOptPid:         return parsePidList(hit.value, options.pids);
			case top_detail::kOptUser:        options.user = hit.value;                return true;
			case top_detail::kOptSort:        return parseSortField(hit.value, options);
			case top_detail::kOptIdle:        options.hide_idle = true;                return true;
			default:                          return true;
		}
	}

	static bool parseTopArgs(const std::vector<std::string>& args, TopOptions& options) {
		OptScanResult scanned;
		if (!scanOptions("top", args, topOptionDefs(), scanned)) return false;

		for (const OptHit& hit : scanned.hits) {
			if (!applyTopHit(hit, options)) return false;
		}

		if (scanned.operands.empty()) return true;
		return topFail("unknown argument '" + scanned.operands[0] + "'");
	}

	static int builtin_top(Executor&, const std::vector<std::string>& args) {
		Top top;
		if (!parseTopArgs(args, top.options)) return 1;

		const bool interactive = !top.options.batch && isConsoleStream(stdin)
			&& isConsoleStream(stdout) && openTopConsole(top.console);
		if (!interactive) return runBatch(top);

		runInteractive(top);
		closeTopConsole(top.console);
		return 0;
	}

	void registerTopBuiltin(Executor& exec) {
		exec.registerBuiltin("top", builtin_top);
	}

#else

	void registerTopBuiltin(Executor&) {}

#endif /* _WIN32 */

}  // namespace wbsh

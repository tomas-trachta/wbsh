/**
 * @file procs.cpp
 * @brief A live process monitor, and a process count in the terminal's
 *        status bar, from one DLL.
 *
 * In the shell, `procs` redraws a sorted table every second until q is
 * pressed; it takes m, n and p to sort by memory, name or pid, Up, Down,
 * PageUp, PageDown, Home and End to scroll, and r to refresh at once.
 * When stdout is not a terminal, or with --once, it prints the table one
 * time and exits, so `procs | grep chrome` works too.
 *
 * In wbshterm the same DLL registers a segment that shows how many
 * processes are running. A segment must answer instantly on the paint
 * thread, so a background thread does the counting and the segment only
 * reads the number it left behind.
 */

#include "wbshsdk.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif /* WIN32_LEAN_AND_MEAN */
#include <windows.h>

#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace procs {

	struct Row {
		unsigned long pid = 0;
		unsigned long parent = 0;
		unsigned long threads = 0;
		unsigned long long working_set = 0;
		std::string name;
	};

	enum class SortBy { Memory, Name, Pid };

	struct Monitor {
		WbshTerminal* terminal = nullptr;
		std::vector<Row> rows;
		SortBy sort = SortBy::Memory;
		std::size_t first_shown = 0;
		std::size_t visible_rows = 20;
		std::size_t drawn_lines = 0;
		int columns = 80;
	};

	/* ---- snapshot ---------------------------------------------------- */

	static std::string narrow(const wchar_t* text) {
		const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0,
			nullptr, nullptr);
		if (needed <= 1) return std::string();

		std::string out(static_cast<std::size_t>(needed) - 1, '\0');
		::WideCharToMultiByte(CP_UTF8, 0, text, -1, &out[0], needed, nullptr, nullptr);
		return out;
	}

	// Some processes refuse even limited queries; those are listed with
	// no memory figure rather than left out.
	static unsigned long long workingSetOf(unsigned long pid) {
		const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (process == nullptr) return 0;

		PROCESS_MEMORY_COUNTERS counters{};
		counters.cb = sizeof(counters);
		const bool known = ::K32GetProcessMemoryInfo(process, &counters, sizeof(counters)) != 0;
		::CloseHandle(process);

		return known ? counters.WorkingSetSize : 0;
	}

	static Row rowFrom(const PROCESSENTRY32W& entry) {
		Row row;
		row.pid = entry.th32ProcessID;
		row.parent = entry.th32ParentProcessID;
		row.threads = entry.cntThreads;
		row.name = narrow(entry.szExeFile);
		row.working_set = workingSetOf(entry.th32ProcessID);
		return row;
	}

	static std::vector<Row> snapshot() {
		std::vector<Row> rows;

		const HANDLE list = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (list == INVALID_HANDLE_VALUE) return rows;

		PROCESSENTRY32W entry{};
		entry.dwSize = sizeof(entry);
		for (BOOL more = ::Process32FirstW(list, &entry); more;
				more = ::Process32NextW(list, &entry)) {
			rows.push_back(rowFrom(entry));
		}

		::CloseHandle(list);
		return rows;
	}

	static bool before(const Row& a, const Row& b, SortBy sort) {
		switch (sort) {
		case SortBy::Memory: return a.working_set > b.working_set;
		case SortBy::Name:   return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
		case SortBy::Pid:    return a.pid < b.pid;
		default:             return false;
		}
	}

	static void sortRows(std::vector<Row>& rows, SortBy sort) {
		std::stable_sort(rows.begin(), rows.end(),
			[sort](const Row& a, const Row& b) { return before(a, b, sort); });
	}

	static void refresh(Monitor& monitor) {
		monitor.rows = snapshot();
		sortRows(monitor.rows, monitor.sort);

		const std::size_t last_first = monitor.rows.size() > monitor.visible_rows
			? monitor.rows.size() - monitor.visible_rows : 0;
		monitor.first_shown = std::min(monitor.first_shown, last_first);
	}

	/* ---- formatting -------------------------------------------------- */

	static std::string memoryText(unsigned long long bytes) {
		const double mebibytes = static_cast<double>(bytes) / (1024.0 * 1024);

		char text[32];
		if (bytes == 0) {
			std::snprintf(text, sizeof(text), "%9s", "-");
		} else if (mebibytes >= 1024.0) {
			std::snprintf(text, sizeof(text), "%8.1fG", mebibytes / 1024.0);
		} else {
			std::snprintf(text, sizeof(text), "%8.1fM", mebibytes);
		}
		return text;
	}

	static std::string fitted(const std::string& line, int columns) {
		const std::size_t width = columns > 0 ? static_cast<std::size_t>(columns) : 0;
		return line.size() > width ? line.substr(0, width) : line;
	}

	static std::string headingLine() {
		char line[96];
		std::snprintf(line, sizeof(line), "%7s %7s %9s %5s  %s",
			"PID", "PPID", "MEM", "THR", "NAME");
		return line;
	}

	static std::string rowLine(const Row& row) {
		char line[64];
		std::snprintf(line, sizeof(line), "%7lu %7lu %s %5lu  ",
			row.pid, row.parent, memoryText(row.working_set).c_str(), row.threads);
		return line + row.name;
	}

	static const char* sortName(SortBy sort) {
		switch (sort) {
		case SortBy::Memory: return "memory";
		case SortBy::Name:   return "name";
		case SortBy::Pid:    return "pid";
		default:             return "?";
		}
	}

	/* ---- drawing ----------------------------------------------------- */

	static std::string titleLine(const Monitor& monitor) {
		char line[160];
		std::snprintf(line, sizeof(line),
			"\x1b[1m%zu processes\x1b[0m  sorted by %s   "
			"\x1b[90m[m]em [n]ame [p]id  [r]efresh  [q]uit\x1b[0m",
			monitor.rows.size(), sortName(monitor.sort));
		return line;
	}

	static std::string erasePrevious(const Monitor& monitor) {
		std::string out;
		if (monitor.drawn_lines > 0) out += "\x1b[" + std::to_string(monitor.drawn_lines) + "A";
		return out + "\r\x1b[J";
	}

	static void fitToWindow(Monitor& monitor) {
		int rows = 0;
		if (wbshTerminalSize(monitor.terminal, &monitor.columns, &rows) != WBSH_OK) return;

		monitor.visible_rows = rows > 4 ? static_cast<std::size_t>(rows) - 4 : 1;
	}

	static void render(Monitor& monitor) {
		std::string frame = erasePrevious(monitor);
		frame += fitted(titleLine(monitor), monitor.columns + 24) + "\r\n";
		frame += "\x1b[4m" + fitted(headingLine(), monitor.columns) + "\x1b[0m\r\n";

		const std::size_t end = std::min(monitor.rows.size(),
			monitor.first_shown + monitor.visible_rows);
		for (std::size_t at = monitor.first_shown; at < end; ++at) {
			frame += fitted(rowLine(monitor.rows[at]), monitor.columns) + "\r\n";
		}

		monitor.drawn_lines = 2 + (end - monitor.first_shown);
		wbshTerminalWrite(monitor.terminal, frame.data(), frame.size());
	}

	/* ---- keys -------------------------------------------------------- */

	static void scroll(Monitor& monitor, long delta) {
		const long last_first = monitor.rows.size() > monitor.visible_rows
			? static_cast<long>(monitor.rows.size() - monitor.visible_rows) : 0;
		const long target = std::clamp(static_cast<long>(monitor.first_shown) + delta,
			0L, last_first);
		monitor.first_shown = static_cast<std::size_t>(target);
	}

	static bool changeSort(Monitor& monitor, const WbshKey& key) {
		if (key.kind != WBSH_KEY_CHAR || key.modifiers != 0) return false;

		if (std::strcmp(key.text, "m") == 0) monitor.sort = SortBy::Memory;
		else if (std::strcmp(key.text, "n") == 0) monitor.sort = SortBy::Name;
		else if (std::strcmp(key.text, "p") == 0) monitor.sort = SortBy::Pid;
		else return false;

		monitor.first_shown = 0;
		sortRows(monitor.rows, monitor.sort);
		return true;
	}

	static bool navigate(Monitor& monitor, const WbshKey& key) {
		const long page = static_cast<long>(monitor.visible_rows);

		switch (key.kind) {
		case WBSH_KEY_UP:        scroll(monitor, -1);       return true;
		case WBSH_KEY_DOWN:      scroll(monitor, 1);        return true;
		case WBSH_KEY_PAGE_UP:   scroll(monitor, -page);    return true;
		case WBSH_KEY_PAGE_DOWN: scroll(monitor, page);     return true;
		case WBSH_KEY_HOME:      scroll(monitor, -(1L << 30)); return true;
		case WBSH_KEY_END:       scroll(monitor, 1L << 30); return true;
		case WBSH_KEY_RESIZE:    fitToWindow(monitor);      return true;
		default:                 return false;
		}
	}

	static bool wantsToQuit(const WbshKey& key) {
		if (key.kind == WBSH_KEY_ESCAPE) return true;
		if (key.kind != WBSH_KEY_CHAR) return false;

		const bool ctrl = (key.modifiers & WBSH_MOD_CTRL) != 0;
		return (key.modifiers == 0 && std::strcmp(key.text, "q") == 0)
			|| (ctrl && std::strcmp(key.text, "c") == 0);
	}

	static bool wantsRefresh(const WbshKey& key) {
		return key.kind == WBSH_KEY_CHAR && key.modifiers == 0 && std::strcmp(key.text, "r") == 0;
	}

	// The timeout is what makes this a monitor: a second with no key is
	// the signal to take a fresh snapshot.
	static int runLoop(Monitor& monitor) {
		fitToWindow(monitor);
		refresh(monitor);
		render(monitor);

		while (true) {
			WbshKey key;
			const int got = wbshTerminalReadKey(monitor.terminal, &key, 1000);
			if (got < 0) return 1;

			if (got == 0 || wantsRefresh(key)) refresh(monitor);
			else if (wantsToQuit(key)) return 0;
			else if (!navigate(monitor, key)) changeSort(monitor, key);

			render(monitor);
		}
	}

	/* ---- the command ------------------------------------------------- */

	static int printOnce() {
		std::vector<Row> rows = snapshot();
		sortRows(rows, SortBy::Memory);

		wbshPrint((headingLine() + "\n").c_str());
		for (const Row& row : rows) wbshPrint((rowLine(row) + "\n").c_str());
		return 0;
	}

	static int runMonitor() {
		Monitor monitor;
		monitor.terminal = wbshTerminalOpen();
		if (monitor.terminal == nullptr) return printOnce();

		const int status = runLoop(monitor);
		const std::string clear = erasePrevious(monitor);
		wbshTerminalWrite(monitor.terminal, clear.data(), clear.size());
		wbshTerminalClose(monitor.terminal);
		return status;
	}

	static int usage() {
		wbshPrint("usage: procs          watch the process table, q to quit\n"
		          "       procs --once   print it once and exit\n");
		return 0;
	}

	static int command(void*, int argc, const char* const* argv) {
		if (argc > 1 && std::strcmp(argv[1], "--help") == 0) return usage();
		if (argc > 1 && std::strcmp(argv[1], "--once") == 0) return printOnce();
		if (argc > 1) { wbshPrintError("procs: unknown option\n"); return 2; }

		return wbshIsTerminal(1) ? runMonitor() : printOnce();
	}

	/* ---- the segment ------------------------------------------------- */

	static std::atomic<int> g_process_count{0};
	static std::atomic<bool> g_stop_counting{false};
	static std::thread g_counter;
	static char g_segment[32] = "";

	static int countProcesses() {
		const HANDLE list = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (list == INVALID_HANDLE_VALUE) return 0;

		int count = 0;
		PROCESSENTRY32W entry{};
		entry.dwSize = sizeof(entry);
		for (BOOL more = ::Process32FirstW(list, &entry); more;
				more = ::Process32NextW(list, &entry)) {
			++count;
		}

		::CloseHandle(list);
		return count;
	}

	static void keepCounting() {
		while (!g_stop_counting.load()) {
			g_process_count.store(countProcesses());
			for (int waited = 0; waited < 20 && !g_stop_counting.load(); ++waited) ::Sleep(100);
		}
	}

	// Only the paint thread ever touches the text; the worker leaves one
	// integer behind and nothing else, so no lock is needed on either side.
	static const char* segment(void*) {
		std::snprintf(g_segment, sizeof(g_segment), "procs: %d", g_process_count.load());
		return g_segment;
	}

	static void startCounting() {
		g_stop_counting.store(false);
		g_counter = std::thread(keepCounting);
	}

	static void stopCounting() {
		g_stop_counting.store(true);
		if (g_counter.joinable()) g_counter.join();
	}

}  /* namespace procs */

static const WbshUtilInfo kInfo = {
	WBSH_SDK_ABI,
	"procs",
	"1.0.0",
	"A live process monitor, and a process count in the status bar."
};

extern "C" {

WBSH_UTIL_API const WbshUtilInfo* wbshUtilDescribe(void) {
	return &kInfo;
}

WBSH_UTIL_API int wbshUtilLoad(WbshHostKind host) {
	if (host == WBSH_HOST_SHELL) wbshRegisterCommand("procs", procs::command, nullptr);

	if (host == WBSH_HOST_TERMINAL
		&& wbshRegisterStatusSegment("procs", procs::segment, nullptr) == WBSH_OK) {
		procs::startCounting();
	}

	return WBSH_OK;
}

WBSH_UTIL_API void wbshUtilUnload(void) {
	procs::stopCounting();
}

}  /* extern "C" */

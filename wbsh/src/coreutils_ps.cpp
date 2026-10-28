/**
 * @file coreutils_ps.cpp
 * @brief ps with the procps selection and output-format options, plus the
 *        process listing and command-line reader pgrep / pkill share.
 *
 * Processes come from a Toolhelp snapshot (pid, parent, threads, image
 * name). Owner, CPU time and memory come from WTSEnumerateProcessesEx,
 * which reports them for every process without opening it, so they are
 * known even for processes of other users. Start time and the command
 * line need the process opened for limited query; when Windows refuses,
 * the start time shows as `?` and the command line falls back to the
 * image name. Windows has no terminals in the POSIX sense, so TTY is `?`
 * and every process is selected unless a selection option narrows it.
 */

#include "coreutils_internal.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <tlhelp32.h>
#  include <wtsapi32.h>
#  pragma comment(lib, "wtsapi32.lib")
#endif /* _WIN32 */

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#include "executor.h"
#include "numparse.h"
#include "optscan.h"
#include "pathconv.h"

namespace wbsh {

#ifdef _WIN32

	namespace ps_detail {

		static const ULONG          kProcessCommandLineInformation = 60;
		static const std::int64_t   kTicksPerSecond = 10000000;
		static const std::int64_t   kSecondsPerDay = 86400;
		static const std::uint64_t  kBytesPerKibibyte = 1024;
		static const std::uint64_t  kPageKibibytes = 4;
		static const std::size_t    kTextCapacity = 64;

		enum PsOptionId {
			kOptAll = 1, kOptFull, kOptExtraFull, kOptLong, kOptFormat, kOptPid, kOptPpid,
			kOptCommand, kOptUser, kOptDeselect, kOptNoHeaders, kOptSort, kOptIgnored,
		};

		enum class Layout { Default, Full, ExtraFull, Long, User, Custom };

		struct Column {
			std::string key;
			std::string header;
		};

		struct PsOptions {
			bool all = false;
			bool deselect = false;
			bool headers = true;
			Layout layout = Layout::Default;
			std::vector<Column> columns;
			std::vector<unsigned long> pids;
			std::vector<unsigned long> ppids;
			std::vector<std::string> commands;
			std::vector<std::string> users;
			std::vector<std::string> sort_keys;
		};

		using Usage = ProcessUsage;

		struct Row {
			ProcessInfo info;
			Usage usage;
			bool have_usage = false;
			bool probed_times = false;
			bool have_times = false;
			std::int64_t created = 0;
			std::int64_t cpu_ticks = 0;
			bool have_args = false;
			std::string args;
		};

		struct Context {
			std::int64_t now = 0;
			std::uint64_t total_memory = 0;
			unsigned long self = 0;
			std::string self_user;
		};

		using TextFn = std::string (*)(Context&, Row&);
		using NumberFn = double (*)(Context&, Row&);

		struct ColumnSpec {
			const char* key;
			const char* header;
			bool right;
			TextFn text;
			NumberFn number;
		};

		struct CommandLineString {
			USHORT length;
			USHORT capacity;
			PWSTR buffer;
		};

		using QueryInformationFn = LONG (NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

		static std::string lowerAscii(std::string text) {
			for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return text;
		}

		static std::string nameStem(const std::string& name) {
			const std::string lower = lowerAscii(name);
			const bool exe = lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".exe") == 0;
			return exe ? name.substr(0, name.size() - 4) : name;
		}

		static std::int64_t fileTimeTicks(const FILETIME& time) {
			ULARGE_INTEGER value;
			value.LowPart = time.dwLowDateTime;
			value.HighPart = time.dwHighDateTime;
			return static_cast<std::int64_t>(value.QuadPart);
		}

		static std::int64_t currentTicks() {
			FILETIME now{};
			::GetSystemTimeAsFileTime(&now);
			return fileTimeTicks(now);
		}

		static QueryInformationFn queryInformationProcess() {
			static const QueryInformationFn query = reinterpret_cast<QueryInformationFn>(
				reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"),
					"NtQueryInformationProcess")));
			return query;
		}

		static std::string readCommandLine(HANDLE process) {
			const QueryInformationFn query = queryInformationProcess();
			if (query == nullptr) return std::string();

			ULONG needed = 0;
			query(process, kProcessCommandLineInformation, nullptr, 0, &needed);
			if (needed < sizeof(CommandLineString)) return std::string();

			std::vector<unsigned char> buffer(needed);
			const LONG status = query(process, kProcessCommandLineInformation, buffer.data(),
				needed, &needed);
			if (status < 0) return std::string();

			const auto* text = reinterpret_cast<const CommandLineString*>(buffer.data());
			if (text->buffer == nullptr) return std::string();
			return wideToUtf8(std::wstring(text->buffer, text->length / sizeof(wchar_t)));
		}

		static std::string sidName(PSID sid, std::map<std::string, std::string>& cache) {
			if (sid == nullptr || !::IsValidSid(sid)) return "?";

			const std::string key(reinterpret_cast<const char*>(sid), ::GetLengthSid(sid));
			const auto hit = cache.find(key);
			if (hit != cache.end()) return hit->second;

			wchar_t name[256];
			wchar_t domain[256];
			DWORD name_length = 256;
			DWORD domain_length = 256;
			SID_NAME_USE use;
			const bool found = ::LookupAccountSidW(nullptr, sid, name, &name_length, domain,
				&domain_length, &use) != 0;
			const std::string user = found ? wideToUtf8(std::wstring(name, name_length)) : "?";
			cache[key] = user;
			return user;
		}

		static Usage usageFrom(const WTS_PROCESS_INFO_EXW& process,
		                       std::map<std::string, std::string>& names) {
			Usage usage;
			usage.user = sidName(process.pUserSid, names);
			usage.working_set = process.WorkingSetSize;
			usage.pagefile = process.PagefileUsage;
			usage.cpu_ticks = process.UserTime.QuadPart + process.KernelTime.QuadPart;
			return usage;
		}

		static std::map<unsigned long, Usage> collectUsage() {
			std::map<unsigned long, Usage> usage;
			std::map<std::string, std::string> names;
			DWORD level = 1;
			WTS_PROCESS_INFO_EXW* processes = nullptr;
			DWORD count = 0;
			const BOOL listed = ::WTSEnumerateProcessesExW(WTS_CURRENT_SERVER_HANDLE, &level,
				WTS_ANY_SESSION, reinterpret_cast<LPWSTR*>(&processes), &count);
			if (listed == FALSE) return usage;

			for (DWORD i = 0; i < count; ++i) {
				usage[processes[i].ProcessId] = usageFrom(processes[i], names);
			}

			::WTSFreeMemoryExW(WTSTypeProcessInfoLevel1, processes, count);
			return usage;
		}

		static void probeTimes(Row& row) {
			if (row.probed_times) return;

			row.probed_times = true;
			const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
				row.info.pid);
			if (process == nullptr) return;

			FILETIME created{};
			FILETIME exited{};
			FILETIME kernel{};
			FILETIME user{};
			row.have_times = ::GetProcessTimes(process, &created, &exited, &kernel, &user) != 0;
			::CloseHandle(process);
			if (!row.have_times) return;

			row.created = fileTimeTicks(created);
			if (!row.have_usage) row.cpu_ticks = fileTimeTicks(kernel) + fileTimeTicks(user);
		}

		static std::int64_t cpuTicks(Row& row) {
			if (row.have_usage) return row.usage.cpu_ticks;

			probeTimes(row);
			return row.cpu_ticks;
		}

		static double elapsedSeconds(Context& context, Row& row) {
			probeTimes(row);
			if (!row.have_times) return 0.0;
			return static_cast<double>(context.now - row.created) / kTicksPerSecond;
		}

		static const std::string& rowArgs(Row& row) {
			if (row.have_args) return row.args;

			row.have_args = true;
			const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
				row.info.pid);
			if (process != nullptr) {
				row.args = readCommandLine(process);
				::CloseHandle(process);
			}

			if (row.args.empty()) row.args = row.info.name;
			return row.args;
		}

		static std::string formatClock(long long seconds, bool always_hours) {
			const long long days = seconds / kSecondsPerDay;
			const long long hours = (seconds / 3600) % 24;
			const long long minutes = (seconds / 60) % 60;
			const long long secs = seconds % 60;

			char text[kTextCapacity];
			if (days > 0) {
				std::snprintf(text, sizeof(text), "%lld-%02lld:%02lld:%02lld", days, hours,
					minutes, secs);
			} else if (hours > 0 || always_hours) {
				std::snprintf(text, sizeof(text), "%02lld:%02lld:%02lld", hours, minutes, secs);
			} else {
				std::snprintf(text, sizeof(text), "%02lld:%02lld", minutes, secs);
			}

			return text;
		}

		static std::string formatMinutes(long long seconds) {
			char text[kTextCapacity];
			std::snprintf(text, sizeof(text), "%lld:%02lld", seconds / 60, seconds % 60);
			return text;
		}

		static std::string formatPercent(double value) {
			char text[kTextCapacity];
			std::snprintf(text, sizeof(text), "%.1f", value);
			return text;
		}

		static std::tm localTime(std::int64_t ticks) {
			const std::time_t seconds = static_cast<std::time_t>(
				ticks / kTicksPerSecond - 11644473600LL);
			std::tm local{};
			localtime_s(&local, &seconds);
			return local;
		}

		static std::string strftimeText(const std::tm& time, const char* format) {
			char text[kTextCapacity];
			const std::size_t length = std::strftime(text, sizeof(text), format, &time);
			return std::string(text, length);
		}

		static std::string startText(Context& context, Row& row) {
			probeTimes(row);
			if (!row.have_times) return "?";

			const std::tm started = localTime(row.created);
			const std::tm today = localTime(context.now);
			const bool same_day = started.tm_yday == today.tm_yday
				&& started.tm_year == today.tm_year;
			if (same_day) return strftimeText(started, "%H:%M");
			if (started.tm_year == today.tm_year) return strftimeText(started, "%b%d");
			return strftimeText(started, "%Y");
		}

		static std::string textPid(Context&, Row& row) {
			return std::to_string(row.info.pid);
		}

		static std::string textPpid(Context&, Row& row) {
			return std::to_string(row.info.parent_pid);
		}

		static std::string textUser(Context&, Row& row) {
			return row.have_usage ? row.usage.user : std::string("?");
		}

		static std::string textComm(Context&, Row& row) {
			return nameStem(row.info.name);
		}

		static std::string textArgs(Context&, Row& row) {
			return rowArgs(row);
		}

		static std::string textElapsed(Context& context, Row& row) {
			probeTimes(row);
			if (!row.have_times) return "-";
			return formatClock(static_cast<long long>(elapsedSeconds(context, row)), false);
		}

		static std::string textElapsedSeconds(Context& context, Row& row) {
			probeTimes(row);
			if (!row.have_times) return "-";
			return std::to_string(static_cast<long long>(elapsedSeconds(context, row)));
		}

		static std::string textCpuTime(Context&, Row& row) {
			return formatClock(cpuTicks(row) / kTicksPerSecond, true);
		}

		static std::string textCpuMinutes(Context&, Row& row) {
			return formatMinutes(cpuTicks(row) / kTicksPerSecond);
		}

		static std::string textCpuSeconds(Context&, Row& row) {
			return std::to_string(cpuTicks(row) / kTicksPerSecond);
		}

		static std::string textRss(Context&, Row& row) {
			return std::to_string(row.usage.working_set / kBytesPerKibibyte);
		}

		static std::string textVsz(Context&, Row& row) {
			return std::to_string(row.usage.pagefile / kBytesPerKibibyte);
		}

		static std::string textSize(Context&, Row& row) {
			return std::to_string(row.usage.pagefile / kBytesPerKibibyte / kPageKibibytes);
		}

		static double numberCpuPercent(Context& context, Row& row) {
			const double elapsed = elapsedSeconds(context, row);
			if (elapsed <= 0.0) return 0.0;
			return static_cast<double>(cpuTicks(row)) / kTicksPerSecond * 100.0 / elapsed;
		}

		static std::string textCpuPercent(Context& context, Row& row) {
			return formatPercent(numberCpuPercent(context, row));
		}

		static std::string textCpuWhole(Context& context, Row& row) {
			return std::to_string(static_cast<long long>(numberCpuPercent(context, row)));
		}

		static double numberMemPercent(Context& context, Row& row) {
			if (context.total_memory == 0) return 0.0;
			return static_cast<double>(row.usage.working_set) * 100.0
				/ static_cast<double>(context.total_memory);
		}

		static std::string textMemPercent(Context& context, Row& row) {
			return formatPercent(numberMemPercent(context, row));
		}

		static std::string textStart(Context& context, Row& row) {
			return startText(context, row);
		}

		static std::string textLongStart(Context&, Row& row) {
			probeTimes(row);
			if (!row.have_times) return "?";
			return strftimeText(localTime(row.created), "%a %b %e %H:%M:%S %Y");
		}

		static std::string textTty(Context&, Row&) {
			return "?";
		}

		static std::string textState(Context& context, Row& row) {
			return row.info.pid == context.self ? "R" : "S";
		}

		static std::string textThreads(Context&, Row& row) {
			return std::to_string(row.info.threads);
		}

		static std::string textZero(Context&, Row&) {
			return "0";
		}

		static std::string textNice(Context&, Row& row) {
			return std::to_string(nicenessOfBasePriority(row.info.base_priority));
		}

		static int rowNiceness(const Row& row) {
			return nicenessOfBasePriority(row.info.base_priority);
		}

		static std::string textPri(Context&, Row& row) {
			return std::to_string(19 - rowNiceness(row));
		}

		static std::string textOldPri(Context&, Row& row) {
			return std::to_string(80 + rowNiceness(row));
		}

		static std::string textKernelPriority(Context&, Row& row) {
			return std::to_string(20 + rowNiceness(row));
		}

		static std::string textDash(Context&, Row&) {
			return "-";
		}

		static double numberPid(Context&, Row& row) {
			return static_cast<double>(row.info.pid);
		}

		static double numberPpid(Context&, Row& row) {
			return static_cast<double>(row.info.parent_pid);
		}

		static double numberCpu(Context&, Row& row) {
			return static_cast<double>(cpuTicks(row));
		}

		static double numberRss(Context&, Row& row) {
			return static_cast<double>(row.usage.working_set);
		}

		static double numberVsz(Context&, Row& row) {
			return static_cast<double>(row.usage.pagefile);
		}

		static double numberStart(Context&, Row& row) {
			probeTimes(row);
			return static_cast<double>(row.created);
		}

		static double numberElapsed(Context& context, Row& row) {
			return elapsedSeconds(context, row);
		}

		static double numberThreads(Context&, Row& row) {
			return static_cast<double>(row.info.threads);
		}

		static double numberNice(Context&, Row& row) {
			return static_cast<double>(rowNiceness(row));
		}

		static double numberPri(Context&, Row& row) {
			return static_cast<double>(19 - rowNiceness(row));
		}

		static const ColumnSpec kColumns[] = {
			{ "pid",        "PID",     true,  textPid,            numberPid },
			{ "ppid",       "PPID",    true,  textPpid,           numberPpid },
			{ "user",       "USER",    false, textUser,           nullptr },
			{ "euser",      "EUSER",   false, textUser,           nullptr },
			{ "ruser",      "RUSER",   false, textUser,           nullptr },
			{ "uname",      "USER",    false, textUser,           nullptr },
			{ "uid_name",   "UID",     false, textUser,           nullptr },
			{ "comm",       "COMMAND", false, textComm,           nullptr },
			{ "ucomm",      "COMMAND", false, textComm,           nullptr },
			{ "ucmd",       "CMD",     false, textComm,           nullptr },
			{ "fname",      "COMMAND", false, textComm,           nullptr },
			{ "args",       "COMMAND", false, textArgs,           nullptr },
			{ "cmd",        "CMD",     false, textArgs,           nullptr },
			{ "command",    "COMMAND", false, textArgs,           nullptr },
			{ "etime",      "ELAPSED", true,  textElapsed,        numberElapsed },
			{ "etimes",     "ELAPSED", true,  textElapsedSeconds, numberElapsed },
			{ "time",       "TIME",    true,  textCpuTime,        numberCpu },
			{ "cputime",    "TIME",    true,  textCpuTime,        numberCpu },
			{ "cputimes",   "TIME",    true,  textCpuSeconds,     numberCpu },
			{ "times",      "TIME",    true,  textCpuSeconds,     numberCpu },
			{ "bsdtime",    "TIME",    true,  textCpuMinutes,     numberCpu },
			{ "rss",        "RSS",     true,  textRss,            numberRss },
			{ "rssize",     "RSS",     true,  textRss,            numberRss },
			{ "rsz",        "RSZ",     true,  textRss,            numberRss },
			{ "vsz",        "VSZ",     true,  textVsz,            numberVsz },
			{ "vsize",      "VSZ",     true,  textVsz,            numberVsz },
			{ "sz",         "SZ",      true,  textSize,           numberVsz },
			{ "%cpu",       "%CPU",    true,  textCpuPercent,     numberCpuPercent },
			{ "pcpu",       "%CPU",    true,  textCpuPercent,     numberCpuPercent },
			{ "c",          "C",       true,  textCpuWhole,       numberCpuPercent },
			{ "%mem",       "%MEM",    true,  textMemPercent,     numberMemPercent },
			{ "pmem",       "%MEM",    true,  textMemPercent,     numberMemPercent },
			{ "stime",      "STIME",   false, textStart,          numberStart },
			{ "start_time", "START",   false, textStart,          numberStart },
			{ "bsdstart",   "START",   false, textStart,          numberStart },
			{ "start",      "STARTED", false, textStart,          numberStart },
			{ "lstart",     "STARTED", false, textLongStart,      numberStart },
			{ "tty",        "TT",      false, textTty,            nullptr },
			{ "tt",         "TT",      false, textTty,            nullptr },
			{ "tname",      "TTY",     false, textTty,            nullptr },
			{ "stat",       "STAT",    false, textState,          nullptr },
			{ "s",          "S",       false, textState,          nullptr },
			{ "state",      "S",       false, textState,          nullptr },
			{ "nlwp",       "NLWP",    true,  textThreads,        numberThreads },
			{ "thcount",    "THCNT",   true,  textThreads,        numberThreads },
			{ "pri",        "PRI",     true,  textPri,            numberPri },
			{ "opri",       "PRI",     true,  textOldPri,         numberNice },
			{ "intpri",     "PRI",     true,  textOldPri,         numberNice },
			{ "priority",   "PRI",     true,  textKernelPriority, numberNice },
			{ "ni",         "NI",      true,  textNice,           numberNice },
			{ "nice",       "NI",      true,  textNice,           numberNice },
			{ "psr",        "PSR",     true,  textZero,           nullptr },
			{ "f",          "F",       true,  textZero,           nullptr },
			{ "flags",      "F",       true,  textZero,           nullptr },
			{ "addr",       "ADDR",    true,  textDash,           nullptr },
			{ "wchan",      "WCHAN",   false, textDash,           nullptr },
		};

		static const ColumnSpec* findColumn(const std::string& key) {
			const std::string lower = lowerAscii(key);
			for (const ColumnSpec& spec : kColumns) {
				if (lower == spec.key) return &spec;
			}

			return nullptr;
		}

		static void addLayoutColumns(PsOptions& options, const char* const* keys) {
			for (; *keys != nullptr; ++keys) {
				const ColumnSpec* spec = findColumn(*keys);
				options.columns.push_back({ spec->key, spec->header });
			}
		}

		static void applyLayout(PsOptions& options) {
			static const char* const kDefault[] = { "pid", "ppid", "user", "bsdtime", "comm",
				nullptr };
			static const char* const kFull[] = { "uid_name", "pid", "ppid", "c", "stime", "tname",
				"time", "cmd", nullptr };
			static const char* const kExtraFull[] = { "uid_name", "pid", "ppid", "c", "sz",
				"rss", "psr", "stime", "tname", "time", "cmd", nullptr };
			static const char* const kLong[] = { "f", "s", "uid_name", "pid", "ppid", "c", "opri",
				"ni", "addr", "sz", "wchan", "tname", "time", "ucmd", nullptr };
			static const char* const kUser[] = { "user", "pid", "%cpu", "%mem", "vsz", "rss",
				"tname", "stat", "bsdstart", "bsdtime", "command", nullptr };

			switch (options.layout) {
				case Layout::Full:      addLayoutColumns(options, kFull);      break;
				case Layout::ExtraFull: addLayoutColumns(options, kExtraFull); break;
				case Layout::Long:      addLayoutColumns(options, kLong);      break;
				case Layout::User:      addLayoutColumns(options, kUser);      break;
				case Layout::Custom:    break;
				default:                addLayoutColumns(options, kDefault);   break;
			}
		}

		static bool psFail(const std::string& message) {
			std::fprintf(stderr, "wbsh: ps: %s\n", message.c_str());
			return false;
		}

		static std::vector<std::string> splitList(const std::string& text) {
			std::vector<std::string> items;
			std::string current;
			for (char c : text) {
				if (c != ',' && c != ' ' && c != '\t') {
					current.push_back(c);
					continue;
				}

				if (!current.empty()) items.push_back(current);
				current.clear();
			}

			if (!current.empty()) items.push_back(current);
			return items;
		}

		static bool parseIdList(const std::string& text, std::vector<unsigned long>& ids) {
			const std::vector<std::string> items = splitList(text);
			for (const std::string& item : items) {
				unsigned long id = 0;
				std::size_t used = 0;
				if (!parseUL(item, id, 10, &used) || used != item.size()) {
					return psFail("process ID list syntax error");
				}

				ids.push_back(id);
			}

			return !items.empty() || psFail("process ID list syntax error");
		}

		static bool addFormatColumn(const std::string& item, PsOptions& options) {
			const std::size_t eq = item.find('=');
			const std::string key = item.substr(0, eq);
			const ColumnSpec* spec = findColumn(key);
			if (spec == nullptr) {
				return psFail("unknown user-defined format specifier \"" + key + "\"");
			}

			const std::string header = eq == std::string::npos ? spec->header : item.substr(eq + 1);
			options.columns.push_back({ spec->key, header });
			return true;
		}

		static bool parseFormat(const std::string& text, PsOptions& options) {
			options.layout = Layout::Custom;
			std::size_t start = 0;
			while (start < text.size()) {
				const std::size_t eq = text.find('=', start);
				const std::size_t comma = text.find_first_of(", ", start);
				const bool header_to_end = eq != std::string::npos && eq < comma;
				const std::size_t end = header_to_end || comma == std::string::npos
					? text.size() : comma;
				if (end > start && !addFormatColumn(text.substr(start, end - start), options)) {
					return false;
				}

				start = end + 1;
			}

			return true;
		}

		static void addSortKeys(const std::string& text, PsOptions& options) {
			for (const std::string& key : splitList(text)) options.sort_keys.push_back(key);
		}

		static const std::vector<OptDef>& psOptionDefs() {
			static const std::vector<OptDef> defs = {
				{ kOptAll,       'e', nullptr,      OptArg::None },
				{ kOptAll,       'A', nullptr,      OptArg::None },
				{ kOptAll,       'a', nullptr,      OptArg::None },
				{ kOptAll,       'd', nullptr,      OptArg::None },
				{ kOptAll,       'x', nullptr,      OptArg::None },
				{ kOptFull,      'f', nullptr,      OptArg::None },
				{ kOptExtraFull, 'F', nullptr,      OptArg::None },
				{ kOptLong,      'l', nullptr,      OptArg::None },
				{ kOptFormat,    'o', "format",     OptArg::Required },
				{ kOptPid,       'p', "pid",        OptArg::Required },
				{ kOptPid,       'q', "quick-pid",  OptArg::Required },
				{ kOptPpid,      0,   "ppid",       OptArg::Required },
				{ kOptCommand,   'C', nullptr,      OptArg::Required },
				{ kOptUser,      'u', "user",       OptArg::Required },
				{ kOptUser,      'U', "User",       OptArg::Required },
				{ kOptDeselect,  'N', "deselect",   OptArg::None },
				{ kOptNoHeaders, 0,   "no-headers", OptArg::None },
				{ kOptNoHeaders, 0,   "no-heading", OptArg::None },
				{ kOptSort,      0,   "sort",       OptArg::Required },
				{ kOptIgnored,   'w', "width",      OptArg::Optional },
				{ kOptIgnored,   'H', "forest",     OptArg::None },
				{ kOptIgnored,   'y', nullptr,      OptArg::None },
			};
			return defs;
		}

		static void applyFullFlag(PsOptions& options, Layout layout) {
			if (options.layout != Layout::Custom) options.layout = layout;
		}

		static bool applyPsHit(const OptHit& hit, PsOptions& options) {
			switch (hit.id) {
				case kOptAll:       options.all = true;                         return true;
				case kOptFull:      applyFullFlag(options, Layout::Full);       return true;
				case kOptExtraFull: applyFullFlag(options, Layout::ExtraFull);  return true;
				case kOptLong:      applyFullFlag(options, Layout::Long);       return true;
				case kOptFormat:    return parseFormat(hit.value, options);
				case kOptPid:       return parseIdList(hit.value, options.pids);
				case kOptPpid:      return parseIdList(hit.value, options.ppids);
				case kOptCommand:   options.commands = splitList(hit.value);    return true;
				case kOptUser:      options.users = splitList(hit.value);       return true;
				case kOptDeselect:  options.deselect = true;                    return true;
				case kOptNoHeaders: options.headers = false;                    return true;
				case kOptSort:      addSortKeys(hit.value, options);            return true;
				default:            return true;
			}
		}

		static bool bsdTakesValue(char letter) {
			return std::strchr("opUkO", letter) != nullptr;
		}

		static bool applyBsdValue(char letter, const std::string& value, PsOptions& options) {
			switch (letter) {
				case 'o': return parseFormat(value, options);
				case 'O': return parseFormat("pid," + value, options);
				case 'p': return parseIdList(value, options.pids);
				case 'U': options.users = splitList(value);  return true;
				case 'k': addSortKeys(value, options);       return true;
				default:  return true;
			}
		}

		static bool applyBsdLetter(char letter, PsOptions& options) {
			switch (letter) {
				case 'a':
				case 'x':
				case 'e':
				case 'r':
				case 'g': options.all = true;                       return true;
				case 'u': applyFullFlag(options, Layout::User);     return true;
				case 'w':
				case 'f':
				case 'h': options.headers = letter != 'h';          return true;
				case 'l': applyFullFlag(options, Layout::Long);     return true;
				default:  return psFail(std::string("unsupported option (BSD syntax): ") + letter);
			}
		}

		static bool applyBsdCluster(const std::vector<std::string>& operands, std::size_t& i,
		                            PsOptions& options) {
			const std::string& cluster = operands[i];
			for (std::size_t k = 0; k < cluster.size(); ++k) {
				if (!bsdTakesValue(cluster[k])) {
					if (!applyBsdLetter(cluster[k], options)) return false;
					continue;
				}

				const bool attached = k + 1 < cluster.size();
				if (!attached && i + 1 >= operands.size()) {
					return psFail(std::string("option ") + cluster[k] + " requires an argument");
				}

				const std::string value = attached ? cluster.substr(k + 1) : operands[++i];
				return applyBsdValue(cluster[k], value, options);
			}

			return true;
		}

		static bool applyOperands(const std::vector<std::string>& operands, PsOptions& options) {
			for (std::size_t i = 0; i < operands.size(); ++i) {
				const std::string& operand = operands[i];
				const bool numeric = !operand.empty() && std::isdigit(static_cast<unsigned char>(
					operand[0]));
				const bool ok = numeric ? parseIdList(operand, options.pids)
					: applyBsdCluster(operands, i, options);
				if (!ok) return false;
			}

			return true;
		}

		static bool parsePsArgs(const std::vector<std::string>& args, PsOptions& options) {
			OptScanResult scanned;
			if (!scanOptions("ps", args, psOptionDefs(), scanned)) return false;

			for (const OptHit& hit : scanned.hits) {
				if (!applyPsHit(hit, options)) return false;
			}

			if (!applyOperands(scanned.operands, options)) return false;
			if (options.columns.empty()) applyLayout(options);
			return true;
		}

		static bool containsId(const std::vector<unsigned long>& ids, unsigned long id) {
			return std::find(ids.begin(), ids.end(), id) != ids.end();
		}

		static bool containsName(const std::vector<std::string>& names, const std::string& name) {
			const std::string lower = lowerAscii(name);
			for (const std::string& wanted : names) {
				if (lowerAscii(wanted) == lower) return true;
			}

			return false;
		}

		static bool matchesUser(const std::vector<std::string>& users, const Context& context,
		                        const Row& row) {
			if (!row.have_usage) return false;
			if (containsName(users, row.usage.user)) return true;

			const bool mine = lowerAscii(row.usage.user) == lowerAscii(context.self_user);
			return mine && containsName(users, "1000");
		}

		static bool hasSelection(const PsOptions& options) {
			return !options.pids.empty() || !options.ppids.empty() || !options.commands.empty()
				|| !options.users.empty();
		}

		static bool matchesSelection(const PsOptions& options, const Context& context,
		                             const Row& row) {
			if (containsId(options.pids, row.info.pid)) return true;
			if (containsId(options.ppids, row.info.parent_pid)) return true;

			const bool named = containsName(options.commands, row.info.name)
				|| containsName(options.commands, nameStem(row.info.name));
			if (named) return true;

			return !options.users.empty() && matchesUser(options.users, context, row);
		}

		static bool isSelected(const PsOptions& options, const Context& context, const Row& row) {
			const bool hit = !hasSelection(options) || matchesSelection(options, context, row);
			return hit != options.deselect;
		}

		struct SortValue {
			double number = 0.0;
			std::string text;
		};

		struct SortKey {
			const ColumnSpec* spec = nullptr;
			bool descending = false;
		};

		struct SortedRow {
			std::vector<SortValue> values;
			std::size_t index = 0;
		};

		static Context makeContext(Executor& exec) {
			Context context;
			context.now = currentTicks();
			context.self = ::GetCurrentProcessId();
			context.self_user = currentUserName(exec);

			MEMORYSTATUSEX memory{};
			memory.dwLength = sizeof(memory);
			if (::GlobalMemoryStatusEx(&memory)) context.total_memory = memory.ullTotalPhys;
			return context;
		}

		static std::vector<Row> collectRows(Context& context, const PsOptions& options) {
			const std::map<unsigned long, Usage> usage = collectUsage();
			std::vector<Row> rows;
			for (const ProcessInfo& info : listProcesses()) {
				Row row;
				row.info = info;
				const auto hit = usage.find(info.pid);
				row.have_usage = hit != usage.end();
				if (row.have_usage) row.usage = hit->second;
				if (isSelected(options, context, row)) rows.push_back(row);
			}

			std::sort(rows.begin(), rows.end(),
				[](const Row& a, const Row& b) { return a.info.pid < b.info.pid; });
			return rows;
		}

		static bool parseSortKey(const std::string& text, SortKey& key) {
			std::string name = text;
			if (!name.empty() && (name[0] == '-' || name[0] == '+')) {
				key.descending = name[0] == '-';
				name.erase(0, 1);
			}

			key.spec = findColumn(name);
			if (key.spec != nullptr) return true;
			return psFail("unknown sort specifier \"" + text + "\"");
		}

		static SortValue sortValue(Context& context, const SortKey& key, Row& row) {
			SortValue value;
			if (key.spec->number != nullptr) value.number = key.spec->number(context, row);
			else                             value.text = key.spec->text(context, row);
			return value;
		}

		static int compareValues(const SortValue& a, const SortValue& b) {
			if (a.number != b.number) return a.number < b.number ? -1 : 1;
			return a.text.compare(b.text);
		}

		static bool sortedBefore(const std::vector<SortKey>& keys, const SortedRow& a,
		                         const SortedRow& b) {
			for (std::size_t k = 0; k < keys.size(); ++k) {
				const int order = compareValues(a.values[k], b.values[k]);
				if (order != 0) return keys[k].descending ? order > 0 : order < 0;
			}

			return false;
		}

		static bool sortRows(Context& context, const PsOptions& options, std::vector<Row>& rows) {
			std::vector<SortKey> keys(options.sort_keys.size());
			for (std::size_t k = 0; k < keys.size(); ++k) {
				if (!parseSortKey(options.sort_keys[k], keys[k])) return false;
			}

			if (keys.empty()) return true;

			std::vector<SortedRow> sorted(rows.size());
			for (std::size_t i = 0; i < rows.size(); ++i) {
				sorted[i].index = i;
				for (const SortKey& key : keys) {
					sorted[i].values.push_back(sortValue(context, key, rows[i]));
				}
			}

			const auto before = [&keys](const SortedRow& a, const SortedRow& b) {
				return sortedBefore(keys, a, b);
			};
			std::stable_sort(sorted.begin(), sorted.end(), before);

			std::vector<Row> reordered;
			for (const SortedRow& entry : sorted) reordered.push_back(rows[entry.index]);
			rows.swap(reordered);
			return true;
		}

		using Cells = std::vector<std::vector<std::string>>;

		static Cells renderCells(Context& context, const PsOptions& options,
		                         std::vector<Row>& rows) {
			Cells cells;
			for (Row& row : rows) {
				std::vector<std::string> line;
				for (const Column& column : options.columns) {
					line.push_back(findColumn(column.key)->text(context, row));
				}

				cells.push_back(line);
			}

			return cells;
		}

		static bool wantsHeader(const PsOptions& options) {
			if (!options.headers) return false;
			for (const Column& column : options.columns) {
				if (!column.header.empty()) return true;
			}

			return false;
		}

		static std::vector<std::size_t> columnWidths(const PsOptions& options, const Cells& cells,
		                                             bool header) {
			std::vector<std::size_t> widths(options.columns.size(), 0);
			for (std::size_t c = 0; c < widths.size(); ++c) {
				if (header) widths[c] = options.columns[c].header.size();
				for (const std::vector<std::string>& line : cells) {
					widths[c] = std::max(widths[c], line[c].size());
				}
			}

			return widths;
		}

		static void printLine(const PsOptions& options, const std::vector<std::string>& line,
		                      const std::vector<std::size_t>& widths) {
			std::string out;
			for (std::size_t c = 0; c < line.size(); ++c) {
				const bool last = c + 1 == line.size();
				const std::string padding(widths[c] - std::min(widths[c], line[c].size()), ' ');
				const bool right = findColumn(options.columns[c].key)->right;
				if (c != 0) out.push_back(' ');
				if (right) out += padding;
				out += line[c];
				if (!right && !last) out += padding;
			}

			std::printf("%s\n", out.c_str());
		}

		static void printTable(Context& context, const PsOptions& options, std::vector<Row>& rows) {
			const Cells cells = renderCells(context, options, rows);
			const bool header = wantsHeader(options);
			const std::vector<std::size_t> widths = columnWidths(options, cells, header);

			if (header) {
				std::vector<std::string> titles;
				for (const Column& column : options.columns) titles.push_back(column.header);
				printLine(options, titles, widths);
			}

			for (const std::vector<std::string>& line : cells) printLine(options, line, widths);
			std::fflush(stdout);
		}

		static int builtin_ps(Executor& exec, const std::vector<std::string>& args) {
			PsOptions options;
			if (!parsePsArgs(args, options)) return 1;

			Context context = makeContext(exec);
			std::vector<Row> rows = collectRows(context, options);
			if (!sortRows(context, options, rows)) return 1;

			printTable(context, options, rows);
			return rows.empty() ? 1 : 0;
		}

	}  // namespace ps_detail

	std::vector<ProcessInfo> listProcesses() {
		std::vector<ProcessInfo> processes;
		const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE) return processes;

		PROCESSENTRY32W entry{};
		entry.dwSize = sizeof(entry);
		for (BOOL more = ::Process32FirstW(snapshot, &entry); more != 0;
		     more = ::Process32NextW(snapshot, &entry)) {
			ProcessInfo info;
			info.pid = entry.th32ProcessID;
			info.parent_pid = entry.th32ParentProcessID;
			info.threads = entry.cntThreads;
			info.base_priority = entry.pcPriClassBase;
			info.name = wideToUtf8(entry.szExeFile);
			processes.push_back(info);
		}

		::CloseHandle(snapshot);
		return processes;
	}

	std::map<unsigned long, ProcessUsage> collectProcessUsage() {
		return ps_detail::collectUsage();
	}

	std::string processCommandLine(unsigned long pid) {
		const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (process == nullptr) return std::string();

		const std::string line = ps_detail::readCommandLine(process);
		::CloseHandle(process);
		return line;
	}

	void registerPsBuiltin(Executor& exec) {
		exec.registerBuiltin("ps", ps_detail::builtin_ps);
	}

#else

	void registerPsBuiltin(Executor&) {}

#endif /* _WIN32 */

}  // namespace wbsh

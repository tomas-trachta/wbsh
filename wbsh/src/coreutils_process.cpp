/**
 * @file coreutils_process.cpp
 * @brief Process and system-state builtins: ps, pgrep, pkill, pidof,
 *        killall, timeout, watch, uptime, free.
 *
 * Windows has no signals to deliver, so every "signal" a user names is
 * accepted for compatibility and ends the target the only way Windows
 * can: TerminateProcess, with 128 + the signal number as its exit code.
 * Signal 0 delivers nothing and only checks that the target is running.
 */

#include "coreutils_internal.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <psapi.h>
#  pragma comment(lib, "psapi.lib")
#  include <tlhelp32.h>
#endif /* _WIN32 */

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "executor.h"
#include "exitstatus.h"
#include "interrupt.h"
#include "numparse.h"
#include "pathconv.h"
#include "regexutil.h"
#include "termreq.h"

namespace wbsh {

#ifdef _WIN32

	namespace process_detail {

		struct ProcessInfo {
			DWORD pid = 0;
			DWORD parent_pid = 0;
			std::string name;
		};

		struct SignalName {
			const char* name;
			int number;
		};

		static const SignalName kSignalNames[] = {
			{ "HUP",  1  }, { "INT",  2  }, { "QUIT", 3  }, { "ILL",  4  },
			{ "TRAP", 5  }, { "ABRT", 6  }, { "BUS",  7  }, { "FPE",  8  },
			{ "KILL", 9  }, { "USR1", 10 }, { "SEGV", 11 }, { "USR2", 12 },
			{ "PIPE", 13 }, { "ALRM", 14 }, { "TERM", 15 },
		};

		struct MatchOptions {
			bool exact = false;
			bool ignore_case = false;
			bool invert = false;
			bool newest = false;
			bool oldest = false;
			bool list_name = false;
			int signal = 15;
			std::string delimiter = "\n";
			std::string pattern;
		};

		struct TimeoutOptions {
			double seconds = 0.0;
			int signal = 15;
			bool preserve_status = false;
			bool verbose = false;
			std::vector<std::string> command;
		};

		struct WatchOptions {
			double interval = 2.0;
			bool show_header = true;
			std::string command;
		};

		struct PageFileTotals {
			unsigned long long total_pages = 0;
			unsigned long long used_pages = 0;
		};

	}  // namespace process_detail

	using process_detail::MatchOptions;
	using process_detail::ProcessInfo;
	using process_detail::TimeoutOptions;
	using process_detail::WatchOptions;

	static const int    kStatusTimedOut       = 124;
	static const int    kStatusTimeoutFailed  = 125;
	static const int    kStatusCannotInvoke   = 126;
	static const int    kStatusNotFound       = 127;
	static const int    kSignalKill           = 9;
	static const int    kSignalTerm           = 15;
	static const DWORD  kWatchPollMs          = 50;
	static const double kBytesPerKibibyte     = 1024.0;
	static const unsigned long long kMsPerSecond = 1000ULL;

	static std::string upperAscii(std::string text) {
		for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		return text;
	}

	static std::string lowerAscii(std::string text) {
		for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return text;
	}

	static bool parseSignalName(const std::string& text, int& signum) {
		if (parseInt(text, signum)) return signum >= 0;

		std::string name = upperAscii(text);
		if (name.compare(0, 3, "SIG") == 0) name = name.substr(3);
		for (const auto& entry : process_detail::kSignalNames) {
			if (name != entry.name) continue;
			signum = entry.number;
			return true;
		}

		return false;
	}

	static std::string processNameStem(const std::string& name) {
		const std::string lower = lowerAscii(name);
		if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".exe") == 0) {
			return name.substr(0, name.size() - 4);
		}

		return name;
	}

	static std::vector<ProcessInfo> listProcesses() {
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
			info.name = wideToUtf8(entry.szExeFile);
			processes.push_back(info);
		}

		::CloseHandle(snapshot);
		return processes;
	}

	ProcessProbe probeProcess(unsigned long pid) {
		const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
			static_cast<DWORD>(pid));
		if (process == nullptr) {
			return ::GetLastError() == ERROR_ACCESS_DENIED ? ProcessProbe::Denied : ProcessProbe::Missing;
		}

		DWORD code = 0;
		const bool alive = ::GetExitCodeProcess(process, &code) != 0 && code == STILL_ACTIVE;
		::CloseHandle(process);
		return alive ? ProcessProbe::Alive : ProcessProbe::Missing;
	}

	static bool terminateProcessId(DWORD pid, int signum) {
		if (signum == 0) return probeProcess(pid) == ProcessProbe::Alive;

		const HANDLE process = ::OpenProcess(PROCESS_TERMINATE, FALSE, pid);
		if (process == nullptr) return false;

		const bool terminated = ::TerminateProcess(process, static_cast<UINT>(128 + signum)) != 0;
		::CloseHandle(process);
		return terminated;
	}

	static unsigned long long fileTimeToMs(const FILETIME& time) {
		ULARGE_INTEGER value;
		value.LowPart = time.dwLowDateTime;
		value.HighPart = time.dwHighDateTime;
		return value.QuadPart / 10000ULL;
	}

	// Creation time doubles as the "start order" pgrep -n / -o sort by.
	static bool processTimes(DWORD pid, unsigned long long& cpu_ms,
	                         unsigned long long& created_ms) {
		const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (process == nullptr) return false;

		FILETIME created{};
		FILETIME exited{};
		FILETIME kernel{};
		FILETIME user{};
		const bool ok = ::GetProcessTimes(process, &created, &exited, &kernel, &user) != 0;
		::CloseHandle(process);
		if (!ok) return false;

		cpu_ms = fileTimeToMs(kernel) + fileTimeToMs(user);
		created_ms = fileTimeToMs(created);
		return true;
	}

	static std::string sidAccountName(PSID sid) {
		wchar_t name[256];
		wchar_t domain[256];
		DWORD name_length = 256;
		DWORD domain_length = 256;
		SID_NAME_USE use;
		if (::LookupAccountSidW(nullptr, sid, name, &name_length, domain, &domain_length,
		                        &use) == 0) {
			return "?";
		}

		return wideToUtf8(std::wstring(name, name_length));
	}

	static std::string tokenUserName(HANDLE token) {
		DWORD needed = 0;
		::GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
		if (needed == 0) return "?";

		std::vector<unsigned char> buffer(needed);
		if (::GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed) == 0) {
			return "?";
		}

		const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
		return sidAccountName(user->User.Sid);
	}

	static std::string processUserName(DWORD pid) {
		const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (process == nullptr) return "?";

		HANDLE token = nullptr;
		std::string user = "?";
		if (::OpenProcessToken(process, TOKEN_QUERY, &token) != 0) {
			user = tokenUserName(token);
			::CloseHandle(token);
		}

		::CloseHandle(process);
		return user;
	}

	static std::string formatCpuTime(unsigned long long cpu_ms) {
		const unsigned long long total_seconds = cpu_ms / kMsPerSecond;
		char text[32];
		std::snprintf(text, sizeof(text), "%llu:%02llu", total_seconds / 60ULL,
			total_seconds % 60ULL);
		return text;
	}

	static void printProcessRow(const ProcessInfo& process) {
		unsigned long long cpu_ms = 0;
		unsigned long long created_ms = 0;
		const bool has_times = processTimes(process.pid, cpu_ms, created_ms);
		const std::string user = processUserName(process.pid);
		const std::string time = has_times ? formatCpuTime(cpu_ms) : "0:00";
		std::printf("%6lu %6lu %-12.12s %7s %s\n",
			static_cast<unsigned long>(process.pid),
			static_cast<unsigned long>(process.parent_pid),
			user.c_str(), time.c_str(), process.name.c_str());
	}

	// Every option is accepted and ignored: Windows shows every process to
	// whoever may query it, so `ps`, `ps aux` and `ps -ef` print the same list.
	static int builtin_ps(Executor&, const std::vector<std::string>&) {
		std::printf("%6s %6s %-12s %7s %s\n", "PID", "PPID", "USER", "TIME", "COMMAND");
		for (const ProcessInfo& process : listProcesses()) printProcessRow(process);
		std::fflush(stdout);
		return 0;
	}

	static bool applyMatchFlag(char flag, MatchOptions& options) {
		switch (flag) {
		case 'x': options.exact = true;       return true;
		case 'i': options.ignore_case = true; return true;
		case 'v': options.invert = true;      return true;
		case 'n': options.newest = true;      return true;
		case 'o': options.oldest = true;      return true;
		case 'l': options.list_name = true;   return true;
		case 'f': return true;
		default:  return false;
		}
	}

	static bool applyMatchCluster(const std::string& cluster, MatchOptions& options) {
		for (std::size_t k = 1; k < cluster.size(); ++k) {
			if (!applyMatchFlag(cluster[k], options)) return false;
		}

		return true;
	}

	static bool parseMatchOption(const std::vector<std::string>& args, std::size_t& i,
	                             MatchOptions& options, bool allow_signal) {
		const std::string& arg = args[i];
		if (arg == "-d" && i + 1 < args.size()) {
			options.delimiter = args[++i];
			return true;
		}

		if (allow_signal && arg == "-s" && i + 1 < args.size()) {
			return parseSignalName(args[++i], options.signal);
		}

		if (applyMatchCluster(arg, options)) return true;
		return allow_signal && parseSignalName(arg.substr(1), options.signal);
	}

	static bool parseMatchArgs(const std::vector<std::string>& args, MatchOptions& options,
	                           bool allow_signal, const char* cmd) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg.size() > 1 && arg[0] == '-' && options.pattern.empty()) {
				if (parseMatchOption(args, i, options, allow_signal)) continue;
				perr(cmd, "unknown option: " + arg);
				return false;
			}

			if (!options.pattern.empty()) {
				perr(cmd, "only one pattern can be provided");
				return false;
			}

			options.pattern = arg;
		}

		if (!options.pattern.empty()) return true;
		perr(cmd, "no matching criteria specified");
		return false;
	}

	static bool nameMatches(const std::string& name, const std::regex& pattern, bool exact) {
		const std::string stem = processNameStem(name);
		if (!exact) return searchRegex(stem, pattern) || searchRegex(name, pattern);

		return matchRegex(stem, pattern) || matchRegex(name, pattern);
	}

	static void keepNewestOrOldest(std::vector<ProcessInfo>& matches, bool newest) {
		if (matches.size() < 2) return;

		std::size_t best = 0;
		unsigned long long best_created = newest ? 0ULL : ~0ULL;
		for (std::size_t i = 0; i < matches.size(); ++i) {
			unsigned long long cpu_ms = 0;
			unsigned long long created_ms = 0;
			if (!processTimes(matches[i].pid, cpu_ms, created_ms)) continue;
			const bool better = newest ? created_ms >= best_created : created_ms < best_created;
			if (!better) continue;
			best = i;
			best_created = created_ms;
		}

		matches = { matches[best] };
	}

	static bool findMatchingProcesses(const MatchOptions& options, const char* cmd,
	                                  std::vector<ProcessInfo>& matches) {
		std::regex pattern;
		const auto flags = options.ignore_case
			? std::regex::extended | std::regex::icase
			: std::regex::extended;
		if (!compileRegex(pattern, options.pattern, flags)) {
			perr(cmd, "invalid pattern: " + options.pattern);
			return false;
		}

		const DWORD self = ::GetCurrentProcessId();
		for (const ProcessInfo& process : listProcesses()) {
			if (process.pid == self || process.pid == 0) continue;
			const bool hit = nameMatches(process.name, pattern, options.exact);
			if (hit != options.invert) matches.push_back(process);
		}

		if (options.newest || options.oldest) keepNewestOrOldest(matches, options.newest);
		return true;
	}

	static void printMatches(const std::vector<ProcessInfo>& matches, const MatchOptions& options) {
		for (std::size_t i = 0; i < matches.size(); ++i) {
			if (i != 0) std::fputs(options.delimiter.c_str(), stdout);
			std::printf("%lu", static_cast<unsigned long>(matches[i].pid));
			if (options.list_name) std::printf(" %s", processNameStem(matches[i].name).c_str());
		}

		if (!matches.empty()) std::fputc('\n', stdout);
		std::fflush(stdout);
	}

	static int builtin_pgrep(Executor&, const std::vector<std::string>& args) {
		MatchOptions options;
		if (!parseMatchArgs(args, options, false, "pgrep")) return 2;

		std::vector<ProcessInfo> matches;
		if (!findMatchingProcesses(options, "pgrep", matches)) return 2;

		printMatches(matches, options);
		return matches.empty() ? 1 : 0;
	}

	static int terminateAll(const std::vector<ProcessInfo>& targets, int signum, const char* cmd,
	                        bool quiet) {
		int killed = 0;
		for (const ProcessInfo& process : targets) {
			if (terminateProcessId(process.pid, signum)) {
				++killed;
				continue;
			}

			if (!quiet) {
				std::fprintf(stderr, "wbsh: %s: cannot kill %lu (%s)\n", cmd,
					static_cast<unsigned long>(process.pid), process.name.c_str());
			}
		}

		return killed;
	}

	static int builtin_pkill(Executor&, const std::vector<std::string>& args) {
		MatchOptions options;
		if (!parseMatchArgs(args, options, true, "pkill")) return 2;

		std::vector<ProcessInfo> matches;
		if (!findMatchingProcesses(options, "pkill", matches)) return 2;

		return terminateAll(matches, options.signal, "pkill", false) > 0 ? 0 : 1;
	}

	static bool sameProcessName(const std::string& process_name, const std::string& wanted) {
		const std::string lower_name = lowerAscii(process_name);
		const std::string lower_wanted = lowerAscii(wanted);
		if (lower_name == lower_wanted) return true;
		return lowerAscii(processNameStem(process_name)) == lower_wanted;
	}

	static std::string trailingName(const std::string& path) {
		const std::size_t slash = path.find_last_of("/\\");
		return slash == std::string::npos ? path : path.substr(slash + 1);
	}

	static std::vector<ProcessInfo> processesNamed(const std::string& wanted) {
		std::vector<ProcessInfo> found;
		const DWORD self = ::GetCurrentProcessId();
		const std::string name = trailingName(wanted);
		for (const ProcessInfo& process : listProcesses()) {
			if (process.pid == self) continue;
			if (sameProcessName(process.name, name)) found.push_back(process);
		}

		return found;
	}

	static int builtin_pidof(Executor&, const std::vector<std::string>& args) {
		bool single = false;
		bool any_printed = false;
		for (const std::string& arg : args) {
			if (arg == "-s") {
				single = true;
				continue;
			}

			for (const ProcessInfo& process : processesNamed(arg)) {
				const unsigned long pid = static_cast<unsigned long>(process.pid);
				std::printf("%s%lu", any_printed ? " " : "", pid);
				any_printed = true;
				if (single) break;
			}
		}

		if (any_printed) std::fputc('\n', stdout);
		std::fflush(stdout);
		return any_printed ? 0 : 1;
	}

	static bool parseKillallOption(const std::string& arg, int& signum, bool& quiet) {
		if (arg == "-q") {
			quiet = true;
			return true;
		}

		return parseSignalName(arg.substr(1), signum);
	}

	static int builtin_killall(Executor&, const std::vector<std::string>& args) {
		int signum = kSignalTerm;
		bool quiet = false;
		std::vector<std::string> names;
		for (const std::string& arg : args) {
			if (arg.size() > 1 && arg[0] == '-' && names.empty()) {
				if (parseKillallOption(arg, signum, quiet)) continue;
				perr("killall", "unknown option: " + arg);
				return 2;
			}

			names.push_back(arg);
		}

		int status = 0;
		for (const std::string& name : names) {
			const std::vector<ProcessInfo> targets = processesNamed(name);
			if (terminateAll(targets, signum, "killall", quiet) > 0) continue;
			if (!quiet) perr("killall", name + ": no process killed");
			status = 1;
		}

		return status;
	}

	static bool parseTimeoutOption(const std::vector<std::string>& args, std::size_t& i,
	                               TimeoutOptions& options) {
		const std::string& arg = args[i];
		if (arg == "--preserve-status") {
			options.preserve_status = true;
			return true;
		}

		if (arg == "-v" || arg == "--verbose") {
			options.verbose = true;
			return true;
		}

		if (arg == "--foreground") return true;
		if ((arg == "-s" || arg == "--signal") && i + 1 < args.size()) {
			return parseSignalName(args[++i], options.signal);
		}

		if (arg == "-k" && i + 1 < args.size()) {
			++i;
			return true;
		}

		if (arg.compare(0, 9, "--signal=") == 0) {
			return parseSignalName(arg.substr(9), options.signal);
		}

		return arg.compare(0, 13, "--kill-after=") == 0;
	}

	static bool parseTimeoutArgs(const std::vector<std::string>& args, TimeoutOptions& options) {
		std::size_t i = 0;
		for (; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg == "--") {
				++i;
				break;
			}

			if (arg.size() < 2 || arg[0] != '-') break;
			if (parseTimeoutOption(args, i, options)) continue;
			perr("timeout", "invalid option: " + arg);
			return false;
		}

		if (i + 1 >= args.size()) {
			perr("timeout", "usage: timeout [-s SIG] [--preserve-status] DURATION COMMAND...");
			return false;
		}

		if (!parseDurationSeconds(args[i], options.seconds)) {
			perr("timeout", "invalid time interval '" + args[i] + "'");
			return false;
		}

		options.command.assign(args.begin() + static_cast<std::ptrdiff_t>(i) + 1, args.end());
		return true;
	}

	// The child goes into a kill-on-close job so a builtin run through a
	// child wbsh takes its own children down with it when time runs out.
	static HANDLE createKillOnCloseJob() {
		const HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
		if (job == nullptr) return nullptr;

		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
		limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
			sizeof(limits));
		return job;
	}

	static DWORD timeoutMilliseconds(double seconds) {
		if (seconds <= 0.0) return INFINITE;
		const double ms = seconds * 1000.0;
		if (ms >= static_cast<double>(INFINITE - 1)) return INFINITE - 1;
		return static_cast<DWORD>(ms);
	}

	static int processExitCode(HANDLE process) {
		DWORD exit_code = 0;
		::GetExitCodeProcess(process, &exit_code);
		return shellStatusFromExitCode(exit_code);
	}

	static int expireTimedOutChild(HANDLE job, HANDLE process, const TimeoutOptions& options) {
		if (options.verbose) {
			std::fprintf(stderr, "timeout: sending signal %d to command '%s'\n", options.signal,
				options.command[0].c_str());
		}

		const UINT code = static_cast<UINT>(128 + options.signal);
		if (job == nullptr || ::TerminateJobObject(job, code) == 0) {
			::TerminateProcess(process, code);
		}

		::WaitForSingleObject(process, INFINITE);
		if (options.preserve_status) return processExitCode(process);
		return options.signal == kSignalKill ? 128 + kSignalKill : kStatusTimedOut;
	}

	static int runUnderTimeout(Executor& exec, const TimeoutOptions& options) {
		const HANDLE process = exec.spawnCommand(options.command);
		if (process == INVALID_HANDLE_VALUE) {
			return exec.findExecutable(options.command[0]).empty()
				? kStatusNotFound
				: kStatusCannotInvoke;
		}

		const HANDLE job = createKillOnCloseJob();
		if (job != nullptr) ::AssignProcessToJobObject(job, process);

		const DWORD waited = ::WaitForSingleObject(process, timeoutMilliseconds(options.seconds));
		const int status = waited == WAIT_TIMEOUT
			? expireTimedOutChild(job, process, options)
			: processExitCode(process);

		::CloseHandle(process);
		if (job != nullptr) ::CloseHandle(job);
		return status;
	}

	static int builtin_timeout(Executor& exec, const std::vector<std::string>& args) {
		TimeoutOptions options;
		if (!parseTimeoutArgs(args, options)) return kStatusTimeoutFailed;
		return runUnderTimeout(exec, options);
	}

	static bool parseWatchArgs(const std::vector<std::string>& args, WatchOptions& options) {
		std::size_t i = 0;
		for (; i < args.size() && args[i].size() > 1 && args[i][0] == '-'; ++i) {
			const std::string& arg = args[i];
			if (arg == "--") {
				++i;
				break;
			}

			if (arg == "-t" || arg == "--no-title") options.show_header = false;
			else if (arg == "-n" && i + 1 < args.size()) parseDouble(args[++i], options.interval);
			else if (arg.compare(0, 2, "-n") == 0) parseDouble(arg.substr(2), options.interval);
			else if (arg != "-d" && arg != "-x") break;
		}

		for (; i < args.size(); ++i) {
			if (!options.command.empty()) options.command.push_back(' ');
			options.command += args[i];
		}

		if (options.interval < 0.1) options.interval = 0.1;
		if (!options.command.empty()) return true;

		perr("watch", "usage: watch [-n SECS] [-t] COMMAND...");
		return false;
	}

	static std::string currentClockText() {
		const std::time_t now = std::time(nullptr);
		std::tm local{};
		localtime_s(&local, &now);
		char text[64];
		std::strftime(text, sizeof(text), "%a %b %e %H:%M:%S %Y", &local);
		return text;
	}

	static void printWatchHeader(const WatchOptions& options) {
		int columns = 80;
		int rows = 24;
		queryConsoleSize(columns, rows);

		char left[64];
		std::snprintf(left, sizeof(left), "Every %.1fs: ", options.interval);
		const std::string head = left + options.command;
		const std::string clock = currentClockText();
		const int gap = columns - static_cast<int>(head.size() + clock.size());
		std::printf("%s%*s%s\n\n", head.c_str(), gap > 1 ? gap : 1, "", clock.c_str());
	}

	static bool sleepUnlessInterrupted(double seconds) {
		const auto deadline = std::chrono::steady_clock::now()
			+ std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0));
		while (std::chrono::steady_clock::now() < deadline) {
			if (ctrlCPending()) return false;
			std::this_thread::sleep_for(std::chrono::milliseconds(kWatchPollMs));
		}

		return !ctrlCPending();
	}

	static int builtin_watch(Executor& exec, const std::vector<std::string>& args) {
		WatchOptions options;
		if (!parseWatchArgs(args, options)) return 1;

		ScopedCtrlCCapture capture;
		for (;;) {
			std::fputs("\x1b[H\x1b[2J", stdout);
			if (options.show_header) printWatchHeader(options);
			std::fflush(stdout);

			exec.executeText(options.command, "watch");
			std::fflush(stdout);
			if (exec.flowPending() || !sleepUnlessInterrupted(options.interval)) break;
		}

		takeCtrlC();
		return 0;
	}

	static std::string uptimeText(unsigned long long uptime_ms) {
		const unsigned long long minutes_total = uptime_ms / kMsPerSecond / 60ULL;
		const unsigned long long days = minutes_total / (24ULL * 60ULL);
		const unsigned long long hours = (minutes_total / 60ULL) % 24ULL;
		const unsigned long long minutes = minutes_total % 60ULL;

		char text[64];
		if (days == 0) {
			std::snprintf(text, sizeof(text), "%2llu:%02llu", hours, minutes);
			return text;
		}

		std::snprintf(text, sizeof(text), "%llu day%s, %2llu:%02llu", days,
			days == 1 ? "" : "s", hours, minutes);
		return text;
	}

	static int builtin_uptime(Executor&, const std::vector<std::string>& args) {
		const unsigned long long uptime_ms = ::GetTickCount64();
		if (!args.empty() && (args[0] == "-p" || args[0] == "--pretty")) {
			std::printf("up %s\n", uptimeText(uptime_ms).c_str());
			return 0;
		}

		const std::time_t now = std::time(nullptr);
		std::tm local{};
		localtime_s(&local, &now);
		std::printf(" %02d:%02d:%02d up %s,  load average: 0.00, 0.00, 0.00\n",
			local.tm_hour, local.tm_min, local.tm_sec, uptimeText(uptime_ms).c_str());
		return 0;
	}

	static double freeUnitDivisor(const std::vector<std::string>& args, bool& human) {
		const double kibibyte = kBytesPerKibibyte;
		double divisor = kibibyte;
		for (const std::string& arg : args) {
			if (arg == "-b")      divisor = 1.0;
			else if (arg == "-k") divisor = kibibyte;
			else if (arg == "-m") divisor = kibibyte * kibibyte;
			else if (arg == "-g") divisor = kibibyte * kibibyte * kibibyte;
			else if (arg == "-h") human = true;
		}

		return divisor;
	}

	static std::string humanSize(unsigned long long bytes) {
		static const char kUnits[] = "BKMGTP";
		double value = static_cast<double>(bytes);
		std::size_t unit = 0;
		while (value >= kBytesPerKibibyte && unit + 1 < sizeof(kUnits) - 1) {
			value /= kBytesPerKibibyte;
			++unit;
		}

		char text[32];
		if (unit == 0) std::snprintf(text, sizeof(text), "%lluB", bytes);
		else           std::snprintf(text, sizeof(text), "%.1f%ci", value, kUnits[unit]);
		return text;
	}

	static std::string freeCell(unsigned long long bytes, double divisor, bool human) {
		if (human) return humanSize(bytes);
		char text[32];
		std::snprintf(text, sizeof(text), "%llu",
			static_cast<unsigned long long>(static_cast<double>(bytes) / divisor));
		return text;
	}

	static void printFreeRow(const char* label, unsigned long long total, unsigned long long free,
	                         double divisor, bool human) {
		std::printf("%-6s %12s %12s %12s\n", label,
			freeCell(total, divisor, human).c_str(),
			freeCell(total - free, divisor, human).c_str(),
			freeCell(free, divisor, human).c_str());
	}

	using process_detail::PageFileTotals;

	static BOOL CALLBACK addPageFile(LPVOID context, PENUM_PAGE_FILE_INFORMATION info, LPCWSTR) {
		auto* totals = static_cast<PageFileTotals*>(context);
		totals->total_pages += info->TotalSize;
		totals->used_pages += info->TotalInUse;
		return TRUE;
	}

	static void pageFileUsage(unsigned long long& total, unsigned long long& free) {
		PageFileTotals totals;
		::EnumPageFilesW(addPageFile, &totals);

		SYSTEM_INFO system{};
		::GetSystemInfo(&system);
		total = totals.total_pages * system.dwPageSize;
		free = (totals.total_pages - totals.used_pages) * system.dwPageSize;
	}

	static int builtin_free(Executor&, const std::vector<std::string>& args) {
		MEMORYSTATUSEX status{};
		status.dwLength = sizeof(status);
		if (::GlobalMemoryStatusEx(&status) == 0) {
			perr("free", "cannot query memory status");
			return 1;
		}

		bool human = false;
		const double divisor = freeUnitDivisor(args, human);
		unsigned long long swap_total = 0;
		unsigned long long swap_free = 0;
		pageFileUsage(swap_total, swap_free);

		std::printf("%-6s %12s %12s %12s\n", "", "total", "used", "free");
		printFreeRow("Mem:", status.ullTotalPhys, status.ullAvailPhys, divisor, human);
		printFreeRow("Swap:", swap_total, swap_free, divisor, human);
		std::fflush(stdout);
		return 0;
	}

	void registerProcessBuiltins(Executor& exec) {
		exec.registerBuiltin("ps",      builtin_ps);
		exec.registerBuiltin("pgrep",   builtin_pgrep);
		exec.registerBuiltin("pkill",   builtin_pkill);
		exec.registerBuiltin("pidof",   builtin_pidof);
		exec.registerBuiltin("killall", builtin_killall);
		exec.registerBuiltin("timeout", builtin_timeout);
		exec.registerBuiltin("watch",   builtin_watch);
		exec.registerBuiltin("uptime",  builtin_uptime);
		exec.registerBuiltin("free",    builtin_free);
	}

#else

	void registerProcessBuiltins(Executor&) {}

#endif /* _WIN32 */

}  // namespace wbsh

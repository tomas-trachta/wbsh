/**
 * @file coreutils_session.cpp
 * @brief nohup, setsid, nice, renice, and su / sudo pointers to Windows' own elevation tools.
 *
 * Windows has no sessions, hang-ups or nice values. A command that has to
 * outlive the console (nohup, setsid) starts on a hidden console of its
 * own and in a process group of its own, so neither Ctrl+C, Ctrl+Break
 * nor closing the window reaches it. Another console cannot use this
 * one's handles, though, so setsid leaves a command whose standard streams
 * are still the console on it, where only Ctrl+C passes it by; nohup has
 * already moved all of them off the console. Niceness is the priority class, by
 * the one mapping nice, renice, ps and top share (Cygwin's, but never
 * REALTIME):
 *
 *     -20..-13 HIGH   -12..-5 ABOVE_NORMAL   -4..3 NORMAL
 *     4..11 BELOW_NORMAL   12..19 IDLE
 *
 * A class reads back as -16, -8, 0, 8 or 16 (REALTIME, set by someone
 * else, as -20). Windows hands a below-normal class down to a process's
 * children, but starts the children of a raised one at NORMAL again.
 */

#include "coreutils_internal.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <fcntl.h>
#  include <io.h>
#endif /* _WIN32 */

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "executor.h"
#include "exitstatus.h"
#include "interrupt.h"
#include "numparse.h"
#include "optscan.h"
#include "pathconv.h"

namespace wbsh {

#ifdef _WIN32

	namespace session_detail {

		struct NicenessBand {
			int lowest;
			DWORD priority_class;
			int reported;
		};

		static const NicenessBand kNicenessBands[] = {
			{ -20, HIGH_PRIORITY_CLASS,         -16 },
			{ -12, ABOVE_NORMAL_PRIORITY_CLASS, -8  },
			{ -4,  NORMAL_PRIORITY_CLASS,       0   },
			{ 4,   BELOW_NORMAL_PRIORITY_CLASS, 8   },
			{ 12,  IDLE_PRIORITY_CLASS,         16  },
		};

		struct BasePriorityBand {
			long highest_base;
			int niceness;
		};

		static const BasePriorityBand kBasePriorityBands[] = {
			{ 0, 0 }, { 4, 16 }, { 6, 8 }, { 8, 0 }, { 10, -8 }, { 15, -16 },
		};

		struct StdioSwap {
			bool swapped[3] = { false, false, false };
			int saved[3] = { -1, -1, -1 };
		};

		struct NohupPlan {
			bool ignoring_input = false;
			bool redirecting_stdout = false;
			bool redirecting_stderr = false;
			bool stdout_closed = false;
			HANDLE input = INVALID_HANDLE_VALUE;
			HANDLE output = INVALID_HANDLE_VALUE;
		};

		enum SetsidOptionId {
			kSetsidCtty = 1, kSetsidFork, kSetsidWait, kSetsidHelp, kSetsidVersion,
		};

		struct SetsidOptions {
			bool keep_console = false;
			bool fork = false;
			bool wait = false;
			bool help = false;
			bool version = false;
			std::vector<std::string> command;
		};

		enum NiceOptionId { kNiceAdjustment = 1 };

		struct NiceOptions {
			bool adjustment_given = false;
			int adjustment = 10;
			std::vector<std::string> command;
		};

		enum class ReniceWhich { Process, Group, User };

		struct ReniceRequest {
			int priority = 0;
			bool relative = false;
		};

		struct ReniceTarget {
			std::string who;
			const char* id_type = "process ID";
			std::vector<unsigned long> pids;
		};

	}  // namespace session_detail

	using session_detail::NiceOptions;
	using session_detail::NohupPlan;
	using session_detail::ReniceRequest;
	using session_detail::ReniceTarget;
	using session_detail::ReniceWhich;
	using session_detail::SetsidOptions;
	using session_detail::StdioSwap;

	static const int   kStatusCanceled     = 125;
	static const int   kStatusCannotInvoke = 126;
	static const int   kStatusNotFound     = 127;
	static const int   kSignalInterrupt    = 2;
	static const int   kNicenessMin        = -20;
	static const int   kNicenessMax        = 19;
	static const int   kAdjustmentLimit    = 39;
	static const DWORD kWaitPollMs         = 50;

	unsigned long priorityClassForNiceness(int niceness) {
		const int clamped = std::clamp(niceness, kNicenessMin, kNicenessMax);
		DWORD priority_class = HIGH_PRIORITY_CLASS;
		for (const auto& band : session_detail::kNicenessBands) {
			if (clamped >= band.lowest) priority_class = band.priority_class;
		}

		return priority_class;
	}

	int nicenessOfPriorityClass(unsigned long priority_class) {
		if (priority_class == REALTIME_PRIORITY_CLASS) return kNicenessMin;

		for (const auto& band : session_detail::kNicenessBands) {
			if (band.priority_class == priority_class) return band.reported;
		}

		return 0;
	}

	int nicenessOfBasePriority(long base_priority) {
		for (const auto& band : session_detail::kBasePriorityBands) {
			if (base_priority <= band.highest_base) return band.niceness;
		}

		return kNicenessMin;
	}

	static std::error_code lastErrorCode() {
		return std::error_code(static_cast<int>(::GetLastError()), std::system_category());
	}

	// OpenProcess says "invalid parameter" for a PID nothing runs under.
	static std::error_code openErrorCode() {
		if (::GetLastError() == ERROR_INVALID_PARAMETER) {
			return std::make_error_code(std::errc::no_such_process);
		}

		return lastErrorCode();
	}

	bool queryProcessNiceness(unsigned long pid, int& niceness, std::error_code& ec) {
		const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (process == nullptr) {
			ec = openErrorCode();
			return false;
		}

		const DWORD priority_class = ::GetPriorityClass(process);
		if (priority_class == 0) ec = lastErrorCode();
		::CloseHandle(process);
		if (priority_class == 0) return false;

		niceness = nicenessOfPriorityClass(priority_class);
		return true;
	}

	bool setProcessNiceness(unsigned long pid, int niceness, std::error_code& ec) {
		const HANDLE process = ::OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
		if (process == nullptr) {
			ec = openErrorCode();
			return false;
		}

		const bool set = ::SetPriorityClass(process, priorityClassForNiceness(niceness)) != 0;
		if (!set) ec = lastErrorCode();
		::CloseHandle(process);
		return set;
	}

	static int currentNiceness() {
		return nicenessOfPriorityClass(::GetPriorityClass(::GetCurrentProcess()));
	}

	static bool commandExists(Executor& exec, const std::string& name) {
		if (exec.isFunction(name) || exec.isBuiltin(name) || exec.isAlias(name)) return true;
		return !exec.findExecutable(name).empty();
	}

	static int exitStatusOf(HANDLE process) {
		DWORD code = 0;
		::GetExitCodeProcess(process, &code);
		return shellStatusFromExitCode(code);
	}

	static int waitForCommand(HANDLE process) {
		::WaitForSingleObject(process, INFINITE);
		const int status = exitStatusOf(process);
		::CloseHandle(process);
		return status;
	}

	// Ctrl+C never reaches a command in a process group of its own, so the
	// key only gives the shell back; the command carries on, as a session
	// setsid -w started outlives setsid itself.
	static int waitUnlessInterrupted(HANDLE process) {
		const ScopedCtrlCCapture capture;
		while (::WaitForSingleObject(process, kWaitPollMs) == WAIT_TIMEOUT) {
			if (!takeCtrlC()) continue;
			::CloseHandle(process);
			return kSignalStatusBase + kSignalInterrupt;
		}

		return waitForCommand(process);
	}

	static void swapInHandle(StdioSwap& swap, int fd, HANDLE handle) {
		const HANDLE self = ::GetCurrentProcess();
		HANDLE copy = nullptr;
		if (::DuplicateHandle(self, handle, self, &copy, 0, FALSE, DUPLICATE_SAME_ACCESS) == 0) {
			return;
		}

		const int replacement = _open_osfhandle(reinterpret_cast<intptr_t>(copy), _O_BINARY);
		if (replacement < 0) {
			::CloseHandle(copy);
			return;
		}

		swap.saved[fd] = _dup(fd);
		swap.swapped[fd] = true;
		_dup2(replacement, fd);
		_close(replacement);
	}

	static void restoreStdio(StdioSwap& swap) {
		for (int fd = 0; fd < 3; ++fd) {
			if (!swap.swapped[fd]) continue;
			if (swap.saved[fd] < 0) {
				_close(fd);
				continue;
			}

			_dup2(swap.saved[fd], fd);
			_close(swap.saved[fd]);
		}
	}

	static HANDLE openUnreadableInput() {
		return ::CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	}

	// FILE_APPEND_DATA without FILE_WRITE_DATA makes every write of the
	// command land at the end, however many writers share the file.
	static HANDLE openForAppending(Executor& exec, const std::string& path) {
		const std::wstring native = toNative(exec, path).wstring();
		return ::CreateFileW(native.c_str(), FILE_APPEND_DATA | SYNCHRONIZE,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, nullptr);
	}

	static std::string singleQuoted(const std::string& text) {
		return "'" + text + "'";
	}

	static void reportOpenFailure(const std::string& path, const std::error_code& ec) {
		perr("nohup", "failed to open " + singleQuoted(path) + ": " + posixErrorText(ec));
	}

	static bool openNohupOutput(Executor& exec, NohupPlan& plan, std::string& name) {
		name = "nohup.out";
		plan.output = openForAppending(exec, name);
		if (plan.output != INVALID_HANDLE_VALUE) return true;

		const std::error_code local_error = lastErrorCode();
		const std::string home = exec.env().get("HOME");
		if (!home.empty()) {
			name = home + (home.back() == '/' || home.back() == '\\' ? "" : "/") + "nohup.out";
			plan.output = openForAppending(exec, name);
			if (plan.output != INVALID_HANDLE_VALUE) return true;
		}

		const std::error_code home_error = lastErrorCode();
		reportOpenFailure("nohup.out", local_error);
		if (!home.empty()) reportOpenFailure(name, home_error);
		return false;
	}

	static bool stdoutIsClosed() {
		const intptr_t raw = _get_osfhandle(_fileno(stdout));
		return raw == -1 || raw == -2;
	}

	static void surveyStdio(NohupPlan& plan) {
		plan.ignoring_input = isConsoleStream(stdin);
		plan.redirecting_stdout = isConsoleStream(stdout);
		plan.redirecting_stderr = isConsoleStream(stderr);
		plan.stdout_closed = !plan.redirecting_stdout && stdoutIsClosed();
	}

	static bool planInput(NohupPlan& plan) {
		if (!plan.ignoring_input) return true;

		plan.input = openUnreadableInput();
		if (plan.input == INVALID_HANDLE_VALUE) {
			perr("nohup", "failed to render standard input unusable");
			return false;
		}

		if (!plan.redirecting_stdout && !plan.redirecting_stderr) perr("nohup", "ignoring input");
		return true;
	}

	static bool planOutput(Executor& exec, NohupPlan& plan) {
		if (!plan.redirecting_stdout && !(plan.redirecting_stderr && plan.stdout_closed)) {
			return true;
		}

		std::string name;
		if (!openNohupOutput(exec, plan, name)) return false;

		const char* lead = plan.ignoring_input ? "ignoring input and appending output to "
			: "appending output to ";
		perr("nohup", lead + singleQuoted(name));
		return true;
	}

	static void announceStderr(const NohupPlan& plan) {
		if (!plan.redirecting_stderr || plan.redirecting_stdout) return;

		perr("nohup", plan.ignoring_input ? "ignoring input and redirecting stderr to stdout"
			: "redirecting stderr to stdout");
	}

	static void closePlan(NohupPlan& plan) {
		if (plan.input != INVALID_HANDLE_VALUE) ::CloseHandle(plan.input);
		if (plan.output != INVALID_HANDLE_VALUE) ::CloseHandle(plan.output);
	}

	static HANDLE stderrTarget(const NohupPlan& plan) {
		if (plan.output != INVALID_HANDLE_VALUE) return plan.output;
		return reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout)));
	}

	static HANDLE spawnWithPlan(Executor& exec, const std::vector<std::string>& command,
	                            const NohupPlan& plan) {
		std::fflush(stdout);
		std::fflush(stderr);

		StdioSwap swap;
		if (plan.input != INVALID_HANDLE_VALUE) swapInHandle(swap, 0, plan.input);
		if (plan.redirecting_stdout) swapInHandle(swap, 1, plan.output);
		if (plan.redirecting_stderr) swapInHandle(swap, 2, stderrTarget(plan));

		const HANDLE process = exec.spawnCommand(command, detachedPlacement(false));
		restoreStdio(swap);
		return process;
	}

	static std::string nohupOptionError(const std::string& arg) {
		if (arg.compare(0, 2, "--") == 0) return "unrecognized option '" + arg + "'";
		return std::string("invalid option -- '") + arg[1] + "'";
	}

	static bool parseNohupArgs(const std::vector<std::string>& args,
	                           std::vector<std::string>& command) {
		const bool dashdash = !args.empty() && args[0] == "--";
		if (!dashdash && !args.empty() && args[0].size() > 1 && args[0][0] == '-') {
			perr("nohup", nohupOptionError(args[0]));
			return false;
		}

		command.assign(args.begin() + (dashdash ? 1 : 0), args.end());
		if (!command.empty()) return true;

		perr("nohup", "missing operand");
		return false;
	}

	static int reportMissingCommand(const char* cmd, const std::string& what) {
		perr(cmd, what + ": No such file or directory");
		return kStatusNotFound;
	}

	static const char* const kNohupUsage =
		"Usage: nohup COMMAND [ARG]...\n"
		"Run COMMAND so that hanging up (closing the console) does not end it.\n"
		"\n"
		"If standard input is a terminal, it is replaced by an unreadable file.\n"
		"If standard output is a terminal, output is appended to 'nohup.out',\n"
		"or to '$HOME/nohup.out' when that cannot be written.\n"
		"If standard error is a terminal, it goes where standard output goes.\n";

	static const char* const kSetsidUsage =
		"Usage: setsid [options] <program> [arguments ...]\n"
		"Run a program in a new session: a process group of its own, on a hidden\n"
		"console unless its standard streams are still this one.\n"
		"\n"
		"Options:\n"
		" -c, --ctty     stay on the current console\n"
		" -f, --fork     always return at once\n"
		" -w, --wait     wait for the program and return its exit status\n"
		" -h, --help     display this help\n"
		" -V, --version  display version\n";

	static int showUsage(const char* usage) {
		std::fputs(usage, stdout);
		std::fflush(stdout);
		return 0;
	}

	static int showVersion(const char* cmd) {
		std::printf("%s (wbsh) %d.%d.%d\n", cmd, WBSH_VERSION_MAJOR, WBSH_VERSION_MINOR,
			WBSH_VERSION_PATCH);
		std::fflush(stdout);
		return 0;
	}

	static int builtin_nohup(Executor& exec, const std::vector<std::string>& args) {
		if (!args.empty() && args[0] == "--help") return showUsage(kNohupUsage);
		if (!args.empty() && args[0] == "--version") return showVersion("nohup");

		const int failure = exec.env().get("POSIXLY_CORRECT").empty() ? kStatusCanceled
			: kStatusNotFound;
		std::vector<std::string> command;
		if (!parseNohupArgs(args, command)) return failure;
		if (!commandExists(exec, command[0])) {
			const std::string what = "failed to run command " + singleQuoted(command[0]);
			return reportMissingCommand("nohup", what);
		}

		NohupPlan plan;
		surveyStdio(plan);
		const bool planned = planInput(plan) && planOutput(exec, plan);
		if (planned) announceStderr(plan);

		const HANDLE process = planned ? spawnWithPlan(exec, command, plan) : INVALID_HANDLE_VALUE;
		closePlan(plan);
		if (!planned) return failure;
		if (process == INVALID_HANDLE_VALUE) return kStatusCannotInvoke;

		return waitUnlessInterrupted(process);
	}

	static const std::vector<OptDef>& setsidOptionDefs() {
		static const std::vector<OptDef> defs = {
			{ session_detail::kSetsidCtty,    'c', "ctty",    OptArg::None },
			{ session_detail::kSetsidFork,    'f', "fork",    OptArg::None },
			{ session_detail::kSetsidWait,    'w', "wait",    OptArg::None },
			{ session_detail::kSetsidHelp,    'h', "help",    OptArg::None },
			{ session_detail::kSetsidVersion, 'V', "version", OptArg::None },
		};
		return defs;
	}

	static bool parseSetsidArgs(const std::vector<std::string>& args, SetsidOptions& options) {
		OptScanResult scanned;
		if (!scanOptions("setsid", args, setsidOptionDefs(), scanned, true)) return false;

		for (const OptHit& hit : scanned.hits) {
			if (hit.id == session_detail::kSetsidCtty) options.keep_console = true;
			if (hit.id == session_detail::kSetsidFork) options.fork = true;
			if (hit.id == session_detail::kSetsidWait) options.wait = true;
			if (hit.id == session_detail::kSetsidHelp) options.help = true;
			if (hit.id == session_detail::kSetsidVersion) options.version = true;
		}

		options.command = scanned.operands;
		if (!options.command.empty() || options.help || options.version) return true;

		perr("setsid", "no command specified");
		return false;
	}

	// util-linux setsid has to fork when it already leads a process group,
	// as every command of an interactive (job-control) shell does; the
	// parent then returns at once. A script's command execs and is waited for.
	static bool returnsAtOnce(Executor& exec, const SetsidOptions& options) {
		if (options.wait) return false;
		return options.fork || exec.env().interactive();
	}

	static bool keepsConsole(const SetsidOptions& options) {
		if (options.keep_console) return true;
		return isConsoleStream(stdin) || isConsoleStream(stdout) || isConsoleStream(stderr);
	}

	static int builtin_setsid(Executor& exec, const std::vector<std::string>& args) {
		SetsidOptions options;
		if (!parseSetsidArgs(args, options)) return 1;
		if (options.help) return showUsage(kSetsidUsage);
		if (options.version) return showVersion("setsid");
		if (!commandExists(exec, options.command[0])) {
			return reportMissingCommand("setsid", "failed to execute " + options.command[0]);
		}

		const HANDLE process = exec.spawnCommand(options.command,
			detachedPlacement(keepsConsole(options)));
		if (process == INVALID_HANDLE_VALUE) return kStatusCannotInvoke;

		if (!returnsAtOnce(exec, options)) return waitUnlessInterrupted(process);

		::CloseHandle(process);
		return 0;
	}

	static bool isLegacyAdjustment(const std::string& arg) {
		if (arg.size() < 2 || arg[0] != '-') return false;

		const std::size_t digit = arg[1] == '-' || arg[1] == '+' ? 2 : 1;
		return digit < arg.size() && std::isdigit(static_cast<unsigned char>(arg[digit])) != 0;
	}

	// `nice -5 cmd` and `nice --5 cmd` are `-n 5` and `-n -5`, wherever
	// they come before the command.
	static std::vector<std::string> expandLegacyAdjustments(const std::vector<std::string>& args) {
		std::vector<std::string> expanded;
		std::size_t i = 0;
		for (; i < args.size() && args[i] != "--" && args[i].size() > 1 && args[i][0] == '-'; ++i) {
			if (isLegacyAdjustment(args[i])) {
				expanded.push_back("-n");
				expanded.push_back(args[i].substr(1));
				continue;
			}

			expanded.push_back(args[i]);
			const bool takes_next = args[i] == "-n" || args[i] == "--adjustment";
			if (takes_next && i + 1 < args.size()) expanded.push_back(args[++i]);
		}

		expanded.insert(expanded.end(), args.begin() + static_cast<std::ptrdiff_t>(i), args.end());
		return expanded;
	}

	static bool parseAdjustment(const std::string& text, int& adjustment) {
		long long value = 0;
		std::size_t used = 0;
		if (!parseLL(text, value, 10, &used) || used != text.size()) {
			perr("nice", "invalid adjustment " + singleQuoted(text));
			return false;
		}

		const long long limit = kAdjustmentLimit;
		adjustment = static_cast<int>(std::clamp(value, -limit, limit));
		return true;
	}

	static bool parseNiceArgs(const std::vector<std::string>& args, NiceOptions& options) {
		static const std::vector<OptDef> defs = {
			{ session_detail::kNiceAdjustment, 'n', "adjustment", OptArg::Required },
		};

		OptScanResult scanned;
		if (!scanOptions("nice", expandLegacyAdjustments(args), defs, scanned, true)) return false;

		for (const OptHit& hit : scanned.hits) {
			if (!parseAdjustment(hit.value, options.adjustment)) return false;
			options.adjustment_given = true;
		}

		options.command = scanned.operands;
		if (!options.command.empty() || !options.adjustment_given) return true;

		perr("nice", "a command must be given with an adjustment");
		return false;
	}

	static void warnIfPriorityRefused(HANDLE process, DWORD wanted) {
		if (::GetPriorityClass(process) == wanted) return;
		perr("nice", "cannot set niceness: Permission denied");
	}

	static int runNiced(Executor& exec, const NiceOptions& options) {
		if (!commandExists(exec, options.command[0])) {
			return reportMissingCommand("nice", singleQuoted(options.command[0]));
		}

		const DWORD wanted = priorityClassForNiceness(currentNiceness() + options.adjustment);
		SpawnPlacement placement;
		placement.creation_flags = wanted;
		const HANDLE process = exec.spawnCommand(options.command, placement);
		if (process == INVALID_HANDLE_VALUE) return kStatusCannotInvoke;

		warnIfPriorityRefused(process, wanted);
		return waitForCommand(process);
	}

	static int builtin_nice(Executor& exec, const std::vector<std::string>& args) {
		NiceOptions options;
		if (!parseNiceArgs(args, options)) return kStatusCanceled;
		if (!options.command.empty()) return runNiced(exec, options);

		std::printf("%d\n", currentNiceness());
		std::fflush(stdout);
		return 0;
	}

	static std::string lowerAscii(std::string text) {
		for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return text;
	}

	static std::string accountName(const std::string& user) {
		const std::size_t slash = user.find_last_of('\\');
		return lowerAscii(slash == std::string::npos ? user : user.substr(slash + 1));
	}

	static bool accountExists(const std::string& user) {
		const std::wstring name = utf8ToWide(user);
		DWORD sid_size = 0;
		DWORD domain_size = 0;
		SID_NAME_USE use;
		::LookupAccountNameW(nullptr, name.c_str(), nullptr, &sid_size, nullptr, &domain_size,
			&use);
		return ::GetLastError() == ERROR_INSUFFICIENT_BUFFER;
	}

	static std::vector<unsigned long> processesOfUser(const std::string& user) {
		const std::string wanted = accountName(user);
		std::vector<unsigned long> pids;
		for (const auto& entry : collectProcessUsage()) {
			if (accountName(entry.second.user) == wanted) pids.push_back(entry.first);
		}

		return pids;
	}

	static bool processStartTime(unsigned long pid, std::uint64_t& started) {
		const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (process == nullptr) return false;

		FILETIME created{};
		FILETIME exited{};
		FILETIME kernel{};
		FILETIME user{};
		const bool known = ::GetProcessTimes(process, &created, &exited, &kernel, &user) != 0;
		::CloseHandle(process);
		started = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32)
			| created.dwLowDateTime;
		return known;
	}

	// A parent PID outlives its process, so a child only counts when it
	// started after the process now holding that PID.
	static bool isChildOf(const ProcessInfo& process, unsigned long parent) {
		std::uint64_t parent_started = 0;
		std::uint64_t child_started = 0;
		if (process.parent_pid != parent) return false;
		if (!processStartTime(parent, parent_started)) return false;
		return processStartTime(process.pid, child_started) && child_started >= parent_started;
	}

	static std::vector<unsigned long> processTree(unsigned long root) {
		const std::vector<ProcessInfo> processes = listProcesses();
		std::vector<unsigned long> tree;
		if (probeProcess(root) != ProcessProbe::Missing) tree.push_back(root);

		std::set<unsigned long> seen(tree.begin(), tree.end());
		for (std::size_t next = 0; next < tree.size(); ++next) {
			for (const ProcessInfo& process : processes) {
				if (seen.count(process.pid) != 0 || !isChildOf(process, tree[next])) continue;
				seen.insert(process.pid);
				tree.push_back(process.pid);
			}
		}

		return tree;
	}

	static bool lowestNiceness(const ReniceTarget& target, int& niceness, std::error_code& ec) {
		bool found = false;
		ec = std::make_error_code(std::errc::no_such_process);
		for (const unsigned long pid : target.pids) {
			int one = 0;
			std::error_code one_error;
			if (!queryProcessNiceness(pid, one, one_error)) {
				ec = one_error;
				continue;
			}

			niceness = found ? std::min(niceness, one) : one;
			found = true;
		}

		return found;
	}

	static bool setTargetNiceness(const ReniceTarget& target, int niceness, std::error_code& ec) {
		bool any = false;
		for (const unsigned long pid : target.pids) {
			std::error_code one_error;
			if (setProcessNiceness(pid, niceness, one_error)) any = true;
			else ec = one_error;
		}

		return any;
	}

	static bool reniceFailure(const char* what, const ReniceTarget& target,
	                          const std::error_code& ec) {
		perr("renice", std::string("failed to ") + what + " priority for " + target.who + " ("
			+ target.id_type + "): " + posixErrorText(ec));
		return false;
	}

	static bool reniceTarget(const ReniceTarget& target, const ReniceRequest& request) {
		int old_niceness = 0;
		std::error_code ec;
		if (!lowestNiceness(target, old_niceness, ec)) return reniceFailure("get", target, ec);

		const int wanted = request.relative ? old_niceness + request.priority : request.priority;
		if (!setTargetNiceness(target, wanted, ec)) return reniceFailure("set", target, ec);

		int new_niceness = 0;
		if (!lowestNiceness(target, new_niceness, ec)) return reniceFailure("get", target, ec);

		std::printf("%s (%s) old priority %d, new priority %d\n", target.who.c_str(),
			target.id_type, old_niceness, new_niceness);
		std::fflush(stdout);
		return true;
	}

	static const char* reniceIdType(ReniceWhich which) {
		switch (which) {
			case ReniceWhich::Group: return "process group ID";
			case ReniceWhich::User:  return "user ID";
			default:                 return "process ID";
		}
	}

	static std::string currentProcessUser() {
		const std::map<unsigned long, ProcessUsage> usage = collectProcessUsage();
		const auto self = usage.find(::GetCurrentProcessId());
		return self == usage.end() ? std::string() : self->second.user;
	}

	// As in setpriority(), 0 names the caller: here the shell, its process
	// tree, or the user running it.
	static bool resolveUserTarget(const std::string& word, ReniceTarget& target) {
		const std::string user = word == "0" ? currentProcessUser() : word;
		if (user.empty() || !accountExists(user)) {
			perr("renice", "unknown user " + word);
			return false;
		}

		target.pids = processesOfUser(user);
		return true;
	}

	static bool resolveReniceTarget(ReniceWhich which, const std::string& word,
	                                ReniceTarget& target) {
		target.who = word;
		target.id_type = reniceIdType(which);
		if (which == ReniceWhich::User) return resolveUserTarget(word, target);

		unsigned long pid = 0;
		std::size_t used = 0;
		const bool numeric = !word.empty() && word[0] != '-' && parseUL(word, pid, 10, &used);
		if (!numeric || used != word.size()) {
			perr("renice", std::string("bad ") + target.id_type + " value: " + word);
			return false;
		}

		if (pid == 0) pid = ::GetCurrentProcessId();
		target.pids = which == ReniceWhich::Group ? processTree(pid)
			: std::vector<unsigned long>{ pid };
		return true;
	}

	static bool switchesTarget(const std::string& word, ReniceWhich& which) {
		if (word == "-p" || word == "--pid")  which = ReniceWhich::Process;
		else if (word == "-g" || word == "--pgrp") which = ReniceWhich::Group;
		else if (word == "-u" || word == "--user") which = ReniceWhich::User;
		else return false;
		return true;
	}

	static bool parseRenicePriority(Executor& exec, const std::vector<std::string>& args,
	                                ReniceRequest& request, std::size_t& next) {
		std::size_t first = 0;
		if (!args.empty() && (args[0] == "-n" || args[0] == "--priority")) {
			request.relative = args[0] == "-n" && !exec.env().get("POSIXLY_CORRECT").empty();
			first = 1;
		} else if (!args.empty() && args[0] == "--relative") {
			request.relative = true;
			first = 1;
		}

		if (args.size() < first + 2 || args[first].empty()) {
			perr("renice", "not enough arguments");
			return false;
		}

		std::size_t used = 0;
		if (!parseInt(args[first], request.priority, 10, &used) || used != args[first].size()) {
			perr("renice", "invalid priority " + singleQuoted(args[first]));
			return false;
		}

		next = first + 1;
		return true;
	}

	static int builtin_renice(Executor& exec, const std::vector<std::string>& args) {
		ReniceRequest request;
		std::size_t first = 0;
		if (!parseRenicePriority(exec, args, request, first)) return 1;

		ReniceWhich which = ReniceWhich::Process;
		bool failed = false;
		for (std::size_t i = first; i < args.size(); ++i) {
			if (switchesTarget(args[i], which)) continue;

			ReniceTarget target;
			const bool done = resolveReniceTarget(which, args[i], target)
				&& reniceTarget(target, request);
			failed = failed || !done;
		}

		return failed ? 1 : 0;
	}

	static std::string suTargetUser(const std::vector<std::string>& args) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			const bool takes_value = arg == "-c" || arg == "-s" || arg == "-g" || arg == "-G"
				|| arg == "--command" || arg == "--shell" || arg == "--group";
			if (takes_value) ++i;
			else if (arg.empty() || arg[0] != '-') return arg;
		}

		return "Administrator";
	}

	static int builtin_su(Executor&, const std::vector<std::string>& args) {
		perr("su", "Windows has no su; to run a shell as another user: runas /user:"
			+ suTargetUser(args) + " wbsh");
		perr("su", "for an elevated shell, use sudo (Windows 11 24H2 or later) or gsudo");
		return 1;
	}

	// Windows' sudo and gsudo run programs, not wbsh builtins, so a builtin
	// with no program of the same name goes through a wbsh of its own.
	static std::vector<std::string> sudoArgv(Executor& exec, const std::vector<std::string>& args) {
		std::vector<std::string> argv = { "sudo" };
		const bool wraps = !args.empty() && !args[0].empty() && args[0][0] != '-'
			&& exec.isBuiltin(args[0]) && exec.findExecutable(args[0]).empty();
		if (!wraps) {
			argv.insert(argv.end(), args.begin(), args.end());
			return argv;
		}

		argv.push_back(selfExecutablePath());
		argv.push_back("-r");
		argv.push_back("-c");
		argv.push_back(quoteShellArgv(args));
		return argv;
	}

	static int explainMissingSudo() {
		perr("sudo", "no sudo on PATH, and wbsh cannot switch users by itself");
		perr("sudo", "on Windows 11 24H2 or later, turn on Settings > System > For developers"
			" > Enable sudo");
		perr("sudo", "otherwise start an elevated wbsh (runas /user:Administrator wbsh)"
			" or install gsudo (winget install gerardog.gsudo)");
		return 1;
	}

	static int builtin_sudo(Executor& exec, const std::vector<std::string>& args) {
		if (exec.findExecutable("sudo").empty()) return explainMissingSudo();
		return exec.runExternal(sudoArgv(exec, args), {});
	}

	void registerSessionBuiltins(Executor& exec) {
		exec.registerBuiltin("nohup",  builtin_nohup);
		exec.registerBuiltin("setsid", builtin_setsid);
		exec.registerBuiltin("nice",   builtin_nice);
		exec.registerBuiltin("renice", builtin_renice);
		exec.registerBuiltin("su",     builtin_su);
		exec.registerBuiltin("sudo",   builtin_sudo);
	}

#else

	void registerSessionBuiltins(Executor&) {}

#endif /* _WIN32 */

}  // namespace wbsh

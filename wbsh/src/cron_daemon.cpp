/**
 * @file cron_daemon.cpp
 * @brief crond: busybox's options, one crond per crontab directory, and the minute loop.
 *
 * crond looks at the clock every second and acts once per new minute:
 * it rereads the crontabs that changed and starts the jobs due in that
 * minute only, so after the machine slept, or the clock was set forward,
 * the minutes in between are skipped as cron skips them. After the clock
 * is set back by up to three hours nothing runs until it has caught up.
 * @reboot jobs run when crond first starts after Windows booted (Windows'
 * boot count, recorded in .crond.reboot). Without -f crond starts a
 * hidden copy of itself, detached from the console, and returns.
 */

#include "cron_internal.h"

#include <string>
#include <vector>

#include "coreutils_internal.h"
#include "crond_internal.h"

#ifdef _WIN32

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <system_error>
#include <utility>

#include "executor.h"
#include "exitstatus.h"
#include "interrupt.h"
#include "numparse.h"
#include "optscan.h"
#include "pathconv.h"

#endif /* _WIN32 */

namespace wbsh {

#ifdef _WIN32

	namespace fs = std::filesystem;

	namespace cron_detail {

		enum CrondOptionId {
			kCrondForeground = 1,
			kCrondBackground,
			kCrondSyslog,
			kCrondLevel,
			kCrondDebug,
			kCrondLogFile,
			kCrondDirectory,
			kCrondOnce,
			kCrondLockHandle,
			kCrondHelp,
		};

		struct CrondOptions {
			bool foreground = false;
			bool to_stderr = false;
			bool help = false;
			int level = kCrondLogNotice;
			std::string log_file;
			std::string directory;
			std::string once;
			unsigned long lock_handle = 0;
		};

		struct CrondSetup {
			fs::path directory;
			CrondLog log;
			std::string user;
			bool once = false;
			CivilTime once_at;
			unsigned long lock_handle = 0;
		};

		struct CrontabFile {
			fs::path path;
			fs::file_time_type modified;
			std::uintmax_t size = 0;
		};

		struct LoadedCrontab {
			fs::file_time_type modified;
			std::uintmax_t size = 0;
			CronTable table;
		};

		struct CrondState {
			Executor& exec;
			const CrondSetup& setup;
			std::unique_ptr<CronZone> zone;
			std::map<std::string, LoadedCrontab> crontabs;
			std::vector<RunningJob> jobs;
		};

		struct CrondLock {
			HANDLE file = INVALID_HANDLE_VALUE;
			fs::path path;
		};

		static const char* const kPidFileName = ".crond.pid";
		static const char* const kRebootStampName = ".crond.reboot";
		static const char* const kLogFileName = ".crond.log";
		static const wchar_t* const kPrefetchKey = L"SYSTEM\\CurrentControlSet\\Control\\"
			L"Session Manager\\Memory Management\\PrefetchParameters";

		static const std::int64_t kSecondsPerMinute = 60;
		static const std::int64_t kBackwardPatience = 3 * 3600;
		static const std::int64_t kBootTimeGranularity = 600;
		static const long long kMillisecondsPerMinute = 60000;
		static const long long kSliceMs = 1000;
		static const long long kPastTheMinuteMs = 20;
		static const int kSignalInterrupt = 2;
		static const DWORD kTeardownWaitMs = 2000;

	}  // namespace cron_detail

	using cron_detail::CrondLock;
	using cron_detail::CrondOptions;
	using cron_detail::CrondSetup;
	using cron_detail::CrondState;
	using cron_detail::CrontabFile;
	using cron_detail::LoadedCrontab;

	static void printCrondUsage(std::FILE* stream) {
		std::fputs(
			"usage: crond [-fbS] [-l N] [-d N] [-L LOGFILE] [-c DIR]\n"
			"\n"
			"  -f         stay in the foreground\n"
			"  -b         run in the background, hidden (the default)\n"
			"  -S         log to DIR/.crond.log (the default)\n"
			"  -l N       log level: 0 is the most verbose, 8 the default\n"
			"  -d N       log level N, logging to standard error (implies -f)\n"
			"  -L FILE    log to FILE\n"
			"  -c DIR     crontab directory (default %APPDATA%\\wbsh\\crontabs)\n"
			"\n"
			"Job output goes to the log. A background crond records its PID in\n"
			"DIR/.crond.pid; stop it with: kill $(cat DIR/.crond.pid)\n", stream);
	}

	static const std::vector<OptDef>& crondOptionDefs() {
		static const std::vector<OptDef> defs = {
			{ cron_detail::kCrondForeground,  'f', nullptr,       OptArg::None     },
			{ cron_detail::kCrondBackground,  'b', nullptr,       OptArg::None     },
			{ cron_detail::kCrondSyslog,      'S', nullptr,       OptArg::None     },
			{ cron_detail::kCrondLevel,       'l', nullptr,       OptArg::Required },
			{ cron_detail::kCrondDebug,       'd', nullptr,       OptArg::Required },
			{ cron_detail::kCrondLogFile,     'L', nullptr,       OptArg::Required },
			{ cron_detail::kCrondDirectory,   'c', nullptr,       OptArg::Required },
			{ cron_detail::kCrondOnce,        0,   "once",        OptArg::Required },
			{ cron_detail::kCrondLockHandle,  0,   "lock-handle", OptArg::Required },
			{ cron_detail::kCrondHelp,        0,   "help",        OptArg::None     },
		};
		return defs;
	}

	static bool parseLevel(const std::string& text, int& level) {
		std::size_t used = 0;
		const bool digits = !text.empty() && text[0] >= '0' && text[0] <= '9';
		if (digits && parseInt(text, level, 10, &used) && used == text.size()) return true;

		perr("crond", "invalid number '" + text + "'");
		return false;
	}

	static bool parseHandle(const std::string& text, unsigned long& handle) {
		std::size_t used = 0;
		if (parseUL(text, handle, 10, &used) && used == text.size()) return true;

		perr("crond", "invalid handle '" + text + "'");
		return false;
	}

	static bool applyCrondHit(const OptHit& hit, CrondOptions& options) {
		switch (hit.id) {
			case cron_detail::kCrondForeground:
				options.foreground = true;
				return true;
			case cron_detail::kCrondBackground:
				options.foreground = false;
				return true;
			case cron_detail::kCrondSyslog:
				options.log_file.clear();
				options.to_stderr = false;
				return true;
			case cron_detail::kCrondLevel:
				return parseLevel(hit.value, options.level);
			case cron_detail::kCrondDebug:
				options.to_stderr = true;
				options.foreground = true;
				return parseLevel(hit.value, options.level);
			case cron_detail::kCrondLogFile:
				options.log_file = hit.value;
				options.to_stderr = false;
				return true;
			case cron_detail::kCrondDirectory:
				options.directory = hit.value;
				return true;
			case cron_detail::kCrondOnce:
				options.once = hit.value;
				return true;
			case cron_detail::kCrondLockHandle:
				return parseHandle(hit.value, options.lock_handle);
			default:
				options.help = true;
				return true;
		}
	}

	static bool parseCrondArgs(const std::vector<std::string>& args, CrondOptions& options) {
		OptScanResult scanned;
		bool parsed = scanOptions("crond", args, crondOptionDefs(), scanned);
		for (std::size_t i = 0; parsed && i < scanned.hits.size(); ++i) {
			parsed = applyCrondHit(scanned.hits[i], options);
		}

		if (parsed && !scanned.operands.empty()) {
			perr("crond", "extra operand '" + scanned.operands[0] + "'");
			parsed = false;
		}

		if (!parsed) printCrondUsage(stderr);
		return parsed;
	}

	static fs::path absolutePath(const fs::path& path) {
		std::error_code ec;
		const fs::path absolute = fs::absolute(path, ec);
		return ec ? path : absolute.lexically_normal();
	}

	static bool makeDirectory(const fs::path& directory) {
		std::error_code ec;
		fs::create_directories(directory, ec);
		if (!ec) return true;

		perr("crond", "can't create directory '" + pathToUtf8(directory) + "': "
			+ posixErrorText(ec));
		return false;
	}

	static bool checkLog(const CrondLog& log) {
		if (crondLogWritable(log)) return true;

		perr("crond", "can't open '" + pathToUtf8(log.file) + "'");
		return false;
	}

	static bool prepareSetup(Executor& exec, const CrondOptions& options, CrondSetup& setup) {
		setup.directory = crontabDirectory(exec, options.directory);
		setup.user = currentUserName(exec);
		setup.log.level = options.level;
		setup.lock_handle = options.lock_handle;
		if (!options.to_stderr) {
			setup.log.file = options.log_file.empty() ? setup.directory / cron_detail::kLogFileName
				: absolutePath(toNative(exec, options.log_file));
		}

		setup.once = !options.once.empty();
		if (setup.once && !parseCivilMinute(options.once, setup.once_at)) {
			perr("crond", "invalid time '" + options.once + "'");
			return false;
		}

		return makeDirectory(setup.directory) && checkLog(setup.log);
	}

	static std::string lastErrorText() {
		return posixErrorText(std::error_code(static_cast<int>(::GetLastError()),
			std::system_category()));
	}

	static fs::path pidFilePath(const fs::path& directory) {
		return directory / cron_detail::kPidFileName;
	}

	// The PID file is also the lock: the crond holding it keeps it open for
	// writing and lets others only read it, so opening it for writing is
	// the one atomic test, and nothing opens it for writing just to look.
	static HANDLE openPidFile(const fs::path& path, bool inheritable) {
		SECURITY_ATTRIBUTES attributes{};
		attributes.nLength = sizeof(attributes);
		attributes.bInheritHandle = inheritable ? TRUE : FALSE;
		return ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
			OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	}

	static unsigned long recordedPid(const fs::path& directory) {
		std::string text;
		std::error_code ec;
		unsigned long pid = 0;
		if (readWholeFile(pidFilePath(directory), text, ec)) parseUL(text, pid);
		return pid;
	}

	static void reportRunning(const fs::path& directory, unsigned long pid) {
		const std::string which = pid == 0 ? "" : " (pid " + std::to_string(pid) + ")";
		perr("crond", "already running for " + pathToUtf8(directory) + which);
	}

	// TerminateProcess returns, and the exit code is set, before Windows has
	// closed the process's handles; a crond just killed still holds the
	// lock until then.
	static void waitForEndingCrond(unsigned long pid) {
		const HANDLE process = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
			FALSE, pid);
		if (process == nullptr) return;

		DWORD code = 0;
		if (::GetExitCodeProcess(process, &code) != 0 && code != STILL_ACTIVE) {
			::WaitForSingleObject(process, cron_detail::kTeardownWaitMs);
		}

		::CloseHandle(process);
	}

	static bool lockDirectory(const fs::path& directory, bool inheritable, CrondLock& lock) {
		lock.path = pidFilePath(directory);
		lock.file = openPidFile(lock.path, inheritable);
		if (lock.file == INVALID_HANDLE_VALUE && ::GetLastError() == ERROR_SHARING_VIOLATION) {
			waitForEndingCrond(recordedPid(directory));
			lock.file = openPidFile(lock.path, inheritable);
		}

		if (lock.file != INVALID_HANDLE_VALUE) return true;

		if (::GetLastError() == ERROR_SHARING_VIOLATION) {
			reportRunning(directory, recordedPid(directory));
			return false;
		}

		perr("crond", "can't open " + pathToUtf8(lock.path) + ": " + lastErrorText());
		return false;
	}

	static bool writePid(HANDLE file, unsigned long pid) {
		const std::string text = std::to_string(pid) + "\n";
		const LARGE_INTEGER start{};
		DWORD written = 0;
		const bool wrote = ::SetFilePointerEx(file, start, nullptr, FILE_BEGIN) != 0
			&& ::WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written,
				nullptr) != 0
			&& written == text.size();
		return wrote && ::SetEndOfFile(file) != 0;
	}

	static void releaseLock(CrondLock& lock) {
		::CloseHandle(lock.file);
		lock.file = INVALID_HANDLE_VALUE;
		::DeleteFileW(lock.path.c_str());
	}

	static bool recordOwnPid(CrondLock& lock) {
		if (writePid(lock.file, ::GetCurrentProcessId())) return true;

		perr("crond", "can't write " + pathToUtf8(lock.path) + ": " + lastErrorText());
		releaseLock(lock);
		return false;
	}

	static bool isFileAt(HANDLE file, const fs::path& path) {
		BY_HANDLE_FILE_INFORMATION held{};
		FileIdentity expected;
		if (::GetFileInformationByHandle(file, &held) == 0) return false;
		if (!queryFileIdentity(path, expected)) return false;

		FileIdentity actual;
		actual.volume = held.dwVolumeSerialNumber;
		actual.index = (std::uint64_t{ held.nFileIndexHigh } << 32) | held.nFileIndexLow;
		return actual == expected;
	}

	// A background crond is handed its parent's open PID file, so no other
	// crond can slip in between the parent's check and the child's start.
	static bool adoptLock(const CrondSetup& setup, CrondLock& lock) {
		const HANDLE handed_over = ::ULongToHandle(setup.lock_handle);
		if (!isFileAt(handed_over, pidFilePath(setup.directory))) {
			return lockDirectory(setup.directory, false, lock) && recordOwnPid(lock);
		}

		::SetHandleInformation(handed_over, HANDLE_FLAG_INHERIT, 0);
		lock.path = pidFilePath(setup.directory);
		lock.file = handed_over;
		return recordOwnPid(lock);
	}

	static bool acquireLock(const CrondSetup& setup, CrondLock& lock) {
		if (setup.lock_handle != 0) return adoptLock(setup, lock);
		return lockDirectory(setup.directory, false, lock) && recordOwnPid(lock);
	}

	static std::string bootIdentity() {
		DWORD boot_id = 0;
		DWORD size = sizeof(boot_id);
		const LSTATUS status = ::RegGetValueW(HKEY_LOCAL_MACHINE, cron_detail::kPrefetchKey,
			L"BootId", RRF_RT_REG_DWORD, nullptr, &boot_id, &size);
		if (status == ERROR_SUCCESS) return "boot " + std::to_string(boot_id);

		const std::int64_t uptime = static_cast<std::int64_t>(::GetTickCount64() / 1000);
		const std::int64_t booted = static_cast<std::int64_t>(std::time(nullptr)) - uptime;
		return "booted " + std::to_string(booted / cron_detail::kBootTimeGranularity);
	}

	static bool firstStartSinceBoot(const CrondSetup& setup) {
		const fs::path stamp = setup.directory / cron_detail::kRebootStampName;
		const std::string identity = bootIdentity() + "\n";

		std::string recorded;
		std::error_code ec;
		if (readWholeFile(stamp, recorded, ec) && recorded == identity) return false;

		writeWholeFile(stamp, identity, ec);
		return true;
	}

	static std::map<std::string, CrontabFile> listCrontabFiles(const fs::path& directory) {
		std::map<std::string, CrontabFile> files;
		std::error_code ec;
		for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
			const std::string name = pathToUtf8(it->path().filename());
			std::error_code entry_error;
			if (!isCrontabName(name) || !it->is_regular_file(entry_error)) continue;

			CrontabFile file;
			file.path = it->path();
			file.modified = it->last_write_time(entry_error);
			file.size = it->file_size(entry_error);
			files.emplace(name, file);
		}

		return files;
	}

	static void logSyntaxErrors(const CrondState& state, const std::string& user,
	                            const CronTable& table) {
		for (const CronSyntaxError& error : table.errors) {
			crondLog(state.setup.log, kCrondLogError, "user " + user + ": parse error at line "
				+ std::to_string(error.line) + ": " + error.message);
		}
	}

	static bool isCrondUser(const CrondState& state, const std::string& user) {
		const std::string& own = state.setup.user;
		return user.size() == own.size() && std::equal(user.begin(), user.end(), own.begin(),
			[](char a, char b) { return std::tolower(static_cast<unsigned char>(a))
				== std::tolower(static_cast<unsigned char>(b)); });
	}

	static CronTable readCrontab(const CrondState& state, const std::string& user,
	                             const CrontabFile& file) {
		if (!isCrondUser(state, user)) {
			crondLog(state.setup.log, kCrondLogError, "user " + user
				+ ": not the user crond runs as (" + state.setup.user + "), ignored");
			return CronTable();
		}

		std::string text;
		std::error_code ec;
		if (!readWholeFile(file.path, text, ec)) {
			crondLog(state.setup.log, kCrondLogError, "user " + user + ": can't read crontab: "
				+ posixErrorText(ec));
			return CronTable();
		}

		CronTable table = parseCrontab(text);
		logSyntaxErrors(state, user, table);
		crondLog(state.setup.log, kCrondLogDebug, "user " + user + ": "
			+ std::to_string(table.entries.size()) + " job(s) loaded");
		return table;
	}

	static void loadCrontab(CrondState& state, const std::string& user, const CrontabFile& file) {
		LoadedCrontab loaded;
		loaded.modified = file.modified;
		loaded.size = file.size;
		loaded.table = readCrontab(state, user, file);
		state.crontabs[user] = std::move(loaded);
	}

	static void dropRemovedCrontabs(CrondState& state,
	                                const std::map<std::string, CrontabFile>& files) {
		for (auto known = state.crontabs.begin(); known != state.crontabs.end();) {
			if (files.count(known->first) != 0) {
				++known;
				continue;
			}

			crondLog(state.setup.log, kCrondLogDebug, "user " + known->first + ": crontab removed");
			known = state.crontabs.erase(known);
		}
	}

	static bool hasChanged(const LoadedCrontab& loaded, const CrontabFile& file) {
		return loaded.modified != file.modified || loaded.size != file.size;
	}

	static void rescanCrontabs(CrondState& state) {
		const std::map<std::string, CrontabFile> files = listCrontabFiles(state.setup.directory);
		dropRemovedCrontabs(state, files);

		for (const auto& [user, file] : files) {
			const auto known = state.crontabs.find(user);
			if (known != state.crontabs.end() && !hasChanged(known->second, file)) continue;
			loadCrontab(state, user, file);
		}
	}

	static void startJob(CrondState& state, const std::string& user, const CronEntry& entry) {
		const JobLaunch launch{ state.exec, state.setup.log, user };
		startCronJob(launch, entry, state.jobs);
	}

	static void runRebootJobs(CrondState& state) {
		if (!firstStartSinceBoot(state.setup)) return;

		for (const auto& [user, loaded] : state.crontabs) {
			for (const CronEntry& entry : loaded.table.entries) {
				if (entry.schedule.at_reboot) startJob(state, user, entry);
			}
		}
	}

	static void runDueJobs(CrondState& state, std::int64_t minute_start) {
		for (const auto& [user, loaded] : state.crontabs) {
			for (const CronEntry& entry : loaded.table.entries) {
				if (!isCronDue(entry.schedule, minute_start, *state.zone)) continue;
				startJob(state, user, entry);
			}
		}
	}

	static std::int64_t currentMinuteStart() {
		const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
		return now - now % cron_detail::kSecondsPerMinute;
	}

	static unsigned long sliceUntilNextMinute() {
		const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
		const long long into_minute = std::chrono::duration_cast<std::chrono::milliseconds>(
			since_epoch).count() % cron_detail::kMillisecondsPerMinute;
		const long long until_next = cron_detail::kMillisecondsPerMinute - into_minute
			+ cron_detail::kPastTheMinuteMs;
		return static_cast<unsigned long>(std::min(until_next, cron_detail::kSliceMs));
	}

	static bool isNewMinute(std::int64_t last, std::int64_t now) {
		if (now > last) return true;
		return last - now > cron_detail::kBackwardPatience;
	}

	static int serveMinutes(CrondState& state) {
		std::int64_t last = currentMinuteStart();
		while (sleepUnlessCtrlC(sliceUntilNextMinute())) {
			collectFinishedJobs(state.setup.log, state.jobs);
			const std::int64_t now = currentMinuteStart();
			if (!isNewMinute(last, now)) continue;

			last = now;
			rescanCrontabs(state);
			runDueJobs(state, now);
		}

		takeCtrlC();
		collectFinishedJobs(state.setup.log, state.jobs);
		releaseJobs(state.jobs);
		return kSignalStatusBase + cron_detail::kSignalInterrupt;
	}

	static int runOnce(CrondState& state) {
		runDueJobs(state, instantOfCivil(*state.zone, state.setup.once_at));
		waitForJobs(state.setup.log, state.jobs);
		releaseJobs(state.jobs);
		return 0;
	}

	static int runForeground(Executor& exec, const CrondSetup& setup) {
		CrondLock lock;
		if (!acquireLock(setup, lock)) return 1;

		const ScopedCtrlCCapture capture;
		CrondState state{ exec, setup, localCronZone() };
		crondLog(setup.log, kCrondLogNotice, "crond (wbsh) started, log level "
			+ std::to_string(setup.log.level));
		rescanCrontabs(state);
		runRebootJobs(state);

		const int status = setup.once ? runOnce(state) : serveMinutes(state);
		releaseLock(lock);
		return status;
	}

	static std::string foregroundCommand(const CrondSetup& setup, HANDLE lock) {
		return quoteShellArgv({ "crond", "-f", "-c", pathToUtf8(setup.directory), "-l",
			std::to_string(setup.log.level), "-L", pathToUtf8(setup.log.file), "--lock-handle",
			std::to_string(::HandleToULong(lock)) });
	}

	static std::wstring systemDriveRoot() {
		wchar_t windows[MAX_PATH + 1] = {};
		const UINT length = ::GetWindowsDirectoryW(windows, MAX_PATH + 1);
		if (length < 3 || windows[1] != L':') return std::wstring();
		return std::wstring(windows, 3);
	}

	static HANDLE openInheritableNul() {
		SECURITY_ATTRIBUTES attributes{};
		attributes.nLength = sizeof(attributes);
		attributes.bInheritHandle = TRUE;
		return ::CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_EXISTING, 0, nullptr);
	}

	static HiddenProcess backgroundCrond(Executor& exec, const CrondSetup& setup, HANDLE lock) {
		const std::string self = selfExecutablePath();
		HiddenProcess process;
		process.application = utf8ToWide(self);
		process.command_line = windowsCommandLine({ self, "-r", "-c",
			foregroundCommand(setup, lock) });
		process.environment = environmentBlock(exec, exportedEnvironment(exec));
		process.directory = systemDriveRoot();
		process.extra_flags = CREATE_NEW_PROCESS_GROUP | jobBreakawayFlag();
		process.input = openInheritableNul();
		process.output = process.input;
		process.handed_over = lock;
		return process;
	}

	// The lock is taken here and handed to the hidden crond, which keeps
	// it; the PID file names that crond before this one returns.
	static int startInBackground(Executor& exec, const CrondSetup& setup) {
		CrondLock lock;
		if (!lockDirectory(setup.directory, true, lock)) return 1;

		writePid(lock.file, ::GetCurrentProcessId());
		HiddenProcess process = backgroundCrond(exec, setup, lock.file);
		const HANDLE started = startHiddenProcess(process);
		const std::string failure = lastErrorText();
		if (process.input != INVALID_HANDLE_VALUE) ::CloseHandle(process.input);
		if (started == nullptr) {
			perr("crond", "can't start in the background: " + failure);
			releaseLock(lock);
			return 1;
		}

		writePid(lock.file, ::GetProcessId(started));
		::CloseHandle(lock.file);
		::CloseHandle(started);
		return 0;
	}

	int runCrond(Executor& exec, const std::vector<std::string>& args) {
		CrondOptions options;
		if (!parseCrondArgs(args, options)) return 1;
		if (options.help) {
			printCrondUsage(stdout);
			return 0;
		}

		CrondSetup setup;
		if (!prepareSetup(exec, options, setup)) return 1;
		if (!options.foreground && !setup.once) return startInBackground(exec, setup);

		return runForeground(exec, setup);
	}

#else

	int runCrond(Executor&, const std::vector<std::string>&) {
		perr("crond", "needs Windows");
		return 1;
	}

#endif /* _WIN32 */

}  // namespace wbsh

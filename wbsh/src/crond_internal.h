#pragma once

/**
 * @file crond_internal.h
 * @brief crond's parts: its log, hidden processes, and the jobs it has running.
 *
 * Log levels are busybox crond's: a message shows when its level is at
 * least the -l level, so 0 shows everything and the default 8 shows
 * start-up, each job started and its output, and every error.
 */

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif /* _WIN32 */

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "cron_schedule.h"

namespace wbsh {

	class Executor;

#ifdef _WIN32

	constexpr int kCrondLogDebug = 5;
	constexpr int kCrondLogInfo = 7;
	constexpr int kCrondLogNotice = 8;
	constexpr int kCrondLogError = 9;

	/// Where crond's messages go: @c file, or standard error when it is empty (-d).
	struct CrondLog {
		std::filesystem::path file;
		int level = kCrondLogNotice;
	};

	/// Append "Mon DD HH:MM:SS crond[PID]: message" when @p level passes the log's level.
	void crondLog(const CrondLog& log, int level, const std::string& message);

	/// Append @p text under a stamped @p header line to @p destination, or
	/// to the log when @p destination is empty.
	void appendCrondOutput(const CrondLog& log, const std::filesystem::path& destination,
	                       const std::string& header, const std::string& text);

	/// Whether the log file can be opened for appending.
	bool crondLogWritable(const CrondLog& log);

	using CrondEnvironment = std::vector<std::pair<std::string, std::string>>;

	/// A process started without a window, on @c input and @c output (as
	/// standard output and standard error); those and @c handed_over, when
	/// set, are inherited, nothing else.
	struct HiddenProcess {
		std::wstring application;
		std::wstring command_line;
		std::wstring environment;
		std::wstring directory;
		HANDLE input = INVALID_HANDLE_VALUE;
		HANDLE output = INVALID_HANDLE_VALUE;
		HANDLE handed_over = nullptr;
		DWORD extra_flags = 0;
	};

	/// The process handle, or nullptr with GetLastError() set.
	HANDLE startHiddenProcess(HiddenProcess& process);

	/// The command line that runs @p argv, quoted as the C runtime parses it.
	std::wstring windowsCommandLine(const std::vector<std::string>& argv);

	/// The shell's exported variables, less wbsh's internal WBSH_* hand-overs.
	CrondEnvironment exportedEnvironment(Executor& exec);

	/// A CreateProcess environment block; PATH, HOME, TEMP and TMP go out in Win32 form.
	std::wstring environmentBlock(Executor& exec, const CrondEnvironment& variables);

	struct RunningJob {
		HANDLE process = nullptr;
		unsigned long pid = 0;
		std::filesystem::path output;
		std::filesystem::path input;
		std::filesystem::path destination;
		std::string user;
		std::string text;
		bool discard = false;
	};

	struct JobLaunch {
		Executor& exec;
		const CrondLog& log;
		const std::string& user;
	};

	/// Start @p entry for @p launch's user and add it to @p jobs; failures are logged.
	void startCronJob(const JobLaunch& launch, const CronEntry& entry,
	                  std::vector<RunningJob>& jobs);

	/// Write out and forget every job that has ended.
	void collectFinishedJobs(const CrondLog& log, std::vector<RunningJob>& jobs);

	/// Collect jobs until none is left or Ctrl+C is pressed.
	void waitForJobs(const CrondLog& log, std::vector<RunningJob>& jobs);

	/// Let the jobs run on unwatched.
	void releaseJobs(std::vector<RunningJob>& jobs);

#endif /* _WIN32 */

}  // namespace wbsh

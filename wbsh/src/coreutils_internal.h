#pragma once

/**
 * @file coreutils_internal.h
 * @brief Internal split-points between coreutils translation units.
 *
 * coreutils.cpp is the main bundled-coreutils entry; self-contained
 * subsystems (text processing, archives, encodings, the `bc`
 * calculator, `curl`, hashing) live in their own files and expose only
 * a per-category register function so that registerCoreutils() can
 * wire them in without seeing their internals.
 */

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace wbsh {

	class Executor;

	void perr(const std::string& cmd, const std::string& path,
	          const std::error_code& ec);
	void perr(const std::string& cmd, const std::string& msg);

	/// strerror-style text ("No such file or directory") for errors that map
	/// to an errno value; Windows' own message for the rest.
	std::string posixErrorText(const std::error_code& ec);

	/// Translate an arg-style path (POSIX-or-Win32, UTF-8) to a native
	/// filesystem::path without mojibake on non-ASCII names.
	std::filesystem::path toNative(Executor& exec, const std::string& p);

	/// fopen on a UTF-8, POSIX-or-Win32 path (`_wfopen` on Windows).
	std::FILE* fopenNative(Executor& exec, const std::string& p, const char* mode);

	/// The directory named by the shell's TMPDIR, TEMP or TMP, in that order
	/// (POSIX or Win32 form), else the system's; empty when there is none.
	std::filesystem::path shellTempDirectory(Executor& exec);

	/// Symbolic links and NTFS junctions are both "links" to the POSIX tools.
	bool isLinkStatus(const std::filesystem::file_status& status);

	/// Volume serial plus file index: equal for every name of one file or directory.
	struct FileIdentity {
		std::uint64_t volume = 0;
		std::uint64_t index = 0;

		bool operator==(const FileIdentity& other) const {
			return volume == other.volume && index == other.index;
		}
	};

	/// Identity of the file @p native resolves to (links followed); false if it cannot be opened.
	bool queryFileIdentity(const std::filesystem::path& native, FileIdentity& identity);

	/// Create and open (binary, write) a new file `<prefix>XXXXXX` in the directory of
	/// @p target, so it can later replace @p target by rename; nullptr with errno set on failure.
	std::FILE* createSiblingTempFile(const std::filesystem::path& target, const std::string& prefix,
	                                 std::filesystem::path& temp);

	/// The words of @p argv single-quoted for the shell parser, joined by spaces.
	std::string quoteShellArgv(const std::vector<std::string>& argv);

	/// .exe / .bat / .cmd / .com / .ps1: what `ls` and `find -perm` treat as executable.
	bool hasExecutableExtension(const std::filesystem::path& full);

	/// Remove a file, link or empty directory, clearing a read-only attribute if that is
	/// what stands in the way. Links are removed, never followed.
	bool removeFileOrEmptyDirectory(const std::filesystem::path& native, std::error_code& ec);

	struct ProcessInfo {
		unsigned long pid = 0;
		unsigned long parent_pid = 0;
		unsigned long threads = 0;
		long base_priority = 0;
		std::string name;
	};

	/// Every running process (Toolhelp snapshot order).
	std::vector<ProcessInfo> listProcesses();

	/// The command line @p pid was started with; empty when Windows refuses to tell.
	std::string processCommandLine(unsigned long pid);

	struct ProcessUsage {
		std::string user;
		std::uint64_t working_set = 0;
		std::uint64_t pagefile = 0;
		std::int64_t cpu_ticks = 0;
	};

	/// Owner, memory and CPU time (100 ns ticks) of every process by PID, read
	/// without opening any of them, so known for other users' processes too.
	std::map<unsigned long, ProcessUsage> collectProcessUsage();

	/// A signal operand as kill takes it: a number, or a name with or without SIG.
	bool parseSignalName(const std::string& text, int& signum);

	/// Ends @p pid with exit code 128 + @p signum; signal 0 only checks it is running.
	bool terminateProcessId(unsigned long pid, int signum);

	/// "2 days,  3:04": the time since boot as `uptime` and `top` show it.
	std::string uptimeText(unsigned long long uptime_ms);

	/// Size and free bytes of all page files together.
	void pageFileUsage(unsigned long long& total, unsigned long long& free);

	/// Niceness <-> priority class, the one mapping nice, renice, ps and top share
	/// (coreutils_session.cpp has the table).
	unsigned long priorityClassForNiceness(int niceness);
	int nicenessOfPriorityClass(unsigned long priority_class);
	/// From Toolhelp's thread base priority, which needs no handle to the process.
	int nicenessOfBasePriority(long base_priority);

	bool queryProcessNiceness(unsigned long pid, int& niceness, std::error_code& ec);
	bool setProcessNiceness(unsigned long pid, int niceness, std::error_code& ec);

	/// Append every line of `path` ("-" = stdin) to `lines`, stripping
	/// the trailing `\n` / `\r\n`. Returns false if the file can't be
	/// opened (errno is set).
	bool readAllLines(Executor& exec, const std::string& path,
	                  std::vector<std::string>& lines);

	/// zlib / gzip / zip CRC-32; pass 0 to start, the previous result to continue.
	std::uint32_t crc32Update(std::uint32_t crc, const std::uint8_t* data, std::size_t length);

	/// $USER, then $USERNAME, then "user".
	std::string currentUserName(Executor& exec);

	/// True only for a real console: _isatty also says yes for NUL.
	struct WindowsVersion {
		unsigned long major = 0;
		unsigned long minor = 0;
		unsigned long build = 0;
	};

	WindowsVersion windowsVersion();

	bool isConsoleStream(std::FILE* stream);

	/// A regular file, where a read never waits for a writer.
	bool isDiskStream(std::FILE* stream);

	/// A write to stdout failed, typically because the pipe's reader exited.
	/// Producer loops stop then, as SIGPIPE would stop them on POSIX.
	bool stdoutFailed();

	/// The status a builtin stopped by stdoutFailed() returns: 141 (killed by
	/// SIGPIPE) when stdout is a pipe, 1 otherwise.
	int stdoutFailureStatus();

	/// Visible console window size; false when stdout is not a console.
	bool queryConsoleSize(int& columns, int& rows);

	/// A count with an optional GNU size suffix: b (512), kB / KB (1000), K / KiB
	/// (1024), and the same for M, G, T, P, E; dd's c (1) and w (2) as well.
	bool parseByteSize(const std::string& text, unsigned long long& bytes);

	/// `sleep`-style interval: a number with an optional s / m / h / d unit.
	bool parseDurationSeconds(const std::string& arg, double& seconds);

	enum class ProcessProbe { Alive, Missing, Denied };

	/// The `kill -0` check: whether @p pid names a process that is still running.
	ProcessProbe probeProcess(unsigned long pid);

	void registerHeadTailBuiltins(Executor& exec);

	void registerTextBuiltins(Executor& exec);

	void registerTrBuiltin(Executor& exec);

	void registerSortBuiltin(Executor& exec);

	void registerXargsBuiltin(Executor& exec);

	void registerEncodingBuiltins(Executor& exec);

	void registerDumpBuiltins(Executor& exec);

	void registerArchiveBuiltins(Executor& exec);

	void registerCompressBuiltins(Executor& exec);

	void registerTarBuiltin(Executor& exec);

	void registerPatchBuiltin(Executor& exec);

	void registerPagerBuiltins(Executor& exec);

	void registerWgetBuiltin(Executor& exec);

	void registerMiscBuiltins(Executor& exec);

	void registerCpioBuiltin(Executor& exec);

	void registerArBuiltin(Executor& exec);

	void registerSedBuiltin(Executor& exec);

	void registerGrepBuiltins(Executor& exec);

	void registerFindBuiltin(Executor& exec);

	void registerBcBuiltin(Executor& exec);

	void registerCurlBuiltin(Executor& exec);

	void registerHashBuiltins(Executor& exec);

	void registerProcessBuiltins(Executor& exec);

	void registerPsBuiltin(Executor& exec);

	void registerSystemExtraBuiltins(Executor& exec);

	void registerFileOpsBuiltins(Executor& exec);
	/// coreutils_stat.cpp: touch, stat.
	void registerStatBuiltins(Executor& exec);

	void registerRemoveBuiltin(Executor& exec);

	void registerCopyBuiltins(Executor& exec);

	void registerFilterBuiltins(Executor& exec);

	void registerFzfBuiltin(Executor& exec);
	void registerTmuxBuiltin(Executor& exec);
	void registerUtilsBuiltin(Executor& exec);

	void registerSessionBuiltins(Executor& exec);

	void registerTopBuiltin(Executor& exec);

	void registerOwnerBuiltins(Executor& exec);

	void registerInstallBuiltin(Executor& exec);

	void registerFileTypeBuiltin(Executor& exec);

	void registerManBuiltin(Executor& exec);

	void registerLocaleBuiltin(Executor& exec);

	void registerEdBuiltin(Executor& exec);

	void registerViBuiltin(Executor& exec);

	void registerMakeBuiltin(Executor& exec);

	void registerNcBuiltin(Executor& exec);

	void registerFtpBuiltins(Executor& exec);

	void registerWhoisBuiltin(Executor& exec);

	void registerHttpdBuiltin(Executor& exec);

	void registerCronBuiltins(Executor& exec);
	void registerScriptBuiltins(Executor& exec);

}  // namespace wbsh

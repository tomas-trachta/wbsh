/**
 * @file cron_jobs.cpp
 * @brief crond's jobs: the shell, environment and directory cron gives them, and their output.
 *
 * A job runs as `wbsh -r -c COMMAND` (or `SHELL -c COMMAND` for a SHELL
 * that names another program) without a window and in a console process
 * group of its own, with HOME as its directory. Its standard input is the
 * text after `%`, or nothing; standard output and standard error go to a
 * temporary file that crond appends to the log once the job has ended,
 * as cron mails it: only when there was output, and never with MAILTO="".
 */

#include "crond_internal.h"

#ifdef _WIN32

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "coreutils_internal.h"
#include "cron_internal.h"
#include "environment.h"
#include "executor.h"
#include "exitstatus.h"
#include "interrupt.h"
#include "pathconv.h"

namespace wbsh {

	namespace fs = std::filesystem;

	namespace cron_detail {

		static const char* const kKeptWbshVariables[] = {
			"WBSH_PLUGINS", "WBSH_NO_PATHCONV", "WBSH_CRONTAB_DIR",
		};

		static const DWORD kJobPollMs = 50;

		struct JobShell {
			std::string program;
			std::vector<std::string> argv;
			std::string variable;
		};

	}  // namespace cron_detail

	using cron_detail::JobShell;

	static bool argumentNeedsQuotes(const std::wstring& argument) {
		return argument.empty() || argument.find_first_of(L" \t\n\v\"") != std::wstring::npos;
	}

	// Backslashes count only before a quote: double them there, and
	// escape the quote itself.
	static std::wstring quoteArgument(const std::wstring& argument) {
		if (!argumentNeedsQuotes(argument)) return argument;

		std::wstring quoted = L"\"";
		std::size_t backslashes = 0;
		for (const wchar_t c : argument) {
			if (c == L'\\') {
				++backslashes;
				continue;
			}

			quoted.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
			quoted.push_back(c);
			backslashes = 0;
		}

		quoted.append(backslashes * 2, L'\\');
		quoted.push_back(L'"');
		return quoted;
	}

	std::wstring windowsCommandLine(const std::vector<std::string>& argv) {
		std::wstring line;
		for (const std::string& argument : argv) {
			if (!line.empty()) line.push_back(L' ');
			line += quoteArgument(utf8ToWide(argument));
		}

		return line;
	}

	static bool isHandOver(const std::string& name) {
		if (name.compare(0, 5, "WBSH_") != 0) return false;

		for (const char* kept : cron_detail::kKeptWbshVariables) {
			if (name == kept) return false;
		}

		return true;
	}

	CrondEnvironment exportedEnvironment(Executor& exec) {
		const Environment& env = exec.env();
		CrondEnvironment variables;
		for (const std::string& name : env.exportedSet()) {
			if (isHandOver(name) || !env.has(name)) continue;
			variables.emplace_back(name, env.get(name));
		}

		return variables;
	}

	static bool sameNameIgnoringCase(const std::string& a, const std::string& b) {
		return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
			[](char x, char y) { return std::toupper(static_cast<unsigned char>(x))
				== std::toupper(static_cast<unsigned char>(y)); });
	}

	static void setVariable(CrondEnvironment& variables, const std::string& name,
	                        const std::string& value) {
		for (auto& variable : variables) {
			if (!sameNameIgnoringCase(variable.first, name)) continue;
			variable = { name, value };
			return;
		}

		variables.emplace_back(name, value);
	}

	static const std::string* findVariable(const CrondEnvironment& variables,
	                                       const std::string& name) {
		for (const auto& variable : variables) {
			if (sameNameIgnoringCase(variable.first, name)) return &variable.second;
		}

		return nullptr;
	}

	// Windows programs read PATH, HOME, TEMP and TMP; a PATH already in
	// Win32 form (`;`-separated) is left alone.
	static std::string win32Value(Executor& exec,
	                              const std::pair<std::string, std::string>& variable) {
		const PathConv& conv = exec.pathConv();
		if (sameNameIgnoringCase(variable.first, "PATH")) {
			if (variable.second.find(';') != std::string::npos) return variable.second;
			return conv.pathListPosixToWin32(variable.second);
		}

		if (sameNameIgnoringCase(variable.first, "HOME")) return conv.toWin32Short(variable.second);
		const bool temp = sameNameIgnoringCase(variable.first, "TEMP")
			|| sameNameIgnoringCase(variable.first, "TMP");
		return temp ? conv.toWin32(variable.second) : variable.second;
	}

	static bool entryLess(const std::wstring& a, const std::wstring& b) {
		return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
			[](wchar_t x, wchar_t y) { return std::towlower(x) < std::towlower(y); });
	}

	std::wstring environmentBlock(Executor& exec, const CrondEnvironment& variables) {
		std::vector<std::wstring> entries;
		for (const auto& variable : variables) {
			entries.push_back(utf8ToWide(variable.first + "=" + win32Value(exec, variable)));
		}

		std::sort(entries.begin(), entries.end(), entryLess);

		std::wstring block;
		for (const std::wstring& entry : entries) {
			block += entry;
			block.push_back(L'\0');
		}

		if (entries.empty()) block.push_back(L'\0');
		block.push_back(L'\0');
		return block;
	}

	static STARTUPINFOEXW hiddenStartupInfo(const HiddenProcess& process,
	                                        LPPROC_THREAD_ATTRIBUTE_LIST attributes) {
		STARTUPINFOEXW info{};
		info.StartupInfo.cb = sizeof(info);
		info.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
		info.StartupInfo.wShowWindow = SW_HIDE;
		info.StartupInfo.hStdInput = process.input;
		info.StartupInfo.hStdOutput = process.output;
		info.StartupInfo.hStdError = process.output;
		info.lpAttributeList = attributes;
		return info;
	}

	static LPPROC_THREAD_ATTRIBUTE_LIST newAttributeList() {
		SIZE_T size = 0;
		::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
		const auto attributes = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
			::HeapAlloc(::GetProcessHeap(), 0, size));
		if (attributes == nullptr) return nullptr;
		if (::InitializeProcThreadAttributeList(attributes, 1, 0, &size) != 0) return attributes;

		::HeapFree(::GetProcessHeap(), 0, attributes);
		return nullptr;
	}

	// The inherited[] array has to outlive every use of the attribute
	// list, so both live in this one frame.
	HANDLE startHiddenProcess(HiddenProcess& process) {
		const LPPROC_THREAD_ATTRIBUTE_LIST attributes = newAttributeList();
		if (attributes == nullptr) return nullptr;

		HANDLE inherited[3] = { process.input, process.output, process.handed_over };
		DWORD count = process.input == process.output ? 1 : 2;
		if (process.handed_over != nullptr) inherited[count++] = process.handed_over;
		::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
			count * sizeof(HANDLE), nullptr, nullptr);

		STARTUPINFOEXW info = hiddenStartupInfo(process, attributes);
		PROCESS_INFORMATION started{};
		const DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT
			| CREATE_NO_WINDOW | process.extra_flags;
		const BOOL created = ::CreateProcessW(process.application.c_str(),
			process.command_line.data(), nullptr, nullptr, TRUE, flags, process.environment.data(),
			process.directory.empty() ? nullptr : process.directory.c_str(), &info.StartupInfo,
			&started);
		const DWORD error = ::GetLastError();
		::DeleteProcThreadAttributeList(attributes);
		::HeapFree(::GetProcessHeap(), 0, attributes);
		if (created == 0) {
			::SetLastError(error);
			return nullptr;
		}

		::CloseHandle(started.hThread);
		return started.hProcess;
	}

	static std::string lowerBaseName(const std::string& path) {
		const std::size_t slash = path.find_last_of("/\\");
		std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
		for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

		const std::string extension = ".exe";
		const bool has_extension = name.size() > extension.size()
			&& name.compare(name.size() - extension.size(), extension.size(), extension) == 0;
		if (has_extension) name.resize(name.size() - extension.size());
		return name;
	}

	// There is no /bin/sh to run, so the shells crontabs name mean wbsh.
	static bool meansWbsh(const std::string& shell) {
		const std::string name = lowerBaseName(shell);
		return name.empty() || name == "sh" || name == "bash" || name == "wbsh";
	}

	static std::string findProgram(Executor& exec, const std::string& wanted) {
		std::error_code ec;
		const fs::path native = toNative(exec, wanted);
		if (fs::is_regular_file(native, ec)) return pathToUtf8(native);
		return exec.findExecutable(wanted);
	}

	static bool chooseShell(const JobLaunch& launch, const CronEntry& entry, JobShell& shell) {
		const std::string* wanted = findVariable(entry.variables, "SHELL");
		if (wanted == nullptr || meansWbsh(*wanted)) {
			shell.program = selfExecutablePath();
			shell.argv = { shell.program, "-r", "-c", entry.command };
			shell.variable = wanted != nullptr ? *wanted
				: launch.exec.pathConv().toPosix(shell.program);
			return true;
		}

		shell.program = findProgram(launch.exec, *wanted);
		shell.argv = { shell.program, "-c", entry.command };
		shell.variable = *wanted;
		if (!shell.program.empty()) return true;

		crondLog(launch.log, kCrondLogError, "user " + launch.user + ": can't find shell "
			+ *wanted);
		return false;
	}

	// LOGNAME (and USER) are always the crontab's owner, as in cron; HOME
	// and SHELL can be set in the crontab.
	static CrondEnvironment jobEnvironment(const JobLaunch& launch, const CronEntry& entry,
	                                       const JobShell& shell) {
		CrondEnvironment variables = exportedEnvironment(launch.exec);
		const std::string home = launch.exec.env().get("HOME");
		if (!home.empty()) setVariable(variables, "HOME", home);
		setVariable(variables, "SHELL", shell.variable);
		for (const auto& variable : entry.variables) {
			setVariable(variables, variable.first, variable.second);
		}

		setVariable(variables, "LOGNAME", launch.user);
		setVariable(variables, "USER", launch.user);
		return variables;
	}

	static bool chooseDirectory(const JobLaunch& launch, const CrondEnvironment& variables,
	                            std::wstring& directory) {
		const std::string* home = findVariable(variables, "HOME");
		if (home == nullptr || home->empty()) return true;

		std::error_code ec;
		const fs::path native = toNative(launch.exec, *home);
		if (fs::is_directory(native, ec)) {
			directory = native.wstring();
			return true;
		}

		crondLog(launch.log, kCrondLogError, "can't change directory to '" + *home + "'");
		return false;
	}

	static bool looksLikePath(const std::string& text) {
		return text.find_first_of("/\\") != std::string::npos;
	}

	// No mail on Windows: MAILTO="" still discards the output, and a MAILTO
	// that is a path gets it appended instead of the log.
	static void chooseDestination(const JobLaunch& launch, const CronEntry& entry,
	                              const CrondEnvironment& variables, RunningJob& job) {
		const std::string* mailto = findVariable(entry.variables, "MAILTO");
		if (mailto == nullptr) return;

		job.discard = mailto->empty();
		if (!looksLikePath(*mailto)) return;

		const std::string* home = findVariable(variables, "HOME");
		const bool from_home = mailto->compare(0, 2, "~/") == 0 && home != nullptr;
		job.destination = toNative(launch.exec, from_home ? *home + mailto->substr(1) : *mailto);
	}

	static SECURITY_ATTRIBUTES inheritableHandles() {
		SECURITY_ATTRIBUTES attributes{};
		attributes.nLength = sizeof(attributes);
		attributes.bInheritHandle = TRUE;
		return attributes;
	}

	static bool newTempFile(Executor& exec, fs::path& path) {
		const std::wstring directory = shellTempDirectory(exec).wstring();
		wchar_t name[MAX_PATH + 1] = {};
		if (::GetTempFileNameW(directory.c_str(), L"crn", 0, name) == 0) return false;

		path = name;
		return true;
	}

	static HANDLE openInherited(const std::wstring& path, DWORD access, DWORD disposition) {
		SECURITY_ATTRIBUTES attributes = inheritableHandles();
		return ::CreateFileW(path.c_str(), access,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &attributes, disposition,
			FILE_ATTRIBUTE_TEMPORARY, nullptr);
	}

	static bool openInput(const JobLaunch& launch, const CronEntry& entry, RunningJob& job,
	                      HiddenProcess& process) {
		if (entry.input.empty()) {
			process.input = openInherited(L"NUL", GENERIC_READ, OPEN_EXISTING);
			return process.input != INVALID_HANDLE_VALUE;
		}

		std::error_code ec;
		if (!newTempFile(launch.exec, job.input)) return false;
		if (!writeWholeFile(job.input, entry.input, ec)) return false;

		process.input = openInherited(job.input.wstring(), GENERIC_READ, OPEN_EXISTING);
		return process.input != INVALID_HANDLE_VALUE;
	}

	static bool openOutput(const JobLaunch& launch, RunningJob& job, HiddenProcess& process) {
		if (!newTempFile(launch.exec, job.output)) return false;

		process.output = openInherited(job.output.wstring(), GENERIC_WRITE, CREATE_ALWAYS);
		return process.output != INVALID_HANDLE_VALUE;
	}

	static void closeJobFiles(HiddenProcess& process) {
		if (process.input != INVALID_HANDLE_VALUE) ::CloseHandle(process.input);
		if (process.output != INVALID_HANDLE_VALUE) ::CloseHandle(process.output);
		process.input = INVALID_HANDLE_VALUE;
		process.output = INVALID_HANDLE_VALUE;
	}

	static void removeJobFiles(const RunningJob& job) {
		std::error_code ec;
		if (!job.input.empty()) fs::remove(job.input, ec);
		if (!job.output.empty()) fs::remove(job.output, ec);
	}

	static std::string lastErrorText() {
		return posixErrorText(std::error_code(static_cast<int>(::GetLastError()),
			std::system_category()));
	}

	static bool prepareProcess(const JobLaunch& launch, const CronEntry& entry, RunningJob& job,
	                           HiddenProcess& process) {
		JobShell shell;
		if (!chooseShell(launch, entry, shell)) return false;

		const CrondEnvironment variables = jobEnvironment(launch, entry, shell);
		if (!chooseDirectory(launch, variables, process.directory)) return false;

		chooseDestination(launch, entry, variables, job);
		process.application = utf8ToWide(shell.program);
		process.command_line = windowsCommandLine(shell.argv);
		process.environment = environmentBlock(launch.exec, variables);
		process.extra_flags = CREATE_NEW_PROCESS_GROUP;
		const bool opened = openInput(launch, entry, job, process)
			&& openOutput(launch, job, process);
		if (opened) return true;

		crondLog(launch.log, kCrondLogError, "user " + launch.user
			+ ": can't create the job's temporary files: " + lastErrorText());
		return false;
	}

	void startCronJob(const JobLaunch& launch, const CronEntry& entry,
	                  std::vector<RunningJob>& jobs) {
		RunningJob job;
		job.user = launch.user;
		job.text = entry.text;

		HiddenProcess process;
		const bool prepared = prepareProcess(launch, entry, job, process);
		job.process = prepared ? startHiddenProcess(process) : nullptr;
		const std::string failure = lastErrorText();
		closeJobFiles(process);
		if (job.process == nullptr) {
			if (prepared) {
				crondLog(launch.log, kCrondLogError, "user " + launch.user + ": can't start job: "
					+ failure);
			}

			removeJobFiles(job);
			return;
		}

		job.pid = ::GetProcessId(job.process);
		crondLog(launch.log, kCrondLogNotice, "USER " + job.user + " pid "
			+ std::to_string(job.pid) + " cmd " + job.text);
		jobs.push_back(std::move(job));
	}

	static int exitStatusOf(HANDLE process) {
		DWORD code = 0;
		::GetExitCodeProcess(process, &code);
		return shellStatusFromExitCode(code);
	}

	static std::string takeOutput(const RunningJob& job) {
		std::string output;
		std::error_code ec;
		readWholeFile(job.output, output, ec);
		removeJobFiles(job);
		return output;
	}

	static void finishJob(const CrondLog& log, RunningJob& job) {
		const int status = exitStatusOf(job.process);
		::CloseHandle(job.process);
		const std::string output = takeOutput(job);
		const std::string who = "USER " + job.user + " pid " + std::to_string(job.pid);
		crondLog(log, kCrondLogInfo, who + " exited with status " + std::to_string(status));
		if (output.empty() || job.discard) return;

		appendCrondOutput(log, job.destination, "output of " + who + " cmd " + job.text + ":",
			output);
	}

	void collectFinishedJobs(const CrondLog& log, std::vector<RunningJob>& jobs) {
		for (auto job = jobs.begin(); job != jobs.end();) {
			if (::WaitForSingleObject(job->process, 0) != WAIT_OBJECT_0) {
				++job;
				continue;
			}

			finishJob(log, *job);
			job = jobs.erase(job);
		}
	}

	void waitForJobs(const CrondLog& log, std::vector<RunningJob>& jobs) {
		collectFinishedJobs(log, jobs);
		while (!jobs.empty() && sleepUnlessCtrlC(cron_detail::kJobPollMs)) {
			collectFinishedJobs(log, jobs);
		}
	}

	void releaseJobs(std::vector<RunningJob>& jobs) {
		for (const RunningJob& job : jobs) ::CloseHandle(job.process);
		jobs.clear();
	}

}  // namespace wbsh

#endif /* _WIN32 */

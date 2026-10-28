/**
 * @file gitprompt.cpp
 * @brief The prompt's git branch and state; `git status` runs directly and in the background.
 */

#include "gitprompt.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif /* _WIN32 */

#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "executor.h"
#include "lineedit.h"
#include "numparse.h"
#include "pathconv.h"

namespace wbsh {

	namespace fs = std::filesystem;

	namespace gitprompt_detail {

		static const long long kDefaultWaitMs = 50;
		static const char* const kStatusArgs =
			"--no-optional-locks status --porcelain --ignore-submodules=dirty";

		struct GitCommand {
			std::wstring exe;
			std::wstring args;
			std::wstring cwd;
		};

		// One line of git's output; false stops reading and ends git.
		using LineHandler = std::function<bool(const std::string& line)>;

		static void stripLineEnding(std::string& line) {
			while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
		}

#ifdef _WIN32
		static HANDLE openNul() {
			SECURITY_ATTRIBUTES inheritable{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
			return ::CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
				FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr);
		}

		// git shares the shell's console -- a console of its own would cost
		// a conhost start each time -- and only gets a hidden one when the
		// shell has none, so nothing it starts can open a window.
		static DWORD creationFlags() {
			const bool has_console = ::GetConsoleCP() != 0;
			return has_console ? 0 : CREATE_NO_WINDOW;
		}

		// Only these handles are inherited, so nothing else the shell holds
		// open leaks into git.
		static HANDLE startGit(const GitCommand& command, HANDLE output, HANDLE nul) {
			SIZE_T size = 0;
			::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
			std::vector<char> storage(size);
			auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
			if (!::InitializeProcThreadAttributeList(attributes, 1, 0, &size)) return nullptr;

			HANDLE inherited[2] = { output, nul };
			::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
				inherited, sizeof(inherited), nullptr, nullptr);

			STARTUPINFOEXW startup{};
			startup.StartupInfo.cb = sizeof(STARTUPINFOEXW);
			startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
			startup.StartupInfo.hStdInput = nul;
			startup.StartupInfo.hStdOutput = output;
			startup.StartupInfo.hStdError = nul;
			startup.lpAttributeList = attributes;

			std::wstring line = L"\"" + command.exe + L"\" " + command.args;
			PROCESS_INFORMATION process{};
			const BOOL started = ::CreateProcessW(command.exe.c_str(), line.data(), nullptr,
				nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT | creationFlags(), nullptr,
				command.cwd.c_str(), &startup.StartupInfo, &process);
			::DeleteProcThreadAttributeList(attributes);
			if (!started) return nullptr;

			::CloseHandle(process.hThread);
			return process.hProcess;
		}

		static bool deliverLines(std::string& pending, const LineHandler& on_line) {
			std::size_t newline = pending.find('\n');
			while (newline != std::string::npos) {
				std::string line = pending.substr(0, newline);
				pending.erase(0, newline + 1);
				stripLineEnding(line);
				if (!on_line(line)) return false;

				newline = pending.find('\n');
			}

			return true;
		}

		static bool readLines(HANDLE output, const LineHandler& on_line) {
			std::string pending;
			char chunk[4096];
			DWORD got = 0;
			while (::ReadFile(output, chunk, sizeof(chunk), &got, nullptr) && got != 0) {
				pending.append(chunk, got);
				if (!deliverLines(pending, on_line)) return false;
			}

			if (!pending.empty()) on_line(pending);
			return true;
		}

		static void runGit(const GitCommand& command, const LineHandler& on_line) {
			SECURITY_ATTRIBUTES inheritable{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
			HANDLE read = nullptr;
			HANDLE write = nullptr;
			if (!::CreatePipe(&read, &write, &inheritable, 0)) return;

			::SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
			const HANDLE nul = openNul();
			const HANDLE process = startGit(command, write, nul);
			::CloseHandle(write);
			if (nul != INVALID_HANDLE_VALUE) ::CloseHandle(nul);

			if (process != nullptr) {
				if (!readLines(read, on_line)) ::TerminateProcess(process, 1);
				::WaitForSingleObject(process, INFINITE);
				::CloseHandle(process);
			}

			::CloseHandle(read);
		}

		static std::wstring currentDirectory() {
			std::error_code ec;
			return fs::current_path(ec).wstring();
		}
#else
		static void runGit(const GitCommand&, const LineHandler&) {}

		static std::wstring currentDirectory() {
			return {};
		}
#endif /* _WIN32 */

		// Git for Windows puts launchers in cmd\ and bin\ that only start
		// mingw64\bin\git.exe; starting that one directly saves a process.
		static fs::path realGitBehind(const fs::path& launcher) {
			const fs::path dir = launcher.parent_path();
			std::wstring name = dir.filename().wstring();
			for (wchar_t& letter : name) letter = static_cast<wchar_t>(std::towlower(letter));
			if (name != L"cmd" && name != L"bin") return launcher;

			std::error_code ec;
			for (const wchar_t* flavour : { L"mingw64", L"clangarm64", L"mingw32" }) {
				const fs::path real = dir.parent_path() / flavour / L"bin" / L"git.exe";
				if (fs::is_regular_file(real, ec)) return real;
			}

			return launcher;
		}

		static std::wstring findGitOnPath(const std::string& path, const PathConv& conv) {
			for (const std::string& dir : splitPathList(path)) {
				if (dir.empty()) continue;

				std::error_code ec;
				const fs::path candidate = utf8ToPath(conv.toWin32(dir)) / "git.exe";
				if (fs::is_regular_file(candidate, ec)) return realGitBehind(candidate).wstring();
			}

			return {};
		}

		// Only a git.exe: a .cmd or .bat wrapper would need cmd.exe, which
		// is what this avoids, and the dirty state is then left out. The
		// lookup is kept until PATH changes.
		static std::wstring gitExecutable(const Environment& env, const PathConv& conv) {
			static std::string cached_path;
			static std::wstring cached_git;
			const std::string path = env.get("PATH");
			if (path != cached_path || cached_git.empty()) {
				cached_git = findGitOnPath(path, conv);
				cached_path = path;
			}

			return cached_git;
		}

		static std::string firstLineOf(const GitCommand& command) {
			std::string first;
			runGit(command, [&first](const std::string& line) {
				first = line;
				return false;
			});
			return first;
		}

		// A reftable repository's HEAD file is only a stub pointing at
		// refs/heads/.invalid; the real HEAD has to come from git.
		static std::string branchFromGit(const std::wstring& git) {
			GitCommand command{ git, L"symbolic-ref --short -q HEAD", currentDirectory() };
			if (command.exe.empty()) return {};

			const std::string branch = firstLineOf(command);
			if (!branch.empty()) return branch;

			command.args = L"rev-parse --short HEAD";
			const std::string commit = firstLineOf(command);
			return commit.empty() ? std::string() : commit + " (detached)";
		}

		static std::string branchFromHeadLine(std::string line) {
			const std::string ref_prefix = "ref: ";
			if (line.compare(0, ref_prefix.size(), ref_prefix) != 0) {
				if (line.size() >= 7) line.resize(7);
				return line + " (detached)";
			}

			const std::string ref = line.substr(ref_prefix.size());
			const std::string heads = "refs/heads/";
			if (ref.compare(0, heads.size(), heads) == 0) return ref.substr(heads.size());

			return ref;
		}

		static std::string branchOf(const fs::path& git_dir, const std::wstring& git) {
			std::ifstream head(git_dir / "HEAD");
			std::string line;
			if (!std::getline(head, line)) return {};

			stripLineEnding(line);
			const std::string branch = branchFromHeadLine(line);
			return branch == ".invalid" ? branchFromGit(git) : branch;
		}

		static std::string operationState(const fs::path& git_dir) {
			std::error_code ec;
			if (fs::exists(git_dir / "MERGE_HEAD", ec))        return "merging";
			if (fs::exists(git_dir / "rebase-merge", ec) ||
			    fs::exists(git_dir / "rebase-apply", ec))      return "rebasing";
			if (fs::exists(git_dir / "CHERRY_PICK_HEAD", ec))  return "cherry-picking";
			if (fs::exists(git_dir / "REVERT_HEAD", ec))       return "reverting";
			if (fs::exists(git_dir / "BISECT_LOG", ec))        return "bisecting";
			return {};
		}

		static bool dirtyCheckDisabled(const Environment& env) {
			const std::string off = env.get("WBSH_GIT_NO_DIRTY");
			return !off.empty() && off != "0";
		}

		struct DirtyFlags {
			bool unstaged = false;
			bool staged = false;
			bool untracked = false;
		};

		static void classify(const std::string& line, DirtyFlags& flags) {
			if (line.size() < 2) return;
			if (line[0] == '?' && line[1] == '?') {
				flags.untracked = true;
				return;
			}

			if (line[0] != ' ') flags.staged = true;
			if (line[1] != ' ') flags.unstaged = true;
		}

		// Unstaged changes outrank everything else, so git is stopped at
		// the first one.
		static std::string dirtyStateOf(const GitCommand& command) {
			DirtyFlags flags;
			runGit(command, [&flags](const std::string& line) {
				classify(line, flags);
				return !flags.unstaged;
			});

			if (flags.unstaged)  return "unstaged";
			if (flags.staged)    return "staged";
			if (flags.untracked) return "untracked";
			return {};
		}

		struct StatusJob {
			std::string key;
			std::string git_dir;
			bool finished = false;
			bool shown_stale = false;
			bool redraw_requested = false;
			std::string shown;
			std::string result;
		};

		struct Registry {
			std::mutex lock;
			std::condition_variable finished;
			std::map<std::string, std::string> last_state;
			std::shared_ptr<StatusJob> current;
			std::function<void()> listener;
		};

		// Shared with the status threads, which may outlive a prompt.
		static std::shared_ptr<Registry> registry() {
			static const std::shared_ptr<Registry> instance = std::make_shared<Registry>();
			return instance;
		}

		using JobPtr = std::shared_ptr<StatusJob>;

		static std::function<void()> recordResult(Registry& reg, const JobPtr& job,
		                                          const std::string& result) {
			const std::lock_guard<std::mutex> guard(reg.lock);
			job->result = result;
			job->finished = true;
			reg.last_state[job->git_dir] = result;

			const bool redraw = job == reg.current && job->shown_stale && result != job->shown;
			if (!redraw) return {};

			job->redraw_requested = true;
			return reg.listener;
		}

		static void runJob(const std::shared_ptr<Registry>& reg, const JobPtr& job,
		                   const GitCommand& command) {
			const std::function<void()> listener = recordResult(*reg, job, dirtyStateOf(command));
			reg->finished.notify_all();
			if (listener) listener();
		}

		// A redraw asked for by a finished job takes that job's state
		// instead of running git again.
		static bool takeRequestedState(Registry& reg, const std::string& key, std::string& state) {
			const std::shared_ptr<StatusJob>& job = reg.current;
			if (!job || job->key != key || !job->finished || !job->redraw_requested) return false;

			job->redraw_requested = false;
			state = job->result;
			return true;
		}

		static long long waitBudgetMs(const Environment& env) {
			long long budget = kDefaultWaitMs;
			const std::string value = env.get("WBSH_GIT_STATUS_WAIT");
			if (!value.empty() && parseLL(value, budget) && budget >= 0) return budget;

			return kDefaultWaitMs;
		}

		static JobPtr startJob(const std::shared_ptr<Registry>& reg, const std::string& key,
		                       const std::string& git_dir, const GitCommand& command) {
			auto job = std::make_shared<StatusJob>();
			job->key = key;
			job->git_dir = git_dir;
			reg->current = job;
			std::thread(runJob, reg, job, command).detach();
			return job;
		}

		// A run still going for the same place is waited for again rather
		// than started twice.
		static JobPtr jobFor(const std::shared_ptr<Registry>& reg, const std::string& key,
		                     const std::string& git_dir, const GitCommand& command) {
			const JobPtr& running = reg->current;
			if (running && running->key == key && !running->finished) return running;

			return startJob(reg, key, git_dir, command);
		}

		static std::string dirtyState(const Environment& env, const std::wstring& git,
		                              const fs::path& git_dir, bool can_redraw) {
			const GitCommand command{ git, utf8ToWide(kStatusArgs), currentDirectory() };
			const std::string dir = pathToUtf8(git_dir);
			const std::string key = dir + "|" + wideToUtf8(command.cwd);
			const long long budget = waitBudgetMs(env);
			const std::shared_ptr<Registry> reg = registry();

			std::unique_lock<std::mutex> guard(reg->lock);
			std::string state;
			if (takeRequestedState(*reg, key, state)) return state;

			const JobPtr job = jobFor(reg, key, dir, command);
			const auto done = [&job] { return job->finished; };
			if (!can_redraw) reg->finished.wait(guard, done);

			const auto wait = std::chrono::milliseconds(budget);
			if (reg->finished.wait_for(guard, wait, done)) return job->result;

			job->shown = reg->last_state[dir];
			job->shown_stale = true;
			return job->shown;
		}

	}  // namespace gitprompt_detail

	GitInfo gitPromptInfo(const Environment& env, const PathConv& conv, bool can_redraw) {
		GitInfo info;
		const fs::path git_dir = findGitDir();
		if (git_dir.empty()) return info;

		const std::wstring git = gitprompt_detail::gitExecutable(env, conv);
		info.branch = gitprompt_detail::branchOf(git_dir, git);
		if (info.branch.empty()) return info;

		info.state = gitprompt_detail::operationState(git_dir);
		const bool check_dirty = info.state.empty() && !git.empty()
			&& !gitprompt_detail::dirtyCheckDisabled(env);
		if (check_dirty) info.state = gitprompt_detail::dirtyState(env, git, git_dir, can_redraw);

		return info;
	}

	void setGitStatusListener(std::function<void()> listener) {
		const std::shared_ptr<gitprompt_detail::Registry> reg = gitprompt_detail::registry();
		const std::lock_guard<std::mutex> guard(reg->lock);
		reg->listener = std::move(listener);
	}

}  // namespace wbsh

/**
 * @file executor.cpp
 * @brief AST walkers, redirections, external process spawning, and the
 *        job / history / scope bookkeeping behind them.
 */

#include "executor.h"

#include "interrupt.h"
#include "utils.h"

#ifdef _WIN32
// winsock2.h must precede windows.h or the legacy winsock symbols leak in.
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>

#  include <fcntl.h>
#  include <io.h>
#  pragma comment(lib, "ws2_32.lib")
#endif /* _WIN32 */

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unordered_set>
#include <utility>
#include <vector>

#include "fnmatch.h"
#include "lexer.h"
#include "numparse.h"
#include "parser.h"
#include "regexutil.h"

namespace wbsh {

	using NameValueList = std::vector<std::pair<std::string, std::string>>;

	static const int kStatusCommandNotFound = 127;
	static const long long kMillisPerMinute = 60000;
	static const std::size_t kShebangProbeBytes = 256;
	static const std::size_t kMaxHistory = 5000;

	static const std::string kDevFdPrefix  = "/dev/fd/";
	static const std::string kDevTcpPrefix = "/dev/tcp/";
	static const std::string kDevUdpPrefix = "/dev/udp/";

	static const char* const kMsysPathMarkers[] = {
		"\\git\\usr\\bin\\",
		"\\git\\mingw32\\bin\\",
		"\\git\\mingw64\\bin\\",
		"\\msys64\\",
		"\\msys2\\",
		"\\cygwin\\",
		"\\cygwin64\\",
	};

	static const char* const kExecutableExtensions[] = { ".exe", ".cmd", ".bat", "" };

	// On-disk history format:
	//
	//   #!wbsh-history-v2
	//   <status>\t<command>
	//   <status>\t<command>
	//   ...
	//
	// Legacy files (no header, one command per line) still load: each line
	// becomes an entry with status 0 ("treat as OK"). This keeps existing
	// ~/.wbsh_history files working across the upgrade.
	static const char* const kHistoryV2Header = "#!wbsh-history-v2";

	namespace executor_detail {
#ifdef _WIN32
		struct StdioHandles {
			HANDLE in  = INVALID_HANDLE_VALUE;
			HANDLE out = INVALID_HANDLE_VALUE;
			HANDLE err = INVALID_HANDLE_VALUE;
		};

		struct SpawnCommand {
			std::wstring exe;
			std::wstring cmdline;
			std::wstring envblock;
		};
#endif

		enum class ExtensionClass {
			Unknown,
			Native,
			Script,
		};
	}

	using executor_detail::ExtensionClass;
#ifdef _WIN32
	using executor_detail::SpawnCommand;
	using executor_detail::StdioHandles;
#endif

	static void reportExpanderError(Expander& expander) {
		std::fprintf(stderr, "wbsh: %s\n", expander.takeError().c_str());
	}

	static void reportPathError(const std::string& path) {
		std::fprintf(stderr, "wbsh: %s: %s\n", path.c_str(), std::strerror(errno));
	}

	static void reportCommandNotFound(const std::string& name) {
		std::fprintf(stderr, "wbsh: %s: command not found\n", name.c_str());
	}

	static std::string asciiLower(std::string text) {
		for (char& c : text) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}

		return text;
	}

	static bool hasPrefixAndRest(const std::string& text, const std::string& prefix) {
		return text.size() > prefix.size()
			&& text.compare(0, prefix.size(), prefix) == 0;
	}

	static void appendFields(std::vector<std::string>& into, std::vector<std::string>& fields) {
		for (auto& field : fields) into.push_back(std::move(field));
	}

	static void removeFile(const std::string& path) {
		_wremove(utf8ToWide(path).c_str());
	}

	static std::string makeTempFile() {
#ifdef _WIN32
		wchar_t dir[MAX_PATH];
		const DWORD dir_length = ::GetTempPathW(MAX_PATH, dir);
		if (dir_length == 0 || dir_length > MAX_PATH) return {};

		wchar_t path[MAX_PATH];
		if (::GetTempFileNameW(dir, L"wbsh", 0, path) == 0) return {};
		return wideToUtf8(path);
#else
		char name_template[] = "/tmp/wbshXXXXXX";
		const int fd = mkstemp(name_template);
		if (fd < 0) return {};

		::close(fd);
		return std::string(name_template);
#endif
	}

	static std::string readAllText(const std::string& path) {
		std::ifstream file(utf8ToPath(path), std::ios::binary);
		std::stringstream buffer;
		buffer << file.rdbuf();
		return buffer.str();
	}

	static void writeAllText(const std::string& path, const std::string& body) {
		std::ofstream file(utf8ToPath(path), std::ios::binary | std::ios::trunc);
		file.write(body.data(), static_cast<std::streamsize>(body.size()));
	}

#ifdef _WIN32
	static bool argNeedsQuotes(const std::wstring& arg) {
		for (wchar_t c : arg) {
			if (c == L' ' || c == L'\t' || c == L'\n' || c == L'\v' || c == L'"') return true;
		}

		return false;
	}

	static std::wstring quoteArg(const std::wstring& arg) {
		if (arg.empty()) return L"\"\"";
		if (!argNeedsQuotes(arg)) return arg;

		std::wstring out = L"\"";
		std::size_t backslashes = 0;
		for (wchar_t c : arg) {
			if (c == L'\\') {
				++backslashes;
				continue;
			}

			if (c == L'"') {
				out.append(backslashes * 2 + 1, L'\\');
			} else {
				out.append(backslashes, L'\\');
			}

			out.push_back(c);
			backslashes = 0;
		}

		out.append(backslashes * 2, L'\\');
		out.push_back(L'"');
		return out;
	}

	static std::wstring buildCommandLine(const std::vector<std::string>& argv) {
		std::wstring cmdline;
		for (std::size_t i = 0; i < argv.size(); ++i) {
			if (i != 0) cmdline.push_back(L' ');
			cmdline += quoteArg(utf8ToWide(argv[i]));
		}

		return cmdline;
	}

	static std::string lowerExtension(const std::string& path) {
		const std::size_t dot = path.find_last_of('.');
		if (dot == std::string::npos) return {};
		return asciiLower(path.substr(dot + 1));
	}

	static bool isBatchFile(const std::string& path) {
		const std::string ext = lowerExtension(path);
		return ext == "cmd" || ext == "bat";
	}

	static std::wstring cmdExePath() {
		wchar_t system_dir[MAX_PATH];
		const UINT length = ::GetSystemDirectoryW(system_dir, MAX_PATH);
		if (length == 0 || length >= MAX_PATH) return L"cmd.exe";
		return std::wstring(system_dir, length) + L"\\cmd.exe";
	}

	static std::wstring wrapWithCmdExe(const std::wstring& cmdline) {
		std::wstring cmd = cmdExePath();
		if (cmd.find(L' ') != std::wstring::npos) cmd = L"\"" + cmd + L"\"";
		return cmd + L" /d /s /c \"" + cmdline + L"\"";
	}

	static std::vector<std::wstring> envEntries(const Environment& env, bool include_unexported) {
		std::vector<std::wstring> entries;
		for (const auto& var : env.vars()) {
			if (!include_unexported && !env.isExported(var.first)) continue;
			entries.push_back(utf8ToWide(var.first + "=" + var.second));
		}

		return entries;
	}

	static bool startsWith(const std::wstring& text, const std::wstring& prefix) {
		return text.size() >= prefix.size()
			&& std::equal(prefix.begin(), prefix.end(), text.begin());
	}

	static void overrideEnvEntry(std::vector<std::wstring>& entries,
	                             const std::pair<std::string, std::string>& assignment) {
		const std::wstring prefix = utf8ToWide(assignment.first) + L"=";
		std::wstring entry = prefix + utf8ToWide(assignment.second);
		for (auto& existing : entries) {
			if (!startsWith(existing, prefix)) continue;
			existing = std::move(entry);
			return;
		}

		entries.push_back(std::move(entry));
	}

	static bool envEntryLess(const std::wstring& a, const std::wstring& b) {
		const std::size_t common = a.size() < b.size() ? a.size() : b.size();
		for (std::size_t i = 0; i < common; ++i) {
			const wchar_t ca = static_cast<wchar_t>(towlower(a[i]));
			const wchar_t cb = static_cast<wchar_t>(towlower(b[i]));
			if (ca != cb) return ca < cb;
		}

		return a.size() < b.size();
	}

	static std::wstring buildEnvBlock(const Environment& env,
	                                  const NameValueList& overrides,
	                                  bool include_unexported = false) {
		std::vector<std::wstring> entries = envEntries(env, include_unexported);
		for (const auto& assignment : overrides) overrideEnvEntry(entries, assignment);
		std::sort(entries.begin(), entries.end(), envEntryLess);

		std::wstring block;
		for (const auto& entry : entries) {
			block += entry;
			block.push_back(L'\0');
		}

		block.push_back(L'\0');
		return block;
	}

	static std::string lastErrorString() {
		const DWORD error = ::GetLastError();
		if (error == 0) return {};

		LPSTR buffer = nullptr;
		const DWORD length = ::FormatMessageA(
			FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
			| FORMAT_MESSAGE_IGNORE_INSERTS,
			nullptr, error, 0, reinterpret_cast<LPSTR>(&buffer), 0, nullptr);

		std::string message;
		if (buffer != nullptr && length != 0) message.assign(buffer, length);
		if (buffer != nullptr) ::LocalFree(buffer);

		while (!message.empty()
		       && (message.back() == '\n' || message.back() == '\r' || message.back() == ' ')) {
			message.pop_back();
		}

		return message;
	}

	static std::string getSelfExecutablePath() {
		wchar_t buffer[MAX_PATH];
		const DWORD length = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
		if (length == 0 || length >= MAX_PATH) return "wbsh.exe";

		const int utf8_length = ::WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length),
			nullptr, 0, nullptr, nullptr);
		std::string out(static_cast<std::size_t>(utf8_length), '\0');
		::WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length),
			out.data(), utf8_length, nullptr, nullptr);
		return out;
	}

	static bool isMsysBinary(const std::string& path) {
		const std::string lower = asciiLower(path);
		for (const char* marker : kMsysPathMarkers) {
			if (lower.find(marker) != std::string::npos) return true;
		}

		return false;
	}

	static std::string effectiveEnvValue(const NameValueList& overrides,
	                                     const Environment& env,
	                                     const char* name) {
		for (const auto& assignment : overrides) {
			if (assignment.first == name) return assignment.second;
		}

		return env.get(name);
	}

	static bool noPathConvSet(const NameValueList& overrides, const Environment& env) {
		return !effectiveEnvValue(overrides, env, "WBSH_NO_PATHCONV").empty()
			|| !effectiveEnvValue(overrides, env, "MSYS_NO_PATHCONV").empty();
	}

	static HANDLE stdioHandle(int fd) {
		return reinterpret_cast<HANDLE>(_get_osfhandle(fd));
	}

	static StdioHandles currentStdio() {
		StdioHandles stdio;
		stdio.in  = stdioHandle(0);
		stdio.out = stdioHandle(1);
		stdio.err = stdioHandle(2);
		return stdio;
	}

	static int waitProcessAndClose(HANDLE process) {
		::WaitForSingleObject(process, INFINITE);
		DWORD exit_code = 0;
		::GetExitCodeProcess(process, &exit_code);
		::CloseHandle(process);
		return static_cast<int>(exit_code);
	}

	static LPPROC_THREAD_ATTRIBUTE_LIST allocAttrList() {
		SIZE_T attr_size = 0;
		::InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
		auto attr_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
			::HeapAlloc(::GetProcessHeap(), 0, attr_size));
		if (attr_list == nullptr) return nullptr;

		if (::InitializeProcThreadAttributeList(attr_list, 1, 0, &attr_size) == 0) {
			::HeapFree(::GetProcessHeap(), 0, attr_list);
			return nullptr;
		}

		return attr_list;
	}

	static void freeAttrList(LPPROC_THREAD_ATTRIBUTE_LIST attr_list) {
		::DeleteProcThreadAttributeList(attr_list);
		::HeapFree(::GetProcessHeap(), 0, attr_list);
	}

	static void addInheritHandle(HANDLE inherits[3], DWORD& count, HANDLE handle) {
		if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return;
		for (DWORD i = 0; i < count; ++i) {
			if (inherits[i] == handle) return;
		}

		inherits[count++] = handle;
		::SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
	}

	static DWORD collectInheritHandles(HANDLE inherits[3], const StdioHandles& stdio) {
		DWORD count = 0;
		addInheritHandle(inherits, count, stdio.in);
		addInheritHandle(inherits, count, stdio.out);
		addInheritHandle(inherits, count, stdio.err);
		return count;
	}

	static bool attachInheritList(LPPROC_THREAD_ATTRIBUTE_LIST attr_list,
	                              HANDLE inherits[3], DWORD count) {
		return ::UpdateProcThreadAttribute(attr_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
			inherits, count * sizeof(HANDLE), nullptr, nullptr) != 0;
	}

	static STARTUPINFOEXW makeStartupInfo(const StdioHandles& stdio,
	                                      LPPROC_THREAD_ATTRIBUTE_LIST attr_list) {
		STARTUPINFOEXW info{};
		info.StartupInfo.cb = sizeof(STARTUPINFOEXW);
		info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		info.StartupInfo.hStdInput  = stdio.in;
		info.StartupInfo.hStdOutput = stdio.out;
		info.StartupInfo.hStdError  = stdio.err;
		info.lpAttributeList = attr_list;
		return info;
	}

	// The inherits[] buffer is referenced by attr_list until
	// DeleteProcThreadAttributeList runs, so it must live in this frame
	// (not in a helper) — per the Win32 attribute-list ownership rules.
	static HANDLE spawnWithHandles(SpawnCommand& command, const StdioHandles& stdio) {
		LPPROC_THREAD_ATTRIBUTE_LIST attr_list = allocAttrList();
		if (attr_list == nullptr) return INVALID_HANDLE_VALUE;

		HANDLE inherits[3];
		const DWORD count = collectInheritHandles(inherits, stdio);
		if (!attachInheritList(attr_list, inherits, count)) {
			freeAttrList(attr_list);
			return INVALID_HANDLE_VALUE;
		}

		STARTUPINFOEXW info = makeStartupInfo(stdio, attr_list);
		PROCESS_INFORMATION process{};
		const BOOL created = ::CreateProcessW(command.exe.c_str(), command.cmdline.data(),
			nullptr, nullptr, TRUE,
			EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
			command.envblock.data(), nullptr, &info.StartupInfo, &process);
		freeAttrList(attr_list);

		if (created == 0) {
			std::fprintf(stderr, "wbsh: CreateProcess failed: %s\n", lastErrorString().c_str());
			return INVALID_HANDLE_VALUE;
		}

		::CloseHandle(process.hThread);
		return process.hProcess;
	}

	static SpawnCommand makeSpawnCommand(const std::vector<std::string>& argv,
	                                     const std::string& exec_path,
	                                     std::wstring envblock) {
		SpawnCommand command;
		command.exe      = utf8ToWide(exec_path);
		command.cmdline  = buildCommandLine(argv);
		command.envblock = std::move(envblock);
		if (isBatchFile(exec_path)) {
			command.cmdline = wrapWithCmdExe(command.cmdline);
			command.exe     = cmdExePath();
		}

		return command;
	}
#endif  // _WIN32

	Executor::Executor(Environment& env)
		: env_(env), expander_(env, this) {
		registerCoreBuiltins(*this);
		registerCoreutils(*this);
		loadUtils(*this);
	}

	int Executor::execute(const Node& root) {
		return execNode(root);
	}

	int Executor::execNode(const Node& node) {
		if (node.loc.line > 0) env_.setCurrentLineno(static_cast<int>(node.loc.line));

		switch (node.kind) {
		case Node::Kind::List:          return execList(static_cast<const List&>(node));
		case Node::Kind::AndOr:         return execAndOr(static_cast<const AndOr&>(node));
		case Node::Kind::Pipeline:      return execPipeline(static_cast<const Pipeline&>(node));
		case Node::Kind::SimpleCommand:
			return execSimpleCommand(static_cast<const SimpleCommand&>(node));
		case Node::Kind::BraceGroup:    return execBraceGroup(static_cast<const BraceGroup&>(node));
		case Node::Kind::Subshell:      return execSubshell(static_cast<const Subshell&>(node));
		case Node::Kind::IfClause:      return execIf(static_cast<const IfClause&>(node));
		case Node::Kind::WhileClause:   return execWhile(static_cast<const WhileClause&>(node));
		case Node::Kind::ForClause:     return execFor(static_cast<const ForClause&>(node));
		case Node::Kind::CaseClause:    return execCase(static_cast<const CaseClause&>(node));
		case Node::Kind::FunctionDef:
			return execFunctionDef(static_cast<const FunctionDef&>(node));
		case Node::Kind::DBracket:      return execDBracket(static_cast<const DBracketCond&>(node));
		case Node::Kind::ArithCommand:
			return execArithCommand(static_cast<const ArithCommand&>(node));
		}

		return 0;
	}

	// A node parsed from a REPL line carries no source pointer of its own;
	// the executor's copy of the source stands in for it then.
	static std::string nodeSourceSlice(const Node& node, const std::string& fallback) {
		const std::string* source = node.source_text;
		if (source == nullptr && !fallback.empty()) source = &fallback;
		if (source == nullptr) return {};
		if (node.src_end <= node.src_start || node.src_end > source->size()) return {};
		return source->substr(node.src_start, node.src_end - node.src_start);
	}

	static int negateStatus(int status) {
		return (status == 0) ? 1 : 0;
	}

	bool Executor::applyRedirectionsOrUndo(const std::vector<Redirection>& redirs,
	                                       RedirState& state) {
		if (applyRedirections(redirs, state)) return true;
		undoRedirections(state);
		return false;
	}

	void Executor::runBody(const Node* body, int& status) {
		const int result = (body != nullptr) ? execNode(*body) : 0;
		if (!flowPending()) status = result;
	}

#ifdef _WIN32
	int Executor::launchBackgroundCommand(const Node& cmd) {
		const StdioHandles stdio = currentStdio();
		const HANDLE process = launchPipelineElement(cmd, stdio.in, stdio.out, stdio.err);
		if (process == INVALID_HANDLE_VALUE) return 1;

		const long long pid = static_cast<long long>(::GetProcessId(process));
		const int job_id = registerJob(process, pid, nodeSourceSlice(cmd, std::string()));
		env_.setLastBgPid(pid);
		std::fprintf(stderr, "[%d] %lld\n", job_id, pid);
		return 0;
	}
#endif

	int Executor::execList(const List& list) {
		int status = 0;
		for (const auto& item : list.items) {
#ifdef _WIN32
			if (item.background) status = launchBackgroundCommand(*item.command);
			else                 status = execNode(*item.command);
#else
			status = execNode(*item.command);
#endif
			if (flowPending()) return status;
			setLastStatus(status);

			const bool errexit_fires = env_.errexit() && status != 0
				&& errexit_suppress_ == 0 && !item.background;
			if (!errexit_fires) continue;

			raiseExit(status);
			return status;
		}

		return status;
	}

	int Executor::execAndOr(const AndOr& and_or) {
		pushErrexitSuppress();
		const int left = execNode(*and_or.left);
		popErrexitSuppress();
		if (flowPending()) return left;
		setLastStatus(left);

		const bool short_circuit = (and_or.op == AndOr::Op::AndIf) ? (left != 0) : (left == 0);
		if (short_circuit) return left;

		const int right = execNode(*and_or.right);
		if (flowPending()) return right;
		setLastStatus(right);
		return right;
	}

	namespace executor_detail {
		struct BangGuard {
			Executor* exec;
			bool active;

			BangGuard(Executor* owner, bool bang) : exec(owner), active(bang) {
				if (active) exec->pushErrexitSuppress();
			}

			~BangGuard() {
				if (active) exec->popErrexitSuppress();
			}
		};

		struct PipelineTimeGuard {
			bool active;
			std::chrono::steady_clock::time_point start;

			~PipelineTimeGuard() {
				if (!active) return;

				const auto elapsed = std::chrono::steady_clock::now() - start;
				const auto ms =
					std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
				const long long minutes = ms / kMillisPerMinute;
				const double seconds = (ms % kMillisPerMinute) / 1000.0;
				std::fprintf(stderr, "\nreal\t%lldm%.3fs\n", minutes, seconds);
			}
		};
	}

#ifdef _WIN32
	static void closeCreatedPipes(std::vector<HANDLE>& pipe_r, std::vector<HANDLE>& pipe_w,
	                              std::size_t created) {
		for (std::size_t i = 0; i < created; ++i) {
			::CloseHandle(pipe_r[i]);
			::CloseHandle(pipe_w[i]);
		}

		pipe_r.clear();
		pipe_w.clear();
	}

	static void closeValidHandles(const std::vector<HANDLE>& handles) {
		for (HANDLE handle : handles) {
			if (handle != INVALID_HANDLE_VALUE) ::CloseHandle(handle);
		}
	}

	static bool createPipelinePipes(std::size_t count,
	                                std::vector<HANDLE>& pipe_r,
	                                std::vector<HANDLE>& pipe_w) {
		SECURITY_ATTRIBUTES attributes{};
		attributes.nLength = sizeof(attributes);
		attributes.bInheritHandle = TRUE;

		pipe_r.assign(count - 1, INVALID_HANDLE_VALUE);
		pipe_w.assign(count - 1, INVALID_HANDLE_VALUE);
		for (std::size_t i = 0; i + 1 < count; ++i) {
			if (::CreatePipe(&pipe_r[i], &pipe_w[i], &attributes, 0) != 0) continue;

			std::fprintf(stderr, "wbsh: pipe creation failed: %s\n", lastErrorString().c_str());
			closeCreatedPipes(pipe_r, pipe_w, i);
			return false;
		}

		return true;
	}

	static int waitPipelineProcessesAndStatus(const std::vector<HANDLE>& processes,
	                                          bool pipefail) {
		std::vector<int> statuses(processes.size(), 0);
		for (std::size_t i = 0; i < processes.size(); ++i) {
			statuses[i] = waitProcessAndClose(processes[i]);
		}

		int last = statuses.empty() ? 0 : statuses.back();
		if (!pipefail) return last;

		for (auto it = statuses.rbegin(); it != statuses.rend(); ++it) {
			if (*it == 0) continue;
			last = *it;
			break;
		}

		return last;
	}

	static StdioHandles pipelineStageStdio(const Pipeline& pipeline, std::size_t index,
	                                       const std::vector<HANDLE>& pipe_r,
	                                       const std::vector<HANDLE>& pipe_w,
	                                       const StdioHandles& outer) {
		const std::size_t count = pipeline.commands.size();
		const bool first = index == 0;
		const bool last  = index + 1 == count;

		StdioHandles stdio;
		stdio.in  = first ? outer.in  : pipe_r[index - 1];
		stdio.out = last  ? outer.out : pipe_w[index];
		stdio.err = outer.err;

		const bool merge_stderr = !last
			&& index < pipeline.stderr_to_stdout.size()
			&& pipeline.stderr_to_stdout[index];
		if (merge_stderr) stdio.err = stdio.out;
		return stdio;
	}

	int Executor::execPipelineMultiCmd(const Pipeline& pipeline) {
		const std::size_t count = pipeline.commands.size();

		std::vector<HANDLE> pipe_r;
		std::vector<HANDLE> pipe_w;
		if (!createPipelinePipes(count, pipe_r, pipe_w)) return 1;

		std::fflush(stdout);
		std::fflush(stderr);
		const StdioHandles outer = currentStdio();

		std::vector<HANDLE> processes;
		processes.reserve(count);
		for (std::size_t i = 0; i < count; ++i) {
			const StdioHandles stdio = pipelineStageStdio(pipeline, i, pipe_r, pipe_w, outer);
			const HANDLE process = launchPipelineElement(*pipeline.commands[i],
				stdio.in, stdio.out, stdio.err);
			if (process == INVALID_HANDLE_VALUE) break;

			processes.push_back(process);
			if (flowPending()) break;
		}

		closeValidHandles(pipe_r);
		closeValidHandles(pipe_w);

		const int status = waitPipelineProcessesAndStatus(processes, env_.pipefail());
		if (processes.size() < count) return 1;
		return status;
	}
#endif  // _WIN32

	int Executor::execPipeline(const Pipeline& pipeline) {
		executor_detail::BangGuard bang_guard(this, pipeline.bang);
		executor_detail::PipelineTimeGuard time_guard{
			pipeline.timed, std::chrono::steady_clock::now() };

		if (pipeline.commands.size() == 1) {
			int status = execNode(*pipeline.commands[0]);
			if (flowPending()) return status;
			if (pipeline.bang) status = negateStatus(status);
			return status;
		}

#ifdef _WIN32
		int status = execPipelineMultiCmd(pipeline);
		if (pipeline.bang) status = negateStatus(status);
		return status;
#else
		return 1;
#endif
	}

#ifdef _WIN32
	// Unlike expandSimpleCmdArgv this does not stop on a pending flow
	// signal; the element is launched as a separate process regardless.
	static bool expandWordsForDirectLaunch(Expander& expander, const std::vector<Word>& words,
	                                       std::vector<std::string>& argv) {
		for (const auto& word : words) {
			std::vector<std::string> fields = expander.expandWord(word);
			if (expander.failed()) {
				reportExpanderError(expander);
				return false;
			}

			appendFields(argv, fields);
		}

		return true;
	}

	HANDLE Executor::tryDirectExternalLaunch(const Node& elem,
	                                         HANDLE h_in, HANDLE h_out, HANDLE h_err,
	                                         bool* tried) {
		*tried = false;
		if (elem.kind != Node::Kind::SimpleCommand) return INVALID_HANDLE_VALUE;

		const auto& command = static_cast<const SimpleCommand&>(elem);
		if (!command.redirs.empty()) return INVALID_HANDLE_VALUE;

		std::vector<std::string> argv;
		if (!expandWordsForDirectLaunch(expander_, command.words, argv)) {
			*tried = true;
			return INVALID_HANDLE_VALUE;
		}

		if (argv.empty()) return INVALID_HANDLE_VALUE;
		if (isBuiltin(argv[0]) || isFunction(argv[0]) || isAlias(argv[0])) {
			return INVALID_HANDLE_VALUE;
		}

		const std::string exec_path = findExecutable(argv[0]);
		if (exec_path.empty()) {
			reportCommandNotFound(argv[0]);
			*tried = true;
			return INVALID_HANDLE_VALUE;
		}

		if (looksLikeShellScript(exec_path)) return INVALID_HANDLE_VALUE;

		*tried = true;
		return launchExternalDirect(command, argv, exec_path, h_in, h_out, h_err);
	}

	static std::vector<std::string> unexportedNames(const Environment& env) {
		std::vector<std::string> names;
		for (const auto& var : env.vars()) {
			if (!env.isExported(var.first)) names.push_back(var.first);
		}

		std::sort(names.begin(), names.end());
		return names;
	}

	static std::string joinWithSpaces(const std::vector<std::string>& words) {
		std::string joined;
		for (std::size_t i = 0; i < words.size(); ++i) {
			if (i != 0) joined.push_back(' ');
			joined += words[i];
		}

		return joined;
	}

	NameValueList Executor::buildSelfSpawnOverrides() {
		NameValueList overrides;

		const std::string posix_path = env_.get("PATH");
		if (!posix_path.empty()) {
			overrides.emplace_back("PATH", path_conv_.pathListPosixToWin32(posix_path));
		}

		const std::string functions = serializeFunctions();
		if (!functions.empty()) overrides.emplace_back("WBSH_FUNCTIONS", functions);
		const std::string aliases = serializeAliases();
		if (!aliases.empty()) overrides.emplace_back("WBSH_ALIASES", aliases);
		const std::string arrays = serializeArrays();
		if (!arrays.empty()) overrides.emplace_back("WBSH_ARRAYS", arrays);

		const std::vector<std::string> locals = unexportedNames(env_);
		if (!locals.empty()) overrides.emplace_back("WBSH_LOCAL_NAMES", joinWithSpaces(locals));

		return overrides;
	}

	void Executor::appendHeredocBodiesToSlice(const SimpleCommand& sc, std::string& slice) {
		for (const auto& redir : sc.redirs) {
			if (redir.op != RedirOp::DLess && redir.op != RedirOp::DLessDash) continue;

			if (!slice.empty() && slice.back() != '\n') slice.push_back('\n');
			slice += redir.heredoc_body;

			std::string delimiter = expander_.expandStringValue(redir.target);
			if (expander_.failed()) {
				expander_.takeError();
				delimiter.clear();
			}

			slice += delimiter;
			slice.push_back('\n');
		}
	}

	HANDLE Executor::selfSpawnPipelineElement(const Node& elem,
	                                          HANDLE h_in, HANDLE h_out, HANDLE h_err) {
		std::string slice = nodeSourceSlice(elem, source_text_);
		if (slice.empty()) {
			std::fprintf(stderr, "wbsh: cannot extract pipeline element source\n");
			return INVALID_HANDLE_VALUE;
		}

		if (elem.kind == Node::Kind::SimpleCommand) {
			appendHeredocBodiesToSlice(static_cast<const SimpleCommand&>(elem), slice);
		}

		const std::string self = getSelfExecutablePath();
		const std::vector<std::string> argv = { self, "-r", "-c", slice };

		SpawnCommand command;
		command.exe      = utf8ToWide(self);
		command.cmdline  = buildCommandLine(argv);
		command.envblock = buildEnvBlock(env_, buildSelfSpawnOverrides(),
			/*include_unexported=*/true);
		return spawnWithHandles(command, StdioHandles{ h_in, h_out, h_err });
	}

	HANDLE Executor::launchPipelineElement(const Node& elem,
	                                       HANDLE h_in, HANDLE h_out, HANDLE h_err) {
		bool tried_direct = false;
		const HANDLE direct = tryDirectExternalLaunch(elem, h_in, h_out, h_err, &tried_direct);
		if (tried_direct) return direct;

		return selfSpawnPipelineElement(elem, h_in, h_out, h_err);
	}

	// A prefix assignment whose value fails to expand is dropped rather
	// than aborting the launch.
	static NameValueList expandPrefixAssignments(Expander& expander,
	                                             const std::vector<Assignment>& assignments) {
		NameValueList temp_env;
		for (const auto& assignment : assignments) {
			std::string value = expander.expandStringValue(assignment.value);
			if (expander.failed()) {
				expander.takeError();
				continue;
			}

			temp_env.emplace_back(assignment.name, std::move(value));
		}

		return temp_env;
	}

	HANDLE Executor::launchExternalDirect(const SimpleCommand& sc,
	                                      const std::vector<std::string>& argv,
	                                      const std::string& exec_path,
	                                      HANDLE h_in, HANDLE h_out, HANDLE h_err) {
		const NameValueList temp_env = expandPrefixAssignments(expander_, sc.assignments);
		const std::vector<std::string> child_argv = prepareExternalArgv(argv, exec_path, temp_env);
		const NameValueList overrides = prepareExternalEnvOverrides(temp_env);

		SpawnCommand command = makeSpawnCommand(child_argv, exec_path,
			buildEnvBlock(env_, overrides));
		return spawnWithHandles(command, StdioHandles{ h_in, h_out, h_err });
	}
#endif  // _WIN32

	static int defaultRedirTargetFd(RedirOp op) {
		switch (op) {
		case RedirOp::Less:
		case RedirOp::DLess:
		case RedirOp::DLessDash:
		case RedirOp::TLess:
		case RedirOp::LessGreat:
		case RedirOp::LessAnd:
			return 0;
		default:
			return 1;
		}
	}

	void Executor::saveFd(RedirState& state, int fd) const {
		const int backup = _dup(fd);
		state.saved.push_back({ fd, backup });
	}

	int Executor::dupSpecialDevFd(const std::string& path) {
		if (path == "/dev/stdin")  return _dup(0);
		if (path == "/dev/stdout") return _dup(1);
		if (path == "/dev/stderr") return _dup(2);
		if (!hasPrefixAndRest(path, kDevFdPrefix)) return -1;

		int fd = 0;
		if (!parseInt(path.substr(kDevFdPrefix.size()), fd)) return -1;
		return _dup(fd);
	}

	static bool ensureWinsockStarted() {
		static bool started = false;
		if (started) return true;

		WSADATA data;
		if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
		started = true;
		return true;
	}

	static addrinfo* resolveEndpoint(const std::string& host, const std::string& port, bool udp) {
		addrinfo hints{};
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = udp ? SOCK_DGRAM : SOCK_STREAM;

		addrinfo* result = nullptr;
		if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0) return nullptr;
		return result;
	}

	static SOCKET connectToAny(addrinfo* candidates) {
		for (addrinfo* candidate = candidates; candidate != nullptr;
		     candidate = candidate->ai_next) {
			const SOCKET sock = ::socket(candidate->ai_family, candidate->ai_socktype,
				candidate->ai_protocol);
			if (sock == INVALID_SOCKET) continue;
			if (::connect(sock, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0) {
				return sock;
			}

			::closesocket(sock);
		}

		return INVALID_SOCKET;
	}

	int Executor::openTcpUdpStream(const std::string& path) {
		const bool is_tcp = hasPrefixAndRest(path, kDevTcpPrefix);
		const bool is_udp = hasPrefixAndRest(path, kDevUdpPrefix);
		if (!is_tcp && !is_udp) return -1;

		const std::string rest = path.substr(is_udp ? kDevUdpPrefix.size() : kDevTcpPrefix.size());
		const std::size_t slash = rest.rfind('/');
		if (slash == std::string::npos) return -1;
		const std::string host = rest.substr(0, slash);
		const std::string port = rest.substr(slash + 1);

		if (!ensureWinsockStarted()) return -1;
		addrinfo* candidates = resolveEndpoint(host, port, is_udp);
		if (candidates == nullptr) return -1;

		const SOCKET sock = connectToAny(candidates);
		::freeaddrinfo(candidates);
		if (sock == INVALID_SOCKET) return -1;

		const int fd = _open_osfhandle(static_cast<intptr_t>(sock), _O_BINARY);
		if (fd < 0) {
			::closesocket(sock);
			return -1;
		}

		return fd;
	}

	int Executor::openRedirSourceFd(const std::string& path, int flags) const {
		int fd = dupSpecialDevFd(path);
		if (fd >= 0) return fd;

		fd = openTcpUdpStream(path);
		if (fd >= 0) return fd;

		const std::wstring win_path = utf8ToWide(path_conv_.toWin32(path));
		return _wopen(win_path.c_str(), flags | _O_BINARY, _S_IREAD | _S_IWRITE);
	}

	bool Executor::redirectFdFromPath(const std::string& path, int flags,
	                                  int target, RedirState& state) {
		const int fd = openRedirSourceFd(path, flags);
		if (fd < 0) {
			reportPathError(path);
			return false;
		}

		saveFd(state, target);
		_dup2(fd, target);
		_close(fd);
		return true;
	}

	bool Executor::expandRedirTarget(const Word& target, std::string& out) {
		out = expander_.expandStringValue(target);
		if (!expander_.failed()) return true;

		reportExpanderError(expander_);
		return false;
	}

	bool Executor::applyLessRedir(const Redirection& redir, int target, RedirState& state) {
		std::string path;
		if (!expandRedirTarget(redir.target, path)) return false;
		return redirectFdFromPath(path, _O_RDONLY, target, state);
	}

	bool Executor::applyTruncOrClobber(const Redirection& redir, int target, RedirState& state) {
		std::string path;
		if (!expandRedirTarget(redir.target, path)) return false;
		return redirectFdFromPath(path, _O_WRONLY | _O_CREAT | _O_TRUNC, target, state);
	}

	bool Executor::applyAppendRedir(const Redirection& redir, int target, RedirState& state) {
		std::string path;
		if (!expandRedirTarget(redir.target, path)) return false;
		return redirectFdFromPath(path, _O_WRONLY | _O_CREAT | _O_APPEND, target, state);
	}

	bool Executor::applyAmpRedir(const Redirection& redir, int extra_flags, RedirState& state) {
		std::string path;
		if (!expandRedirTarget(redir.target, path)) return false;

		const int fd = openRedirSourceFd(path, _O_WRONLY | _O_CREAT | extra_flags);
		if (fd < 0) {
			reportPathError(path);
			return false;
		}

		saveFd(state, 1);
		_dup2(fd, 1);
		saveFd(state, 2);
		_dup2(fd, 2);
		_close(fd);
		return true;
	}

	bool Executor::applyLessGreatRedir(const Redirection& redir, int target, RedirState& state) {
		std::string path;
		if (!expandRedirTarget(redir.target, path)) return false;
		return redirectFdFromPath(path, _O_RDWR | _O_CREAT, target, state);
	}

	bool Executor::applyDupRedir(const Redirection& redir, int target, RedirState& state) {
		std::string spec;
		if (!expandRedirTarget(redir.target, spec)) return false;

		if (spec == "-") {
			saveFd(state, target);
			_close(target);
			return true;
		}

		int from_fd = 0;
		if (!parseInt(spec, from_fd)) {
			std::fprintf(stderr, "wbsh: %s: bad fd\n", spec.c_str());
			return false;
		}

		saveFd(state, target);
		_dup2(from_fd, target);
		return true;
	}

	bool Executor::installRedirFromTempBody(std::string body, int target, RedirState& state) {
		std::string temp_path = makeTempFile();
		if (temp_path.empty()) return false;
		writeAllText(temp_path, body);

		const int fd = openRedirSourceFd(temp_path, _O_RDONLY);
		if (fd < 0) {
			removeFile(temp_path);
			return false;
		}

		saveFd(state, target);
		_dup2(fd, target);
		_close(fd);
		state.temps.push_back(std::move(temp_path));
		return true;
	}

	bool Executor::applyHeredocRedir(const Redirection& redir, int target, RedirState& state) {
		std::string body = expander_.expandHeredoc(redir.heredoc_body, redir.heredoc_quoted);
		if (expander_.failed()) {
			reportExpanderError(expander_);
			return false;
		}

		return installRedirFromTempBody(std::move(body), target, state);
	}

	bool Executor::applyHerestringRedir(const Redirection& redir, int target, RedirState& state) {
		std::string body;
		if (!expandRedirTarget(redir.target, body)) return false;

		body.push_back('\n');
		return installRedirFromTempBody(std::move(body), target, state);
	}

	bool Executor::applyRedirections(const std::vector<Redirection>& redirs, RedirState& state) {
		for (const auto& redir : redirs) {
			const int target = (redir.fd != -1) ? redir.fd : defaultRedirTargetFd(redir.op);
			std::fflush(stdout);
			std::fflush(stderr);

			bool ok = true;
			switch (redir.op) {
			case RedirOp::Less:        ok = applyLessRedir       (redir, target, state); break;
			case RedirOp::Great:
			case RedirOp::Clobber:     ok = applyTruncOrClobber  (redir, target, state); break;
			case RedirOp::DGreat:      ok = applyAppendRedir     (redir, target, state); break;
			case RedirOp::AmpGreat:    ok = applyAmpRedir(redir, _O_TRUNC, state);        break;
			case RedirOp::AmpDGreat:   ok = applyAmpRedir(redir, _O_APPEND, state);       break;
			case RedirOp::LessGreat:   ok = applyLessGreatRedir  (redir, target, state); break;
			case RedirOp::LessAnd:
			case RedirOp::GreatAnd:    ok = applyDupRedir        (redir, target, state); break;
			case RedirOp::DLess:
			case RedirOp::DLessDash:   ok = applyHeredocRedir    (redir, target, state); break;
			case RedirOp::TLess:       ok = applyHerestringRedir (redir, target, state); break;
			}

			if (!ok) return false;
			if (flowPending()) return false;
		}

		return true;
	}

	void Executor::undoRedirections(RedirState& state) {
		std::fflush(stdout);
		std::fflush(stderr);
		for (auto it = state.saved.rbegin(); it != state.saved.rend(); ++it) {
			const int fd     = it->first;
			const int backup = it->second;
			_dup2(backup, fd);
			_close(backup);
		}

		state.saved.clear();
		for (const auto& temp_path : state.temps) removeFile(temp_path);
		state.temps.clear();
	}

	static Executor::ArrayAssign expandArrayAssign(Expander& expander,
	                                               const Assignment& assignment) {
		Executor::ArrayAssign array;
		array.name = assignment.name;
		array.append = assignment.append;
		for (const auto& item : assignment.keyed_items) {
			std::string value = expander.expandStringValue(item.value);
			if (item.has_key) {
				array.sparse = true;
				std::string key = expander.expandStringValue(item.key);
				array.keyed.emplace_back(std::move(key), std::move(value));
			} else {
				std::vector<std::string> fields = expander.expandWord(item.value);
				appendFields(array.items, fields);
			}

			if (expander.failed()) break;
		}

		return array;
	}

	static Executor::ElemAssign expandElemAssign(Expander& expander, const Assignment& assignment) {
		Executor::ElemAssign elem;
		elem.name = assignment.name;
		elem.subscript = assignment.subscript;
		elem.value = expander.expandStringValue(assignment.value);
		elem.append = assignment.append;
		return elem;
	}

	static Executor::ScalarAssign expandScalarAssign(Expander& expander,
	                                                 const Assignment& assignment) {
		Executor::ScalarAssign scalar;
		scalar.name = assignment.name;
		scalar.value = expander.expandStringValue(assignment.value);
		scalar.append = assignment.append;
		return scalar;
	}

	bool Executor::expandSimpleCmdAssigns(const SimpleCommand& sc, SimpleCmdAssigns& out) {
		for (const auto& assignment : sc.assignments) {
			if (assignment.is_array) {
				out.array.push_back(expandArrayAssign(expander_, assignment));
			} else if (assignment.has_subscript) {
				out.elem.push_back(expandElemAssign(expander_, assignment));
			} else {
				out.scalar.push_back(expandScalarAssign(expander_, assignment));
			}

			if (expander_.failed()) {
				reportExpanderError(expander_);
				return false;
			}

			if (flowPending()) return false;
		}

		return true;
	}

	bool Executor::expandSimpleCmdArgv(const SimpleCommand& sc, std::vector<std::string>& argv) {
		for (const auto& word : sc.words) {
			std::vector<std::string> fields = expander_.expandWord(word);
			if (expander_.failed()) {
				reportExpanderError(expander_);
				return false;
			}

			if (flowPending()) return false;
			appendFields(argv, fields);
		}

		return true;
	}

	static std::vector<std::string> aliasReplacementWords(Expander& expander,
	                                                      const std::string& alias_value) {
		Lexer lexer(alias_value);
		std::vector<Token> tokens = lexer.tokenize();

		std::vector<std::string> replacement;
		for (auto& token : tokens) {
			if (token.kind != TokKind::Word) continue;

			// Tokens are local and visited once — steal instead of
			// deep-copying the segment list.
			Word word;
			word.segments = std::move(token.segments);
			word.raw = std::move(token.text);

			std::vector<std::string> fields = expander.expandWord(word);
			if (expander.failed()) {
				expander.takeError();
				replacement.push_back(word.raw);   // token.text was moved into word.raw
				continue;
			}

			appendFields(replacement, fields);
		}

		return replacement;
	}

	void Executor::aliasExpandArgvHead(std::vector<std::string>& argv) {
		if (argv.empty() || !env_.expand_aliases()) return;

		std::unordered_set<std::string> seen;
		while (isAlias(argv[0]) && seen.insert(argv[0]).second) {
			const std::vector<std::string> replacement =
				aliasReplacementWords(expander_, aliasValue(argv[0]));
			if (replacement.empty()) break;

			argv.erase(argv.begin());
			argv.insert(argv.begin(), replacement.begin(), replacement.end());
		}
	}

	int Executor::execBareRedirsForExec(const std::vector<Redirection>& redirs) {
		RedirState state;
		if (!applyRedirectionsOrUndo(redirs, state)) return 1;

		for (const auto& saved : state.saved) _close(saved.second);
		return 0;
	}

	static long long nextIndexedAppendSlot(const Environment& env, const std::string& name) {
		const auto* indexed = env.getIndexedArray(name);
		if (indexed == nullptr || indexed->empty()) return 0;
		return indexed->rbegin()->first + 1;
	}

	static std::string readElementValue(const Environment& env,
	                                    const std::string& name,
	                                    long long index,
	                                    const std::string& key,
	                                    bool is_assoc) {
		if (is_assoc) {
			const auto* assoc = env.getAssocArray(name);
			if (assoc == nullptr) return {};

			const auto it = assoc->find(key);
			return (it == assoc->end()) ? std::string() : it->second;
		}

		const auto* indexed = env.getIndexedArray(name);
		if (indexed == nullptr) return {};

		const auto it = indexed->find(index);
		return (it == indexed->end()) ? std::string() : it->second;
	}

	static void appendToIndexedArray(Environment& env, Expander& expander,
	                                 const Executor::ArrayAssign& array) {
		long long next_index = nextIndexedAppendSlot(env, array.name);
		for (const auto& keyed : array.keyed) {
			long long index = 0;
			if (!expander.tryEvalArith(keyed.first, index)) index = next_index;
			env.setIndexedElement(array.name, index, keyed.second);
			next_index = index + 1;
		}

		for (const auto& value : array.items) {
			env.setIndexedElement(array.name, next_index++, value);
		}
	}

	static void assignSparseIndexedArray(Environment& env, Expander& expander,
	                                     const Executor::ArrayAssign& array) {
		std::map<long long, std::string> sparse;
		long long next_index = 0;
		for (const auto& keyed : array.keyed) {
			long long index = 0;
			if (!expander.tryEvalArith(keyed.first, index)) index = next_index;
			sparse[index] = keyed.second;
			next_index = index + 1;
		}

		for (const auto& value : array.items) sparse[next_index++] = value;
		env.setIndexedArraySparse(array.name, std::move(sparse));
	}

	static void applyArrayAssignToEnv(Environment& env, Expander& expander,
	                                  const Executor::ArrayAssign& array) {
		if (env.isAssocArray(array.name)) {
			if (!array.append) env.declareAssocArray(array.name);
			for (const auto& keyed : array.keyed) {
				env.setAssocElement(array.name, keyed.first, keyed.second);
			}

			return;
		}

		if (array.append) {
			appendToIndexedArray(env, expander, array);
			return;
		}

		if (array.sparse) {
			assignSparseIndexedArray(env, expander, array);
			return;
		}

		env.setIndexedArrayFromList(array.name, array.items);
	}

	static void applyElemAssignToEnv(Environment& env, Expander& expander,
	                                 const Executor::ElemAssign& elem) {
		std::string subscript = expander.expandStringValue(elem.subscript);
		if (expander.failed()) {
			expander.takeError();
			subscript.clear();
		}

		const bool is_assoc = env.isAssocArray(elem.name);
		long long index = 0;
		if (!is_assoc && !expander.tryEvalArith(subscript, index)) index = 0;

		std::string value = elem.value;
		if (elem.append) {
			value = readElementValue(env, elem.name, index, subscript, is_assoc) + value;
		}

		if (is_assoc) {
			env.setAssocElement(elem.name, std::move(subscript), std::move(value));
		} else {
			env.setIndexedElement(elem.name, index, std::move(value));
		}
	}

	static void applyScalarAssignToEnv(Environment& env, const Executor::ScalarAssign& scalar) {
		if (!scalar.append) {
			env.set(scalar.name, scalar.value);
			return;
		}

		if (env.isIndexedArray(scalar.name)) {
			const std::string current = readElementValue(env, scalar.name, 0, std::string(), false);
			env.setIndexedElement(scalar.name, 0, current + scalar.value);
			return;
		}

		env.set(scalar.name, env.get(scalar.name) + scalar.value);
	}

	void Executor::applyBareAssignmentsToShell(const SimpleCmdAssigns& assigns) {
		for (const auto& scalar : assigns.scalar) applyScalarAssignToEnv(env_, scalar);
		for (const auto& elem : assigns.elem)     applyElemAssignToEnv(env_, expander_, elem);
		for (const auto& array : assigns.array)   applyArrayAssignToEnv(env_, expander_, array);
	}

	void Executor::traceXtrace(const std::vector<std::string>& argv) {
		if (!env_.xtrace() || argv.empty()) return;

		std::string ps4 = env_.get("PS4");
		if (ps4.empty()) ps4 = "+ ";
		std::fputs(ps4.c_str(), stderr);
		for (std::size_t i = 0; i < argv.size(); ++i) {
			if (i != 0) std::fputc(' ', stderr);
			std::fputs(argv[i].c_str(), stderr);
		}

		std::fputc('\n', stderr);
		std::fflush(stderr);
	}

	namespace executor_detail {
		struct PriorValue {
			std::string name;
			bool existed;
			std::string value;
		};

		struct EnvAssignsGuard {
			Environment& env;
			std::vector<PriorValue> prior;

			EnvAssignsGuard(Environment& owner, const NameValueList& assigns) : env(owner) {
				prior.reserve(assigns.size());
				for (const auto& assign : assigns) {
					prior.push_back({ assign.first, env.has(assign.first), env.get(assign.first) });
					env.set(assign.first, assign.second);
				}
			}

			~EnvAssignsGuard() {
				for (auto it = prior.rbegin(); it != prior.rend(); ++it) {
					if (it->existed) env.set(it->name, it->value);
					else             env.unset(it->name);
				}
			}
		};
	}

	static int callFunctionWithAssigns(Executor& exec,
	                                   const std::string& cmd,
	                                   const std::vector<std::string>& args,
	                                   const NameValueList& assigns) {
		executor_detail::EnvAssignsGuard guard(exec.env(), assigns);
		return exec.callFunction(cmd, args);
	}

	static int callBuiltinWithAssigns(Executor& exec,
	                                  const std::string& cmd,
	                                  const std::vector<std::string>& args,
	                                  const NameValueList& assigns) {
		executor_detail::EnvAssignsGuard guard(exec.env(), assigns);
		return exec.callBuiltin(cmd, args);
	}

	int Executor::execSimpleCommandNoArgv(const SimpleCommand& sc, SimpleCmdAssigns& assigns_data) {
		RedirState state;
		if (!applyRedirectionsOrUndo(sc.redirs, state)) return 1;

		applyBareAssignmentsToShell(assigns_data);
		undoRedirections(state);
		return 0;
	}

	int Executor::runResolvedCommand(const std::vector<std::string>& argv,
	                                 const NameValueList& assigns) {
		const std::string& cmd = argv[0];
		if (isFunction(cmd)) {
			const std::vector<std::string> args(argv.begin() + 1, argv.end());
			return callFunctionWithAssigns(*this, cmd, args, assigns);
		}

		if (isBuiltin(cmd)) {
			const std::vector<std::string> args(argv.begin() + 1, argv.end());
			return callBuiltinWithAssigns(*this, cmd, args, assigns);
		}

		return runExternal(argv, assigns);
	}

	int Executor::execSimpleCommand(const SimpleCommand& sc) {
		const std::size_t proc_subst_watermark = expander_.pendingTempFileWatermark();

		SimpleCmdAssigns assigns_data;
		if (!expandSimpleCmdAssigns(sc, assigns_data)) return 1;
		const NameValueList assigns = assigns_data.scalarPairs();

		std::vector<std::string> argv;
		if (!expandSimpleCmdArgv(sc, argv)) return 1;
		aliasExpandArgvHead(argv);

		if (argv.size() == 1 && argv[0] == "exec") return execBareRedirsForExec(sc.redirs);
		if (argv.empty()) return execSimpleCommandNoArgv(sc, assigns_data);

		if (!assigns_data.array.empty() || !assigns_data.elem.empty()) {
			std::fprintf(stderr, "wbsh: array assignments cannot be used as command prefix\n");
			return 1;
		}

		RedirState state;
		if (!applyRedirectionsOrUndo(sc.redirs, state)) return 1;

		traceXtrace(argv);
		const int status = runResolvedCommand(argv, assigns);
		undoRedirections(state);
		if (flowPending()) return status;

		for (const auto& temp_file : expander_.drainTempFilesSince(proc_subst_watermark)) {
			std::remove(temp_file.c_str());
		}

		setLastStatus(status);
		return status;
	}

	bool Executor::isAbsoluteOrRelativePath(const std::string& name) const {
		if (name.empty()) return false;
		if (name.find('/') != std::string::npos) return true;
#ifdef _WIN32
		if (name.find('\\') != std::string::npos) return true;
		if (name.size() >= 2 && name[1] == ':') return true;
#endif
		return false;
	}

	static bool isExistingFile(const std::filesystem::path& path) {
		struct stat info {};
		return statPath(pathToUtf8(path), info) && (info.st_mode & S_IFMT) != S_IFDIR;
	}

	static std::string tryExecutableWithExtensions(const std::filesystem::path& base) {
#ifdef _WIN32
		for (const char* ext : kExecutableExtensions) {
			std::filesystem::path candidate = base;
			if (ext[0] != '\0') candidate += ext;
			if (isExistingFile(candidate)) return pathToUtf8(candidate);
		}

		return {};
#else
		if (isExistingFile(base)) return base.string();
		return {};
#endif
	}

	std::vector<std::string> splitPathList(const std::string& path) {
		std::vector<std::string> dirs;
		std::string current;
		for (const char c : path) {
			if (c == ';') {
				dirs.push_back(current);
				current.clear();
				continue;
			}

			if (c == ':') {
				const bool drive_letter = current.size() == 1
					&& std::isalpha(static_cast<unsigned char>(current[0]));
				if (drive_letter) {
					current.push_back(c);
				} else {
					dirs.push_back(current);
					current.clear();
				}

				continue;
			}

			current.push_back(c);
		}

		if (!current.empty()) dirs.push_back(current);
		return dirs;
	}

	bool changeDirectory(Executor& exec, const std::string& target, std::string& err) {
		namespace fs = std::filesystem;
		const fs::path win_target = utf8ToPath(exec.pathConv().toWin32(target));
		std::error_code ec;
		fs::current_path(win_target, ec);
		if (ec) {
			err = ec.message();
			return false;
		}

		const std::string old_pwd = exec.env().get("PWD");
		const fs::path cwd = fs::current_path(ec);
		if (ec) return true;

		if (!old_pwd.empty()) exec.env().set("OLDPWD", old_pwd);
		exec.env().set("PWD", exec.pathConv().toPosix(pathToUtf8(cwd)));
		return true;
	}

	std::string Executor::lookupInPathDirs(const std::string& name, const std::string& path) {
		for (const auto& dir : splitPathList(path)) {
			if (dir.empty()) continue;

			std::filesystem::path base = utf8ToPath(path_conv_.toWin32(dir));
			base /= utf8ToPath(name);
			const std::string resolved = tryExecutableWithExtensions(base);
			if (resolved.empty()) continue;

			exec_path_cache_.emplace(name, resolved);
			return resolved;
		}

		return {};
	}

	std::string Executor::findExecutable(const std::string& name) {
		if (isAbsoluteOrRelativePath(name)) {
			const std::filesystem::path candidate = utf8ToPath(path_conv_.toWin32(name));
			return tryExecutableWithExtensions(candidate);
		}

		const std::string path = env_.get("PATH");
		if (path.empty()) return {};

		if (path != exec_path_cache_path_) {
			exec_path_cache_.clear();
			exec_path_cache_path_ = path;
		}

		const auto cached = exec_path_cache_.find(name);
		if (cached != exec_path_cache_.end()) return cached->second;

		return lookupInPathDirs(name, path);
	}

	int Executor::runExternal(const std::vector<std::string>& argv,
	                          const NameValueList& temp_env) {
		if (argv.empty()) return 0;

		const std::string exec_path = findExecutable(argv[0]);
		if (exec_path.empty()) {
			reportCommandNotFound(argv[0]);
			return kStatusCommandNotFound;
		}

		if (looksLikeShellScript(exec_path)) return runShellScript(exec_path, argv, temp_env);

#ifdef _WIN32
		const std::vector<std::string> child_argv = prepareExternalArgv(argv, exec_path, temp_env);
		const NameValueList child_env = prepareExternalEnvOverrides(temp_env);
		SpawnCommand command = makeSpawnCommand(child_argv, exec_path,
			buildEnvBlock(env_, child_env));

		std::fflush(stdout);
		std::fflush(stderr);

		int spawn_status = 0;
		const bool spawned = spawnExternalAndWait(command.exe, command.cmdline,
			command.envblock, &spawn_status);
		if (spawned) return spawn_status;

		if (::GetLastError() == ERROR_BAD_EXE_FORMAT) {
			return runShellScript(exec_path, argv, temp_env);
		}

		std::fprintf(stderr, "wbsh: %s: %s\n", argv[0].c_str(), lastErrorString().c_str());
		return kStatusCommandNotFound;
#else
		return kStatusCommandNotFound;
#endif
	}

#ifdef _WIN32
	std::vector<std::string>
	Executor::prepareExternalArgv(const std::vector<std::string>& argv,
	                              const std::string& exec_path,
	                              const NameValueList& temp_env) {
		const bool translate = !isMsysBinary(exec_path) && !noPathConvSet(temp_env, env_);

		std::vector<std::string> child_argv = argv;
		child_argv[0] = exec_path;
		if (!translate) return child_argv;

		for (std::size_t i = 1; i < child_argv.size(); ++i) {
			child_argv[i] = path_conv_.translateArg(child_argv[i]);
		}

		return child_argv;
	}

	NameValueList Executor::prepareExternalEnvOverrides(const NameValueList& temp_env) {
		NameValueList out = temp_env;

		bool have_path = false;
		bool have_home = false;
		for (auto& assignment : out) {
			if (assignment.first == "PATH") {
				assignment.second = path_conv_.pathListPosixToWin32(assignment.second);
				have_path = true;
			} else if (assignment.first == "HOME") {
				have_home = true;
			}
		}

		if (!have_path) {
			const std::string path = env_.get("PATH");
			if (!path.empty()) out.emplace_back("PATH", path_conv_.pathListPosixToWin32(path));
		}

		if (!have_home) {
			const std::string home = env_.get("HOME");
			if (!home.empty()) out.emplace_back("HOME", path_conv_.toWin32Short(home));
		}

		return out;
	}

	static STARTUPINFOW inheritedStdioStartupInfo() {
		STARTUPINFOW info{};
		info.cb = sizeof(info);
		info.dwFlags = STARTF_USESTDHANDLES;
		info.hStdInput  = stdioHandle(0);
		info.hStdOutput = stdioHandle(1);
		info.hStdError  = stdioHandle(2);

		const HANDLE std_handles[3] = { info.hStdInput, info.hStdOutput, info.hStdError };
		for (HANDLE handle : std_handles) {
			if (handle == nullptr || handle == INVALID_HANDLE_VALUE) continue;
			::SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
		}

		return info;
	}

	bool Executor::spawnExternalAndWait(const std::wstring& exe,
	                                    std::wstring& cmdline,
	                                    std::wstring& envblock,
	                                    int* exit_status) {
		STARTUPINFOW info = inheritedStdioStartupInfo();
		PROCESS_INFORMATION process{};
		const BOOL created = ::CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr, TRUE,
			CREATE_UNICODE_ENVIRONMENT, envblock.data(), nullptr, &info, &process);
		if (created == 0) return false;

		::CloseHandle(process.hThread);
		*exit_status = waitProcessAndClose(process.hProcess);
		return true;
	}
#endif  // _WIN32

	static ExtensionClass classifyByExtension(const std::string& path) {
		const std::string ext = lowerExtension(path);
		if (ext == "sh" || ext == "bash") return ExtensionClass::Script;
		if (ext == "exe" || ext == "com" || ext == "dll" || ext == "msi"
		    || ext == "bat" || ext == "cmd" || ext == "ps1") {
			return ExtensionClass::Native;
		}

		return ExtensionClass::Unknown;
	}

	static std::string readShebangLine(const std::string& path) {
		std::ifstream file(utf8ToPath(path), std::ios::binary);
		if (!file) return {};

		char probe[kShebangProbeBytes];
		file.read(probe, sizeof(probe));
		const std::streamsize got = file.gcount();
		if (got < 2) return {};
		if (probe[0] != '#' || probe[1] != '!') return {};

		std::string line(probe, static_cast<std::size_t>(got));
		const std::size_t newline = line.find('\n');
		if (newline != std::string::npos) line.resize(newline);
		return line;
	}

	// `#!/usr/bin/env [-flags] interp`: the interpreter is the first word
	// after env's own options.
	static std::string interpFromEnvShebang(std::string body) {
		const std::size_t start = body.find_first_not_of(" \t");
		if (start == std::string::npos) return {};
		body = body.substr(start);

		while (!body.empty() && body[0] == '-') {
			const std::size_t space = body.find_first_of(" \t");
			if (space == std::string::npos) return {};
			body = body.substr(space);

			const std::size_t next = body.find_first_not_of(" \t");
			if (next == std::string::npos) return {};
			body = body.substr(next);
		}

		const std::size_t space = body.find_first_of(" \t");
		return (space == std::string::npos) ? body : body.substr(0, space);
	}

	static std::string interpBasenameFromShebang(const std::string& shebang_line) {
		if (shebang_line.size() < 2) return {};

		std::string rest = shebang_line.substr(2);
		const std::size_t start = rest.find_first_not_of(" \t");
		if (start == std::string::npos) return {};
		rest = rest.substr(start);

		const std::size_t space = rest.find_first_of(" \t");
		const std::string interp = (space == std::string::npos) ? rest : rest.substr(0, space);
		const std::size_t slash = interp.find_last_of("/\\");
		std::string base = (slash == std::string::npos) ? interp : interp.substr(slash + 1);

		if (base == "env" && space != std::string::npos) {
			std::string env_interp = interpFromEnvShebang(rest.substr(space + 1));
			if (!env_interp.empty()) base = std::move(env_interp);
		}

		return base;
	}

	static bool isKnownShellInterpreter(const std::string& base) {
		return base == "sh"   || base == "bash" || base == "dash"
		    || base == "zsh"  || base == "ksh"  || base == "wbsh";
	}

	bool Executor::looksLikeShellScript(const std::string& path) const {
		switch (classifyByExtension(path)) {
		case ExtensionClass::Script:  return true;
		case ExtensionClass::Native:  return false;
		case ExtensionClass::Unknown: break;
		}

		const std::string shebang = readShebangLine(path);
		if (shebang.empty()) return false;
		return isKnownShellInterpreter(interpBasenameFromShebang(shebang));
	}

	static void unsetVarsNotIn(Environment& env,
	                           const std::unordered_map<std::string, std::string>& saved_vars) {
		std::vector<std::string> to_unset;
		for (const auto& var : env.vars()) {
			if (saved_vars.count(var.first) == 0) to_unset.push_back(var.first);
		}

		for (const auto& name : to_unset) env.unset(name);
	}

	Executor::ShellScriptScope Executor::snapshotShellScriptScope() const {
		ShellScriptScope snap;
		snap.saved_vars  = env_.vars();
		snap.saved_pos   = env_.positional();
		snap.saved_name  = env_.shellName();
		snap.saved_funcs = functions_;
		snap.s_errexit   = env_.errexit();
		snap.s_nounset   = env_.nounset();
		snap.s_xtrace    = env_.xtrace();
		snap.s_pipefail  = env_.pipefail();
		snap.s_noglob    = env_.noglob();

		std::error_code ec;
		snap.saved_cwd = std::filesystem::current_path(ec);
		return snap;
	}

	void Executor::restoreShellScriptScope(ShellScriptScope& snap, bool force_set) {
		unsetVarsNotIn(env_, snap.saved_vars);

		// `forceSet` (readonly bypass) only when unwinding a pending
		// signal, plain `set` otherwise — a deliberate asymmetry preserved
		// from the original implementation.
		for (const auto& var : snap.saved_vars) {
			if (force_set) env_.forceSet(var.first, var.second);
			else           env_.set(var.first, var.second);
		}

		env_.setPositional(std::move(snap.saved_pos));
		env_.setShellName(snap.saved_name);
		env_.setErrexit(snap.s_errexit);
		env_.setNounset(snap.s_nounset);
		env_.setXtrace(snap.s_xtrace);
		env_.setPipefail(snap.s_pipefail);
		env_.setNoglob(snap.s_noglob);
		functions_ = std::move(snap.saved_funcs);

		std::error_code ec;
		std::filesystem::current_path(snap.saved_cwd, ec);
	}

	static std::string readScriptBody(std::ifstream& file) {
		std::stringstream buffer;
		buffer << file.rdbuf();
		std::string body = buffer.str();
		normalizeCrlf(body);
		return body;
	}

	int Executor::runShellScript(const std::string& path,
	                             const std::vector<std::string>& argv,
	                             const NameValueList& temp_env) {
		std::ifstream file(utf8ToPath(path), std::ios::binary);
		if (!file) {
			reportPathError(path);
			return kStatusCommandNotFound;
		}

		const std::string body = readScriptBody(file);
		ShellScriptScope snap = snapshotShellScriptScope();

		for (const auto& assignment : temp_env) env_.set(assignment.first, assignment.second);
		env_.setShellName(path);
		env_.setPositional(std::vector<std::string>(argv.begin() + 1, argv.end()));

		int status = executeText(body, path);
		consumeFlow(FlowSignal::Kind::Exit, &status);
		restoreShellScriptScope(snap, /*force_set=*/flowPending());
		return status;
	}

	int Executor::execBraceGroup(const BraceGroup& group) {
		RedirState state;
		if (!applyRedirectionsOrUndo(group.redirs, state)) return 1;

		int status = 0;
		if (group.body != nullptr) status = execNode(*group.body);
		undoRedirections(state);
		return status;
	}

	Executor::SubshellScope Executor::snapshotSubshellScope() const {
		SubshellScope snap;
		snap.saved_vars = env_.vars();
		snap.errexit  = env_.errexit();
		snap.nounset  = env_.nounset();
		snap.xtrace   = env_.xtrace();
		snap.pipefail = env_.pipefail();
		snap.noglob   = env_.noglob();
		snap.traps = trap_handlers_;

		std::error_code ec;
		snap.saved_cwd = std::filesystem::current_path(ec);
		return snap;
	}

	void Executor::restoreSubshellScope(SubshellScope& snap) {
		unsetVarsNotIn(env_, snap.saved_vars);
		for (const auto& var : snap.saved_vars) env_.forceSet(var.first, var.second);

		env_.setErrexit(snap.errexit);
		env_.setNounset(snap.nounset);
		env_.setXtrace(snap.xtrace);
		env_.setPipefail(snap.pipefail);
		env_.setNoglob(snap.noglob);
		trap_handlers_ = std::move(snap.traps);

		std::error_code ec;
		std::filesystem::current_path(snap.saved_cwd, ec);
	}

	int Executor::execSubshell(const Subshell& subshell) {
		SubshellScope snap = snapshotSubshellScope();

		RedirState state;
		if (!applyRedirectionsOrUndo(subshell.redirs, state)) return 1;

		int status = 0;
		if (subshell.body != nullptr) status = execNode(*subshell.body);
		consumeFlow(FlowSignal::Kind::Exit, &status);
		undoRedirections(state);

		if (!flowPending()) fireExitTrap();
		restoreSubshellScope(snap);
		return status;
	}

	int Executor::runIfBranches(const IfClause& clause) {
		int status = 0;
		for (const auto& branch : clause.branches) {
			pushErrexitSuppress();
			const int cond = (branch.cond != nullptr) ? execNode(*branch.cond) : 0;
			popErrexitSuppress();
			if (flowPending()) return status;

			setLastStatus(cond);
			if (cond != 0) continue;

			if (branch.body != nullptr) runBody(branch.body, status);
			return status;
		}

		if (clause.else_body != nullptr) runBody(clause.else_body, status);
		return status;
	}

	int Executor::execIf(const IfClause& clause) {
		RedirState state;
		if (!applyRedirectionsOrUndo(clause.redirs, state)) return 1;

		const int status = runIfBranches(clause);
		undoRedirections(state);
		return status;
	}

	LoopFlowAction Executor::dispatchLoopFlow() {
		switch (flow_.kind) {
		case FlowSignal::Kind::Continue:
			if (--flow_.count > 0) return LoopFlowAction::Propagate;
			clearFlow();
			return LoopFlowAction::NextIter;
		case FlowSignal::Kind::Break:
			if (--flow_.count > 0) return LoopFlowAction::Propagate;
			clearFlow();
			return LoopFlowAction::ExitLoop;
		case FlowSignal::Kind::Return:
		case FlowSignal::Kind::Exit:
			return LoopFlowAction::Propagate;
		case FlowSignal::Kind::None:
		default:
			return LoopFlowAction::Normal;
		}
	}

	static bool loopEnds(LoopFlowAction action) {
		return action == LoopFlowAction::ExitLoop || action == LoopFlowAction::Propagate;
	}

	int Executor::runWhileLoop(const WhileClause& clause) {
		int status = 0;
		for (;;) {
			pushErrexitSuppress();
			const int cond = (clause.cond != nullptr) ? execNode(*clause.cond) : 0;
			popErrexitSuppress();
			if (flowPending()) break;

			setLastStatus(cond);
			const bool keep_going = clause.until ? (cond != 0) : (cond == 0);
			if (!keep_going) break;

			runBody(clause.body, status);
			if (loopEnds(dispatchLoopFlow())) break;
		}

		return status;
	}

	int Executor::execWhile(const WhileClause& clause) {
		RedirState state;
		if (!applyRedirectionsOrUndo(clause.redirs, state)) return 1;

		++loop_depth_;
		const int status = runWhileLoop(clause);
		--loop_depth_;
		undoRedirections(state);
		return status;
	}

	static bool evalArithOrReport(Expander& expander, const std::string& expr, long long& value) {
		value = expander.evalArith(expr);
		if (!expander.failed()) return true;

		reportExpanderError(expander);
		return false;
	}

	int Executor::runArithLoop(const ForClause& clause) {
		long long value = 0;
		if (!clause.arith_init.empty() && !evalArithOrReport(expander_, clause.arith_init, value)) {
			return 1;
		}

		int status = 0;
		for (;;) {
			if (!clause.arith_cond.empty()) {
				if (!evalArithOrReport(expander_, clause.arith_cond, value)) return 1;
				if (value == 0) break;
			}

			runBody(clause.body, status);
			if (loopEnds(dispatchLoopFlow())) break;

			if (!clause.arith_update.empty()
			    && !evalArithOrReport(expander_, clause.arith_update, value)) {
				return 1;
			}
		}

		return status;
	}

	int Executor::execForArith(const ForClause& clause) {
		RedirState state;
		if (!applyRedirectionsOrUndo(clause.redirs, state)) return 1;

		++loop_depth_;
		const int status = runArithLoop(clause);
		--loop_depth_;
		undoRedirections(state);
		return status;
	}

	// The list a `for`/`select` header iterates over: the expanded
	// `in word...` list, or `"$@"` when there is no `in` clause. Returns
	// false if expansion failed or shell control flow (return from a
	// function, etc.) fired mid-expansion; `*out_status` then holds what
	// the caller should propagate.
	bool Executor::expandForWordList(const ForClause& fc, std::vector<std::string>& values,
	                                 int* out_status) {
		*out_status = 0;
		if (!fc.has_in) {
			values = env_.positional();
			return true;
		}

		for (const auto& word : fc.items) {
			std::vector<std::string> fields = expander_.expandWord(word);
			if (expander_.failed()) {
				reportExpanderError(expander_);
				*out_status = 1;
				return false;
			}

			if (flowPending()) return false;
			appendFields(values, fields);
		}

		return true;
	}

	int Executor::runForLoop(const ForClause& clause) {
		int status = 0;
		std::vector<std::string> values;
		if (!expandForWordList(clause, values, &status)) return status;

		for (const auto& value : values) {
			env_.set(clause.var, value);
			runBody(clause.body, status);
			if (loopEnds(dispatchLoopFlow())) break;
		}

		return status;
	}

	int Executor::execFor(const ForClause& clause) {
		if (clause.is_arith) return execForArith(clause);
		if (clause.is_select) return execSelect(clause);

		RedirState state;
		if (!applyRedirectionsOrUndo(clause.redirs, state)) return 1;

		++loop_depth_;
		const int status = runForLoop(clause);
		--loop_depth_;
		undoRedirections(state);
		return status;
	}

	// A minimal line reader for `select`'s menu prompt: reads up to (and
	// consuming) the next '\n', or to EOF. Returns false only on EOF
	// with nothing read, matching `select`'s "stop the loop" signal.
	static bool readSelectReplyLine(std::string& line) {
		int c = 0;
		while ((c = std::fgetc(stdin)) != EOF) {
			if (c == '\n') return true;
			line.push_back(static_cast<char>(c));
		}

		return !line.empty();
	}

	static void printSelectMenu(const std::vector<std::string>& values) {
		for (std::size_t i = 0; i < values.size(); ++i) {
			std::fprintf(stderr, "%zu) %s\n", i + 1, values[i].c_str());
		}
	}

	static std::string selectedValue(const std::string& reply,
	                                 const std::vector<std::string>& values) {
		long long choice = 0;
		if (!parseLL(reply, choice)) return {};
		if (choice < 1 || static_cast<std::size_t>(choice) > values.size()) return {};
		return values[static_cast<std::size_t>(choice) - 1];
	}

	int Executor::runSelectLoop(const ForClause& clause) {
		int status = 0;
		std::vector<std::string> values;
		if (!expandForWordList(clause, values, &status)) return status;

		const std::string ps3 = env_.has("PS3") ? env_.get("PS3") : "#? ";
		for (;;) {
			printSelectMenu(values);
			std::fputs(ps3.c_str(), stderr);
			std::fflush(stderr);

			std::string reply;
			if (!readSelectReplyLine(reply)) return 1;
			env_.set("REPLY", reply);
			env_.set(clause.var, selectedValue(reply, values));

			runBody(clause.body, status);
			if (loopEnds(dispatchLoopFlow())) break;
		}

		return status;
	}

	int Executor::execSelect(const ForClause& clause) {
		RedirState state;
		if (!applyRedirectionsOrUndo(clause.redirs, state)) return 1;

		++loop_depth_;
		const int status = runSelectLoop(clause);
		--loop_depth_;
		undoRedirections(state);
		return status;
	}

	bool Executor::patternMatches(const std::string& pat, const std::string& s) {
		return fnmatchFull(pat, s);
	}

	bool Executor::caseItemMatches(const CaseClause::Item& item, const std::string& subject) {
		for (const auto& pattern_word : item.patterns) {
			const std::string pattern = expander_.expandStringValue(pattern_word);
			if (expander_.failed()) {
				expander_.takeError();
				continue;
			}

			if (patternMatches(pattern, subject)) return true;
		}

		return false;
	}

	// `;;` ends the case, `;&` falls into the next arm's body unconditionally,
	// `;;&` keeps testing the remaining patterns.
	int Executor::runCaseItems(const CaseClause& clause, const std::string& subject) {
		int status = 0;
		for (std::size_t i = 0; i < clause.items.size(); ++i) {
			const auto& item = clause.items[i];
			if (!caseItemMatches(item, subject)) continue;

			if (item.body != nullptr) runBody(item.body, status);
			if (flowPending()) break;
			if (item.term == CaseClause::Term::DSemiAmp) continue;

			if (item.term == CaseClause::Term::SemiAmp && i + 1 < clause.items.size()) {
				const auto& next = clause.items[i + 1];
				if (next.body != nullptr) runBody(next.body, status);
			}

			break;
		}

		return status;
	}

	int Executor::execCase(const CaseClause& clause) {
		RedirState state;
		if (!applyRedirectionsOrUndo(clause.redirs, state)) return 1;

		const std::string subject = expander_.expandStringValue(clause.subject);
		if (expander_.failed()) {
			reportExpanderError(expander_);
			undoRedirections(state);
			return 1;
		}

		const int status = runCaseItems(clause, subject);
		undoRedirections(state);
		return status;
	}

	int Executor::execFunctionDef(const FunctionDef& fd) {
		functions_[fd.name] = &fd;
		return 0;
	}

	namespace executor_detail {
		struct DBracketContext {
			Expander& expander;
			const PathConv& path_conv;
			Environment& env;
			bool nocasematch;
		};
	}

	using executor_detail::DBracketContext;

	static bool evalDBracketVarSet(const std::string& lhs, Environment& env) {
		const std::size_t bracket = lhs.find('[');
		if (bracket == std::string::npos || lhs.empty() || lhs.back() != ']') {
			if (env.isIndexedArray(lhs) || env.isAssocArray(lhs)) return true;
			return env.has(lhs);
		}

		const std::string name = lhs.substr(0, bracket);
		const std::string subscript = lhs.substr(bracket + 1, lhs.size() - bracket - 2);
		if (const auto* assoc = env.getAssocArray(name)) return assoc->count(subscript) != 0;
		if (const auto* indexed = env.getIndexedArray(name)) {
			long long index = 0;
			return parseLL(subscript, index) && indexed->count(index) != 0;
		}

		return false;
	}

	static bool evalDBracketUnaryTest(char op, const std::string& lhs, const DBracketContext& ctx) {
		if (op == 'v') return evalDBracketVarSet(lhs, ctx.env);
		if (op == 'z') return lhs.empty();
		if (op == 'n') return !lhs.empty();

		const std::string path = ctx.path_conv.toWin32(lhs);
		struct stat info {};
		const bool exists = statPath(path, info);
		switch (op) {
		case 'e': return exists;
		case 'f': return exists && (info.st_mode & S_IFMT) == S_IFREG;
		case 'd': return exists && (info.st_mode & S_IFMT) == S_IFDIR;
		case 's': return exists && info.st_size > 0;
		case 'r': return exists && (info.st_mode & 0444) != 0;
		case 'w': return exists && (info.st_mode & 0222) != 0;
		case 'x': return exists && (info.st_mode & 0111) != 0;
		default:  return false;
		}
	}

	// `[[ x == pat ]]` glob-matches an unquoted RHS against x (bash
	// treats it as a pattern, not a literal string) but compares
	// literally when the RHS was quoted, e.g. `[[ x == "pat" ]]`.
	static bool rhsWordIsPattern(const Word& rhs) {
		for (const auto& segment : rhs.segments) {
			if (segment.kind != WordSegment::Kind::SingleQuoted
			    && segment.kind != WordSegment::Kind::DoubleQuoted) {
				return true;
			}
		}

		return false;
	}

	static bool isDBracketStringOp(const std::string& op) {
		return op == "==" || op == "=" || op == "!=" || op == "<" || op == ">";
	}

	static bool isDBracketArithOp(const std::string& op) {
		return op == "-eq" || op == "-ne" || op == "-lt"
		    || op == "-le" || op == "-gt" || op == "-ge";
	}

	static bool isDBracketFileOp(const std::string& op) {
		return op == "-ef" || op == "-nt" || op == "-ot";
	}

	static bool evalDBracketStringOp(const std::string& op, const std::string& lhs,
	                                 const std::string& rhs, bool rhs_is_pattern) {
		if (op == "==" || op == "=") return rhs_is_pattern ? fnmatchFull(rhs, lhs) : lhs == rhs;
		if (op == "!=")              return rhs_is_pattern ? !fnmatchFull(rhs, lhs) : lhs != rhs;
		if (op == "<")               return lhs < rhs;
		if (op == ">")               return lhs > rhs;
		return false;
	}

	static bool evalDBracketRegexOp(const std::string& lhs, const std::string& rhs,
	                                bool nocasematch) {
		auto flags = std::regex::ECMAScript;
		if (nocasematch) flags = flags | std::regex::icase;

		std::regex re;
		if (!compileRegex(re, rhs, flags)) return false;
		return searchRegex(lhs, re);
	}

	static bool evalDBracketArithOp(const std::string& op,
	                                const std::string& lhs, const std::string& rhs) {
		long long left = 0;
		long long right = 0;
		if (!parseLL(lhs, left) || !parseLL(rhs, right)) return false;

		if (op == "-eq") return left == right;
		if (op == "-ne") return left != right;
		if (op == "-lt") return left <  right;
		if (op == "-le") return left <= right;
		if (op == "-gt") return left >  right;
		return left >= right;
	}

	static bool evalDBracketFileOp(const std::string& op,
	                               const std::string& lhs, const std::string& rhs,
	                               const PathConv& path_conv) {
		const std::string lhs_path = path_conv.toWin32(lhs);
		const std::string rhs_path = path_conv.toWin32(rhs);
		struct stat lhs_info{};
		struct stat rhs_info{};
		const bool lhs_exists = statPath(lhs_path, lhs_info);
		const bool rhs_exists = statPath(rhs_path, rhs_info);

		if (op == "-nt") {
			return lhs_exists && (!rhs_exists || lhs_info.st_mtime > rhs_info.st_mtime);
		}

		if (op == "-ot") {
			return rhs_exists && (!lhs_exists || lhs_info.st_mtime < rhs_info.st_mtime);
		}

		return lhs_exists && rhs_exists
			&& lhs_info.st_dev == rhs_info.st_dev && lhs_info.st_ino == rhs_info.st_ino;
	}

	static bool evalDBracketBinary(const DBracketCond::Expr& expr, const std::string& lhs,
	                               const DBracketContext& ctx) {
		const std::string rhs = ctx.expander.expandStringValue(expr.rhs);
		if (ctx.expander.failed()) return false;

		if (isDBracketStringOp(expr.op)) {
			const std::string cmp_lhs = ctx.nocasematch ? asciiLower(lhs) : lhs;
			const std::string cmp_rhs = ctx.nocasematch ? asciiLower(rhs) : rhs;
			return evalDBracketStringOp(expr.op, cmp_lhs, cmp_rhs, rhsWordIsPattern(expr.rhs));
		}

		if (expr.op == "=~") return evalDBracketRegexOp(lhs, rhs, ctx.nocasematch);
		if (isDBracketArithOp(expr.op)) return evalDBracketArithOp(expr.op, lhs, rhs);
		if (isDBracketFileOp(expr.op)) return evalDBracketFileOp(expr.op, lhs, rhs, ctx.path_conv);
		return false;
	}

	static bool evalDBracketPrimary(const DBracketCond::Expr& expr, const DBracketContext& ctx) {
		const std::string lhs = ctx.expander.expandStringValue(expr.lhs);
		if (ctx.expander.failed()) return false;
		if (expr.op.empty()) return !lhs.empty();
		if (expr.op.size() == 2 && expr.op[0] == '-') {
			return evalDBracketUnaryTest(expr.op[1], lhs, ctx);
		}

		return evalDBracketBinary(expr, lhs, ctx);
	}

	static bool evalDBracketExpr(const DBracketCond::Expr& expr, const DBracketContext& ctx) {
		using K = DBracketCond::Expr::K;
		switch (expr.k) {
		case K::And:  return evalDBracketExpr(*expr.a, ctx) && evalDBracketExpr(*expr.b, ctx);
		case K::Or:   return evalDBracketExpr(*expr.a, ctx) || evalDBracketExpr(*expr.b, ctx);
		case K::Not:  return !evalDBracketExpr(*expr.a, ctx);
		case K::Prim: return evalDBracketPrimary(expr, ctx);
		}

		return false;
	}

	int Executor::execDBracket(const DBracketCond& dc) {
		RedirState state;
		const bool ok = applyRedirections(dc.redirs, state);

		int status = 1;
		if (ok && dc.root != nullptr) {
			const DBracketContext ctx{ expander_, path_conv_, env_, env_.nocasematch() };
			status = evalDBracketExpr(*dc.root, ctx) ? 0 : 1;
		}

		if (expander_.failed()) {
			reportExpanderError(expander_);
			status = 1;
		}

		undoRedirections(state);
		setLastStatus(status);
		return status;
	}

	int Executor::execArithCommand(const ArithCommand& ac) {
		RedirState state;
		const bool ok = applyRedirections(ac.redirs, state);

		int status = 1;
		long long value = 0;
		if (ok && evalArithOrReport(expander_, ac.expr, value)) status = (value != 0) ? 0 : 1;

		undoRedirections(state);
		setLastStatus(status);
		return status;
	}

	void Executor::registerBuiltin(std::string name, BuiltinFn fn) {
		builtins_[std::move(name)] = fn;
	}

	// A util's command refuses to take a bundled name: shadowing `ls`
	// from a DLL someone dropped in a folder is not a thing a shell
	// should let happen quietly.
	bool Executor::registerPluginCommand(std::string name, PluginCommand command) {
		if (command.fn == nullptr || name.empty()) return false;
		if (builtins_.count(name) != 0) return false;
		if (plugin_commands_.count(name) != 0) return false;

		plugin_commands_[std::move(name)] = command;
		return true;
	}

	bool Executor::isBuiltin(const std::string& name) const {
		return builtins_.count(name) != 0 || plugin_commands_.count(name) != 0;
	}

	bool Executor::isFunction(const std::string& name) const {
		return functions_.count(name) != 0;
	}

	// argv is rebuilt the way a program expects it -- the command's own
	// name first -- because a util is written against argc/argv and not
	// against this shell's idea of an argument list.
	static int callPluginCommand(const PluginCommand& command, const std::string& name,
			const std::vector<std::string>& args) {
		std::vector<const char*> argv;
		argv.reserve(args.size() + 2);
		argv.push_back(name.c_str());
		for (const std::string& arg : args) argv.push_back(arg.c_str());
		argv.push_back(nullptr);

		ScopedCtrlCCapture capture;
		return command.fn(command.user, static_cast<int>(argv.size()) - 1, argv.data());
	}

	int Executor::callBuiltin(const std::string& name, const std::vector<std::string>& args) {
		const auto builtin = builtins_.find(name);
		if (builtin != builtins_.end()) return builtin->second(*this, args);

		const auto plugin = plugin_commands_.find(name);
		if (plugin == plugin_commands_.end()) return kStatusCommandNotFound;

		return callPluginCommand(plugin->second, name, args);
	}

	int Executor::callFunction(const std::string& name, const std::vector<std::string>& args) {
		const auto it = functions_.find(name);
		if (it == functions_.end()) return kStatusCommandNotFound;
		const FunctionDef* def = it->second;

		const std::vector<std::string> saved_positional = env_.positional();
		env_.setPositional(args);
		++func_depth_;
		scope_stack_.emplace_back();

		int status = 0;
		if (def->body != nullptr) status = execNode(*def->body);
		consumeFlow(FlowSignal::Kind::Return, &status);

		popLocalScope();
		--func_depth_;
		env_.setPositional(saved_positional);
		return status;
	}

	void Executor::popLocalScope() {
		if (scope_stack_.empty()) return;

		const std::vector<ScopeEntry>& top = scope_stack_.back();
		for (auto entry = top.rbegin(); entry != top.rend(); ++entry) {
			if (entry->had_prev) env_.set(entry->name, entry->prev_value);
			else                 env_.unset(entry->name);
		}

		scope_stack_.pop_back();
	}

	int Executor::registerJob(void* handle, long long pid, std::string cmd_text) {
		Job job;
		job.id = ++next_job_id_;
#ifdef _WIN32
		job.handle = static_cast<HANDLE>(handle);
#else
		job.handle = handle;
#endif
		job.pid = pid;
		job.cmd_text = std::move(cmd_text);
		job.running = true;
		jobs_.push_back(job);
		return job.id;
	}

	bool Executor::reapJobs() {
		bool changed = false;
#ifdef _WIN32
		for (auto& job : jobs_) {
			if (!job.running || job.handle == nullptr) continue;

			DWORD exit_code = 0;
			if (::GetExitCodeProcess(job.handle, &exit_code) == 0) continue;
			if (exit_code == STILL_ACTIVE) continue;

			job.running = false;
			job.exit_code = static_cast<int>(exit_code);
			::CloseHandle(job.handle);
			job.handle = nullptr;
			changed = true;
		}
#endif
		return changed;
	}

	static void waitJobToFinish(Executor::Job& job) {
#ifdef _WIN32
		job.exit_code = waitProcessAndClose(job.handle);
		job.running = false;
		job.handle = nullptr;
#else
		(void)job;
#endif
	}

	int Executor::waitForJob(int id) {
		for (auto& job : jobs_) {
			if (job.id != id) continue;
			if (job.running) waitJobToFinish(job);
			return job.exit_code;
		}

		return -1;
	}

	void Executor::waitForAllJobs() {
		for (auto& job : jobs_) {
			if (job.running) waitJobToFinish(job);
		}
	}

	void Executor::fireExitTrap() {
		const auto it = trap_handlers_.find("EXIT");
		if (it == trap_handlers_.end()) return;

		const std::string cmd = it->second;
		trap_handlers_.erase(it);
		executeText(cmd, "<EXIT trap>");
		clearFlow();
	}

	static void trimToMaxHistory(std::vector<std::string>& cmds, std::vector<int>& statuses) {
		if (cmds.size() <= kMaxHistory) return;

		const std::size_t drop = cmds.size() - kMaxHistory;
		cmds.erase(cmds.begin(), cmds.begin() + static_cast<std::ptrdiff_t>(drop));
		if (statuses.size() >= drop) {
			statuses.erase(statuses.begin(), statuses.begin() + static_cast<std::ptrdiff_t>(drop));
		} else {
			statuses.clear();
		}
	}

	void Executor::addHistoryEntry(std::string line) {
		if (line.empty()) return;
		if (!history_.empty() && history_.back() == line) return;

		history_.push_back(std::move(line));
		history_status_.push_back(0);
		trimToMaxHistory(history_, history_status_);
	}

	void Executor::markLastHistoryStatus(int status) {
		if (history_status_.empty()) return;
		history_status_.back() = status;
	}

	void Executor::setHistoryEntryStatus(std::size_t index, int status) {
		if (index >= history_status_.size()) return;
		history_status_[index] = status;
	}

	static void stripLineEnding(std::string& line) {
		while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
	}

	static bool parseHistoryV2Line(const std::string& line, int& status, std::string& command) {
		const std::size_t tab = line.find('\t');
		if (tab == std::string::npos) return false;
		if (!parseInt(line.substr(0, tab), status)) return false;

		command = line.substr(tab + 1);
		return !command.empty();
	}

	bool Executor::loadHistoryFromFile(const std::string& path) {
		std::ifstream file(utf8ToPath(path));
		if (!file) return false;

		std::string line;
		bool header_checked = false;
		bool v2 = false;
		while (std::getline(file, line)) {
			stripLineEnding(line);
			if (!header_checked) {
				header_checked = true;
				if (line == kHistoryV2Header) {
					v2 = true;
					continue;
				}
			}

			if (line.empty()) continue;

			int status = 0;
			std::string command = line;
			if (v2 && !parseHistoryV2Line(line, status, command)) continue;

			history_.push_back(std::move(command));
			history_status_.push_back(status);
		}

		trimToMaxHistory(history_, history_status_);
		return true;
	}

	bool Executor::saveHistoryToFile(const std::string& path) const {
		std::ofstream file(utf8ToPath(path), std::ios::trunc);
		if (!file) return false;

		file << kHistoryV2Header << '\n';
		for (std::size_t i = 0; i < history_.size(); ++i) {
			const int status = (i < history_status_.size()) ? history_status_[i] : 0;
			file << status << '\t' << history_[i] << '\n';
		}

		return true;
	}

	static std::string shellQuoteSingle(const std::string& text) {
		std::string quoted = "'";
		for (char c : text) {
			if (c == '\'') quoted += "'\\''";
			else           quoted.push_back(c);
		}

		quoted += "'";
		return quoted;
	}

	template <typename Map>
	static std::vector<std::string> sortedKeys(const Map& map) {
		std::vector<std::string> keys;
		keys.reserve(map.size());
		for (const auto& entry : map) keys.push_back(entry.first);
		std::sort(keys.begin(), keys.end());
		return keys;
	}

	std::string Executor::serializeAliases() const {
		std::vector<std::pair<std::string, std::string>> sorted(aliases_.begin(), aliases_.end());
		std::sort(sorted.begin(), sorted.end());

		std::string out;
		for (const auto& alias : sorted) {
			out += "alias ";
			out += alias.first;
			out += "=";
			out += shellQuoteSingle(alias.second);
			out += "\n";
		}

		return out;
	}

	static void appendIndexedArray(std::string& out, const std::string& name,
	                               const Environment::IndexedArray& array) {
		out += name;
		out += "=(";
		bool first = true;
		for (const auto& element : array) {
			if (!first) out.push_back(' ');
			out += "[";
			out += std::to_string(element.first);
			out += "]=";
			out += shellQuoteSingle(element.second);
			first = false;
		}

		out += ")\n";
	}

	static void appendAssocArray(std::string& out, const std::string& name,
	                             const Environment::AssocArray& array) {
		out += "declare -A ";
		out += name;
		out += "\n";
		for (const auto& element : array) {
			out += name;
			out += "[";
			out += shellQuoteSingle(element.first);
			out += "]=";
			out += shellQuoteSingle(element.second);
			out += "\n";
		}
	}

	std::string Executor::serializeArrays() const {
		std::string out;
		for (const auto& name : sortedKeys(env_.indexedArrays())) {
			const auto* array = env_.getIndexedArray(name);
			if (array != nullptr) appendIndexedArray(out, name, *array);
		}

		for (const auto& name : sortedKeys(env_.assocArrays())) {
			const auto* array = env_.getAssocArray(name);
			if (array != nullptr) appendAssocArray(out, name, *array);
		}

		return out;
	}

	static bool functionNameLess(const FunctionDef* a, const FunctionDef* b) {
		return a->name < b->name;
	}

	std::string Executor::serializeFunctions() const {
		std::vector<const FunctionDef*> defs;
		defs.reserve(functions_.size());
		for (const auto& entry : functions_) {
			if (entry.second != nullptr && !entry.second->body_text.empty()) {
				defs.push_back(entry.second);
			}
		}

		std::sort(defs.begin(), defs.end(), functionNameLess);
		std::string out;
		for (const FunctionDef* def : defs) {
			out += def->name;
			out += "() ";
			out += def->body_text;
			out.push_back('\n');
		}

		return out;
	}

	template <typename Scope>
	static bool scopeHasName(const Scope& scope, const std::string& name) {
		for (const auto& entry : scope) {
			if (entry.name == name) return true;
		}

		return false;
	}

	void Executor::declareLocal(const std::string& name, const std::string& value) {
		if (scope_stack_.empty()) {
			env_.set(name, value);
			return;
		}

		std::vector<ScopeEntry>& top = scope_stack_.back();
		if (!scopeHasName(top, name)) {
			ScopeEntry entry;
			entry.name = name;
			// has()/get() rather than a single vars() lookup: both also
			// see array variables (get() reads element 0), and the scope
			// pop must restore those instead of unsetting them.
			entry.had_prev = env_.has(name);
			entry.prev_value = env_.get(name);
			top.push_back(std::move(entry));
		}

		env_.set(name, value);
	}

	template <typename Error>
	static void reportSourceErrors(const std::string& origin, const std::vector<Error>& errors) {
		for (const auto& error : errors) {
			std::fprintf(stderr, "wbsh: %s:%zu:%zu: %s\n",
				origin.c_str(), error.loc.line, error.loc.column, error.message.c_str());
		}
	}

	int Executor::executeText(const std::string& source_text, const std::string& origin) {
		Lexer lexer(source_text);
		std::vector<Token> tokens = lexer.tokenize();
		reportSourceErrors(origin, lexer.errors());

		Parser parser(std::move(tokens), source_text);
		Node* root = parser.parseProgram();
		reportSourceErrors(origin, parser.errors());
		if (root == nullptr) return 1;

		owned_arenas_.push_back(parser.takeArena());
		return execute(*root);
	}

	std::string Executor::run(const std::string& body) {
		std::string out = runRaw(body);
		// `\r` is stripped along with `\n`: the child's CRT text mode may
		// have written `\r\n`, and a stray CR corrupts captured values.
		while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
		return out;
	}

	std::string Executor::runRaw(const std::string& body) {
		const std::string capture_path = makeTempFile();
		if (capture_path.empty()) return {};

		const int capture_fd = _open(capture_path.c_str(),
			_O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
		if (capture_fd < 0) {
			removeFile(capture_path);
			return {};
		}

		std::fflush(stdout);
		const int saved_stdout = _dup(1);
		_dup2(capture_fd, 1);
		_close(capture_fd);
		executeText(body, "<command-substitution>");
		consumeFlow(FlowSignal::Kind::Exit);
		std::fflush(stdout);
		_dup2(saved_stdout, 1);
		_close(saved_stdout);

		std::string out;
		if (!flowPending()) out = readAllText(capture_path);
		removeFile(capture_path);
		return out;
	}

}  // namespace wbsh

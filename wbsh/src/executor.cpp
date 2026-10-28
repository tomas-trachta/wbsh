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
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unordered_set>
#include <utility>
#include <vector>

#include "devices.h"
#include "exitstatus.h"
#include "fnmatch.h"
#include "keymap.h"
#include "lexer.h"
#include "numparse.h"
#include "parser.h"
#include "posixregex.h"
#include "stdinsync.h"
#include "testops.h"

namespace wbsh {

	using NameValueList = std::vector<std::pair<std::string, std::string>>;

	static const int kStatusCommandNotFound = 127;
	static const long long kMillisPerMinute = 60000;
	static const std::size_t kShebangProbeBytes = 256;

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

	static std::string shellQuoteSingle(const std::string& text) {
		std::string quoted = "'";
		for (char c : text) {
			if (c == '\'') quoted += "'\\''";
			else           quoted.push_back(c);
		}

		quoted += "'";
		return quoted;
	}

	static bool isQuotedShellChar(const std::string& word, std::size_t i) {
		static const std::string kMetaChars = " \t\n'\"\\|&;()<>!{}*[?]^$`";
		const char c = word[i];
		if (kMetaChars.find(c) != std::string::npos) return true;
		if (c == '~') return i == 0 || word[i - 1] == '=' || word[i - 1] == ':';
		if (c == '#') return i == 0;
		return false;
	}

	static bool isControlChar(char c) {
		const unsigned char code = static_cast<unsigned char>(c);
		return code < 0x20 || code == 0x7f;
	}

	static void appendAnsiCEscape(std::string& out, char c) {
		static const char kLetters[] = "\aa\bb\x1b" "e\ff\nn\rr\tt\vv\\\\''";
		for (std::size_t k = 0; kLetters[k] != '\0'; k += 2) {
			if (kLetters[k] != c) continue;

			out.push_back('\\');
			out.push_back(kLetters[k + 1]);
			return;
		}

		if (!isControlChar(c)) {
			out.push_back(c);
			return;
		}

		char octal[8];
		std::snprintf(octal, sizeof octal, "\\%03o", static_cast<unsigned char>(c));
		out += octal;
	}

	static std::string ansiCQuote(const std::string& word) {
		std::string quoted = "$'";
		for (const char c : word) appendAnsiCEscape(quoted, c);
		quoted.push_back('\'');
		return quoted;
	}

	std::string shellQuoteAlways(const std::string& word) {
		for (const char c : word) {
			if (isControlChar(c)) return ansiCQuote(word);
		}

		return shellQuoteSingle(word);
	}

	std::string shellQuoteForReuse(const std::string& word) {
		if (word.empty()) return "''";

		bool needs_quotes = false;
		for (std::size_t i = 0; i < word.size(); ++i) {
			if (isControlChar(word[i])) return ansiCQuote(word);
			if (isQuotedShellChar(word, i)) needs_quotes = true;
		}

		return needs_quotes ? shellQuoteSingle(word) : word;
	}

	static void appendFields(std::vector<std::string>& into, std::vector<std::string>& fields) {
		for (auto& field : fields) into.push_back(std::move(field));
	}

	static void removeFile(const std::string& path) {
		_wremove(utf8ToFileWide(path).c_str());
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
		return shellStatusFromExitCode(exit_code);
	}

	static std::wstring currentDirectoryWide() {
		const DWORD needed = ::GetCurrentDirectoryW(0, nullptr);
		std::wstring cwd(needed, L'\0');
		const DWORD length = ::GetCurrentDirectoryW(needed, cwd.data());
		cwd.resize(length < needed ? length : 0);
		return cwd;
	}

	static std::wstring shortPathWide(const std::wstring& path) {
		const std::wstring extended = longPathSafe(std::filesystem::path(path)).native();
		const DWORD needed = ::GetShortPathNameW(extended.c_str(), nullptr, 0);
		if (needed == 0) return std::wstring();

		std::wstring shortened(needed, L'\0');
		const DWORD length = ::GetShortPathNameW(extended.c_str(), shortened.data(), needed);
		if (length == 0 || length >= needed) return std::wstring();

		shortened.resize(length);
		return withoutExtendedPrefix(std::filesystem::path(shortened)).native();
	}

	// CreateProcess refuses to start a child in a current directory past
	// MAX_PATH even in a long-path-aware process, but the 8.3 form of a deep
	// directory usually fits; empty means "inherit ours as it is".
	static std::wstring childCurrentDirectory() {
		const std::wstring cwd = currentDirectoryWide();
		if (cwd.size() < MAX_PATH) return std::wstring();

		const std::wstring shortened = shortPathWide(cwd);
		return shortened.size() < MAX_PATH ? shortened : std::wstring();
	}

	static const wchar_t* optionalDirectory(const std::wstring& directory) {
		return directory.empty() ? nullptr : directory.c_str();
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

	static DWORD placementFlags(const SpawnPlacement& placement) {
		DWORD flags = 0;
		if (placement.job != nullptr) flags |= CREATE_SUSPENDED;
		if (placement.new_process_group) flags |= CREATE_NEW_PROCESS_GROUP;
		return flags;
	}

	static void releasePlacedChild(const PROCESS_INFORMATION& process,
	                               const SpawnPlacement& placement) {
		if (placement.job == nullptr) return;

		::AssignProcessToJobObject(placement.job, process.hProcess);
		::ResumeThread(process.hThread);
	}

	// The inherits[] buffer is referenced by attr_list until
	// DeleteProcThreadAttributeList runs, so it must live in this frame
	// (not in a helper) — per the Win32 attribute-list ownership rules.
	static HANDLE spawnWithHandles(SpawnCommand& command, const StdioHandles& stdio,
	                               const SpawnPlacement& placement = SpawnPlacement()) {
		giveBackStdinReadAhead();
		LPPROC_THREAD_ATTRIBUTE_LIST attr_list = allocAttrList();
		if (attr_list == nullptr) return INVALID_HANDLE_VALUE;

		HANDLE inherits[3];
		const DWORD count = collectInheritHandles(inherits, stdio);
		if (!attachInheritList(attr_list, inherits, count)) {
			freeAttrList(attr_list);
			return INVALID_HANDLE_VALUE;
		}

		STARTUPINFOEXW info = makeStartupInfo(stdio, attr_list);
		const std::wstring directory = childCurrentDirectory();
		PROCESS_INFORMATION process{};
		const BOOL created = ::CreateProcessW(command.exe.c_str(), command.cmdline.data(),
			nullptr, nullptr, TRUE,
			EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | placementFlags(placement),
			command.envblock.data(), optionalDirectory(directory), &info.StartupInfo, &process);
		freeAttrList(attr_list);

		if (created == 0) {
			std::fprintf(stderr, "wbsh: CreateProcess failed: %s\n", lastErrorString().c_str());
			return INVALID_HANDLE_VALUE;
		}

		releasePlacedChild(process, placement);
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

	static bool evalIntegerAttribute(void* context, const std::string& expr, long long& out) {
		Expander& expander = *static_cast<Expander*>(context);
		if (expander.failed()) return false;

		out = expander.evalArith(expr);
		if (!expander.failed()) return true;

		if (!expander.fatalErrorPending()) reportExpanderError(expander);
		return false;
	}

	Executor::Executor(Environment& env)
		: env_(env), expander_(env, this) {
		env_.setArithEvaluator(evalIntegerAttribute, &expander_);
		registerCoreBuiltins(*this);
		registerCoreutils(*this);
		loadUtils(*this);
	}

	int Executor::execute(const Node& root) {
		return execNode(root);
	}

	int Executor::execNode(const Node& node) {
		if (env_.noexec() && !env_.interactive()) return 0;
		if (node.loc.line > 0) env_.setCurrentLineno(static_cast<int>(node.loc.line));

		const int status = dispatchNode(node);
		exitOnFatalExpansionError();
		return status;
	}

	// POSIX: a failed `${name?}` or a `set -u` error ends a
	// non-interactive shell; a subshell or `$(...)` ends just itself.
	void Executor::exitOnFatalExpansionError() {
		if (!expander_.takeFatalError()) return;
		if (env_.interactive() || flowPending()) return;

		raiseExit(1);
	}

	int Executor::dispatchNode(const Node& node) {
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
		if (env_.interactive()) std::fprintf(stderr, "[%d] %lld\n", job_id, pid);
		return 0;
	}
#endif

	int Executor::execList(const List& list) {
		int status = 0;
		for (std::size_t index = 0; index < list.items.size(); ++index) {
			const ListItem& item = list.items[index];
			if (&list == input_echo_.list) echoInputBefore(list, index);

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

	// `verbose` and `noexec` stay behind: the child re-reads text the
	// parent has already echoed, and a parent under noexec spawns nothing.
	static bool inheritsSetOption(const SetOptionSpec& spec) {
		return spec.flag != &SetOptions::verbose && spec.flag != &SetOptions::noexec;
	}

	static void appendOptionWord(std::string& words, const char* name, bool on) {
		if (!words.empty()) words.push_back(' ');
		words.push_back(on ? '-' : '+');
		words += name;
	}

	static void serializeChangedSetOptions(const SetOptions& options, std::string& words) {
		const SetOptions defaults;
		for (const SetOptionSpec& spec : setOptionTable()) {
			const bool on = options.*(spec.flag);
			if (inheritsSetOption(spec) && on != defaults.*(spec.flag)) {
				appendOptionWord(words, spec.name, on);
			}
		}
	}

	static void serializeChangedShopts(const ShoptFlags& shopts, std::string& words) {
		const ShoptFlags defaults;
		for (const ShoptSpec& spec : shoptTable()) {
			const bool on = shopts.*(spec.flag);
			if (on != defaults.*(spec.flag)) appendOptionWord(words, spec.name, on);
		}
	}

	// `set -o` and `shopt` names do not overlap, so one list carries both.
	static std::string serializeChangedOptions(const Environment& env) {
		std::string words;
		serializeChangedSetOptions(env.options(), words);
		serializeChangedShopts(env.shopts(), words);
		return words;
	}

	// A pipeline stage is a subshell of this shell: `$?` and `$$` are ours.
	static void addShellParameters(NameValueList& overrides, const Environment& env,
			int last_status) {
		if (last_status != 0) overrides.emplace_back("WBSH_STATUS", std::to_string(last_status));
		overrides.emplace_back("WBSH_SHELL_PID", std::to_string(env.shellPid()));
	}

	// A self-spawned subshell starts with this shell's history and job
	// table, as a forked bash subshell does. History can be long, so it
	// travels in a temp file that the child deletes once it has read it.
	static std::string writeHistorySnapshot(const Executor& exec) {
		if (exec.history().empty()) return {};

		wchar_t dir[MAX_PATH + 1] = {};
		wchar_t name[MAX_PATH + 1] = {};
		if (::GetTempPathW(MAX_PATH + 1, dir) == 0) return {};
		if (::GetTempFileNameW(dir, L"wbh", 0, name) == 0) return {};

		const std::string path = wideToUtf8(name);
		if (exec.saveHistoryToFile(path)) return path;

		::DeleteFileW(name);
		return {};
	}

	static std::string serializeJobs(const std::vector<Executor::Job>& jobs) {
		std::string text;
		for (const Executor::Job& job : jobs) {
			std::string command = job.cmd_text;
			std::replace(command.begin(), command.end(), '\n', ' ');

			text += std::to_string(job.id) + ' ' + std::to_string(job.pid) + ' ';
			text += std::string(job.running ? "1" : "0") + ' ' + std::to_string(job.exit_code);
			text += ' ' + command + '\n';
		}

		return text;
	}

	void Executor::addSubshellState(NameValueList& overrides) {
		const std::string history_file = writeHistorySnapshot(*this);
		if (!history_file.empty()) overrides.emplace_back("WBSH_HISTORY", history_file);

		reapJobs();
		const std::string jobs = serializeJobs(jobs_);
		if (!jobs.empty()) overrides.emplace_back("WBSH_JOBS", jobs);
	}

	static void discardHistorySnapshot(const NameValueList& overrides) {
		for (const auto& entry : overrides) {
			if (entry.first != "WBSH_HISTORY") continue;

			std::error_code ec;
			std::filesystem::remove(utf8ToPath(entry.second), ec);
		}
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
		const std::string attributes = serializeAttributes();
		if (!attributes.empty()) overrides.emplace_back("WBSH_ATTRIBUTES", attributes);
		const std::string bindings = serializeKeyBindings();
		if (!bindings.empty()) overrides.emplace_back("WBSH_BINDINGS", bindings);

		const std::vector<std::string> locals = unexportedNames(env_);
		if (!locals.empty()) overrides.emplace_back("WBSH_LOCAL_NAMES", joinWithSpaces(locals));

		const std::string options = serializeChangedOptions(env_);
		if (!options.empty()) overrides.emplace_back("WBSH_OPTIONS", options);

		addShellParameters(overrides, env_, last_status_);
		addSubshellState(overrides);
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

	// The child gets `$0` and the positionals the way `bash -c cmd name args`
	// would, so `"$1" | cat` sees the same `$1` as the parent.
	std::vector<std::string> Executor::selfSpawnArgv(const std::string& self,
			const std::string& command) const {
		std::vector<std::string> argv = { self, "-r", "-c", command, env_.shellName() };
		const std::vector<std::string>& positional = env_.positional();
		argv.insert(argv.end(), positional.begin(), positional.end());
		return argv;
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
		const std::vector<std::string> argv = selfSpawnArgv(self, slice);

		const NameValueList overrides = buildSelfSpawnOverrides();
		SpawnCommand command;
		command.exe      = utf8ToWide(self);
		command.cmdline  = buildCommandLine(argv);
		command.envblock = buildEnvBlock(env_, overrides, /*include_unexported=*/true);

		const HANDLE process = spawnWithHandles(command, StdioHandles{ h_in, h_out, h_err });
		if (process == INVALID_HANDLE_VALUE) discardHistorySnapshot(overrides);
		return process;
	}

	HANDLE Executor::launchPipelineElement(const Node& elem,
	                                       HANDLE h_in, HANDLE h_out, HANDLE h_err) {
		bool tried_direct = false;
		const HANDLE direct = tryDirectExternalLaunch(elem, h_in, h_out, h_err, &tried_direct);

		// The element runs as a process of its own, so an expansion error
		// met while preparing it here ends only that element.
		expander_.takeFatalError();
		if (tried_direct) return direct;

		return selfSpawnPipelineElement(elem, h_in, h_out, h_err);
	}

	static std::string quotedCommandText(const std::vector<std::string>& argv) {
		std::string text;
		for (const std::string& word : argv) {
			if (!text.empty()) text.push_back(' ');
			text += shellQuoteSingle(word);
		}

		return text;
	}

	bool Executor::runsInsideShell(const std::string& name) const {
		return isFunction(name) || isBuiltin(name) || isAlias(name);
	}

	HANDLE Executor::spawnShellCommand(const std::vector<std::string>& argv,
	                                   const SpawnPlacement& placement) {
		const std::string self = getSelfExecutablePath();
		const std::vector<std::string> child_argv = selfSpawnArgv(self, quotedCommandText(argv));

		const NameValueList overrides = buildSelfSpawnOverrides();
		SpawnCommand command;
		command.exe      = utf8ToWide(self);
		command.cmdline  = buildCommandLine(child_argv);
		command.envblock = buildEnvBlock(env_, overrides, /*include_unexported=*/true);

		const HANDLE process = spawnWithHandles(command, currentStdio(), placement);
		if (process == INVALID_HANDLE_VALUE) discardHistorySnapshot(overrides);
		return process;
	}

	HANDLE Executor::spawnCommand(const std::vector<std::string>& argv,
	                              const SpawnPlacement& placement) {
		if (argv.empty()) return INVALID_HANDLE_VALUE;

		std::fflush(stdout);
		std::fflush(stderr);
		if (runsInsideShell(argv[0])) return spawnShellCommand(argv, placement);

		const std::string exec_path = findExecutable(argv[0]);
		if (exec_path.empty()) {
			reportCommandNotFound(argv[0]);
			return INVALID_HANDLE_VALUE;
		}

		if (looksLikeShellScript(exec_path)) return spawnShellCommand(argv, placement);

		const NameValueList no_assignments;
		SpawnCommand command = makeSpawnCommand(
			prepareExternalArgv(argv, exec_path, no_assignments), exec_path,
			buildEnvBlock(env_, prepareExternalEnvOverrides(no_assignments)));
		return spawnWithHandles(command, currentStdio(), placement);
	}

	// A prefix assignment whose value fails to expand is dropped rather
	// than aborting the launch.
	static NameValueList expandPrefixAssignments(Expander& expander,
	                                             const std::vector<Assignment>& assignments) {
		NameValueList temp_env;
		for (const auto& assignment : assignments) {
			std::string value = expander.expandAssignmentValue(assignment.value);
			if (expander.failed()) {
				reportExpanderError(expander);
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

	static const int kFirstSavedFd = 10;

	// Saved copies live at fd 10 and up, as in bash, so a command's own
	// `>&3` cannot reach one and write past a redirection.
	static int dupAboveUserFds(int fd) {
		std::vector<int> low_copies;
		int copy = _dup(fd);
		while (copy >= 0 && copy < kFirstSavedFd) {
			low_copies.push_back(copy);
			copy = _dup(fd);
		}

		for (const int low : low_copies) _close(low);
		return copy;
	}

	static void beforeRepointing(int target) {
		if (target == 0) resetStdinBuffer();
	}

	// The opened fd can already be `target` when target was closed.
	static void moveFdTo(int fd, int target) {
		if (fd == target) return;

		beforeRepointing(target);
		_dup2(fd, target);
		_close(fd);
	}

	// A backup of -1 records that @p fd was closed, so undoing closes it.
	void Executor::saveFd(RedirState& state, int fd) const {
		state.saved.push_back({ fd, dupAboveUserFds(fd) });
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

	static bool opensForWriting(int flags) {
		return (flags & (_O_WRONLY | _O_RDWR)) != 0;
	}

	int Executor::openRedirSourceFd(const std::string& path, int flags) const {
		int fd = dupSpecialDevFd(path);
		if (fd >= 0) return fd;

		if (isGeneratorDevice(path) && !opensForWriting(flags)) return openGeneratorDevice(path);
		if (isGeneratorDevice(path)) return _wopen(L"NUL", flags | _O_BINARY, _S_IREAD | _S_IWRITE);

		fd = openTcpUdpStream(path);
		if (fd >= 0) return fd;

		const std::wstring win_path = utf8ToFileWide(path_conv_.toWin32(path));
		return _wopen(win_path.c_str(), flags | _O_BINARY, _S_IREAD | _S_IWRITE);
	}

	bool Executor::redirectFdFromPath(const std::string& path, int flags,
	                                  int target, RedirState& state) {
		saveFd(state, target);
		const int fd = openRedirSourceFd(path, flags);
		if (fd < 0) {
			reportPathError(path);
			return false;
		}

		moveFdTo(fd, target);
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

	bool Executor::refusesClobber(const std::string& path) const {
		if (!env_.noclobber()) return false;

		std::error_code ec;
		const std::filesystem::path native = utf8ToPath(path_conv_.toWin32(path));
		if (!std::filesystem::is_regular_file(native, ec)) return false;

		std::fprintf(stderr, "wbsh: %s: cannot overwrite existing file\n", path.c_str());
		return true;
	}

	bool Executor::applyTruncOrClobber(const Redirection& redir, int target, RedirState& state) {
		std::string path;
		if (!expandRedirTarget(redir.target, path)) return false;
		if (redir.op == RedirOp::Great && refusesClobber(path)) return false;

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
		if ((extra_flags & _O_TRUNC) != 0 && refusesClobber(path)) return false;

		saveFd(state, 1);
		saveFd(state, 2);
		const int fd = openRedirSourceFd(path, _O_WRONLY | _O_CREAT | extra_flags);
		if (fd < 0) {
			reportPathError(path);
			return false;
		}

		if (fd != 1) _dup2(fd, 1);
		if (fd != 2) _dup2(fd, 2);
		if (fd != 1 && fd != 2) _close(fd);
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
			beforeRepointing(target);
			_close(target);
			return true;
		}

		int from_fd = 0;
		if (!parseInt(spec, from_fd)) {
			std::fprintf(stderr, "wbsh: %s: bad fd\n", spec.c_str());
			return false;
		}

		// bash treats `N>&N` as a no-op, even when N is closed.
		if (from_fd == target) return true;

		saveFd(state, target);
		beforeRepointing(target);
		if (_dup2(from_fd, target) == 0) return true;

		std::fprintf(stderr, "wbsh: %d: Bad file descriptor\n", from_fd);
		return false;
	}

	bool Executor::installRedirFromTempBody(std::string body, int target, RedirState& state) {
		std::string temp_path = makeTempFile();
		if (temp_path.empty()) return false;
		writeAllText(temp_path, body);

		saveFd(state, target);
		const int fd = openRedirSourceFd(temp_path, _O_RDONLY);
		if (fd < 0) {
			removeFile(temp_path);
			return false;
		}

		moveFdTo(fd, target);
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
		if (!expandRedirTarget(redir.target, body)) {
			// bash fails only the command when a here-string hits `set -u`.
			expander_.takeFatalError();
			return false;
		}

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

	static void restoreSavedFd(int fd, int backup) {
		beforeRepointing(fd);
		if (backup < 0) {
			_close(fd);
			return;
		}

		_dup2(backup, fd);
		_close(backup);
	}

	void Executor::undoRedirections(RedirState& state) {
		std::fflush(stdout);
		std::fflush(stderr);
		for (auto it = state.saved.rbegin(); it != state.saved.rend(); ++it) {
			restoreSavedFd(it->first, it->second);
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
			if (item.has_key) {
				array.sparse = true;
				std::string key = expander.expandStringValue(item.key);
				std::string value = expander.expandAssignmentValue(item.value);
				array.items.push_back({ true, std::move(key), std::move(value) });
			} else {
				for (std::string& field : expander.expandWord(item.value)) {
					array.items.push_back({ false, std::string(), std::move(field) });
				}
			}

			if (expander.failed()) break;
		}

		return array;
	}

	static Executor::ElemAssign expandElemAssign(Expander& expander, const Assignment& assignment) {
		Executor::ElemAssign elem;
		elem.name = assignment.name;
		elem.subscript = assignment.subscript;
		elem.value = expander.expandAssignmentValue(assignment.value);
		elem.append = assignment.append;
		return elem;
	}

	static Executor::ScalarAssign expandScalarAssign(Expander& expander,
	                                                 const Assignment& assignment) {
		Executor::ScalarAssign scalar;
		scalar.name = assignment.name;
		scalar.value = expander.expandAssignmentValue(assignment.value);
		scalar.append = assignment.append;
		return scalar;
	}

	std::string Executor::expandDeclarationArg(const DeclarationArg& arg) {
		if (!arg.compound_text.empty()) return arg.compound_text;

		const Assignment& assignment = arg.assignment;
		std::string field = assignment.name;
		if (assignment.has_subscript) {
			field += "[" + expander_.expandStringValue(assignment.subscript) + "]";
		}

		field += assignment.append ? "+=" : "=";
		return field + expander_.expandAssignmentValue(assignment.value);
	}

	static const DeclarationArg* declarationArgAt(const SimpleCommand& sc, std::size_t word_index,
	                                              std::size_t& cursor) {
		if (cursor >= sc.declaration_args.size()) return nullptr;
		if (sc.declaration_args[cursor].word_index != word_index) return nullptr;

		return &sc.declaration_args[cursor++];
	}

	bool Executor::expandSimpleCmdArgv(const SimpleCommand& sc, std::vector<std::string>& argv,
	                                   std::vector<bool>& compound) {
		std::size_t cursor = 0;
		for (std::size_t i = 0; i < sc.words.size(); ++i) {
			const DeclarationArg* declaration = declarationArgAt(sc, i, cursor);
			std::vector<std::string> fields = declaration != nullptr
				? std::vector<std::string>{ expandDeclarationArg(*declaration) }
				: expander_.expandWord(sc.words[i]);
			if (expander_.failed()) {
				reportExpanderError(expander_);
				return false;
			}

			if (flowPending()) return false;
			appendFields(argv, fields);

			compound.resize(argv.size(), false);
			if (declaration != nullptr && !declaration->compound_text.empty()) compound.back() = true;
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

		keepExecRedirs(state);
		return 0;
	}

	void Executor::keepExecRedirs(const RedirState& state) {
		if (exec_redir_journal_.empty()) {
			for (const auto& saved : state.saved) {
				if (saved.second >= 0) _close(saved.second);
			}

			return;
		}

		RedirState& journal = exec_redir_journal_.back();
		journal.saved.insert(journal.saved.end(), state.saved.begin(), state.saved.end());
		journal.temps.insert(journal.temps.end(), state.temps.begin(), state.temps.end());
	}

	void Executor::endExecRedirJournal() {
		undoRedirections(exec_redir_journal_.back());
		exec_redir_journal_.pop_back();
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
		for (const auto& item : array.items) {
			long long index = next_index;
			if (item.has_key && !expander.tryEvalArith(item.key, index)) index = next_index;
			env.setIndexedElement(array.name, index, item.value);
			next_index = index + 1;
		}
	}

	static void assignSparseIndexedArray(Environment& env, Expander& expander,
	                                     const Executor::ArrayAssign& array) {
		std::map<long long, std::string> sparse;
		long long next_index = 0;
		for (const auto& item : array.items) {
			long long index = next_index;
			if (item.has_key && !expander.tryEvalArith(item.key, index)) index = next_index;
			sparse[index] = item.value;
			next_index = index + 1;
		}

		env.setIndexedArraySparse(array.name, std::move(sparse));
	}

	// Words without a `[key]=` pair up as key and value, as in bash 5.1+.
	static void assignAssocItems(Environment& env, const Executor::ArrayAssign& array) {
		const std::string* pending_key = nullptr;
		for (const auto& item : array.items) {
			if (item.has_key) {
				env.setAssocElement(array.name, item.key, item.value);
			} else if (pending_key == nullptr) {
				pending_key = &item.value;
			} else {
				env.setAssocElement(array.name, *pending_key, item.value);
				pending_key = nullptr;
			}
		}

		if (pending_key != nullptr) env.setAssocElement(array.name, *pending_key, std::string());
	}

	static std::vector<std::string> itemValues(const Executor::ArrayAssign& array) {
		std::vector<std::string> values;
		values.reserve(array.items.size());
		for (const auto& item : array.items) values.push_back(item.value);
		return values;
	}

	static void applyArrayAssignToEnv(Environment& env, Expander& expander,
	                                  const Executor::ArrayAssign& array) {
		if (env.isAssocArray(array.name)) {
			if (!array.append) env.declareAssocArray(array.name);
			assignAssocItems(env, array);
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

		env.setIndexedArrayFromList(array.name, itemValues(array));
	}

	static void applyElemAssignToEnv(Environment& env, Expander& expander,
	                                 const Executor::ElemAssign& elem) {
		std::string subscript = expander.expandStringValue(elem.subscript);
		if (expander.failed()) {
			reportExpanderError(expander);
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

		env.set(scalar.name, env.appendedValue(scalar.name, scalar.value));
	}

	// A prefix assignment is visible to the expansions after it while
	// the assignments expand; the command gets them all afterwards.
	using SavedVars = std::vector<std::pair<std::string, Environment::VarState>>;

	static void setTemporarily(Environment& env, const Executor::ScalarAssign& scalar,
			SavedVars& prior) {
		if (env.isReadonly(scalar.name)) return;

		const std::string target = env.resolveRef(scalar.name);
		prior.emplace_back(target, env.saveVar(target));
		env.set(scalar.name, scalar.value);
	}

	static void restoreTemporaries(Environment& env, SavedVars& prior) {
		for (auto it = prior.rbegin(); it != prior.rend(); ++it) {
			env.restoreVar(it->first, std::move(it->second));
		}
	}

	static void applyAssignNow(Environment& env, Expander& expander,
			Executor::SimpleCmdAssigns& expanded) {
		for (const auto& scalar : expanded.scalar) applyScalarAssignToEnv(env, scalar);
		for (const auto& elem : expanded.elem)     applyElemAssignToEnv(env, expander, elem);
		for (const auto& array : expanded.array)   applyArrayAssignToEnv(env, expander, array);
	}

	static void expandAssignInto(Expander& expander, const Assignment& assignment,
			Executor::SimpleCmdAssigns& out) {
		if (assignment.is_array) {
			out.array.push_back(expandArrayAssign(expander, assignment));
			return;
		}

		if (assignment.has_subscript) {
			out.elem.push_back(expandElemAssign(expander, assignment));
			return;
		}

		out.scalar.push_back(expandScalarAssign(expander, assignment));
	}

	bool Executor::expandOneAssign(const Assignment& assignment, SimpleCmdAssigns& out, bool bare,
			SavedVars& prior) {
		SimpleCmdAssigns one;
		expandAssignInto(expander_, assignment, one);
		if (expander_.failed() || flowPending()) return false;

		if (bare) {
			applyAssignNow(env_, expander_, one);
			return true;
		}

		if (!one.scalar.empty()) setTemporarily(env_, one.scalar.back(), prior);
		for (auto& scalar : one.scalar) out.scalar.push_back(std::move(scalar));
		for (auto& elem : one.elem)     out.elem.push_back(std::move(elem));
		for (auto& array : one.array)   out.array.push_back(std::move(array));
		return true;
	}

	// Like bash, the assignments expand left to right, each seeing the
	// ones before it; a bare assignment (no command) takes effect at once.
	bool Executor::expandSimpleCmdAssigns(const SimpleCommand& sc, SimpleCmdAssigns& out,
			bool bare) {
		SavedVars prior;
		bool ok = true;
		for (const auto& assignment : sc.assignments) {
			ok = expandOneAssign(assignment, out, bare, prior);
			if (!ok) break;
		}

		restoreTemporaries(env_, prior);
		if (expander_.failed()) reportExpanderError(expander_);
		return ok;
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
			std::fputs(shellQuoteForReuse(argv[i]).c_str(), stderr);
		}

		std::fputc('\n', stderr);
		std::fflush(stderr);
	}

	namespace executor_detail {
		struct PriorValue {
			std::string name;
			bool existed;
			std::string value;
			bool exported;
		};

		// Prefix assignments are in the command's environment, so they are
		// exported while it runs (awk's ENVIRON, env, printenv see them).
		struct EnvAssignsGuard {
			Environment& env;
			std::vector<PriorValue> prior;

			EnvAssignsGuard(Environment& owner, const NameValueList& assigns) : env(owner) {
				prior.reserve(assigns.size());
				for (const auto& assign : assigns) {
					const std::string& name = assign.first;
					prior.push_back({ name, env.has(name), env.get(name), env.isExported(name) });
					env.set(name, assign.second);
					env.exportVar(name);
				}
			}

			~EnvAssignsGuard() {
				for (auto it = prior.rbegin(); it != prior.rend(); ++it) {
					if (!it->existed) {
						env.unset(it->name);
						continue;
					}

					env.set(it->name, it->value);
					if (!it->exported) env.unexportVar(it->name);
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
		return cmdsubst_ran_ ? cmdsubst_status_ : 0;
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

	// As POSIX orders it, the words expand before the assignment values.
	int Executor::execSimpleCommand(const SimpleCommand& sc) {
		const std::size_t proc_subst_watermark = expander_.pendingTempFileWatermark();
		cmdsubst_ran_ = false;

		std::vector<std::string> argv;
		std::vector<bool> compound;
		if (!expandSimpleCmdArgv(sc, argv, compound)) return 1;

		SimpleCmdAssigns assigns_data;
		if (!expandSimpleCmdAssigns(sc, assigns_data, argv.empty())) return 1;
		const NameValueList assigns = assigns_data.scalarPairs();
		const std::size_t words_before_alias = argv.size();
		aliasExpandArgvHead(argv);
		if (argv.size() != words_before_alias) compound.clear();

		if (argv.size() == 1 && argv[0] == "exec") return execBareRedirsForExec(sc.redirs);
		if (argv.empty()) return execSimpleCommandNoArgv(sc, assigns_data);

		if (!assigns_data.array.empty() || !assigns_data.elem.empty()) {
			std::fprintf(stderr, "wbsh: array assignments cannot be used as command prefix\n");
			return 1;
		}

		RedirState state;
		if (!applyRedirectionsOrUndo(sc.redirs, state)) return 1;

		traceXtrace(argv);
		compound_args_ = std::move(compound);
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

	static std::filesystem::path physicalPath(const std::filesystem::path& path) {
		std::error_code ec;
		const std::filesystem::path resolved = std::filesystem::canonical(path, ec);
		return ec ? path : resolved;
	}

	bool changeDirectory(Executor& exec, const std::string& target, std::string& err,
	                     bool physical) {
		namespace fs = std::filesystem;
		fs::path win_target = withoutExtendedPrefix(utf8ToPath(exec.pathConv().toWin32(target)));
		if (physical) win_target = physicalPath(win_target);

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

	std::vector<std::string> Executor::findAllExecutables(const std::string& name) {
		std::vector<std::string> found;
		if (isAbsoluteOrRelativePath(name)) {
			const std::string direct = findExecutable(name);
			if (!direct.empty()) found.push_back(direct);
			return found;
		}

		for (const auto& dir : splitPathList(env_.get("PATH"))) {
			if (dir.empty()) continue;

			std::filesystem::path base = utf8ToPath(path_conv_.toWin32(dir));
			base /= utf8ToPath(name);
			const std::string resolved = tryExecutableWithExtensions(base);
			if (!resolved.empty()) found.push_back(resolved);
		}

		return found;
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
		giveBackStdinReadAhead();
		STARTUPINFOW info = inheritedStdioStartupInfo();
		const std::wstring directory = childCurrentDirectory();
		PROCESS_INFORMATION process{};
		const BOOL created = ::CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr, TRUE,
			CREATE_UNICODE_ENVIRONMENT, envblock.data(), optionalDirectory(directory), &info,
			&process);
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
		snap.saved_traps = trap_handlers_;
		snap.saved_lists_inherited_traps = lists_inherited_traps_;
		snap.saved_options = env_.options();

		std::error_code ec;
		snap.saved_cwd = std::filesystem::current_path(ec);
		return snap;
	}

	void Executor::restoreShellScriptScope(ShellScriptScope& snap, bool force_set) {
		// Putting a value back is not an assignment, so allexport must not
		// export it.
		env_.options().allexport = false;
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
		env_.options() = snap.saved_options;
		functions_ = std::move(snap.saved_funcs);
		trap_handlers_ = std::move(snap.saved_traps);
		lists_inherited_traps_ = snap.saved_lists_inherited_traps;

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
		trap_handlers_.clear();
		lists_inherited_traps_ = false;
		beginExecRedirJournal();

		for (const auto& assignment : temp_env) env_.set(assignment.first, assignment.second);
		env_.setShellName(path);
		env_.setPositional(std::vector<std::string>(argv.begin() + 1, argv.end()));

		int status = executeInputText(body, path);
		consumeFlow(FlowSignal::Kind::Exit, &status);
		if (!flowPending()) status = fireExitTrap(status);
		endExecRedirJournal();
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

	Executor::SubshellScope Executor::enterSubshell() {
		SubshellScope snap{ env_ };
		snap.functions = functions_;
		snap.aliases = aliases_;
		snap.traps = trap_handlers_;
		snap.inherited_traps = inherited_traps_;
		snap.lists_inherited_traps = lists_inherited_traps_;
		snap.dir_stack = dir_stack_;
		snap.scope_stack = scope_stack_;
		snap.getopts_subindex = getopts_subindex_;

		std::error_code ec;
		snap.cwd = std::filesystem::current_path(ec);

		resetSubshellTraps();
		env_.reseedRandom();
		beginExecRedirJournal();
		return snap;
	}

	// The parent's traps, EXIT included, belong to the parent and never
	// run in the subshell; only ignored signals stay ignored.
	void Executor::resetSubshellTraps() {
		if (!lists_inherited_traps_) inherited_traps_ = trap_handlers_;
		lists_inherited_traps_ = true;

		for (auto it = trap_handlers_.begin(); it != trap_handlers_.end(); ) {
			if (it->second.empty()) ++it;
			else it = trap_handlers_.erase(it);
		}
	}

	void Executor::leaveSubshell(SubshellScope& snap) {
		endExecRedirJournal();
		env_.restoreFrom(snap.env);
		functions_ = std::move(snap.functions);
		aliases_ = std::move(snap.aliases);
		trap_handlers_ = std::move(snap.traps);
		inherited_traps_ = std::move(snap.inherited_traps);
		lists_inherited_traps_ = snap.lists_inherited_traps;
		dir_stack_ = std::move(snap.dir_stack);
		scope_stack_ = std::move(snap.scope_stack);
		getopts_subindex_ = snap.getopts_subindex;

		std::error_code ec;
		std::filesystem::current_path(snap.cwd, ec);
	}

	// `exit`, `return`, `break` and `continue` all end just the subshell,
	// with the status they carry.
	int Executor::consumeSubshellFlow(int status) {
		if (!flowPending()) return status;

		const int flow_status = flow_.status;
		clearFlow();
		return flow_status;
	}

	int Executor::endSubshell(int status) {
		return fireExitTrap(consumeSubshellFlow(status));
	}

	int Executor::execSubshell(const Subshell& subshell) {
		RedirState state;
		if (!applyRedirectionsOrUndo(subshell.redirs, state)) return 1;

		SubshellScope snap = enterSubshell();
		int status = 0;
		if (subshell.body != nullptr) status = execNode(*subshell.body);
		status = endSubshell(status);
		leaveSubshell(snap);

		undoRedirections(state);
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

	// A nameref loop variable is pointed at each word in turn.
	static void setLoopVariable(Environment& env, const std::string& name,
			const std::string& value) {
		if (env.isNameref(name)) env.setNameref(name, value);
		else env.set(name, value);
	}

	int Executor::runForLoop(const ForClause& clause) {
		int status = 0;
		std::vector<std::string> values;
		if (!expandForWordList(clause, values, &status)) return status;

		for (const auto& value : values) {
			setLoopVariable(env_, clause.var, value);
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
		return fnmatchFull(pat, s, env_.extglob());
	}

	bool Executor::caseItemMatches(const CaseClause::Item& item, const std::string& subject) {
		for (const auto& pattern_word : item.patterns) {
			const std::string pattern = expander_.expandPattern(pattern_word);
			if (expander_.failed()) {
				reportExpanderError(expander_);
				exitOnFatalExpansionError();
				return false;
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
			if (!caseItemMatches(item, subject)) {
				if (flowPending()) break;
				continue;
			}

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
			Executor& exec;
			Expander& expander;
			bool nocasematch;
			bool& invalid_regex;
		};
	}

	using executor_detail::DBracketContext;

	static bool isDBracketPatternOp(const std::string& op) {
		return op == "==" || op == "=" || op == "!=";
	}

	static bool isDBracketStringOp(const std::string& op) {
		return op == "==" || op == "=" || op == "!=" || op == "<" || op == ">";
	}

	static bool isDBracketArithOp(const std::string& op) {
		return op == "-eq" || op == "-ne" || op == "-lt"
		    || op == "-le" || op == "-gt" || op == "-ge";
	}

	// As in bash, `[[ == ]]` patterns are matched as if extglob were on.
	static bool evalDBracketStringOp(const std::string& op, const std::string& lhs,
	                                 const std::string& rhs) {
		if (op == "==" || op == "=") return fnmatchFull(rhs, lhs, true);
		if (op == "!=")              return !fnmatchFull(rhs, lhs, true);
		if (op == "<")               return lhs < rhs;
		if (op == ">")               return lhs > rhs;
		return false;
	}

	static std::vector<std::string> rematchGroups(const std::string& subject,
	                                              const RegexMatch& match) {
		std::vector<std::string> groups;
		for (std::size_t k = 0; k < match.groups(); ++k) groups.push_back(match.str(subject, k));
		return groups;
	}

	// An invalid regex makes the whole `[[ ]]` return 2 and leaves
	// BASH_REMATCH alone; otherwise BASH_REMATCH is rebuilt, empty on
	// no match.
	static bool evalDBracketRegexOp(const std::string& lhs, const std::string& rhs,
	                                const DBracketContext& ctx) {
		RegexOptions options;
		options.syntax = RegexSyntax::Extended;
		options.ignore_case = ctx.nocasematch;
		PosixRegex re;
		if (!re.compile(rhs, options)) {
			ctx.invalid_regex = true;
			return false;
		}

		RegexMatch match;
		const bool matched = re.search(lhs, 0, match);
		ctx.exec.env().setIndexedArrayFromList("BASH_REMATCH",
			matched ? rematchGroups(lhs, match) : std::vector<std::string>());
		return matched;
	}

	// `[[ ]]` evaluates both sides of -eq and friends as arithmetic.
	static bool evalDBracketArithOp(const std::string& op, const std::string& lhs,
	                                const std::string& rhs, Expander& expander) {
		long long left = 0;
		long long right = 0;
		if (!expander.tryEvalArith(lhs, left) || !expander.tryEvalArith(rhs, right)) return false;

		if (op == "-eq") return left == right;
		if (op == "-ne") return left != right;
		if (op == "-lt") return left <  right;
		if (op == "-le") return left <= right;
		if (op == "-gt") return left >  right;
		return left >= right;
	}

	// The `==` / `!=` right side is a pattern in which only unquoted
	// glob characters are wildcards; `[[ x == "a*" ]]` compares literally.
	static std::string expandDBracketRhs(const DBracketCond::Expr& expr, Expander& expander) {
		if (isDBracketPatternOp(expr.op)) return expander.expandPattern(expr.rhs);
		if (expr.op == "=~") return expander.expandRegex(expr.rhs);
		return expander.expandStringValue(expr.rhs);
	}

	static bool evalDBracketBinary(const DBracketCond::Expr& expr, const std::string& lhs,
	                               const DBracketContext& ctx) {
		const std::string rhs = expandDBracketRhs(expr, ctx.expander);
		if (ctx.expander.failed()) return false;

		if (isDBracketStringOp(expr.op)) {
			const std::string cmp_lhs = ctx.nocasematch ? asciiLower(lhs) : lhs;
			const std::string cmp_rhs = ctx.nocasematch ? asciiLower(rhs) : rhs;
			return evalDBracketStringOp(expr.op, cmp_lhs, cmp_rhs);
		}

		if (expr.op == "=~") return evalDBracketRegexOp(lhs, rhs, ctx);
		if (isDBracketArithOp(expr.op)) return evalDBracketArithOp(expr.op, lhs, rhs, ctx.expander);
		if (isFileCompareOperator(expr.op)) return evalFileCompare(ctx.exec, expr.op, lhs, rhs);
		return false;
	}

	static bool evalDBracketPrimary(const DBracketCond::Expr& expr, const DBracketContext& ctx) {
		const std::string lhs = ctx.expander.expandStringValue(expr.lhs);
		if (ctx.expander.failed()) return false;
		if (expr.op.empty()) return !lhs.empty();
		if (isUnaryTestOperator(expr.op)) return evalUnaryTestOperator(ctx.exec, expr.op[1], lhs);

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
			bool invalid_regex = false;
			const DBracketContext ctx{ *this, expander_, env_.nocasematch(), invalid_regex };
			status = evalDBracketExpr(*dc.root, ctx) ? 0 : 1;
			if (invalid_regex) status = 2;
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

		std::vector<ScopeEntry>& top = scope_stack_.back();
		for (auto entry = top.rbegin(); entry != top.rend(); ++entry) {
			env_.restoreVar(entry->name, std::move(entry->saved));
		}

		scope_stack_.pop_back();
	}

	const FunctionDef* Executor::findFunction(const std::string& name) const {
		const auto found = functions_.find(name);
		return found == functions_.end() ? nullptr : found->second;
	}

	bool Executor::unsetFunction(const std::string& name) {
		return functions_.erase(name) != 0;
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

	void Executor::adoptInheritedJob(const Job& job) {
		jobs_.push_back(job);
		if (job.id > next_job_id_) next_job_id_ = job.id;
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
			job.exit_code = shellStatusFromExitCode(exit_code);
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

	int Executor::fireExitTrap(int status) {
		const auto it = trap_handlers_.find("EXIT");
		if (it == trap_handlers_.end()) return status;

		const std::string action = it->second;
		const int final_status = runExitTrapAction(action, status);
		trap_handlers_.erase("EXIT");
		return final_status;
	}

	int Executor::runExitTrapAction(const std::string& action, int status) {
		const bool outer_in_trap = in_exit_trap_;
		const int  outer_status  = exit_trap_status_;
		in_exit_trap_ = true;
		exit_trap_status_ = status;

		clearFlow();
		setLastStatus(status);
		executeText(action, "<EXIT trap>");

		int final_status = status;
		consumeFlow(FlowSignal::Kind::Exit, &final_status);
		clearFlow();

		in_exit_trap_ = outer_in_trap;
		exit_trap_status_ = outer_status;
		return final_status;
	}

	static void eraseHistoryRange(HistoryList& list, std::size_t first, std::size_t count) {
		const auto from = static_cast<std::ptrdiff_t>(first);
		const auto to = static_cast<std::ptrdiff_t>(first + count);
		list.commands.erase(list.commands.begin() + from, list.commands.begin() + to);
		list.statuses.erase(list.statuses.begin() + from, list.statuses.begin() + to);
	}

	static void forgetSavedEntries(std::size_t& saved, std::size_t first, std::size_t count) {
		if (saved <= first) return;

		const std::size_t saved_in_range = saved - first;
		saved -= count < saved_in_range ? count : saved_in_range;
	}

	static void trimHistoryTo(HistoryList& list, std::size_t& saved, long long limit) {
		if (limit < 0 || list.commands.size() <= static_cast<std::size_t>(limit)) return;

		const std::size_t drop = list.commands.size() - static_cast<std::size_t>(limit);
		eraseHistoryRange(list, 0, drop);
		forgetSavedEntries(saved, 0, drop);
	}

	void Executor::addHistoryEntry(std::string line) {
		if (line.empty()) return;

		history_.commands.push_back(std::move(line));
		history_.statuses.push_back(0);
		trimHistoryTo(history_, history_saved_, historyLimit(env_.get("HISTSIZE")));
	}

	void Executor::eraseHistoryEntries(const std::string& line) {
		std::size_t i = 0;
		while (i < history_.commands.size()) {
			if (history_.commands[i] != line) {
				++i;
				continue;
			}

			eraseHistoryRange(history_, i, 1);
			forgetSavedEntries(history_saved_, i, 1);
		}
	}

	void Executor::markLastHistoryStatus(int status) {
		if (history_.statuses.empty()) return;
		history_.statuses.back() = status;
	}

	void Executor::setHistoryEntryStatus(std::size_t index, int status) {
		if (index >= history_.statuses.size()) return;
		history_.statuses[index] = status;
	}

	void Executor::clearHistory() {
		history_.commands.clear();
		history_.statuses.clear();
		history_saved_ = 0;
	}

	bool Executor::loadHistoryFromFile(const std::string& path) {
		HistoryList loaded;
		if (!readHistoryFile(path, loaded)) return false;

		history_.commands.insert(history_.commands.end(), loaded.commands.begin(),
			loaded.commands.end());
		history_.statuses.insert(history_.statuses.end(), loaded.statuses.begin(),
			loaded.statuses.end());
		history_saved_ = history_.commands.size();
		trimHistoryTo(history_, history_saved_, historyLimit(env_.get("HISTSIZE")));
		return true;
	}

	bool Executor::saveHistoryToFile(const std::string& path) const {
		return writeHistoryFile(path, history_, 0);
	}

	bool Executor::writeHistory(const std::string& path) {
		if (!saveHistoryToFile(path)) return false;

		history_saved_ = history_.commands.size();
		return true;
	}

	bool Executor::appendNewHistory(const std::string& path) {
		if (!appendHistoryFile(path, history_, history_saved_)) return false;

		history_saved_ = history_.commands.size();
		return true;
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

	static std::string attributeOptionLetters(const Environment& env, const std::string& name) {
		const unsigned attrs = env.attributes(name);
		std::string letters;
		if ((attrs & kAttrInteger) != 0)   letters += 'i';
		if ((attrs & kAttrLowercase) != 0) letters += 'l';
		if ((attrs & kAttrUppercase) != 0) letters += 'u';
		if (env.isReadonly(name))          letters += 'r';
		return letters;
	}

	std::string Executor::serializeAttributes() const {
		std::set<std::string> names(env_.readonlySet().begin(), env_.readonlySet().end());
		for (const auto& entry : env_.attributeMap()) names.insert(entry.first);

		std::string out;
		for (const std::string& name : names) {
			const std::string letters = attributeOptionLetters(env_, name);
			if (!letters.empty()) out += "declare -" + letters + " " + name + "\n";
		}

		for (const auto& entry : sortedKeys(env_.namerefs())) {
			out += "declare -n " + entry + "=" + shellQuoteSingle(env_.namerefTarget(entry)) + "\n";
		}

		return out;
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

	bool Executor::makeLocal(const std::string& name) {
		if (scope_stack_.empty()) return false;

		std::vector<ScopeEntry>& top = scope_stack_.back();
		if (scopeHasName(top, name)) return true;

		ScopeEntry entry;
		entry.name = name;
		entry.saved = env_.saveVar(name);
		const bool exported = entry.saved.exported;
		top.push_back(std::move(entry));

		env_.restoreVar(name, Environment::VarState{});
		if (exported) env_.exportVar(name);
		return true;
	}

	std::vector<std::string> Executor::localNames() const {
		std::vector<std::string> names;
		if (scope_stack_.empty()) return names;

		for (const ScopeEntry& entry : scope_stack_.back()) names.push_back(entry.name);
		return names;
	}

	bool Executor::assignCompound(const std::string& text) {
		Lexer lexer(text);
		std::vector<Token> tokens = lexer.tokenize();
		Parser parser(std::move(tokens));
		Assignment assignment;
		if (!lexer.errors().empty() || !parser.parseLoneArrayAssignment(assignment)) return false;

		const ArrayAssign array = expandArrayAssign(expander_, assignment);
		if (expander_.failed()) {
			reportExpanderError(expander_);
			return false;
		}

		applyArrayAssignToEnv(env_, expander_, array);
		return true;
	}

	template <typename Error>
	static void reportSourceErrors(const std::string& origin, const std::vector<Error>& errors) {
		for (const auto& error : errors) {
			std::fprintf(stderr, "wbsh: %s:%zu:%zu: %s\n",
				origin.c_str(), error.loc.line, error.loc.column, error.message.c_str());
		}
	}

	Node* Executor::parseText(const std::string& source_text, const std::string& origin) {
		Lexer lexer(source_text);
		std::vector<Token> tokens = lexer.tokenize();
		reportSourceErrors(origin, lexer.errors());

		Parser parser(std::move(tokens), source_text);
		Node* root = parser.parseProgram();
		reportSourceErrors(origin, parser.errors());
		if (root == nullptr) return nullptr;

		owned_arenas_.push_back(parser.takeArena());
		return root;
	}

	int Executor::executeText(const std::string& source_text, const std::string& origin) {
		Node* root = parseText(source_text, origin);
		if (root == nullptr) return 1;

		return execute(*root);
	}

	int Executor::executeInputText(const std::string& source_text, const std::string& origin) {
		Node* root = parseText(source_text, origin);
		if (root == nullptr) return 1;

		return executeInput(*root, source_text);
	}

	int Executor::executeInput(const Node& root, const std::string& source_text) {
		const InputEcho saved = input_echo_;
		const bool is_list = root.kind == Node::Kind::List;
		input_echo_ = { is_list ? static_cast<const List*>(&root) : nullptr, &source_text, 0 };
		if (!is_list) echoInputUpTo(source_text.size());

		const int status = execute(root);
		if (!flowPending()) echoInputUpTo(source_text.size());

		input_echo_ = saved;
		return status;
	}

	static std::size_t lineEndAfter(const std::string& text, std::size_t pos) {
		if (pos >= text.size()) return text.size();

		const std::size_t newline = text.find('\n', pos);
		return newline == std::string::npos ? text.size() : newline + 1;
	}

	static std::size_t lineStartOf(const std::string& text, std::size_t pos) {
		if (pos == 0 || text.empty()) return 0;

		const std::size_t from = (pos < text.size()) ? pos - 1 : text.size() - 1;
		const std::size_t newline = text.rfind('\n', from);
		return newline == std::string::npos ? 0 : newline + 1;
	}

	static bool isBlankOrCommentLine(const std::string& text, std::size_t start, std::size_t end) {
		std::size_t k = start;
		while (k < end && (text[k] == ' ' || text[k] == '\t' || text[k] == '\r')) ++k;
		return k >= end || text[k] == '\n' || text[k] == '#';
	}

	// Comment and blank lines after a command are read together with the
	// next one, so they belong to its echo, not this one's.
	static std::size_t dropTrailingCommentLines(const std::string& text, std::size_t floor,
			std::size_t end) {
		while (end > floor) {
			const std::size_t start = lineStartOf(text, end - 1);
			if (start < floor || !isBlankOrCommentLine(text, start, end)) break;
			end = start;
		}

		return end;
	}

	// The input bash has read by the time it runs item `index`: through the
	// end of the item's last line, heredoc bodies included.
	static std::size_t inputReadThrough(const List& list, std::size_t index,
			const std::string& text) {
		const Node& command = *list.items[index].command;
		const std::size_t last_byte = (command.src_end > command.loc.offset)
			? command.src_end
			: command.loc.offset;
		const std::size_t command_end = lineEndAfter(text, last_byte);

		const bool last = index + 1 >= list.items.size();
		const std::size_t next_line = last
			? text.size()
			: lineStartOf(text, list.items[index + 1].command->loc.offset);

		const std::size_t with_heredocs = dropTrailingCommentLines(text, command_end, next_line);
		return (with_heredocs > command_end) ? with_heredocs : command_end;
	}

	void Executor::echoInputBefore(const List& list, std::size_t index) {
		echoInputUpTo(inputReadThrough(list, index, *input_echo_.text));
	}

	static void writeInputEcho(const std::string& chunk) {
		std::fflush(stdout);
		std::fputs(chunk.c_str(), stderr);
		if (chunk.back() != '\n') std::fputc('\n', stderr);
		std::fflush(stderr);
	}

	void Executor::echoInputUpTo(std::size_t end) {
		InputEcho& echo = input_echo_;
		if (end <= echo.cursor) return;

		if (env_.verbose()) writeInputEcho(echo.text->substr(echo.cursor, end - echo.cursor));
		echo.cursor = end;
	}

	// `$?` takes the status at once, so `a=$(exit 3) b=$?` sets b to 3.
	std::string Executor::run(const std::string& body) {
		int status = 0;
		std::string out = captureSubstitution(body, status);
		if (!flowPending()) {
			cmdsubst_ran_ = true;
			cmdsubst_status_ = status;
			setLastStatus(status);
		}

		// `\r` is stripped along with `\n`: the child's CRT text mode may
		// have written `\r\n`, and a stray CR corrupts captured values.
		while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
		return out;
	}

	int Executor::runCommandSubstitutionBody(const std::string& body) {
		SubshellScope snap = enterSubshell();

		const int status = endSubshell(executeText(body, "<command-substitution>"));
		leaveSubshell(snap);
		return status;
	}

	std::string Executor::runRaw(const std::string& body) {
		int status = 0;
		return captureSubstitution(body, status);
	}

	std::string Executor::captureSubstitution(const std::string& body, int& status) {
		status = 1;
		const std::string capture_path = makeTempFile();
		if (capture_path.empty()) return {};

		const int capture_fd = _open(capture_path.c_str(),
			_O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
		if (capture_fd < 0) {
			removeFile(capture_path);
			return {};
		}

		std::fflush(stdout);
		const int saved_stdout = dupAboveUserFds(1);
		_dup2(capture_fd, 1);
		_close(capture_fd);
		status = runCommandSubstitutionBody(body);
		std::fflush(stdout);
		_dup2(saved_stdout, 1);
		_close(saved_stdout);

		std::string out;
		if (!flowPending()) out = readAllText(capture_path);
		removeFile(capture_path);
		return out;
	}

}  // namespace wbsh

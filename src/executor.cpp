#include "executor.h"

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

	static std::string makeTempFile() {
#ifdef _WIN32
		wchar_t dir[MAX_PATH];
		DWORD n = GetTempPathW(MAX_PATH, dir);
		if (n == 0 || n > MAX_PATH) return {};
		wchar_t path[MAX_PATH];
		if (GetTempFileNameW(dir, L"wbsh", 0, path) == 0) return {};
		return wideToUtf8(path);
#else
		char tmpl[] = "/tmp/wbshXXXXXX";
		int fd = mkstemp(tmpl);
		if (fd < 0) return {};
		::close(fd);
		return std::string(tmpl);
#endif
	}

	static std::string readAllText(const std::string& path) {
		std::ifstream f(utf8ToPath(path), std::ios::binary);
		std::stringstream ss;
		ss << f.rdbuf();
		return ss.str();
	}

#ifdef _WIN32
	static std::wstring quoteArg(const std::wstring& arg) {
		if (arg.empty()) return L"\"\"";
		bool needs = false;
		for (wchar_t c : arg) {
			if (c == L' ' || c == L'\t' || c == L'\n' || c == L'\v' || c == L'"') {
				needs = true; break;
			}
		}

		if (!needs) return arg;
		std::wstring out = L"\"";
		int backslashes = 0;
		for (wchar_t c : arg) {
			if (c == L'\\') {
				++backslashes;
			} else if (c == L'"') {
				out.append(static_cast<std::size_t>(backslashes) * 2 + 1, L'\\');
				out.push_back(L'"');
				backslashes = 0;
			} else {
				out.append(static_cast<std::size_t>(backslashes), L'\\');
				out.push_back(c);
				backslashes = 0;
			}
		}

		out.append(static_cast<std::size_t>(backslashes) * 2, L'\\');
		out.push_back(L'"');
		return out;
	}

	static std::wstring buildCommandLine(const std::vector<std::string>& argv) {
		std::wstring cmd;
		for (std::size_t i = 0; i < argv.size(); ++i) {
			if (i) cmd.push_back(L' ');
			cmd += quoteArg(utf8ToWide(argv[i]));
		}

		return cmd;
	}

	static bool isBatchFile(const std::string& path) {
		auto dot = path.find_last_of('.');
		if (dot == std::string::npos) return false;
		std::string ext = path.substr(dot + 1);
		std::transform(ext.begin(), ext.end(), ext.begin(),
			[](char c) { return static_cast<char>(std::tolower((unsigned char)c)); });
		return ext == "cmd" || ext == "bat";
	}

	static std::wstring cmdExePath() {
		wchar_t buf[MAX_PATH];
		UINT n = GetSystemDirectoryW(buf, MAX_PATH);
		if (n && n < MAX_PATH) {
			std::wstring p(buf, n);
			p += L"\\cmd.exe";
			return p;
		}

		return L"cmd.exe";
	}

	static std::wstring wrapWithCmdExe(const std::wstring& cmdline) {
		std::wstring cmd = cmdExePath();
		std::wstring quoted = (cmd.find(L' ') != std::wstring::npos)
			? L"\"" + cmd + L"\""
			: cmd;
		return quoted + L" /d /s /c \"" + cmdline + L"\"";
	}

	static std::wstring buildEnvBlock(const Environment& env,
	                           const std::vector<std::pair<std::string, std::string>>& overrides,
	                           bool include_unexported = false) {
		std::vector<std::wstring> entries;
		for (const auto& kv : env.vars()) {
			if (!env.isExported(kv.first) && !include_unexported) continue;
			std::string entry = kv.first + "=" + kv.second;
			entries.push_back(utf8ToWide(entry));
		}

		for (const auto& ov : overrides) {
			std::wstring prefix = utf8ToWide(ov.first) + L"=";
			auto it = std::find_if(entries.begin(), entries.end(),
				[&](const std::wstring& e) {
					return e.size() >= prefix.size()
						&& std::equal(prefix.begin(), prefix.end(), e.begin());
				});
			std::wstring entry = prefix + utf8ToWide(ov.second);
			if (it != entries.end()) *it = std::move(entry);
			else entries.push_back(std::move(entry));
		}

		std::sort(entries.begin(), entries.end(),
			[](const std::wstring& a, const std::wstring& b) {
				const std::size_t n = a.size() < b.size() ? a.size() : b.size();
				for (std::size_t i = 0; i < n; ++i) {
					const wchar_t ca = static_cast<wchar_t>(::towlower(a[i]));
					const wchar_t cb = static_cast<wchar_t>(::towlower(b[i]));
					if (ca != cb) return ca < cb;
				}

				return a.size() < b.size();
			});
		std::wstring block;
		for (const auto& e : entries) {
			block += e;
			block.push_back(L'\0');
		}

		block.push_back(L'\0');
		return block;
	}

	static std::string lastErrorString() {
		DWORD err = GetLastError();
		if (err == 0) return {};
		LPSTR buf = nullptr;
		DWORD n = FormatMessageA(
			FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
			| FORMAT_MESSAGE_IGNORE_INSERTS,
			nullptr, err, 0, (LPSTR)&buf, 0, nullptr);
		std::string s = (buf && n) ? std::string(buf, n) : std::string();
		if (buf) LocalFree(buf);
		while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
			s.pop_back();
		return s;
	}

	static std::string getSelfExecutablePath() {
		wchar_t buf[MAX_PATH];
		DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
		if (n == 0 || n >= MAX_PATH) return "wbsh.exe";
		int u = WideCharToMultiByte(CP_UTF8, 0, buf, (int)n,
			nullptr, 0, nullptr, nullptr);
		std::string out(u, '\0');
		WideCharToMultiByte(CP_UTF8, 0, buf, (int)n,
			out.data(), u, nullptr, nullptr);
		return out;
	}

	static bool isMsysBinary(const std::string& p) {
		std::string lower(p.size(), '\0');
		std::transform(p.begin(), p.end(), lower.begin(),
			[](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
		static const char* markers[] = {
			"\\git\\usr\\bin\\",
			"\\git\\mingw32\\bin\\",
			"\\git\\mingw64\\bin\\",
			"\\msys64\\",
			"\\msys2\\",
			"\\cygwin\\",
			"\\cygwin64\\",
			nullptr,
		};
		for (int i = 0; markers[i]; ++i) {
			if (lower.find(markers[i]) != std::string::npos) return true;
		}

		return false;
	}

	static bool noPathConvSet(
		const std::vector<std::pair<std::string, std::string>>& overrides,
		const Environment& env) {
		auto effective = [&](const char* name) -> std::string {
			for (const auto& kv : overrides) {
				if (kv.first == name) return kv.second;
			}

			return env.get(name);
		};
		return !effective("WBSH_NO_PATHCONV").empty()
			|| !effective("MSYS_NO_PATHCONV").empty();
	}

	static LPPROC_THREAD_ATTRIBUTE_LIST allocAttrList() {
		SIZE_T attr_size = 0;
		InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
		auto attr_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
			HeapAlloc(GetProcessHeap(), 0, attr_size));
		if (!attr_list) return nullptr;
		if (!InitializeProcThreadAttributeList(attr_list, 1, 0, &attr_size)) {
			HeapFree(GetProcessHeap(), 0, attr_list);
			return nullptr;
		}

		return attr_list;
	}

	static DWORD collectInheritHandles(HANDLE inherits[3],
	                                   HANDLE h_in, HANDLE h_out, HANDLE h_err) {
		DWORD count = 0;
		auto add_h = [&](HANDLE h) {
			if (h == nullptr || h == INVALID_HANDLE_VALUE) return;
			for (DWORD i = 0; i < count; ++i) if (inherits[i] == h) return;
			inherits[count++] = h;
			SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
		};
		add_h(h_in); add_h(h_out); add_h(h_err);
		return count;
	}

	// The inherits[] buffer is referenced by attr_list until
	// DeleteProcThreadAttributeList runs, so it must live in this frame
	// (not in a helper) — per the Win32 attribute-list ownership rules.
	static HANDLE spawnWithHandles(const std::wstring& exe,
	                        std::wstring& cmdline,
	                        std::wstring& envblock,
	                        HANDLE h_in, HANDLE h_out, HANDLE h_err) {
		LPPROC_THREAD_ATTRIBUTE_LIST attr_list = allocAttrList();
		if (!attr_list) return INVALID_HANDLE_VALUE;

		HANDLE inherits[3];
		const DWORD count = collectInheritHandles(inherits, h_in, h_out, h_err);
		if (!UpdateProcThreadAttribute(attr_list, 0,
			PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
			inherits, count * sizeof(HANDLE),
			nullptr, nullptr)) {
			DeleteProcThreadAttributeList(attr_list);
			HeapFree(GetProcessHeap(), 0, attr_list);
			return INVALID_HANDLE_VALUE;
		}

		STARTUPINFOEXW siex{};
		siex.StartupInfo.cb = sizeof(STARTUPINFOEXW);
		siex.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		siex.StartupInfo.hStdInput  = h_in;
		siex.StartupInfo.hStdOutput = h_out;
		siex.StartupInfo.hStdError  = h_err;
		siex.lpAttributeList = attr_list;

		PROCESS_INFORMATION pi{};
		BOOL ok = CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr, TRUE,
			EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
			envblock.data(), nullptr, &siex.StartupInfo, &pi);

		DeleteProcThreadAttributeList(attr_list);
		HeapFree(GetProcessHeap(), 0, attr_list);

		if (!ok) {
			std::fprintf(stderr, "wbsh: CreateProcess failed: %s\n",
				lastErrorString().c_str());
			return INVALID_HANDLE_VALUE;
		}

		CloseHandle(pi.hThread);
		return pi.hProcess;
	}
#endif  // _WIN32

	Executor::Executor(Environment& env)
		: env_(env), expander_(env, this) {
		registerCoreBuiltins(*this);
		registerCoreutils(*this);
	}

	int Executor::execute(const Node& root) {
		return execNode(root);
	}

	int Executor::execNode(const Node& n) {
		if (n.loc.line > 0) {
			env_.setCurrentLineno(static_cast<int>(n.loc.line));
		}

		switch (n.kind) {
		case Node::Kind::List:          return execList(static_cast<const List&>(n));
		case Node::Kind::AndOr:         return execAndOr(static_cast<const AndOr&>(n));
		case Node::Kind::Pipeline:      return execPipeline(static_cast<const Pipeline&>(n));
		case Node::Kind::SimpleCommand:
			return execSimpleCommand(static_cast<const SimpleCommand&>(n));
		case Node::Kind::BraceGroup:    return execBraceGroup(static_cast<const BraceGroup&>(n));
		case Node::Kind::Subshell:      return execSubshell(static_cast<const Subshell&>(n));
		case Node::Kind::IfClause:      return execIf(static_cast<const IfClause&>(n));
		case Node::Kind::WhileClause:   return execWhile(static_cast<const WhileClause&>(n));
		case Node::Kind::ForClause:     return execFor(static_cast<const ForClause&>(n));
		case Node::Kind::CaseClause:    return execCase(static_cast<const CaseClause&>(n));
		case Node::Kind::FunctionDef:   return execFunctionDef(static_cast<const FunctionDef&>(n));
		case Node::Kind::DBracket:      return execDBracket(static_cast<const DBracketCond&>(n));
		}

		return 0;
	}

	static std::string captureNodeSourceText(const Node& n) {
		if (!n.source_text) return {};
		if (n.src_end <= n.src_start) return {};
		if (n.src_end > n.source_text->size()) return {};
		return n.source_text->substr(n.src_start, n.src_end - n.src_start);
	}

#ifdef _WIN32
	int Executor::launchBackgroundCommand(const Node& cmd) {
		HANDLE in  = reinterpret_cast<HANDLE>(_get_osfhandle(0));
		HANDLE out = reinterpret_cast<HANDLE>(_get_osfhandle(1));
		HANDLE err = reinterpret_cast<HANDLE>(_get_osfhandle(2));
		HANDLE proc = launchPipelineElement(cmd, in, out, err);
		if (proc == INVALID_HANDLE_VALUE) return 1;

		const long long pid = static_cast<long long>(GetProcessId(proc));
		const int jid = registerJob(proc, pid, captureNodeSourceText(cmd));
		env_.setLastBgPid(pid);
		std::fprintf(stderr, "[%d] %lld\n", jid, pid);
		return 0;
	}
#endif

	int Executor::execList(const List& l) {
		int status = 0;
		for (const auto& it : l.items) {
			if (it.background) {
#ifdef _WIN32
				status = launchBackgroundCommand(*it.command);
#else
				status = execNode(*it.command);
#endif
			} else {
				status = execNode(*it.command);
			}

			if (flowPending()) return status;
			setLastStatus(status);

			if (env_.errexit() && status != 0
			    && errexit_suppress_ == 0
			    && !it.background) {
				raiseExit(status);
				return status;
			}
		}

		return status;
	}

	int Executor::execAndOr(const AndOr& a) {
		pushErrexitSuppress();
		const int l = execNode(*a.left);
		popErrexitSuppress();
		if (flowPending()) return l;
		setLastStatus(l);
		if (a.op == AndOr::Op::AndIf) {
			if (l != 0) return l;
		} else {
			if (l == 0) return l;
		}

		const int r = execNode(*a.right);
		if (flowPending()) return r;
		setLastStatus(r);
		return r;
	}

	namespace executor_internal {
		struct BangGuard {
			Executor* e;
			bool active;
			BangGuard(Executor* x, bool a) : e(x), active(a) {
				if (active) e->pushErrexitSuppress();
			}

			~BangGuard() {
				if (active) e->popErrexitSuppress();
			}
		};

		struct PipelineTimeGuard {
			bool active;
			std::chrono::steady_clock::time_point start;
			~PipelineTimeGuard() {
				if (!active) return;
				const auto dur = std::chrono::steady_clock::now() - start;
				const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(dur).count();
				const long long mins = ms / 60000;
				const double secs = (ms % 60000) / 1000.0;
				std::fprintf(stderr, "\nreal\t%lldm%.3fs\n", mins, secs);
			}
		};
	}  // namespace executor_internal

#ifdef _WIN32
	static bool createPipelinePipes(std::size_t count,
	                                std::vector<HANDLE>& pipe_r,
	                                std::vector<HANDLE>& pipe_w) {
		SECURITY_ATTRIBUTES sa{};
		sa.nLength = sizeof(sa);
		sa.bInheritHandle = TRUE;
		pipe_r.assign(count - 1, INVALID_HANDLE_VALUE);
		pipe_w.assign(count - 1, INVALID_HANDLE_VALUE);
		for (std::size_t i = 0; i + 1 < count; ++i) {
			if (!CreatePipe(&pipe_r[i], &pipe_w[i], &sa, 0)) {
				std::fprintf(stderr, "wbsh: pipe creation failed: %s\n",
					lastErrorString().c_str());
				for (std::size_t j = 0; j < i; ++j) {
					CloseHandle(pipe_r[j]);
					CloseHandle(pipe_w[j]);
				}

				pipe_r.clear();
				pipe_w.clear();
				return false;
			}
		}

		return true;
	}

	static int waitPipelineProcessesAndStatus(const std::vector<HANDLE>& processes,
	                                          bool pipefail) {
		std::vector<int> stats(processes.size(), 0);
		for (std::size_t i = 0; i < processes.size(); ++i) {
			WaitForSingleObject(processes[i], INFINITE);
			DWORD ec = 0;
			GetExitCodeProcess(processes[i], &ec);
			stats[i] = static_cast<int>(ec);
			CloseHandle(processes[i]);
		}

		int last = stats.empty() ? 0 : stats.back();
		if (pipefail) {
			for (auto it = stats.rbegin(); it != stats.rend(); ++it) {
				if (*it != 0) { last = *it; break; }
			}
		}

		return last;
	}

	int Executor::execPipelineMultiCmd(const Pipeline& p) {
		const std::size_t n = p.commands.size();

		std::vector<HANDLE> pipe_r;
		std::vector<HANDLE> pipe_w;
		if (!createPipelinePipes(n, pipe_r, pipe_w)) return 1;

		std::fflush(stdout);
		std::fflush(stderr);
		HANDLE std_in  = reinterpret_cast<HANDLE>(_get_osfhandle(0));
		HANDLE std_out = reinterpret_cast<HANDLE>(_get_osfhandle(1));
		HANDLE std_err = reinterpret_cast<HANDLE>(_get_osfhandle(2));

		std::vector<HANDLE> processes;
		processes.reserve(n);
		bool launch_ok = true;
		for (std::size_t i = 0; i < n && launch_ok; ++i) {
			HANDLE h_in  = (i == 0)     ? std_in  : pipe_r[i - 1];
			HANDLE h_out = (i + 1 == n) ? std_out : pipe_w[i];
			HANDLE h_err = std_err;
			if (i + 1 < n && i < p.stderr_to_stdout.size() && p.stderr_to_stdout[i]) {
				h_err = h_out;
			}

			HANDLE proc = launchPipelineElement(*p.commands[i], h_in, h_out, h_err);
			if (proc == INVALID_HANDLE_VALUE) {
				launch_ok = false;
				break;
			}

			processes.push_back(proc);
			if (flowPending()) {
				launch_ok = false;
				break;
			}
		}

		for (HANDLE h : pipe_r) if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
		for (HANDLE h : pipe_w) if (h != INVALID_HANDLE_VALUE) CloseHandle(h);

		const int status = waitPipelineProcessesAndStatus(processes, env_.pipefail());
		if (!launch_ok && processes.size() < n) return 1;
		return status;
	}
#endif  // _WIN32

	int Executor::execPipeline(const Pipeline& p) {
		executor_internal::BangGuard bguard(this, p.bang);
		executor_internal::PipelineTimeGuard tguard{
			p.timed, std::chrono::steady_clock::now() };

		if (p.commands.size() == 1) {
			int r = execNode(*p.commands[0]);
			if (flowPending()) return r;
			if (p.bang) r = (r == 0) ? 1 : 0;
			return r;
		}

#ifdef _WIN32
		int last = execPipelineMultiCmd(p);
		if (p.bang) last = (last == 0) ? 1 : 0;
		return last;
#else
		(void)p;
		return 1;
#endif
	}

#ifdef _WIN32
	HANDLE Executor::tryDirectExternalLaunch(const Node& elem,
	                                         HANDLE h_in, HANDLE h_out, HANDLE h_err,
	                                         bool* tried)
	{
		*tried = false;
		if (elem.kind != Node::Kind::SimpleCommand) return INVALID_HANDLE_VALUE;

		const auto& sc = static_cast<const SimpleCommand&>(elem);
		if (!sc.redirs.empty()) return INVALID_HANDLE_VALUE;

		std::vector<std::string> argv;
		for (const auto& w : sc.words) {
			auto fields = expander_.expandWord(w);
			if (expander_.failed()) {
				std::fprintf(stderr, "wbsh: %s\n", expander_.takeError().c_str());
				*tried = true;
				return INVALID_HANDLE_VALUE;
			}

			for (auto& f : fields) argv.push_back(std::move(f));
		}

		if (argv.empty()) return INVALID_HANDLE_VALUE;
		if (isBuiltin(argv[0]) || isFunction(argv[0]) || isAlias(argv[0]))
			return INVALID_HANDLE_VALUE;

		const std::string exec_path = findExecutable(argv[0]);
		if (exec_path.empty()) {
			std::fprintf(stderr, "wbsh: %s: command not found\n", argv[0].c_str());
			*tried = true;
			return INVALID_HANDLE_VALUE;
		}

		if (looksLikeShellScript(exec_path)) return INVALID_HANDLE_VALUE;

		*tried = true;
		return launchExternalDirect(sc, argv, exec_path, h_in, h_out, h_err);
	}

	static std::string extractNodeSourceSlice(const Node& elem,
	                                          const std::string& fallback) {
		const std::string* src = elem.source_text
			? elem.source_text
			: (fallback.empty() ? nullptr : &fallback);
		if (!src || elem.src_end <= elem.src_start
		    || elem.src_end > src->size()) {
			return {};
		}

		return src->substr(elem.src_start, elem.src_end - elem.src_start);
	}

	std::vector<std::pair<std::string, std::string>>
	Executor::buildSelfSpawnOverrides() {
		std::vector<std::pair<std::string, std::string>> overrides;

		const std::string posix_path = env_.get("PATH");
		if (!posix_path.empty()) {
			overrides.emplace_back("PATH",
				path_conv_.pathListPosixToWin32(posix_path));
		}

		const std::string fns = serializeFunctions();
		if (!fns.empty()) overrides.emplace_back("WBSH_FUNCTIONS", fns);
		const std::string als = serializeAliases();
		if (!als.empty()) overrides.emplace_back("WBSH_ALIASES", als);
		const std::string arrs = serializeArrays();
		if (!arrs.empty()) overrides.emplace_back("WBSH_ARRAYS", arrs);

		std::vector<std::string> locals;
		for (const auto& kv : env_.vars()) {
			if (!env_.isExported(kv.first)) locals.push_back(kv.first);
		}

		std::sort(locals.begin(), locals.end());
		if (!locals.empty()) {
			std::string list;
			for (std::size_t i = 0; i < locals.size(); ++i) {
				if (i) list.push_back(' ');
				list += locals[i];
			}

			overrides.emplace_back("WBSH_LOCAL_NAMES", std::move(list));
		}

		return overrides;
	}

	void Executor::appendHeredocBodiesToSlice(const SimpleCommand& sc,
	                                          std::string& slice) {
		for (const auto& r : sc.redirs) {
			if (r.op != RedirOp::DLess && r.op != RedirOp::DLessDash) continue;
			if (!slice.empty() && slice.back() != '\n') slice.push_back('\n');
			slice += r.heredoc_body;
			std::string delim = expander_.expandStringValue(r.target);
			if (expander_.failed()) {
				expander_.takeError();
				delim.clear();
			}

			slice += delim;
			slice.push_back('\n');
		}
	}

	HANDLE Executor::selfSpawnPipelineElement(const Node& elem,
	                                          HANDLE h_in, HANDLE h_out, HANDLE h_err)
	{
		std::string slice = extractNodeSourceSlice(elem, source_text_);
		if (slice.empty()) {
			std::fprintf(stderr, "wbsh: cannot extract pipeline element source\n");
			return INVALID_HANDLE_VALUE;
		}

		if (elem.kind == Node::Kind::SimpleCommand) {
			appendHeredocBodiesToSlice(static_cast<const SimpleCommand&>(elem),
			                           slice);
		}

		const std::string self = getSelfExecutablePath();
		std::vector<std::string> argv = { self, "-r", "-c", slice };

		std::wstring exe_w     = utf8ToWide(self);
		std::wstring cmdline_w = buildCommandLine(argv);
		auto overrides = buildSelfSpawnOverrides();
		std::wstring env_w = buildEnvBlock(env_, overrides,
			/*include_unexported=*/true);

		return spawnWithHandles(exe_w, cmdline_w, env_w, h_in, h_out, h_err);
	}

	HANDLE Executor::launchPipelineElement(const Node& elem,
	                                       HANDLE h_in, HANDLE h_out, HANDLE h_err)
	{
		bool tried_direct = false;
		HANDLE direct = tryDirectExternalLaunch(elem, h_in, h_out, h_err, &tried_direct);
		if (tried_direct) return direct;

		return selfSpawnPipelineElement(elem, h_in, h_out, h_err);
	}

	HANDLE Executor::launchExternalDirect(const SimpleCommand& sc,
	                                       const std::vector<std::string>& argv,
	                                       const std::string& exec_path,
	                                       HANDLE h_in, HANDLE h_out, HANDLE h_err) {
		std::vector<std::pair<std::string, std::string>> temp_env;
		for (const auto& as : sc.assignments) {
			std::string val = expander_.expandStringValue(as.value);
			if (expander_.failed()) {
				expander_.takeError();
				continue;
			}

			temp_env.emplace_back(as.name, std::move(val));
		}

		std::vector<std::string> a = prepareExternalArgv(argv, exec_path, temp_env);
		std::vector<std::pair<std::string, std::string>> overrides =
			prepareExternalEnvOverrides(temp_env);

		std::wstring exe_w     = utf8ToWide(exec_path);
		std::wstring cmdline_w = buildCommandLine(a);
		std::wstring env_w     = buildEnvBlock(env_, overrides);
		if (isBatchFile(exec_path)) {
			cmdline_w = wrapWithCmdExe(cmdline_w);
			exe_w     = cmdExePath();
		}

		return spawnWithHandles(exe_w, cmdline_w, env_w, h_in, h_out, h_err);
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

	void Executor::saveFd(RedirState& s, int fd) const {
		const int backup = _dup(fd);
		s.saved.push_back({ fd, backup });
	}

	int Executor::dupSpecialDevFd(const std::string& path) {
		if (path == "/dev/stdin")  return _dup(0);
		if (path == "/dev/stdout") return _dup(1);
		if (path == "/dev/stderr") return _dup(2);
		const std::string fd_pfx = "/dev/fd/";
		if (path.size() > fd_pfx.size()
		    && path.compare(0, fd_pfx.size(), fd_pfx) == 0)
		{
			int n = 0;
			if (!parseInt(path.substr(fd_pfx.size()), n)) return -1;
			return _dup(n);
		}

		return -1;
	}

	int Executor::openTcpUdpStream(const std::string& path) {
		const std::string tcp_pfx = "/dev/tcp/";
		const std::string udp_pfx = "/dev/udp/";
		const bool is_tcp = path.compare(0, tcp_pfx.size(), tcp_pfx) == 0
		                    && path.size() > tcp_pfx.size();
		const bool is_udp = path.compare(0, udp_pfx.size(), udp_pfx) == 0
		                    && path.size() > udp_pfx.size();
		if (!is_tcp && !is_udp) return -1;

		const std::string rest = path.substr(is_udp ? udp_pfx.size() : tcp_pfx.size());
		const auto sl = rest.rfind('/');
		if (sl == std::string::npos) return -1;
		const std::string host = rest.substr(0, sl);
		const std::string port = rest.substr(sl + 1);

		static bool wsa_inited = false;
		if (!wsa_inited) {
			WSADATA wd;
			if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return -1;
			wsa_inited = true;
		}

		addrinfo hints{};
		addrinfo* res = nullptr;
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = is_udp ? SOCK_DGRAM : SOCK_STREAM;
		if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res)
			return -1;

		SOCKET sk = INVALID_SOCKET;
		for (addrinfo* p = res; p; p = p->ai_next) {
			sk = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
			if (sk == INVALID_SOCKET) continue;
			if (connect(sk, p->ai_addr, static_cast<int>(p->ai_addrlen)) == 0) break;
			closesocket(sk);
			sk = INVALID_SOCKET;
		}

		freeaddrinfo(res);
		if (sk == INVALID_SOCKET) return -1;

		const int fd = _open_osfhandle(static_cast<intptr_t>(sk), _O_BINARY);
		if (fd < 0) { closesocket(sk); return -1; }
		return fd;
	}

	int Executor::openRedirSourceFd(const std::string& path, int flags) const {
		int fd = dupSpecialDevFd(path);
		if (fd >= 0) return fd;
		fd = openTcpUdpStream(path);
		if (fd >= 0) return fd;
		const std::wstring wp = utf8ToWide(path_conv_.toWin32(path));
		return _wopen(wp.c_str(), flags | _O_BINARY, _S_IREAD | _S_IWRITE);
	}

	bool Executor::redirectFdFromPath(const std::string& path, int flags,
	                                  int target, RedirState& s) {
		const int fd = openRedirSourceFd(path, flags);
		if (fd < 0) {
			std::fprintf(stderr, "wbsh: %s: %s\n", path.c_str(), std::strerror(errno));
			return false;
		}

		saveFd(s, target);
		_dup2(fd, target);
		_close(fd);
		return true;
	}

	bool Executor::expandRedirTarget(const Word& target, std::string& out) {
		out = expander_.expandStringValue(target);
		if (expander_.failed()) {
			std::fprintf(stderr, "wbsh: %s\n", expander_.takeError().c_str());
			return false;
		}

		return true;
	}

	bool Executor::applyLessRedir(const Redirection& r, int target, RedirState& s) {
		std::string path;
		if (!expandRedirTarget(r.target, path)) return false;
		return redirectFdFromPath(path, _O_RDONLY, target, s);
	}

	bool Executor::applyTruncOrClobber(const Redirection& r, int target, RedirState& s) {
		std::string path;
		if (!expandRedirTarget(r.target, path)) return false;
		return redirectFdFromPath(path, _O_WRONLY | _O_CREAT | _O_TRUNC, target, s);
	}

	bool Executor::applyAppendRedir(const Redirection& r, int target, RedirState& s) {
		std::string path;
		if (!expandRedirTarget(r.target, path)) return false;
		return redirectFdFromPath(path, _O_WRONLY | _O_CREAT | _O_APPEND, target, s);
	}

	bool Executor::applyAmpRedir(const Redirection& r, int extra_flags, RedirState& s) {
		std::string path;
		if (!expandRedirTarget(r.target, path)) return false;
		const int fd = openRedirSourceFd(path, _O_WRONLY | _O_CREAT | extra_flags);
		if (fd < 0) {
			std::fprintf(stderr, "wbsh: %s: %s\n", path.c_str(), std::strerror(errno));
			return false;
		}

		saveFd(s, 1); _dup2(fd, 1);
		saveFd(s, 2); _dup2(fd, 2);
		_close(fd);
		return true;
	}

	bool Executor::applyLessGreatRedir(const Redirection& r, int target, RedirState& s) {
		std::string path;
		if (!expandRedirTarget(r.target, path)) return false;
		return redirectFdFromPath(path, _O_RDWR | _O_CREAT, target, s);
	}

	bool Executor::applyDupRedir(const Redirection& r, int target, RedirState& s) {
		std::string what;
		if (!expandRedirTarget(r.target, what)) return false;
		if (what == "-") {
			saveFd(s, target);
			_close(target);
			return true;
		}

		int from_fd = 0;
		if (!parseInt(what, from_fd)) {
			std::fprintf(stderr, "wbsh: %s: bad fd\n", what.c_str());
			return false;
		}

		saveFd(s, target);
		_dup2(from_fd, target);
		return true;
	}

	bool Executor::installRedirFromTempBody(std::string body, int target, RedirState& s) {
		std::string tmp = makeTempFile();
		if (tmp.empty()) return false;
		{
			std::ofstream f(utf8ToPath(tmp), std::ios::binary | std::ios::trunc);
			f.write(body.data(), body.size());
		}

		const int fd = openRedirSourceFd(tmp, _O_RDONLY);
		if (fd < 0) {
			_wremove(utf8ToWide(tmp).c_str());
			return false;
		}

		saveFd(s, target);
		_dup2(fd, target);
		_close(fd);
		s.temps.push_back(std::move(tmp));
		return true;
	}

	bool Executor::applyHeredocRedir(const Redirection& r, int target, RedirState& s) {
		std::string body = expander_.expandHeredoc(r.heredoc_body, r.heredoc_quoted);
		if (expander_.failed()) {
			std::fprintf(stderr, "wbsh: %s\n", expander_.takeError().c_str());
			return false;
		}

		return installRedirFromTempBody(std::move(body), target, s);
	}

	bool Executor::applyHerestringRedir(const Redirection& r, int target, RedirState& s) {
		std::string body;
		if (!expandRedirTarget(r.target, body)) return false;
		body.push_back('\n');
		return installRedirFromTempBody(std::move(body), target, s);
	}

	bool Executor::applyRedirections(const std::vector<Redirection>& rs, RedirState& s) {
		for (const auto& r : rs) {
			const int target = (r.fd != -1) ? r.fd : defaultRedirTargetFd(r.op);
			std::fflush(stdout);
			std::fflush(stderr);

			bool ok = true;
			switch (r.op) {
			case RedirOp::Less:        ok = applyLessRedir       (r, target, s); break;
			case RedirOp::Great:
			case RedirOp::Clobber:     ok = applyTruncOrClobber  (r, target, s); break;
			case RedirOp::DGreat:      ok = applyAppendRedir     (r, target, s); break;
			case RedirOp::AmpGreat:    ok = applyAmpRedir(r, _O_TRUNC, s);        break;
			case RedirOp::AmpDGreat:   ok = applyAmpRedir(r, _O_APPEND, s);       break;
			case RedirOp::LessGreat:   ok = applyLessGreatRedir  (r, target, s); break;
			case RedirOp::LessAnd:
			case RedirOp::GreatAnd:    ok = applyDupRedir        (r, target, s); break;
			case RedirOp::DLess:
			case RedirOp::DLessDash:   ok = applyHeredocRedir    (r, target, s); break;
			case RedirOp::TLess:       ok = applyHerestringRedir (r, target, s); break;
			}

			if (!ok) return false;
			if (flowPending()) return false;
		}

		return true;
	}

	void Executor::undoRedirections(RedirState& s) {
		std::fflush(stdout);
		std::fflush(stderr);
		for (auto it = s.saved.rbegin(); it != s.saved.rend(); ++it) {
			_dup2(it->second, it->first);
			_close(it->second);
		}

		s.saved.clear();
		for (const auto& tmp : s.temps) _wremove(utf8ToWide(tmp).c_str());
		s.temps.clear();
	}

	bool Executor::expandSimpleCmdAssigns(const SimpleCommand& sc, SimpleCmdAssigns& out) {
		for (const auto& a : sc.assignments) {
			if (a.is_array) {
				ArrayAssign aa;
				aa.name = a.name;
				aa.append = a.append;
				for (const auto& k : a.keyed_items) {
					std::string val = expander_.expandStringValue(k.value);
					if (k.has_key) {
						aa.sparse = true;
						std::string key = expander_.expandStringValue(k.key);
						aa.keyed.emplace_back(std::move(key), std::move(val));
					} else {
						auto fields = expander_.expandWord(k.value);
						for (auto& f : fields) aa.items.push_back(std::move(f));
					}

					if (expander_.failed()) break;
				}

				out.array.push_back(std::move(aa));
			} else if (a.has_subscript) {
				ElemAssign ea;
				ea.name = a.name;
				ea.subscript = a.subscript;
				ea.value = expander_.expandStringValue(a.value);
				ea.append = a.append;
				out.elem.push_back(std::move(ea));
			} else {
				ScalarAssign sa;
				sa.name = a.name;
				sa.value = expander_.expandStringValue(a.value);
				sa.append = a.append;
				out.scalar.push_back(std::move(sa));
			}

			if (expander_.failed()) {
				std::fprintf(stderr, "wbsh: %s\n", expander_.takeError().c_str());
				return false;
			}

			if (flowPending()) return false;
		}

		return true;
	}

	bool Executor::expandSimpleCmdArgv(const SimpleCommand& sc, std::vector<std::string>& argv) {
		for (const auto& w : sc.words) {
			auto fields = expander_.expandWord(w);
			if (expander_.failed()) {
				std::fprintf(stderr, "wbsh: %s\n", expander_.takeError().c_str());
				return false;
			}

			if (flowPending()) return false;
			for (auto& f : fields) argv.push_back(std::move(f));
		}

		return true;
	}

	void Executor::aliasExpandArgvHead(std::vector<std::string>& argv) {
		if (argv.empty() || !env_.expand_aliases()) return;
		std::unordered_set<std::string> seen;
		while (isAlias(argv[0]) && seen.insert(argv[0]).second) {
			const std::string val = aliasValue(argv[0]);
			Lexer alex(val);
			auto atokens = alex.tokenize();
			std::vector<std::string> repl;
			for (auto& t : atokens) {
				if (t.kind != TokKind::Word) continue;
				Word w;
				// Tokens are local and visited once — steal instead of
				// deep-copying the segment list.
				w.segments = std::move(t.segments);
				w.raw = std::move(t.text);
				auto fields = expander_.expandWord(w);
				if (expander_.failed()) {
					expander_.takeError();
					repl.push_back(w.raw);   // t.text was moved into w.raw
					continue;
				}

				for (auto& f : fields) repl.push_back(std::move(f));
			}

			if (repl.empty()) break;
			argv.erase(argv.begin());
			argv.insert(argv.begin(), repl.begin(), repl.end());
		}
	}

	int Executor::execBareRedirsForExec(const std::vector<Redirection>& redirs) {
		RedirState rs;
		if (!applyRedirections(redirs, rs)) {
			undoRedirections(rs);
			return 1;
		}

		for (auto& kv : rs.saved) _close(kv.second);
		return 0;
	}

	static long long nextIndexedAppendSlot(const Environment& env,
	                                       const std::string& name) {
		const auto* ia = env.getIndexedArray(name);
		if (!ia || ia->empty()) return 0;
		return ia->rbegin()->first + 1;
	}

	static std::string readElementValue(const Environment& env,
	                                    const std::string& name,
	                                    long long idx,
	                                    const std::string& key,
	                                    bool is_assoc) {
		if (is_assoc) {
			if (const auto* aa = env.getAssocArray(name)) {
				auto it = aa->find(key);
				if (it != aa->end()) return it->second;
			}

			return {};
		}

		if (const auto* ia = env.getIndexedArray(name)) {
			auto it = ia->find(idx);
			if (it != ia->end()) return it->second;
		}

		return {};
	}

	static void applyArrayAssignToEnv(Environment& env, Expander& exp,
	                                  const Executor::ArrayAssign& aa) {
		if (env.isAssocArray(aa.name)) {
			if (!aa.append) env.declareAssocArray(aa.name);
			for (const auto& kv : aa.keyed)
				env.setAssocElement(aa.name, kv.first, kv.second);
			return;
		}

		if (aa.append) {
			long long next_idx = nextIndexedAppendSlot(env, aa.name);
			for (const auto& k : aa.keyed) {
				long long idx = 0;
				if (!exp.tryEvalArith(k.first, idx)) idx = next_idx;
				env.setIndexedElement(aa.name, idx, k.second);
				next_idx = idx + 1;
			}

			for (const auto& v : aa.items) {
				env.setIndexedElement(aa.name, next_idx++, v);
			}

			return;
		}

		if (aa.sparse) {
			std::map<long long, std::string> sparse;
			long long next_idx = 0;
			for (const auto& k : aa.keyed) {
				long long idx = 0;
				if (!exp.tryEvalArith(k.first, idx)) idx = next_idx;
				sparse[idx] = k.second;
				next_idx = idx + 1;
			}

			for (auto& v : aa.items) sparse[next_idx++] = v;
			env.setIndexedArraySparse(aa.name, std::move(sparse));
			return;
		}

		env.setIndexedArrayFromList(aa.name, aa.items);
	}

	static void applyElemAssignToEnv(Environment& env, Expander& exp,
	                                 const Executor::ElemAssign& ea) {
		std::string sub_str = exp.expandStringValue(ea.subscript);
		if (exp.failed()) {
			exp.takeError();
			sub_str.clear();
		}

		const bool is_assoc = env.isAssocArray(ea.name);
		long long idx = 0;
		if (!is_assoc) {
			if (!exp.tryEvalArith(sub_str, idx)) idx = 0;
		}

		std::string final_val = ea.value;
		if (ea.append) {
			final_val = readElementValue(env, ea.name, idx, sub_str, is_assoc)
				+ final_val;
		}

		if (is_assoc) {
			env.setAssocElement(ea.name, std::move(sub_str), std::move(final_val));
		} else {
			env.setIndexedElement(ea.name, idx, std::move(final_val));
		}
	}

	static void applyScalarAssignToEnv(Environment& env,
	                                   const Executor::ScalarAssign& s) {
		if (!s.append) {
			env.set(s.name, s.value);
			return;
		}

		if (env.isIndexedArray(s.name)) {
			std::string cur;
			if (const auto* ia = env.getIndexedArray(s.name)) {
				auto it = ia->find(0);
				if (it != ia->end()) cur = it->second;
			}

			env.setIndexedElement(s.name, 0, cur + s.value);
			return;
		}

		env.set(s.name, env.get(s.name) + s.value);
	}

	void Executor::applyBareAssignmentsToShell(const SimpleCmdAssigns& a) {
		for (const auto& s : a.scalar) applyScalarAssignToEnv(env_, s);
		for (const auto& e : a.elem)   applyElemAssignToEnv(env_, expander_, e);
		for (const auto& aa : a.array) applyArrayAssignToEnv(env_, expander_, aa);
	}

	void Executor::traceXtrace(const std::vector<std::string>& argv) {
		if (!env_.xtrace() || argv.empty()) return;
		std::string ps4 = env_.get("PS4");
		if (ps4.empty()) ps4 = "+ ";
		std::fputs(ps4.c_str(), stderr);
		for (std::size_t i = 0; i < argv.size(); ++i) {
			if (i) std::fputc(' ', stderr);
			std::fputs(argv[i].c_str(), stderr);
		}

		std::fputc('\n', stderr);
		std::fflush(stderr);
	}

	namespace executor_internal {
		struct EnvAssignsGuard {
			Environment& env;
			std::vector<std::tuple<std::string, bool, std::string>> prior;

			EnvAssignsGuard(Environment& e,
			                const std::vector<std::pair<std::string, std::string>>& assigns)
				: env(e)
			{
				prior.reserve(assigns.size());
				for (const auto& a : assigns) {
					prior.emplace_back(a.first, env.has(a.first), env.get(a.first));
					env.set(a.first, a.second);
				}
			}

			~EnvAssignsGuard() {
				for (auto it = prior.rbegin(); it != prior.rend(); ++it) {
					const auto& name = std::get<0>(*it);
					if (std::get<1>(*it)) env.set(name, std::get<2>(*it));
					else                  env.unset(name);
				}
			}
		};
	}  // namespace executor_internal

	using NameValueList = std::vector<std::pair<std::string, std::string>>;

	static int callFunctionWithAssigns(Executor& exec,
	                                   const std::string& cmd,
	                                   const std::vector<std::string>& args,
	                                   const NameValueList& assigns)
	{
		executor_internal::EnvAssignsGuard guard(exec.env(), assigns);
		return exec.callFunction(cmd, args);
	}

	static int callBuiltinWithAssigns(Executor& exec,
	                                  const std::string& cmd,
	                                  const std::vector<std::string>& args,
	                                  const NameValueList& assigns)
	{
		executor_internal::EnvAssignsGuard guard(exec.env(), assigns);
		return exec.callBuiltin(cmd, args);
	}

	int Executor::execSimpleCommandNoArgv(const SimpleCommand& sc,
	                                      SimpleCmdAssigns& assigns_data) {
		RedirState rs;
		if (!applyRedirections(sc.redirs, rs)) {
			undoRedirections(rs);
			return 1;
		}

		applyBareAssignmentsToShell(assigns_data);
		undoRedirections(rs);
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
		const std::vector<std::pair<std::string, std::string>> assigns =
			assigns_data.scalarPairs();

		std::vector<std::string> argv;
		if (!expandSimpleCmdArgv(sc, argv)) return 1;

		aliasExpandArgvHead(argv);

		if (!argv.empty() && argv[0] == "exec" && argv.size() == 1) {
			return execBareRedirsForExec(sc.redirs);
		}

		if (argv.empty()) return execSimpleCommandNoArgv(sc, assigns_data);

		if (!assigns_data.array.empty() || !assigns_data.elem.empty()) {
			std::fprintf(stderr,
				"wbsh: array assignments cannot be used as command prefix\n");
			return 1;
		}

		RedirState rs;
		if (!applyRedirections(sc.redirs, rs)) {
			undoRedirections(rs);
			return 1;
		}

		traceXtrace(argv);

		const int status = runResolvedCommand(argv, assigns);
		undoRedirections(rs);
		if (flowPending()) return status;

		for (const auto& tf : expander_.drainTempFilesSince(proc_subst_watermark)) {
			std::remove(tf.c_str());
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

	static std::string tryExecutableWithExtensions(const std::filesystem::path& base) {
#ifdef _WIN32
		static const char* exts[] = { ".exe", ".cmd", ".bat", "", nullptr };
		for (int i = 0; exts[i]; ++i) {
			std::filesystem::path q = base;
			if (exts[i][0]) q += exts[i];
			std::error_code ec;
			if (std::filesystem::exists(q, ec) && !std::filesystem::is_directory(q, ec))
				return pathToUtf8(q);
		}

		return {};
#else
		std::error_code ec;
		if (std::filesystem::exists(base, ec) && !std::filesystem::is_directory(base, ec))
			return base.string();
		return {};
#endif
	}

	std::vector<std::string> splitPathList(const std::string& path) {
		std::vector<std::string> dirs;
		std::string cur;
		for (std::size_t i = 0; i < path.size(); ++i) {
			const char c = path[i];
			if (c == ';') {
				dirs.push_back(cur);
				cur.clear();
				continue;
			}

			if (c == ':') {
				const bool drive_letter =
					cur.size() == 1
					&& std::isalpha(static_cast<unsigned char>(cur[0]));
				if (drive_letter) {
					cur.push_back(c);
				} else {
					dirs.push_back(cur);
					cur.clear();
				}
				continue;
			}

			cur.push_back(c);
		}

		if (!cur.empty()) dirs.push_back(cur);
		return dirs;
	}

	std::string Executor::findExecutable(const std::string& name) {
		namespace fs = std::filesystem;
		if (isAbsoluteOrRelativePath(name)) {
			const fs::path p = utf8ToPath(path_conv_.toWin32(name));
			return tryExecutableWithExtensions(p);
		}

		const std::string path = env_.get("PATH");
		if (path.empty()) return {};
		const std::vector<std::string> dirs = splitPathList(path);

		for (const auto& d : dirs) {
			if (d.empty()) continue;
			fs::path base = utf8ToPath(path_conv_.toWin32(d));
			base /= utf8ToPath(name);
			std::string r = tryExecutableWithExtensions(base);
			if (!r.empty()) return r;
		}

		return {};
	}

	int Executor::runExternal(const std::vector<std::string>& argv,
	                          const std::vector<std::pair<std::string, std::string>>& temp_env) {
		if (argv.empty()) return 0;
		std::string exec_path = findExecutable(argv[0]);
		if (exec_path.empty()) {
			std::fprintf(stderr, "wbsh: %s: command not found\n", argv[0].c_str());
			return 127;
		}

		if (looksLikeShellScript(exec_path)) {
			return runShellScript(exec_path, argv, temp_env);
		}

#ifdef _WIN32
		std::vector<std::string> a = prepareExternalArgv(argv, exec_path, temp_env);
		std::vector<std::pair<std::string, std::string>> child_env =
			prepareExternalEnvOverrides(temp_env);

		std::wstring cmdline = buildCommandLine(a);
		std::wstring exe     = utf8ToWide(exec_path);
		std::wstring envblock = buildEnvBlock(env_, child_env);
		if (isBatchFile(exec_path)) {
			cmdline = wrapWithCmdExe(cmdline);
			exe     = cmdExePath();
		}

		std::fflush(stdout);
		std::fflush(stderr);

		int spawn_status = 0;
		const bool ok = spawnExternalAndWait(exe, cmdline, envblock, &spawn_status);
		if (ok) return spawn_status;

		if (GetLastError() == ERROR_BAD_EXE_FORMAT) {
			return runShellScript(exec_path, argv, temp_env);
		}

		std::fprintf(stderr, "wbsh: %s: %s\n",
			argv[0].c_str(), lastErrorString().c_str());
		return 127;
#else
		(void)temp_env;
		return 127;
#endif
	}

#ifdef _WIN32
	std::vector<std::string>
	Executor::prepareExternalArgv(
		const std::vector<std::string>& argv,
		const std::string& exec_path,
		const std::vector<std::pair<std::string, std::string>>& temp_env) {
		const bool translate =
			!isMsysBinary(exec_path)
			&& !noPathConvSet(temp_env, env_);

		std::vector<std::string> a = argv;
		a[0] = exec_path;
		if (!translate) return a;
		for (std::size_t i = 1; i < a.size(); ++i) {
			a[i] = path_conv_.translateArg(a[i]);
		}

		return a;
	}

	std::vector<std::pair<std::string, std::string>>
	Executor::prepareExternalEnvOverrides(
		const std::vector<std::pair<std::string, std::string>>& temp_env)
	{
		std::vector<std::pair<std::string, std::string>> out = temp_env;

		bool have_path = false;
		bool have_home = false;
		for (auto& kv : out) {
			if (kv.first == "PATH") {
				kv.second = path_conv_.pathListPosixToWin32(kv.second);
				have_path = true;
			} else if (kv.first == "HOME") {
				have_home = true;
			}
		}

		if (!have_path) {
			const std::string p = env_.get("PATH");
			if (!p.empty()) {
				out.emplace_back("PATH", path_conv_.pathListPosixToWin32(p));
			}
		}

		if (!have_home) {
			const std::string h = env_.get("HOME");
			if (!h.empty()) out.emplace_back("HOME", path_conv_.toWin32Short(h));
		}

		return out;
	}

	bool Executor::spawnExternalAndWait(const std::wstring& exe,
	                                    std::wstring& cmdline,
	                                    std::wstring& envblock,
	                                    int* exit_status) {
		STARTUPINFOW si{};
		si.cb = sizeof(si);
		si.dwFlags = STARTF_USESTDHANDLES;
		si.hStdInput  = reinterpret_cast<HANDLE>(_get_osfhandle(0));
		si.hStdOutput = reinterpret_cast<HANDLE>(_get_osfhandle(1));
		si.hStdError  = reinterpret_cast<HANDLE>(_get_osfhandle(2));
		HANDLE hs[3] = { si.hStdInput, si.hStdOutput, si.hStdError };
		for (HANDLE h : hs) {
			if (h && h != INVALID_HANDLE_VALUE)
				SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
		}

		PROCESS_INFORMATION pi{};
		const BOOL ok = CreateProcessW(
			exe.c_str(),
			cmdline.data(),
			nullptr, nullptr,
			TRUE,
			CREATE_UNICODE_ENVIRONMENT,
			envblock.data(),
			nullptr,
			&si, &pi);
		if (!ok) return false;

		CloseHandle(pi.hThread);
		WaitForSingleObject(pi.hProcess, INFINITE);
		DWORD ec = 0;
		GetExitCodeProcess(pi.hProcess, &ec);
		CloseHandle(pi.hProcess);
		*exit_status = static_cast<int>(ec);
		return true;
	}
#endif  // _WIN32

	static int classifyByExtension(const std::string& path) {
		const auto dot = path.find_last_of('.');
		if (dot == std::string::npos) return -1;
		std::string ext = path.substr(dot + 1);
		std::transform(ext.begin(), ext.end(), ext.begin(),
			[](char c) {
				return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			});
		if (ext == "sh" || ext == "bash") return 1;
		if (ext == "exe" || ext == "com" || ext == "dll" || ext == "msi"
		    || ext == "bat" || ext == "cmd" || ext == "ps1") {
			return 0;
		}

		return -1;
	}

	static std::string readShebangLine(const std::string& path) {
		std::ifstream f(utf8ToPath(path), std::ios::binary);
		if (!f) return {};
		char buf[256];
		f.read(buf, sizeof(buf));
		const std::streamsize n = f.gcount();
		if (n < 2) return {};
		if (buf[0] != '#' || buf[1] != '!') return {};
		const std::size_t lim = static_cast<std::size_t>(
			std::min<std::streamsize>(n, 256));
		std::string line(buf, lim);
		const auto nl = line.find('\n');
		if (nl != std::string::npos) line.resize(nl);
		return line;
	}

	static std::string interpFromEnvShebang(std::string body) {
		const auto rstart = body.find_first_not_of(" \t");
		if (rstart == std::string::npos) return {};
		body = body.substr(rstart);
		while (!body.empty() && body[0] == '-') {
			const auto sp = body.find_first_of(" \t");
			if (sp == std::string::npos) return {};
			body = body.substr(sp);
			const auto rs = body.find_first_not_of(" \t");
			if (rs == std::string::npos) return {};
			body = body.substr(rs);
		}

		const auto sp = body.find_first_of(" \t");
		return (sp == std::string::npos) ? body : body.substr(0, sp);
	}

	static std::string interpBasenameFromShebang(const std::string& shebang_line) {
		if (shebang_line.size() < 2) return {};
		std::string rest = shebang_line.substr(2);
		const auto start = rest.find_first_not_of(" \t");
		if (start == std::string::npos) return {};
		rest = rest.substr(start);

		const auto sp = rest.find_first_of(" \t");
		const std::string interp = (sp == std::string::npos) ? rest : rest.substr(0, sp);
		const auto slash = interp.find_last_of("/\\");
		std::string base = (slash == std::string::npos)
			? interp
			: interp.substr(slash + 1);

		if (base == "env" && sp != std::string::npos) {
			std::string env_arg_interp = interpFromEnvShebang(rest.substr(sp + 1));
			if (!env_arg_interp.empty()) base = std::move(env_arg_interp);
		}

		return base;
	}

	static bool isKnownShellInterpreter(const std::string& base) {
		return base == "sh"   || base == "bash" || base == "dash"
		    || base == "zsh"  || base == "ksh"  || base == "wbsh";
	}

	bool Executor::looksLikeShellScript(const std::string& path) const {
		switch (classifyByExtension(path)) {
		case 1:  return true;
		case 0:  return false;
		default: break;
		}

		const std::string shebang = readShebangLine(path);
		if (shebang.empty()) return false;
		const std::string interp = interpBasenameFromShebang(shebang);
		return isKnownShellInterpreter(interp);
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
		std::vector<std::string> to_unset;
		for (auto& kv : env_.vars()) {
			if (snap.saved_vars.count(kv.first) == 0) to_unset.push_back(kv.first);
		}

		for (auto& n : to_unset) env_.unset(n);

		// Restore prior values. `forceSet` (readonly bypass) only when
		// unwinding a pending signal, plain `set` otherwise — a deliberate
		// asymmetry preserved from the original implementation.
		for (auto& kv : snap.saved_vars) {
			if (force_set) env_.forceSet(kv.first, kv.second);
			else           env_.set(kv.first, kv.second);
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

	static std::string readScriptBody(std::ifstream& f) {
		std::stringstream ss;
		ss << f.rdbuf();
		std::string body = ss.str();
		normalizeCrlf(body);
		return body;
	}

	int Executor::runShellScript(const std::string& path,
	                             const std::vector<std::string>& argv,
	                             const std::vector<std::pair<std::string, std::string>>& temp_env)
	{
		std::ifstream f(utf8ToPath(path), std::ios::binary);
		if (!f) {
			std::fprintf(stderr, "wbsh: %s: %s\n", path.c_str(), std::strerror(errno));
			return 127;
		}

		std::string body = readScriptBody(f);

		ShellScriptScope snap = snapshotShellScriptScope();

		for (const auto& kv : temp_env) env_.set(kv.first, kv.second);
		env_.setShellName(path);
		env_.setPositional(std::vector<std::string>(argv.begin() + 1, argv.end()));

		int status = executeText(body, path);
		consumeFlow(FlowSignal::Kind::Exit, &status);
		restoreShellScriptScope(snap, /*force_set=*/flowPending());
		return status;
	}

	int Executor::execBraceGroup(const BraceGroup& bg) {
		RedirState rs;
		if (!applyRedirections(bg.redirs, rs)) { undoRedirections(rs); return 1; }
		int status = 0;
		if (bg.body) status = execNode(*bg.body);
		undoRedirections(rs);
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
		std::vector<std::string> to_unset;
		for (auto& kv : env_.vars()) {
			if (snap.saved_vars.count(kv.first) == 0) to_unset.push_back(kv.first);
		}

		for (auto& n : to_unset) env_.unset(n);
		for (auto& kv : snap.saved_vars) env_.forceSet(kv.first, kv.second);

		env_.setErrexit(snap.errexit);
		env_.setNounset(snap.nounset);
		env_.setXtrace(snap.xtrace);
		env_.setPipefail(snap.pipefail);
		env_.setNoglob(snap.noglob);
		trap_handlers_ = std::move(snap.traps);

		std::error_code ec;
		std::filesystem::current_path(snap.saved_cwd, ec);
	}

	int Executor::execSubshell(const Subshell& ss) {
		SubshellScope snap = snapshotSubshellScope();

		RedirState rs;
		if (!applyRedirections(ss.redirs, rs)) { undoRedirections(rs); return 1; }

		int status = 0;
		if (ss.body) status = execNode(*ss.body);
		consumeFlow(FlowSignal::Kind::Exit, &status);
		undoRedirections(rs);

		if (!flowPending()) fireExitTrap();
		restoreSubshellScope(snap);
		return status;
	}

	int Executor::execIf(const IfClause& ic) {
		RedirState rs;
		if (!applyRedirections(ic.redirs, rs)) { undoRedirections(rs); return 1; }
		int status = 0;
		bool fired = false;
		for (const auto& br : ic.branches) {
			pushErrexitSuppress();
			const int c = br.cond ? execNode(*br.cond) : 0;
			popErrexitSuppress();
			if (flowPending()) { fired = true; break; }
			setLastStatus(c);
			if (c != 0) continue;
			if (br.body) {
				const int s = execNode(*br.body);
				if (!flowPending()) status = s;
			}

			fired = true;
			break;
		}

		if (!fired && ic.else_body) {
			const int s = execNode(*ic.else_body);
			if (!flowPending()) status = s;
		}

		undoRedirections(rs);
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

	int Executor::execWhile(const WhileClause& wc) {
		RedirState rs;
		if (!applyRedirections(wc.redirs, rs)) { undoRedirections(rs); return 1; }
		int status = 0;
		++loop_depth_;
		while (true) {
			pushErrexitSuppress();
			const int c = wc.cond ? execNode(*wc.cond) : 0;
			popErrexitSuppress();
			if (flowPending()) break;
			setLastStatus(c);
			const bool keep = wc.until ? (c != 0) : (c == 0);
			if (!keep) break;
			const int s = wc.body ? execNode(*wc.body) : 0;
			if (!flowPending()) status = s;
			const LoopFlowAction act = dispatchLoopFlow();
			if (act == LoopFlowAction::NextIter) continue;
			if (act == LoopFlowAction::ExitLoop || act == LoopFlowAction::Propagate) break;
		}

		--loop_depth_;
		undoRedirections(rs);
		return status;
	}

	int Executor::execFor(const ForClause& fc) {
		RedirState rs;
		if (!applyRedirections(fc.redirs, rs)) { undoRedirections(rs); return 1; }
		int status = 0;
		++loop_depth_;
		std::vector<std::string> values;
		if (fc.has_in) {
			for (const auto& w : fc.items) {
				auto fields = expander_.expandWord(w);
				if (expander_.failed()) {
					std::fprintf(stderr, "wbsh: %s\n", expander_.takeError().c_str());
					--loop_depth_;
					undoRedirections(rs);
					return 1;
				}

				if (flowPending()) {
					--loop_depth_;
					undoRedirections(rs);
					return status;
				}

				for (auto& f : fields) values.push_back(std::move(f));
			}
		} else {
			values = env_.positional();
		}

		for (const auto& v : values) {
			env_.set(fc.var, v);
			const int s = fc.body ? execNode(*fc.body) : 0;
			if (!flowPending()) status = s;
			const LoopFlowAction act = dispatchLoopFlow();
			if (act == LoopFlowAction::NextIter) continue;
			if (act == LoopFlowAction::ExitLoop || act == LoopFlowAction::Propagate) break;
		}

		--loop_depth_;
		undoRedirections(rs);
		return status;
	}

	bool Executor::patternMatches(const std::string& pat, const std::string& s) {
		return fnmatchFull(pat, s);
	}

	int Executor::execCase(const CaseClause& cc) {
		RedirState rs;
		if (!applyRedirections(cc.redirs, rs)) { undoRedirections(rs); return 1; }
		int status = 0;
		const std::string subject = expander_.expandStringValue(cc.subject);
		if (expander_.failed()) {
			std::fprintf(stderr, "wbsh: %s\n", expander_.takeError().c_str());
			undoRedirections(rs);
			return 1;
		}

		for (std::size_t i = 0; i < cc.items.size(); ++i) {
			const auto& it = cc.items[i];
			bool m = false;
			for (const auto& pw : it.patterns) {
				const std::string pat = expander_.expandStringValue(pw);
				if (expander_.failed()) {
					expander_.takeError();
					continue;
				}

				if (patternMatches(pat, subject)) { m = true; break; }
			}

			if (!m) continue;
			if (it.body) {
				const int s = execNode(*it.body);
				if (!flowPending()) status = s;
			}

			if (flowPending()) break;
			if (it.term == CaseClause::Term::DSemi) break;
			if (it.term == CaseClause::Term::SemiAmp) {
				if (i + 1 < cc.items.size()) {
					const auto& nx = cc.items[i + 1];
					if (nx.body) {
						const int s = execNode(*nx.body);
						if (!flowPending()) status = s;
					}
				}
				break;
			}

			if (it.term == CaseClause::Term::DSemiAmp) {
				continue;
			}
			break;
		}

		undoRedirections(rs);
		return status;
	}

	int Executor::execFunctionDef(const FunctionDef& fd) {
		functions_[fd.name] = &fd;
		return 0;
	}

	static bool evalDBracketExpr(const DBracketCond::Expr& e,
	                             Expander& exp,
	                             const PathConv& pc,
	                             bool nocasematch);

	static bool evalDBracketUnaryTest(char op, const std::string& lhs, const PathConv& pc) {
		if (op == 'z') return lhs.empty();
		if (op == 'n') return !lhs.empty();
		std::string path = pc.toWin32(lhs);
		struct stat st {};
		bool ok = ::stat(path.c_str(), &st) == 0;
		switch (op) {
		case 'e': return ok;
		case 'f': return ok && (st.st_mode & S_IFMT) == S_IFREG;
		case 'd': return ok && (st.st_mode & S_IFMT) == S_IFDIR;
		case 's': return ok && st.st_size > 0;
		case 'r': return ok && (st.st_mode & 0444);
		case 'w': return ok && (st.st_mode & 0222);
		case 'x': return ok && (st.st_mode & 0111);
		default:  return false;
		}
	}

	static bool evalDBracketStringOp(const std::string& op,
	                                 const std::string& clhs, const std::string& crhs) {
		if (op == "==" || op == "=") return clhs == crhs;
		if (op == "!=") return clhs != crhs;
		if (op == "<")  return clhs <  crhs;
		if (op == ">")  return clhs >  crhs;
		return false;
	}

	static bool evalDBracketArithOp(const std::string& op,
	                                const std::string& lhs, const std::string& rhs) {
		long long li = 0;
		long long ri = 0;
		if (!parseLL(lhs, li) || !parseLL(rhs, ri)) return false;
		if (op == "-eq") return li == ri;
		if (op == "-ne") return li != ri;
		if (op == "-lt") return li <  ri;
		if (op == "-le") return li <= ri;
		if (op == "-gt") return li >  ri;
		return li >= ri;
	}

	static bool evalDBracketFileOp(const std::string& op,
	                               const std::string& lhs, const std::string& rhs,
	                               const PathConv& pc) {
		std::string a = pc.toWin32(lhs);
		std::string b = pc.toWin32(rhs);
		struct stat sa{}, sb{};
		bool oa = ::stat(a.c_str(), &sa) == 0;
		bool ob = ::stat(b.c_str(), &sb) == 0;
		if (op == "-nt") return oa && (!ob || sa.st_mtime > sb.st_mtime);
		if (op == "-ot") return ob && (!oa || sa.st_mtime < sb.st_mtime);
		return oa && ob && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
	}

	static bool evalDBracketExpr(const DBracketCond::Expr& e,
	                             Expander& exp,
	                             const PathConv& pc,
	                             bool nocasematch) {
		using K = DBracketCond::Expr::K;
		switch (e.k) {
		case K::And: return evalDBracketExpr(*e.a, exp, pc, nocasematch)
		                 && evalDBracketExpr(*e.b, exp, pc, nocasematch);
		case K::Or:  return evalDBracketExpr(*e.a, exp, pc, nocasematch)
		                 || evalDBracketExpr(*e.b, exp, pc, nocasematch);
		case K::Not: return !evalDBracketExpr(*e.a, exp, pc, nocasematch);
		case K::Prim: break;
		}

		std::string lhs = exp.expandStringValue(e.lhs);
		if (exp.failed()) return false;
		if (e.op.empty()) return !lhs.empty();
		if (e.op.size() == 2 && e.op[0] == '-') {
			return evalDBracketUnaryTest(e.op[1], lhs, pc);
		}

		std::string rhs = exp.expandStringValue(e.rhs);
		if (exp.failed()) return false;
		auto lower = [](std::string s) {
			for (auto& c : s) c = (char)std::tolower((unsigned char)c);
			return s;
		};
		std::string clhs = nocasematch ? lower(lhs) : lhs;
		std::string crhs = nocasematch ? lower(rhs) : rhs;
		if (e.op == "==" || e.op == "=" || e.op == "!=" || e.op == "<" || e.op == ">") {
			return evalDBracketStringOp(e.op, clhs, crhs);
		}

		if (e.op == "=~") {
			auto flags = std::regex::ECMAScript;
			if (nocasematch) flags = flags | std::regex::icase;
			std::regex re;
			if (!compileRegex(re, rhs, flags)) return false;
			return searchRegex(lhs, re);
		}

		if (e.op == "-eq" || e.op == "-ne" || e.op == "-lt"
		    || e.op == "-le" || e.op == "-gt" || e.op == "-ge")
		{
			return evalDBracketArithOp(e.op, lhs, rhs);
		}

		if (e.op == "-ef" || e.op == "-nt" || e.op == "-ot") {
			return evalDBracketFileOp(e.op, lhs, rhs, pc);
		}

		return false;
	}

	int Executor::execDBracket(const DBracketCond& dc) {
		RedirState rs;
		bool ok = applyRedirections(dc.redirs, rs);
		int status;
		if (!ok) status = 1;
		else if (!dc.root) status = 1;
		else status = evalDBracketExpr(*dc.root, expander_, path_conv_,
		                               env_.nocasematch()) ? 0 : 1;
		if (expander_.failed()) {
			std::fprintf(stderr, "wbsh: %s\n", expander_.takeError().c_str());
			status = 1;
		}

		undoRedirections(rs);
		setLastStatus(status);
		return status;
	}

	void Executor::registerBuiltin(std::string name, BuiltinFn fn) {
		builtins_[std::move(name)] = fn;
	}

	bool Executor::isBuiltin(const std::string& name) const {
		return builtins_.count(name) != 0;
	}

	bool Executor::isFunction(const std::string& name) const {
		return functions_.count(name) != 0;
	}

	int Executor::callBuiltin(const std::string& name, const std::vector<std::string>& args) {
		auto it = builtins_.find(name);
		if (it == builtins_.end()) return 127;
		return it->second(*this, args);
	}

	int Executor::callFunction(const std::string& name, const std::vector<std::string>& args) {
		auto it = functions_.find(name);
		if (it == functions_.end()) return 127;
		const FunctionDef* fd = it->second;
		auto saved_positional = env_.positional();
		env_.setPositional(args);
		++func_depth_;
		scope_stack_.emplace_back();
		int status = 0;
		if (fd->body) status = execNode(*fd->body);
		consumeFlow(FlowSignal::Kind::Return, &status);
		popLocalScope();
		--func_depth_;
		env_.setPositional(saved_positional);
		return status;
	}

	void Executor::popLocalScope() {
		if (scope_stack_.empty()) return;
		auto& top = scope_stack_.back();
		for (auto rit = top.rbegin(); rit != top.rend(); ++rit) {
			if (rit->had_prev) env_.set(rit->name, rit->prev_value);
			else               env_.unset(rit->name);
		}

		scope_stack_.pop_back();
	}

	int Executor::registerJob(void* handle, long long pid, std::string cmd_text) {
		Job j;
		j.id = ++next_job_id_;
#ifdef _WIN32
		j.handle = static_cast<HANDLE>(handle);
#else
		j.handle = handle;
#endif
		j.pid = pid;
		j.cmd_text = std::move(cmd_text);
		j.running = true;
		jobs_.push_back(j);
		return j.id;
	}

	bool Executor::reapJobs() {
		bool changed = false;
#ifdef _WIN32
		for (auto& j : jobs_) {
			if (!j.running || j.handle == nullptr) continue;
			DWORD ec = 0;
			if (GetExitCodeProcess(j.handle, &ec) && ec != STILL_ACTIVE) {
				j.running = false;
				j.exit_code = static_cast<int>(ec);
				CloseHandle(j.handle);
				j.handle = nullptr;
				changed = true;
			}
		}
#endif
		return changed;
	}

	int Executor::waitForJob(int id) {
		for (auto& j : jobs_) {
			if (j.id != id) continue;
			if (!j.running) return j.exit_code;
#ifdef _WIN32
			WaitForSingleObject(j.handle, INFINITE);
			DWORD ec = 0;
			GetExitCodeProcess(j.handle, &ec);
			j.exit_code = static_cast<int>(ec);
			j.running = false;
			CloseHandle(j.handle);
			j.handle = nullptr;
#endif
			return j.exit_code;
		}

		return -1;
	}

	void Executor::waitForAllJobs() {
		for (auto& j : jobs_) {
			if (!j.running) continue;
#ifdef _WIN32
			WaitForSingleObject(j.handle, INFINITE);
			DWORD ec = 0;
			GetExitCodeProcess(j.handle, &ec);
			j.exit_code = static_cast<int>(ec);
			j.running = false;
			CloseHandle(j.handle);
			j.handle = nullptr;
#endif
		}
	}

	void Executor::fireExitTrap() {
		auto it = trap_handlers_.find("EXIT");
		if (it == trap_handlers_.end()) return;
		std::string cmd = it->second;
		trap_handlers_.erase(it);
		executeText(cmd, "<EXIT trap>");
		clearFlow();
	}

	static constexpr std::size_t kMaxHistory = 5000;

	static void trimToMaxHistory(std::vector<std::string>& cmds,
	                             std::vector<int>& statuses) {
		if (cmds.size() <= kMaxHistory) return;
		const std::size_t drop = cmds.size() - kMaxHistory;
		cmds.erase(cmds.begin(), cmds.begin() + drop);
		if (statuses.size() >= drop) {
			statuses.erase(statuses.begin(), statuses.begin() + drop);
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

	// On-disk format:
	//
	//   #!wbsh-history-v2
	//   <status>\t<command>
	//   <status>\t<command>
	//   ...
	//
	// Legacy files (no header, one command per line) still load: each line
	// becomes an entry with status 0 ("treat as OK"). This keeps existing
	// ~/.wbsh_history files working across the upgrade.
	static constexpr const char* kHistoryV2Header = "#!wbsh-history-v2";

	bool Executor::loadHistoryFromFile(const std::string& path) {
		std::ifstream f(utf8ToPath(path));
		if (!f) return false;
		std::string line;
		bool header_checked = false;
		bool v2 = false;
		while (std::getline(f, line)) {
			while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
				line.pop_back();
			if (!header_checked) {
				header_checked = true;
				if (line == kHistoryV2Header) { v2 = true; continue; }
			}

			if (line.empty()) continue;
			int status = 0;
			std::string cmd = line;
			if (v2) {
				const std::size_t tab = line.find('\t');
				if (tab == std::string::npos) continue;
				if (!parseInt(line.substr(0, tab), status)) continue;
				cmd = line.substr(tab + 1);
				if (cmd.empty()) continue;
			}

			history_.push_back(std::move(cmd));
			history_status_.push_back(status);
		}

		trimToMaxHistory(history_, history_status_);
		return true;
	}

	bool Executor::saveHistoryToFile(const std::string& path) const {
		std::ofstream f(utf8ToPath(path), std::ios::trunc);
		if (!f) return false;
		f << kHistoryV2Header << '\n';
		for (std::size_t i = 0; i < history_.size(); ++i) {
			const int s = (i < history_status_.size()) ? history_status_[i] : 0;
			f << s << '\t' << history_[i] << '\n';
		}

		return true;
	}

	static std::string shellQuoteSingle(const std::string& s) {
		std::string q = "'";
		for (char c : s) {
			if (c == '\'') q += "'\\''";
			else q.push_back(c);
		}

		q += "'";
		return q;
	}

	std::string Executor::serializeAliases() const {
		std::vector<std::pair<std::string, std::string>> v(
			aliases_.begin(), aliases_.end());
		std::sort(v.begin(), v.end());
		std::string out;
		for (const auto& kv : v) {
			out += "alias ";
			out += kv.first;
			out += "=";
			out += shellQuoteSingle(kv.second);
			out += "\n";
		}

		return out;
	}

	std::string Executor::serializeArrays() const {
		std::string out;

		std::vector<std::string> ix_names;
		ix_names.reserve(env_.indexedArrays().size());
		for (const auto& kv : env_.indexedArrays()) ix_names.push_back(kv.first);
		std::sort(ix_names.begin(), ix_names.end());
		for (const auto& name : ix_names) {
			const auto* arr = env_.getIndexedArray(name);
			if (!arr) continue;
			out += name;
			out += "=(";
			bool first = true;
			for (const auto& kv : *arr) {
				if (!first) out.push_back(' ');
				out += "[";
				out += std::to_string(kv.first);
				out += "]=";
				out += shellQuoteSingle(kv.second);
				first = false;
			}

			out += ")\n";
		}

		std::vector<std::string> ax_names;
		ax_names.reserve(env_.assocArrays().size());
		for (const auto& kv : env_.assocArrays()) ax_names.push_back(kv.first);
		std::sort(ax_names.begin(), ax_names.end());
		for (const auto& name : ax_names) {
			const auto* arr = env_.getAssocArray(name);
			if (!arr) continue;
			out += "declare -A ";
			out += name;
			out += "\n";
			for (const auto& kv : *arr) {
				out += name;
				out += "[";
				out += shellQuoteSingle(kv.first);
				out += "]=";
				out += shellQuoteSingle(kv.second);
				out += "\n";
			}
		}

		return out;
	}

	std::string Executor::serializeFunctions() const {
		std::vector<const FunctionDef*> defs;
		defs.reserve(functions_.size());
		for (const auto& kv : functions_) {
			if (kv.second && !kv.second->body_text.empty()) defs.push_back(kv.second);
		}

		std::sort(defs.begin(), defs.end(),
			[](const FunctionDef* a, const FunctionDef* b) {
				return a->name < b->name;
			});
		std::string out;
		for (const FunctionDef* fd : defs) {
			out += fd->name;
			out += "() ";
			out += fd->body_text;
			out.push_back('\n');
		}

		return out;
	}

	void Executor::declareLocal(const std::string& name, const std::string& value) {
		if (scope_stack_.empty()) {
			env_.set(name, value);
			return;
		}

		auto& top = scope_stack_.back();
		bool already = false;
		for (const auto& e : top) if (e.name == name) { already = true; break; }
		if (!already) {
			ScopeEntry se;
			se.name = name;
			// has()/get() rather than a single vars() lookup: both also
			// see array variables (get() reads element 0), and the scope
			// pop must restore those instead of unsetting them.
			se.had_prev = env_.has(name);
			se.prev_value = env_.get(name);
			top.push_back(std::move(se));
		}

		env_.set(name, value);
	}

	int Executor::executeText(const std::string& source_text, const std::string& origin) {
		Lexer lex(source_text);
		auto tokens = lex.tokenize();
		for (const auto& e : lex.errors()) {
			std::fprintf(stderr, "wbsh: %s:%zu:%zu: %s\n",
				origin.c_str(), e.loc.line, e.loc.column, e.message.c_str());
		}

		Parser parser(std::move(tokens), source_text);
		Node* root = parser.parseProgram();
		for (const auto& e : parser.errors()) {
			std::fprintf(stderr, "wbsh: %s:%zu:%zu: %s\n",
				origin.c_str(), e.loc.line, e.loc.column, e.message.c_str());
		}

		if (!root) return 1;
		owned_arenas_.push_back(parser.takeArena());
		return execute(*root);
	}

	std::string Executor::run(const std::string& body) {
		std::string out = runRaw(body);
		// `\r` is stripped along with `\n`: the child's CRT text mode may
		// have written `\r\n`, and a stray CR corrupts captured values.
		while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
			out.pop_back();
		return out;
	}

	std::string Executor::runRaw(const std::string& body) {
		std::string tmp = makeTempFile();
		if (tmp.empty()) return {};
		int fd = _open(tmp.c_str(),
			_O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY,
			_S_IREAD | _S_IWRITE);
		if (fd < 0) { _wremove(utf8ToWide(tmp).c_str()); return {}; }
		std::fflush(stdout);
		int saved = _dup(1);
		_dup2(fd, 1);
		_close(fd);
		executeText(body, "<command-substitution>");
		consumeFlow(FlowSignal::Kind::Exit);
		std::fflush(stdout);
		_dup2(saved, 1);
		_close(saved);
		if (flowPending()) {
			_wremove(utf8ToWide(tmp).c_str());
			return {};
		}

		std::string out = readAllText(tmp);
		_wremove(utf8ToWide(tmp).c_str());
		return out;
	}

}  // namespace wbsh

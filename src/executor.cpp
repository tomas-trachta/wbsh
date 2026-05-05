/**
 * @file executor.cpp
 * @brief AST executor implementation: pipelines, redirection, control
 *        flow, builtin / function dispatch, and process spawning.
 */

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
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unordered_set>
#include <utility>
#include <vector>

#include "lexer.h"
#include "parser.h"

namespace wbsh {

	// ---- fnmatch (subset, used for case patterns) ----
	static bool matchHere(const std::string& p, std::size_t pi,
	                      const std::string& s, std::size_t si);

	static bool matchBracket(const std::string& p, std::size_t& pi, char c) {
		std::size_t k = pi + 1;
		bool negate = false;
		if (k < p.size() && (p[k] == '!' || p[k] == '^')) { negate = true; ++k; }
		bool match = false;
		bool first = true;
		while (k < p.size() && (first || p[k] != ']')) {
			char a = p[k++];
			if (a == '\\' && k < p.size()) a = p[k++];
			if (k < p.size() && p[k] == '-' && k + 1 < p.size() && p[k + 1] != ']') {
				++k;
				char b = p[k++];
				if (b == '\\' && k < p.size()) b = p[k++];
				if (c >= a && c <= b) match = true;
			} else {
				if (c == a) match = true;
			}
			first = false;
		}
		if (k < p.size() && p[k] == ']') ++k;
		pi = k;
		return match != negate;
	}

	static bool matchHere(const std::string& p, std::size_t pi,
	               const std::string& s, std::size_t si) {
		while (pi < p.size()) {
			char pc = p[pi];
			if (pc == '*') {
				while (pi < p.size() && p[pi] == '*') ++pi;
				if (pi >= p.size()) return true;
				for (std::size_t k = si; k <= s.size(); ++k)
					if (matchHere(p, pi, s, k)) return true;
				return false;
			}
			if (si >= s.size()) return false;
			if (pc == '?') { ++pi; ++si; continue; }
			if (pc == '[') {
				std::size_t newpi = pi;
				if (!matchBracket(p, newpi, s[si])) return false;
				pi = newpi; ++si; continue;
			}
			if (pc == '\\' && pi + 1 < p.size()) {
				++pi;
				if (p[pi] != s[si]) return false;
				++pi; ++si; continue;
			}
			if (pc != s[si]) return false;
			++pi; ++si;
		}
		return si == s.size();
	}

	// ---- Temp-file helpers ----

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
	std::wstring quoteArg(const std::wstring& arg) {
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

	std::wstring buildCommandLine(const std::vector<std::string>& argv) {
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

	std::wstring cmdExePath() {
		wchar_t buf[MAX_PATH];
		UINT n = GetSystemDirectoryW(buf, MAX_PATH);
		if (n && n < MAX_PATH) {
			std::wstring p(buf, n);
			p += L"\\cmd.exe";
			return p;
		}
		return L"cmd.exe";
	}

	// Wrap a built command line so it runs through cmd.exe. Required for
	// .cmd/.bat scripts (e.g. VS Code's `code.cmd`): CreateProcessW can't
	// launch them directly. /d skips registry AutoRun, /s with outer
	// quotes lets cmd treat the entire inner string as one command and
	// preserves quoting around paths/args with spaces.
	std::wstring wrapWithCmdExe(const std::wstring& cmdline) {
		std::wstring cmd = cmdExePath();
		std::wstring quoted = (cmd.find(L' ') != std::wstring::npos)
			? L"\"" + cmd + L"\""
			: cmd;
		return quoted + L" /d /s /c \"" + cmdline + L"\"";
	}

	std::wstring buildEnvBlock(const Environment& env,
	                           const std::vector<std::pair<std::string, std::string>>& overrides,
	                           bool include_unexported = false) {
		std::vector<std::wstring> entries;
		for (const auto& kv : env.vars()) {
			if (!env.isExported(kv.first) && !include_unexported) continue;
			std::string entry = kv.first + "=" + kv.second;
			entries.push_back(utf8ToWide(entry));
		}
		// Apply overrides (replace existing entries with the same name).
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
				// Case-insensitive compare per Windows env conventions.
				std::wstring la(a.size(), L'\0'), lb(b.size(), L'\0');
				std::transform(a.begin(), a.end(), la.begin(), ::towlower);
				std::transform(b.begin(), b.end(), lb.begin(), ::towlower);
				return la < lb;
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

	// Spawn a process with explicit stdin / stdout / stderr handles via
	// STARTUPINFOEX + PROC_THREAD_ATTRIBUTE_HANDLE_LIST. Each of h_in,
	// h_out, h_err must already be inheritable (HANDLE_FLAG_INHERIT).
	// Returns the process handle on success, INVALID_HANDLE_VALUE on failure.
	static HANDLE spawnWithHandles(const std::wstring& exe,
	                        std::wstring& cmdline,
	                        std::wstring& envblock,
	                        HANDLE h_in, HANDLE h_out, HANDLE h_err) {
		SIZE_T attr_size = 0;
		InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
		auto attr_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
			HeapAlloc(GetProcessHeap(), 0, attr_size));
		if (!attr_list) return INVALID_HANDLE_VALUE;
		if (!InitializeProcThreadAttributeList(attr_list, 1, 0, &attr_size)) {
			HeapFree(GetProcessHeap(), 0, attr_list);
			return INVALID_HANDLE_VALUE;
		}

		HANDLE inherits[3];
		DWORD inherit_count = 0;
		auto add_h = [&](HANDLE h) {
			if (h == nullptr || h == INVALID_HANDLE_VALUE) return;
			for (DWORD i = 0; i < inherit_count; ++i) {
				if (inherits[i] == h) return;
			}
			inherits[inherit_count++] = h;
			SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
		};
		add_h(h_in);
		add_h(h_out);
		add_h(h_err);

		if (!UpdateProcThreadAttribute(attr_list, 0,
			PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
			inherits, inherit_count * sizeof(HANDLE),
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
		BOOL ok = CreateProcessW(
			exe.c_str(),
			cmdline.data(),
			nullptr, nullptr,
			TRUE,
			EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
			envblock.data(),
			nullptr,
			&siex.StartupInfo,
			&pi);

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

	// ---------------------------------------------------------------------------
	// Construction & dispatch
	// ---------------------------------------------------------------------------

	Executor::Executor(Environment& env)
		: env_(env), expander_(env, this) {
		registerCoreBuiltins(*this);
		registerCoreutils(*this);
	}

	int Executor::execute(const Node& root) {
		// Let ShellExit propagate so the REPL can tear down (run EXIT trap,
		// save history, etc.) and main can return cleanly.
		return execNode(root);
	}

	int Executor::execNode(const Node& n) {
		// Track current source line for $LINENO. We only update for nodes
		// that have a real position (line > 0) to avoid clobbering on
		// synthetic / wrapper nodes.
		if (n.loc.line > 0) {
			env_.setCurrentLineno(static_cast<int>(n.loc.line));
		}
		switch (n.kind) {
		case Node::Kind::List:          return execList(static_cast<const List&>(n));
		case Node::Kind::AndOr:         return execAndOr(static_cast<const AndOr&>(n));
		case Node::Kind::Pipeline:      return execPipeline(static_cast<const Pipeline&>(n));
		case Node::Kind::SimpleCommand: return execSimpleCommand(static_cast<const SimpleCommand&>(n));
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

	// ---------------------------------------------------------------------------
	// Lists / and-or / pipelines
	// ---------------------------------------------------------------------------

	int Executor::execList(const List& l) {
		int status = 0;
		for (const auto& it : l.items) {
			if (it.background) {
#ifdef _WIN32
				// Spawn detached using the same plumbing as pipeline elements,
				// keeping parent's stdin / stdout / stderr.
				HANDLE in  = (HANDLE)_get_osfhandle(0);
				HANDLE out = (HANDLE)_get_osfhandle(1);
				HANDLE err = (HANDLE)_get_osfhandle(2);
				HANDLE proc = launchPipelineElement(*it.command, in, out, err);
				if (proc != INVALID_HANDLE_VALUE) {
					std::string cmd_text;
					if (it.command->source_text
					    && it.command->src_end > it.command->src_start
					    && it.command->src_end <= it.command->source_text->size()) {
						cmd_text = it.command->source_text->substr(
							it.command->src_start,
							it.command->src_end - it.command->src_start);
					}
					long long pid = static_cast<long long>(GetProcessId(proc));
					int jid = registerJob(proc, pid, std::move(cmd_text));
					env_.setLastBgPid(pid);
					std::fprintf(stderr, "[%d] %lld\n", jid, pid);
					status = 0;
				} else {
					status = 1;
				}
				setLastStatus(status);
#else
				status = execNode(*it.command);
				setLastStatus(status);
#endif
			} else {
				status = execNode(*it.command);
				setLastStatus(status);
			}
			// `set -e`: abort on the first failing command unless we're
			// inside a context where errexit is suppressed (if/while
			// conditions, AndOr left side, bang pipeline).
			if (env_.errexit() && status != 0
			    && errexit_suppress_ == 0
			    && !it.background) {
				throw ShellExit{ status };
			}
		}
		return status;
	}

	int Executor::execAndOr(const AndOr& a) {
		// errexit doesn't fire on the LEFT side of `&&` / `||`.
		pushErrexitSuppress();
		int l = 0;
		try { l = execNode(*a.left); }
		catch (...) { popErrexitSuppress(); throw; }
		popErrexitSuppress();
		setLastStatus(l);
		if (a.op == AndOr::Op::AndIf) {
			if (l != 0) return l;
		} else {
			if (l == 0) return l;
		}
		int r = execNode(*a.right);
		setLastStatus(r);
		return r;
	}

	int Executor::execPipeline(const Pipeline& p) {
		// `! pipeline` suppresses errexit firing on the inner result.
		struct BangGuard {
			Executor* e; bool active;
			BangGuard(Executor* x, bool a) : e(x), active(a) { if (active) e->pushErrexitSuppress(); }
			~BangGuard() { if (active) e->popErrexitSuppress(); }
		} bguard(this, p.bang);

		// `time pipeline` — measure real wall time and print to stderr after
		// the pipeline finishes. Bash also prints user/sys but we only have
		// reliable wall clock without per-process CPU accounting on Windows.
		using clk = std::chrono::steady_clock;
		auto t0 = clk::now();
		struct TimeGuard {
			bool active;
			clk::time_point start;
			~TimeGuard() {
				if (!active) return;
				auto dur = clk::now() - start;
				auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(dur).count();
				long long mins = ms / 60000;
				double secs = (ms % 60000) / 1000.0;
				std::fprintf(stderr, "\nreal\t%lldm%.3fs\n", mins, secs);
			}
		} tguard{ p.timed, t0 };

		if (p.commands.size() == 1) {
			int r = execNode(*p.commands[0]);
			if (p.bang) r = (r == 0) ? 1 : 0;
			return r;
		}

#ifdef _WIN32
		const std::size_t n = p.commands.size();

		// Create n-1 OS pipes; both ends inheritable (we constrain inheritance
		// per child via PROC_THREAD_ATTRIBUTE_HANDLE_LIST below).
		SECURITY_ATTRIBUTES sa{};
		sa.nLength = sizeof(sa);
		sa.bInheritHandle = TRUE;

		std::vector<HANDLE> pipe_r(n - 1, INVALID_HANDLE_VALUE);
		std::vector<HANDLE> pipe_w(n - 1, INVALID_HANDLE_VALUE);
		for (std::size_t i = 0; i + 1 < n; ++i) {
			if (!CreatePipe(&pipe_r[i], &pipe_w[i], &sa, 0)) {
				std::fprintf(stderr, "wbsh: pipe creation failed: %s\n",
					lastErrorString().c_str());
				for (std::size_t j = 0; j < i; ++j) {
					CloseHandle(pipe_r[j]);
					CloseHandle(pipe_w[j]);
				}
				return 1;
			}
		}

		std::fflush(stdout);
		std::fflush(stderr);
		HANDLE std_in  = (HANDLE)_get_osfhandle(0);
		HANDLE std_out = (HANDLE)_get_osfhandle(1);
		HANDLE std_err = (HANDLE)_get_osfhandle(2);

		std::vector<HANDLE> processes;
		processes.reserve(n);
		bool launch_ok = true;

		for (std::size_t i = 0; i < n && launch_ok; ++i) {
			HANDLE h_in  = (i == 0)         ? std_in  : pipe_r[i - 1];
			HANDLE h_out = (i + 1 == n)     ? std_out : pipe_w[i];
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
		}

		// Close all pipe handles in the parent so EOF propagates correctly
		// when the writer end's only remaining ref (the child's copy) closes.
		for (HANDLE h : pipe_r) if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
		for (HANDLE h : pipe_w) if (h != INVALID_HANDLE_VALUE) CloseHandle(h);

		// Wait for everything we launched, then collect statuses.
		std::vector<int> stats(processes.size(), 0);
		for (std::size_t i = 0; i < processes.size(); ++i) {
			WaitForSingleObject(processes[i], INFINITE);
			DWORD ec = 0;
			GetExitCodeProcess(processes[i], &ec);
			stats[i] = static_cast<int>(ec);
			CloseHandle(processes[i]);
		}
		// Default pipeline status is the last command's exit code.
		// `set -o pipefail` makes it the rightmost non-zero status.
		int last = stats.empty() ? 0 : stats.back();
		if (env_.pipefail()) {
			for (auto it = stats.rbegin(); it != stats.rend(); ++it) {
				if (*it != 0) { last = *it; break; }
			}
		}

		if (!launch_ok && processes.size() < n) return 1;
		if (p.bang) last = (last == 0) ? 1 : 0;
		return last;
#else
		(void)p;
		return 1;
#endif
	}

#ifdef _WIN32
	HANDLE Executor::launchPipelineElement(const Node& elem,
	                                       HANDLE h_in, HANDLE h_out, HANDLE h_err) {
		// Try a direct external launch when the element is a SimpleCommand
		// without redirections whose head resolves to a non-builtin / non-
		// function. This avoids spawning an intermediate wbsh.exe.
		if (elem.kind == Node::Kind::SimpleCommand) {
			const auto& sc = static_cast<const SimpleCommand&>(elem);
			if (sc.redirs.empty()) {
				std::vector<std::string> argv;
				bool expand_failed = false;
				for (const auto& w : sc.words) {
					try {
						auto fields = expander_.expandWord(w);
						for (auto& f : fields) argv.push_back(std::move(f));
					} catch (const ExpandError& e) {
						std::fprintf(stderr, "wbsh: %s\n", e.what());
						expand_failed = true;
						break;
					}
				}
				if (expand_failed) return INVALID_HANDLE_VALUE;
				if (!argv.empty() && !isBuiltin(argv[0]) && !isFunction(argv[0])
				    && !isAlias(argv[0])) {
					std::string exec_path = findExecutable(argv[0]);
					if (!exec_path.empty()) {
						// Shell scripts can't go through CreateProcess —
						// fall through to self-spawn so wbsh itself
						// interprets them.
						if (!looksLikeShellScript(exec_path)) {
							return launchExternalDirect(sc, argv, exec_path,
								h_in, h_out, h_err);
						}
					} else {
						std::fprintf(stderr, "wbsh: %s: command not found\n",
							argv[0].c_str());
						return INVALID_HANDLE_VALUE;
					}
				}
			}
		}

		// Fall back: spawn `wbsh.exe -r -c "<source slice>"`. The slice
		// reproduces this AST node from its own source text (each Node
		// carries a shared_ptr to the parse-time source it came from, so
		// nodes from inherited functions / sourced files slice correctly).
		const std::string* src_for_node = elem.source_text
			? elem.source_text.get()
			: (source_text_.empty() ? nullptr : &source_text_);
		if (!src_for_node || elem.src_end <= elem.src_start
		    || elem.src_end > src_for_node->size()) {
			std::fprintf(stderr, "wbsh: cannot extract pipeline element source\n");
			return INVALID_HANDLE_VALUE;
		}
		std::string slice = src_for_node->substr(elem.src_start,
			elem.src_end - elem.src_start);
		std::string self = getSelfExecutablePath();
		std::vector<std::string> argv = { self, "-r", "-c", slice };

		std::wstring exe_w     = utf8ToWide(self);
		std::wstring cmdline_w = buildCommandLine(argv);
		// Self-spawn mirrors a fork(): the child should see all our shell
		// state, not just the exported subset.
		//   - PATH: translate POSIX -> `;`-Win32 (child re-translates).
		//   - Functions: serialised into WBSH_FUNCTIONS.
		//   - Non-exported shell vars: emitted into the env block too,
		//     with a WBSH_LOCAL_NAMES marker telling the child to drop them
		//     from its export set after startup.
		std::vector<std::pair<std::string, std::string>> overrides;
		std::string p = env_.get("PATH");
		if (!p.empty()) overrides.emplace_back("PATH",
			path_conv_.pathListPosixToWin32(p));
		std::string fns = serializeFunctions();
		if (!fns.empty()) overrides.emplace_back("WBSH_FUNCTIONS", fns);
		std::string als = serializeAliases();
		if (!als.empty()) overrides.emplace_back("WBSH_ALIASES", als);
		// Compute local-names list (everything we currently hold that isn't
		// exported). The order is deterministic for stable spawn behavior.
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
			overrides.emplace_back("WBSH_LOCAL_NAMES", list);
		}
		std::wstring env_w = buildEnvBlock(env_, overrides,
			/*include_unexported=*/true);

		return spawnWithHandles(exe_w, cmdline_w, env_w, h_in, h_out, h_err);
	}

	HANDLE Executor::launchExternalDirect(const SimpleCommand& sc,
	                                       const std::vector<std::string>& argv,
	                                       const std::string& exec_path,
	                                       HANDLE h_in, HANDLE h_out, HANDLE h_err) {
		// Apply optional path-conversion to argv (skip for MSYS callees).
		std::vector<std::string> a = argv;
		a[0] = exec_path;
		const bool translate_args =
			!isMsysBinary(exec_path)
			&& env_.get("WBSH_NO_PATHCONV").empty();
		if (translate_args) {
			for (std::size_t i = 1; i < a.size(); ++i) {
				a[i] = path_conv_.translateArg(a[i]);
			}
		}

		// Build env: prefix assignments + PATH translated to Win32.
		std::vector<std::pair<std::string, std::string>> overrides;
		for (const auto& as : sc.assignments) {
			try { overrides.emplace_back(as.name, expander_.expandStringValue(as.value)); }
			catch (const ExpandError&) { /* skip */ }
		}
		bool path_set = false;
		bool home_set = false;
		for (auto& kv : overrides) {
			if (kv.first == "PATH") {
				kv.second = path_conv_.pathListPosixToWin32(kv.second);
				path_set = true;
			} else if (kv.first == "HOME") {
				home_set = true;
			}
		}
		if (!path_set) {
			std::string p = env_.get("PATH");
			if (!p.empty()) overrides.emplace_back("PATH",
				path_conv_.pathListPosixToWin32(p));
		}
		// HOME goes out in Win32 form for every external child. Native
		// binaries read HOME as a literal Windows path; MinGW-built tools
		// (Git-for-Windows mingw64\bin\git.exe, etc.) also want Win32 HOME
		// even though they tolerate POSIX-style argv. True cygwin1.dll-
		// linked binaries are rare on Windows and their runtime auto-
		// translates either form, so a Win32 HOME is safe across the board.
		// Without this, `git config --global` reads `/c/Users/...` as a
		// literal path and fails with "Author identity unknown".
		// `toWin32Short` collapses to the 8.3 form for users with diacritics
		// in their profile path (`C:\Users\Tomáš` -> `C:\Users\TOMA~1`):
		// MinGW's ANSI getenv reads the env block via CP_ACP and would
		// otherwise hand git a path with `?` where the diacritics were.
		if (!home_set) {
			std::string h = env_.get("HOME");
			if (!h.empty()) overrides.emplace_back("HOME",
				path_conv_.toWin32Short(h));
		}

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

	// ---------------------------------------------------------------------------
	// Redirections
	// ---------------------------------------------------------------------------

	bool Executor::applyRedirections(const std::vector<Redirection>& rs, RedirState& s) {
		for (const auto& r : rs) {
			int default_target;
			switch (r.op) {
			case RedirOp::Less:
			case RedirOp::DLess:
			case RedirOp::DLessDash:
			case RedirOp::TLess:
			case RedirOp::LessGreat:
			case RedirOp::LessAnd:
				default_target = 0; break;
			case RedirOp::AmpGreat:
			case RedirOp::AmpDGreat:
				default_target = 1; break;
			default:
				default_target = 1; break;
			}
			int target = (r.fd != -1) ? r.fd : default_target;
			std::fflush(stdout);
			std::fflush(stderr);

			auto save = [&](int fd) {
				int bak = _dup(fd);
				s.saved.push_back({ fd, bak });
			};

			// Special POSIX-style virtual paths bash recognizes in redirs.
			// Returns >=0 on hit (dup of an inherited fd) or -1 on no-match.
			auto special_fd = [&](const std::string& path) -> int {
				if (path == "/dev/stdin")  return _dup(0);
				if (path == "/dev/stdout") return _dup(1);
				if (path == "/dev/stderr") return _dup(2);
				const std::string fd_pfx = "/dev/fd/";
				if (path.size() > fd_pfx.size()
				    && path.compare(0, fd_pfx.size(), fd_pfx) == 0)
				{
					try {
						int n = std::stoi(path.substr(fd_pfx.size()));
						return _dup(n);
					} catch (...) {}
				}
				// /dev/tcp/HOST/PORT — open a TCP connection and wrap the
				// socket as a CRT fd so redirection can dup it.
				const std::string tcp_pfx = "/dev/tcp/";
				const std::string udp_pfx = "/dev/udp/";
				if ((path.compare(0, tcp_pfx.size(), tcp_pfx) == 0
				     && path.size() > tcp_pfx.size())
				    || (path.compare(0, udp_pfx.size(), udp_pfx) == 0
				        && path.size() > udp_pfx.size()))
				{
					bool udp = path.compare(0, udp_pfx.size(), udp_pfx) == 0;
					std::string rest = path.substr(udp ? udp_pfx.size() : tcp_pfx.size());
					auto sl = rest.rfind('/');
					if (sl == std::string::npos) return -1;
					std::string host = rest.substr(0, sl);
					std::string port = rest.substr(sl + 1);
					WSADATA wd;
					static bool wsa_inited = false;
					if (!wsa_inited) {
						if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return -1;
						wsa_inited = true;
					}
					addrinfo hints{}, *res = nullptr;
					hints.ai_family = AF_UNSPEC;
					hints.ai_socktype = udp ? SOCK_DGRAM : SOCK_STREAM;
					if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0
					    || !res) return -1;
					SOCKET sk = INVALID_SOCKET;
					for (addrinfo* p = res; p; p = p->ai_next) {
						sk = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
						if (sk == INVALID_SOCKET) continue;
						if (connect(sk, p->ai_addr, (int)p->ai_addrlen) == 0) break;
						closesocket(sk);
						sk = INVALID_SOCKET;
					}
					freeaddrinfo(res);
					if (sk == INVALID_SOCKET) return -1;
					int fd = _open_osfhandle((intptr_t)sk, _O_BINARY);
					if (fd < 0) { closesocket(sk); return -1; }
					return fd;
				}
				return -1;
			};

			auto open_path = [&](const std::string& path, int flags) -> int {
				int fd = special_fd(path);
				if (fd >= 0) return fd;
				std::wstring wp = utf8ToWide(path_conv_.toWin32(path));
				return _wopen(wp.c_str(), flags | _O_BINARY, _S_IREAD | _S_IWRITE);
			};

			switch (r.op) {
			case RedirOp::Less: {
				std::string path = expander_.expandStringValue(r.target);
				int fd = open_path(path, _O_RDONLY);
				if (fd < 0) {
					std::fprintf(stderr, "wbsh: %s: %s\n", path.c_str(), std::strerror(errno));
					return false;
				}
				save(target);
				_dup2(fd, target);
				_close(fd);
				break;
			}
			case RedirOp::Great:
			case RedirOp::Clobber: {
				std::string path = expander_.expandStringValue(r.target);
				int fd = open_path(path, _O_WRONLY | _O_CREAT | _O_TRUNC);
				if (fd < 0) {
					std::fprintf(stderr, "wbsh: %s: %s\n", path.c_str(), std::strerror(errno));
					return false;
				}
				save(target);
				_dup2(fd, target);
				_close(fd);
				break;
			}
			case RedirOp::DGreat: {
				std::string path = expander_.expandStringValue(r.target);
				int fd = open_path(path, _O_WRONLY | _O_CREAT | _O_APPEND);
				if (fd < 0) {
					std::fprintf(stderr, "wbsh: %s: %s\n", path.c_str(), std::strerror(errno));
					return false;
				}
				save(target);
				_dup2(fd, target);
				_close(fd);
				break;
			}
			case RedirOp::AmpGreat: {
				std::string path = expander_.expandStringValue(r.target);
				int fd = open_path(path, _O_WRONLY | _O_CREAT | _O_TRUNC);
				if (fd < 0) {
					std::fprintf(stderr, "wbsh: %s: %s\n", path.c_str(), std::strerror(errno));
					return false;
				}
				save(1); _dup2(fd, 1);
				save(2); _dup2(fd, 2);
				_close(fd);
				break;
			}
			case RedirOp::AmpDGreat: {
				std::string path = expander_.expandStringValue(r.target);
				int fd = open_path(path, _O_WRONLY | _O_CREAT | _O_APPEND);
				if (fd < 0) {
					std::fprintf(stderr, "wbsh: %s: %s\n", path.c_str(), std::strerror(errno));
					return false;
				}
				save(1); _dup2(fd, 1);
				save(2); _dup2(fd, 2);
				_close(fd);
				break;
			}
			case RedirOp::LessGreat: {
				std::string path = expander_.expandStringValue(r.target);
				int fd = open_path(path, _O_RDWR | _O_CREAT);
				if (fd < 0) {
					std::fprintf(stderr, "wbsh: %s: %s\n", path.c_str(), std::strerror(errno));
					return false;
				}
				save(target);
				_dup2(fd, target);
				_close(fd);
				break;
			}
			case RedirOp::LessAnd:
			case RedirOp::GreatAnd: {
				std::string what = expander_.expandStringValue(r.target);
				if (what == "-") {
					save(target);
					_close(target);
					break;
				}
				try {
					int from_fd = std::stoi(what);
					save(target);
					_dup2(from_fd, target);
				} catch (...) {
					std::fprintf(stderr, "wbsh: %s: bad fd\n", what.c_str());
					return false;
				}
				break;
			}
			case RedirOp::DLess:
			case RedirOp::DLessDash: {
				std::string body = expander_.expandHeredoc(r.heredoc_body, r.heredoc_quoted);
				std::string tmp = makeTempFile();
				if (tmp.empty()) return false;
				{
					std::ofstream f(utf8ToPath(tmp), std::ios::binary | std::ios::trunc);
					f.write(body.data(), body.size());
				}
				int fd = open_path(tmp, _O_RDONLY);
				if (fd < 0) { _wremove(utf8ToWide(tmp).c_str()); return false; }
				save(target);
				_dup2(fd, target);
				_close(fd);
				s.temps.push_back(std::move(tmp));
				break;
			}
			case RedirOp::TLess: {
				std::string body = expander_.expandStringValue(r.target);
				body.push_back('\n');
				std::string tmp = makeTempFile();
				if (tmp.empty()) return false;
				{
					std::ofstream f(utf8ToPath(tmp), std::ios::binary | std::ios::trunc);
					f.write(body.data(), body.size());
				}
				int fd = open_path(tmp, _O_RDONLY);
				if (fd < 0) { _wremove(utf8ToWide(tmp).c_str()); return false; }
				save(target);
				_dup2(fd, target);
				_close(fd);
				s.temps.push_back(std::move(tmp));
				break;
			}
			}
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

	// ---------------------------------------------------------------------------
	// SimpleCommand
	// ---------------------------------------------------------------------------

	int Executor::execSimpleCommand(const SimpleCommand& sc) {
		// Snapshot the current count of pending `<(...)` temp files. Anything
		// added during *this* command's expansion is what we'll clean up at
		// the end; outer files (when we're nested inside a command-/process-
		// substitution) stay alive for the caller.
		std::size_t proc_subst_watermark = expander_.pendingTempFileWatermark();

		// Expand assignments first.
		struct ScalarAssign { std::string name; std::string value; };
		struct ElemAssign   { std::string name; Word subscript; std::string value; };
		struct ArrayAssign  {
			std::string name;
			// If any keyed item present, the literal is sparse / assoc-style.
			bool sparse = false;
			std::vector<std::pair<std::string, std::string>> keyed;   // key, value
			std::vector<std::string> items;                            // unkeyed
		};
		std::vector<ScalarAssign> scalar_assigns;
		std::vector<ElemAssign>   elem_assigns;
		std::vector<ArrayAssign>  array_assigns;
		for (const auto& a : sc.assignments) {
			try {
				if (a.is_array) {
					ArrayAssign aa;
					aa.name = a.name;
					for (const auto& k : a.keyed_items) {
						std::string val = expander_.expandStringValue(k.value);
						if (k.has_key) {
							aa.sparse = true;
							std::string key = expander_.expandStringValue(k.key);
							aa.keyed.emplace_back(std::move(key), std::move(val));
						} else {
							// For unkeyed items, also do word-splitting + globbing
							// like a normal command word so `arr=($x)` splits.
							auto fields = expander_.expandWord(k.value);
							for (auto& f : fields) aa.items.push_back(std::move(f));
						}
					}
					array_assigns.push_back(std::move(aa));
				} else if (a.has_subscript) {
					ElemAssign ea;
					ea.name = a.name;
					ea.subscript = a.subscript;
					ea.value = expander_.expandStringValue(a.value);
					elem_assigns.push_back(std::move(ea));
				} else {
					ScalarAssign sa;
					sa.name = a.name;
					sa.value = expander_.expandStringValue(a.value);
					scalar_assigns.push_back(std::move(sa));
				}
			} catch (const ExpandError& e) {
				std::fprintf(stderr, "wbsh: %s\n", e.what());
				return 1;
			}
		}
		// Legacy alias: some downstream paths still use a flat (name, value)
		// list, so keep one for them.
		std::vector<std::pair<std::string, std::string>> assigns;
		assigns.reserve(scalar_assigns.size());
		for (auto& s : scalar_assigns) assigns.emplace_back(s.name, s.value);

		// Expand command words to argv.
		std::vector<std::string> argv;
		for (const auto& w : sc.words) {
			try {
				auto fields = expander_.expandWord(w);
				for (auto& f : fields) argv.push_back(std::move(f));
			} catch (const ExpandError& e) {
				std::fprintf(stderr, "wbsh: %s\n", e.what());
				return 1;
			}
		}

		// Alias expansion at head position. Cycle-safe: each name expands at
		// most once per resolution. Alias values are tokenised through the
		// lexer + expander so quoted strings and variables behave naturally.
		if (!argv.empty() && env_.expand_aliases()) {
			std::unordered_set<std::string> seen;
			while (isAlias(argv[0]) && seen.insert(argv[0]).second) {
				std::string val = aliasValue(argv[0]);
				Lexer alex(val);
				auto atokens = alex.tokenize();
				std::vector<std::string> repl;
				for (const auto& t : atokens) {
					if (t.kind != TokKind::Word) continue;
					Word w;
					w.segments = t.segments;
					w.raw = t.text;
					try {
						auto fields = expander_.expandWord(w);
						for (auto& f : fields) repl.push_back(std::move(f));
					} catch (const ExpandError&) {
						repl.push_back(t.text);
					}
				}
				if (repl.empty()) break;
				argv.erase(argv.begin());
				argv.insert(argv.begin(), repl.begin(), repl.end());
			}
		}

		// `exec` with no command: redirections take effect on the current
		// shell (no undo). Apply them and discard the saved-fd table.
		if (!argv.empty() && argv[0] == "exec" && argv.size() == 1) {
			RedirState rs;
			if (!applyRedirections(sc.redirs, rs)) {
				// Failure: still tear down any partial saves.
				undoRedirections(rs);
				return 1;
			}
			// Close the dup-saved fds — we don't want to restore later.
			for (auto& kv : rs.saved) _close(kv.second);
			for (int fd : rs.opened) (void)fd;   // owned by the active dup now
			return 0;
		}

		// Pure assignment (no command) — set vars in shell environment.
		if (argv.empty()) {
			RedirState rs;
			if (!applyRedirections(sc.redirs, rs)) {
				undoRedirections(rs);
				return 1;
			}
			for (const auto& a : scalar_assigns) env_.set(a.name, a.value);
			for (const auto& e : elem_assigns) {
				std::string sub_str;
				try { sub_str = expander_.expandStringValue(e.subscript); }
				catch (...) { sub_str = std::string(); }
				if (env_.isAssocArray(e.name)) {
					env_.setAssocElement(e.name, std::move(sub_str), e.value);
					continue;
				}
				long long idx = 0;
				try { idx = expander_.evalArith(sub_str); }
				catch (...) { idx = 0; }
				env_.setIndexedElement(e.name, idx, e.value);
			}
			for (const auto& aa : array_assigns) {
				if (env_.isAssocArray(aa.name)) {
					// Reset and repopulate as assoc.
					env_.declareAssocArray(aa.name);
					for (const auto& kv : aa.keyed)
						env_.setAssocElement(aa.name, kv.first, kv.second);
					// Unkeyed items in assoc context: bash treats this as an
					// error; we just ignore.
				} else if (aa.sparse) {
					std::map<long long, std::string> sparse;
					long long next_idx = 0;
					for (const auto& k : aa.keyed) {
						long long idx;
						try { idx = expander_.evalArith(k.first); }
						catch (...) { idx = next_idx; }
						sparse[idx] = k.second;
						next_idx = idx + 1;
					}
					for (auto& v : aa.items) sparse[next_idx++] = v;
					env_.setIndexedArraySparse(aa.name, std::move(sparse));
				} else {
					env_.setIndexedArrayFromList(aa.name, aa.items);
				}
			}
			undoRedirections(rs);
			return 0;
		}
		// Array / element assignments alongside a command are not supported
		// (bash also disallows `arr=(...) cmd`). Diagnose and proceed with
		// scalar prefix only.
		if (!array_assigns.empty() || !elem_assigns.empty()) {
			std::fprintf(stderr,
				"wbsh: array assignments cannot be used as command prefix\n");
			return 1;
		}

		// Apply redirections (and tear them down on completion).
		RedirState rs;
		if (!applyRedirections(sc.redirs, rs)) {
			undoRedirections(rs);
			return 1;
		}

		// `set -x` (xtrace): announce the command before running it.
		if (env_.xtrace() && !argv.empty()) {
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

		const std::string& cmd = argv[0];
		int status = 0;
		try {
			if (isFunction(cmd)) {
				// Functions see assignments as locals only via export semantics.
				// For now: apply assignments, save originals, run, restore.
				std::vector<std::pair<std::string, bool>> backup;     // name -> existed
				std::vector<std::pair<std::string, std::string>> backup_vals;
				for (const auto& a : assigns) {
					backup.emplace_back(a.first, env_.has(a.first));
					backup_vals.emplace_back(a.first, env_.get(a.first));
					env_.set(a.first, a.second);
				}
				try {
					std::vector<std::string> args(argv.begin() + 1, argv.end());
					status = callFunction(cmd, args);
				} catch (...) {
					for (std::size_t k = 0; k < backup.size(); ++k) {
						if (backup[k].second) env_.set(backup[k].first, backup_vals[k].second);
						else env_.unset(backup[k].first);
					}
					undoRedirections(rs);
					throw;
				}
				for (std::size_t k = 0; k < backup.size(); ++k) {
					if (backup[k].second) env_.set(backup[k].first, backup_vals[k].second);
					else env_.unset(backup[k].first);
				}
			} else if (isBuiltin(cmd)) {
				// For builtins: assignments are visible during the call only,
				// then reverted (matching POSIX for most builtins).
				std::vector<std::pair<std::string, bool>> backup;
				std::vector<std::pair<std::string, std::string>> backup_vals;
				for (const auto& a : assigns) {
					backup.emplace_back(a.first, env_.has(a.first));
					backup_vals.emplace_back(a.first, env_.get(a.first));
					env_.set(a.first, a.second);
				}
				try {
					std::vector<std::string> args(argv.begin() + 1, argv.end());
					status = callBuiltin(cmd, args);
				} catch (...) {
					for (std::size_t k = 0; k < backup.size(); ++k) {
						if (backup[k].second) env_.set(backup[k].first, backup_vals[k].second);
						else env_.unset(backup[k].first);
					}
					undoRedirections(rs);
					throw;
				}
				for (std::size_t k = 0; k < backup.size(); ++k) {
					if (backup[k].second) env_.set(backup[k].first, backup_vals[k].second);
					else env_.unset(backup[k].first);
				}
			} else {
				status = runExternal(argv, assigns);
			}
		} catch (...) {
			undoRedirections(rs);
			throw;
		}
		undoRedirections(rs);
		// Clean up any temp files produced by THIS command's `<(...)`
		// substitutions. Outer-caller files (above the watermark) stay.
		for (const auto& tf : expander_.drainTempFilesSince(proc_subst_watermark)) {
			std::remove(tf.c_str());
		}
		setLastStatus(status);
		return status;
	}

	// ---------------------------------------------------------------------------
	// External process launch
	// ---------------------------------------------------------------------------

	bool Executor::isAbsoluteOrRelativePath(const std::string& name) const {
		if (name.empty()) return false;
		if (name.find('/') != std::string::npos) return true;
#ifdef _WIN32
		if (name.find('\\') != std::string::npos) return true;
		if (name.size() >= 2 && name[1] == ':') return true;
#endif
		return false;
	}

	std::string Executor::findExecutable(const std::string& name) {
		namespace fs = std::filesystem;
		auto try_with_exts = [&](const fs::path& base) -> std::string {
#ifdef _WIN32
			// Try Windows-native extensions first, then fall back to the
			// bare name. cmd.exe's PATHEXT lookup never tries a no-extension
			// file, but we keep it as a fallback so extensionless scripts
			// (e.g. installed by Git/MSYS) still resolve. Putting it last
			// avoids picking up Linux-only sibling scripts that ship with
			// Windows apps -- VS Code's `bin\` has both `code` (a sh script
			// for WSL) and `code.cmd`; we want the latter.
			static const char* exts[] = { ".exe", ".cmd", ".bat", "", nullptr };
			for (int i = 0; exts[i]; ++i) {
				fs::path q = base;
				if (exts[i][0]) q += exts[i];
				std::error_code ec;
				if (fs::exists(q, ec) && !fs::is_directory(q, ec)) return q.string();
			}
			return {};
#else
			std::error_code ec;
			if (fs::exists(base, ec) && !fs::is_directory(base, ec)) return base.string();
			return {};
#endif
		};

		if (isAbsoluteOrRelativePath(name)) {
			fs::path p(path_conv_.toWin32(name));
			return try_with_exts(p);
		}

		// PATH is kept POSIX-style internally (':' separator). Each entry may
		// be POSIX or Win32; translate before use.
		std::string path = env_.get("PATH");
		if (path.empty()) return {};
		std::vector<std::string> dirs;
		std::string cur;
		// Accept ':' as the primary separator but also tolerate ';' so that
		// a PATH that wasn't normalised (e.g. set by a child) still works.
		// We treat a ':' that follows a single alpha char as a drive-letter
		// colon, not a separator.
		for (std::size_t i = 0; i < path.size(); ++i) {
			char c = path[i];
			if (c == ';') {
				dirs.push_back(cur); cur.clear();
			} else if (c == ':') {
				if (cur.size() == 1 && std::isalpha(static_cast<unsigned char>(cur[0]))) {
					cur.push_back(c);
				} else {
					dirs.push_back(cur); cur.clear();
				}
			} else {
				cur.push_back(c);
			}
		}
		if (!cur.empty()) dirs.push_back(cur);

		for (const auto& d : dirs) {
			if (d.empty()) continue;
			fs::path base(path_conv_.toWin32(d));
			base /= name;
			std::string r = try_with_exts(base);
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

		// Shell scripts (.sh / shebang-marked) are interpreted in-process —
		// Windows CreateProcess can't launch them directly.
		if (looksLikeShellScript(exec_path)) {
			return runShellScript(exec_path, argv, temp_env);
		}

#ifdef _WIN32
		// Decide whether to translate POSIX-shaped argv entries to Win32 form.
		//
		// MSYS / Cygwin binaries already understand POSIX paths (and translate
		// them themselves); translating on our side would corrupt their args
		// (e.g. /dev/null -> NUL, which MSYS cat does not recognise). We
		// detect those callees by location and skip translation. Native
		// Win32 binaries get the full POSIX-to-Win32 arg translation.
		auto isMsysBinary = [](const std::string& p) {
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
		};
		const bool translate_args =
			!isMsysBinary(exec_path)
			&& env_.get("WBSH_NO_PATHCONV").empty();

		std::vector<std::string> a = argv;
		a[0] = exec_path;
		if (translate_args) {
			for (std::size_t i = 1; i < a.size(); ++i) {
				a[i] = path_conv_.translateArg(a[i]);
			}
		}

		// Build env block, ensuring PATH is in Win32 (`;`-separated) form.
		auto temp_env_for_child = temp_env;
		bool have_path_override = false;
		bool have_home_override = false;
		for (auto& kv : temp_env_for_child) {
			if (kv.first == "PATH") {
				kv.second = path_conv_.pathListPosixToWin32(kv.second);
				have_path_override = true;
			} else if (kv.first == "HOME") {
				have_home_override = true;
			}
		}
		if (!have_path_override) {
			std::string p = env_.get("PATH");
			if (!p.empty()) {
				temp_env_for_child.emplace_back("PATH",
					path_conv_.pathListPosixToWin32(p));
			}
		}
		// Translate HOME to Win32 form for every external child -- including
		// MinGW-built tools like Git-for-Windows, which want a Windows-style
		// HOME even though they accept POSIX argv. See launchExternalDirect
		// for the full reasoning, including why we fold to the 8.3 short form.
		if (!have_home_override) {
			std::string h = env_.get("HOME");
			if (!h.empty()) temp_env_for_child.emplace_back("HOME",
				path_conv_.toWin32Short(h));
		}
		std::wstring cmdline = buildCommandLine(a);
		std::wstring exe = utf8ToWide(exec_path);
		std::wstring envblock = buildEnvBlock(env_, temp_env_for_child);
		if (isBatchFile(exec_path)) {
			cmdline = wrapWithCmdExe(cmdline);
			exe     = cmdExePath();
		}

		std::fflush(stdout);
		std::fflush(stderr);

		STARTUPINFOW si{};
		si.cb = sizeof(si);
		si.dwFlags = STARTF_USESTDHANDLES;
		si.hStdInput  = (HANDLE)_get_osfhandle(0);
		si.hStdOutput = (HANDLE)_get_osfhandle(1);
		si.hStdError  = (HANDLE)_get_osfhandle(2);
		// Make sure these handles are inheritable.
		HANDLE hs[3] = { si.hStdInput, si.hStdOutput, si.hStdError };
		for (HANDLE h : hs) {
			if (h && h != INVALID_HANDLE_VALUE)
				SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
		}

		PROCESS_INFORMATION pi{};
		BOOL ok = CreateProcessW(
			exe.c_str(),
			cmdline.data(),
			nullptr, nullptr,
			TRUE,
			CREATE_UNICODE_ENVIRONMENT,
			envblock.data(),
			nullptr,
			&si, &pi);
		if (!ok) {
			DWORD err = GetLastError();
			// "Not a Win32 binary" => almost certainly a script the OS can't
			// launch. Re-run via our in-process interpreter.
			if (err == ERROR_BAD_EXE_FORMAT) {
				return runShellScript(exec_path, argv, temp_env);
			}
			std::fprintf(stderr, "wbsh: %s: %s\n",
				argv[0].c_str(), lastErrorString().c_str());
			return 127;
		}
		CloseHandle(pi.hThread);
		WaitForSingleObject(pi.hProcess, INFINITE);
		DWORD exit_code = 0;
		GetExitCodeProcess(pi.hProcess, &exit_code);
		CloseHandle(pi.hProcess);
		return static_cast<int>(exit_code);
#else
		(void)temp_env;
		return 127;
#endif
	}

	// ---------------------------------------------------------------------------
	// Shell-script detection / execution
	// ---------------------------------------------------------------------------

	bool Executor::looksLikeShellScript(const std::string& path) const {
		// Extension hints: positive (.sh/.bash) and negative (.exe/.com/etc.).
		auto dot = path.find_last_of('.');
		if (dot != std::string::npos) {
			std::string ext = path.substr(dot + 1);
			std::transform(ext.begin(), ext.end(), ext.begin(),
				[](char c) { return static_cast<char>(std::tolower((unsigned char)c)); });
			if (ext == "sh" || ext == "bash") return true;
			if (ext == "exe" || ext == "com" || ext == "dll" || ext == "msi"
			    || ext == "bat" || ext == "cmd" || ext == "ps1") return false;
		}
		// Read shebang line (if any).
		std::ifstream f(utf8ToPath(path), std::ios::binary);
		if (!f) return false;
		char buf[256];
		f.read(buf, sizeof(buf));
		std::streamsize n = f.gcount();
		if (n < 2) return false;
		if (buf[0] != '#' || buf[1] != '!') return false;
		std::size_t lim = static_cast<std::size_t>(std::min<std::streamsize>(n, 256));
		std::string line(buf, lim);
		auto nl = line.find('\n');
		if (nl != std::string::npos) line.resize(nl);

		// "#!" + maybe whitespace + interpreter path + optional args.
		std::string rest = line.substr(2);
		auto start = rest.find_first_not_of(" \t");
		if (start == std::string::npos) return false;
		rest = rest.substr(start);
		auto sp = rest.find_first_of(" \t");
		std::string interp = (sp == std::string::npos) ? rest : rest.substr(0, sp);
		auto slash = interp.find_last_of("/\\");
		std::string base = (slash == std::string::npos) ? interp : interp.substr(slash + 1);

		// `#!/usr/bin/env <interp>` form.
		if (base == "env" && sp != std::string::npos) {
			std::string args = rest.substr(sp + 1);
			auto rstart = args.find_first_not_of(" \t");
			if (rstart != std::string::npos) {
				args = args.substr(rstart);
				// Skip env -S option words.
				while (!args.empty() && args[0] == '-') {
					auto sp2 = args.find_first_of(" \t");
					if (sp2 == std::string::npos) return false;
					args = args.substr(sp2);
					auto rs = args.find_first_not_of(" \t");
					if (rs == std::string::npos) return false;
					args = args.substr(rs);
				}
				auto sp2 = args.find_first_of(" \t");
				if (sp2 != std::string::npos) args = args.substr(0, sp2);
				base = args;
			}
		}
		return base == "sh"   || base == "bash" || base == "dash"
		    || base == "zsh"  || base == "ksh"  || base == "wbsh";
	}

	int Executor::runShellScript(const std::string& path,
	                             const std::vector<std::string>& argv,
	                             const std::vector<std::pair<std::string, std::string>>& temp_env) {
		std::ifstream f(utf8ToPath(path), std::ios::binary);
		if (!f) {
			std::fprintf(stderr, "wbsh: %s: %s\n", path.c_str(), std::strerror(errno));
			return 127;
		}
		std::stringstream ss;
		ss << f.rdbuf();
		std::string body = ss.str();

		// Subshell-style isolation: snapshot env / positional / $0 / functions
		// / CWD / option flags; restore after the script finishes.
		auto saved_vars  = env_.vars();
		auto saved_pos   = env_.positional();
		auto saved_name  = env_.shellName();
		auto saved_funcs = functions_;
		bool s_errexit  = env_.errexit();
		bool s_nounset  = env_.nounset();
		bool s_xtrace   = env_.xtrace();
		bool s_pipefail = env_.pipefail();
		bool s_noglob   = env_.noglob();
		std::error_code ec;
		auto saved_cwd = std::filesystem::current_path(ec);

		// Apply prefix-style assignments (FOO=bar ./script.sh).
		for (const auto& kv : temp_env) env_.set(kv.first, kv.second);

		env_.setShellName(path);
		std::vector<std::string> pos(argv.begin() + 1, argv.end());
		env_.setPositional(std::move(pos));

		int status = 0;
		try {
			status = executeText(body, path);
		} catch (ShellExit& e) {
			// `exit` inside a script terminates the script, not our shell.
			status = e.status;
		} catch (...) {
			// Restore even on unexpected exception.
			std::vector<std::string> to_unset;
			for (auto& kv : env_.vars()) {
				if (saved_vars.count(kv.first) == 0) to_unset.push_back(kv.first);
			}
			for (auto& n : to_unset) env_.unset(n);
			for (auto& kv : saved_vars) env_.forceSet(kv.first, kv.second);
			env_.setPositional(std::move(saved_pos));
			env_.setShellName(saved_name);
			env_.setErrexit(s_errexit);
			env_.setNounset(s_nounset);
			env_.setXtrace(s_xtrace);
			env_.setPipefail(s_pipefail);
			env_.setNoglob(s_noglob);
			functions_ = std::move(saved_funcs);
			std::filesystem::current_path(saved_cwd, ec);
			throw;
		}

		std::vector<std::string> to_unset;
		for (auto& kv : env_.vars()) {
			if (saved_vars.count(kv.first) == 0) to_unset.push_back(kv.first);
		}
		for (auto& n : to_unset) env_.unset(n);
		for (auto& kv : saved_vars) env_.set(kv.first, kv.second);
		env_.setPositional(std::move(saved_pos));
		env_.setShellName(saved_name);
		env_.setErrexit(s_errexit);
		env_.setNounset(s_nounset);
		env_.setXtrace(s_xtrace);
		env_.setPipefail(s_pipefail);
		env_.setNoglob(s_noglob);
		functions_ = std::move(saved_funcs);
		std::filesystem::current_path(saved_cwd, ec);
		return status;
	}

	// ---------------------------------------------------------------------------
	// Compound commands
	// ---------------------------------------------------------------------------

	int Executor::execBraceGroup(const BraceGroup& bg) {
		RedirState rs;
		if (!applyRedirections(bg.redirs, rs)) { undoRedirections(rs); return 1; }
		int status = 0;
		try {
			if (bg.body) status = execNode(*bg.body);
		} catch (...) {
			undoRedirections(rs);
			throw;
		}
		undoRedirections(rs);
		return status;
	}

	int Executor::execSubshell(const Subshell& ss) {
		// Save state that should not propagate out.
		auto vars_backup = env_.vars();
		auto exported_backup = env_.vars();   // just to satisfy types — reuse below
		(void)exported_backup;
		// Snapshot CWD too.
		namespace fs = std::filesystem;
		std::error_code ec;
		auto cwd_backup = fs::current_path(ec);
		// Snapshot shell-option flags. `set -e/-u/-x/-f/-o pipefail` inside
		// a subshell must not leak out.
		bool errexit_save = env_.errexit();
		bool nounset_save = env_.nounset();
		bool xtrace_save  = env_.xtrace();
		bool pipefail_save = env_.pipefail();
		bool noglob_save  = env_.noglob();
		// Trap handlers are per-shell-context: a subshell can install / clear
		// its own without affecting the parent. The subshell's EXIT trap
		// fires when the subshell exits.
		auto traps_save = trap_handlers_;

		RedirState rs;
		if (!applyRedirections(ss.redirs, rs)) { undoRedirections(rs); return 1; }
		int status = 0;
		try {
			if (ss.body) status = execNode(*ss.body);
		} catch (ShellExit& e) {
			// `exit` inside a subshell exits only the subshell.
			status = e.status;
		} catch (...) {
			undoRedirections(rs);
			// Restore env and CWD even on exception.
			for (auto& kv : env_.vars()) {
				if (vars_backup.count(kv.first) == 0) env_.unset(kv.first);
			}
			for (auto& kv : vars_backup) env_.forceSet(kv.first, kv.second);
			env_.setErrexit(errexit_save);
			env_.setNounset(nounset_save);
			env_.setXtrace(xtrace_save);
			env_.setPipefail(pipefail_save);
			env_.setNoglob(noglob_save);
			trap_handlers_ = traps_save;
			fs::current_path(cwd_backup, ec);
			throw;
		}
		undoRedirections(rs);

		// Fire the subshell's own EXIT trap (if any) before tearing down.
		fireExitTrap();

		// Discard any var changes inside the subshell.
		std::vector<std::string> to_unset;
		for (auto& kv : env_.vars()) {
			if (vars_backup.count(kv.first) == 0) to_unset.push_back(kv.first);
		}
		for (auto& n : to_unset) env_.unset(n);
		for (auto& kv : vars_backup) env_.forceSet(kv.first, kv.second);
		env_.setErrexit(errexit_save);
		env_.setNounset(nounset_save);
		env_.setXtrace(xtrace_save);
		env_.setPipefail(pipefail_save);
		env_.setNoglob(noglob_save);
		trap_handlers_ = traps_save;
		fs::current_path(cwd_backup, ec);
		return status;
	}

	int Executor::execIf(const IfClause& ic) {
		RedirState rs;
		if (!applyRedirections(ic.redirs, rs)) { undoRedirections(rs); return 1; }
		int status = 0;
		try {
			bool fired = false;
			for (const auto& br : ic.branches) {
				int c;
				pushErrexitSuppress();
				try { c = br.cond ? execNode(*br.cond) : 0; }
				catch (...) { popErrexitSuppress(); throw; }
				popErrexitSuppress();
				setLastStatus(c);
				if (c == 0) {
					if (br.body) status = execNode(*br.body);
					fired = true;
					break;
				}
			}
			if (!fired && ic.else_body) status = execNode(*ic.else_body);
		} catch (...) {
			undoRedirections(rs);
			throw;
		}
		undoRedirections(rs);
		return status;
	}

	int Executor::execWhile(const WhileClause& wc) {
		RedirState rs;
		if (!applyRedirections(wc.redirs, rs)) { undoRedirections(rs); return 1; }
		int status = 0;
		++loop_depth_;
		try {
			while (true) {
				int c;
				pushErrexitSuppress();
				try { c = wc.cond ? execNode(*wc.cond) : 0; }
				catch (...) { popErrexitSuppress(); throw; }
				popErrexitSuppress();
				setLastStatus(c);
				bool keep = wc.until ? (c != 0) : (c == 0);
				if (!keep) break;
				try {
					if (wc.body) status = execNode(*wc.body);
				} catch (LoopContinue& lc) {
					if (--lc.count > 0) { --loop_depth_; undoRedirections(rs); throw; }
					continue;
				} catch (LoopBreak& lb) {
					if (--lb.count > 0) { --loop_depth_; undoRedirections(rs); throw; }
					break;
				}
			}
		} catch (...) {
			--loop_depth_;
			undoRedirections(rs);
			throw;
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
				try {
					auto fields = expander_.expandWord(w);
					for (auto& f : fields) values.push_back(std::move(f));
				} catch (const ExpandError& e) {
					std::fprintf(stderr, "wbsh: %s\n", e.what());
					--loop_depth_;
					undoRedirections(rs);
					return 1;
				}
			}
		} else {
			values = env_.positional();
		}
		try {
			for (const auto& v : values) {
				env_.set(fc.var, v);
				try {
					if (fc.body) status = execNode(*fc.body);
				} catch (LoopContinue& lc) {
					if (--lc.count > 0) { --loop_depth_; undoRedirections(rs); throw; }
					continue;
				} catch (LoopBreak& lb) {
					if (--lb.count > 0) { --loop_depth_; undoRedirections(rs); throw; }
					break;
				}
			}
		} catch (...) {
			--loop_depth_;
			undoRedirections(rs);
			throw;
		}
		--loop_depth_;
		undoRedirections(rs);
		return status;
	}

	bool Executor::patternMatches(const std::string& pat, const std::string& s) {
		return matchHere(pat, 0, s, 0);
	}

	int Executor::execCase(const CaseClause& cc) {
		RedirState rs;
		if (!applyRedirections(cc.redirs, rs)) { undoRedirections(rs); return 1; }
		int status = 0;
		try {
			std::string subject;
			try { subject = expander_.expandStringValue(cc.subject); }
			catch (const ExpandError& e) {
				std::fprintf(stderr, "wbsh: %s\n", e.what());
				undoRedirections(rs);
				return 1;
			}
			bool matched = false;
			for (std::size_t i = 0; i < cc.items.size(); ++i) {
				const auto& it = cc.items[i];
				bool m = false;
				for (const auto& pw : it.patterns) {
					std::string pat;
					try { pat = expander_.expandStringValue(pw); }
					catch (const ExpandError&) { continue; }
					if (patternMatches(pat, subject)) { m = true; break; }
				}
				if (!m) continue;
				matched = true;
				if (it.body) status = execNode(*it.body);
				if (it.term == CaseClause::Term::DSemi) break;
				if (it.term == CaseClause::Term::SemiAmp) {
					// Fall through to next item unconditionally.
					if (i + 1 < cc.items.size()) {
						const auto& nx = cc.items[i + 1];
						if (nx.body) status = execNode(*nx.body);
					}
					break;
				}
				if (it.term == CaseClause::Term::DSemiAmp) {
					// Continue testing remaining items.
					continue;
				}
				break;
			}
			(void)matched;
		} catch (...) {
			undoRedirections(rs);
			throw;
		}
		undoRedirections(rs);
		return status;
	}

	int Executor::execFunctionDef(const FunctionDef& fd) {
		functions_[fd.name] = &fd;
		return 0;
	}

	// ---------------------------------------------------------------------------
	// [[ ... ]] conditional expressions
	// ---------------------------------------------------------------------------

	static bool evalDBracketExpr(const DBracketCond::Expr& e,
	                             Expander& exp,
	                             const PathConv& pc,
	                             bool nocasematch);
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
		if (e.op.empty()) {
			// Single operand: true iff non-empty.
			return !lhs.empty();
		}
		// Unary -X tests.
		if (e.op.size() == 2 && e.op[0] == '-') {
			char op = e.op[1];
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
		std::string rhs = exp.expandStringValue(e.rhs);
		auto lower = [](std::string s) {
			for (auto& c : s) c = (char)std::tolower((unsigned char)c);
			return s;
		};
		std::string clhs = nocasematch ? lower(lhs) : lhs;
		std::string crhs = nocasematch ? lower(rhs) : rhs;
		// String compares (glob match for == != = isn't done yet).
		if (e.op == "==" || e.op == "=" || e.op == "!=") {
			bool eq = (clhs == crhs);
			return (e.op == "!=") ? !eq : eq;
		}
		if (e.op == "<")  return clhs <  crhs;
		if (e.op == ">")  return clhs >  crhs;
		if (e.op == "=~") {
			try {
				auto flags = std::regex::ECMAScript;
				if (nocasematch) flags = flags | std::regex::icase;
				std::regex re(rhs, flags);
				return std::regex_search(lhs, re);
			} catch (const std::regex_error&) {
				return false;
			}
		}
		// Arithmetic comparisons.
		if (e.op == "-eq" || e.op == "-ne" || e.op == "-lt"
		    || e.op == "-le" || e.op == "-gt" || e.op == "-ge")
		{
			long long li = 0, ri = 0;
			try { li = std::stoll(lhs); ri = std::stoll(rhs); }
			catch (...) { return false; }
			if (e.op == "-eq") return li == ri;
			if (e.op == "-ne") return li != ri;
			if (e.op == "-lt") return li <  ri;
			if (e.op == "-le") return li <= ri;
			if (e.op == "-gt") return li >  ri;
			if (e.op == "-ge") return li >= ri;
		}
		// File comparisons.
		if (e.op == "-ef" || e.op == "-nt" || e.op == "-ot") {
			std::string a = pc.toWin32(lhs);
			std::string b = pc.toWin32(rhs);
			struct stat sa{}, sb{};
			bool oa = ::stat(a.c_str(), &sa) == 0;
			bool ob = ::stat(b.c_str(), &sb) == 0;
			if (e.op == "-nt") return oa && (!ob || sa.st_mtime > sb.st_mtime);
			if (e.op == "-ot") return ob && (!oa || sa.st_mtime < sb.st_mtime);
			if (e.op == "-ef") return oa && ob
			    && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
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
		undoRedirections(rs);
		setLastStatus(status);
		return status;
	}

	// ---------------------------------------------------------------------------
	// Builtin / function tables
	// ---------------------------------------------------------------------------

	void Executor::registerBuiltin(std::string name, BuiltinFn fn) {
		builtins_[std::move(name)] = std::move(fn);
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
		auto pop_scope = [&]() {
			if (scope_stack_.empty()) return;
			auto& top = scope_stack_.back();
			for (auto rit = top.rbegin(); rit != top.rend(); ++rit) {
				if (rit->had_prev) env_.set(rit->name, rit->prev_value);
				else               env_.unset(rit->name);
			}
			scope_stack_.pop_back();
		};
		try {
			if (fd->body) status = execNode(*fd->body);
		} catch (FunctionReturn& r) {
			status = r.status;
		} catch (...) {
			pop_scope();
			--func_depth_;
			env_.setPositional(saved_positional);
			throw;
		}
		pop_scope();
		--func_depth_;
		env_.setPositional(saved_positional);
		return status;
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
		trap_handlers_.erase(it);   // prevent re-entry
		try { executeText(cmd, "<EXIT trap>"); }
		catch (ShellExit&) { /* swallow exit-from-EXIT-trap */ }
		catch (...) {}
	}

	void Executor::addHistoryEntry(std::string line) {
		if (line.empty()) return;
		if (!history_.empty() && history_.back() == line) return;
		history_.push_back(std::move(line));
		const std::size_t kMax = 5000;
		if (history_.size() > kMax) {
			history_.erase(history_.begin(),
				history_.begin() + (history_.size() - kMax));
		}
	}

	bool Executor::loadHistoryFromFile(const std::string& path) {
		std::ifstream f(utf8ToPath(path));
		if (!f) return false;
		std::string line;
		while (std::getline(f, line)) {
			while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
				line.pop_back();
			if (!line.empty()) history_.push_back(line);
		}
		const std::size_t kMax = 5000;
		if (history_.size() > kMax) {
			history_.erase(history_.begin(),
				history_.begin() + (history_.size() - kMax));
		}
		return true;
	}

	bool Executor::saveHistoryToFile(const std::string& path) const {
		std::ofstream f(utf8ToPath(path), std::ios::trunc);
		if (!f) return false;
		for (const auto& line : history_) f << line << '\n';
		return true;
	}

	std::string Executor::serializeAliases() const {
		std::vector<std::pair<std::string, std::string>> v(
			aliases_.begin(), aliases_.end());
		std::sort(v.begin(), v.end());
		std::string out;
		for (const auto& kv : v) {
			out += "alias ";
			out += kv.first;
			out += "='";
			for (char c : kv.second) {
				if (c == '\'') out += "'\\''";
				else out.push_back(c);
			}
			out += "'\n";
		}
		return out;
	}

	std::string Executor::serializeFunctions() const {
		// Iterate in deterministic (sorted) order so the env var is stable
		// across spawns. Uses each FunctionDef's pre-captured body_text.
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
			se.had_prev = env_.has(name);
			se.prev_value = env_.get(name);
			top.push_back(std::move(se));
		}
		env_.set(name, value);
	}

	// ---------------------------------------------------------------------------
	// Source-text execution + command substitution
	// ---------------------------------------------------------------------------

	int Executor::executeText(const std::string& source_text, const std::string& origin) {
		Lexer lex(source_text);
		auto tokens = lex.tokenize();
		for (const auto& e : lex.errors()) {
			std::fprintf(stderr, "wbsh: %s:%zu:%zu: %s\n",
				origin.c_str(), e.loc.line, e.loc.column, e.message.c_str());
		}
		Parser parser(std::move(tokens), source_text);
		auto root = parser.parseProgram();
		for (const auto& e : parser.errors()) {
			std::fprintf(stderr, "wbsh: %s:%zu:%zu: %s\n",
				origin.c_str(), e.loc.line, e.loc.column, e.message.c_str());
		}
		if (!root) return 1;
		// Hold the AST alive: any FunctionDefs registered during this call
		// keep raw pointers into `root`. Without this, sourced functions
		// would dangle the moment executeText returns.
		Node* root_ptr = root.get();
		owned_asts_.push_back(std::move(root));
		return execute(*root_ptr);
	}

	std::string Executor::run(const std::string& body) {
		std::string out = runRaw(body);
		// $(...) strips trailing newlines; <(...) does not.
		while (!out.empty() && out.back() == '\n') out.pop_back();
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
		try {
			executeText(body, "<command-substitution>");
		} catch (ShellExit&) {
			// Subshell semantics: exit terminates the substitution only.
		} catch (...) {
			std::fflush(stdout);
			_dup2(saved, 1);
			_close(saved);
			_wremove(utf8ToWide(tmp).c_str());
			throw;
		}
		std::fflush(stdout);
		_dup2(saved, 1);
		_close(saved);
		std::string out = readAllText(tmp);
		_wremove(utf8ToWide(tmp).c_str());
		return out;
	}

}  // namespace wbsh

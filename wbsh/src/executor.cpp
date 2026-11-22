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
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unordered_set>
#include <utility>
#include <vector>

#include "capturefile.h"
#include "coreutils_internal.h"
#include "devices.h"
#include "diagnostics.h"
#include "exitstatus.h"
#include "fdinherit.h"
#include "fifo.h"
#include "fnmatch.h"
#include "funcprint.h"
#include "jobcontrol.h"
#include "keymap.h"
#include "lexer.h"
#include "man_pages.h"
#include "numparse.h"
#include "parser.h"
#include "posixregex.h"
#include "procfs.h"
#include "signalbox.h"
#include "signals.h"
#include "stdinsync.h"
#include "testops.h"
#include "timereport.h"
#include "utf8.h"

namespace wbsh {

	using NameValueList = std::vector<std::pair<std::string, std::string>>;

	static const int kStatusCommandNotFound = 127;
	static const int kStatusBadUsage = 2;
	static const int kStatusCannotExecute = 126;
	static const std::size_t kStackReserveMargin = 1024 * 1024;
	static const int kStatusInterrupted = 130;
	static const int kStatusStopped = 148;
	static const int kStatusHangup = 129;
	static const int kSignalContinue = 18;
	static const int kSignalStop = 19;
	static const int kSignalTerminalStop = 20;
	static const std::size_t kShebangProbeBytes = 256;

	static const std::string kDevFdPrefix  = "/dev/fd/";
	static const std::string kDevTcpPrefix = "/dev/tcp/";
	static const std::string kDevUdpPrefix = "/dev/udp/";
	static const char* const kLocalOptionsName = "-";

	static const char* const kMsysPathMarkers[] = {
		"\\git\\usr\\bin\\",
		"\\git\\mingw32\\bin\\",
		"\\git\\mingw64\\bin\\",
		"\\msys64\\",
		"\\msys2\\",
		"\\cygwin\\",
		"\\cygwin64\\",
	};

	static const char* const kExecutableExtensions[] = { ".exe", ".cmd", ".bat", ".com", "" };

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
			std::string  error;
		};

		struct SelfSpawnLine {
			std::wstring cmdline;
			std::string argv_file;
		};

		// Windows takes the first entry whose name matches regardless of
		// case, so of names that differ only in case the exported ones go
		// first, in byte order: an unexported `tmp` handed to a subshell
		// must not stand in for TMP in the Win32 calls made there.
		struct EnvEntry {
			std::wstring text;
			bool exported = true;
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
	using executor_detail::EnvEntry;
	using executor_detail::SelfSpawnLine;
	using executor_detail::SpawnCommand;
	using executor_detail::StdioHandles;
#endif

	// As in bash, an expansion error drops the rest of the input line (see
	// execNode); arithmetic that a command evaluates itself, as (( )) does,
	// only fails that command.
	static void reportExpanderError(Expander& expander, bool drops_line = true) {
		const bool located = expander.errorIsLocated();
		const std::string message = expander.takeError();
		if (located) std::fprintf(stderr, "%s\n", message.c_str());
		else shellErrorf("%s\n", message.c_str());

		if (drops_line) expander.dropRestOfLine();
	}

	static void reportPathError(const std::string& path) {
		shellErrorf("%s: %s\n", path.c_str(), std::strerror(errno));
	}

	static void reportCommandNotFound(const std::string& name) {
		shellErrorf("%s: command not found\n", printableName(name).c_str());
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

	static void appendOctalByte(std::string& out, char c) {
		char octal[8];
		std::snprintf(octal, sizeof octal, "\\%03o", static_cast<unsigned char>(c));
		out += octal;
	}

	static void appendAnsiCEscape(std::string& out, char c) {
		static const char kLetters[] = "\aa\bb\x1b" "E\ff\nn\rr\tt\vv\\\\''";
		for (std::size_t k = 0; kLetters[k] != '\0'; k += 2) {
			if (kLetters[k] != c) continue;

			out.push_back('\\');
			out.push_back(kLetters[k + 1]);
			return;
		}

		if (isControlChar(c)) appendOctalByte(out, c);
		else                  out.push_back(c);
	}

	static bool isStrayByte(const std::string& word, std::size_t i) {
		if (static_cast<unsigned char>(word[i]) < 0x80) return false;

		const Utf8Char decoded = decodeUtf8(word, i);
		return decoded.length == 1 && decoded.code >= 0xDC80 && decoded.code <= 0xDCFF;
	}

	static std::string ansiCQuote(const std::string& word) {
		std::string quoted = "$'";
		for (std::size_t i = 0; i < word.size(); ++i) {
			if (isStrayByte(word, i)) appendOctalByte(quoted, word[i]);
			else                      appendAnsiCEscape(quoted, word[i]);
		}

		quoted.push_back('\'');
		return quoted;
	}

	bool needsAnsiCQuote(const std::string& word) {
		for (std::size_t i = 0; i < word.size(); ++i) {
			if (isControlChar(word[i]) || isStrayByte(word, i)) return true;
		}

		return false;
	}

	std::string shellQuoteAlways(const std::string& word) {
		return needsAnsiCQuote(word) ? ansiCQuote(word) : shellQuoteSingle(word);
	}

	std::string printableName(const std::string& name) {
		return needsAnsiCQuote(name) ? ansiCQuote(name) : name;
	}

	std::string shellQuoteForReuse(const std::string& word) {
		if (word.empty()) return "''";
		if (word == "'") return "\\'";

		if (needsAnsiCQuote(word)) return ansiCQuote(word);

		bool needs_quotes = false;
		for (std::size_t i = 0; i < word.size(); ++i) {
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

	// cmd.exe parses the command line of a batch file itself and knows no
	// backslash escapes, so a `"` inside an argument would end the quotes and
	// let & | < > run commands (CVE-2024-24576). An argument with any
	// character cmd.exe reads as syntax is quoted, `"` is doubled and `%`
	// becomes `%%cd:~,%`, which cmd.exe expands to a lone `%`. Backslashes
	// before a quote are doubled as for the C runtime, which parses the
	// argument again when the script passes it on. A line break cannot be
	// quoted for cmd.exe at all.
	static bool appendBatchArg(std::wstring& out, const std::wstring& arg) {
		// A plain word goes as it is, so `if "%1"=="install"` in the script
		// still works; anything cmd.exe could read as syntax is quoted.
		static const wchar_t kNeedsQuotes[] = L" \t\"&()[]{}^=;!'+,`~%|<>";
		if (!arg.empty() && arg.find_first_of(kNeedsQuotes) == std::wstring::npos) {
			if (arg.find_first_of(L"\r\n") != std::wstring::npos) return false;
			out += arg;
			return true;
		}

		out.push_back(L'"');
		std::size_t backslashes = 0;
		for (wchar_t c : arg) {
			if (c == L'\r' || c == L'\n' || c == L'\0') return false;
			if (c == L'\\') {
				++backslashes;
				continue;
			}

			out.append(c == L'"' ? backslashes * 2 : backslashes, L'\\');
			backslashes = 0;
			if (c == L'"') {
				out += L"\"\"";
			} else if (c == L'%') {
				out += L"%%cd:~,%";
			} else {
				out.push_back(c);
			}
		}

		out.append(backslashes * 2, L'\\');
		out.push_back(L'"');
		return true;
	}

	static std::wstring fullWindowsPath(const std::string& path) {
		const std::wstring wide = utf8ToWide(path);
		const DWORD needed = ::GetFullPathNameW(wide.c_str(), 0, nullptr, nullptr);
		if (needed == 0) return wide;

		std::wstring full(needed, L'\0');
		const DWORD length = ::GetFullPathNameW(wide.c_str(), needed, full.data(), nullptr);
		full.resize(length < needed ? length : 0);
		return full.empty() ? wide : full;
	}

	// The script goes by its full Windows path: cmd.exe reads `./x.bat` as
	// the command `.`.
	static bool buildBatchCommandLine(const std::vector<std::string>& argv,
	                                  const std::string& exec_path, std::wstring& out) {
		std::wstring line = L"\"" + cmdExePath() + L"\" /e:on /v:off /d /s /c \"";
		if (!appendBatchArg(line, fullWindowsPath(exec_path))) return false;

		for (std::size_t i = 1; i < argv.size(); ++i) {
			line.push_back(L' ');
			if (!appendBatchArg(line, utf8ToWide(argv[i]))) return false;
		}

		line.push_back(L'"');
		out = std::move(line);
		return true;
	}

	static bool isComputedExport(const std::string& name) {
		return name == "BASHOPTS" || name == "SHELLOPTS" || name == "BASH_ARGV0";
	}

	// An exported SHELLOPTS, BASHOPTS or BASH_ARGV0 carries its value as
	// the shell computes it now.
	static void appendComputedExports(const Environment& env, std::vector<EnvEntry>& entries) {
		if (env.isExported("BASH_ARGV0")) {
			entries.push_back(EnvEntry{ utf8ToWide("BASH_ARGV0=" + env.shellName()), true });
		}

		if (env.isExported("SHELLOPTS")) {
			entries.push_back(EnvEntry{ utf8ToWide("SHELLOPTS=" + env.enabledSetOptionNames()), true });
		}

		if (env.isExported("BASHOPTS")) {
			entries.push_back(EnvEntry{ utf8ToWide("BASHOPTS=" + env.enabledShoptNames()), true });
		}
	}

	static std::vector<EnvEntry> envEntries(const Environment& env, bool include_unexported) {
		std::vector<EnvEntry> entries;
		for (const auto& var : env.vars()) {
			const bool exported = env.isExported(var.first);
			if (isComputedExport(var.first)) continue;
			if (!include_unexported && !exported) continue;

			entries.push_back(EnvEntry{ utf8ToWide(var.first + "=" + var.second), exported });
		}

		appendComputedExports(env, entries);
		return entries;
	}

	static bool startsWith(const std::wstring& text, const std::wstring& prefix) {
		return text.size() >= prefix.size()
			&& std::equal(prefix.begin(), prefix.end(), text.begin());
	}

	static void overrideEnvEntry(std::vector<EnvEntry>& entries,
	                             const std::pair<std::string, std::string>& assignment) {
		const std::wstring prefix = utf8ToWide(assignment.first) + L"=";
		EnvEntry entry{ prefix + utf8ToWide(assignment.second), true };
		for (auto& existing : entries) {
			if (!startsWith(existing.text, prefix)) continue;
			existing = std::move(entry);
			return;
		}

		entries.push_back(std::move(entry));
	}

	static std::wstring_view envEntryName(const EnvEntry& entry) {
		const std::wstring_view text(entry.text);
		return text.substr(0, text.find(L'='));
	}

	static int compareFoldedNames(std::wstring_view a, std::wstring_view b) {
		const std::size_t common = a.size() < b.size() ? a.size() : b.size();
		for (std::size_t i = 0; i < common; ++i) {
			const wchar_t ca = static_cast<wchar_t>(towlower(a[i]));
			const wchar_t cb = static_cast<wchar_t>(towlower(b[i]));
			if (ca != cb) return ca < cb ? -1 : 1;
		}

		if (a.size() == b.size()) return 0;
		return a.size() < b.size() ? -1 : 1;
	}

	static bool envEntryLess(const EnvEntry& a, const EnvEntry& b) {
		const std::wstring_view name_a = envEntryName(a);
		const std::wstring_view name_b = envEntryName(b);
		const int folded = compareFoldedNames(name_a, name_b);
		if (folded != 0) return folded < 0;
		if (a.exported != b.exported) return a.exported;

		return name_a < name_b;
	}

	static std::wstring envBlockFromEntries(std::vector<EnvEntry> entries) {
		std::sort(entries.begin(), entries.end(), envEntryLess);

		std::wstring block;
		for (const auto& entry : entries) {
			block += entry.text;
			block.push_back(L'\0');
		}

		block.push_back(L'\0');
		return block;
	}

	static std::wstring buildEnvBlock(const Environment& env,
	                                  const NameValueList& overrides,
	                                  bool include_unexported = false) {
		std::vector<EnvEntry> entries = envEntries(env, include_unexported);
		for (const auto& assignment : overrides) overrideEnvEntry(entries, assignment);
		return envBlockFromEntries(std::move(entries));
	}

	static std::string lastErrorString() {
		const DWORD error = ::GetLastError();
		if (error == 0) return {};

		LPWSTR buffer = nullptr;
		const DWORD length = ::FormatMessageW(
			FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
			| FORMAT_MESSAGE_IGNORE_INSERTS,
			nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);

		std::string message;
		if (buffer != nullptr && length != 0) message = wideToUtf8(std::wstring(buffer, length));
		if (buffer != nullptr) ::LocalFree(buffer);

		while (!message.empty()
		       && (message.back() == '\n' || message.back() == '\r' || message.back() == ' ')) {
			message.pop_back();
		}

		return message;
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

	// _get_osfhandle gives -2 for an fd with no stream behind it, which
	// as a HANDLE would be a pseudo-handle CreateProcess rejects.
	static HANDLE stdioHandle(int fd) {
		const intptr_t raw = _get_osfhandle(fd);
		if (raw == -2) return INVALID_HANDLE_VALUE;

		return reinterpret_cast<HANDLE>(raw);
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
		accountChildProcess(process);
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

	static void addInheritHandle(std::vector<HANDLE>& inherits, HANDLE handle) {
		if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return;
		if (std::find(inherits.begin(), inherits.end(), handle) != inherits.end()) return;

		inherits.push_back(handle);
		::SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
	}

	static std::vector<HANDLE> collectInheritHandles(const StdioHandles& stdio,
	                                                 const ChildFds& fds) {
		std::vector<HANDLE> inherits;
		addInheritHandle(inherits, stdio.in);
		addInheritHandle(inherits, stdio.out);
		addInheritHandle(inherits, stdio.err);

		for (void* handle : fds.handles) addInheritHandle(inherits, static_cast<HANDLE>(handle));
		return inherits;
	}

	static bool attachInheritList(LPPROC_THREAD_ATTRIBUTE_LIST attr_list,
	                              std::vector<HANDLE>& inherits) {
		return ::UpdateProcThreadAttribute(attr_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
			inherits.data(), inherits.size() * sizeof(HANDLE), nullptr, nullptr) != 0;
	}

	static bool attachPseudoConsole(LPPROC_THREAD_ATTRIBUTE_LIST attr_list, HPCON console) {
		return ::UpdateProcThreadAttribute(attr_list, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
			console, sizeof(console), nullptr, nullptr) != 0;
	}

	// Null standard handles with STARTF_USESTDHANDLES are what make a child
	// adopt the pseudoconsole's own; given the shell's, it would write past
	// the pseudoconsole while still being attached to it.
	static StdioHandles pseudoConsoleStdio() {
		StdioHandles stdio;
		stdio.in  = nullptr;
		stdio.out = nullptr;
		stdio.err = nullptr;
		return stdio;
	}

	static STARTUPINFOEXW makeStartupInfo(const StdioHandles& stdio, ChildFds& fds,
	                                      LPPROC_THREAD_ATTRIBUTE_LIST attr_list) {
		STARTUPINFOEXW info{};
		info.StartupInfo.cb = sizeof(STARTUPINFOEXW);
		info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		info.StartupInfo.hStdInput  = stdio.in;
		info.StartupInfo.hStdOutput = stdio.out;
		info.StartupInfo.hStdError  = stdio.err;
		info.lpAttributeList = attr_list;

		if (!fds.block.empty()) {
			info.StartupInfo.cbReserved2 = static_cast<WORD>(fds.block.size());
			info.StartupInfo.lpReserved2 = fds.block.data();
		}

		return info;
	}

	static HANDLE lifetimeJobFor(const SpawnPlacement& placement) {
		return placement.outlives_shell ? nullptr : childLifetimeJob();
	}

	static bool startsSuspended(const SpawnPlacement& placement) {
		return placement.job != nullptr || lifetimeJobFor(placement) != nullptr;
	}

	// A shell started without a console (DETACHED_PROCESS, as some agent
	// hosts start it) would have Windows give every console program it
	// runs a new, visible console window; a hidden one is given instead.
	static DWORD consoleFlags(const SpawnPlacement& placement) {
		const DWORD chosen = CREATE_NEW_CONSOLE | CREATE_NO_WINDOW | DETACHED_PROCESS;
		if (placement.pseudo_console != nullptr) return 0;
		if ((placement.creation_flags & chosen) != 0) return 0;

		return ::GetConsoleCP() != 0 ? 0 : CREATE_NO_WINDOW;
	}

	static DWORD placementFlags(const SpawnPlacement& placement) {
		DWORD flags = placement.creation_flags | consoleFlags(placement);
		if (startsSuspended(placement)) flags |= CREATE_SUSPENDED;
		if (placement.new_process_group) flags |= CREATE_NEW_PROCESS_GROUP;
		return flags;
	}

	// The lifetime job first: a group job assigned after it nests inside
	// it, so the group still stops and resumes as one.
	static void releasePlacedChild(const PROCESS_INFORMATION& process,
	                               const SpawnPlacement& placement) {
		if (!startsSuspended(placement)) return;

		const HANDLE lifetime = lifetimeJobFor(placement);
		if (lifetime != nullptr) ::AssignProcessToJobObject(lifetime, process.hProcess);
		if (placement.job != nullptr) ::AssignProcessToJobObject(placement.job, process.hProcess);
		::ResumeThread(process.hThread);
	}

	// Only the stdio handles and the descriptors the user opened
	// (fdinherit.h) are inherited: anything else the shell holds open
	// (saved fds, pipe ends of other stages) would otherwise leak into the
	// child and keep pipes alive after it is gone.
	// The inherit list and the fd block are referenced by attr_list and
	// the startup info until CreateProcess returns, so they must live in
	// this frame (not in a helper) — per the Win32 attribute-list
	// ownership rules.
	// On failure GetLastError() still holds CreateProcess's error.
	static HANDLE createWithHandleList(SpawnCommand& command, const StdioHandles& stdio,
	                                   const SpawnPlacement& placement) {
		giveBackStdinReadAhead();
		LPPROC_THREAD_ATTRIBUTE_LIST attr_list = allocAttrList();
		if (attr_list == nullptr) return INVALID_HANDLE_VALUE;

		const bool on_pseudo_console = placement.pseudo_console != nullptr;
		const StdioHandles child_stdio = on_pseudo_console ? pseudoConsoleStdio() : stdio;
		ChildFds child_fds = on_pseudo_console ? ChildFds() : collectChildFds();
		std::vector<HANDLE> inherits = collectInheritHandles(child_stdio, child_fds);
		const bool attached = on_pseudo_console
			? attachPseudoConsole(attr_list, placement.pseudo_console)
			: inherits.empty() || attachInheritList(attr_list, inherits);
		if (!attached) {
			freeAttrList(attr_list);
			return INVALID_HANDLE_VALUE;
		}

		STARTUPINFOEXW info = makeStartupInfo(child_stdio, child_fds, attr_list);
		const std::wstring directory = childCurrentDirectory();
		PROCESS_INFORMATION process{};
		const BOOL created = ::CreateProcessW(command.exe.c_str(), command.cmdline.data(),
			nullptr, nullptr, inherits.empty() ? FALSE : TRUE,
			EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | placementFlags(placement),
			command.envblock.data(), optionalDirectory(directory), &info.StartupInfo, &process);
		const DWORD error = ::GetLastError();
		freeAttrList(attr_list);

		if (created == 0) {
			::SetLastError(error);
			return INVALID_HANDLE_VALUE;
		}

		releasePlacedChild(process, placement);
		::CloseHandle(process.hThread);
		return process.hProcess;
	}

	static HANDLE spawnWithHandles(SpawnCommand& command, const StdioHandles& stdio,
	                               const SpawnPlacement& placement = SpawnPlacement()) {
		const HANDLE process = createWithHandleList(command, stdio, placement);
		if (process == INVALID_HANDLE_VALUE) {
			shellErrorf("CreateProcess failed: %s\n", lastErrorString().c_str());
		}

		return process;
	}

	static SpawnCommand makeSpawnCommand(const std::vector<std::string>& argv,
	                                     const std::string& exec_path,
	                                     std::wstring envblock) {
		SpawnCommand command;
		command.exe      = utf8ToWide(exec_path);
		command.cmdline  = buildCommandLine(argv);
		command.envblock = std::move(envblock);
		if (isBatchFile(exec_path)) {
			command.exe = cmdExePath();
			if (!buildBatchCommandLine(argv, exec_path, command.cmdline)) {
				command.error = "an argument with a line break cannot be passed to a batch file";
			}
		}

		return command;
	}

	static bool reportSpawnCommandError(const SpawnCommand& command, const std::string& name) {
		if (command.error.empty()) return false;

		shellErrorf("%s: %s\n", name.c_str(), command.error.c_str());
		return true;
	}
#endif  // _WIN32

	static bool finishedOrReported(Expander& expander) {
		if (!expander.failed()) return true;

		if (!expander.fatalErrorPending()) reportExpanderError(expander);
		return false;
	}

	// The value was expanded when it was assigned; bash expands nothing in
	// it again, so `$(...)` there is an error rather than a command.
	static bool evalIntegerAttribute(void* context, const std::string& expr, long long& out) {
		Expander& expander = *static_cast<Expander*>(context);
		if (expander.failed()) return false;

		out = expander.evalArithLiteral(expr);
		return finishedOrReported(expander);
	}

	// `printf -v 'a[$i]'` and the like: bash expands such a subscript again
	// unless array_expand_once says not to.
	static bool evalElementSubscript(void* context, const std::string& expr, long long& out) {
		Expander& expander = *static_cast<Expander*>(context);
		if (expander.failed()) return false;

		const bool once = expander.environment().shopts().array_expand_once;
		out = once ? expander.evalArithLiteral(expr) : expander.evalArith(expr);
		return finishedOrReported(expander);
	}

	static std::atomic<bool> g_exec_exit_known{ false };
	static std::atomic<unsigned long> g_exec_exit_code{ 0 };

	void rememberExecExitCode(unsigned long code) {
		g_exec_exit_code.store(code);
		g_exec_exit_known.store(true);
	}

	void forgetExecExitCode() {
		g_exec_exit_known.store(false);
	}

	int processExitCode(int status) {
		if (!g_exec_exit_known.load()) return status;

		const unsigned long code = g_exec_exit_code.load();
		const bool same = shellStatusFromExitCode(static_cast<std::uint32_t>(code)) == status;
		return same ? static_cast<int>(code) : status;
	}

	static bool fdIsPipe(int fd);
	static void moveFdTo(int fd, int target);

	// Room for every bundled command, so registering them never rehashes.
	static const std::size_t kBuiltinTableSize = 512;

	Executor::Executor(Environment& env)
		: env_(env), expander_(env, this) {
		stdout_started_as_pipe_ = fdIsPipe(1);
		env_.setArithEvaluator(evalIntegerAttribute, &expander_);
		env_.setSubscriptEvaluator(evalElementSubscript);
		builtins_.reserve(kBuiltinTableSize);
		registerCoreBuiltins(*this);
		registerCoreutils(*this);
		loadUtils(*this);
		publishSignalDispositions();
	}

	int Executor::execute(const Node& root) {
		return execNode(root);
	}

	// A command substitution's commands take their lines from the body
	// as bash reprints it.
	int Executor::nodeLine(const Node& node) const {
		if (line_overrides_.empty()) return static_cast<int>(node.loc.line);

		const auto found = line_overrides_.find(&node);
		return found != line_overrides_.end() ? found->second : static_cast<int>(node.loc.line);
	}

	// Where the command's first word ends, the line bash gives the first
	// command of a substitution in it.
	int Executor::firstWordEndLine(const SimpleCommand& sc) const {
		const std::string* first = nullptr;
		if (!sc.assignments.empty()) first = &sc.assignments[0].value.raw;
		else if (!sc.words.empty()) first = &sc.words[0].raw;

		const int newlines = first == nullptr ? 0
			: static_cast<int>(std::count(first->begin(), first->end(), '\n'));
		return nodeLine(sc) + newlines;
	}

	// The body's parse errors and warnings count from the command's line.
	int Executor::parseBaseLine() const {
		const int lineno = env_.currentLineno();
		return lineno > 0 ? lineno : 1;
	}

	int Executor::substitutionBaseLine() const {
		return command_first_line_ > 0 ? command_first_line_ : parseBaseLine();
	}

	// A function, `eval` or `source` moves `$LINENO` into its own text;
	// once it returns, the line is the caller's again, as an ERR trap
	// for the call itself sees it.
	int Executor::execNode(const Node& node) {
		if (env_.noexec() && !env_.interactive()) return 0;
		const bool tracks_line = nodeTracksLineno(node);
		if (tracks_line) env_.setCurrentLineno(nodeLine(node));

		const int outer_first_line = std::exchange(command_first_line_, 0);
		const std::size_t proc_substs = procSubstWatermark();
		const int status = dispatchNode(node);
		releaseProcessSubstitutionsSince(proc_substs);
		command_first_line_ = outer_first_line;
		if (tracks_line) env_.setCurrentLineno(nodeLine(node));
		exitOnFatalExpansionError();
		discardAfterExpansionError();
		return status;
	}

	// Inside a function bash leaves the line where it was for `select`.
	bool Executor::nodeTracksLineno(const Node& node) const {
		if (node.loc.line == 0) return false;
		const bool select = node.kind == Node::Kind::ForClause
			&& static_cast<const ForClause&>(node).is_select;
		if (select && func_depth_ > 0) return false;

		return frozen_lineno_source_ == nullptr || node.source_text != frozen_lineno_source_;
	}

	// POSIX: a failed `${name?}` or a `set -u` error ends a
	// non-interactive shell; a subshell or `$(...)` ends just itself.
	void Executor::exitOnFatalExpansionError() {
		if (!expander_.takeFatalError()) return;
		if (env_.interactive() || flowPending()) return;

		raiseExit(1);
	}

	// bash stops the whole command after an expansion error: the rest of
	// the input line, out of loops and functions. The input list drops the
	// line; a subshell, `$(...)` or trap action ends with status 1.
	// POSIX mode: an expansion or assignment error ends a non-interactive shell.
	void Executor::discardAfterExpansionError() {
		if (!expander_.takeDropLine() || flowPending()) return;

		if (posixErrorsExit()) raiseExit(1);
		else raiseDiscard();
	}

	int Executor::dispatchNode(const Node& node) {
		switch (node.kind) {
		case Node::Kind::List:          return execList(static_cast<const List&>(node));
		case Node::Kind::AndOr:         return execAndOr(static_cast<const AndOr&>(node));
		case Node::Kind::Pipeline:      return execPipeline(static_cast<const Pipeline&>(node));
		case Node::Kind::SimpleCommand:
			if (!announceLeaf(node)) return last_status_;
			return finishLeafCommand(execSimpleCommand(static_cast<const SimpleCommand&>(node)));
		case Node::Kind::BraceGroup:    return execBraceGroup(static_cast<const BraceGroup&>(node));
		case Node::Kind::Subshell:
			return finishLeafCommand(execSubshell(static_cast<const Subshell&>(node)));
		case Node::Kind::IfClause:      return execIf(static_cast<const IfClause&>(node));
		case Node::Kind::WhileClause:   return execWhile(static_cast<const WhileClause&>(node));
		case Node::Kind::ForClause:     return execFor(static_cast<const ForClause&>(node));
		case Node::Kind::CaseClause:    return execCase(static_cast<const CaseClause&>(node));
		case Node::Kind::FunctionDef:
			return execFunctionDef(static_cast<const FunctionDef&>(node));
		case Node::Kind::DBracket:
			if (!announceLeaf(node)) return last_status_;
			return finishLeafCommand(execDBracket(static_cast<const DBracketCond&>(node)));
		case Node::Kind::ArithCommand:
			if (!announceCommand("((" + static_cast<const ArithCommand&>(node).expr + "))")) {
				return last_status_;
			}
			if (flowPending()) return last_status_;
			return finishLeafCommand(execArithCommand(static_cast<const ArithCommand&>(node)));
		case Node::Kind::Coproc:
			return execCoproc(static_cast<const CoprocCommand&>(node));
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

	// bash shows a job as it reprints the command.
	static std::string backgroundJobText(const Node& node) {
		const std::string printed = commandText(node);
		return printed.empty() ? nodeSourceSlice(node, std::string()) : printed;
	}

	bool Executor::announceNode(const Node& node) {
		return announceCommand(nodeSourceSlice(node, source_text_));
	}

	// False when the DEBUG trap skipped the command or left a flow signal.
	bool Executor::announceLeaf(const Node& node) {
		return announceNode(node) && !flowPending();
	}

	int Executor::negateBangStatus(int status) {
		status_judged_ = true;
		return (status == 0) ? 1 : 0;
	}

	// Besides external programs, wbsh's built-in utilities (cat, sort,
	// ...) are programs that bash forks.
	bool Executor::runsAsProgram(const std::string& name) const {
		if (isFunction(name)) return false;

		const ManPage* page = findManPage(name);
		return !isBuiltin(name) || page == nullptr || page->kind != ManKind::Shell;
	}

	// A compound command sets no line number of its own: as in bash, its
	// redirection errors name the line the input was read through.
	bool Executor::applyCompoundRedirections(const std::vector<Redirection>& redirs,
			RedirState& state) {
		if (redirs.empty()) return true;

		const int line = env_.currentLineno();
		if (func_depth_ == 0 && input_command_end_line_ > 0) {
			env_.setCurrentLineno(input_command_end_line_);
		}

		const bool applied = applyRedirectionsOrUndo(redirs, state);
		if (applied) env_.setCurrentLineno(line);
		return applied;
	}

	bool Executor::applyRedirectionsOrUndo(const std::vector<Redirection>& redirs,
	                                       RedirState& state) {
		if (applyRedirections(redirs, state)) return true;
		undoRedirections(state);
		return false;
	}

	void Executor::runBody(const Node* body, int& status) {
		const int result = (body != nullptr) ? execNode(*body) : 0;
		const bool carries_status = flow_.kind == FlowSignal::Kind::Break
			|| flow_.kind == FlowSignal::Kind::Continue || flow_.kind == FlowSignal::Kind::Discard;
		if (!flowPending() || carries_status) status = result;
	}

#ifdef _WIN32
	static int dupAboveUserFds(int fd);

	struct CoprocPipes {
		HANDLE shell_read = INVALID_HANDLE_VALUE;
		HANDLE shell_write = INVALID_HANDLE_VALUE;
		HANDLE child_in = INVALID_HANDLE_VALUE;
		HANDLE child_out = INVALID_HANDLE_VALUE;
	};

	static bool createCoprocPipes(CoprocPipes& pipes) {
		if (!::CreatePipe(&pipes.shell_read, &pipes.child_out, nullptr, 0)) return false;
		if (::CreatePipe(&pipes.child_in, &pipes.shell_write, nullptr, 0)) return true;

		::CloseHandle(pipes.shell_read);
		::CloseHandle(pipes.child_out);
		return false;
	}

	// bash moves all four pipe ends to the highest free descriptors below
	// 64 and keeps the first (the read end) and the last (the write end).
	static std::vector<int> highestFreeFds(std::size_t count) {
		static const int kHighFdLimit = 64;

		std::vector<int> fds;
		for (int fd = kHighFdLimit - 1; fd >= 0 && fds.size() < count; --fd) {
			if (_get_osfhandle(fd) == -1) fds.push_back(fd);
		}

		return fds;
	}

	static int shellFdFor(HANDLE handle, int flags, int target) {
		const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), flags | _O_BINARY);
		if (fd < 0) {
			::CloseHandle(handle);
			return -1;
		}

		const int high = target >= 0 && _dup2(fd, target) == 0 ? target : dupAboveUserFds(fd);
		_close(fd);
		if (high >= 0) keepFdFromChildren(high);
		return high;
	}

	void Executor::publishCoprocArray(const std::string& name, int read_fd, int write_fd) {
		if (env_.isReadonly(name)) {
			shellErrorf("%s: readonly variable\n", name.c_str());
			return;
		}

		env_.dropUnboundNameref(name);
		env_.setIndexedArrayFromList(name, { std::to_string(read_fd), std::to_string(write_fd) });
	}

	void Executor::publishCoproc(const std::string& name, const CoprocPipes& pipes,
	                             HANDLE process) {
		static const std::size_t kPipeEnds = 4;

		const std::vector<int> high = highestFreeFds(kPipeEnds);
		const bool fits = high.size() == kPipeEnds;
		const int read_fd = shellFdFor(pipes.shell_read, _O_RDONLY, fits ? high.front() : -1);
		const int write_fd = shellFdFor(pipes.shell_write, _O_WRONLY, fits ? high.back() : -1);
		publishCoprocArray(name, read_fd, write_fd);

		const long long pid = static_cast<long long>(::GetProcessId(process));
		coproc_ = { name, read_fd, write_fd, pid };
		if (env_.readonlySet().count(name + "_PID") == 0) env_.set(name + "_PID", std::to_string(pid));
		env_.setLastBgPid(pid);

		const int job_id = registerJob(process, pid, "coproc " + name);
		if (env_.interactive()) std::fprintf(stderr, "[%d] %lld\n", job_id, pid);
	}

	// The coprocess reads what the shell writes to NAME[1] and writes what
	// the shell reads from NAME[0]; it runs asynchronously, like `&`.
	static bool isLoopVariableName(const std::string& name);

	int Executor::execCoproc(const CoprocCommand& coproc) {
		if (!isLoopVariableName(coproc.name)) {
			shellErrorf("`%s': not a valid identifier\n", coproc.name.c_str());
			return 1;
		}

		CoprocPipes pipes;
		if (coproc.command == nullptr || !createCoprocPipes(pipes)) {
			shellErrorf("coproc: cannot create pipes\n");
			return 1;
		}

		std::fflush(stdout);
		std::fflush(stderr);
		const HANDLE process = launchPipelineElement(*coproc.command, pipes.child_in,
			pipes.child_out, currentStdio().err);
		::CloseHandle(pipes.child_in);
		::CloseHandle(pipes.child_out);
		if (process == INVALID_HANDLE_VALUE) {
			::CloseHandle(pipes.shell_read);
			::CloseHandle(pipes.shell_write);
			return 1;
		}

		publishCoproc(coproc.name, pipes, process);
		return 0;
	}
#else
	int Executor::execCoproc(const CoprocCommand&) {
		shellErrorf("coproc: not supported on this platform\n");
		return 1;
	}
#endif

#ifdef _WIN32
	int Executor::launchBackgroundCommand(const Node& cmd) {
		const StdioHandles stdio = currentStdio();
		const HANDLE group = createProcessGroup();
		spawn_placement_.job = group;
		spawn_placement_.new_process_group = true;
		spawn_placement_.outlives_shell = true;
		const HANDLE process = launchPipelineElement(cmd, stdio.in, stdio.out, stdio.err);
		spawn_placement_ = SpawnPlacement();
		if (process == INVALID_HANDLE_VALUE) {
			if (group != nullptr) ::CloseHandle(group);
			return 1;
		}

		const long long pid = static_cast<long long>(::GetProcessId(process));
		const int job_id = registerJob(process, pid, backgroundJobText(cmd));
		jobs_.back().group = group;
		env_.setLastBgPid(pid);
		if (env_.interactive()) std::fprintf(stderr, "[%d] %lld\n", job_id, pid);
		return 0;
	}
#endif

	bool Executor::stopForCtrlC(int& status) const {
		if (!ctrlCPending()) return false;

		status = kStatusInterrupted;
		return true;
	}

	static int lastLineOf(const Node& node);

	int Executor::execList(const List& list) {
		int status = 0;
		for (std::size_t index = 0; index < list.items.size(); ++index) {
			if (stopForCtrlC(status)) return status;

			const ListItem& item = list.items[index];
			if (&list == input_echo_.list) {
				echoInputBefore(list, index);
				input_command_end_line_ = lastLineOf(*item.command);
			}

			status_judged_ = false;
#ifdef _WIN32
			if (item.background) status = launchBackgroundCommand(*item.command);
			else                 status = execNode(*item.command);
#else
			status = execNode(*item.command);
#endif
			// As in bash, a line dropped after an expansion error neither
			// fires the ERR trap nor ends the shell under set -e, and fails
			// with the status its error set, else 1.
			const bool dropped = dropRestOfInputLine(list, index);
			if (dropped && status == 0) status = 1;
			if (flowPending()) return status;
			setLastStatus(status);
			handlePendingSignals();
			if (flowPending()) return status;

			if (!dropped && !item.background && handleCommandFailure(status)) return status;
			if (stopToRelex(list, index)) return status;
		}

		return status;
	}

	static std::size_t inputReadThrough(const List& list, std::size_t index,
			const std::string& text);
	static std::size_t commandLineEnd(const Node& command, const std::string& text);

	// The input list takes a Discard: it skips the other items that start
	// before the end of the line, as bash had read them with this one.
	bool Executor::dropRestOfInputLine(const List& list, std::size_t& index) {
		if (&list != input_echo_.list || !consumeFlow(FlowSignal::Kind::Discard)) return false;

		const std::size_t line_end = inputReadThrough(list, index, *input_echo_.text);
		while (index + 1 < list.items.size()
				&& list.items[index + 1].command->loc.offset < line_end) {
			++index;
		}

		return true;
	}

	// bash reads a line at a time, so the posix option and aliases a line
	// changes count from the next line on: the rest of the input is lexed
	// again.
	bool Executor::stopToRelex(const List& list, std::size_t index) {
		if (&list != input_echo_.list) return false;

		const bool posix_changed = env_.options().posix != input_echo_.lexed_posix;
		if (!posix_changed && aliasState() == input_echo_.lexed_aliases) return false;

		const bool more_items = index + 1 < list.items.size();
		const std::size_t line_end = more_items
			? inputReadThrough(list, index, *input_echo_.text)
			: commandLineEnd(*list.items[index].command, *input_echo_.text);
		if (more_items && list.items[index + 1].command->loc.offset < line_end) return false;
		if (line_end >= input_echo_.text->size()) return false;

		relex_offset_ = line_end;
		return true;
	}

	// True when the shell must stop: the ERR trap left a flow signal, or
	// errexit ends the shell.
	bool Executor::handleCommandFailure(int status) {
		const bool judged = status_judged_;
		status_judged_ = status != 0;
		if (status == 0 || judged || errexit_suppress_ > 0) return false;

		fireErrTrap(status);
		if (flowPending()) return true;
		if (!env_.errexit()) return false;

		raiseExit(status);
		return true;
	}

	int Executor::finishLeafCommand(int status) {
		setPipeStatus({ status });
		status_judged_ = call_set_err_trap_;
		call_set_err_trap_ = false;
		return status;
	}

	// Nearly every command leaves a one-element PIPESTATUS where one
	// already is, which needs no new array.
	static bool overwriteSingleStatus(Environment& env, const std::vector<int>& statuses) {
		if (statuses.size() != 1) return false;

		Environment::IndexedArray* array = env.plainIndexedArray("PIPESTATUS");
		if (array == nullptr || array->size() != 1 || array->begin()->first != 0) return false;

		array->begin()->second = std::to_string(statuses.front());
		return true;
	}

	void Executor::setPipeStatus(const std::vector<int>& statuses) {
		if (overwriteSingleStatus(env_, statuses)) return;

		std::vector<std::string> values;
		values.reserve(statuses.size());
		for (const int status : statuses) values.push_back(std::to_string(status));

		env_.setIndexedArrayFromList("PIPESTATUS", std::move(values));
	}

	// `a && b || c` parses as a tree that leans left: ((a && b) || c). A
	// loop over its links, not a call per level, keeps a chain of many
	// thousand stages off the stack. As before, set -e ignores every stage
	// but the last.
	int Executor::execAndOr(const AndOr& and_or) {
		std::vector<const AndOr*> links;
		const Node* first = &and_or;
		while (first->kind == Node::Kind::AndOr) {
			links.push_back(static_cast<const AndOr*>(first));
			first = links.back()->left;
		}

		pushErrexitSuppress();
		int status = execNode(*first);
		popErrexitSuppress();
		if (flowPending()) return status;
		setLastStatus(status);

		for (std::size_t k = links.size(); k-- > 0;) {
			const AndOr& link = *links[k];
			const bool skip = (link.op == AndOr::Op::AndIf) ? (status != 0) : (status == 0);
			if (skip) {
				status_judged_ = true;
				continue;
			}

			const bool last = k == 0;
			if (!last) pushErrexitSuppress();
			status = execNode(*link.right);
			if (!last) popErrexitSuppress();
			if (flowPending()) return status;
			setLastStatus(status);
		}

		return status;
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

		// An empty TIMEFORMAT silences the report, as in bash.
		static std::string timeReportFormat(Executor& exec, bool posix) {
			if (posix) return kTimeFormatPosix;
			if (!exec.env().has("TIMEFORMAT")) return kTimeFormatDefault;
			return exec.env().get("TIMEFORMAT");
		}

		static bool isNullCommand(const Pipeline& pipeline) {
			if (pipeline.commands.size() != 1) return false;
			if (pipeline.commands[0]->kind != Node::Kind::SimpleCommand) return false;

			const auto& command = static_cast<const SimpleCommand&>(*pipeline.commands[0]);
			return command.words.empty() && command.redirs.empty();
		}

		// POSIX mode: `time` alone reports the shell's and its children's
		// times since the shell started.
		static bool timesWholeShell(Executor& exec, const Pipeline& pipeline) {
			return exec.env().options().posix && isNullCommand(pipeline);
		}

		static CpuSample shellStartSample(Executor& exec) {
			CpuSample start;
			start.real_ms = sampleCpuTimes().real_ms - exec.env().secondsSinceStart() * 1000;
			return start;
		}

		static std::string timeReportFormat(Executor& exec, const Pipeline& pipeline) {
			const bool unset = !exec.env().has("TIMEFORMAT");
			if (!pipeline.time_posix && unset && timesWholeShell(exec, pipeline)) {
				return "user\t%2lU\nsys\t%2lS";
			}

			return timeReportFormat(exec, pipeline.time_posix);
		}

		struct PipelineTimeGuard {
			Executor* exec;
			const Pipeline& pipeline;
			CpuSample start;

			PipelineTimeGuard(Executor* owner, const Pipeline& timed)
				: exec(owner), pipeline(timed) {
				if (!pipeline.timed) return;

				start = timesWholeShell(*exec, pipeline) ? shellStartSample(*exec) : sampleCpuTimes();
			}

			~PipelineTimeGuard() {
				if (!pipeline.timed) return;

				const std::string format = timeReportFormat(*exec, pipeline);
				if (format.empty()) return;

				const std::string report = formatTimeReport(format, start, sampleCpuTimes());
				std::fputs(report.c_str(), stderr);
				std::fflush(stderr);
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

			shellErrorf("pipe creation failed: %s\n", lastErrorString().c_str());
			closeCreatedPipes(pipe_r, pipe_w, i);
			return false;
		}

		return true;
	}

	static int pipelineStatus(const std::vector<int>& statuses, bool pipefail) {
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

	// The pipe end the element run here uses, taken out of the vectors so
	// that closing the rest leaves it open.
	static HANDLE takeLocalStageEnd(PipelineLocalStage local, std::vector<HANDLE>& pipe_r,
	                                std::vector<HANDLE>& pipe_w) {
		HANDLE end = INVALID_HANDLE_VALUE;
		if (local == PipelineLocalStage::LastHere) std::swap(end, pipe_r.back());
		if (local == PipelineLocalStage::FirstInSubshell) std::swap(end, pipe_w.front());
		return end;
	}

	static std::size_t localStageIndex(PipelineLocalStage local, std::size_t count) {
		return local == PipelineLocalStage::LastHere ? count - 1 : 0;
	}

	static void flushStdStreams() {
		std::fflush(stdout);
		std::fflush(stderr);
	}

	static void placeChildStatuses(const std::vector<int>& statuses,
	                               const std::vector<std::size_t>& process_stage,
	                               std::vector<int>& stage_status) {
		for (std::size_t k = 0; k < statuses.size(); ++k) {
			stage_status[process_stage[k]] = statuses[k];
		}
	}

	int Executor::execPipelineMultiCmd(const Pipeline& pipeline) {
		const std::size_t count = pipeline.commands.size();

		std::vector<HANDLE> pipe_r;
		std::vector<HANDLE> pipe_w;
		if (!createPipelinePipes(count, pipe_r, pipe_w)) return 1;

		flushStdStreams();
		const StdioHandles outer = currentStdio();

		// As in bash, an element that cannot start (not found, a failed
		// expansion) gets its own status and the others still run, so
		// `nosuchcmd | cat` gives PIPESTATUS 127 0.
		const HANDLE group = startForegroundGroup();
		const PipelineLocalStage local = chooseLocalStage();
		std::vector<HANDLE> processes;
		std::vector<std::size_t> process_stage;
		std::vector<int> stage_status(count, 1);
		const auto launch = [&](std::size_t i) {
			const StdioHandles stdio = pipelineStageStdio(pipeline, i, pipe_r, pipe_w, outer);
			launch_status_ = 1;
			const HANDLE process = launchPipelineElement(*pipeline.commands[i],
				stdio.in, stdio.out, stdio.err);
			if (process == INVALID_HANDLE_VALUE) {
				stage_status[i] = launch_status_;
				return;
			}

			processes.push_back(process);
			process_stage.push_back(i);
		};

		const std::size_t spawned = local == PipelineLocalStage::LastHere ? count - 1 : count;
		for (std::size_t i = 0; i < spawned; ++i) {
			const Node& stage = *pipeline.commands[i];
			if (stage.kind == Node::Kind::SimpleCommand) announceNode(stage);
			if (i != 0 || local != PipelineLocalStage::FirstInSubshell) launch(i);
			if (flowPending()) break;
		}

		const HANDLE local_end = takeLocalStageEnd(local, pipe_r, pipe_w);
		closeValidHandles(pipe_r);
		closeValidHandles(pipe_w);
		if (local != PipelineLocalStage::None) {
			stage_status[localStageIndex(local, count)] = runLocalStage(pipeline, local, local_end);
		}

		std::vector<int> statuses;
		const int foreground = waitForegroundProcesses(processes, group,
			nodeSourceSlice(pipeline, source_text_), statuses);
		if (foreground == kStatusStopped) return foreground;

		placeChildStatuses(statuses, process_stage, stage_status);
		setPipeStatus(stage_status);
		status_judged_ = false;
		return pipelineStatus(stage_status, env_.pipefail());
	}

	// With lastpipe and no job control, bash runs the last element in this
	// shell, so `... | read v` sets v here.
	bool Executor::runsLastStageHere() const {
		return env_.lastpipe() && !env_.options().monitor && !jobControlActive();
	}

	static int openPipeReadFd(HANDLE input) {
		const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(input), _O_RDONLY | _O_BINARY);
		if (fd < 0) ::CloseHandle(input);
		return fd;
	}

	int Executor::runLastStageHere(const Node& stage, HANDLE input) {
		const int fd = openPipeReadFd(input);
		if (fd < 0) return 1;

		if (flowPending()) {
			_close(fd);
			return 1;
		}

		RedirState state;
		saveFd(state, 0);
		moveFdTo(fd, 0);
		const int status = execNode(stage);
		undoRedirections(state);
		return status;
	}

	// Without job control the first element runs as a subshell inside this
	// process, as `( ... )` does, which saves starting a shell for it. It is
	// the first because its writes land in the pipe at once, as a forked
	// bash subshell's do, while the readers start; a reader run here would
	// often finish before a slow-starting writer wrote, which then saw a
	// broken pipe. Not while jobs exist: the job table is not part of the
	// subshell's snapshot, so a `wait` in it would reap this shell's jobs.
	bool Executor::runsFirstStageInSubshell() const {
		return !env_.options().monitor && !jobControlActive() && jobs_.empty();
	}

	PipelineLocalStage Executor::chooseLocalStage() const {
		if (runsLastStageHere()) return PipelineLocalStage::LastHere;
		if (runsFirstStageInSubshell()) return PipelineLocalStage::FirstInSubshell;
		return PipelineLocalStage::None;
	}

	static int openPipeWriteFd(HANDLE output) {
		const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(output), _O_WRONLY | _O_BINARY);
		if (fd < 0) ::CloseHandle(output);
		return fd;
	}

	// A forked writer dies of SIGPIPE when its reader is gone; this one
	// ends its subshell with that status (see exitIfStdoutReaderGone).
	// History is not in the subshell snapshot, so the stage's own changes
	// to it are undone here, as a stage process's would be lost.
	int Executor::runFirstStageInSubshell(const Node& stage, HANDLE output, bool merge_stderr) {
		const int fd = openPipeWriteFd(output);
		if (fd < 0) return 1;

		if (flowPending()) {
			_close(fd);
			return 1;
		}

		RedirState state;
		saveFd(state, 1);
		moveFdTo(fd, 1);
		if (merge_stderr) {
			saveFd(state, 2);
			moveFdTo(_dup(1), 2);
		}

		const bool started_as_pipe = std::exchange(stdout_started_as_pipe_, true);
		const int sigpipe_depth = std::exchange(sigpipe_redirects_, stdout_redirects_);
		HistoryList history = history_;
		const std::size_t hidden = hideSavedFds(state);
		SubshellScope snap = enterSubshell();
		env_.setSubshellLevel(env_.subshellLevel() - 1);
		enterInheritedStage(stage);
		const int status = endSubshell(execNode(stage));
		stage_function_bump_ = false;
		leaveSubshell(snap);
		showHiddenFds(hidden);
		history_ = std::move(history);
		sigpipe_redirects_ = sigpipe_depth;
		stdout_started_as_pipe_ = started_as_pipe;

		undoRedirections(state);
		return status;
	}

	int Executor::runLocalStage(const Pipeline& pipeline, PipelineLocalStage local,
	                            HANDLE pipe_end) {
		if (local == PipelineLocalStage::LastHere) {
			return runLastStageHere(*pipeline.commands.back(), pipe_end);
		}

		const bool merge_stderr = !pipeline.stderr_to_stdout.empty()
			&& pipeline.stderr_to_stdout.front();
		return runFirstStageInSubshell(*pipeline.commands.front(), pipe_end, merge_stderr);
	}
#endif  // _WIN32

	int Executor::execPipeline(const Pipeline& pipeline) {
		executor_detail::BangGuard bang_guard(this, pipeline.bang);
		executor_detail::PipelineTimeGuard time_guard(this, pipeline);

		if (pipeline.commands.size() == 1) {
			int status = execNode(*pipeline.commands[0]);
			if (flowPending()) return status;
			if (pipeline.bang) status = negateBangStatus(status);
			return status;
		}

#ifdef _WIN32
		int status = execPipelineMultiCmd(pipeline);
		if (pipeline.bang) status = negateBangStatus(status);
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
				// In bash the element's own process expands its words.
				reportExpanderError(expander, /*drops_line=*/false);
				return false;
			}

			appendFields(argv, fields);
		}

		return true;
	}

	static bool textMayHaveSideEffects(const std::string& text) {
		return text.find("$(") != std::string::npos || text.find('`') != std::string::npos
			|| text.find('=') != std::string::npos || text.find("++") != std::string::npos
			|| text.find("--") != std::string::npos;
	}

	static bool segmentsMayHaveSideEffects(const std::vector<WordSegment>& segments);

	static bool segmentMayHaveSideEffects(const WordSegment& segment) {
		using K = WordSegment::Kind;
		switch (segment.kind) {
			case K::CmdSubst:
			case K::ProcSubst:
				return true;
			case K::ParamExp:
			case K::ArithExp:
				return textMayHaveSideEffects(segment.text);
			case K::DoubleQuoted:
				return segmentsMayHaveSideEffects(segment.nested);
			default:
				return false;
		}
	}

	static bool segmentsMayHaveSideEffects(const std::vector<WordSegment>& segments) {
		for (const WordSegment& segment : segments) {
			if (segmentMayHaveSideEffects(segment)) return true;
		}

		return false;
	}

	// A stage's words belong to its own subshell: `$(cat)` there reads
	// the pipe, `${x:=v}` and `$((i++))` must not touch this shell. Only
	// words without such effects may be expanded here to launch an
	// external directly.
	static bool wordsExpandHere(const std::vector<Word>& words) {
		for (const Word& word : words) {
			if (segmentsMayHaveSideEffects(word.segments)) return false;
		}

		return true;
	}

	static bool assignmentsExpandHere(const std::vector<Assignment>& assignments) {
		for (const Assignment& assignment : assignments) {
			if (segmentsMayHaveSideEffects(assignment.value.segments)) return false;
		}

		return true;
	}

	// A <(...) path names a pipe, not a program. Opening it to look for a
	// script line would take the reader's place, so it is refused first,
	// with Git Bash's message and status for /dev/fd/63.
	static bool refusePipeCommand(const std::string& name) {
		const bool pipe = name.rfind("\\\\.\\pipe\\", 0) == 0 || name.rfind("//./pipe/", 0) == 0;
		if (pipe) shellErrorf("%s: No such file or directory\n", name.c_str());
		return pipe;
	}

	// Redirections the shell can apply for the child, as bash's child does
	// before exec: no here-document bodies to expand, no `{fd}` variables to
	// set and no targets whose expansion would act on this shell.
	static bool redirectionsApplyHere(const std::vector<Redirection>& redirs) {
		for (const Redirection& redir : redirs) {
			if (redir.op == RedirOp::DLess || redir.op == RedirOp::DLessDash) return false;
			if (!redir.fd_variable.empty()) return false;
			if (segmentsMayHaveSideEffects(redir.target.segments)) return false;
		}

		return true;
	}

	static bool isShellStdio(HANDLE h_in, HANDLE h_out, HANDLE h_err) {
		const StdioHandles current = currentStdio();
		return h_in == current.in && h_out == current.out && h_err == current.err;
	}

	// Only a command that runs on the shell's own stdio (a background job)
	// can have its redirections applied here: a pipeline stage's are
	// relative to its pipe ends.
	static bool redirectsDirectly(const SimpleCommand& command, HANDLE h_in, HANDLE h_out,
	                              HANDLE h_err) {
		if (command.redirs.empty()) return true;
		return redirectionsApplyHere(command.redirs) && isShellStdio(h_in, h_out, h_err);
	}

	// The child gets the redirected handles and none of the shell's own,
	// so a caller reading the shell's stdout sees EOF when the shell ends.
	HANDLE Executor::launchExternalRedirected(const SimpleCommand& command,
	                                          const std::vector<std::string>& argv,
	                                          const std::string& exec_path) {
		RedirState state;
		if (!applyRedirectionsOrUndo(command.redirs, state)) return INVALID_HANDLE_VALUE;

		const StdioHandles stdio = currentStdio();
		const HANDLE process = launchExternalDirect(command, argv, exec_path, stdio.in, stdio.out,
			stdio.err);
		undoRedirections(state);
		return process;
	}

	HANDLE Executor::tryDirectExternalLaunch(const Node& elem,
	                                         HANDLE h_in, HANDLE h_out, HANDLE h_err,
	                                         bool* tried) {
		*tried = false;
		if (elem.kind != Node::Kind::SimpleCommand) return INVALID_HANDLE_VALUE;

		const auto& command = static_cast<const SimpleCommand&>(elem);
		if (!redirectsDirectly(command, h_in, h_out, h_err)) return INVALID_HANDLE_VALUE;
		if (!wordsExpandHere(command.words) || !assignmentsExpandHere(command.assignments)) {
			return INVALID_HANDLE_VALUE;
		}

		std::vector<std::string> argv;
		if (!expandWordsForDirectLaunch(expander_, command.words, argv)) {
			*tried = true;
			launch_status_ = 1;
			return INVALID_HANDLE_VALUE;
		}

		if (argv.empty()) return INVALID_HANDLE_VALUE;
		if (isBuiltin(argv[0]) || isFunction(argv[0]) || isAlias(argv[0])) {
			return INVALID_HANDLE_VALUE;
		}

		if (refusePipeCommand(argv[0])) {
			launch_status_ = kStatusCommandNotFound;
			*tried = true;
			return INVALID_HANDLE_VALUE;
		}

		const std::string name = systemBinCommand(argv[0]);
		if (isBuiltin(name)) return INVALID_HANDLE_VALUE;

		const std::string exec_path = findExecutable(name);
		if (exec_path.empty()) {
			const bool directory = reportDirectoryCommand(argv[0]);
			if (!directory) reportCommandNotFound(argv[0]);
			launch_status_ = directory ? kStatusCannotExecute : kStatusCommandNotFound;
			*tried = true;
			return INVALID_HANDLE_VALUE;
		}

		if (startsWithShebang(exec_path) || looksLikeShellScript(exec_path)) {
			return INVALID_HANDLE_VALUE;
		}

		*tried = true;
		const HANDLE process = command.redirs.empty()
			? launchExternalDirect(command, argv, exec_path, h_in, h_out, h_err)
			: launchExternalRedirected(command, argv, exec_path);
		if (process == INVALID_HANDLE_VALUE) launch_status_ = kStatusCannotExecute;
		return process;
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

	// A self-spawned shell is told it is one, so it takes this shell's
	// resolved PATH and loaded utils as they are instead of probing the
	// registry, the disk and the plugin folders again (see setup.cpp).
	static void addChildShellMarkers(NameValueList& overrides) {
		overrides.emplace_back("WBSH_SUBSHELL", "1");

		std::string files;
		for (const UtilInfo& util : loadedUtils()) {
			if (!files.empty()) files.push_back(';');
			files += util.path;
		}

		if (!files.empty()) overrides.emplace_back("WBSH_UTIL_FILES", files);
	}

	// A pipeline stage is a subshell of this shell: `$?` and `$$` are ours.
	static void addShellParameters(NameValueList& overrides, const Environment& env,
			int last_status) {
		if (last_status != 0) overrides.emplace_back("WBSH_STATUS", std::to_string(last_status));
		overrides.emplace_back("WBSH_SHELL_PID", std::to_string(env.shellPid()));
		overrides.emplace_back("WBSH_PPID", env.get("PPID"));
		overrides.emplace_back("WBSH_SUBSHELL_LEVEL", std::to_string(env.subshellLevel()));
		const char letter = env.invocationLetter();
		if (letter != 0) overrides.emplace_back("WBSH_INVOCATION", std::string(1, letter));
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

	static std::string joinLines(const std::vector<std::string>& lines) {
		std::string text;
		for (const std::string& line : lines) text += line + "\n";
		return text;
	}

	static std::string serializeDisabledBuiltins(const Executor& exec) {
		const std::unordered_set<std::string>& disabled = exec.disabledBuiltins();
		std::vector<std::string> names(disabled.begin(), disabled.end());
		std::sort(names.begin(), names.end());
		return joinWithSpaces(names);
	}

	NameValueList Executor::buildSelfSpawnOverrides() {
		NameValueList overrides;

		const std::string posix_path = env_.get("PATH");
		if (!posix_path.empty()) {
			overrides.emplace_back("PATH", path_conv_.pathListPosixToWin32(posix_path));
		}

		const std::string functions = serializeFunctions();
		if (!functions.empty()) overrides.emplace_back("WBSH_FUNCTIONS", functions);
		appendExportedFunctions(overrides);
		const std::string function_files = serializeFunctionFiles();
		if (!function_files.empty()) overrides.emplace_back("WBSH_FUNCTION_FILES", function_files);
		overrides.emplace_back("WBSH_LINENO", std::to_string(env_.currentLineno()));
		const std::string aliases = serializeAliases();
		if (!aliases.empty()) overrides.emplace_back("WBSH_ALIASES", aliases);
		const std::string arrays = serializeArrays();
		if (!arrays.empty()) overrides.emplace_back("WBSH_ARRAYS", arrays);
		const std::string attributes = serializeAttributes();
		if (!attributes.empty()) overrides.emplace_back("WBSH_ATTRIBUTES", attributes);
		const std::string bindings = serializeKeyBindings();
		if (!bindings.empty()) overrides.emplace_back("WBSH_BINDINGS", bindings);
		const std::string completions = serializeCompletionSpecs(*this);
		if (!completions.empty()) overrides.emplace_back("WBSH_COMPLETIONS", completions);
		const std::string disabled = serializeDisabledBuiltins(*this);
		if (!disabled.empty()) overrides.emplace_back("WBSH_DISABLED_BUILTINS", disabled);
		const std::string traps = serializeListedTraps();
		if (!traps.empty()) overrides.emplace_back("WBSH_TRAPS", traps);
		const std::string dir_stack = joinLines(dir_stack_);
		if (!dir_stack.empty()) overrides.emplace_back("WBSH_DIRSTACK", dir_stack);
		if (canReturn()) overrides.emplace_back("WBSH_RETURNABLE", "1");

		const std::vector<std::string> locals = unexportedNames(env_);
		if (!locals.empty()) overrides.emplace_back("WBSH_LOCAL_NAMES", joinWithSpaces(locals));

		const std::string options = serializeChangedOptions(env_);
		if (!options.empty()) overrides.emplace_back("WBSH_OPTIONS", options);

		addShellParameters(overrides, env_, last_status_);
		addChildShellMarkers(overrides);
		addSubshellState(overrides);
		return overrides;
	}

	void Executor::appendHeredocBodiesToSlice(const SimpleCommand& sc, std::string& slice) {
		for (const auto& redir : sc.redirs) {
			if (redir.op != RedirOp::DLess && redir.op != RedirOp::DLessDash) continue;

			warnHeredocAtEof(redir);
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

	// CreateProcess takes at most 32767 characters of command line. A
	// longer self-spawn (a big pipeline stage or heredoc) hands its words
	// over in a temp file, NUL-terminated each, which the child reads and
	// deletes (see `--argv-file` in main.cpp).
	static const std::size_t kMaxSelfCommandLine = 32000;

	static std::string writeArgvFile(const std::vector<std::string>& argv) {
		wchar_t dir[MAX_PATH + 1] = {};
		wchar_t name[MAX_PATH + 1] = {};
		if (::GetTempPathW(MAX_PATH + 1, dir) == 0) return {};
		if (::GetTempFileNameW(dir, L"wba", 0, name) == 0) return {};

		std::string body;
		for (std::size_t i = 1; i < argv.size(); ++i) body += argv[i] + '\0';

		const std::string path = wideToUtf8(name);
		writeAllText(path, body);
		return path;
	}

	static SelfSpawnLine buildSelfSpawnLine(const std::vector<std::string>& argv) {
		SelfSpawnLine line;
		line.cmdline = buildCommandLine(argv);
		if (line.cmdline.size() <= kMaxSelfCommandLine) return line;

		line.argv_file = writeArgvFile(argv);
		if (line.argv_file.empty()) return line;

		line.cmdline = buildCommandLine({ argv[0], "--argv-file", line.argv_file });
		return line;
	}

	static void discardSelfSpawnFiles(const SelfSpawnLine& line, const NameValueList& overrides) {
		discardHistorySnapshot(overrides);
		if (line.argv_file.empty()) return;

		std::error_code ec;
		std::filesystem::remove(utf8ToPath(line.argv_file), ec);
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
			shellErrorf("cannot extract pipeline element source\n");
			return INVALID_HANDLE_VALUE;
		}

		if (elem.kind == Node::Kind::SimpleCommand) {
			appendHeredocBodiesToSlice(static_cast<const SimpleCommand&>(elem), slice);
		}

		return spawnSelfCommand(slice, h_in, h_out, h_err);
	}

	HANDLE Executor::spawnSelfCommand(const std::string& command_text,
	                                  HANDLE h_in, HANDLE h_out, HANDLE h_err) {
		const std::string self = selfExecutablePath();
		const SelfSpawnLine line = buildSelfSpawnLine(selfSpawnArgv(self, command_text));

		const NameValueList overrides = buildSelfSpawnOverrides();
		SpawnCommand command;
		command.exe      = utf8ToWide(self);
		command.cmdline  = line.cmdline;
		command.envblock = buildEnvBlock(env_, overrides, /*include_unexported=*/true);

		const HANDLE process = spawnWithHandles(command, StdioHandles{ h_in, h_out, h_err },
			backgroundPlacement());
		if (process == INVALID_HANDLE_VALUE) discardSelfSpawnFiles(line, overrides);
		return process;
	}

	// `<(body)` writes into the pipe the command reads, `>(body)` reads
	// what the command writes; the other two streams are the shell's.
	std::string Executor::startProcessSubstitution(const std::string& body, char direction) {
		ProcSubstRelay relay;
		if (!startProcSubstRelay(direction, relay)) {
			shellErrorf("process substitution: %s\n", lastErrorString().c_str());
			return {};
		}

		std::fflush(stdout);
		std::fflush(stderr);
		StdioHandles stdio = currentStdio();
		if (direction == '<') stdio.out = relay.child_end;
		else                  stdio.in  = relay.child_end;

		const HANDLE process = spawnSelfCommand(body, stdio.in, stdio.out, stdio.err);
		::CloseHandle(relay.child_end);
		if (process == INVALID_HANDLE_VALUE) {
			releaseProcSubstRelay(relay);
			return {};
		}

		env_.setLastBgPid(static_cast<long long>(::GetProcessId(process)));
		rememberProcSubstChild(process);
		proc_substs_.push_back(relay);
		return relay.path;
	}

	void Executor::rememberProcSubstChild(HANDLE process) {
		auto finished = [](HANDLE child) {
			if (::WaitForSingleObject(child, 0) != WAIT_OBJECT_0) return false;

			::CloseHandle(child);
			return true;
		};
		proc_subst_children_.erase(std::remove_if(proc_subst_children_.begin(),
			proc_subst_children_.end(), finished), proc_subst_children_.end());
		proc_subst_children_.push_back(process);
	}

	void Executor::releaseProcessSubstitutionsSince(std::size_t watermark) {
		while (proc_substs_.size() > watermark) {
			releaseProcSubstRelay(proc_substs_.back());
			proc_substs_.pop_back();
		}
	}

	bool Executor::waitForProcSubstChild(long long pid, int& status) {
		for (std::size_t i = 0; i < proc_subst_children_.size(); ++i) {
			const HANDLE child = proc_subst_children_[i];
			if (static_cast<long long>(::GetProcessId(child)) != pid) continue;

			status = waitProcessAndClose(child);
			proc_subst_children_.erase(proc_subst_children_.begin() + static_cast<long>(i));
			return true;
		}

		return false;
	}

	void Executor::waitForProcSubstChildren() {
		for (HANDLE child : proc_subst_children_) waitProcessAndClose(child);
		proc_subst_children_.clear();
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
		const std::string self = selfExecutablePath();
		const SelfSpawnLine line = buildSelfSpawnLine(selfSpawnArgv(self, quotedCommandText(argv)));

		const NameValueList overrides = buildSelfSpawnOverrides();
		SpawnCommand command;
		command.exe      = utf8ToWide(self);
		command.cmdline  = line.cmdline;
		command.envblock = buildEnvBlock(env_, overrides, /*include_unexported=*/true);

		const HANDLE process = spawnWithHandles(command, currentStdio(), placement);
		if (process == INVALID_HANDLE_VALUE) discardSelfSpawnFiles(line, overrides);
		return process;
	}

	HANDLE Executor::spawnCommand(const std::vector<std::string>& argv,
	                              const SpawnPlacement& placement) {
		if (argv.empty()) return INVALID_HANDLE_VALUE;

		std::fflush(stdout);
		std::fflush(stderr);
		if (runsInsideShell(argv[0])) return spawnShellCommand(argv, placement);

		const std::string name = systemBinCommand(argv[0]);
		if (isBuiltin(name)) return spawnShellCommand(argv, placement);

		const std::string exec_path = findExecutable(name);
		if (exec_path.empty()) {
			if (!reportDirectoryCommand(argv[0])) reportCommandNotFound(argv[0]);
			return INVALID_HANDLE_VALUE;
		}

		if (startsWithShebang(exec_path) || looksLikeShellScript(exec_path)) {
			return spawnShellCommand(argv, placement);
		}

		const NameValueList no_assignments;
		std::vector<std::string> child_argv = prepareExternalArgv(argv, exec_path, no_assignments);
		if (!placement.argv0.empty()) child_argv[0] = placement.argv0;
		SpawnCommand command = makeSpawnCommand(std::move(child_argv), exec_path,
			buildEnvBlock(env_, prepareExternalEnvOverrides(no_assignments)));
		if (reportSpawnCommandError(command, argv[0])) return INVALID_HANDLE_VALUE;

		return spawnWithHandles(command, currentStdio(), placement);
	}

	DWORD jobBreakawayFlag() {
		BOOL in_job = FALSE;
		if (::IsProcessInJob(::GetCurrentProcess(), nullptr, &in_job) == 0 || !in_job) return 0;

		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
		const BOOL queried = ::QueryInformationJobObject(nullptr,
			JobObjectExtendedLimitInformation, &limits, sizeof(limits), nullptr);
		if (queried == 0) return 0;

		const DWORD flags = limits.BasicLimitInformation.LimitFlags;
		const bool allowed = (flags & JOB_OBJECT_LIMIT_BREAKAWAY_OK) != 0;
		const bool automatic = (flags & JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK) != 0;
		return allowed && !automatic ? CREATE_BREAKAWAY_FROM_JOB : 0;
	}

	SpawnPlacement detachedPlacement(bool keep_console) {
		SpawnPlacement placement;
		placement.new_process_group = true;
		placement.outlives_shell = true;
		placement.creation_flags = jobBreakawayFlag();
		if (!keep_console) placement.creation_flags |= CREATE_NO_WINDOW;
		return placement;
	}

	HANDLE Executor::spawnWithEnvironment(const std::string& exe_path,
	                                      const std::string& command_line,
	                                      const NameValueList& environment) {
		NameValueList entries = environment;
		for (auto& entry : entries) {
			if (entry.first == "PATH") entry.second = path_conv_.pathListPosixToWin32(entry.second);
			else if (entry.first == "HOME") entry.second = path_conv_.toWin32Short(entry.second);
		}

		if (asciiLower(exe_path) == asciiLower(selfExecutablePath())) {
			addChildShellMarkers(entries);
		}

		std::vector<EnvEntry> wide_entries;
		for (const auto& entry : entries) overrideEnvEntry(wide_entries, entry);

		std::fflush(stdout);
		std::fflush(stderr);
		SpawnCommand command;
		command.exe      = utf8ToWide(exe_path);
		command.cmdline  = utf8ToWide(command_line);
		command.envblock = envBlockFromEntries(std::move(wide_entries));
		return spawnWithHandles(command, currentStdio());
	}

	// A prefix assignment whose value fails to expand is dropped rather
	// than aborting the launch.
	static NameValueList expandPrefixAssignments(Expander& expander,
	                                             const std::vector<Assignment>& assignments) {
		NameValueList temp_env;
		for (const auto& assignment : assignments) {
			std::string value = expander.expandAssignmentValue(assignment.value);
			if (expander.failed()) {
				reportExpanderError(expander, /*drops_line=*/false);
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
		if (reportSpawnCommandError(command, argv[0])) return INVALID_HANDLE_VALUE;

		return spawnWithHandles(command, StdioHandles{ h_in, h_out, h_err },
			backgroundPlacement());
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
	// `>&3` cannot reach one and write past a redirection, and no child
	// gets one.
	static int dupAboveUserFds(int fd) {
		std::vector<int> low_copies;
		int copy = _dup(fd);
		while (copy >= 0 && copy < kFirstSavedFd) {
			low_copies.push_back(copy);
			copy = _dup(fd);
		}

		for (const int low : low_copies) _close(low);
		keepFdFromChildren(copy);
		return copy;
	}

	// A write error belongs to the file fd 1 pointed at when it happened.
	static void beforeRepointing(int target) {
		if (target == 0) resetStdinBuffer();
		if (target == 1) std::clearerr(stdout);
	}

	static void afterRepointing(int target) {
		if (target == 1) bufferStdoutForNul();
	}

	// The opened fd can already be `target` when target was closed.
	static void moveFdTo(int fd, int target) {
		if (fd == target) return;

		beforeRepointing(target);
		_dup2(fd, target);
		_close(fd);
		afterRepointing(target);
	}

	// A backup of -1 records that @p fd was closed, so undoing closes it.
	static bool fdIsPipe(int fd) {
		const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
		return handle != INVALID_HANDLE_VALUE && ::GetFileType(handle) == FILE_TYPE_PIPE;
	}

	// POSIX kills a process that writes to a pipe nobody reads any more
	// (SIGPIPE). Pipeline elements and `wbsh ... | head` run as their own
	// process here, so ending that shell gives the same result: a loop of
	// builtins feeding a reader that has exited cannot run forever.
	void Executor::exitIfStdoutReaderGone() {
		if (!stdout_started_as_pipe_ || stdout_redirects_ != sigpipe_redirects_) return;
		if (std::ferror(stdout) == 0 || !fdIsPipe(1)) return;

		std::clearerr(stdout);
		raiseExit(kSigPipeStatus);
	}

	void Executor::saveFd(RedirState& state, int fd) const {
		if (fd == 1) ++stdout_redirects_;
		evacuateSavedCopy(fd);
		const int copy = dupAboveUserFds(fd);
		if (copy >= 0) saved_copies_.insert(copy);
		state.saved.push_back({ fd, copy });
	}

	void Executor::evacuateSavedCopy(int fd) const {
		if (saved_copies_.erase(fd) == 0) return;

		const int moved = dupAboveUserFds(fd);
		saved_copies_.insert(moved);
		moved_saved_copies_[fd] = moved;
	}

	// Where the copy saved as @p copy is now; it is no longer tracked. The
	// numbers it left go into @p vacated: as in bash, undoing the
	// redirection closes them too.
	int Executor::takeSavedCopy(int copy, std::vector<int>* vacated) const {
		for (auto moved = moved_saved_copies_.find(copy); moved != moved_saved_copies_.end();
				moved = moved_saved_copies_.find(copy)) {
			if (vacated != nullptr) vacated->push_back(copy);
			copy = moved->second;
			moved_saved_copies_.erase(moved);
		}

		saved_copies_.erase(copy);
		return copy;
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

	// The C runtime's _O_APPEND seeks to the end before each write, so two
	// writers can seek to the same end and overwrite each other. A handle
	// with append access only (FILE_APPEND_DATA without FILE_WRITE_DATA)
	// makes Windows put every write at the end, as O_APPEND does on POSIX,
	// also in the programs that inherit it.
	static int openAppendOnly(const std::wstring& path) {
		const HANDLE file = ::CreateFileW(path.c_str(), FILE_APPEND_DATA | SYNCHRONIZE,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) {
			errno = ::GetLastError() == ERROR_ACCESS_DENIED ? EACCES : ENOENT;
			return -1;
		}

		const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(file), _O_WRONLY | _O_BINARY);
		if (fd < 0) ::CloseHandle(file);
		return fd;
	}

	static DWORD redirAccess(int flags) {
		if ((flags & _O_RDWR) != 0) return GENERIC_READ | GENERIC_WRITE;
		if ((flags & _O_WRONLY) != 0) return GENERIC_WRITE;
		return GENERIC_READ;
	}

	static DWORD redirDisposition(int flags) {
		const bool create = (flags & _O_CREAT) != 0;
		const bool truncate = (flags & _O_TRUNC) != 0;
		if (create && (flags & _O_EXCL) != 0) return CREATE_NEW;
		if (create && truncate) return CREATE_ALWAYS;
		if (create) return OPEN_ALWAYS;
		if (truncate) return TRUNCATE_EXISTING;
		return OPEN_EXISTING;
	}

	// The same errno values _wopen derives from these Win32 errors.
	static int errnoOfOpenError(DWORD error) {
		switch (error) {
			case ERROR_FILE_NOT_FOUND:
			case ERROR_PATH_NOT_FOUND:
			case ERROR_INVALID_NAME:
			case ERROR_INVALID_DRIVE:
			case ERROR_BAD_NETPATH:
				return ENOENT;
			case ERROR_FILE_EXISTS:
			case ERROR_ALREADY_EXISTS:
				return EEXIST;
			case ERROR_ACCESS_DENIED:
			case ERROR_SHARING_VIOLATION:
			case ERROR_LOCK_VIOLATION:
			case ERROR_WRITE_PROTECT:
				return EACCES;
			case ERROR_TOO_MANY_OPEN_FILES:
				return EMFILE;
			default:
				return EINVAL;
		}
	}

	static bool isDirectoryPath(const std::wstring& path) {
		const DWORD attributes = ::GetFileAttributesW(path.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	}

	// _wopen leaves out FILE_SHARE_DELETE, so a script could not remove a
	// file it still has open, as it can on POSIX (`exec 3<f; rm f`).
	static int openSharingDelete(const std::wstring& path, int flags) {
		SECURITY_ATTRIBUTES inherit = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
		const HANDLE file = ::CreateFileW(path.c_str(), redirAccess(flags),
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			(flags & _O_NOINHERIT) != 0 ? nullptr : &inherit, redirDisposition(flags),
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) {
			const DWORD error = ::GetLastError();
			errno = isDirectoryPath(path) ? EISDIR : errnoOfOpenError(error);
			return -1;
		}

		const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(file),
			(flags & (_O_WRONLY | _O_RDWR)) | _O_BINARY);
		if (fd < 0) ::CloseHandle(file);
		return fd;
	}

	int Executor::openProcRedirSource(const std::string& path, int flags) const {
		const ProcNode node = lookupProcPath(path_conv_, path, true);
		if (node.type == ProcType::Outside) return openRedirSourceFd(node.target, flags);
		return openProcNode(path_conv_, node, flags);
	}

	// Without a console there is no terminal to open, which Linux reports
	// as ENXIO.
	static int openTerminal(int flags) {
		const bool read_only = (flags & (_O_WRONLY | _O_RDWR)) == 0;
		const int fd = openSharingDelete(read_only ? L"CONIN$" : L"CONOUT$", flags);
		if (fd < 0) errno = ENXIO;
		return fd;
	}

	int Executor::openRedirSourceFd(const std::string& path, int flags) const {
		int fd = dupSpecialDevFd(path);
		if (fd >= 0) return fd;

		if (path == "/dev/tty") return openTerminal(flags);

		if (isGeneratorDevice(path) && !opensForWriting(flags)) return openGeneratorDevice(path);
		if (isGeneratorDevice(path)) return _wopen(L"NUL", flags | _O_BINARY, _S_IREAD | _S_IWRITE);
		if (isProcPath(path)) return openProcRedirSource(path, flags);

		fd = openTcpUdpStream(path);
		if (fd >= 0) return fd;

		const std::string win32 = path_conv_.toWin32(path);
		if (isFifoFile(utf8ToPath(win32))) return openFifo(utf8ToPath(win32), flags);

		const std::wstring win_path = utf8ToFileWide(win32);
		if ((flags & _O_APPEND) != 0) return openAppendOnly(win_path);
		return openSharingDelete(win_path, flags);
	}

	bool Executor::redirectFdFromPath(const std::string& path, int flags,
	                                  int target, RedirState& state) {
		saveFd(state, target);
		const int fd = openRedirSourceFd(path, flags);
		if (fd < 0) {
			if (errno != EINTR) reportPathError(path);
			return false;
		}

		moveFdTo(fd, target);
		return true;
	}

	// As in bash, the target is split and globbed and must come out as one
	// word; in POSIX mode a non-interactive shell does neither.
	bool Executor::expandUnsplitRedirWord(const Word& target, std::string& out) {
		out = expander_.expandStringValue(target);
		if (!expander_.failed()) return true;

		reportExpanderError(expander_);
		return false;
	}

	bool Executor::expandRedirTarget(const Word& target, std::string& out) {
		if (env_.options().posix && !env_.interactive()) {
			out = expander_.expandStringValue(target);
		} else {
			const std::vector<std::string> fields = expander_.expandWord(target);
			if (!expander_.failed() && fields.size() != 1) {
				shellErrorf("%s: ambiguous redirect\n", target.raw.c_str());
				return false;
			}

			if (!expander_.failed()) out = fields[0];
		}

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

		shellErrorf("%s: cannot overwrite existing file\n", path.c_str());
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
		return redirectOutputAndErrors(path, extra_flags, state);
	}

	// As bash: `>&word` with no fd in front and no number in the word is
	// `&>word`; any other word that names no descriptor is ambiguous.
	bool Executor::applyDupWord(const Redirection& redir, const std::string& word,
			RedirState& state) {
		const bool output_and_errors = redir.op == RedirOp::GreatAnd && redir.fd < 0
			&& redir.fd_variable.empty() && !word.empty();
		if (output_and_errors) return redirectOutputAndErrors(word, _O_TRUNC, state);

		shellErrorf("%s: ambiguous redirect\n", word.empty() ? redir.target.raw.c_str() : word.c_str());
		return false;
	}

	bool Executor::redirectOutputAndErrors(const std::string& path, int extra_flags,
			RedirState& state) {
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

	static bool fdIsOpen(int fd) {
		return _get_osfhandle(fd) != -1;
	}

	bool Executor::applyDupRedir(const Redirection& redir, int target, RedirState& state) {
		std::string spec;
		if (!expandRedirTarget(redir.target, spec)) return false;

		if (spec == "-") {
			closeRedirFd(target, state);
			return true;
		}

		const bool moves = spec.size() > 1 && spec.back() == '-';
		if (moves) spec.pop_back();

		int from_fd = 0;
		const bool number = !spec.empty() && spec.find_first_not_of("0123456789") == std::string::npos;
		if (!number || !parseInt(spec, from_fd)) return applyDupWord(redir, spec, state);

		// bash treats `N>&N` as a no-op, even when N is closed. `{v}>&N` only
		// looks like one when N is closed, because then N is the free fd
		// picked for v.
		if (from_fd == target && redir.fd_variable.empty()) return true;

		// Checked before the save, which could otherwise land on a closed
		// from_fd and be duplicated in its place.
		if (!fdIsOpen(from_fd) || savedFdHidden(from_fd)) {
			reportBadDupSource(redir, target);
			return false;
		}

		saveFd(state, target);
		beforeRepointing(target);
		if (_dup2(from_fd, target) != 0) {
			reportBadDupSource(redir, target);
			return false;
		}

		afterRepointing(target);
		if (moves) closeRedirFd(from_fd, state);
		return true;
	}

	void Executor::closeRedirFd(int fd, RedirState& state) {
		saveFd(state, fd);
		beforeRepointing(fd);
		_close(fd);
		forgetCoprocFd(fd);
	}

	void Executor::noteJobEnded(long long pid) {
		noteChildExited();
		if (pid != 0 && pid == coproc_.pid) disposeCoproc();
	}

	// bash closes a coproc's descriptors and unsets NAME and NAME_PID once
	// it has reaped the coproc.
	void Executor::disposeCoproc() {
		for (const int fd : { coproc_.read_fd, coproc_.write_fd }) {
			if (fd < 0) continue;

			beforeRepointing(fd);
			_close(fd);
		}

		if (env_.isReadonly(coproc_.name)) {
			shellErrorf("%s: cannot unset: readonly variable\n", coproc_.name.c_str());
		} else {
			env_.unset(coproc_.name);
		}

		env_.clearReadonly(coproc_.name + "_PID");
		env_.unset(coproc_.name + "_PID");
		coproc_ = CoprocFds();
	}

	// As in bash, NAME[0] or NAME[1] of the last coproc turns -1 once a
	// redirection closes or moves that descriptor.
	void Executor::forgetCoprocFd(int fd) {
		if (coproc_.name.empty()) return;

		if (fd == coproc_.read_fd) {
			coproc_.read_fd = -1;
			env_.setIndexedElement(coproc_.name, 0, "-1");
		}

		if (fd == coproc_.write_fd) {
			coproc_.write_fd = -1;
			env_.setIndexedElement(coproc_.name, 1, "-1");
		}
	}

	static bool isDigits(const std::string& text) {
		return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
			return std::isdigit(static_cast<unsigned char>(c)) != 0;
		});
	}

	// bash names a literal fd, else the word as written when it duplicates
	// onto the default fd, else the fd it duplicates onto.
	static std::string badDupSourceName(const Redirection& redir, int target) {
		if (isDigits(redir.target.raw)) return redir.target.raw;

		const int default_fd = redir.op == RedirOp::LessAnd ? 0 : 1;
		if (target == default_fd && redir.fd_variable.empty()) return redir.target.raw;
		return std::to_string(target);
	}

	void Executor::reportBadDupSource(const Redirection& redir, int target) {
		shellErrorf("%s: Bad file descriptor\n", badDupSourceName(redir, target).c_str());
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

	void Executor::warnHeredocAtEof(const Redirection& redir) {
		if (redir.heredoc_eof_line == 0) return;

		std::string delimiter = expander_.expandStringValue(redir.target);
		if (expander_.failed()) {
			expander_.takeError();
			delimiter = redir.target.raw;
		}

		shellWarningAt(redir.heredoc_eof_line, "here-document at line "
			+ std::to_string(redir.target.loc.line) + " delimited by end-of-file (wanted `"
			+ delimiter + "')");
	}

	bool Executor::applyHeredocRedir(const Redirection& redir, int target, RedirState& state) {
		warnHeredocAtEof(redir);
		std::string body = expander_.expandHeredocBody(redir.heredoc_body, redir.heredoc_quoted);
		if (expander_.failed()) {
			reportExpanderError(expander_);
			return false;
		}

		return installRedirFromTempBody(std::move(body), target, state);
	}

	bool Executor::applyHerestringRedir(const Redirection& redir, int target, RedirState& state) {
		std::string body;
		if (!expandUnsplitRedirWord(redir.target, body)) {
			// bash fails only the command when a here-string hits `set -u`.
			expander_.takeFatalError();
			return false;
		}

		body.push_back('\n');
		return installRedirFromTempBody(std::move(body), target, state);
	}

	static int redirTargetFd(const Redirection& redir) {
		return (redir.fd != -1) ? redir.fd : defaultRedirTargetFd(redir.op);
	}

	static int lowestFreeFdFrom(int first) {
		int fd = first;
		while (_get_osfhandle(fd) != -1) ++fd;
		return fd;
	}

	static void forgetSavedFd(std::vector<std::pair<int, int>>& saved, int fd) {
		saved.erase(std::remove_if(saved.begin(), saved.end(),
			[fd](const std::pair<int, int>& entry) { return entry.first == fd; }), saved.end());
	}

	bool Executor::applyRedirections(const std::vector<Redirection>& redirs, RedirState& state) {
		for (const auto& redir : redirs) {
			std::fflush(stdout);
			std::fflush(stderr);

			const bool ok = redir.fd_variable.empty()
				? applyRedirection(redir, redirTargetFd(redir), state)
				: applyFdVariableRedir(redir, state);
			if (!ok) return false;
			if (flowPending()) return false;
		}

		return true;
	}

	static bool redirectsOutput(RedirOp op) {
		return op == RedirOp::Great || op == RedirOp::Clobber || op == RedirOp::DGreat
			|| op == RedirOp::AmpGreat || op == RedirOp::AmpDGreat || op == RedirOp::LessGreat;
	}

	bool Executor::applyRedirection(const Redirection& redir, int target, RedirState& state) {
		if (env_.restricted() && redirectsOutput(redir.op)) {
			shellErrorf("%s: restricted: cannot redirect output\n", redir.target.raw.c_str());
			return false;
		}

		if (!installRedirection(redir, target, state)) return false;

		passFdToChildren(target);
		return true;
	}

	bool Executor::installRedirection(const Redirection& redir, int target, RedirState& state) {
		switch (redir.op) {
		case RedirOp::Less:        return applyLessRedir       (redir, target, state);
		case RedirOp::Great:
		case RedirOp::Clobber:     return applyTruncOrClobber  (redir, target, state);
		case RedirOp::DGreat:      return applyAppendRedir     (redir, target, state);
		case RedirOp::AmpGreat:    return applyAmpRedir(redir, _O_TRUNC, state);
		case RedirOp::AmpDGreat:   return applyAmpRedir(redir, _O_APPEND, state);
		case RedirOp::LessGreat:   return applyLessGreatRedir  (redir, target, state);
		case RedirOp::LessAnd:
		case RedirOp::GreatAnd:    return applyDupRedir        (redir, target, state);
		case RedirOp::DLess:
		case RedirOp::DLessDash:   return applyHeredocRedir    (redir, target, state);
		case RedirOp::TLess:       return applyHerestringRedir (redir, target, state);
		default:                   return false;
		}
	}

	// `{name}>file` opens the file on the lowest free fd from 10 up and
	// stores it in `name`; `{name}>&-` closes the fd that `name` holds.
	// Either way the change outlives the command, as in bash.
	bool Executor::applyFdVariableRedir(const Redirection& redir, RedirState& state) {
		std::string spec;
		const bool dup = redir.op == RedirOp::GreatAnd || redir.op == RedirOp::LessAnd;
		if (dup && expandRedirTarget(redir.target, spec) && spec == "-") {
			return closeFdVariable(redir.fd_variable);
		}

		const int fd = lowestFreeFdFrom(kFirstSavedFd);
		if (refusesFdAtLimit(fd, redir)) return false;
		if (!applyRedirection(redir, fd, state)) return false;

		if (!env_.shopts().varredir_close) forgetSavedFd(state.saved, fd);
		if (assignFdVariable(redir.fd_variable, fd)) return true;

		forgetSavedFd(state.saved, fd);
		beforeRepointing(fd);
		_close(fd);
		shellErrorf("%s: cannot assign fd to variable\n", redir.fd_variable.c_str());
		return false;
	}

	// bash opens the file and then fails to move it up to @p fd.
	bool Executor::refusesFdAtLimit(int fd, const Redirection& redir) {
		if (open_file_limit_ < 0 || fd < open_file_limit_) return false;

		std::string target;
		if (!expandRedirTarget(redir.target, target)) return true;

		shellErrorWithoutLine("redirection error: cannot duplicate fd: Invalid argument");
		shellErrorf("%s: Invalid argument\n", target.c_str());
		return true;
	}

	// Read without expansion, so `set -u` does not stop at an unset name.
	std::string Executor::fdVariableValue(const std::string& variable) {
		if (variable.find('[') == std::string::npos) return env_.get(variable);

		return expander_.expandHeredoc("${" + variable + "}", false);
	}

	bool Executor::closeFdVariable(const std::string& variable) {
		const std::string value = fdVariableValue(variable);
		int fd = -1;
		if (!parseInt(value, fd) || fd < 0) {
			shellErrorf("%s: ambiguous redirect\n", variable.c_str());
			return false;
		}

		beforeRepointing(fd);
		_close(fd);
		return true;
	}

	bool Executor::assignFdVariable(const std::string& variable, int fd) {
		const std::size_t open = variable.find('[');
		if (open == std::string::npos) {
			const std::string text = std::to_string(fd);
			if (env_.rejectsRefTarget(variable, text)) return false;

			env_.set(variable, text);
			return !env_.isReadonly(variable);
		}

		const std::string name = variable.substr(0, open);
		const std::string subscript = variable.substr(open + 1, variable.size() - open - 2);
		if (env_.isAssocArray(name)) {
			env_.setAssocElement(name, expander_.expandHeredoc(subscript, false),
				std::to_string(fd));
		} else {
			env_.setIndexedElement(name, expander_.evalArith(subscript), std::to_string(fd));
		}

		return !env_.isReadonly(name);
	}

	static void restoreSavedFd(int fd, int backup) {
		beforeRepointing(fd);
		if (backup < 0) {
			_close(fd);
			return;
		}

		_dup2(backup, fd);
		_close(backup);
		afterRepointing(fd);
	}

	void Executor::undoRedirections(RedirState& state) {
		std::fflush(stdout);
		std::fflush(stderr);
		std::vector<int> vacated;
		for (auto it = state.saved.rbegin(); it != state.saved.rend(); ++it) {
			const int copy = it->second < 0 ? it->second : takeSavedCopy(it->second, &vacated);
			restoreSavedFd(it->first, copy);
			if (it->first == 1) --stdout_redirects_;
		}

		for (const int fd : vacated) _close(fd);

		state.saved.clear();
		for (const auto& temp_path : state.temps) removeFile(temp_path);
		state.temps.clear();
	}

	// An unquoted word that expands to nothing is dropped, as in bash.
	static void appendAssocListWord(Expander& expander, const Word& word,
			Executor::ArrayAssign& array) {
		std::string value = expander.expandAssignmentValue(word);
		const bool quoted = word.raw.find_first_of("'\"") != std::string::npos;
		if (value.empty() && !quoted) return;

		array.items.push_back({ false, std::string(), std::move(value), false, word.raw });
	}

	// An associative array's key-value list is expanded word by word,
	// without splitting, as bash does.
	static Executor::ArrayAssign expandArrayAssign(Expander& expander,
	                                               const Assignment& assignment) {
		Executor::ArrayAssign array;
		array.name = assignment.name;
		array.append = assignment.append;
		const bool assoc = expander.environment().isAssocArray(assignment.name);
		for (const auto& item : assignment.keyed_items) {
			if (item.has_key) {
				array.sparse = true;
				std::string key = expander.expandStringValue(item.key);
				std::string value = expander.expandAssignmentValue(item.value);
				array.items.push_back({ true, std::move(key), std::move(value), item.append });
			} else if (assoc) {
				appendAssocListWord(expander, item.value, array);
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

	static std::string expandedArrayItem(Expander& expander, const Assignment::Keyed& item) {
		const std::string value = expander.expandStringValue(item.value);
		if (!item.has_key) return value;

		const std::string op = item.append ? "+=" : "=";
		return "[" + expander.expandStringValue(item.key) + "]" + op + value;
	}

	// `eval a=(x "$y")` hands eval the text with each item expanded but not
	// split, as bash does.
	static std::string expandedCompoundText(Expander& expander, const Assignment& assignment) {
		std::string text = assignment.name + (assignment.append ? "+=(" : "=(");
		for (std::size_t k = 0; k < assignment.keyed_items.size(); ++k) {
			if (k != 0) text.push_back(' ');
			text += expandedArrayItem(expander, assignment.keyed_items[k]);
		}

		return text + ")";
	}

	std::string Executor::expandDeclarationArg(const DeclarationArg& arg) {
		if (arg.expands_items) {
			return expandedCompoundText(expander_, arg.assignment)
				+ expander_.expandStringValue(arg.trailing);
		}

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

	// A word typed as `name[...]`, unquoted: bash's W_ARRAYREF, which lets
	// read, printf -v and wait -p take an associative key as it comes.
	static bool typedAsArrayReference(const std::string& raw) {
		if (raw.empty() || !(std::isalpha(static_cast<unsigned char>(raw[0])) || raw[0] == '_')) {
			return false;
		}

		std::size_t i = 1;
		while (i < raw.size() && (std::isalnum(static_cast<unsigned char>(raw[i])) || raw[i] == '_')) {
			++i;
		}

		return i < raw.size() && raw[i] == '[' && raw.back() == ']';
	}

	bool Executor::expandSimpleCmdArgv(const SimpleCommand& sc, std::vector<std::string>& argv,
	                                   std::vector<bool>& compound) {
		array_ref_words_.clear();
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
			if (typedAsArrayReference(sc.words[i].raw) && fields.size() == 1) {
				array_ref_words_.push_back(fields[0]);
			}

			appendFields(argv, fields);

			compound.resize(argv.size(), false);
			if (declaration != nullptr && !declaration->compound_text.empty()) compound.back() = true;
		}

		return true;
	}

	static bool isAssignmentWord(const std::string& word) {
		const std::size_t eq = word.find('=');
		if (eq == std::string::npos || eq == 0) return false;

		const std::size_t name_end = word[eq - 1] == '+' ? eq - 1 : eq;
		const std::string name = word.substr(0, (std::min)(word.find('['), name_end));
		if (name.empty()) return false;
		if (!std::isalpha(static_cast<unsigned char>(name[0])) && name[0] != '_') return false;

		return std::all_of(name.begin(), name.end(), [](char c) {
			return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
		});
	}

	int Executor::execBareRedirsForExec(const std::vector<Redirection>& redirs) {
		traceText("exec");
		const std::size_t proc_substs = procSubstWatermark();
		RedirState state;
		const std::string outer = env_.assigningCommand();
		env_.setAssigningCommand("exec");
		const bool applied = applyRedirectionsOrUndo(redirs, state);
		env_.setAssigningCommand(outer);
		if (!applied) {
			if (posixErrorsExit()) raiseExit(1);
			return 1;
		}

		keepExecRedirs(state);
		keepProcessSubstitutionsSince(proc_substs);
		return 0;
	}

	void Executor::keepExecRedirs(const RedirState& state) {
		if (exec_redir_journal_.empty()) {
			for (const auto& saved : state.saved) {
				if (saved.second >= 0) _close(takeSavedCopy(saved.second));
			}

			return;
		}

		RedirState& journal = exec_redir_journal_.back();
		journal.saved.insert(journal.saved.end(), state.saved.begin(), state.saved.end());
		journal.temps.insert(journal.temps.end(), state.temps.begin(), state.temps.end());
	}

	std::size_t Executor::hideSavedFds(const RedirState& state) {
		const std::size_t mark = hidden_fds_.size();
		for (const auto& saved : state.saved) {
			if (saved.second >= 0) hidden_fds_.push_back(saved.second);
		}

		return mark;
	}

	bool Executor::savedFdHidden(int fd) const {
		return std::find(hidden_fds_.begin(), hidden_fds_.end(), fd) != hidden_fds_.end();
	}

	void Executor::endExecRedirJournal() {
		undoRedirections(exec_redir_journal_.back());
		exec_redir_journal_.pop_back();
	}

	// A scalar being appended to becomes element 0, so the list goes after it.
	static long long nextIndexedAppendSlot(const Environment& env, const std::string& name) {
		const auto* indexed = env.getIndexedArray(name);
		if (indexed == nullptr) return env.has(name) ? 1 : 0;
		if (indexed->empty()) return 0;
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

	// An error in a subscript stops the assignment; as in bash, the error
	// drops the rest of the line.
	static bool evalSubscriptOrReport(Expander& expander, const std::string& subscript,
	                                  long long& index) {
		index = expander.evalArithLiteral(subscript);
		if (!expander.failed()) return true;

		reportExpanderError(expander);
		return false;
	}

	// `[key]+=value` adds to what the element holds so far.
	static std::string keyedItemValue(const Environment& env, const std::string& name,
	                                  const Executor::ArrayItem& item, const std::string& current) {
		return item.append ? env.appendedTo(name, current, item.value) : item.value;
	}

	static bool evalItemIndex(Expander& expander, const Executor::ArrayItem& item,
	                          long long& index);

	static void appendToIndexedArray(Environment& env, Expander& expander,
	                                 const Executor::ArrayAssign& array) {
		long long next_index = nextIndexedAppendSlot(env, array.name);
		for (const auto& item : array.items) {
			long long index = next_index;
			if (item.has_key && !evalItemIndex(expander, item, index)) return;

			const std::string current = item.append
				? readElementValue(env, array.name, index, std::string(), false)
				: std::string();
			env.setIndexedElement(array.name, index, keyedItemValue(env, array.name, item, current));
			next_index = index + 1;
		}
	}

	static bool rejectsWholeArrayKey(Expander& expander, const Executor::ArrayItem& item) {
		if (item.key != "@" && item.key != "*") return false;

		shellErrorf("[%s]=%s: cannot assign to non-numeric index\n", item.key.c_str(),
			item.value.c_str());
		expander.dropRestOfLine();
		return true;
	}

	// bash quote-removes an indexed key of a compound assignment once more,
	// so a \001 in it escapes the byte after it and disappears.
	static std::string withoutCtlEscapes(const std::string& key) {
		static const char kCtlEsc = '\001';

		std::string out;
		for (std::size_t i = 0; i < key.size(); ++i) {
			if (key[i] != kCtlEsc) {
				out.push_back(key[i]);
				continue;
			}

			if (i + 1 < key.size()) out.push_back(key[++i]);
		}

		return out;
	}

	static bool evalItemIndex(Expander& expander, const Executor::ArrayItem& item,
	                          long long& index) {
		if (!item.has_key) return true;
		if (rejectsWholeArrayKey(expander, item)) return false;
		if (item.key.empty()) {
			shellErrorf("[]=%s: bad array subscript\n", item.value.c_str());
			expander.dropRestOfLine();
			return false;
		}

		const std::string key = withoutCtlEscapes(item.key);
		if (expander.environment().shopts().array_expand_once) {
			return evalSubscriptOrReport(expander, key, index);
		}

		index = expander.evalArith(key);
		if (!expander.failed()) return true;

		reportExpanderError(expander);
		return false;
	}

	// The elements before a bad subscript stay assigned, as in bash.
	static void assignSparseIndexedArray(Environment& env, Expander& expander,
	                                     const Executor::ArrayAssign& array) {
		std::map<long long, std::string> sparse;
		long long next_index = 0;
		for (const auto& item : array.items) {
			long long index = next_index;
			if (!evalItemIndex(expander, item, index)) break;
			if (index < 0 && !sparse.empty()) index += sparse.rbegin()->first + 1;
			if (index < 0) {
				shellErrorf("[%s]=%s: bad array subscript\n", item.key.c_str(), item.value.c_str());
				expander.dropRestOfLine();
				break;
			}

			sparse[index] = keyedItemValue(env, array.name, item, sparse[index]);
			next_index = index + 1;
		}

		env.setIndexedArraySparse(array.name, std::move(sparse));
	}

	// Once the list has started with `[key]=value`, a word without a key is
	// an error that ends the assignment.
	static void assignKeyedAssocItems(Environment& env, const Executor::ArrayAssign& array) {
		for (const auto& item : array.items) {
			if (!item.has_key) {
				shellErrorf("%s: %s: must use subscript when assigning associative array\n",
					array.name.c_str(), item.value.c_str());
				return;
			}

			const std::string current = readElementValue(env, array.name, 0, item.key, true);
			env.setAssocElement(array.name, item.key, keyedItemValue(env, array.name, item, current));
		}
	}

	// Words without a `[key]=` pair up as key and value, as in bash 5.1+.
	static void assignAssocItems(Environment& env, const Executor::ArrayAssign& array) {
		if (!array.items.empty() && array.items.front().has_key) {
			assignKeyedAssocItems(env, array);
			return;
		}

		const Executor::ArrayItem* pending_key = nullptr;
		for (const auto& item : array.items) {
			if (item.has_key) {
				env.setAssocElement(array.name, item.key, item.value);
			} else if (pending_key == nullptr && item.value.empty()) {
				shellErrorf("%s: bad array subscript\n", item.word.c_str());
				return;
			} else if (pending_key == nullptr) {
				pending_key = &item;
			} else {
				env.setAssocElement(array.name, pending_key->value, item.value);
				pending_key = nullptr;
			}
		}

		if (pending_key != nullptr) {
			env.setAssocElement(array.name, pending_key->value, std::string());
		}
	}

	static std::vector<std::string> itemValues(const Executor::ArrayAssign& array) {
		std::vector<std::string> values;
		values.reserve(array.items.size());
		for (const auto& item : array.items) values.push_back(item.value);
		return values;
	}

	// Even `a+=()` gives a declared array a value.
	static void applyArrayAssignToEnv(Environment& env, Expander& expander,
	                                  const Executor::ArrayAssign& array) {
		env.markValued(array.name);
		if (env.isAssocArray(array.name)) {
			if (!array.append) env.resetAssocArray(array.name);
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

	// A negative subscript reaching before the first element drops the
	// rest of the line, as bash does.
	static bool resolveIndexOrDrop(Environment& env, Expander& expander, const std::string& name,
	                               long long& index) {
		if (env.resolveElementIndex(name, index)) return true;

		expander.dropRestOfLine();
		return false;
	}

	// `a[@]=v` and `a[*]=v` name no single element, assoc arrays included;
	// the error drops the rest of the line.
	static bool isWholeArraySubscript(const Word& subscript) {
		if (subscript.segments.size() != 1) return false;

		const WordSegment& only = subscript.segments[0];
		return only.kind == WordSegment::Kind::Literal && (only.text == "@" || only.text == "*");
	}

	// `a[@]=x`, `a[*]=x` and `a[]=x` assign nothing and drop the line.
	static bool rejectWholeArraySubscript(Expander& expander, const Executor::ElemAssign& elem) {
		const bool empty = elem.subscript.segments.empty();
		if (!empty && !isWholeArraySubscript(elem.subscript)) return false;

		const std::string subscript = empty ? std::string() : elem.subscript.segments[0].text;

		shellErrorf("%s[%s]: bad array subscript\n", elem.name.c_str(), subscript.c_str());
		expander.dropRestOfLine();
		return true;
	}

	static void applyElemAssignToEnv(Environment& env, Expander& expander,
	                                 const Executor::ElemAssign& elem) {
		if (!env.isAssocArray(elem.name) && rejectWholeArraySubscript(expander, elem)) return;

		std::string subscript = expander.expandStringValue(elem.subscript);
		if (expander.failed()) {
			reportExpanderError(expander);
			subscript.clear();
		}

		const bool is_assoc = env.isAssocArray(elem.name);
		long long index = 0;
		if (!is_assoc && !evalSubscriptOrReport(expander, subscript, index)) return;
		if (!is_assoc && !resolveIndexOrDrop(env, expander, elem.name, index)) return;

		std::string value = elem.value;
		if (elem.append) {
			const std::string current = readElementValue(env, elem.name, index, subscript, is_assoc);
			value = env.appendedTo(elem.name, current, value);
		}

		if (is_assoc) {
			env.setAssocElement(elem.name, std::move(subscript), std::move(value));
		} else {
			env.setIndexedElement(elem.name, index, std::move(value));
		}
	}

	// `name=v` on an associative array assigns its element "0", as in bash.
	static void assignAssocKeyZero(Environment& env, const Executor::ScalarAssign& scalar) {
		const std::string current = readElementValue(env, scalar.name, 0, "0", true);
		const std::string value = scalar.append
			? env.appendedTo(scalar.name, current, scalar.value)
			: scalar.value;
		env.setAssocElement(scalar.name, "0", value);
	}

	static void applyScalarAssignToEnv(Environment& env, const Executor::ScalarAssign& scalar) {
		if (env.isAssocArray(scalar.name)) {
			assignAssocKeyZero(env, scalar);
			return;
		}

		if (!scalar.append) {
			env.set(scalar.name, scalar.value);
			return;
		}

		if (env.isIndexedArray(scalar.name)) {
			const std::string current = readElementValue(env, scalar.name, 0, std::string(), false);
			env.setIndexedElement(scalar.name, 0, env.appendedTo(scalar.name, current, scalar.value));
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

	// `a+=5 cmd` hands the command the whole new value, as bash does.
	static void setPrefixTemporarily(Environment& env, Executor::ScalarAssign& scalar,
			SavedVars& prior) {
		const bool append = scalar.append;
		if (append) {
			scalar.value = env.appendedValue(scalar.name, scalar.value);
			scalar.append = false;
		}

		setTemporarily(env, scalar, prior);
		if (append) scalar.value = env.get(scalar.name);
	}

	static void restoreTemporaries(Environment& env, SavedVars& prior) {
		for (auto it = prior.rbegin(); it != prior.rend(); ++it) {
			env.restoreVar(it->first, std::move(it->second));
		}
	}

	// bash evaluates an integer variable's old value on its own before
	// adding to it: a bad one is the error, and the line is dropped.
	static bool integerBaseFails(Environment& env, Expander& expander,
			const Executor::ScalarAssign& scalar) {
		if (!scalar.append || (env.attributes(scalar.name) & kAttrInteger) == 0) {
			return false;
		}

		const std::string current = env.get(scalar.name);
		if (current.empty()) return false;

		expander.evalArithLiteral(current);
		if (!expander.failed()) return false;

		reportExpanderError(expander);
		return true;
	}

	static void applyAssignNow(Environment& env, Expander& expander,
			Executor::SimpleCmdAssigns& expanded) {
		for (const auto& scalar : expanded.scalar) {
			if (integerBaseFails(env, expander, scalar)) return;
			applyScalarAssignToEnv(env, scalar);
		}

		for (const auto& elem : expanded.elem)     applyElemAssignToEnv(env, expander, elem);
		for (const auto& array : expanded.array)   applyArrayAssignToEnv(env, expander, array);
	}

	// `a[1]=(x)` parses, but bash refuses it and drops the rest of the line.
	static void rejectListToElement(Expander& expander, const Assignment& assignment) {
		shellErrorf("%s[%s]: cannot assign list to array member\n", assignment.name.c_str(),
			assignment.subscript.raw.c_str());
		expander.dropRestOfLine();
	}

	static void expandAssignInto(Expander& expander, const Assignment& assignment,
			Executor::SimpleCmdAssigns& out) {
		if (assignment.is_array && assignment.has_subscript) {
			rejectListToElement(expander, assignment);
			return;
		}

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

	// A bare assignment to a readonly variable fails with status 1 and, as
	// in bash, drops the rest of the line: a subshell ends there.
	static bool assignsReadonly(const Environment& env,
	                            const Executor::SimpleCmdAssigns& assigns) {
		for (const auto& scalar : assigns.scalar) {
			if (env.isReadonly(scalar.name)) return true;
		}

		for (const auto& elem : assigns.elem) {
			if (env.isReadonly(elem.name)) return true;
		}

		for (const auto& array : assigns.array) {
			if (env.isReadonly(array.name)) return true;
		}

		return false;
	}

	// A readonly prefix assignment is reported as it expands, before the
	// command's redirections, and the command runs without it.
	static bool dropReadonlyPrefix(const Environment& env, Executor::SimpleCmdAssigns& one) {
		if (one.scalar.empty() || !env.isReadonly(one.scalar.back().name)) return false;

		shellErrorf("%s: readonly variable\n", env.resolveRef(one.scalar.back().name).c_str());
		one.scalar.pop_back();
		return true;
	}

	bool Executor::expandOneAssign(const Assignment& assignment, SimpleCmdAssigns& out, bool bare,
			SavedVars& prior) {
		SimpleCmdAssigns one;
		expandAssignInto(expander_, assignment, one);
		if (expander_.failed() || flowPending()) return false;

		traceAssignment(assignment, one);
		checkXtraceFdAssignment(one);
		dropRestrictedHashAssignments(one);
		if (bare) {
			const bool readonly = assignsReadonly(env_, one);
			applyAssignNow(env_, expander_, one);
			if (readonly) expander_.dropRestOfLine();
			return !readonly && !expander_.dropLinePending();
		}

		if (dropReadonlyPrefix(env_, one)) prefix_assign_failed_ = true;
		if (!one.scalar.empty()) setPrefixTemporarily(env_, one.scalar.back(), prior);
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
		adoptShellTableArrays();
	}

	template <typename Table>
	static void copyAssocInto(const Environment& env, const char* name, Table& table) {
		const Environment::AssocArray* array = env.getAssocArray(name);
		if (array == nullptr) return;

		table.clear();
		for (const auto& entry : *array) table[entry.first] = entry.second;
	}

	static bool isLegalAliasName(const std::string& name) {
		return name.find_first_of(" \t\n()<>;&|\"'\\`$/") == std::string::npos;
	}

	// A name no alias can have is reported and taken out of BASH_ALIASES.
	void Executor::adoptAliasArray() {
		const Environment::AssocArray* array = env_.getAssocArray("BASH_ALIASES");
		if (array == nullptr) return;

		std::vector<std::string> illegal;
		for (const auto& entry : *array) {
			if (!isLegalAliasName(entry.first)) illegal.push_back(entry.first);
		}

		for (const std::string& name : illegal) {
			shellErrorf("`%s': invalid alias name\n", name.c_str());
			env_.unsetElement("BASH_ALIASES", 0, name);
		}

		const auto before = aliases_;
		copyAssocInto(env_, "BASH_ALIASES", aliases_);
		if (aliases_ != before) ++alias_generation_;
	}

	void Executor::adoptShellTableArrays() {
		adoptAliasArray();
		copyAssocInto(env_, "BASH_CMDS", hashed_);
	}

	void Executor::traceXtrace(const std::vector<std::string>& argv) {
		if (!env_.xtrace() || argv.empty()) return;

		std::string line;
		for (std::size_t i = 0; i < argv.size(); ++i) {
			if (i != 0) line.push_back(' ');
			line += shellQuoteForReuse(argv[i]);
		}

		traceText(line);
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
					// The set reports the readonly error once; restoring it
					// would report it again.
					if (env.isReadonly(name)) {
						env.set(name, assign.second);
						continue;
					}

					prior.push_back({ name, env.has(name), env.get(name), env.isExported(name) });
					env.set(name, assign.second);
					env.exportVar(name);
				}
			}

			// A declaration builtin makes these temporary values the shell's own;
			// what they replaced is returned.
			std::vector<PriorValue> keep(const std::set<std::string>& names) {
				const auto kept = std::stable_partition(prior.begin(), prior.end(),
					[&names](const PriorValue& value) { return names.count(value.name) == 0; });
				std::vector<PriorValue> replaced(kept, prior.end());
				prior.erase(kept, prior.end());
				return replaced;
			}

			// A temporary value made readonly was bash's temporary variable;
			// the one it hid comes back as it was.
			~EnvAssignsGuard() {
				for (auto it = prior.rbegin(); it != prior.rend(); ++it) {
					env.clearReadonly(env.resolveRef(it->name));
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

	static bool isDeclarationBuiltin(const std::string& name) {
		return name == "declare" || name == "typeset" || name == "readonly" || name == "export"
			|| name == "local";
	}

	// `declare -p NAME` only shows the temporary value.
	static bool printsDeclarations(const std::vector<std::string>& args) {
		for (const std::string& arg : args) {
			if (arg == "--" || arg.empty() || arg[0] != '-') return false;
			if (arg.find('p') != std::string::npos) return true;
		}

		return false;
	}

	static std::set<std::string> operandNames(const std::vector<std::string>& args) {
		std::set<std::string> names;
		for (const std::string& arg : args) {
			if (arg.empty() || arg[0] == '-' || arg[0] == '+') continue;

			names.insert(arg.substr(0, arg.find_first_of("=[+")));
		}

		return names;
	}

	// At the top level the temporary value stays; in a function the local
	// keeps it and hides the variable as it was before.
	static void keepDeclaredTemporaries(Executor& exec, executor_detail::EnvAssignsGuard& guard,
			const std::vector<std::string>& args) {
		for (const auto& replaced : guard.keep(operandNames(args))) {
			if (exec.funcDepth() == 0) continue;

			exec.hideBehindLocal(replaced.name, replaced.existed, replaced.value, replaced.exported);
		}
	}

	static int callFunctionWithAssigns(Executor& exec,
	                                   const std::string& cmd,
	                                   const std::vector<std::string>& args,
	                                   const NameValueList& assigns) {
		executor_detail::EnvAssignsGuard guard(exec.env(), assigns);
		const std::size_t outer = exec.pushTempEnvNames(assigns);
		const int status = exec.callFunction(cmd, args);
		exec.popTempEnvNames(outer);
		guard.keep(exec.takePersistedTempNames());
		return status;
	}

	static int callBuiltinWithAssigns(Executor& exec,
	                                  const std::string& cmd,
	                                  const std::vector<std::string>& args,
	                                  const NameValueList& assigns) {
		executor_detail::EnvAssignsGuard guard(exec.env(), assigns);
		const std::size_t outer = exec.pushTempEnvNames(assigns);
		const int status = exec.callBuiltin(cmd, args);
		exec.popTempEnvNames(outer);
		if (isDeclarationBuiltin(cmd) && !printsDeclarations(args)) {
			keepDeclaredTemporaries(exec, guard, args);
		}

		return status;
	}

	int Executor::execSimpleCommandNoArgv(const SimpleCommand& sc, SimpleCmdAssigns& assigns_data) {
		RedirState state;
		if (!applyRedirectionsOrUndo(sc.redirs, state)) return 1;

		applyBareAssignmentsToShell(assigns_data);
		undoRedirections(state);
		return cmdsubst_ran_ ? cmdsubst_status_ : 0;
	}

	bool Executor::posixErrorsExit() const {
		return env_.options().posix && !env_.interactive();
	}

	// The special builtins whose every non-zero status is an error; `eval`,
	// `.` and `source` report theirs through markSpecialBuiltinFailure().
	// For `trap` only a usage error is one: a bad signal is not.
	static bool failureIsError(const std::string& name, int status) {
		static const char* const kErrorOnFailure[] = {
			"break", "continue", "export", "readonly", "set", "shift", "unset",
		};

		if (status == 0) return false;
		if (name == "trap") return status == kStatusBadUsage;
		for (const char* builtin : kErrorOnFailure) {
			if (name == builtin) return true;
		}

		return false;
	}

	// POSIX mode: assignments before a special builtin stay in the shell,
	// and its errors end a non-interactive shell.
	int Executor::runPosixSpecialBuiltin(const std::string& name,
	                                     const std::vector<std::string>& args,
	                                     const NameValueList& assigns) {
		for (const auto& assign : assigns) {
			env_.set(assign.first, assign.second);
			env_.exportVar(assign.first);
			if (isTempEnvName(assign.first) && !isLocalHere(assign.first)) {
				persisted_temp_names_.insert(assign.first);
			}
		}

		special_builtin_failed_ = false;
		const int status = callBuiltin(name, args);
		const bool failed = special_builtin_failed_ || failureIsError(name, status);
		special_builtin_failed_ = false;
		if (failed && posixErrorsExit() && !flowPending()) raiseExit(status);
		return status;
	}

	bool Executor::runsAsPosixSpecialBuiltin(const std::string& name) const {
		return env_.options().posix && isPosixSpecialBuiltin(name) && isBuiltin(name);
	}

	int Executor::runResolvedCommand(const std::vector<std::string>& argv,
	                                 const NameValueList& assigns) {
		const std::string& cmd = argv[0];
		if (env_.restricted() && cmd.find('/') != std::string::npos) {
			shellErrorf("%s: restricted: cannot specify `/' in command names\n", cmd.c_str());
			return 1;
		}

		if (runsAsPosixSpecialBuiltin(cmd)) {
			const std::vector<std::string> args(argv.begin() + 1, argv.end());
			return runPosixSpecialBuiltin(cmd, args, assigns);
		}

		if (isFunction(cmd)) {
			const std::vector<std::string> args(argv.begin() + 1, argv.end());
			return callFunctionWithAssigns(*this, cmd, args, assigns);
		}

		const std::string name = systemBinCommand(cmd);
		int hashed_status = 0;
		if (runsHashedPath(argv, assigns, hashed_status)) return hashed_status;
		if (isBuiltin(name)) {
			const std::vector<std::string> args(argv.begin() + 1, argv.end());
			return callBuiltinWithAssigns(*this, name, args, assigns);
		}

		// As in bash, a job spec alone is `fg` for that job.
		if (argv.size() == 1 && cmd.rfind('%', 0) == 0 && findExecutable(cmd).empty()) {
			return callBuiltinWithAssigns(*this, "fg", argv, assigns);
		}

		return runExternal(argv, assigns);
	}

	// A path `hash -p` gave one of the utilities runs instead of it, as for
	// any program in bash. A path that is gone fails, or under checkhash
	// is forgotten and looked up again.
	bool Executor::runsHashedPath(const std::vector<std::string>& argv,
			const NameValueList& assigns, int& status) {
		const auto found = hashed_.find(argv[0]);
		if (found == hashed_.end() || !isBuiltin(argv[0]) || !runsAsProgram(argv[0])) return false;

		const std::string path = found->second;
		std::error_code ec;
		if (std::filesystem::is_regular_file(utf8ToPath(path_conv_.toWin32(path)), ec)) {
			std::vector<std::string> direct = argv;
			direct[0] = path;
			status = runExternal(direct, assigns);
			return true;
		}

		if (!env_.shopts().checkhash) {
			shellErrorf("%s: No such file or directory\n", path.c_str());
			status = kStatusCommandNotFound;
			return true;
		}

		unhashCommand(argv[0]);
		const std::string external = findExecutable(argv[0]);
		if (!external.empty()) hashCommand(argv[0], path_conv_.toPosix(external));
		return false;
	}

	static int lastLineOf(const Node& node) {
		const std::string* source = node.source_text;
		const int first = static_cast<int>(node.loc.line);
		if (source == nullptr || node.src_end <= node.src_start || node.src_end > source->size()) {
			return first;
		}

		std::size_t end = node.src_end;
		while (end > node.src_start && (*source)[end - 1] == '\n') --end;
		const auto from = source->begin() + static_cast<std::ptrdiff_t>(node.src_start);
		const auto to = source->begin() + static_cast<std::ptrdiff_t>(end);
		return first + static_cast<int>(std::count(from, to, '\n'));
	}

	// POSIX mode: a failed prefix assignment cancels the command, and before
	// a special builtin it ends a non-interactive shell.
	bool Executor::abandonsAfterAssignError(const std::string& command) {
		if (!env_.options().posix) return false;

		if (posixErrorsExit() && isPosixSpecialBuiltin(command)) raiseExit(1);
		setLastStatus(1);
		return true;
	}

	static std::size_t assignmentNameEnd(const std::string& raw) {
		const std::size_t eq = raw.find('=');
		return raw[eq - 1] == '+' ? eq - 1 : eq;
	}

	static bool isKeywordAssignment(const Word& word) {
		if (!isAssignmentWord(word.raw)) return false;

		const std::size_t bracket = word.raw.find('[');
		const std::size_t name_end = assignmentNameEnd(word.raw);
		return bracket > name_end || word.raw[name_end - 1] == ']';
	}

	static Word lexedWord(const std::string& raw, const SourceLoc& loc) {
		Word word;
		word.raw = raw;
		word.loc = loc;

		Lexer lexer(raw);
		std::vector<Token> tokens = lexer.tokenize();
		if (!tokens.empty() && tokens[0].kind == TokKind::Word) {
			word.segments = std::move(tokens[0].segments);
		}

		return word;
	}

	static bool hasKeywordAssignments(const SimpleCommand& sc) {
		return std::any_of(sc.words.begin() + (sc.words.empty() ? 0 : 1), sc.words.end(),
			isKeywordAssignment);
	}

	static Assignment keywordAssignment(const Word& word) {
		const std::string& raw = word.raw;
		const std::size_t name_end = assignmentNameEnd(raw);
		const std::size_t bracket = raw.find('[');
		Assignment assignment;
		assignment.append = raw[name_end] == '+';
		assignment.has_subscript = bracket < name_end;
		assignment.name = raw.substr(0, (std::min)(bracket, name_end));
		assignment.value = lexedWord(raw.substr(raw.find('=') + 1), word.loc);
		if (assignment.has_subscript) {
			const std::string subscript = raw.substr(bracket + 1, name_end - bracket - 2);
			assignment.subscript = lexedWord(subscript, word.loc);
		}

		return assignment;
	}

	// `set -k`: an assignment word anywhere on the line is made as the
	// assignments in front of the command are, and is not an argument.
	static SimpleCommand withKeywordAssignments(const SimpleCommand& sc) {
		SimpleCommand moved = sc;
		moved.words.clear();
		for (std::size_t i = 0; i < sc.words.size(); ++i) {
			if (i > 0 && isKeywordAssignment(sc.words[i])) {
				moved.assignments.push_back(keywordAssignment(sc.words[i]));
			} else {
				moved.words.push_back(sc.words[i]);
			}
		}

		return moved;
	}

	// `exec` with nothing to run, also as `command exec`, keeps the
	// redirections of its command.
	static bool isBareExec(const std::vector<std::string>& argv) {
		if (argv.size() == 1) return argv[0] == "exec";
		return argv.size() == 2 && argv[0] == "command" && argv[1] == "exec";
	}

	// As POSIX orders it, the words expand before the assignment values.
	int Executor::execSimpleCommand(const SimpleCommand& sc) {
		command_first_line_ = firstWordEndLine(sc);
		const bool keywords = env_.options().keyword && hasKeywordAssignments(sc);
		const int status = keywords ? runSimpleCommand(withKeywordAssignments(sc))
			: runSimpleCommand(sc);
		this_command_name_.clear();
		return status;
	}

	int Executor::runSimpleCommand(const SimpleCommand& sc) {
		cmdsubst_ran_ = false;
		command_end_line_ = lastLineOf(sc);

		std::vector<std::string> argv;
		std::vector<bool> compound;
		if (!expandSimpleCmdArgv(sc, argv, compound)) return 1;

		SimpleCmdAssigns assigns_data;
		prefix_assign_failed_ = false;
		if (!expandSimpleCmdAssigns(sc, assigns_data, argv.empty())) return 1;
		if (prefix_assign_failed_ && !argv.empty() && abandonsAfterAssignError(argv[0])) return 1;
		NameValueList assigns = assigns_data.scalarPairs();

		if (isBareExec(argv)) return execBareRedirsForExec(sc.redirs);
		if (argv.empty()) return execSimpleCommandNoArgv(sc, assigns_data);

		if (!assigns_data.array.empty()) {
			shellErrorf("array assignments cannot be used as command prefix\n");
			return 1;
		}

		// bash names `a[1]=x cmd` no identifier and runs cmd without it.
		for (const ElemAssign& elem : assigns_data.elem) {
			shellErrorf("`%s[%s]': not a valid identifier\n", elem.name.c_str(),
				elem.subscript.raw.c_str());
		}

		RedirState state;
		if (!applyRedirectionsOrUndo(sc.redirs, state)) {
			// bash expands the redirections of a command it forks in the
			// child, so an expansion error there fails only that command.
			if (runsAsProgram(argv[0])) expander_.takeDropLine();
			if (posixErrorsExit() && isPosixSpecialBuiltin(argv[0]) && isBuiltin(argv[0])) {
				raiseExit(1);
			}

			return 1;
		}

		traceXtrace(argv);
		compound_args_ = std::move(compound);
		const int status = runResolvedCommand(argv, assigns);
		if (runsAsProgram(argv[0])) noteChildExited();
		env_.set("_", argv.back());
		undoRedirections(state);
		exitIfStdoutReaderGone();
		if (flowPending()) return status;

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

		const bool ends_empty = path.empty() || path.back() == ':';
		if (!current.empty() || ends_empty) dirs.push_back(current);
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
			err = posixErrorText(ec);
			return false;
		}

		const std::string old_pwd = exec.env().get("PWD");
		const fs::path cwd = fs::current_path(ec);
		if (ec) return true;

		if (!old_pwd.empty()) exec.env().set("OLDPWD", old_pwd);
		exec.env().set("PWD", exec.pathConv().toPosix(pathToUtf8(cwd)));
		return true;
	}

	// An empty element of PATH, as POSIX has it, is the current directory.
	static std::string pathElement(const std::string& dir) {
		return dir.empty() ? std::string(".") : dir;
	}

	std::string Executor::lookupInPathDirs(const std::string& name, const std::string& path) {
		for (const auto& element : splitPathList(path)) {
			const std::string dir = pathElement(element);
			std::filesystem::path base = utf8ToPath(path_conv_.toWin32(dir));
			base /= utf8ToPath(name);
			const std::string resolved = tryExecutableWithExtensions(base);
			if (resolved.empty()) continue;

			exec_path_cache_.emplace(name, resolved);
			return resolved;
		}

		return {};
	}

	// With PATH unset bash looks in the working directory, and gives the
	// whole path of what it finds there.
	std::string Executor::searchPath() const {
		if (env_.has("PATH")) return env_.get("PATH");

		std::error_code ec;
		return path_conv_.toPosix(std::filesystem::current_path(ec).generic_u8string());
	}

	std::vector<std::string> Executor::findAllExecutables(const std::string& name) {
		std::vector<std::string> found;
		if (isAbsoluteOrRelativePath(name)) {
			const std::string direct = findExecutable(name);
			if (!direct.empty()) found.push_back(direct);
			return found;
		}

		for (const auto& element : splitPathList(searchPath())) {
			const std::string dir = pathElement(element);
			std::filesystem::path base = utf8ToPath(path_conv_.toWin32(dir));
			base /= utf8ToPath(name);
			const std::string resolved = tryExecutableWithExtensions(base);
			if (!resolved.empty()) found.push_back(resolved);
		}

		return found;
	}

	static bool startsLikeProgram(const std::filesystem::path& path) {
		std::ifstream file(path, std::ios::binary);
		char head[2] = {};
		file.read(head, sizeof(head));
		if (file.gcount() < 2) return false;

		const bool shebang = head[0] == '#' && head[1] == '!';
		return shebang || (head[0] == 'M' && head[1] == 'Z');
	}

	// A file named by its path runs as named when it is a script or a
	// program, as in bash: `./gradlew` beside `gradlew.bat` is the script.
	// A PATH lookup keeps the Windows order (.exe, .cmd, .bat first). A
	// device path is not probed: a `<(...)` pipe takes one stat only.
	static std::string explicitExecutable(const std::filesystem::path& path) {
		const bool probe = !isDevicePath(path) && isExistingFile(path);
		if (probe && startsLikeProgram(path)) return pathToUtf8(path);
		return tryExecutableWithExtensions(path);
	}

	std::string Executor::findExecutable(const std::string& name) {
		if (isAbsoluteOrRelativePath(name)) {
			return explicitExecutable(utf8ToPath(path_conv_.toWin32(name)));
		}

		const std::string path = searchPath();
		if (path != exec_path_cache_path_) {
			clearExecutablePathCache();
			exec_path_cache_path_ = path;
		}

		const auto hashed = hashed_.find(name);
		if (hashed != hashed_.end()) {
			noteHashHit(name);
			return explicitExecutable(utf8ToPath(path_conv_.toWin32(hashed->second)));
		}

		const auto cached = exec_path_cache_.find(name);
		if (cached != exec_path_cache_.end()) return cached->second;

		return lookupInPathDirs(name, path);
	}

	std::string Executor::systemBinCommand(const std::string& word) {
		static const char* const kSystemDirs[] = {
			"/bin/", "/sbin/", "/usr/bin/", "/usr/sbin/", "/usr/local/bin/",
		};

		for (const char* dir : kSystemDirs) {
			const std::size_t length = std::strlen(dir);
			if (word.size() <= length || word.compare(0, length, dir) != 0) continue;

			const std::string name = word.substr(length);
			if (name.find('/') != std::string::npos) return word;
			return findExecutable(word).empty() ? name : word;
		}

		return word;
	}

	// As in bash: a name with a slash that is a directory cannot run (126).
	bool Executor::reportDirectoryCommand(const std::string& name) const {
		if (name.find_first_of("/\\") == std::string::npos) return false;

		std::error_code ec;
		if (!std::filesystem::is_directory(utf8ToPath(path_conv_.toWin32(name)), ec)) return false;

		shellErrorf("%s: Is a directory\n", name.c_str());
		return true;
	}

	// bash looks the command up with its own PATH= prefix: `PATH=dir cmd`.
	std::string Executor::findExecutableWith(const std::string& name,
	                                         const NameValueList& temp_env) {
		const auto is_path = [](const std::pair<std::string, std::string>& assign) {
			return assign.first == "PATH";
		};
		const auto path = std::find_if(temp_env.begin(), temp_env.end(), is_path);
		if (path == temp_env.end() || env_.isReadonly("PATH")) return findExecutable(name);

		Environment::VarState saved = env_.saveVar("PATH");
		env_.set("PATH", path->second);
		const std::string found = findExecutable(name);
		env_.restoreVar("PATH", std::move(saved));
		return found;
	}

	static const char* const kNotFoundHandler = "command_not_found_handle";

	// bash runs the handler in the child it forked for the command, with
	// the command's words as its arguments.
	int Executor::runNotFoundHandler(const std::vector<std::string>& argv) {
		SubshellScope snap = enterSubshell();
		const int status = endSubshell(callFunction(kNotFoundHandler, argv));
		leaveSubshell(snap);
		return status;
	}

	int Executor::runExternal(const std::vector<std::string>& argv,
	                          const NameValueList& temp_env) {
		if (argv.empty()) return 0;
		if (refusePipeCommand(argv[0])) return kStatusCommandNotFound;

		const std::string exec_path = findExecutableWith(systemBinCommand(argv[0]), temp_env);
		if (exec_path.empty()) {
			if (reportDirectoryCommand(argv[0])) return kStatusCannotExecute;
			if (isFunction(kNotFoundHandler)) return runNotFoundHandler(argv);

			reportCommandNotFound(argv[0]);
			return kStatusCommandNotFound;
		}

		if (looksLikeShellScript(exec_path)) return runShellScript(exec_path, argv, temp_env);
		if (startsWithShebang(exec_path)) return runShebangInterpreter(exec_path, argv, temp_env);

#ifdef _WIN32
		const std::vector<std::string> child_argv = prepareExternalArgv(argv, exec_path, temp_env);
		const NameValueList child_env = prepareExternalEnvOverrides(temp_env);
		SpawnCommand command = makeSpawnCommand(child_argv, exec_path,
			buildEnvBlock(env_, child_env));
		if (reportSpawnCommandError(command, argv[0])) return kStatusCannotExecute;

		std::fflush(stdout);
		std::fflush(stderr);

		int spawn_status = 0;
		const bool spawned = spawnExternalAndWait(command.exe, command.cmdline,
			command.envblock, &spawn_status, joinWithSpaces(argv));
		if (spawned) return spawn_status;

		if (::GetLastError() == ERROR_BAD_EXE_FORMAT) {
			return runShellScript(exec_path, argv, temp_env);
		}

		// The file is there but does not start: bash's status for that.
		shellErrorf("%s: %s\n", argv[0].c_str(), lastErrorString().c_str());
		return kStatusCannotExecute;
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

		const std::string exclusions = effectiveEnvValue(temp_env, env_, "MSYS2_ARG_CONV_EXCL");
		for (std::size_t i = 1; i < child_argv.size(); ++i) {
			if (argExcludedFromConversion(child_argv[i], exclusions)) continue;

			child_argv[i] = path_conv_.translateArg(child_argv[i]);
		}

		return child_argv;
	}

	static bool hasOverride(const NameValueList& out, const std::string& name) {
		return std::any_of(out.begin(), out.end(),
			[&name](const std::pair<std::string, std::string>& entry) { return entry.first == name; });
	}

	bool Executor::exportedValue(const std::string& name, std::string& out) const {
		if (!isLoopVariableName(name)) return env_.exportedEntry(name, out);
		if (env_.has(name)) {
			out = env_.get(name);
			return env_.isExported(name);
		}

		NameValueList shadowed;
		appendShadowedExports(shadowed);
		for (const auto& entry : shadowed) {
			if (entry.first != name) continue;

			out = entry.second;
			return true;
		}

		return false;
	}

	void Executor::hideBehindLocal(const std::string& name, bool existed, const std::string& value,
	                               bool exported) {
		if (scope_stack_.empty()) return;

		for (ScopeEntry& entry : scope_stack_.back()) {
			if (entry.name != name) continue;

			entry.saved = Environment::VarState{};
			entry.saved.has_scalar = existed;
			entry.saved.scalar = value;
			entry.saved.exported = exported;
		}
	}

	// A local without a value hides nothing from a child process: as in
	// bash, it gets the exported value the local shadows.
	void Executor::appendShadowedExports(NameValueList& out) const {
		for (auto scope = scope_stack_.rbegin(); scope != scope_stack_.rend(); ++scope) {
			for (const ScopeEntry& entry : *scope) {
				if (entry.name == kLocalOptionsName || env_.has(entry.name)) continue;
				if (!entry.saved.exported || !entry.saved.has_scalar) continue;
				if (hasOverride(out, entry.name)) continue;

				out.emplace_back(entry.name, entry.saved.scalar);
			}
		}
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

		appendExportedFunctions(out);
		appendShadowedExports(out);
		return out;
	}

	bool Executor::spawnExternalAndWait(const std::wstring& exe,
	                                    std::wstring& cmdline,
	                                    std::wstring& envblock,
	                                    int* exit_status,
	                                    const std::string& job_text) {
		SpawnCommand command;
		command.exe      = exe;
		command.cmdline  = cmdline;
		command.envblock = envblock;

		const HANDLE group = startForegroundGroup();
		const HANDLE process = createWithHandleList(command, currentStdio(), spawn_placement_);
		const DWORD error = ::GetLastError();
		if (process == INVALID_HANDLE_VALUE) {
			endForegroundGroup(group);
			::SetLastError(error);
			return false;
		}

		std::vector<HANDLE> processes = { process };
		std::vector<int> statuses;
		*exit_status = waitForegroundProcesses(processes, group, job_text, statuses);
		return true;
	}

	bool Executor::jobControlActive() const {
		return env_.interactive() && consoleInput() != nullptr;
	}

	// A foreground command of an interactive shell runs in a job object
	// of its own, so Ctrl+Z can stop all of its processes together.
	HANDLE Executor::startForegroundGroup() {
		if (!jobControlActive()) return nullptr;

		const HANDLE group = createProcessGroup();
		spawn_placement_.job = group;
		return group;
	}

	void Executor::endForegroundGroup(HANDLE group) {
		spawn_placement_.job = nullptr;
		if (group != nullptr) ::CloseHandle(group);
	}

	// The job's processes are suspended, not ended: `fg` / `bg` resume them.
	int Executor::stopForegroundJob(std::vector<HANDLE> processes, HANDLE group,
	                                const std::string& text) {
		suspendProcessGroup(group);

		Job job;
		job.id = nextJobId();
		job.handle = processes.back();
		job.pid = static_cast<long long>(::GetProcessId(job.handle));
		job.cmd_text = text;
		job.stopped = true;
		job.exit_code = kStatusStopped;
		job.group = group;
		processes.pop_back();
		job.stages = std::move(processes);
		touchJob(job);
		jobs_.push_back(job);

		std::fputc('\n', stderr);
		printJobState(jobs_.back());
		return kStatusStopped;
	}

	void Executor::printJobState(const Job& job) const {
		std::fprintf(stderr, "%s\n", jobLine(job).c_str());
	}

	static HANDLE firstStillRunning(const std::vector<HANDLE>& processes) {
		for (const HANDLE process : processes) {
			if (::WaitForSingleObject(process, 0) == WAIT_TIMEOUT) return process;
		}

		return nullptr;
	}

	// A wbsh passes the signal on, so it ends its own foreground command in
	// turn; anything else can only be terminated.
	static void passSignalOn(const std::vector<HANDLE>& processes, int signum) {
		for (const HANDLE process : processes) {
			if (postShellSignal(::GetProcessId(process), signum)) continue;
			::TerminateProcess(process, static_cast<UINT>(kSignalStatusBase + signum));
		}
	}

	// The signal is held while the command ends, so the mailbox's watcher
	// does not end the shell first, and posted again for the safe point.
	static void endForegroundWith(const std::vector<HANDLE>& processes, int signum) {
		takeShellSignal(signum);
		passSignalOn(processes, signum);
		if (!processes.empty() && processes.size() <= MAXIMUM_WAIT_OBJECTS) {
			::WaitForMultipleObjects(static_cast<DWORD>(processes.size()), processes.data(), TRUE,
				INFINITE);
		}

		postShellSignal(::GetCurrentProcessId(), signum);
	}

	// A signal that ends the shell reaches the command it waits for too, as
	// it would the whole foreground process group on Unix; one with a trap
	// waits for the command to finish, as in bash.
	void Executor::endForegroundOnFatalSignal(const std::vector<HANDLE>& processes) {
		const HANDLE signal_event = static_cast<HANDLE>(shellSignalEvent());
		if (signal_event == nullptr) return;

		for (;;) {
			const HANDLE running = firstStillRunning(processes);
			if (running == nullptr) return;

			const HANDLE waits[2] = { running, signal_event };
			if (::WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) continue;

			const int signum = interruptingSignal();
			if (signum == 0) continue;
			if (hasTrap(signalName(signum))) return;

			endForegroundWith(processes, signum);
			return;
		}
	}

	int Executor::waitForegroundProcesses(std::vector<HANDLE>& processes, HANDLE group,
	                                      const std::string& text, std::vector<int>& statuses) {
		spawn_placement_.job = nullptr;
		if (processes.empty()) {
			endForegroundGroup(group);
			statuses.clear();
			return 1;
		}

		const HANDLE console = group != nullptr ? consoleInput() : nullptr;
		if (group == nullptr) endForegroundOnFatalSignal(processes);
		if (waitForeground(processes, console) == ForegroundEnd::Stopped) {
			statuses.assign(processes.size(), kStatusStopped);
			return stopForegroundJob(processes, group, text);
		}

		statuses.clear();
		for (const HANDLE process : processes) statuses.push_back(waitProcessAndClose(process));
		if (group != nullptr) ::CloseHandle(group);
		return statuses.empty() ? 0 : statuses.back();
	}

	static void resumeJob(Executor::Job& job) {
		if (job.group != nullptr) resumeProcessGroup(job.group);
		else resumeProcessById(static_cast<unsigned long>(job.pid));
		job.stopped = false;
	}

	int Executor::waitForegroundJob(Job& job) {
		std::vector<HANDLE> processes = job.stages;
		processes.push_back(job.handle);

		const HANDLE console = jobControlActive() ? consoleInput() : nullptr;
		if (waitForeground(processes, console) == ForegroundEnd::Stopped) {
			if (job.group != nullptr) suspendProcessGroup(job.group);
			else suspendProcessById(static_cast<unsigned long>(job.pid));
			job.stopped = true;
			job.exit_code = kStatusStopped;
			touchJob(job);
			std::fputc('\n', stderr);
			printJobState(job);
			return kStatusStopped;
		}

		return waitForJob(job.id);
	}

	int Executor::foregroundJob(int id) {
		for (Job& job : jobs_) {
			if (job.id != id) continue;

			if (job.stopped) resumeJob(job);
			return waitForegroundJob(job);
		}

		return -1;
	}

	bool Executor::continueJobInBackground(int id) {
		for (Job& job : jobs_) {
			if (job.id != id || !job.stopped) continue;

			resumeJob(job);
			touchJob(job);
			std::fprintf(stderr, "[%d]%s %s &\n", job.id, &job == currentJob() ? "+" : " ",
				job.cmd_text.c_str());
			return true;
		}

		return false;
	}

	bool Executor::signalJob(Job& job, int signum) {
		if (signum == kSignalStop || signum == kSignalTerminalStop) {
			if (job.group != nullptr) suspendProcessGroup(job.group);
			else suspendProcessById(static_cast<unsigned long>(job.pid));
			if (!job.stopped) touchJob(job);
			job.stopped = true;
			job.exit_code = 128 + signum;
			return true;
		}

		if (signum == kSignalContinue) {
			resumeJob(job);
			return true;
		}

		if (signum == 0) return job.running;
		if (job.group != nullptr) return ::TerminateJobObject(job.group, 128 + signum) != 0;
		return ::TerminateProcess(job.handle, static_cast<UINT>(128 + signum)) != 0;
	}

	// The rest of the sleep becomes a child shell's `sleep`, started
	// stopped, so `fg` / `bg` can carry on with it like any other job.
	int Executor::stopRemainingSleep(unsigned long milliseconds, const std::string& text) {
		char seconds[32];
		std::snprintf(seconds, sizeof(seconds), "%.3f", milliseconds / 1000.0);
		const std::string rest = std::string("sleep ") + seconds;

		const HANDLE group = startForegroundGroup();
		const StdioHandles stdio = currentStdio();
		const HANDLE process = spawnSelfCommand(rest, stdio.in, stdio.out, stdio.err);
		spawn_placement_.job = nullptr;
		if (process == INVALID_HANDLE_VALUE) {
			endForegroundGroup(group);
			return 1;
		}

		return stopForegroundJob({ process }, group, text);
	}

	void Executor::endStoppedJobs() {
		for (Job& job : jobs_) {
			if (!job.stopped) continue;

			if (job.group != nullptr) ::TerminateJobObject(job.group, kStatusHangup);
			else ::TerminateProcess(job.handle, kStatusHangup);
		}
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
		if (!line.empty() && line.back() == '\r') line.pop_back();
		return line;
	}

	// The words after `#!`. Linux gives the interpreter the rest of the
	// line as one argument; wbsh splits it at blanks, as macOS does, so
	// `#!/usr/bin/env python3 -u` runs `python3 -u`.
	static std::vector<std::string> shebangWords(const std::string& shebang_line) {
		std::vector<std::string> words;
		std::string word;
		for (std::size_t i = 2; i < shebang_line.size(); ++i) {
			const char c = shebang_line[i];
			if (c != ' ' && c != '\t') {
				word.push_back(c);
				continue;
			}

			if (!word.empty()) words.push_back(std::move(word));
			word.clear();
		}

		if (!word.empty()) words.push_back(std::move(word));
		return words;
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

	bool Executor::startsWithShebang(const std::string& path) const {
		if (classifyByExtension(path) == ExtensionClass::Native) return false;
		return !readShebangLine(path).empty();
	}

	int Executor::runShebangInterpreter(const std::string& path,
	                                    const std::vector<std::string>& argv,
	                                    const NameValueList& temp_env) {
		static const int kMaxInterpreterDepth = 4;

		std::vector<std::string> interp_argv = shebangWords(readShebangLine(path));
		if (interp_argv.empty()) return runShellScript(path, argv, temp_env);

		const std::string& interp = interp_argv[0];
		const std::string name = systemBinCommand(interp);
		if (!isBuiltin(name) && findExecutable(name).empty()) {
			shellErrorWithoutLine(argv[0] + ": " + interp
				+ ": bad interpreter: No such file or directory");
			return kStatusCannotExecute;
		}

		if (shebang_depth_ >= kMaxInterpreterDepth) {
			shellErrorf("%s: too many levels of interpreter scripts\n",
				argv[0].c_str());
			return kStatusCannotExecute;
		}

		// The kernel passes the script as it was named; a PATH hit as its path.
		const bool named_by_path = argv[0].find_first_of("/\\") != std::string::npos;
		interp_argv.push_back(named_by_path ? argv[0] : path_conv_.toPosix(path));
		interp_argv.insert(interp_argv.end(), argv.begin() + 1, argv.end());

		++shebang_depth_;
		int status = 0;
		if (isBuiltin(name)) {
			interp_argv[0] = name;
			const std::vector<std::string> args(interp_argv.begin() + 1, interp_argv.end());
			status = callBuiltinWithAssigns(*this, name, args, temp_env);
		} else {
			status = runExternal(interp_argv, temp_env);
		}
		--shebang_depth_;
		return status;
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
		snap.saved_function_files = function_files_;
		snap.saved_exported_functions = exported_functions_;
		snap.saved_call_stack = call_stack_;
		snap.saved_traps = trap_handlers_;
		snap.saved_startup_ignored = startup_ignored_;
		snap.saved_lists_inherited_traps = lists_inherited_traps_;
		snap.saved_options = env_.options();
		snap.saved_expand_aliases = env_.expand_aliases();

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
		// from the original implementation. A readonly variable still holds
		// the saved value; writing it back must not report an error.
		for (const auto& var : snap.saved_vars) {
			if (force_set || env_.isReadonly(var.first)) env_.forceSet(var.first, var.second);
			else                                         env_.set(var.first, var.second);
		}

		env_.setPositional(std::move(snap.saved_pos));
		env_.setShellName(snap.saved_name);
		env_.options() = snap.saved_options;
		env_.setExpandAliases(snap.saved_expand_aliases);
		functions_ = std::move(snap.saved_funcs);
		function_files_ = std::move(snap.saved_function_files);
		exported_functions_ = std::move(snap.saved_exported_functions);
		call_stack_ = std::move(snap.saved_call_stack);
		publishCallStack();
		trap_handlers_ = std::move(snap.saved_traps);
		startup_ignored_ = std::move(snap.saved_startup_ignored);
		lists_inherited_traps_ = snap.saved_lists_inherited_traps;
		publishSignalDispositions();

		std::error_code ec;
		std::filesystem::current_path(snap.saved_cwd, ec);
	}

	static std::string readScriptBody(std::ifstream& file) {
		std::stringstream buffer;
		buffer << file.rdbuf();
		std::string body = buffer.str();
		stripUtf8Bom(body);
		normalizeCrlf(body);
		return body;
	}

	// `$0` as bash sets it: the path as typed, or for a PATH hit the POSIX
	// form of the file found, so `dirname "$0"` launchers see a POSIX path.
	std::string Executor::scriptName(const std::string& typed, const std::string& path) const {
		if (isAbsoluteOrRelativePath(typed)) return typed;
		return path_conv_.toPosix(path);
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
		env_.setExpandAliases(false);
		resetTrapsForScript();
		beginExecRedirJournal();

		const std::string name = scriptName(argv[0], path);
		for (const auto& assignment : temp_env) env_.set(assignment.first, assignment.second);
		env_.setShellName(name);
		env_.setPositional(std::vector<std::string>(argv.begin() + 1, argv.end()));
		call_stack_.clear();
		beginScriptFrame(name);

		int status = executeInputText(body, name);
		if (consumeFlow(FlowSignal::Kind::Exit, &status)) forgetExecExitCode();
		if (!flowPending()) status = fireExitTrap(status);
		endExecRedirJournal();
		restoreShellScriptScope(snap, /*force_set=*/flowPending());
		return status;
	}

	int Executor::execBraceGroup(const BraceGroup& group) {
		RedirState state;
		if (!applyCompoundRedirections(group.redirs, state)) return 1;

		int status = 0;
		if (group.body != nullptr) status = execNode(*group.body);
		undoRedirections(state);
		return status;
	}

	Executor::SubshellScope Executor::enterSubshell() {
		SubshellScope snap{ env_ };
		snap.functions = functions_;
		snap.function_files = function_files_;
		snap.aliases = aliases_;
		snap.disabled_builtins = disabled_builtins_;
		snap.traps = trap_handlers_;
		snap.inherited_traps = inherited_traps_;
		snap.lists_inherited_traps = lists_inherited_traps_;
		snap.dir_stack = dir_stack_;
		snap.scope_stack = scope_stack_;
		snap.getopts_subindex = getopts_subindex_;
		snap.owned_jobs = lendJobsToSubshell();

		std::error_code ec;
		snap.cwd = std::filesystem::current_path(ec);

		resetSubshellTraps();
		env_.reseedRandom();
		env_.enterSubshellPid();
		beginExecRedirJournal();
		return snap;
	}

	// A subshell lists the parent's jobs but cannot wait for or resume them.
	std::vector<int> Executor::lendJobsToSubshell() {
		std::vector<int> owned;
		for (Job& job : jobs_) {
			if (!job.inherited) owned.push_back(job.id);
			job.inherited = true;
		}

		return owned;
	}

	void Executor::takeBackJobs(const std::vector<int>& owned) {
		for (Job& job : jobs_) {
			if (std::find(owned.begin(), owned.end(), job.id) != owned.end()) job.inherited = false;
		}
	}

	// The parent's traps, EXIT included, belong to the parent and never
	// run in the subshell; only ignored signals stay ignored, and ERR /
	// DEBUG / RETURN are inherited under errtrace / functrace.
	void Executor::resetSubshellTraps() {
		if (!lists_inherited_traps_) inherited_traps_ = trap_handlers_;
		lists_inherited_traps_ = true;

		for (auto it = trap_handlers_.begin(); it != trap_handlers_.end(); ) {
			if (it->second.empty() || inheritsTrap(it->first)) ++it;
			else it = trap_handlers_.erase(it);
		}

		publishSignalDispositions();
	}

	void Executor::leaveSubshell(SubshellScope& snap) {
		endExecRedirJournal();
		env_.restoreFrom(snap.env);
		functions_ = std::move(snap.functions);
		function_files_ = std::move(snap.function_files);
		aliases_ = std::move(snap.aliases);
		disabled_builtins_ = std::move(snap.disabled_builtins);
		trap_handlers_ = std::move(snap.traps);
		inherited_traps_ = std::move(snap.inherited_traps);
		lists_inherited_traps_ = snap.lists_inherited_traps;
		publishSignalDispositions();
		dir_stack_ = std::move(snap.dir_stack);
		scope_stack_ = std::move(snap.scope_stack);
		getopts_subindex_ = snap.getopts_subindex;
		takeBackJobs(snap.owned_jobs);

		std::error_code ec;
		std::filesystem::current_path(snap.cwd, ec);
	}

	// `exit`, `return`, `break` and `continue` all end just the subshell,
	// with the status they carry.
	int Executor::consumeSubshellFlow(int status) {
		if (!flowPending()) return status;

		const int flow_status = flow_.status;
		clearFlow();
		forgetExecExitCode();
		return flow_status;
	}

	int Executor::endSubshell(int status) {
		return fireExitTrap(consumeSubshellFlow(status));
	}

	int Executor::execSubshell(const Subshell& subshell) {
		RedirState state;
		if (!applyCompoundRedirections(subshell.redirs, state)) return 1;

		const std::size_t hidden = hideSavedFds(state);
		SubshellScope snap = enterSubshell();
		int status = 0;
		if (subshell.body != nullptr) status = execNode(*subshell.body);
		status = endSubshell(status);
		leaveSubshell(snap);
		showHiddenFds(hidden);

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
		if (!applyCompoundRedirections(clause.redirs, state)) return 1;

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
		case FlowSignal::Kind::Discard:
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
			if (loopEnds(dispatchLoopFlow()) || stopForCtrlC(status)) break;
		}

		return status;
	}

	int Executor::execWhile(const WhileClause& clause) {
		RedirState state;
		if (!applyCompoundRedirections(clause.redirs, state)) return 1;

		++loop_depth_;
		const int status = runWhileLoop(clause);
		--loop_depth_;
		undoRedirections(state);
		return status;
	}

	bool Executor::evalLoopArith(const std::string& expr, long long& value) {
		if (!expr.empty()) announceCommand("((" + expr + "))");
		if (flowPending()) return false;

		return evalTracedArith(expr, true, value);
	}

	// An omitted part evaluates as 1, so an omitted condition loops forever.
	int Executor::runArithLoop(const ForClause& clause) {
		long long value = 0;
		if (!evalLoopArith(clause.arith_init, value)) return 1;

		int status = 0;
		for (;;) {
			if (!evalLoopArith(clause.arith_cond, value)) return 1;
			if (value == 0) break;

			runBody(clause.body, status);
			if (loopEnds(dispatchLoopFlow()) || stopForCtrlC(status)) break;

			if (!evalLoopArith(clause.arith_update, value)) return 1;
		}

		return status;
	}

	int Executor::execForArith(const ForClause& clause) {
		RedirState state;
		if (!applyCompoundRedirections(clause.redirs, state)) return 1;

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

	// BASH_COMMAND for a `for` / `select`: the header up to `; do` or the newline.
	std::string Executor::loopHeaderText(const ForClause& clause) const {
		if (trap_depth_ > 0) return {};

		const std::string slice = nodeSourceSlice(clause, source_text_);
		const std::size_t end = slice.find_first_of(";\n");
		return end == std::string::npos ? slice : slice.substr(0, end);
	}

	// A nameref loop variable is pointed at each word in turn.
	// The set reports a readonly variable; bash then ends the loop with
	// status 1.
	static bool setLoopVariable(Environment& env, const std::string& name,
			const std::string& value) {
		if (env.isNameref(name)) {
			if (env.rejectsRefValue(value)) return false;

			env.setNameref(name, value);
			return true;
		}

		env.set(name, value);
		return !env.isReadonly(name);
	}

	// In POSIX mode a readonly loop variable ends the shell, with the
	// status bash gives it.
	bool Executor::assignLoopVariable(const std::string& name, const std::string& value) {
		if (setLoopVariable(env_, name, value)) return true;

		if (posixErrorsExit()) raiseExit(kStatusCommandNotFound);
		return false;
	}

	int Executor::runForLoop(const ForClause& clause) {
		int status = 0;
		std::vector<std::string> values;
		if (!expandForWordList(clause, values, &status)) return status;

		const std::string header = loopHeaderText(clause);
		for (const auto& value : values) {
			announceCommand(header);
			if (flowPending()) break;

			traceLoopHeader("for", clause);
			if (!assignLoopVariable(clause.var, value)) return 1;
			runBody(clause.body, status);
			if (loopEnds(dispatchLoopFlow()) || stopForCtrlC(status)) break;
		}

		return status;
	}

	static bool isLoopVariableName(const std::string& name) {
		if (name.empty()) return false;
		if (std::isdigit(static_cast<unsigned char>(name[0])) != 0) return false;

		return std::all_of(name.begin(), name.end(), [](char c) {
			return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
		});
	}

	// bash checks the name when the loop runs, not when it parses it.
	bool Executor::loopVariableIsValid(const ForClause& clause) {
		if (isLoopVariableName(clause.var)) return true;

		shellErrorf("`%s': not a valid identifier\n", clause.var.c_str());
		if (posixErrorsExit()) raiseExit(2);
		return false;
	}

	int Executor::execFor(const ForClause& clause) {
		if (clause.is_arith) return execForArith(clause);
		if (!loopVariableIsValid(clause)) return 1;
		if (clause.is_select) return execSelect(clause);

		RedirState state;
		if (!applyCompoundRedirections(clause.redirs, state)) return 1;

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
		if (values.empty()) return 0;

		const std::string ps3 =env_.has("PS3") ? env_.get("PS3") : "#? ";
		const std::string header = loopHeaderText(clause);
		for (;;) {
			announceCommand(header);
			if (flowPending()) break;

			traceLoopHeader("select", clause);
			printSelectMenu(values);
			std::fputs(ps3.c_str(), stderr);
			std::fflush(stderr);

			std::string reply;
			if (!readSelectReplyLine(reply)) return 1;
			env_.set("REPLY", reply);
			if (!assignLoopVariable(clause.var, selectedValue(reply, values))) return 1;

			runBody(clause.body, status);
			if (loopEnds(dispatchLoopFlow()) || stopForCtrlC(status)) break;
		}

		return status;
	}

	int Executor::execSelect(const ForClause& clause) {
		RedirState state;
		if (!applyCompoundRedirections(clause.redirs, state)) return 1;

		++loop_depth_;
		const int status = runSelectLoop(clause);
		--loop_depth_;
		undoRedirections(state);
		return status;
	}

	bool Executor::patternMatches(const std::string& pat, const std::string& s) {
		if (!env_.nocasematch()) return fnmatchFull(pat, s, env_.extglob());
		return fnmatchFull(asciiLower(pat), asciiLower(s), env_.extglob());
	}

	bool Executor::caseItemMatches(const CaseClause::Item& item, const std::string& subject,
			bool& failed) {
		for (const auto& pattern_word : item.patterns) {
			const std::string pattern = expander_.expandPattern(pattern_word);
			if (expander_.failed()) {
				reportExpanderError(expander_);
				exitOnFatalExpansionError();
				failed = true;
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
			bool failed = false;
			if (!caseItemMatches(item, subject, failed)) {
				if (failed) return 1;
				if (flowPending()) break;
				continue;
			}

			i = runCaseArmsFrom(clause, i, status);
			if (flowPending()) break;
			if (clause.items[i].term != CaseClause::Term::DSemiAmp) break;
		}

		return status;
	}

	// Runs the arm at @p first and every arm `;&` falls into after it;
	// returns the index of the last arm run.
	std::size_t Executor::runCaseArmsFrom(const CaseClause& clause, std::size_t first,
			int& status) {
		std::size_t i = first;
		for (;;) {
			runBody(clause.items[i].body, status);
			if (flowPending()) return i;

			const bool falls_through = clause.items[i].term == CaseClause::Term::SemiAmp;
			if (!falls_through || i + 1 >= clause.items.size()) return i;
			++i;
		}
	}

	static bool isBlankOrEnd(const std::string& text, std::size_t pos) {
		return pos >= text.size() || text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n';
	}

	// BASH_COMMAND for a `case`: its header, `case WORD in `.
	static std::string caseHeaderText(const std::string& slice) {
		std::size_t pos = slice.find(" in");
		while (pos != std::string::npos && !isBlankOrEnd(slice, pos + 3)) {
			pos = slice.find(" in", pos + 1);
		}

		return pos == std::string::npos ? slice : slice.substr(0, pos + 3) + ' ';
	}

	int Executor::execCase(const CaseClause& clause) {
		RedirState state;
		if (!applyCompoundRedirections(clause.redirs, state)) return 1;

		const std::string header = caseHeaderText(nodeSourceSlice(clause, source_text_));
		announceCommand(header);
		traceCaseHeader(clause);
		if (flowPending()) {
			undoRedirections(state);
			return last_status_;
		}

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

	// A definition sets no line number of its own: bash reports the line
	// it has read through, the end of the input command around it.
	int Executor::definitionErrorLine(const FunctionDef& fd) const {
		const int own = lastLineOf(fd);
		if (func_depth_ > 0 || input_command_end_line_ < own) return own;
		return input_command_end_line_;
	}

	// POSIX interpretation 383: a function cannot take a special builtin's name.
	bool Executor::rejectSpecialBuiltinFunction(const FunctionDef& fd) {
		const std::string& name = fd.name;
		if (!env_.options().posix || !isPosixSpecialBuiltin(name)) return false;

		env_.setCurrentLineno(definitionErrorLine(fd));

		shellErrorf("`%s': is a special builtin\n", name.c_str());
		if (env_.interactive()) raiseDiscard();
		else raiseExit(kSyntaxErrorStatus);
		return true;
	}

	// bash will not define a function whose name had quotes, `$` or a
	// process substitution in it; it says so on the definition's last
	// line, and in POSIX mode the shell ends.
	bool Executor::functionNameAccepted(const FunctionDef& fd) {
		const std::string& name = fd.name;
		const bool expanded = name.find_first_of("$'\"`\\") != std::string::npos
			|| name.rfind("<(", 0) == 0 || name.rfind(">(", 0) == 0;
		if (!expanded) return true;

		env_.setCurrentLineno(definitionErrorLine(fd));
		shellErrorf("`%s': not a valid identifier\n", name.c_str());
		if (posixErrorsExit()) raiseExit(2);
		return false;
	}

	bool Executor::rejectsReadonlyFunction(const FunctionDef& fd) {
		if (!isFunctionReadonly(fd.name)) return false;

		env_.setCurrentLineno(definitionErrorLine(fd));
		shellErrorf("%s: readonly function\n", fd.name.c_str());
		return true;
	}

	int Executor::execFunctionDef(const FunctionDef& fd) {
		if (rejectSpecialBuiltinFunction(fd)) return kSyntaxErrorStatus;
		if (!functionNameAccepted(fd) || rejectsReadonlyFunction(fd)) return 1;

		functions_[fd.name] = &fd;
		++function_definitions_;
		function_files_[fd.name] = currentSourceFile();
		return 0;
	}

	namespace executor_detail {
		struct DBracketContext {
			Executor& exec;
			Expander& expander;
			bool nocasematch;
			bool& test_error;
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
			shellErrorf("[[: invalid regular expression `%s': %s\n", rhs.c_str(), re.error().c_str());
			ctx.test_error = true;
			return false;
		}

		RegexMatch match;
		const bool matched = re.search(lhs, 0, match);
		ctx.exec.env().setIndexedArrayFromList("BASH_REMATCH",
			matched ? rematchGroups(lhs, match) : std::vector<std::string>());
		return matched;
	}

	// An arithmetic error is reported, `[[:`-labelled, and makes the test false.
	// An operand with a subscript is read as written, as `(( ))` reads it:
	// a key its expansion yields is not parsed again.
	static bool evalDBracketOperand(Expander& expander, const Word& word, const std::string& text,
			long long& out) {
		const bool subscripted = typedAsArrayReference(word.raw);
		out = subscripted ? expander.evalArith(word.raw) : expander.evalArithExpanded(text);
		if (!expander.failed()) return true;
		if (expander.fatalErrorPending()) return false;

		const char* label = expander.errorLabel("[[: ");
		shellErrorf("%s%s\n", label, expander.takeError().c_str());
		return false;
	}

	// `[[ ]]` evaluates both sides of -eq and friends as arithmetic.
	static bool evalDBracketArithOp(const DBracketCond::Expr& expr, const std::string& lhs,
	                                const std::string& rhs, Expander& expander) {
		long long left = 0;
		long long right = 0;
		if (!evalDBracketOperand(expander, expr.lhs, lhs, left)) return false;
		if (!evalDBracketOperand(expander, expr.rhs, rhs, right)) return false;

		const std::string& op = expr.op;
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

	// bash traces each test it evaluates, a `!` right before it included.
	static void traceDBracket(const DBracketContext& ctx, bool negated,
	                          std::vector<std::string> parts) {
		if (negated) parts.insert(parts.begin(), "!");
		ctx.exec.traceConditional(parts);
	}

	static bool evalDBracketBinary(const DBracketCond::Expr& expr, const std::string& lhs,
	                               const DBracketContext& ctx, bool negated) {
		const std::string rhs = expandDBracketRhs(expr, ctx.expander);
		if (ctx.expander.failed()) return false;

		traceDBracket(ctx, negated, { lhs, expr.op, rhs });

		if (isDBracketStringOp(expr.op)) {
			const std::string cmp_lhs = ctx.nocasematch ? asciiLower(lhs) : lhs;
			const std::string cmp_rhs = ctx.nocasematch ? asciiLower(rhs) : rhs;
			return evalDBracketStringOp(expr.op, cmp_lhs, cmp_rhs);
		}

		if (expr.op == "=~") return evalDBracketRegexOp(lhs, rhs, ctx);
		if (isDBracketArithOp(expr.op)) return evalDBracketArithOp(expr, lhs, rhs, ctx.expander);
		if (isFileCompareOperator(expr.op)) return evalFileCompare(ctx.exec, expr.op, lhs, rhs);
		return false;
	}

	static bool isFdNumber(const std::string& text) {
		int fd = -1;
		std::size_t used = 0;
		return parseInt(text, fd, 10, &used) && used == text.size();
	}

	// `[[ -t X ]]` is an error, as in bash, not a false test.
	static bool rejectTerminalOperand(const std::string& operand, const DBracketContext& ctx) {
		shellErrorf("[[: %s: integer expected\n", operand.c_str());
		ctx.test_error = true;
		return false;
	}

	static bool evalDBracketUnary(const DBracketCond::Expr& expr, const std::string& lhs,
	                              const DBracketContext& ctx, bool negated) {
		traceDBracket(ctx, negated, { expr.op, lhs });
		if (expr.op == "-t" && !isFdNumber(lhs)) return rejectTerminalOperand(lhs, ctx);

		return evalUnaryTestOperator(ctx.exec, expr.op[1], lhs);
	}

	static bool evalDBracketPrimary(const DBracketCond::Expr& expr, const DBracketContext& ctx,
	                                bool negated) {
		const std::string lhs = ctx.expander.expandStringValue(expr.lhs);
		if (ctx.expander.failed()) return false;
		if (expr.op.empty()) {
			traceDBracket(ctx, negated, { "-n", lhs });
			return !lhs.empty();
		}

		const bool unary = expr.op == "-t" || isUnaryTestOperator(expr.op);
		if (unary) return evalDBracketUnary(expr, lhs, ctx, negated);

		return evalDBracketBinary(expr, lhs, ctx, negated);
	}

	static bool evalDBracketExpr(const DBracketCond::Expr& expr, const DBracketContext& ctx);

	static bool evalDBracketNegated(const DBracketCond::Expr& expr, const DBracketContext& ctx) {
		if (expr.k == DBracketCond::Expr::K::Prim) return !evalDBracketPrimary(expr, ctx, true);

		return !evalDBracketExpr(expr, ctx);
	}

	static bool evalDBracketExpr(const DBracketCond::Expr& expr, const DBracketContext& ctx) {
		using K = DBracketCond::Expr::K;
		switch (expr.k) {
		case K::And:  return evalDBracketExpr(*expr.a, ctx) && evalDBracketExpr(*expr.b, ctx);
		case K::Or:   return evalDBracketExpr(*expr.a, ctx) || evalDBracketExpr(*expr.b, ctx);
		case K::Not:  return evalDBracketNegated(*expr.a, ctx);
		case K::Prim: return evalDBracketPrimary(expr, ctx, false);
		}

		return false;
	}

	int Executor::execDBracket(const DBracketCond& dc) {
		RedirState state;
		const bool ok = applyRedirections(dc.redirs, state);

		int status = 1;
		if (ok && dc.root != nullptr) {
			bool test_error = false;
			const DBracketContext ctx{ *this, expander_, env_.nocasematch(), test_error };
			status = evalDBracketExpr(*dc.root, ctx) ? 0 : 1;
			if (test_error) status = 2;
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
		const std::string outer = env_.assigningCommand();
		env_.setAssigningCommand("((");
		if (ok && evalTracedArith(ac.expr, false, value)) status = (value != 0) ? 0 : 1;
		env_.setAssigningCommand(outer);

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

	bool Executor::isKnownBuiltin(const std::string& name) const {
		return builtins_.count(name) != 0 || plugin_commands_.count(name) != 0;
	}

	bool Executor::isBuiltin(const std::string& name) const {
		return isKnownBuiltin(name) && disabled_builtins_.count(name) == 0;
	}

	void Executor::setBuiltinEnabled(const std::string& name, bool enabled) {
		if (enabled) disabled_builtins_.erase(name);
		else         disabled_builtins_.insert(name);
	}

	std::vector<std::string> Executor::allBuiltinNames() const {
		std::vector<std::string> names;
		names.reserve(builtins_.size() + plugin_commands_.size());
		for (const auto& entry : builtins_) names.push_back(entry.first);
		for (const auto& entry : plugin_commands_) names.push_back(entry.first);
		return names;
	}

	std::vector<std::string> Executor::builtinNames() const {
		std::vector<std::string> names = allBuiltinNames();
		const auto disabled = [this](const std::string& name) {
			return disabled_builtins_.count(name) != 0;
		};
		names.erase(std::remove_if(names.begin(), names.end(), disabled), names.end());
		return names;
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

	// Builtins for which bash takes `--help` as an operand like any other.
	static bool helpIsAnOperand(const std::string& name) {
		return name == "echo" || name == "test" || name == "[" || name == "true"
			|| name == "false" || name == ":";
	}

	// A synopsis line that starts with blanks continues the form above it.
	static void printSynopsisLines(const std::string& synopsis, const char* first,
	                               const char* rest) {
		const std::string padding(std::strlen(first), ' ');
		std::size_t start = 0;
		for (int line = 0; start <= synopsis.size(); ++line) {
			const std::size_t end = (std::min)(synopsis.find('\n', start), synopsis.size());
			const std::string text = synopsis.substr(start, end - start);
			const bool continues = !text.empty() && text[0] == ' ';
			const char* prefix = line == 0 ? first : continues ? padding.c_str() : rest;
			std::printf("%s%s\n", prefix, text.c_str());
			start = end + 1;
		}
	}

	// `cmd --help` of a command without help of its own prints its synopsis:
	// otherwise `yes --help` printed "--help" for ever and `cat --help`
	// looked for a file of that name. Utilities answer GNU style with status
	// 0, shell builtins bash style with status 2.
	static bool answeredHelpFromManPage(const std::string& name,
	                                    const std::vector<std::string>& args, int& status) {
		if (args.size() != 1 || args[0] != "--help" || helpIsAnOperand(name)) return false;

		const ManPage* page = findManPage(name);
		if (page == nullptr || page->self_help || page->kind == ManKind::Program) return false;

		if (page->kind == ManKind::Shell) {
			const std::string first = name + ": ";
			printSynopsisLines(page->synopsis, first.c_str(), first.c_str());
			std::printf("    %s\n", page->summary);
			status = 2;
			return true;
		}

		printSynopsisLines(page->synopsis, "Usage: ", "  or:  ");
		std::printf("%s\n\nSee `man %s` for more.\n", page->summary, name.c_str());
		status = 0;
		return true;
	}

	// These builtins take no options of their own, yet as in bash a leading
	// `--` ends the options they do not have: `eval -- "a=(x y)"`.
	static bool startsWithIgnoredEndOfOptions(const std::string& name,
			const std::vector<std::string>& args) {
		static const std::unordered_set<std::string> names = {
			"eval", "return", "exit", "logout", "shift", "break", "continue", "let",
			"caller", "getopts", "help", "exec", "pushd", "popd",
		};
		return !args.empty() && args[0] == "--" && names.count(name) != 0;
	}

	int Executor::callBuiltin(const std::string& name, const std::vector<std::string>& args) {
		const auto builtin = builtins_.find(name);
		std::clearerr(stdout);
		if (builtin != builtins_.end()) {
			int help_status = 0;
			if (answeredHelpFromManPage(name, args, help_status)) return help_status;
			const std::string outer = env_.assigningCommand();
			env_.setAssigningCommand(name);
			const int status = startsWithIgnoredEndOfOptions(name, args)
				? builtin->second(*this, std::vector<std::string>(args.begin() + 1, args.end()))
				: builtin->second(*this, args);
			env_.setAssigningCommand(outer);
			return status;
		}

		const auto plugin = plugin_commands_.find(name);
		if (plugin == plugin_commands_.end()) return kStatusCommandNotFound;

		return callPluginCommand(plugin->second, name, args);
	}

	// Each level of shell recursion takes several kB of stack, so a deep
	// enough recursion used to crash the process without a word. Calls stop
	// with bash's FUNCNEST error well before the end of the stack instead;
	// a script exits as bash does, an interactive shell gets its prompt back.
	static bool stackNearlyFull() {
#ifdef _WIN32
		ULONG_PTR low = 0;
		ULONG_PTR high = 0;
		::GetCurrentThreadStackLimits(&low, &high);
		const auto here = reinterpret_cast<ULONG_PTR>(&low);
		return here - low < kStackReserveMargin;
#else
		return false;
#endif
	}

	bool Executor::functionNestingAllowed(const std::string& name) {
		if (nesting_exceeded_) return false;

		long long limit = 0;
		const bool has_limit = parseLL(env_.get("FUNCNEST"), limit) && limit > 0;
		const bool funcnest = has_limit && func_depth_ >= limit;
		if (!funcnest && !stackNearlyFull()) return true;

		const std::string shown = funcnest ? std::to_string(limit) : "out of stack space";
		shellErrorf("%s: maximum function nesting level exceeded (%s)\n",
			name.c_str(), shown.c_str());
		// bash abandons the whole command at the top level and reads on.
		nesting_exceeded_ = true;
		raiseDiscard();
		return false;
	}

	// A forked pipeline stage or background command counts as a subshell
	// when it is a compound command or calls a function; a simple command's
	// words expand at its parent's level, and `( ... )` counts once.
	void Executor::enterInheritedStage(const Node& root) {
		const Node* stage = &root;
		if (root.kind == Node::Kind::List) {
			const List& list = static_cast<const List&>(root);
			if (list.items.size() != 1) return;

			stage = list.items[0].command;
		}

		if (stage->kind == Node::Kind::Subshell) return;
		if (stage->kind == Node::Kind::SimpleCommand) {
			stage_function_bump_ = true;
			return;
		}

		env_.setSubshellLevel(env_.subshellLevel() + 1);
	}

	int Executor::callFunction(const std::string& name, const std::vector<std::string>& args) {
		const auto it = functions_.find(name);
		if (it == functions_.end()) return kStatusCommandNotFound;
		if (std::exchange(stage_function_bump_, false)) {
			env_.setSubshellLevel(env_.subshellLevel() + 1);
		}

		const FunctionDef* def = it->second;
		if (!functionNestingAllowed(name)) return 1;

		pushCallFrame({ name, functionFile(name), env_.currentLineno(), true });
		std::vector<std::string> saved_positional = env_.positional();
		env_.setPositional(args);
		const bool argv_pushed = pushBashArgv(args);
		std::vector<SuspendedTrap> suspended = suspendTrapsForFunction(isFunctionTraced(name));
		if (def->body != nullptr) env_.setCurrentLineno(static_cast<int>(def->body->loc.line));
		fireDebugTrap();

		this_command_name_ = name;
		const int status = runFunctionBody(*def);
		runExitTrapInFunction();

		fireReturnTrap();
		restoreTrapsAfterFunction(suspended);
		popBashArgv(argv_pushed, args.size());
		env_.setPositional(std::move(saved_positional));
		popCallFrame();
		return status;
	}

	// The loops around the call are not the body's to break, as in bash.
	// An `exit` in a function runs the EXIT trap while the function is still
	// on the call stack, so the trap sees its FUNCNAME, as in bash.
	void Executor::runExitTrapInFunction() {
		if (flow_.kind != FlowSignal::Kind::Exit || trap_handlers_.count("EXIT") == 0) return;

		const int code = flow_.status;
		clearFlow();
		raiseExit(fireExitTrap(code));
	}

	int Executor::runFunctionBody(const FunctionDef& def) {
		++func_depth_;
		scope_stack_.emplace_back();
		const int caller_loops = std::exchange(loop_depth_, 0);

		int status = 0;
		if (def.body != nullptr) status = execNode(*def.body);
		consumeFlow(FlowSignal::Kind::Return, &status);

		loop_depth_ = caller_loops;
		popLocalScope();
		--func_depth_;
		if (func_depth_ == 0) nesting_exceeded_ = false;
		return status;
	}

	// A function with its own OPTIND gives its caller's getopts back the
	// place it had reached inside the current argument, as bash 5 does.
	void Executor::restoreGetoptsPosition(int subindex) {
		getopts_subindex_ = subindex;
		noteGetoptsWroteOptind();
	}

	// IGNOREEOF follows ignoreeof back too, as in bash.
	void Executor::restoreLocalOptions(const SetOptions& options) {
		const bool ignored_eof = env_.options().ignoreeof;
		env_.options() = options;
		if (options.ignoreeof == ignored_eof) return;

		if (options.ignoreeof) env_.set("IGNOREEOF", "10");
		else                   env_.unset("IGNOREEOF");
	}

	void Executor::popLocalScope() {
		if (scope_stack_.empty()) return;

		std::vector<ScopeEntry>& top = scope_stack_.back();
		for (auto entry = top.rbegin(); entry != top.rend(); ++entry) {
			if (entry->name == kLocalOptionsName) restoreLocalOptions(entry->options);
			else env_.restoreVar(entry->name, std::move(entry->saved));
			if (entry->name == "OPTIND") restoreGetoptsPosition(entry->getopts_subindex);
		}

		scope_stack_.pop_back();
	}

	const FunctionDef* Executor::findFunction(const std::string& name) const {
		const auto found = functions_.find(name);
		return found == functions_.end() ? nullptr : found->second;
	}

	bool Executor::unsetFunction(const std::string& name) {
		function_files_.erase(name);
		exported_functions_.erase(name);
		traced_functions_.erase(name);
		return functions_.erase(name) != 0;
	}

	int Executor::registerJob(void* handle, long long pid, std::string cmd_text) {
		Job job;
		job.id = nextJobId();
#ifdef _WIN32
		job.handle = static_cast<HANDLE>(handle);
#else
		job.handle = handle;
#endif
		job.pid = pid;
		job.cmd_text = std::move(cmd_text);
		job.running = true;
		job.with_job_control = env_.options().monitor || env_.interactive();
		touchJob(job);
		jobs_.push_back(job);
		return job.id;
	}

	void Executor::adoptInheritedJob(const Job& job) {
		jobs_.push_back(job);
		jobs_.back().inherited = true;
		touchJob(jobs_.back());
	}

	static const Executor::Job* latestJobExcept(const std::vector<Executor::Job>& jobs,
			const Executor::Job* skipped) {
		const Executor::Job* latest = nullptr;
		for (const Executor::Job& job : jobs) {
			if (&job == skipped) continue;
			if (latest == nullptr || job.recency > latest->recency) latest = &job;
		}

		return latest;
	}

	const Executor::Job* Executor::currentJob() const {
		return latestJobExcept(jobs_, nullptr);
	}

	const Executor::Job* Executor::previousJob() const {
		return latestJobExcept(jobs_, currentJob());
	}

	// As in bash, numbering restarts once the table is empty.
	int Executor::nextJobId() const {
		int highest = 0;
		for (const Job& job : jobs_) {
			if (job.id > highest) highest = job.id;
		}

		return highest + 1;
	}

#ifdef _WIN32
	static void releaseJobExtras(Executor::Job& job) {
		for (const HANDLE stage : job.stages) ::CloseHandle(stage);
		job.stages.clear();
		if (job.group != nullptr) ::CloseHandle(job.group);
		job.group = nullptr;
	}
#endif

	bool Executor::reapJobs() {
		bool changed = false;
#ifdef _WIN32
		for (auto& job : jobs_) {
			if (!job.running || job.handle == nullptr) continue;

			DWORD exit_code = 0;
			if (::GetExitCodeProcess(job.handle, &exit_code) == 0) continue;
			if (exit_code == STILL_ACTIVE) continue;

			job.running = false;
			job.stopped = false;
			job.exit_code = shellStatusFromExitCode(exit_code);
			::CloseHandle(job.handle);
			job.handle = nullptr;
			releaseJobExtras(job);
			noteJobEnded(job.pid);
			changed = true;
		}
#endif
		return changed;
	}

	static void waitJobToFinish(Executor::Job& job) {
#ifdef _WIN32
		if (job.stopped) {
			job.exit_code = kStatusStopped;
			return;
		}

		for (const HANDLE stage : job.stages) ::WaitForSingleObject(stage, INFINITE);
		job.exit_code = waitProcessAndClose(job.handle);
		job.running = false;
		job.handle = nullptr;
		releaseJobExtras(job);
#else
		(void)job;
#endif
	}

#ifdef _WIN32
	// The signal event stays set while a signal is pending, so one that is
	// to be ignored is dropped before the wait goes on.
	static bool waitUnlessSignalled(Executor& exec, HANDLE handle) {
		const HANDLE signal_event = static_cast<HANDLE>(shellSignalEvent());
		if (signal_event == nullptr) {
			return ::WaitForSingleObject(handle, INFINITE) == WAIT_OBJECT_0;
		}

		for (;;) {
			const HANDLE waits[2] = { handle, signal_event };
			const DWORD woke = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
			if (woke != WAIT_OBJECT_0 + 1) return true;
			if (exec.interruptingSignal() != 0) return false;
		}
	}

	static bool awaitJobUnlessSignalled(Executor& exec, const Executor::Job& job) {
		for (const HANDLE stage : job.stages) {
			if (!waitUnlessSignalled(exec, stage)) return false;
		}

		return job.handle == nullptr || waitUnlessSignalled(exec, job.handle);
	}
#else
	static bool awaitJobUnlessSignalled(Executor&, const Executor::Job&) {
		return true;
	}
#endif

	int Executor::waitForJob(int id, bool interruptible) {
		for (auto& job : jobs_) {
			if (job.id != id) continue;
			if (job.stopped) return job.exit_code;
			if (job.running && interruptible && !awaitJobUnlessSignalled(*this, job)) {
				return kSignalStatusBase + interruptingSignal();
			}

			if (job.running) {
				waitJobToFinish(job);
				noteJobEnded(job.pid);
			}

			last_waited_pid_ = job.pid;

			const int status = job.exit_code;
			forgetJob(id);
			return status;
		}

		return -1;
	}

	int Executor::waitForAllJobs(bool interruptible) {
		std::vector<int> ids;
		for (const Job& job : jobs_) {
			if (!job.stopped) ids.push_back(job.id);
		}

		for (const int id : ids) {
			const int status = waitForJob(id, interruptible);
			const bool interrupted = interruptible && status > kSignalStatusBase;
			if (interrupted && interruptingSignal() != 0) return status;
		}

		return 0;
	}

	static bool isAmong(const std::vector<int>& only, int id) {
		return only.empty() || std::find(only.begin(), only.end(), id) != only.end();
	}

	int Executor::waitForAnyJob(const std::vector<int>& only) {
		reapJobs();
		for (const Job& job : jobs_) {
			if (!job.running && isAmong(only, job.id)) return waitForJob(job.id);
		}

		return waitForFirstRunningJob(only);
	}

	int Executor::waitForFirstRunningJob(const std::vector<int>& only) {
#ifdef _WIN32
		std::vector<HANDLE> handles;
		std::vector<int> ids;
		for (const Job& job : jobs_) {
			if (!job.running || job.stopped || job.handle == nullptr) continue;
			if (!isAmong(only, job.id)) continue;

			handles.push_back(job.handle);
			ids.push_back(job.id);
		}

		if (handles.empty() || handles.size() >= MAXIMUM_WAIT_OBJECTS) return -1;

		const DWORD count = static_cast<DWORD>(handles.size());
		const HANDLE signal_event = static_cast<HANDLE>(shellSignalEvent());
		if (signal_event != nullptr) handles.push_back(signal_event);

		for (;;) {
			const DWORD which = ::WaitForMultipleObjects(static_cast<DWORD>(handles.size()),
				handles.data(), FALSE, INFINITE);
			if (which < WAIT_OBJECT_0 + count) return waitForJob(ids[which - WAIT_OBJECT_0]);
			if (which != WAIT_OBJECT_0 + count) return -1;

			const int signum = interruptingSignal();
			if (signum != 0) return kSignalStatusBase + signum;
		}
#else
		return -1;
#endif
	}

	void Executor::forgetJob(int id) {
		for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
			if (it->id != id) continue;

			if (!it->running) finished_job_statuses_[it->pid] = it->exit_code;
			jobs_.erase(it);
			return;
		}
	}

	bool Executor::finishedJobStatus(long long pid, int& status) const {
		const auto found = finished_job_statuses_.find(pid);
		if (found == finished_job_statuses_.end()) return false;

		status = found->second;
		return true;
	}

	static bool jobCommandMatches(const Executor::Job& job, const std::string& pattern) {
		if (!pattern.empty() && pattern[0] == '?') {
			return job.cmd_text.find(pattern.substr(1)) != std::string::npos;
		}

		return job.cmd_text.compare(0, pattern.size(), pattern) == 0;
	}

	Executor::Job* Executor::findJob(const std::string& spec) {
		if (spec.empty() || spec[0] != '%' || jobs_.empty()) return nullptr;

		const std::string rest = spec.substr(1);
		const Job* current = currentJob();
		const Job* previous = previousJob();
		if (rest.empty() || rest == "%" || rest == "+") return const_cast<Job*>(current);
		if (rest == "-") return const_cast<Job*>(previous != nullptr ? previous : current);

		int id = 0;
		for (Job& job : jobs_) {
			if (parseInt(rest, id) ? job.id == id : jobCommandMatches(job, rest)) return &job;
		}

		return nullptr;
	}

	static const std::size_t kJobStateWidth = 27;

	static std::string jobStateText(const Executor::Job& job) {
		if (job.stopped) return "Stopped";
		if (job.running) return "Running";
		if (job.exit_code == 0) return "Done";
		return "Exit " + std::to_string(job.exit_code);
	}

	// Laid out as bash 5.3 does: `[1]+  Running                    sleep 9 &`.
	std::string Executor::jobLine(const Job& job) const {
		char mark = ' ';
		if (&job == currentJob()) mark = '+';
		else if (&job == previousJob()) mark = '-';

		std::string state = jobStateText(job);
		state.resize(std::max<std::size_t>(state.size(), kJobStateWidth), ' ');

		std::string line = "[" + std::to_string(job.id) + "]" + mark + "  " + state + job.cmd_text;
		if (job.running && !job.stopped) line += " &";
		return line;
	}

	void Executor::reportFinishedJobs(std::FILE* out) {
		reapJobs();
		std::vector<int> finished;
		for (const Job& job : jobs_) {
			if (job.running) continue;

			std::fprintf(out, "%s\n", jobLine(job).c_str());
			finished.push_back(job.id);
		}

		for (const int id : finished) forgetJob(id);
	}

#ifndef _WIN32
	bool Executor::waitForProcSubstChild(long long, int&) {
		return false;
	}

	void Executor::waitForProcSubstChildren() {
	}

	std::string Executor::startProcessSubstitution(const std::string&, char) {
		return {};
	}
#endif

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
		runExitTrapText(action);

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
		list.times.erase(list.times.begin() + from, list.times.begin() + to);
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
		history_.times.push_back(static_cast<long long>(std::time(nullptr)));
		trimHistoryTo(history_, history_saved_, historyLimit(env_.get("HISTSIZE")));
	}

	void Executor::replaceLastHistoryEntry(std::string line) {
		if (history_.commands.empty() || line.empty()) return;

		history_.commands.back() = std::move(line);
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
		history_.times.clear();
		history_saved_ = 0;
	}

	bool Executor::loadHistoryFromFile(const std::string& path) {
		HistoryList loaded;
		if (!readHistoryFile(path, loaded)) return false;

		history_.commands.insert(history_.commands.end(), loaded.commands.begin(),
			loaded.commands.end());
		history_.statuses.insert(history_.statuses.end(), loaded.statuses.begin(),
			loaded.statuses.end());
		history_.times.insert(history_.times.end(), loaded.times.begin(), loaded.times.end());
		history_saved_ = history_.commands.size();
		trimHistoryTo(history_, history_saved_, historyLimit(env_.get("HISTSIZE")));
		return true;
	}

	// As bash: entry times go to the file while HISTTIMEFORMAT is set.
	bool Executor::writesHistoryTimes() const {
		return env_.has("HISTTIMEFORMAT");
	}

	bool Executor::saveHistoryToFile(const std::string& path) const {
		return writeHistoryFile(path, history_, 0, writesHistoryTimes());
	}

	bool Executor::writeHistory(const std::string& path) {
		if (!saveHistoryToFile(path)) return false;

		history_saved_ = history_.commands.size();
		return true;
	}

	bool Executor::appendNewHistory(const std::string& path) {
		if (!appendHistoryFile(path, history_, history_saved_, writesHistoryTimes())) return false;

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
			out += "alias -- ";
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
		// Oldest first, so the child rebuilds each hash chain in the same order.
		for (auto it = array.rbegin(); it != array.rend(); ++it) {
			const auto& element = *it;
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
		if ((attrs & kAttrCapitalize) != 0) letters += 'c';
		if (env.isReadonly(name))          letters += 'r';
		return letters;
	}

	// A self-spawned subshell sets these readonly variables up itself.
	static bool isSeededBySubshell(const std::string& name) {
		return name == "BASH_VERSINFO" || name == "PPID" || name == "UID" || name == "EUID";
	}

	std::string Executor::serializeAttributes() const {
		std::set<std::string> names(env_.readonlySet().begin(), env_.readonlySet().end());
		for (const auto& entry : env_.attributeMap()) names.insert(entry.first);

		std::string out;
		for (const std::string& name : names) {
			if (isSeededBySubshell(name)) continue;

			const std::string letters = attributeOptionLetters(env_, name);
			if (!letters.empty()) out += "declare -" + letters + " " + name + "\n";
		}

		for (const auto& entry : sortedKeys(env_.namerefs())) {
			const std::string target = env_.namerefTarget(entry);
			const std::string value = target.empty() ? "" : "=" + shellQuoteSingle(target);
			out += "declare -n " + entry + value + "\n";
		}

		return out;
	}

	std::string Executor::serializeArrays() const {
		std::string out;
		for (const auto& name : sortedKeys(env_.indexedArrays())) {
			if (isSeededBySubshell(name)) continue;

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
			out += "function ";
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

	// bash's dynamic scoping: unsetting a variable that a calling function
	// made local removes that local and shows the value it hid (the
	// "upvar" idiom of bash-completion). A local of the current function
	// is just unset.
	bool Executor::unsetCallerLocal(const std::string& name) {
		if (scope_stack_.size() < 2 || scopeHasName(scope_stack_.back(), name)) return false;

		for (auto scope = scope_stack_.rbegin() + 1; scope != scope_stack_.rend(); ++scope) {
			for (auto entry = scope->begin(); entry != scope->end(); ++entry) {
				if (entry->name != name) continue;

				env_.restoreVar(name, std::move(entry->saved));
				scope->erase(entry);
				return true;
			}
		}

		return false;
	}

	bool Executor::makeLocal(const std::string& name, bool inherit) {
		if (scope_stack_.empty()) return false;

		std::vector<ScopeEntry>& top = scope_stack_.back();
		if (scopeHasName(top, name)) return true;

		ScopeEntry entry;
		entry.name = name;
		if (name == kLocalOptionsName) {
			entry.options = env_.options();
			top.push_back(std::move(entry));
			return true;
		}

		entry.saved = env_.saveVar(name);
		entry.getopts_subindex = getopts_subindex_;
		if (name == "OPTIND") resetGetopts();
		const bool exported = entry.saved.exported;
		top.push_back(std::move(entry));

		// bash's local of a name in the temporary environment keeps its value,
		// and under localvar_inherit any local starts as what it hides, but
		// for the nameref attribute. Otherwise it is declared with no value.
		if (inherit || localInherits(name)) {
			Environment::VarState inherited = top.back().saved;
			inherited.has_nameref = false;
			env_.restoreVar(name, std::move(inherited));
		} else {
			env_.restoreVar(name, Environment::VarState{});
			env_.markValueless(name);
		}

		if (exported) env_.exportVar(name);
		return true;
	}

	bool Executor::isCallerLocal(const std::string& name) const {
		if (scope_stack_.size() < 2) return false;

		for (auto scope = scope_stack_.rbegin() + 1; scope != scope_stack_.rend(); ++scope) {
			if (scopeHasName(*scope, name)) return true;
		}

		return false;
	}

	bool Executor::isLocalHere(const std::string& name) const {
		return !scope_stack_.empty() && scopeHasName(scope_stack_.back(), name);
	}

	bool Executor::hasLocalOptions() const {
		return !scope_stack_.empty() && scopeHasName(scope_stack_.back(), kLocalOptionsName);
	}

	bool Executor::localInherits(const std::string& name) const {
		return env_.shopts().localvar_inherit || isTempEnvName(name);
	}

	bool Executor::isTempEnvName(const std::string& name) const {
		return std::find(temp_env_names_.begin(), temp_env_names_.end(), name)
			!= temp_env_names_.end();
	}

	// POSIX: `var=20 return` in a function made `var=30 f`'s var the shell's.
	std::set<std::string> Executor::takePersistedTempNames() {
		return std::exchange(persisted_temp_names_, {});
	}

	std::size_t Executor::pushTempEnvNames(
			const std::vector<std::pair<std::string, std::string>>& assigns) {
		const std::size_t outer = temp_env_names_.size();
		for (const auto& assign : assigns) temp_env_names_.push_back(assign.first);
		return outer;
	}

	// Each local, innermost first, gives way to the value it hid; after the
	// body, outermost first, what the body left becomes the hidden value.
	int Executor::runInGlobalScope(const std::function<int()>& body) {
		std::vector<Environment::VarState> locals;
		for (auto scope = scope_stack_.rbegin(); scope != scope_stack_.rend(); ++scope) {
			for (auto entry = scope->rbegin(); entry != scope->rend(); ++entry) {
				if (entry->name == kLocalOptionsName) continue;

				locals.push_back(env_.saveVar(entry->name));
				env_.restoreVar(entry->name, entry->saved);
			}
		}

		const int status = body();
		for (auto& scope : scope_stack_) {
			for (auto& entry : scope) {
				if (entry.name == kLocalOptionsName) continue;

				entry.saved = env_.saveVar(entry.name);
				env_.restoreVar(entry.name, std::move(locals.back()));
				locals.pop_back();
			}
		}

		return status;
	}

	std::vector<std::string> Executor::localNames() const {
		std::vector<std::string> names;
		if (scope_stack_.empty()) return names;

		for (const ScopeEntry& entry : scope_stack_.back()) {
			if (entry.name != kLocalOptionsName) names.push_back(entry.name);
		}

		return names;
	}

	bool Executor::assignCompound(const std::string& text) {
		std::function<void()> apply;
		if (!expandCompound(text, apply)) return false;

		apply();
		return true;
	}

	bool Executor::expandCompound(const std::string& text, std::function<void()>& apply) {
		Lexer lexer(text);
		std::vector<Token> tokens = lexer.tokenize();
		Parser parser(std::move(tokens));
		Assignment assignment;
		if (!lexer.errors().empty() || !parser.parseLoneArrayAssignment(assignment)) return false;

		ArrayAssign array = expandArrayAssign(expander_, assignment);
		if (expander_.failed()) {
			reportExpanderError(expander_);
			return false;
		}

		apply = [this, array = std::move(array)]() { applyArrayAssignToEnv(env_, expander_, array); };
		return true;
	}

	static std::string syntaxInputName(const std::string& origin) {
		if (origin == "<command-substitution>") return "command substitution";
		return origin;
	}

	static std::string formatSourceError(const std::string& origin, const std::string& source_text,
			std::size_t first_line, const Lexer& lexer, const Parser& parser) {
		ParseError error;
		if (!firstSyntaxError(lexer.errors(), parser.errors(), error)) return std::string();

		const std::string input = syntaxInputName(origin);
		std::string report;
		for (const LexError& warning : lexer.warnings()) {
			if (warning.loc.line > error.loc.line) continue;
			report += syntaxWarningReport(input, warning.loc.line, warning.message);
		}

		report += syntaxErrorReport(input, source_text, first_line, error.loc.line, error.message);

		ParseError follow_up;
		if (!followUpSyntaxError(parser.errors(), error, follow_up)) return report;

		return report + syntaxErrorReport(input, source_text, first_line, follow_up.loc.line,
			follow_up.message);
	}

	// Only the commands on the lines before a syntax error are kept; the
	// diagnostics are printed once execution reaches the error.
	Executor::ParsedText Executor::parseText(const std::string& source_text,
			const std::string& origin, std::size_t first_line) {
		Lexer lexer(source_text);
		lexer.startAtLine(first_line);
		lexer.lexInPosixMode(env_.options().posix);
		lexer.acceptHeredocAtEof();
		if (origin == "<command-substitution>") lexer.lexSubstitutionBody();
		lexWithAliases(lexer);
		std::vector<Token> tokens = lexer.tokenize();
		Parser parser(std::move(tokens), lexer.source());

		ParsedText parsed;
		parsed.lexed_source = lexer.source();
		parsed.splices = lexer.aliasSplices();
		parsed.root = parser.parseProgram();
		parsed.diagnostics = formatSourceError(origin, source_text, first_line, lexer, parser);
		if (parsed.root == nullptr) return parsed;

		truncateAtSyntaxError(*parsed.root, parser,
			firstSyntaxErrorOffset(lexer.errors(), parser.errors()));
		if (!env_.options().posix) {
			const std::size_t resume = discardResumeOffset(lexer.source(), lexer.errors(),
				parser.errors());
			parsed.resume_offset = resume == std::string::npos ? resume
				: offsetBeforeAliases(parsed.splices, resume);
		}

		parsed.arena = parser.takeArena();
		return parsed;
	}

	void Executor::keepArenaIfDefined(ParsedText& parsed, std::size_t definitions_before) {
		if (function_definitions_ == definitions_before) return;

		owned_arenas_.push_back(std::move(parsed.arena));
	}

	// A Discard ends the text it came from (a trap action, `$(...)`).
	int Executor::finishParsedText(const ParsedText& parsed, int status) {
		if (consumeFlow(FlowSignal::Kind::Discard) && status == 0) status = 1;
		if (parsed.diagnostics.empty() || flowPending()) return status;

		std::fputs(parsed.diagnostics.c_str(), stderr);
		special_builtin_failed_ = true;
		return kSyntaxErrorStatus;
	}

	int Executor::executeText(const std::string& source_text, const std::string& origin,
			std::size_t first_line) {
		ParsedText parsed = parseText(source_text, origin, first_line);
		const std::size_t defined = function_definitions_;
		const int status = parsed.root != nullptr ? execute(*parsed.root) : 0;
		keepArenaIfDefined(parsed, defined);
		return finishParsedText(parsed, status);
	}

	int Executor::executeInputText(const std::string& source_text, const std::string& origin,
			std::size_t first_line) {
		ParsedText parsed = parseText(source_text, origin, first_line);
		const std::size_t defined = function_definitions_;
		const int status = parsed.root != nullptr
			? executeInput(*parsed.root, parsed.lexed_source)
			: 0;
		keepArenaIfDefined(parsed, defined);
		if (relex_offset_ != std::string::npos) {
			return relexRestOfInput(source_text,
				{ origin, first_line, parsed.lexed_source, parsed.splices }, status);
		}

		if (parsed.resume_offset == std::string::npos || flowPending()) {
			return finishParsedText(parsed, status);
		}

		return resumeAfterDiscardedCommand(parsed, source_text, origin, first_line);
	}

	// bash reports the error, fails the command it discarded and reads on.
	int Executor::resumeAfterDiscardedCommand(const ParsedText& parsed,
			const std::string& source_text, const std::string& origin, std::size_t first_line) {
		std::fputs(parsed.diagnostics.c_str(), stderr);
		setLastStatus(1);

		const std::size_t resume = parsed.resume_offset;
		if (resume >= source_text.size()) return 1;

		const auto skipped_lines = std::count(source_text.begin(),
			source_text.begin() + static_cast<std::ptrdiff_t>(resume), '\n');
		const std::size_t line = first_line + static_cast<std::size_t>(skipped_lines);
		return executeInputText(source_text.substr(resume), origin, line);
	}

	// A line can end inside an alias's text: bash has that text as input
	// already and reads the rest of it before going on.
	static std::string aliasTextLeft(const Executor::RelexInput& input, std::size_t relex) {
		const std::size_t original = offsetBeforeAliases(input.splices, relex);
		const std::size_t resume = offsetAfterAliases(input.splices, original);
		if (resume <= relex) return std::string();

		return input.lexed_source.substr(relex, resume - relex);
	}

	// @p relex_offset_ points into the text as lexed; the rest of the input
	// is the rest of @p source_text from where that was.
	int Executor::relexRestOfInput(const std::string& source_text, const RelexInput& input,
			int status) {
		const std::size_t relex = std::exchange(relex_offset_, std::string::npos);
		if (relex == std::string::npos || flowPending()) return status;

		const std::string alias_rest = aliasTextLeft(input, relex);
		const std::size_t resume = offsetBeforeAliases(input.splices, relex);
		if (resume >= source_text.size() && alias_rest.empty()) return status;

		const auto skipped_lines = std::count(source_text.begin(),
			source_text.begin() + static_cast<std::ptrdiff_t>((std::min)(resume, source_text.size())),
			'\n');
		const auto alias_lines = std::count(alias_rest.begin(), alias_rest.end(), '\n');
		const std::size_t line = input.first_line + static_cast<std::size_t>(skipped_lines)
			- (std::min)(static_cast<std::size_t>(alias_lines), input.first_line - 1);
		const std::string rest = resume < source_text.size() ? source_text.substr(resume) : "";
		return executeInputText(alias_rest + rest, input.origin, line);
	}

	// What lexing depends on of the aliases: none while they do not expand.
	unsigned long long Executor::aliasState() const {
		return env_.expand_aliases() ? alias_generation_ + 1 : 0;
	}

	int Executor::executeInput(const Node& root, const std::string& source_text) {
		const InputEcho saved = input_echo_;
		const bool is_list = root.kind == Node::Kind::List;
		input_echo_ = { is_list ? static_cast<const List*>(&root) : nullptr, &source_text, 0,
			env_.options().posix, aliasState() };
		if (!is_list) echoInputUpTo(source_text.size());
		if (!is_list) input_command_end_line_ = lastLineOf(root);

		int status = execute(root);
		if (consumeFlow(FlowSignal::Kind::Discard) && status == 0) status = 1;
		if (!flowPending()) echoInputUpTo(source_text.size());
		if (!is_list && !flowPending()) relexAfterSoleCommand(root, source_text);

		input_echo_ = saved;
		return status;
	}

	// Input cut short by a syntax error can be a single command; the lines
	// after it are lexed again as after an item of a list.
	void Executor::relexAfterSoleCommand(const Node& command, const std::string& source_text) {
		const bool posix_changed = env_.options().posix != input_echo_.lexed_posix;
		if (!posix_changed && aliasState() == input_echo_.lexed_aliases) return;

		const std::size_t line_end = commandLineEnd(command, source_text);
		if (line_end < source_text.size()) relex_offset_ = line_end;
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

	static std::size_t commandLineEnd(const Node& command, const std::string& text) {
		const std::size_t last_byte = (command.src_end > command.loc.offset)
			? command.src_end
			: command.loc.offset;
		return lineEndAfter(text, last_byte);
	}

	// The input bash has read by the time it runs item `index`: through the
	// end of the item's last line, heredoc bodies included.
	static std::size_t inputReadThrough(const List& list, std::size_t index,
			const std::string& text) {
		const std::size_t command_end = commandLineEnd(*list.items[index].command, text);

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
		return finishSubstitution(std::move(out), status);
	}

	std::string Executor::runInCurrentShell(const std::string& body) {
		int status = 0;
		std::string out = captureSubstitution(body, status, /*in_current_shell=*/true);
		return finishSubstitution(std::move(out), status);
	}

	std::string Executor::finishSubstitution(std::string out, int status) {
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

	// bash clears errexit in `$(...)` unless inherit_errexit or posix mode
	// keeps it.
	void Executor::dropErrexitForSubstitution() {
		if (env_.shopts().inherit_errexit || env_.options().posix) return;

		env_.setErrexit(false);
	}

	int Executor::runCommandSubstitutionBody(const std::string& body) {
		SubshellScope snap = enterSubshell();
		dropErrexitForSubstitution();

		const std::size_t line = static_cast<std::size_t>(parseBaseLine());
		const int status = endSubshell(executeSubstitutionText(body, line));
		leaveSubshell(snap);
		return status;
	}

	// The lone `< file` of `$(< file)`, which bash reads instead of running.
	static const Redirection* fileReadRedirection(const Node& root) {
		if (root.kind != Node::Kind::List) return nullptr;

		const List& list = static_cast<const List&>(root);
		if (list.items.size() != 1 || list.items[0].background) return nullptr;
		if (list.items[0].command->kind != Node::Kind::SimpleCommand) return nullptr;

		const SimpleCommand& sc = static_cast<const SimpleCommand&>(*list.items[0].command);
		if (!sc.words.empty() || !sc.assignments.empty() || sc.redirs.size() != 1) return nullptr;

		const Redirection& redir = sc.redirs[0];
		const bool reads_stdin = redir.fd == -1 || redir.fd == 0;
		if (redir.op != RedirOp::Less || !reads_stdin || !redir.fd_variable.empty()) return nullptr;
		return &redir;
	}

	static void copyFdToStdout(int fd) {
		char buffer[65536];
		int got = 0;
		while ((got = _read(fd, buffer, sizeof buffer)) > 0) {
			if (_write(1, buffer, static_cast<unsigned int>(got)) != got) return;
		}
	}

	int Executor::writeFileToStdout(const Redirection& redir) {
		std::string path;
		if (!expandRedirTarget(redir.target, path)) return 1;

		const int fd = openRedirSourceFd(path, _O_RDONLY);
		if (fd < 0) {
			reportPathError(path);
			return 1;
		}

		std::fflush(stdout);
		copyFdToStdout(fd);
		_close(fd);
		return 0;
	}

	int Executor::executeSubstitutionText(const std::string& body, std::size_t first_line) {
		ParsedText parsed = parseText(body, "<command-substitution>", first_line);
		if (parsed.root == nullptr) return finishParsedText(parsed, 0);

		const std::size_t defined = function_definitions_;
		const Redirection* file_read = parsed.diagnostics.empty()
			? fileReadRedirection(*parsed.root)
			: nullptr;
		const NodeLines lines = reprintedSubstitutionLines(*parsed.root, body);
		const int base = substitutionBaseLine();
		for (const auto& entry : lines) {
			line_overrides_[entry.first] = base + static_cast<int>(entry.second);
		}

		enterTraceLevel();
		const int status = file_read != nullptr
			? writeFileToStdout(*file_read)
			: execute(*parsed.root);
		leaveTraceLevel();
		for (const auto& entry : lines) line_overrides_.erase(entry.first);
		keepArenaIfDefined(parsed, defined);
		return finishParsedText(parsed, status);
	}

	// bash runs `${ body; }` like an unnamed function that shares the
	// caller's positional parameters: `local` and `return` work in it.
	int Executor::runSubstitutionInCurrentShell(const std::string& body) {
		const std::size_t line = static_cast<std::size_t>(parseBaseLine());

		const bool had_errexit = env_.errexit();
		dropErrexitForSubstitution();
		const bool dropped_errexit = had_errexit && !env_.errexit();

		++func_depth_;
		scope_stack_.emplace_back();
		int status = executeSubstitutionText(body, line);
		consumeFlow(FlowSignal::Kind::Return, &status);

		popLocalScope();
		--func_depth_;
		if (dropped_errexit) env_.setErrexit(true);
		return status;
	}

	int Executor::runSubstitutionBody(const std::string& body, bool in_current_shell) {
		if (in_current_shell) return runSubstitutionInCurrentShell(body);

		return runCommandSubstitutionBody(body);
	}

	std::string Executor::runForReply(const std::string& body) {
		const bool had_reply = env_.has("REPLY");
		const std::string saved_reply = env_.get("REPLY");
		env_.unset("REPLY");

		const int status = runSubstitutionInCurrentShell(body);
		const std::string value = env_.get("REPLY");
		if (had_reply) env_.set("REPLY", saved_reply);
		else            env_.unset("REPLY");

		finishSubstitution({}, status);
		return value;
	}

	std::string Executor::runRaw(const std::string& body) {
		int status = 0;
		return captureSubstitution(body, status);
	}

	std::string Executor::captureSubstitution(const std::string& body, int& status,
			bool in_current_shell) {
		status = 1;
		void* const capture_file = takeCaptureFile();
		if (capture_file == nullptr) return {};

		const int capture_fd = openCaptureFd(capture_file);
		if (capture_fd < 0) {
			releaseCaptureFile(capture_file);
			return {};
		}

		std::fflush(stdout);
		const int saved_stdout = dupAboveUserFds(1);
		const std::size_t hidden = hidden_fds_.size();
		if (!in_current_shell) hidden_fds_.push_back(saved_stdout);
		_dup2(capture_fd, 1);
		_close(capture_fd);
		status = runSubstitutionBody(body, in_current_shell);
		std::fflush(stdout);
		showHiddenFds(hidden);
		_dup2(saved_stdout, 1);
		_close(saved_stdout);

		std::string out;
		if (!flowPending()) out = readCaptureFile(capture_file);
		releaseCaptureFile(capture_file);
		return out;
	}

}  // namespace wbsh

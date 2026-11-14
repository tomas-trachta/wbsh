/**
 * @file coreutils_gitbash.cpp
 * @brief cygpath, start and winpty: the helpers Git Bash ships, which
 *        scripts written for Git Bash call.
 *
 * cygpath converts with wbsh's own mount table (pathconv.h), so `/c/x`,
 * `/tmp` and `/` mean here what they mean everywhere else in wbsh.
 */

#include "coreutils_internal.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <userenv.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "executor.h"
#include "exitstatus.h"
#include "optscan.h"
#include "pathconv.h"

#pragma comment(lib, "userenv.lib")

namespace wbsh {

	namespace fs = std::filesystem;

	namespace gitbash_detail {

		enum class PathForm { Unix, Windows, Mixed };

		enum class SpecialFolder { None, Desktop, HomeRoot, MyDocs, Programs, SysDir, WinDir, Id };

		enum CygpathOption {
			kCygUnix = 1, kCygWindows, kCygMixed, kCygDos, kCygType, kCygAbsolute, kCygLong,
			kCygShort, kCygPath, kCygFile, kCygNull, kCygAllUsers, kCygDesktop, kCygHomeRoot,
			kCygMyDocs, kCygPrograms, kCygSysDir, kCygWinDir, kCygFolder, kCygCodepage,
			kCygProcDrive, kCygHelp, kCygVersion
		};

		struct CygpathOptions {
			PathForm form = PathForm::Unix;
			bool absolute = false;
			bool long_name = false;
			bool short_name = false;
			bool path_list = false;
			bool null_terminated = false;
			bool all_users = false;
			bool help = false;
			bool version = false;
			SpecialFolder folder = SpecialFolder::None;
			std::string folder_id;
			std::vector<std::string> files;
			std::vector<std::string> names;
		};

		struct StartRequest {
			bool help = false;
			bool version = false;
			bool wait = false;
			int show = SW_SHOWNORMAL;
			std::vector<std::string> command;
		};

	}  // namespace gitbash_detail

	using gitbash_detail::CygpathOptions;
	using gitbash_detail::PathForm;
	using gitbash_detail::SpecialFolder;
	using gitbash_detail::StartRequest;

	static const int kStatusShortNameFailed = 2;

	static const char* const kCygpathUsage =
		"Usage: cygpath (-d|-m|-u|-w|-t TYPE) [-f FILE] [OPTION]... NAME...\n"
		"       cygpath [-ADHOPSW]\n"
		"       cygpath [-F ID]\n"
		"\n"
		"Convert Unix and Windows format paths, or output system path information\n"
		"\n"
		"Output type options:\n"
		"\n"
		"  -d, --dos             print DOS (short) form of NAMEs (C:\\PROGRA~1\\)\n"
		"  -m, --mixed           like --windows, but with regular slashes (C:/WINNT)\n"
		"  -u, --unix            (default) print Unix form of NAMEs (/c/winnt)\n"
		"  -w, --windows         print Windows form of NAMEs (C:\\WINNT)\n"
		"  -t, --type TYPE       print TYPE form: 'dos', 'mixed', 'unix', or 'windows'\n"
		"\n"
		"Path conversion options:\n"
		"\n"
		"  -a, --absolute        output absolute path\n"
		"  -l, --long-name       print Windows long form of NAMEs (with -w, -m only)\n"
		"  -p, --path            NAME is a PATH list (i.e., '/bin:/usr/bin')\n"
		"  -s, --short-name      print DOS (short) form of NAMEs (with -w, -m only)\n"
		"  -f, --file FILE       read FILE for input; use - to read from STDIN\n"
		"  -0, -z, --null        end each output name with NUL instead of newline\n"
		"  -C, --codepage CP     accepted and ignored (wbsh paths are UTF-8)\n"
		"  -U, --proc-cygdrive   accepted and ignored\n"
		"\n"
		"System information:\n"
		"\n"
		"  -A, --allusers        use `All Users' instead of current user for -D, -O, -P\n"
		"  -D, --desktop         output `Desktop' directory and exit\n"
		"  -H, --homeroot        output `Profiles' directory (home root) and exit\n"
		"  -O, --mydocs          output `My Documents' directory and exit\n"
		"  -P, --smprograms      output Start Menu `Programs' directory and exit\n"
		"  -S, --sysdir          output system directory and exit\n"
		"  -W, --windir          output `Windows' directory and exit\n"
		"  -F, --folder ID       output special folder with numeric ID and exit\n"
		"\n"
		"Other options:\n"
		"\n"
		"  -h, --help            output usage information and exit\n"
		"  -V, --version         output version information and exit\n"
		"\n"
		"Conversion follows wbsh's own mounts: /c/x is C:\\x, /tmp is %TEMP%.\n";

	static const char* const kCygpathShortUsage =
		"Usage: cygpath (-d|-m|-u|-w|-t TYPE) [-f FILE] [OPTION]... NAME...\n"
		"       cygpath [-ADHOPSW]\n"
		"       cygpath [-F ID]\n"
		"Try 'cygpath --help' for more information.\n";

	static const char* const kStartUsage =
		"Usage: start [/wait] [/min | /max] FILE|URL|PROGRAM [ARG]...\n"
		"Open FILE, a folder or URL with the program Windows associates with it,\n"
		"or start PROGRAM with ARGs, without waiting for it.\n"
		"\n"
		"  -w, --wait, /wait   wait for the program and return its exit status\n"
		"  /min, /max          start the window minimized or maximized\n"
		"  -h, --help          display this help and exit\n"
		"      --version       output version information and exit\n";

	static const char* const kWinptyUsage =
		"Usage: winpty [OPTIONS] PROGRAM [ARG]...\n"
		"Run PROGRAM with ARGs.\n"
		"\n"
		"wbsh runs programs on a real console or pseudo console already, so winpty\n"
		"only passes the command through: a builtin, function or program on PATH,\n"
		"with its exit status. winpty's own options (-Xallow-non-tty, -Xplain,\n"
		"-Xcolor, --mouse, --showkey) are accepted and ignored.\n";

	static int showText(const char* text) {
		std::fputs(text, stdout);
		std::fflush(stdout);
		return 0;
	}

	static void printVersionLine(const char* cmd) {
		std::printf("%s (wbsh) %d.%d.%d\n", cmd, WBSH_VERSION_MAJOR, WBSH_VERSION_MINOR,
			WBSH_VERSION_PATCH);
	}

	static int showVersion(const char* cmd, const char* line) {
		printVersionLine(cmd);
		std::fputs(line, stdout);
		std::fflush(stdout);
		return 0;
	}

	static int usageFailure(const char* text) {
		std::fputs(text, stderr);
		return 1;
	}

	static std::string withSlashes(std::string path) {
		for (char& c : path) {
			if (c == '\\') c = '/';
		}

		return path;
	}

	static bool isSeparator(char c) {
		return c == '/' || c == '\\';
	}

	static bool isDriveLetter(char c) {
		return std::isalpha(static_cast<unsigned char>(c)) != 0;
	}

	static bool isDriveOnly(const std::string& name) {
		return name.size() == 2 && isDriveLetter(name[0]) && name[1] == ':';
	}

	static bool isDriveRelative(const std::string& name) {
		return name.size() > 2 && isDriveLetter(name[0]) && name[1] == ':' && !isSeparator(name[2]);
	}

	static std::string fullPathName(const std::string& path) {
		const std::wstring wide = utf8ToWide(path);
		const DWORD needed = ::GetFullPathNameW(wide.c_str(), 0, nullptr, nullptr);
		if (needed == 0) return path;

		std::wstring full(needed, L'\0');
		const DWORD length = ::GetFullPathNameW(wide.c_str(), needed, full.data(), nullptr);
		if (length == 0 || length >= needed) return path;

		full.resize(length);
		return wideToUtf8(full);
	}

	static bool shortPathName(const std::string& path, std::string& short_form) {
		const std::wstring wide = utf8ToWide(path);
		const DWORD needed = ::GetShortPathNameW(wide.c_str(), nullptr, 0);
		if (needed == 0) return false;

		std::wstring buffer(needed, L'\0');
		const DWORD length = ::GetShortPathNameW(wide.c_str(), buffer.data(), needed);
		if (length == 0 || length >= needed) return false;

		buffer.resize(length);
		short_form = wideToUtf8(buffer);
		return true;
	}

	static std::vector<std::string> pathComponents(const std::string& path) {
		std::vector<std::string> parts;
		std::string current;
		for (char c : path) {
			if (!isSeparator(c)) {
				current.push_back(c);
				continue;
			}

			parts.push_back(current);
			current.clear();
		}

		parts.push_back(current);
		return parts;
	}

	static bool startsWithParent(const std::string& path) {
		return pathComponents(path).front() == "..";
	}

	// A relative name keeps its place: "./a//b/" is "a\b\", as cygpath
	// prints it, without consulting the file system.
	static std::string relativeWindowsPath(const std::string& path) {
		const std::vector<std::string> parts = pathComponents(path);
		std::string out;
		for (const std::string& part : parts) {
			if (part.empty() || part == ".") continue;
			if (!out.empty()) out.push_back('\\');
			out += part;
		}

		if (!out.empty() && isSeparator(path.back())) out.push_back('\\');
		return out.empty() ? "." : out;
	}

	static std::string windowsPath(Executor& exec, const CygpathOptions& options,
	                               const std::string& name) {
		const std::string native = exec.pathConv().toWin32(name);
		const bool resolve = options.absolute || PathConv::isWin32Absolute(native)
			|| startsWithParent(native);
		return resolve ? fullPathName(native) : relativeWindowsPath(native);
	}

	static std::string driveRootName(const std::string& drive) {
		const char letter = static_cast<char>(std::tolower(static_cast<unsigned char>(drive[0])));
		return std::string("/") + letter;
	}

	static std::string windowsNameToUnix(Executor& exec, const CygpathOptions& options,
	                                     const std::string& name) {
		if (isDriveRelative(name)) return name;
		if (isDriveOnly(name)) return driveRootName(name);

		const bool resolve = options.absolute || PathConv::isWin32Absolute(name);
		return exec.pathConv().toPosix(resolve ? fullPathName(name) : name);
	}

	// A POSIX name is printed as given, even with -a; only a relative one
	// is resolved against the current directory.
	static std::string unixPath(Executor& exec, const CygpathOptions& options,
	                            const std::string& name) {
		if (PathConv::looksLikeWin32(name)) return windowsNameToUnix(exec, options, name);
		if (!options.absolute || PathConv::isPosixAbsolute(name)) return name;

		return exec.pathConv().toPosix(fullPathName(name));
	}

	// Names converted before the failing one go out first, so the message
	// lands where it happened.
	static void reportCygpathError(const std::string& message) {
		std::fflush(stdout);
		perr("cygpath", message);
	}

	static int reportShortNameFailure(const std::string& path) {
		reportCygpathError("cannot create short name of " + path);
		return kStatusShortNameFailed;
	}

	static int windowsFormOf(Executor& exec, const CygpathOptions& options,
	                         const std::string& name, std::string& out) {
		std::string native = windowsPath(exec, options, name);
		if (options.short_name && !shortPathName(native, native)) {
			return reportShortNameFailure(native);
		}

		if (options.long_name) native = pathToUtf8(expandShortNames(utf8ToPath(native)));
		out = options.form == PathForm::Mixed ? withSlashes(native) : native;
		return 0;
	}

	static int convertSingle(Executor& exec, const CygpathOptions& options,
	                         const std::string& name, std::string& out) {
		if (options.form != PathForm::Unix) return windowsFormOf(exec, options, name, out);

		out = unixPath(exec, options, name);
		return 0;
	}

	static bool isDriveColon(const std::string& list, std::size_t i, const std::string& current) {
		const bool lone_letter = current.size() == 1 && isDriveLetter(current[0]);
		return lone_letter && i + 1 < list.size() && isSeparator(list[i + 1]);
	}

	// A list with a ';' is a Windows list; otherwise ':' separates entries,
	// except right after a lone drive letter (`C:\x:/usr/bin`).
	static std::vector<std::string> splitPathList(const std::string& list) {
		const char separator = list.find(';') != std::string::npos ? ';' : ':';
		std::vector<std::string> entries;
		std::string current;
		for (std::size_t i = 0; i < list.size(); ++i) {
			const char c = list[i];
			if (c != separator || (separator == ':' && isDriveColon(list, i, current))) {
				current.push_back(c);
				continue;
			}

			entries.push_back(current);
			current.clear();
		}

		entries.push_back(current);
		return entries;
	}

	// As cygpath: an empty entry is dropped from a Unix list and becomes
	// "." in a Windows one.
	static int convertList(Executor& exec, const CygpathOptions& options,
	                       const std::string& list, std::string& out) {
		const bool unix_form = options.form == PathForm::Unix;
		out.clear();
		bool first = true;
		for (const std::string& entry : splitPathList(list)) {
			if (entry.empty() && unix_form) continue;

			std::string converted = ".";
			const int status = entry.empty() ? 0 : convertSingle(exec, options, entry, converted);
			if (status != 0) return status;

			if (!first) out.push_back(unix_form ? ':' : ';');
			out += converted;
			first = false;
		}

		return 0;
	}

	static void emitName(const std::string& name, const CygpathOptions& options) {
		std::fwrite(name.data(), 1, name.size(), stdout);
		std::fputc(options.null_terminated ? '\0' : '\n', stdout);
	}

	static int convertNames(Executor& exec, const CygpathOptions& options,
	                        const std::vector<std::string>& names) {
		int status = 0;
		for (const std::string& name : names) {
			if (name.empty()) {
				reportCygpathError("can't convert empty path");
				status = 1;
				break;
			}

			std::string converted;
			const int result = options.path_list ? convertList(exec, options, name, converted)
				: convertSingle(exec, options, name, converted);
			if (result == 0) emitName(converted, options);
			else status = result;
		}

		std::fflush(stdout);
		return status;
	}

	static bool readNameFiles(Executor& exec, const CygpathOptions& options,
	                          std::vector<std::string>& names) {
		for (const std::string& file : options.files) {
			if (readAllLines(exec, file, names)) continue;

			perr("cygpath", file + ": " + std::strerror(errno));
			return false;
		}

		return true;
	}

	static int convertRequestedNames(Executor& exec, const CygpathOptions& options) {
		std::vector<std::string> names;
		if (!readNameFiles(exec, options, names)) return 1;

		names.insert(names.end(), options.names.begin(), options.names.end());
		if (names.empty() && options.files.empty()) return usageFailure(kCygpathShortUsage);
		return convertNames(exec, options, names);
	}

	static std::string inForm(Executor& exec, PathForm form, const std::string& native) {
		if (form == PathForm::Unix) return exec.pathConv().toPosix(native);
		if (form == PathForm::Mixed) return withSlashes(native);
		return native;
	}

	static bool shellFolderPath(int csidl, std::string& native) {
		wchar_t buffer[MAX_PATH];
		const HRESULT result = ::SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT,
			buffer);
		if (FAILED(result)) return false;

		native = wideToUtf8(buffer);
		return true;
	}

	static bool profilesDirectory(std::string& native) {
		DWORD length = 0;
		::GetProfilesDirectoryW(nullptr, &length);
		if (length == 0) return false;

		std::wstring buffer(length, L'\0');
		if (::GetProfilesDirectoryW(buffer.data(), &length) == 0) return false;

		buffer.resize(::wcslen(buffer.c_str()));
		native = wideToUtf8(buffer);
		return true;
	}

	static bool parseFolderId(const std::string& text, int& csidl) {
		if (text.empty()) return false;

		char* end = nullptr;
		const long value = std::strtol(text.c_str(), &end, 0);
		if (*end != '\0' || value < 0 || value > 0xFFFF) return false;

		csidl = static_cast<int>(value);
		return true;
	}

	static int namedFolderCsidl(const CygpathOptions& options) {
		switch (options.folder) {
		case SpecialFolder::Desktop:
			return options.all_users ? CSIDL_COMMON_DESKTOPDIRECTORY : CSIDL_DESKTOPDIRECTORY;
		case SpecialFolder::MyDocs:
			return options.all_users ? CSIDL_COMMON_DOCUMENTS : CSIDL_PERSONAL;
		case SpecialFolder::Programs:
			return options.all_users ? CSIDL_COMMON_PROGRAMS : CSIDL_PROGRAMS;
		case SpecialFolder::SysDir:
			return CSIDL_SYSTEM;
		case SpecialFolder::WinDir:
			return CSIDL_WINDOWS;
		default:
			return -1;
		}
	}

	static int folderFailure() {
		perr("cygpath", "failed to retrieve special folder path");
		return 1;
	}

	static int printSpecialFolder(Executor& exec, const CygpathOptions& options) {
		int csidl = namedFolderCsidl(options);
		if (options.folder == SpecialFolder::Id && !parseFolderId(options.folder_id, csidl)) {
			perr("cygpath", "syntax error in special folder ID " + options.folder_id);
			return 1;
		}

		std::string native;
		const bool found = options.folder == SpecialFolder::HomeRoot ? profilesDirectory(native)
			: shellFolderPath(csidl, native);
		if (!found) return folderFailure();

		emitName(inForm(exec, options.form, native), options);
		std::fflush(stdout);
		return 0;
	}

	static bool applyCygpathType(const std::string& type, CygpathOptions& options) {
		if (type == "unix") options.form = PathForm::Unix;
		else if (type == "windows") options.form = PathForm::Windows;
		else if (type == "mixed") options.form = PathForm::Mixed;
		else if (type == "dos") options.form = PathForm::Windows;
		else return false;

		options.short_name = options.short_name || type == "dos";
		return true;
	}

	static bool applyCygpathForm(const OptHit& hit, CygpathOptions& options) {
		switch (hit.id) {
		case gitbash_detail::kCygUnix:
			options.form = PathForm::Unix;
			return true;
		case gitbash_detail::kCygWindows:
			options.form = PathForm::Windows;
			return true;
		case gitbash_detail::kCygMixed:
			options.form = PathForm::Mixed;
			return true;
		case gitbash_detail::kCygDos:
			return applyCygpathType("dos", options);
		case gitbash_detail::kCygType:
			if (applyCygpathType(hit.value, options)) return true;

			perr("cygpath", "unknown type: " + hit.value);
			return false;
		default:
			return true;
		}
	}

	static void applyCygpathFolder(const OptHit& hit, CygpathOptions& options) {
		static const struct {
			int id;
			SpecialFolder folder;
		} kFolders[] = {
			{ gitbash_detail::kCygDesktop,  SpecialFolder::Desktop },
			{ gitbash_detail::kCygHomeRoot, SpecialFolder::HomeRoot },
			{ gitbash_detail::kCygMyDocs,   SpecialFolder::MyDocs },
			{ gitbash_detail::kCygPrograms, SpecialFolder::Programs },
			{ gitbash_detail::kCygSysDir,   SpecialFolder::SysDir },
			{ gitbash_detail::kCygWinDir,   SpecialFolder::WinDir },
			{ gitbash_detail::kCygFolder,   SpecialFolder::Id },
		};

		for (const auto& entry : kFolders) {
			if (entry.id == hit.id) options.folder = entry.folder;
		}

		if (hit.id == gitbash_detail::kCygFolder) options.folder_id = hit.value;
	}

	static void applyCygpathFlag(const OptHit& hit, CygpathOptions& options) {
		switch (hit.id) {
		case gitbash_detail::kCygAbsolute:
			options.absolute = true;
			break;
		case gitbash_detail::kCygLong:
			options.long_name = true;
			break;
		case gitbash_detail::kCygShort:
			options.short_name = true;
			break;
		case gitbash_detail::kCygPath:
			options.path_list = true;
			break;
		case gitbash_detail::kCygFile:
			options.files.push_back(hit.value);
			break;
		case gitbash_detail::kCygNull:
			options.null_terminated = true;
			break;
		case gitbash_detail::kCygAllUsers:
			options.all_users = true;
			break;
		case gitbash_detail::kCygHelp:
			options.help = true;
			break;
		case gitbash_detail::kCygVersion:
			options.version = true;
			break;
		default:
			break;
		}
	}

	static const std::vector<OptDef>& cygpathOptionDefs() {
		static const std::vector<OptDef> defs = {
			{ gitbash_detail::kCygUnix,      'u', "unix",          OptArg::None },
			{ gitbash_detail::kCygWindows,   'w', "windows",       OptArg::None },
			{ gitbash_detail::kCygMixed,     'm', "mixed",         OptArg::None },
			{ gitbash_detail::kCygDos,       'd', "dos",           OptArg::None },
			{ gitbash_detail::kCygType,      't', "type",          OptArg::Required },
			{ gitbash_detail::kCygAbsolute,  'a', "absolute",      OptArg::None },
			{ gitbash_detail::kCygLong,      'l', "long-name",     OptArg::None },
			{ gitbash_detail::kCygShort,     's', "short-name",    OptArg::None },
			{ gitbash_detail::kCygPath,      'p', "path",          OptArg::None },
			{ gitbash_detail::kCygFile,      'f', "file",          OptArg::Required },
			{ gitbash_detail::kCygNull,      '0', "null",          OptArg::None },
			{ gitbash_detail::kCygNull,      'z', nullptr,         OptArg::None },
			{ gitbash_detail::kCygAllUsers,  'A', "allusers",      OptArg::None },
			{ gitbash_detail::kCygDesktop,   'D', "desktop",       OptArg::None },
			{ gitbash_detail::kCygHomeRoot,  'H', "homeroot",      OptArg::None },
			{ gitbash_detail::kCygMyDocs,    'O', "mydocs",        OptArg::None },
			{ gitbash_detail::kCygPrograms,  'P', "smprograms",    OptArg::None },
			{ gitbash_detail::kCygSysDir,    'S', "sysdir",        OptArg::None },
			{ gitbash_detail::kCygWinDir,    'W', "windir",        OptArg::None },
			{ gitbash_detail::kCygFolder,    'F', "folder",        OptArg::Required },
			{ gitbash_detail::kCygCodepage,  'C', "codepage",      OptArg::Required },
			{ gitbash_detail::kCygProcDrive, 'U', "proc-cygdrive", OptArg::None },
			{ gitbash_detail::kCygHelp,      'h', "help",          OptArg::None },
			{ gitbash_detail::kCygVersion,   'V', "version",       OptArg::None },
		};
		return defs;
	}

	static bool parseCygpathArgs(const std::vector<std::string>& args, CygpathOptions& options) {
		OptScanResult scanned;
		if (!scanOptions("cygpath", args, cygpathOptionDefs(), scanned)) return false;

		for (const OptHit& hit : scanned.hits) {
			if (!applyCygpathForm(hit, options)) return false;

			applyCygpathFolder(hit, options);
			applyCygpathFlag(hit, options);
		}

		options.names = scanned.operands;
		return true;
	}

	static int builtin_cygpath(Executor& exec, const std::vector<std::string>& args) {
		CygpathOptions options;
		if (!parseCygpathArgs(args, options)) {
			return usageFailure("Try 'cygpath --help' for more information.\n");
		}

		if (options.help) return showText(kCygpathUsage);
		if (options.version) return showVersion("cygpath", "Path Conversion Utility\n");
		if (options.folder != SpecialFolder::None) return printSpecialFolder(exec, options);
		return convertRequestedNames(exec, options);
	}

	static bool equalsIgnoringCase(const std::string& text, const char* word) {
		const std::size_t length = std::strlen(word);
		if (text.size() != length) return false;

		for (std::size_t i = 0; i < length; ++i) {
			if (std::tolower(static_cast<unsigned char>(text[i])) != word[i]) return false;
		}

		return true;
	}

	static bool applyStartOption(const std::string& arg, StartRequest& request) {
		if (arg == "-h" || arg == "--help") request.help = true;
		else if (arg == "--version") request.version = true;
		else if (arg == "-w" || arg == "--wait" || equalsIgnoringCase(arg, "/wait")) {
			request.wait = true;
		} else if (equalsIgnoringCase(arg, "/min")) request.show = SW_SHOWMINNOACTIVE;
		else if (equalsIgnoringCase(arg, "/max")) request.show = SW_SHOWMAXIMIZED;
		else return false;

		return true;
	}

	// A leading "" is the window title `cmd /c start` expects; Git Bash's
	// start passes one itself, so a script that adds its own is not wrong.
	static void parseStartArgs(const std::vector<std::string>& args, StartRequest& request) {
		std::size_t i = 0;
		while (i < args.size() && applyStartOption(args[i], request)) ++i;
		if (i < args.size() && args[i] == "--") ++i;
		if (i + 1 < args.size() && args[i].empty()) ++i;

		request.command.assign(args.begin() + static_cast<std::ptrdiff_t>(i), args.end());
	}

	static bool isUrl(const std::string& target) {
		return target.find("://") != std::string::npos || target.rfind("mailto:", 0) == 0;
	}

	// An existing file or folder goes to the shell as an absolute Win32 path;
	// anything else (a program name, a URL) as written.
	static std::wstring startTarget(Executor& exec, const std::string& target) {
		if (isUrl(target)) return utf8ToWide(target);

		const fs::path native = toNative(exec, target);
		std::error_code ec;
		if (!fs::exists(native, ec)) return utf8ToWide(exec.pathConv().toWin32(target));

		const fs::path full = fs::absolute(native, ec);
		return (ec ? native : full).native();
	}

	static void appendWindowsArgument(std::wstring& line, const std::wstring& arg) {
		if (!line.empty()) line.push_back(L' ');
		if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
			line += arg;
			return;
		}

		line.push_back(L'"');
		std::size_t backslashes = 0;
		for (const wchar_t c : arg) {
			if (c == L'\\') ++backslashes;
			else if (c == L'"') line.append(backslashes + 1, L'\\');
			if (c != L'\\') backslashes = 0;
			line.push_back(c);
		}

		line.append(backslashes, L'\\');
		line.push_back(L'"');
	}

	static std::wstring startParameters(Executor& exec, const std::vector<std::string>& command) {
		std::wstring line;
		for (std::size_t i = 1; i < command.size(); ++i) {
			appendWindowsArgument(line, utf8ToWide(exec.pathConv().translateArg(command[i])));
		}

		return line;
	}

	static int waitForStarted(HANDLE process) {
		::WaitForSingleObject(process, INFINITE);
		DWORD code = 0;
		::GetExitCodeProcess(process, &code);
		::CloseHandle(process);
		return shellStatusFromExitCode(code);
	}

	// SEE_MASK_FLAG_NO_UI: a failure is reported here, never in a dialog.
	static int launchStart(Executor& exec, const StartRequest& request) {
		const std::wstring target = startTarget(exec, request.command[0]);
		const std::wstring parameters = startParameters(exec, request.command);

		SHELLEXECUTEINFOW info = {};
		info.cbSize = sizeof(info);
		info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI
			| (request.wait ? SEE_MASK_NOCLOSEPROCESS : 0);
		info.lpFile = target.c_str();
		info.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
		info.nShow = request.show;
		if (::ShellExecuteExW(&info) == 0) {
			const std::error_code ec(static_cast<int>(::GetLastError()), std::system_category());
			perr("start", request.command[0] + ": " + posixErrorText(ec));
			return 1;
		}

		if (!request.wait || info.hProcess == nullptr) return 0;
		return waitForStarted(info.hProcess);
	}

	static int builtin_start(Executor& exec, const std::vector<std::string>& args) {
		StartRequest request;
		parseStartArgs(args, request);
		if (request.help) return showText(kStartUsage);
		if (request.version) return showVersion("start", "");
		if (request.command.empty()) return usageFailure(kStartUsage);

		return launchStart(exec, request);
	}

	static bool isWinptyOption(const std::string& arg) {
		return arg.rfind("-X", 0) == 0 || arg == "--mouse" || arg == "--showkey";
	}

	static int runPassedThrough(Executor& exec, const std::vector<std::string>& command) {
		const std::vector<std::string> args(command.begin() + 1, command.end());
		if (exec.isFunction(command[0])) return exec.callFunction(command[0], args);
		if (exec.isBuiltin(command[0])) return exec.callBuiltin(command[0], args);

		return exec.runExternal(command, {});
	}

	static int builtin_winpty(Executor& exec, const std::vector<std::string>& args) {
		std::size_t first = 0;
		while (first < args.size() && isWinptyOption(args[first])) ++first;
		if (first < args.size() && (args[first] == "-h" || args[first] == "--help")) {
			return showText(kWinptyUsage);
		}

		if (first < args.size() && args[first] == "--version") {
			return showVersion("winpty", "pass-through: wbsh gives programs a real console\n");
		}

		if (first < args.size() && args[first] == "--") ++first;
		if (first >= args.size()) return usageFailure(kWinptyUsage);

		const std::vector<std::string> command(args.begin() + static_cast<std::ptrdiff_t>(first),
			args.end());
		return runPassedThrough(exec, command);
	}

	void registerGitBashBuiltins(Executor& exec) {
		exec.registerBuiltin("cygpath", builtin_cygpath);
		exec.registerBuiltin("start",   builtin_start);
		exec.registerBuiltin("winpty",  builtin_winpty);
	}

}  // namespace wbsh

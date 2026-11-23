/**
 * @file testops.cpp
 * @brief The unary and file-comparison operators shared by `test` / `[`
 *        and `[[ ]]`.
 */

#include "testops.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>

#  include <io.h>
#endif /* _WIN32 */

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <string>
#include <sys/stat.h>

#include "diagnostics.h"
#include "executor.h"
#include "fifo.h"
#include "filemode.h"
#include "numparse.h"
#include "pathconv.h"
#include "procfs.h"

namespace wbsh::testops_detail {

	namespace fs = std::filesystem;

	static const char kUnaryLetters[] = "abcdefghkLNOGprsStuwxznvoR";

	static const char* const kCharDevices[] = {
		"/dev/null", "/dev/zero", "/dev/random", "/dev/urandom", "/dev/full", "/dev/tty",
		"/dev/stdin", "/dev/stdout", "/dev/stderr", "NUL", "nul", "CON", "con",
	};

	static const std::string kFdPrefix = "/dev/fd/";

	static bool isCharDevice(const std::string& path) {
		for (const char* device : kCharDevices) {
			if (path == device) return true;
		}

		return false;
	}

	static bool statOperand(Executor& exec, const std::string& operand, struct stat& info) {
		if (operand.empty()) return false;
		return statPath(exec.pathConv().toWin32(operand), info);
	}

	static fs::path nativePath(Executor& exec, const std::string& operand) {
		return utf8ToPath(exec.pathConv().toWin32(operand));
	}

	static bool isRunnable(Executor& exec, const std::string& operand) {
		struct stat info {};
		if (!statOperand(exec, operand, info)) return false;

		return hasOwnerExecute(nativePath(exec, operand), (info.st_mode & S_IFMT) == S_IFDIR);
	}

	static bool isLink(Executor& exec, const std::string& operand) {
		if (operand.empty()) return false;

		std::error_code ec;
		const fs::file_status status = fs::symlink_status(nativePath(exec, operand), ec);
		if (ec) return false;

		return status.type() == fs::file_type::symlink || status.type() == fs::file_type::junction;
	}

	static bool fdIsPipe(int fd) {
		const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
		return handle != INVALID_HANDLE_VALUE && GetFileType(handle) == FILE_TYPE_PIPE;
	}

	static bool isNamedPipePath(const std::string& win32_path) {
		std::string lower = win32_path;
		std::replace(lower.begin(), lower.end(), '/', '\\');
		for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return lower.rfind("\\\\.\\pipe\\", 0) == 0;
	}

	static const int kLinkHopLimit = 40;

	// Resolved one link at a time: canonical() would open the end of the
	// chain, which a socket file refuses.
	static fs::path linkChainEnd(const fs::path& start) {
		fs::path current = start;
		for (int hop = 0; hop < kLinkHopLimit; ++hop) {
			std::error_code ec;
			if (!fs::is_symlink(fs::symlink_status(current, ec)) || ec) return current;

			const fs::path target = fs::read_symlink(current, ec);
			if (ec) return current;
			current = target.is_absolute() ? target : current.parent_path() / target;
		}

		return current;
	}

	static bool isSocketOperand(Executor& exec, const std::string& operand) {
		return !operand.empty() && isUnixSocketFile(linkChainEnd(nativePath(exec, operand)));
	}

	static bool isFifoOperand(Executor& exec, const std::string& operand) {
		return !operand.empty() && isFifoFile(linkChainEnd(nativePath(exec, operand)));
	}

	// stat calls a socket file or a FIFO's marker a regular file.
	static bool isRegularFile(Executor& exec, const std::string& operand) {
		struct stat info {};
		if (!statOperand(exec, operand, info) || (info.st_mode & S_IFMT) != S_IFREG) return false;
		return !isSocketOperand(exec, operand) && !isFifoOperand(exec, operand);
	}

	// A FIFO reads as empty until someone writes to it, as on Linux.
	static bool hasContent(Executor& exec, const std::string& operand) {
		struct stat info {};
		if (!statOperand(exec, operand, info) || info.st_size <= 0) return false;
		return !isFifoOperand(exec, operand);
	}

	static bool isPipe(Executor& exec, const std::string& operand) {
		int fd = -1;
		if (operand.rfind(kFdPrefix, 0) == 0 && parseInt(operand.substr(kFdPrefix.size()), fd)) {
			return fdIsPipe(fd);
		}

		if (operand == "/dev/stdin") return fdIsPipe(0);

		const std::string win32 = exec.pathConv().toWin32(operand);
		if (!isNamedPipePath(win32)) return isFifoOperand(exec, operand);

		return GetFileAttributesW(utf8ToPath(win32).c_str()) != INVALID_FILE_ATTRIBUTES;
	}

	static bool isTerminalFd(const std::string& operand) {
		int fd = -1;
		std::size_t used = 0;
		if (!parseInt(operand, fd, 10, &used) || used != operand.size() || fd < 0) return false;

		const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
		DWORD mode = 0;
		return handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode) != 0;
	}

	// As for any builtin, bash expands the subscript again unless
	// array_expand_once is on; an error in it makes the test false.
	static bool evalTestSubscript(Executor& exec, const std::string& subscript, long long& out) {
		Expander& expander = exec.expander();
		const bool once = exec.env().shopts().array_expand_once;
		out = once ? expander.evalArithLiteral(subscript) : expander.evalArith(subscript);
		if (!expander.failed()) return true;

		shellErrorf("%s\n", expander.takeError().c_str());
		return false;
	}

	// An array's name alone stands for its element 0, as in bash.
	static bool isSetName(const Environment& env, const std::string& name) {
		if (const auto* indexed = env.getIndexedArray(name)) return indexed->count(0) != 0;
		if (const auto* assoc = env.getAssocArray(name)) return assoc->count("0") != 0;
		return env.has(name);
	}

	static bool hasAnyElement(const Environment& env, const std::string& name) {
		if (const auto* indexed = env.getIndexedArray(name)) return !indexed->empty();
		if (const auto* assoc = env.getAssocArray(name)) return !assoc->empty();
		return env.has(name);
	}

	// The test builtin expands an associative key once more, unless
	// assoc_expand_once; `[[ -v ]]` takes it as its word left it.
	static bool isSetVariable(Executor& exec, const std::string& operand, bool expands_key) {
		Environment& env = exec.env();
		const std::size_t bracket = operand.find('[');
		if (bracket == std::string::npos || operand.back() != ']') return isSetName(env, operand);

		const std::string name = operand.substr(0, bracket);
		const std::string subscript = operand.substr(bracket + 1, operand.size() - bracket - 2);
		// For an associative array `@` and `*` are keys like any other.
		if (const auto* assoc = env.getAssocArray(name)) {
			const bool expands = expands_key && !env.shopts().array_expand_once;
			return assoc->count(expands ? exec.expander().assocKeyText(subscript) : subscript) != 0;
		}
		if (subscript == "@" || subscript == "*") return hasAnyElement(env, name);

		long long index = 0;
		if (!evalTestSubscript(exec, subscript, index)) return false;
		if (const auto* indexed = env.getIndexedArray(name)) return indexed->count(index) != 0;

		return index == 0 && env.has(name);
	}

	static bool isOptionOn(Environment& env, const std::string& operand) {
		const SetOptionSpec* spec = findSetOption(operand);
		return spec != nullptr && env.options().*(spec->flag);
	}

	static bool modifiedSinceRead(const struct stat& info) {
		return info.st_mtime > info.st_atime;
	}

	static bool evalStringOperator(char op, const std::string& operand) {
		return op == 'z' ? operand.empty() : !operand.empty();
	}

	static bool evalStatOperator(char op, const struct stat& info) {
		const unsigned type = info.st_mode & S_IFMT;
		switch (op) {
			case 'a':
			case 'e': return true;
			case 'f': return type == S_IFREG;
			case 'd': return type == S_IFDIR;
			case 's': return info.st_size > 0;
			case 'r': return (info.st_mode & 0444) != 0;
			case 'w': return (info.st_mode & 0222) != 0;
			case 'O':
			case 'G': return true;
			case 'N': return modifiedSinceRead(info);
			default:  return false;
		}
	}


	static bool evalFileOperator(Executor& exec, char op, const std::string& operand);

	static bool procNodePasses(char op, const ProcNode& node) {
		const bool exists = node.type != ProcType::Missing;
		const bool descriptor = node.fd >= 0;
		switch (op) {
			case 'a':
			case 'e':
			case 'r':
			case 'O':
			case 'G': return exists;
			case 'f': return node.type == ProcType::File;
			case 'd':
			case 'x': return node.type == ProcType::Directory;
			case 'w': return descriptor;
			case 'p': return descriptor && node.target.rfind("pipe:", 0) == 0;
			case 'c': return descriptor && node.target.rfind("/dev/", 0) == 0;
			default:  return false;
		}
	}

	static bool evalProcOperator(Executor& exec, char op, const std::string& operand) {
		const bool link_test = op == 'h' || op == 'L';
		const ProcNode node = lookupProcPath(exec.pathConv(), operand, !link_test);
		if (link_test) return node.type == ProcType::Link;
		if (node.type == ProcType::Outside) return evalFileOperator(exec, op, node.target);
		return procNodePasses(op, node);
	}

	// As in Git Bash, a missing name with a `.exe` beside it is that file,
	// so launchers such as mvn and gradlew pass `[ -x "$JAVA_HOME/bin/java" ]`.
	// A device path is not probed: a `<(...)` pipe takes one stat only.
	static std::string withImplicitExe(Executor& exec, const std::string& operand) {
		struct stat info {};
		if (operand.empty() || isDevicePath(nativePath(exec, operand))) return operand;
		if (statOperand(exec, operand, info)) return operand;

		const std::string exe = operand + ".exe";
		const bool found = statOperand(exec, exe, info) && (info.st_mode & S_IFMT) == S_IFREG;
		return found ? exe : operand;
	}

	static int standardStreamFd(const std::string& name) {
		if (name == "/dev/stdin") return 0;
		if (name == "/dev/stdout") return 1;
		if (name == "/dev/stderr") return 2;

		int fd = -1;
		if (name.rfind(kFdPrefix, 0) != 0 || !parseInt(name.substr(kFdPrefix.size()), fd)) return -1;
		return fd;
	}

	static bool isAccessOperator(char op) {
		return op == 'a' || op == 'e' || op == 'r' || op == 'w';
	}

	// bash asks the descriptor behind /dev/fd/N and /dev/std*; the other
	// character devices always exist and can be read and written, and are
	// nothing but character devices: `-f /dev/tty` is false.
	static bool evalDeviceOperator(char op, const std::string& name, bool& handled) {
		handled = isAccessOperator(op);
		const int fd = standardStreamFd(name);
		if (fd >= 0) return handled && _get_osfhandle(fd) != -1;

		if (isCharDevice(name) && !isAccessOperator(op) && op != 'c' && std::isupper(op) == 0) {
			handled = true;
			return false;
		}

		handled = handled && isCharDevice(name) && name != "/dev/tty";
		return handled;
	}

	static bool evalFileOperator(Executor& exec, char op, const std::string& name) {
		bool device = false;
		const bool device_passes = evalDeviceOperator(op, name, device);
		if (device) return device_passes;
		if (isProcPath(name)) return evalProcOperator(exec, op, name);

		const bool type_of_entry = op == 'h' || op == 'L' || op == 'p' || op == 'c' || op == 'S';
		const std::string operand = type_of_entry ? name : withImplicitExe(exec, name);
		switch (op) {
			case 'x': return isRunnable(exec, operand);
			case 'h':
			case 'L': return isLink(exec, operand);
			case 'p': return isPipe(exec, operand);
			case 'c': return isCharDevice(operand);
			case 'S': return isSocketOperand(exec, operand);
			case 'f': return isRegularFile(exec, operand);
			case 's': return hasContent(exec, operand);
			case 'b':
			case 'g':
			case 'k':
			case 'u': return false;
			default:  break;
		}

		struct stat info {};
		return statOperand(exec, operand, info) && evalStatOperator(op, info);
	}

	static fs::file_time_type modificationTime(Executor& exec, const std::string& operand,
			bool& exists) {
		std::error_code ec;
		const fs::file_time_type time = fs::last_write_time(nativePath(exec, operand), ec);
		exists = !ec && !operand.empty();
		return time;
	}

	static bool sameFile(Executor& exec, const std::string& lhs, const std::string& rhs) {
		if (lhs.empty() || rhs.empty()) return false;

		std::error_code ec;
		const bool same = fs::equivalent(nativePath(exec, lhs), nativePath(exec, rhs), ec);
		return !ec && same;
	}

}  // namespace wbsh::testops_detail

namespace wbsh {

	bool isUnaryTestOperator(const std::string& op) {
		return op.size() == 2 && op[0] == '-' && op[1] != '\0'
			&& std::strchr(testops_detail::kUnaryLetters, op[1]) != nullptr;
	}

	bool evalUnaryTestOperator(Executor& exec, char op, const std::string& operand,
			bool test_builtin) {
		switch (op) {
			case 'z':
			case 'n': return testops_detail::evalStringOperator(op, operand);
			case 'v': return testops_detail::isSetVariable(exec, operand, test_builtin);
			case 'R': return exec.env().isNameref(operand);
			case 'o': return testops_detail::isOptionOn(exec.env(), operand);
			case 't': return testops_detail::isTerminalFd(operand);
			default:  return testops_detail::evalFileOperator(exec, op, operand);
		}
	}

	bool isFileCompareOperator(const std::string& op) {
		return op == "-nt" || op == "-ot" || op == "-ef";
	}

	bool evalFileCompare(Executor& exec, const std::string& op, const std::string& lhs,
			const std::string& rhs) {
		if (op == "-ef") return testops_detail::sameFile(exec, lhs, rhs);

		bool lhs_exists = false;
		bool rhs_exists = false;
		const auto lhs_time = testops_detail::modificationTime(exec, lhs, lhs_exists);
		const auto rhs_time = testops_detail::modificationTime(exec, rhs, rhs_exists);
		if (op == "-nt") return lhs_exists && (!rhs_exists || lhs_time > rhs_time);
		return rhs_exists && (!lhs_exists || lhs_time < rhs_time);
	}

}  // namespace wbsh

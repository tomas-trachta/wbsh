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
#include <fstream>
#include <string>
#include <sys/stat.h>

#include "executor.h"
#include "numparse.h"
#include "pathconv.h"

namespace wbsh::testops_detail {

	namespace fs = std::filesystem;

	static const char kUnaryLetters[] = "abcdefghkLNOGprsStuwxznvo";

	static const char* const kCharDevices[] = {
		"/dev/null", "/dev/zero", "/dev/random", "/dev/urandom", "/dev/full", "/dev/tty",
		"/dev/stdin", "/dev/stdout", "/dev/stderr", "NUL", "nul", "CON", "con",
	};

	static const char* const kRunnableExtensions[] = { ".exe", ".com", ".bat", ".cmd" };

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

	static std::string lowerExtension(const fs::path& path) {
		std::string extension = pathToUtf8(path.extension());
		for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return extension;
	}

	static bool startsWithRunnableMagic(const fs::path& path) {
		std::ifstream file(path, std::ios::binary);
		char magic[2] = { 0, 0 };
		if (!file.read(magic, sizeof magic)) return false;

		return (magic[0] == '#' && magic[1] == '!') || (magic[0] == 'M' && magic[1] == 'Z');
	}

	static bool isRunnable(Executor& exec, const std::string& operand) {
		struct stat info {};
		if (!statOperand(exec, operand, info)) return false;
		if ((info.st_mode & S_IFMT) == S_IFDIR) return true;

		const fs::path path = nativePath(exec, operand);
		const std::string extension = lowerExtension(path);
		for (const char* runnable : kRunnableExtensions) {
			if (extension == runnable) return true;
		}

		return startsWithRunnableMagic(path);
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

	static bool isPipe(Executor& exec, const std::string& operand) {
		int fd = -1;
		if (operand.rfind(kFdPrefix, 0) == 0 && parseInt(operand.substr(kFdPrefix.size()), fd)) {
			return fdIsPipe(fd);
		}

		if (operand == "/dev/stdin") return fdIsPipe(0);

		const std::string win32 = exec.pathConv().toWin32(operand);
		if (!isNamedPipePath(win32)) return false;

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

	static bool isSetVariable(Environment& env, const std::string& operand) {
		const std::size_t bracket = operand.find('[');
		if (bracket == std::string::npos || operand.back() != ']') return env.has(operand);

		const std::string name = operand.substr(0, bracket);
		const std::string subscript = operand.substr(bracket + 1, operand.size() - bracket - 2);
		if (const auto* assoc = env.getAssocArray(name)) return assoc->count(subscript) != 0;
		if (const auto* indexed = env.getIndexedArray(name)) {
			long long index = 0;
			return parseLL(subscript, index) && indexed->count(index) != 0;
		}

		return subscript == "0" && env.has(name);
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

	static bool evalFileOperator(Executor& exec, char op, const std::string& operand) {
		switch (op) {
			case 'x': return isRunnable(exec, operand);
			case 'h':
			case 'L': return isLink(exec, operand);
			case 'p': return isPipe(exec, operand);
			case 'c': return isCharDevice(operand);
			case 'b':
			case 'g':
			case 'k':
			case 'S':
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

	bool evalUnaryTestOperator(Executor& exec, char op, const std::string& operand) {
		switch (op) {
			case 'z':
			case 'n': return testops_detail::evalStringOperator(op, operand);
			case 'v': return testops_detail::isSetVariable(exec.env(), operand);
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

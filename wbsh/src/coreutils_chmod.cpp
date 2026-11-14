/**
 * @file coreutils_chmod.cpp
 * @brief chmod: octal and symbolic modes, -R, --reference, -v / -c.
 *
 * Modes map onto Windows through filemode.h: the owner write bit is the
 * read-only attribute, and the execute bits `test -x`, `ls -l` and `stat`
 * report follow what chmod set. A clause without u/g/o/a is limited by the
 * shell's umask, as GNU chmod does.
 */

#include "coreutils_internal.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif /* _WIN32 */

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "executor.h"
#include "filemode.h"
#include "numparse.h"
#include "pathconv.h"

namespace wbsh {

	namespace fs = std::filesystem;

	namespace chmod_detail {

		struct ChmodOptions {
			bool recursive = false;
			bool verbose = false;
			bool changes = false;
			bool quiet = false;
			unsigned umask = 022;
			std::string mode;
			std::string reference;
			std::vector<std::string> files;
		};

	}  // namespace chmod_detail

	using chmod_detail::ChmodOptions;

	static const unsigned kPermissionBits = 0777;
	static const unsigned kTypeDirectory = 0040000;
	static const unsigned kDefaultUmask = 022;

	static const std::string kReferencePrefix = "--reference=";

	static bool isFlagCluster(const std::string& arg) {
		return arg.size() > 1 && arg[0] == '-'
			&& arg.find_first_not_of("Rvcf", 1) == std::string::npos;
	}

	static void applyFlag(char flag, ChmodOptions& options) {
		switch (flag) {
			case 'R':
				options.recursive = true;
				break;
			case 'v':
				options.verbose = true;
				break;
			case 'c':
				options.changes = true;
				break;
			default:
				options.quiet = true;
				break;
		}
	}

	static char longOptionFlag(const std::string& arg) {
		if (arg == "--recursive") return 'R';
		if (arg == "--verbose") return 'v';
		if (arg == "--changes") return 'c';
		if (arg == "--silent" || arg == "--quiet") return 'f';
		return 0;
	}

	static bool applyLongOption(const std::string& arg, ChmodOptions& options) {
		if (arg.compare(0, kReferencePrefix.size(), kReferencePrefix) == 0) {
			options.reference = arg.substr(kReferencePrefix.size());
			return true;
		}

		const char flag = longOptionFlag(arg);
		if (flag != 0) applyFlag(flag, options);
		return flag != 0 || arg == "--preserve-root" || arg == "--no-preserve-root";
	}

	// -w, -x and -rwx are modes, not options: only R, v, c and f are flags.
	static bool sortArgument(const std::string& arg, ChmodOptions& options,
	                         std::vector<std::string>& operands) {
		if (isFlagCluster(arg)) {
			for (std::size_t k = 1; k < arg.size(); ++k) applyFlag(arg[k], options);
			return true;
		}

		if (arg.compare(0, 2, "--") != 0) {
			operands.push_back(arg);
			return true;
		}

		if (applyLongOption(arg, options)) return true;

		perr("chmod", "unrecognized option '" + arg + "'");
		return false;
	}

	static bool splitOperands(std::vector<std::string> operands, ChmodOptions& options) {
		if (options.reference.empty() && !operands.empty()) {
			options.mode = operands.front();
			operands.erase(operands.begin());
		}

		options.files = std::move(operands);
		if (!options.files.empty()) return true;

		const std::string after = options.mode.empty() ? "" : " after '" + options.mode + "'";
		perr("chmod", "missing operand" + after);
		return false;
	}

	static bool parseChmodArgs(const std::vector<std::string>& args, ChmodOptions& options) {
		std::vector<std::string> operands;
		bool only_operands = false;
		for (const std::string& arg : args) {
			if (only_operands) {
				operands.push_back(arg);
			} else if (arg == "--") {
				only_operands = true;
			} else if (!sortArgument(arg, options, operands)) {
				return false;
			}
		}

		return splitOperands(std::move(operands), options);
	}

	static unsigned shellUmask(Executor& exec) {
		unsigned long mask = 0;
		const std::string text = exec.env().get("_WBSH_UMASK");
		if (text.empty() || !parseUL(text, mask, 8)) return kDefaultUmask;

		return static_cast<unsigned>(mask) & kPermissionBits;
	}

	static bool isReadOnly(const fs::path& native) {
#ifdef _WIN32
		const DWORD attributes = ::GetFileAttributesW(native.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0;
#else
		(void)native;
		return false;
#endif
	}

	static unsigned currentPermissions(const fs::path& native, bool is_directory) {
		return filePermissions(native, is_directory, isReadOnly(native));
	}

	static bool referenceMode(Executor& exec, ChmodOptions& options) {
		const fs::path native = toNative(exec, options.reference);
		std::error_code ec;
		const fs::file_status status = fs::status(native, ec);
		if (ec || !fs::exists(status)) {
			perr("chmod", "failed to get attributes of '" + options.reference
				+ "': No such file or directory");
			return false;
		}

		char octal[8];
		const unsigned mode = currentPermissions(native, fs::is_directory(status));
		std::snprintf(octal, sizeof(octal), "%o", mode);
		options.mode = octal;
		return true;
	}

	static bool validMode(const ChmodOptions& options) {
		unsigned probe = 0;
		if (applyModeText(options.mode, options.umask, probe)) return true;

		perr("chmod", "invalid mode: '" + options.mode + "'");
		return false;
	}

	static unsigned changedPermissions(const ChmodOptions& options, unsigned old_mode,
	                                   bool is_directory) {
		unsigned mode = old_mode | (is_directory ? kTypeDirectory : 0);
		applyModeText(options.mode, options.umask, mode);
		return mode & kPermissionBits;
	}

	static std::string describeMode(unsigned mode) {
		static const char kLetters[] = "rwxrwxrwx";
		std::string letters;
		for (int bit = 0; bit < 9; ++bit) {
			letters.push_back((mode & (0400u >> bit)) != 0 ? kLetters[bit] : '-');
		}

		char octal[8];
		std::snprintf(octal, sizeof(octal), "%04o", mode);
		return std::string(octal) + " (" + letters + ")";
	}

	static void reportChange(const ChmodOptions& options, const std::string& shown,
	                         unsigned old_mode, unsigned new_mode) {
		if (old_mode != new_mode && (options.verbose || options.changes)) {
			std::printf("mode of '%s' changed from %s to %s\n", shown.c_str(),
				describeMode(old_mode).c_str(), describeMode(new_mode).c_str());
		} else if (old_mode == new_mode && options.verbose) {
			std::printf("mode of '%s' retained as %s\n", shown.c_str(),
				describeMode(old_mode).c_str());
		}
	}

	static bool changeOne(const ChmodOptions& options, const std::string& shown,
	                      const fs::path& native, bool is_directory) {
		const unsigned old_mode = currentPermissions(native, is_directory);
		const unsigned new_mode = changedPermissions(options, old_mode, is_directory);
		std::error_code ec;
		if (new_mode != old_mode && !setFilePermissions(native, new_mode, ec)) {
			if (!options.quiet) {
				perr("chmod", "changing permissions of '" + shown + "': " + posixErrorText(ec));
			}

			return false;
		}

		reportChange(options, shown, old_mode, new_mode);
		return true;
	}

	static std::string childName(const std::string& parent, const fs::path& child) {
		const std::string name = pathToUtf8(child.filename());
		const char last = parent.empty() ? '\0' : parent.back();
		return last == '/' || last == '\\' ? parent + name : parent + "/" + name;
	}

	static bool changeTree(const ChmodOptions& options, const std::string& shown,
	                       const fs::path& native);

	// Links met on the way down are skipped, as GNU chmod -R does by default.
	static bool changeChildren(const ChmodOptions& options, const std::string& shown,
	                           const fs::path& native) {
		std::error_code ec;
		fs::directory_iterator it(native, ec);
		if (ec) {
			if (!options.quiet) {
				perr("chmod", "cannot read directory '" + shown + "': " + posixErrorText(ec));
			}

			return false;
		}

		bool ok = true;
		for (const fs::directory_entry& child : it) {
			if (isLinkStatus(child.symlink_status(ec))) continue;
			if (!changeTree(options, childName(shown, child.path()), child.path())) ok = false;
		}

		return ok;
	}

	static bool changeTree(const ChmodOptions& options, const std::string& shown,
	                       const fs::path& native) {
		std::error_code ec;
		const fs::file_status status = fs::status(native, ec);
		if (ec || !fs::exists(status)) {
			if (!options.quiet) {
				perr("chmod", "cannot access '" + shown + "': No such file or directory");
			}

			return false;
		}

		const bool is_directory = fs::is_directory(status);
		bool ok = changeOne(options, shown, native, is_directory);
		if (options.recursive && is_directory) ok = changeChildren(options, shown, native) && ok;
		return ok;
	}

	static int builtin_chmod(Executor& exec, const std::vector<std::string>& args) {
		ChmodOptions options;
		options.umask = shellUmask(exec);
		if (!parseChmodArgs(args, options)) return 1;
		if (!options.reference.empty() && !referenceMode(exec, options)) return 1;
		if (!validMode(options)) return 1;

		int status = 0;
		for (const std::string& file : options.files) {
			if (!changeTree(options, file, toNative(exec, file))) status = 1;
		}

		return status;
	}

	void registerChmodBuiltin(Executor& exec) {
		exec.registerBuiltin("chmod", builtin_chmod);
	}

}  // namespace wbsh

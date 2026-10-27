/**
 * @file coreutils_remove.cpp
 * @brief rm, with GNU's refusal to remove `.`, `..` and the root.
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
#include <vector>

#include "executor.h"
#include "optscan.h"
#include "pathconv.h"

namespace wbsh {

	namespace fs = std::filesystem;

	namespace remove_detail {

		enum OptId {
			kOptForce,
			kOptRecursive,
			kOptPreserveRoot,
			kOptNoPreserveRoot,
		};

		struct RmOptions {
			bool recursive = false;
			bool force = false;
			bool preserve_root = true;
			std::vector<std::string> operands;
		};

	}  // namespace remove_detail

	using remove_detail::RmOptions;

	static const std::vector<OptDef>& rmOptionDefs() {
		static const std::vector<OptDef> defs = {
			{ remove_detail::kOptForce,          'f', "force",            OptArg::None },
			{ remove_detail::kOptRecursive,      'r', "recursive",        OptArg::None },
			{ remove_detail::kOptRecursive,      'R', nullptr,            OptArg::None },
			{ remove_detail::kOptPreserveRoot,   0,   "preserve-root",    OptArg::Optional },
			{ remove_detail::kOptNoPreserveRoot, 0,   "no-preserve-root", OptArg::None },
		};
		return defs;
	}

	static void applyRmOption(const OptHit& hit, RmOptions& opts) {
		switch (hit.id) {
			case remove_detail::kOptForce:
				opts.force = true;
				break;
			case remove_detail::kOptRecursive:
				opts.recursive = true;
				break;
			case remove_detail::kOptPreserveRoot:
				opts.preserve_root = true;
				break;
			case remove_detail::kOptNoPreserveRoot:
				opts.preserve_root = false;
				break;
			default:
				break;
		}
	}

	static bool parseRmArgs(const std::vector<std::string>& args, RmOptions& opts) {
		OptScanResult scanned;
		if (!scanOptions("rm", args, rmOptionDefs(), scanned)) return false;

		for (const OptHit& hit : scanned.hits) applyRmOption(hit, opts);
		opts.operands = scanned.operands;
		return true;
	}

	static void reportCannotRemove(const std::string& shown, const std::string& reason) {
		std::fprintf(stderr, "wbsh: rm: cannot remove '%s': %s\n", shown.c_str(), reason.c_str());
	}

	static bool isNotFound(const std::error_code& ec) {
		return ec == std::errc::no_such_file_or_directory;
	}

	static bool failUnlessForcedAway(const std::string& shown, const std::error_code& ec,
	                                 const RmOptions& opts) {
		if (opts.force && isNotFound(ec)) return true;

		reportCannotRemove(shown, posixErrorText(ec));
		return false;
	}

	static bool isSeparator(char c) {
		return c == '/' || c == '\\';
	}

	static std::string lastComponent(const std::string& path) {
		std::size_t end = path.size();
		while (end > 0 && isSeparator(path[end - 1])) --end;

		std::size_t begin = end;
		while (begin > 0 && !isSeparator(path[begin - 1])) --begin;
		return path.substr(begin, end - begin);
	}

	static bool namesDotOrDotDot(const std::string& path) {
		const std::string last = lastComponent(path);
		return last == "." || last == "..";
	}

	static bool isUncShareRoot(const fs::path& normal) {
		const std::wstring root_name = normal.root_name().native();
		const bool unc = root_name.size() > 2 && root_name[0] == L'\\' && root_name[1] == L'\\';
		if (!unc) return false;

		std::size_t components = 0;
		for (const fs::path& part : normal.relative_path()) {
			if (!part.empty()) ++components;
		}

		return components <= 1;
	}

	static bool isFilesystemRoot(const fs::path& native) {
		std::error_code ec;
		const fs::path absolute = fs::absolute(native, ec);
		if (ec) return false;

		const fs::path normal = absolute.lexically_normal();
		if (normal.has_root_path() && normal.relative_path().empty()) return true;
		return isUncShareRoot(normal);
	}

	static bool refuseDotOrRoot(const std::string& operand, const fs::path& native,
	                            const RmOptions& opts) {
		if (namesDotOrDotDot(operand)) {
			std::fprintf(stderr, "wbsh: rm: refusing to remove '.' or '..' directory: "
				"skipping '%s'\n", operand.c_str());
			return true;
		}

		if (!opts.preserve_root || !isFilesystemRoot(native)) return false;

		std::fprintf(stderr, "wbsh: rm: it is dangerous to operate recursively on '%s'\n",
			operand.c_str());
		std::fprintf(stderr, "wbsh: rm: use --no-preserve-root to override this failsafe\n");
		return true;
	}

	static bool clearReadOnly(const fs::path& native) {
#ifdef _WIN32
		const DWORD attributes = ::GetFileAttributesW(native.c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES) return false;
		if ((attributes & FILE_ATTRIBUTE_READONLY) == 0) return false;

		return ::SetFileAttributesW(native.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY) != 0;
#else
		(void)native;
		return false;
#endif /* _WIN32 */
	}

	static bool removeOne(const fs::path& native, std::error_code& ec) {
		if (fs::remove(native, ec)) return true;
		if (ec != std::errc::permission_denied || !clearReadOnly(native)) return false;

		ec.clear();
		return fs::remove(native, ec);
	}

	static bool removeEntry(const fs::path& native, const std::string& shown,
	                        const RmOptions& opts) {
		std::error_code ec;
		if (removeOne(native, ec)) return true;
		if (!ec) ec = std::make_error_code(std::errc::no_such_file_or_directory);
		return failUnlessForcedAway(shown, ec, opts);
	}

	static bool queryLinkStatus(const fs::path& native, fs::file_status& status,
	                            std::error_code& ec) {
		status = fs::symlink_status(native, ec);
		if (ec) return false;
		if (status.type() != fs::file_type::not_found) return true;

		ec = std::make_error_code(std::errc::no_such_file_or_directory);
		return false;
	}

	static bool isRealDirectory(const fs::file_status& status) {
		return status.type() == fs::file_type::directory;
	}

	static std::string childShownName(const std::string& shown, const fs::path& child) {
		const std::string name = pathToUtf8(child.filename());
		if (!shown.empty() && isSeparator(shown.back())) return shown + name;
		return shown + "/" + name;
	}

	static bool listChildren(const fs::path& native, const std::string& shown,
	                         const RmOptions& opts, std::vector<fs::path>& children) {
		std::error_code ec;
		fs::directory_iterator it(native, ec);
		if (ec) return failUnlessForcedAway(shown, ec, opts);

		for (; it != fs::directory_iterator(); it.increment(ec)) {
			children.push_back(it->path());
		}

		if (!ec) return true;
		return failUnlessForcedAway(shown, ec, opts);
	}

	static bool removeTree(const fs::path& native, const std::string& shown,
	                       const RmOptions& opts);

	static bool removeDirectoryTree(const fs::path& native, const std::string& shown,
	                                const RmOptions& opts) {
		std::vector<fs::path> children;
		if (!listChildren(native, shown, opts, children)) return false;

		bool all_removed = true;
		for (const fs::path& child : children) {
			if (!removeTree(child, childShownName(shown, child), opts)) all_removed = false;
		}

		if (!all_removed) return false;
		return removeEntry(native, shown, opts);
	}

	static bool removeTree(const fs::path& native, const std::string& shown,
	                       const RmOptions& opts) {
		std::error_code ec;
		fs::file_status status;
		if (!queryLinkStatus(native, status, ec)) return failUnlessForcedAway(shown, ec, opts);

		if (isRealDirectory(status)) return removeDirectoryTree(native, shown, opts);
		return removeEntry(native, shown, opts);
	}

	static bool removeOperand(Executor& exec, const std::string& operand, const RmOptions& opts) {
		const fs::path native = toNative(exec, operand);
		if (opts.recursive && refuseDotOrRoot(operand, native, opts)) return false;

		std::error_code ec;
		fs::file_status status;
		if (!queryLinkStatus(native, status, ec)) return failUnlessForcedAway(operand, ec, opts);

		if (opts.recursive) return removeTree(native, operand, opts);
		if (isRealDirectory(status)) {
			reportCannotRemove(operand, "Is a directory");
			return false;
		}

		return removeEntry(native, operand, opts);
	}

	static int builtin_rm(Executor& exec, const std::vector<std::string>& args) {
		RmOptions opts;
		if (!parseRmArgs(args, opts)) return 1;

		if (opts.operands.empty()) {
			if (opts.force) return 0;
			perr("rm", "missing operand");
			return 1;
		}

		int status = 0;
		for (const std::string& operand : opts.operands) {
			if (!removeOperand(exec, operand, opts)) status = 1;
		}

		return status;
	}

	void registerRemoveBuiltin(Executor& exec) {
		exec.registerBuiltin("rm", builtin_rm);
	}

}  // namespace wbsh

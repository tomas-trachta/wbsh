/**
 * @file coreutils_copy.cpp
 * @brief mv, with GNU's operand rules (-t, -T, several sources into a
 *        directory) and overwrite modes.
 *
 * A move is a MoveFileExW rename. Only a move to another volume
 * (ERROR_NOT_SAME_DEVICE) falls back to copy + remove; every other
 * failure is reported and leaves both sides untouched.
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

	namespace copy_detail {

		enum OptId {
			kOptForce,
			kOptInteractive,
			kOptNoClobber,
			kOptUpdate,
			kOptVerbose,
			kOptTargetDir,
			kOptNoTargetDir,
		};

		enum class Overwrite { Replace, Ask, Never };

		struct TargetOptions {
			bool no_target_dir = false;
			bool has_target_dir = false;
			std::string target_dir;
		};

		struct MvOptions {
			Overwrite overwrite = Overwrite::Replace;
			bool update = false;
			bool verbose = false;
			TargetOptions target;
			std::vector<std::string> operands;
		};

		struct Transfer {
			std::string source;
			std::string dest;
		};

		struct Endpoint {
			std::string shown;
			fs::path native;
			fs::file_status status;
			bool exists = false;
		};

	}  // namespace copy_detail

	using copy_detail::Endpoint;
	using copy_detail::MvOptions;
	using copy_detail::Overwrite;
	using copy_detail::TargetOptions;
	using copy_detail::Transfer;

	static void reportCopy(const char* tool, const std::string& message) {
		std::fprintf(stderr, "wbsh: %s: %s\n", tool, message.c_str());
	}

	static std::string inQuotes(const std::string& text) {
		return "'" + text + "'";
	}

	static bool isSeparator(char c) {
		return c == '/' || c == '\\';
	}

	static std::string baseName(const std::string& path) {
		std::size_t end = path.size();
		while (end > 0 && isSeparator(path[end - 1])) --end;

		std::size_t begin = end;
		while (begin > 0 && !isSeparator(path[begin - 1])) --begin;
		return path.substr(begin, end - begin);
	}

	static std::string joinShown(const std::string& dir, const std::string& name) {
		if (!dir.empty() && isSeparator(dir.back())) return dir + name;
		return dir + "/" + name;
	}

	static bool isRealDirectory(const fs::file_status& status) {
		return status.type() == fs::file_type::directory;
	}

	static bool isDirectoryOperand(Executor& exec, const std::string& operand) {
		std::error_code ec;
		return fs::is_directory(toNative(exec, operand), ec);
	}

	static bool askOverwrite(const char* tool, const std::string& dest) {
		std::fprintf(stderr, "%s: overwrite %s? ", tool, inQuotes(dest).c_str());
		std::fflush(stderr);

		char answer[256] = {};
		if (std::fgets(answer, sizeof(answer), stdin) == nullptr) return false;

		std::size_t i = 0;
		while (answer[i] == ' ' || answer[i] == '\t') ++i;
		return answer[i] == 'y' || answer[i] == 'Y';
	}

	static bool planIntoDirectory(Executor& exec, const char* tool, const std::string& dir,
	                              const std::vector<std::string>& sources,
	                              std::vector<Transfer>& plan) {
		if (!isDirectoryOperand(exec, dir)) {
			reportCopy(tool, "target " + inQuotes(dir) + " is not a directory");
			return false;
		}

		for (const std::string& source : sources) {
			plan.push_back({ source, joinShown(dir, baseName(source)) });
		}

		return true;
	}

	static bool checkOperandCount(const char* tool, const std::vector<std::string>& operands) {
		if (operands.empty()) {
			reportCopy(tool, "missing file operand");
			return false;
		}

		if (operands.size() == 1) {
			reportCopy(tool, "missing destination file operand after " + inQuotes(operands[0]));
			return false;
		}

		return true;
	}

	static bool planNoTargetDirectory(const char* tool, const std::vector<std::string>& operands,
	                                  std::vector<Transfer>& plan) {
		if (operands.size() > 2) {
			reportCopy(tool, "extra operand " + inQuotes(operands[2]));
			return false;
		}

		plan.push_back({ operands[0], operands[1] });
		return true;
	}

	static bool planTransfers(Executor& exec, const char* tool, const TargetOptions& target,
	                          const std::vector<std::string>& operands,
	                          std::vector<Transfer>& plan) {
		if (target.has_target_dir && target.no_target_dir) {
			reportCopy(tool, "cannot combine --target-directory (-t) "
				"and --no-target-directory (-T)");
			return false;
		}

		if (target.has_target_dir && operands.empty()) {
			reportCopy(tool, "missing file operand");
			return false;
		}

		if (target.has_target_dir) {
			return planIntoDirectory(exec, tool, target.target_dir, operands, plan);
		}

		if (!checkOperandCount(tool, operands)) return false;
		if (target.no_target_dir) return planNoTargetDirectory(tool, operands, plan);

		const std::string& dest = operands.back();
		const std::vector<std::string> sources(operands.begin(), operands.end() - 1);
		if (sources.size() > 1 || isDirectoryOperand(exec, dest)) {
			return planIntoDirectory(exec, tool, dest, sources, plan);
		}

		plan.push_back({ sources[0], dest });
		return true;
	}

	static const std::vector<OptDef>& mvOptionDefs() {
		static const std::vector<OptDef> defs = {
			{ copy_detail::kOptForce,       'f', "force",               OptArg::None },
			{ copy_detail::kOptInteractive, 'i', "interactive",         OptArg::None },
			{ copy_detail::kOptNoClobber,   'n', "no-clobber",          OptArg::None },
			{ copy_detail::kOptUpdate,      'u', "update",              OptArg::None },
			{ copy_detail::kOptVerbose,     'v', "verbose",             OptArg::None },
			{ copy_detail::kOptTargetDir,   't', "target-directory",    OptArg::Required },
			{ copy_detail::kOptNoTargetDir, 'T', "no-target-directory", OptArg::None },
		};
		return defs;
	}

	static void applyTargetOption(const OptHit& hit, TargetOptions& target) {
		if (hit.id == copy_detail::kOptNoTargetDir) {
			target.no_target_dir = true;
			return;
		}

		target.has_target_dir = true;
		target.target_dir = hit.value;
	}

	static void applyMvOption(const OptHit& hit, MvOptions& opts) {
		switch (hit.id) {
			case copy_detail::kOptForce:
				opts.overwrite = Overwrite::Replace;
				break;
			case copy_detail::kOptInteractive:
				opts.overwrite = Overwrite::Ask;
				break;
			case copy_detail::kOptNoClobber:
				opts.overwrite = Overwrite::Never;
				break;
			case copy_detail::kOptUpdate:
				opts.update = true;
				break;
			case copy_detail::kOptVerbose:
				opts.verbose = true;
				break;
			default:
				applyTargetOption(hit, opts.target);
				break;
		}
	}

	static bool parseMvArgs(const std::vector<std::string>& args, MvOptions& opts) {
		OptScanResult scanned;
		if (!scanOptions("mv", args, mvOptionDefs(), scanned)) return false;

		for (const OptHit& hit : scanned.hits) applyMvOption(hit, opts);
		opts.operands = scanned.operands;
		return true;
	}

	static bool loadEndpoint(Executor& exec, const std::string& shown, Endpoint& endpoint) {
		endpoint.shown = shown;
		endpoint.native = toNative(exec, shown);

		std::error_code ec;
		endpoint.status = fs::symlink_status(endpoint.native, ec);
		endpoint.exists = !ec && endpoint.status.type() != fs::file_type::not_found;
		return endpoint.exists;
	}

	static fs::path resolvedPath(const fs::path& native) {
		std::error_code ec;
		const fs::path resolved = fs::weakly_canonical(native, ec);
		if (!ec) return resolved;
		return fs::absolute(native, ec).lexically_normal();
	}

	static bool sameComponent(const fs::path& a, const fs::path& b) {
#ifdef _WIN32
		return ::CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
		return a == b;
#endif /* _WIN32 */
	}

	static bool isStrictlyInside(const fs::path& inner, const fs::path& outer) {
		auto in = inner.begin();
		for (const fs::path& part : outer) {
			if (part.empty()) continue;
			if (in == inner.end() || !sameComponent(*in, part)) return false;
			++in;
		}

		while (in != inner.end() && in->empty()) ++in;
		return in != inner.end();
	}

	static bool movesIntoItself(const Endpoint& src, const Endpoint& dst) {
		if (!isRealDirectory(src.status)) return false;
		return isStrictlyInside(resolvedPath(dst.native), resolvedPath(src.native));
	}

	static bool isCaseOnlyRename(const Endpoint& src, const Endpoint& dst) {
		const fs::path src_name = src.native.filename();
		const fs::path dst_name = dst.native.filename();
		return src_name != dst_name && sameComponent(src_name, dst_name);
	}

	static bool isSameFile(const Endpoint& src, const Endpoint& dst) {
		if (!dst.exists) return false;

		std::error_code ec;
		return fs::equivalent(src.native, dst.native, ec) && !ec && !isCaseOnlyRename(src, dst);
	}

	static bool reportTypeConflict(const Endpoint& src, const Endpoint& dst) {
		const bool src_dir = isRealDirectory(src.status);
		const bool dst_dir = isRealDirectory(dst.status);
		if (src_dir == dst_dir) return false;

		if (src_dir) {
			reportCopy("mv", "cannot overwrite non-directory " + inQuotes(dst.shown)
				+ " with directory " + inQuotes(src.shown));
			return true;
		}

		reportCopy("mv", "cannot overwrite directory " + inQuotes(dst.shown)
			+ " with non-directory");
		return true;
	}

	static bool isSourceNewer(const Endpoint& src, const Endpoint& dst) {
		std::error_code ec;
		const fs::file_time_type src_time = fs::last_write_time(src.native, ec);
		if (ec) return true;

		const fs::file_time_type dst_time = fs::last_write_time(dst.native, ec);
		return ec || src_time > dst_time;
	}

	static bool keepExistingDestination(const Endpoint& src, const Endpoint& dst,
	                                    const MvOptions& opts) {
		if (!dst.exists || isCaseOnlyRename(src, dst)) return false;
		if (opts.overwrite == Overwrite::Never) return true;
		if (opts.update && !isSourceNewer(src, dst)) return true;
		return opts.overwrite == Overwrite::Ask && !askOverwrite("mv", dst.shown);
	}

	static std::error_code lastWin32Error() {
#ifdef _WIN32
		return std::error_code(static_cast<int>(::GetLastError()), std::system_category());
#else
		return std::make_error_code(std::errc::operation_not_supported);
#endif /* _WIN32 */
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

	static bool renameReplacing(const fs::path& from, const fs::path& to, std::error_code& ec) {
#ifdef _WIN32
		if (::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0) return true;

		ec = lastWin32Error();
		if (ec.value() != ERROR_ACCESS_DENIED || !clearReadOnly(to)) return false;
		if (::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0) return true;

		ec = lastWin32Error();
		return false;
#else
		fs::rename(from, to, ec);
		return !ec;
#endif /* _WIN32 */
	}

	static bool isCrossVolume(const std::error_code& ec) {
#ifdef _WIN32
		return ec.category() == std::system_category() && ec.value() == ERROR_NOT_SAME_DEVICE;
#else
		return ec == std::errc::cross_device_link;
#endif /* _WIN32 */
	}

	static bool removeEmptyDirectoryTarget(const Endpoint& src, const Endpoint& dst,
	                                       std::error_code& ec) {
		const bool both_dirs = isRealDirectory(dst.status) && isRealDirectory(src.status);
		if (!dst.exists || !both_dirs) return true;
		if (isCaseOnlyRename(src, dst)) return true;

		fs::remove(dst.native, ec);
		return !ec;
	}

	static bool copyAcrossVolumes(const Endpoint& src, const Endpoint& dst, std::error_code& ec) {
		const fs::copy_options options = fs::copy_options::recursive
			| fs::copy_options::copy_symlinks | fs::copy_options::overwrite_existing;
		fs::copy(src.native, dst.native, options, ec);
		if (ec) {
			std::error_code cleanup_ec;
			const bool created_here = !dst.exists || isRealDirectory(src.status);
			if (created_here) fs::remove_all(dst.native, cleanup_ec);
			return false;
		}

		fs::remove_all(src.native, ec);
		return !ec;
	}

	static bool performMove(const Endpoint& src, const Endpoint& dst) {
		std::error_code ec;
		const bool moved = removeEmptyDirectoryTarget(src, dst, ec)
			&& (renameReplacing(src.native, dst.native, ec)
				|| (isCrossVolume(ec) && copyAcrossVolumes(src, dst, ec)));
		if (moved) return true;

		reportCopy("mv", "cannot move " + inQuotes(src.shown) + " to " + inQuotes(dst.shown)
			+ ": " + posixErrorText(ec));
		return false;
	}

	static bool refuseMove(const Endpoint& src, const Endpoint& dst) {
		if (isSameFile(src, dst)) {
			reportCopy("mv", inQuotes(src.shown) + " and " + inQuotes(dst.shown)
				+ " are the same file");
			return true;
		}

		if (movesIntoItself(src, dst)) {
			reportCopy("mv", "cannot move " + inQuotes(src.shown)
				+ " to a subdirectory of itself, " + inQuotes(dst.shown));
			return true;
		}

		return dst.exists && !isCaseOnlyRename(src, dst) && reportTypeConflict(src, dst);
	}

	static bool moveOne(Executor& exec, const Transfer& transfer, const MvOptions& opts) {
		Endpoint src;
		if (!loadEndpoint(exec, transfer.source, src)) {
			reportCopy("mv", "cannot stat " + inQuotes(transfer.source)
				+ ": No such file or directory");
			return false;
		}

		Endpoint dst;
		loadEndpoint(exec, transfer.dest, dst);
		if (refuseMove(src, dst)) return false;
		if (keepExistingDestination(src, dst, opts)) return true;
		if (!performMove(src, dst)) return false;

		if (opts.verbose) {
			const std::string from = inQuotes(src.shown);
			const std::string to = inQuotes(dst.shown);
			std::printf("renamed %s -> %s\n", from.c_str(), to.c_str());
			std::fflush(stdout);
		}

		return true;
	}

	static int builtin_mv(Executor& exec, const std::vector<std::string>& args) {
		MvOptions opts;
		if (!parseMvArgs(args, opts)) return 1;

		std::vector<Transfer> plan;
		if (!planTransfers(exec, "mv", opts.target, opts.operands, plan)) return 1;

		int status = 0;
		for (const Transfer& transfer : plan) {
			if (!moveOne(exec, transfer, opts)) status = 1;
		}

		return status;
	}

	void registerCopyBuiltins(Executor& exec) {
		exec.registerBuiltin("mv", builtin_mv);
	}

}  // namespace wbsh

/**
 * @file coreutils_copy.cpp
 * @brief cp and mv, with GNU's operand rules (-t, -T, several sources
 *        into a directory) and overwrite modes.
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
#  include <winioctl.h>
#endif /* _WIN32 */

#include <cstddef>
#include <cstdio>
#include <cstring>
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
			kOptRecursive,
			kOptArchive,
			kOptPreserveCommon,
			kOptPreserve,
			kOptNoPreserve,
			kOptDereference,
			kOptNoDereference,
			kOptCommandLine,
			kOptRemoveDest,
		};

		enum class Overwrite { Replace, Ask, Never };

		enum class Deref { Unset, Never, CommandLine, Always };

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

		struct CpOptions {
			Overwrite overwrite = Overwrite::Replace;
			Deref deref = Deref::Unset;
			bool force = false;
			bool remove_destination = false;
			bool recursive = false;
			bool preserve_times = false;
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

	using copy_detail::CpOptions;
	using copy_detail::Deref;
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

	static bool keepExisting(const char* tool, const Endpoint& src, const Endpoint& dst,
	                         Overwrite overwrite, bool update) {
		if (!dst.exists) return false;
		if (overwrite == Overwrite::Never) return true;
		if (update && !isSourceNewer(src, dst)) return true;
		return overwrite == Overwrite::Ask && !askOverwrite(tool, dst.shown);
	}

	static bool keepExistingDestination(const Endpoint& src, const Endpoint& dst,
	                                    const MvOptions& opts) {
		if (isCaseOnlyRename(src, dst)) return false;
		return keepExisting("mv", src, dst, opts.overwrite, opts.update);
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

	static const std::vector<OptDef>& cpOptionDefs() {
		static const std::vector<OptDef> defs = {
			{ copy_detail::kOptRecursive,      'r', "recursive",           OptArg::None },
			{ copy_detail::kOptRecursive,      'R', nullptr,               OptArg::None },
			{ copy_detail::kOptArchive,        'a', "archive",             OptArg::None },
			{ copy_detail::kOptPreserveCommon, 'p', nullptr,               OptArg::None },
			{ copy_detail::kOptPreserve,       0,   "preserve",            OptArg::Optional },
			{ copy_detail::kOptNoPreserve,     0,   "no-preserve",         OptArg::Required },
			{ copy_detail::kOptForce,          'f', "force",               OptArg::None },
			{ copy_detail::kOptInteractive,    'i', "interactive",         OptArg::None },
			{ copy_detail::kOptNoClobber,      'n', "no-clobber",          OptArg::None },
			{ copy_detail::kOptUpdate,         'u', nullptr,               OptArg::None },
			{ copy_detail::kOptUpdate,         0,   "update",              OptArg::Optional },
			{ copy_detail::kOptVerbose,        'v', "verbose",             OptArg::None },
			{ copy_detail::kOptDereference,    'L', "dereference",         OptArg::None },
			{ copy_detail::kOptNoDereference,  'P', "no-dereference",      OptArg::None },
			{ copy_detail::kOptNoDereference,  'd', nullptr,               OptArg::None },
			{ copy_detail::kOptCommandLine,    'H', nullptr,               OptArg::None },
			{ copy_detail::kOptRemoveDest,     0,   "remove-destination",  OptArg::None },
			{ copy_detail::kOptTargetDir,      't', "target-directory",    OptArg::Required },
			{ copy_detail::kOptNoTargetDir,    'T', "no-target-directory", OptArg::None },
		};
		return defs;
	}

	static bool isKnownAttribute(const std::string& name) {
		static const char* const kAttributes[] = {
			"mode", "ownership", "timestamps", "links", "context", "xattr", "all",
		};
		for (const char* known : kAttributes) {
			if (name == known) return true;
		}

		return false;
	}

	static bool applyPreserveList(const OptHit& hit, const char* option, bool enable,
	                              CpOptions& opts) {
		if (!hit.has_value) {
			opts.preserve_times = enable;
			return true;
		}

		std::size_t start = 0;
		while (start <= hit.value.size()) {
			const std::size_t comma = hit.value.find(',', start);
			const std::size_t end = comma == std::string::npos ? hit.value.size() : comma;
			const std::string name = hit.value.substr(start, end - start);
			if (!isKnownAttribute(name)) {
				reportCopy("cp", "invalid argument " + inQuotes(name) + " for '--" + option + "'");
				return false;
			}

			if (name == "timestamps" || name == "all") opts.preserve_times = enable;
			start = end + 1;
		}

		return true;
	}

	static bool applyUpdateWhen(const OptHit& hit, CpOptions& opts) {
		if (!hit.has_value || hit.value == "older") {
			opts.update = true;
			return true;
		}

		if (hit.value == "all") return true;
		if (hit.value == "none") {
			opts.overwrite = Overwrite::Never;
			return true;
		}

		reportCopy("cp", "invalid argument " + inQuotes(hit.value) + " for '--update'");
		return false;
	}

	static void applyArchive(CpOptions& opts) {
		opts.recursive = true;
		opts.preserve_times = true;
		opts.deref = Deref::Never;
	}

	static bool applyCpLinkOption(const OptHit& hit, CpOptions& opts) {
		switch (hit.id) {
			case copy_detail::kOptDereference:
				opts.deref = Deref::Always;
				return true;
			case copy_detail::kOptNoDereference:
				opts.deref = Deref::Never;
				return true;
			case copy_detail::kOptCommandLine:
				opts.deref = Deref::CommandLine;
				return true;
			default:
				applyTargetOption(hit, opts.target);
				return true;
		}
	}

	static bool applyCpOption(const OptHit& hit, CpOptions& opts) {
		switch (hit.id) {
			case copy_detail::kOptRecursive:
				opts.recursive = true;
				return true;
			case copy_detail::kOptArchive:
				applyArchive(opts);
				return true;
			case copy_detail::kOptPreserveCommon:
				opts.preserve_times = true;
				return true;
			case copy_detail::kOptPreserve:
				return applyPreserveList(hit, "preserve", true, opts);
			case copy_detail::kOptNoPreserve:
				return applyPreserveList(hit, "no-preserve", false, opts);
			case copy_detail::kOptForce:
				opts.force = true;
				return true;
			case copy_detail::kOptInteractive:
				opts.overwrite = Overwrite::Ask;
				return true;
			case copy_detail::kOptNoClobber:
				opts.overwrite = Overwrite::Never;
				return true;
			case copy_detail::kOptUpdate:
				return applyUpdateWhen(hit, opts);
			case copy_detail::kOptVerbose:
				opts.verbose = true;
				return true;
			case copy_detail::kOptRemoveDest:
				opts.remove_destination = true;
				return true;
			default:
				return applyCpLinkOption(hit, opts);
		}
	}

	static bool parseCpArgs(const std::vector<std::string>& args, CpOptions& opts) {
		OptScanResult scanned;
		if (!scanOptions("cp", args, cpOptionDefs(), scanned)) return false;

		for (const OptHit& hit : scanned.hits) {
			if (!applyCpOption(hit, opts)) return false;
		}

		opts.operands = scanned.operands;
		return true;
	}

	static bool followsLinks(const CpOptions& opts, bool command_line) {
		switch (opts.deref) {
			case Deref::Always:
				return true;
			case Deref::Never:
				return false;
			case Deref::CommandLine:
				return command_line;
			default:
				return !opts.recursive;
		}
	}

	static void loadCopySource(const fs::path& native, const std::string& shown, bool follow,
	                           Endpoint& endpoint) {
		endpoint.shown = shown;
		endpoint.native = native;

		std::error_code ec;
		endpoint.status = follow ? fs::status(native, ec) : fs::symlink_status(native, ec);
		endpoint.exists = !ec && endpoint.status.type() != fs::file_type::not_found;
	}

	static void loadCopyTarget(const fs::path& native, const std::string& shown,
	                           Endpoint& endpoint) {
		loadCopySource(native, shown, false, endpoint);
	}

	static bool isDirectoryOrLinkToOne(const Endpoint& endpoint) {
		std::error_code ec;
		return isRealDirectory(endpoint.status) || fs::is_directory(endpoint.native, ec);
	}

	static void announceCopy(const Endpoint& src, const Endpoint& dst, const CpOptions& opts) {
		if (!opts.verbose) return;

		const std::string from = inQuotes(src.shown);
		const std::string to = inQuotes(dst.shown);
		std::printf("%s -> %s\n", from.c_str(), to.c_str());
		std::fflush(stdout);
	}

	static bool removeForRetry(const fs::path& native) {
		clearReadOnly(native);

		std::error_code ec;
		return fs::remove(native, ec);
	}

	static bool copyFileContents(const fs::path& from, const fs::path& to, std::error_code& ec) {
#ifdef _WIN32
		if (::CopyFileW(from.c_str(), to.c_str(), FALSE) != 0) return true;

		ec = lastWin32Error();
		return false;
#else
		return fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
#endif /* _WIN32 */
	}

	static bool writeFileCopy(const Endpoint& src, const Endpoint& dst, const CpOptions& opts) {
		if (opts.remove_destination && dst.exists) removeForRetry(dst.native);

		std::error_code ec;
		if (copyFileContents(src.native, dst.native, ec)) return true;

		const bool retry = opts.force && dst.exists && removeForRetry(dst.native);
		if (retry && copyFileContents(src.native, dst.native, ec)) return true;

		reportCopy("cp", "cannot copy " + inQuotes(src.shown) + " to " + inQuotes(dst.shown)
			+ ": " + posixErrorText(ec));
		return false;
	}

	static void stampCopy(const Endpoint& src, const Endpoint& dst, const CpOptions& opts) {
		std::error_code ec;
		if (!opts.preserve_times) {
			fs::last_write_time(dst.native, fs::file_time_type::clock::now(), ec);
			return;
		}

		const fs::file_time_type source_time = fs::last_write_time(src.native, ec);
		if (!ec) fs::last_write_time(dst.native, source_time, ec);
	}

	static bool copyRegularFile(const Endpoint& src, const Endpoint& dst, const CpOptions& opts) {
		if (dst.exists && isRealDirectory(dst.status)) {
			reportCopy("cp", "cannot overwrite directory " + inQuotes(dst.shown)
				+ " with non-directory");
			return false;
		}

		if (keepExisting("cp", src, dst, opts.overwrite, opts.update)) return true;
		if (!writeFileCopy(src, dst, opts)) return false;

		stampCopy(src, dst, opts);
		announceCopy(src, dst, opts);
		return true;
	}

	static const wchar_t* const kNtPathPrefix = L"\\??\\";

#ifdef _WIN32
	// The mount-point layout of REPARSE_DATA_BUFFER, which only the DDK headers declare.
	struct MountPointHeader {
		DWORD tag;
		WORD data_length;
		WORD reserved;
		WORD substitute_offset;
		WORD substitute_length;
		WORD print_offset;
		WORD print_length;
	};

	static std::vector<BYTE> mountPointBuffer(const std::wstring& target) {
		const std::wstring substitute = kNtPathPrefix + target;
		const std::size_t name_chars = substitute.size() + 1 + target.size() + 1;
		const std::size_t names_bytes = name_chars * sizeof(WCHAR);
		std::vector<BYTE> buffer(sizeof(MountPointHeader) + names_bytes, 0);

		MountPointHeader header{};
		header.tag = IO_REPARSE_TAG_MOUNT_POINT;
		const std::size_t fields_bytes = sizeof(MountPointHeader)
			- offsetof(MountPointHeader, substitute_offset);
		header.data_length = static_cast<WORD>(fields_bytes + names_bytes);
		header.substitute_length = static_cast<WORD>(substitute.size() * sizeof(WCHAR));
		header.print_offset = static_cast<WORD>(header.substitute_length + sizeof(WCHAR));
		header.print_length = static_cast<WORD>(target.size() * sizeof(WCHAR));
		std::memcpy(buffer.data(), &header, sizeof(header));

		BYTE* names = buffer.data() + sizeof(MountPointHeader);
		std::memcpy(names, substitute.c_str(), header.substitute_length);
		std::memcpy(names + header.print_offset, target.c_str(), header.print_length);
		return buffer;
	}

	static bool setMountPoint(const fs::path& link, const std::wstring& target,
	                          std::error_code& ec) {
		const HANDLE handle = ::CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		if (handle == INVALID_HANDLE_VALUE) {
			ec = lastWin32Error();
			return false;
		}

		std::vector<BYTE> buffer = mountPointBuffer(target);
		DWORD returned = 0;
		const bool set = ::DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(),
			static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr) != 0;
		if (!set) ec = lastWin32Error();
		::CloseHandle(handle);
		return set;
	}
#endif /* _WIN32 */

	static std::wstring junctionTarget(const fs::path& junction, std::error_code& ec) {
		std::wstring target = fs::read_symlink(junction, ec).native();
		const std::wstring prefix = kNtPathPrefix;
		if (target.compare(0, prefix.size(), prefix) == 0) target.erase(0, prefix.size());
		return target;
	}

	static bool copyJunction(const fs::path& from, const fs::path& to, std::error_code& ec) {
		const std::wstring target = junctionTarget(from, ec);
		if (ec) return false;

		fs::create_directory(to, ec);
		if (ec) return false;
#ifdef _WIN32
		if (setMountPoint(to, target, ec)) return true;
#endif /* _WIN32 */
		std::error_code cleanup_ec;
		fs::remove(to, cleanup_ec);
		return false;
	}

	static bool copyLink(const Endpoint& src, const Endpoint& dst, const CpOptions& opts) {
		if (keepExisting("cp", src, dst, opts.overwrite, opts.update)) return true;
		if (dst.exists && !isRealDirectory(dst.status)) removeForRetry(dst.native);

		std::error_code ec;
		if (src.status.type() == fs::file_type::junction) copyJunction(src.native, dst.native, ec);
		else fs::copy_symlink(src.native, dst.native, ec);
		if (ec) {
			reportCopy("cp", "cannot create symbolic link " + inQuotes(dst.shown) + ": "
				+ posixErrorText(ec));
			return false;
		}

		announceCopy(src, dst, opts);
		return true;
	}

	static bool prepareDirectoryTarget(const Endpoint& src, const Endpoint& dst,
	                                   const CpOptions& opts) {
		if (dst.exists && isDirectoryOrLinkToOne(dst)) return true;
		if (dst.exists) {
			reportCopy("cp", "cannot overwrite non-directory " + inQuotes(dst.shown)
				+ " with directory " + inQuotes(src.shown));
			return false;
		}

		std::error_code ec;
		fs::create_directory(dst.native, ec);
		if (ec) {
			reportCopy("cp", "cannot create directory " + inQuotes(dst.shown) + ": "
				+ posixErrorText(ec));
			return false;
		}

		announceCopy(src, dst, opts);
		return true;
	}

	static bool listDirectory(const Endpoint& dir, std::vector<fs::path>& children) {
		std::error_code ec;
		fs::directory_iterator it(dir.native, ec);
		for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
			children.push_back(longPathSafe(it->path()));
		}

		if (!ec) return true;
		reportCopy("cp", "cannot access " + inQuotes(dir.shown) + ": " + posixErrorText(ec));
		return false;
	}

	static bool copyEntry(const Endpoint& src, const Endpoint& dst, const CpOptions& opts);

	static bool copyChild(const fs::path& child, const Endpoint& src_dir, const Endpoint& dst_dir,
	                      const CpOptions& opts) {
		const fs::path name = child.filename();
		const std::string name_text = pathToUtf8(name);

		Endpoint src;
		loadCopySource(child, joinShown(src_dir.shown, name_text), followsLinks(opts, false), src);
		if (!src.exists) {
			reportCopy("cp", "cannot stat " + inQuotes(src.shown) + ": No such file or directory");
			return false;
		}

		Endpoint dst;
		const fs::path target = longPathSafe(dst_dir.native / name);
		loadCopyTarget(target, joinShown(dst_dir.shown, name_text), dst);
		return copyEntry(src, dst, opts);
	}

	static bool copyDirectory(const Endpoint& src, const Endpoint& dst, const CpOptions& opts) {
		if (!opts.recursive) {
			reportCopy("cp", "-r not specified; omitting directory " + inQuotes(src.shown));
			return false;
		}

		std::vector<fs::path> children;
		if (!listDirectory(src, children)) return false;
		if (!prepareDirectoryTarget(src, dst, opts)) return false;

		bool ok = true;
		for (const fs::path& child : children) {
			if (!copyChild(child, src, dst, opts)) ok = false;
		}

		if (opts.preserve_times) stampCopy(src, dst, opts);
		return ok;
	}

	static bool copyEntry(const Endpoint& src, const Endpoint& dst, const CpOptions& opts) {
		if (isLinkStatus(src.status)) return copyLink(src, dst, opts);
		if (isRealDirectory(src.status)) return copyDirectory(src, dst, opts);
		return copyRegularFile(src, dst, opts);
	}

	static bool refuseCopy(const Endpoint& src, const Endpoint& dst, const CpOptions& opts) {
		std::error_code ec;
		if (dst.exists && fs::equivalent(src.native, dst.native, ec) && !ec) {
			reportCopy("cp", inQuotes(src.shown) + " and " + inQuotes(dst.shown)
				+ " are the same file");
			return true;
		}

		if (!opts.recursive || !movesIntoItself(src, dst)) return false;

		reportCopy("cp", "cannot copy a directory, " + inQuotes(src.shown) + ", into itself, "
			+ inQuotes(dst.shown));
		return true;
	}

	static bool copyOne(Executor& exec, const Transfer& transfer, const CpOptions& opts) {
		Endpoint src;
		loadCopySource(toNative(exec, transfer.source), transfer.source, followsLinks(opts, true),
			src);
		if (!src.exists) {
			reportCopy("cp", "cannot stat " + inQuotes(transfer.source)
				+ ": No such file or directory");
			return false;
		}

		Endpoint dst;
		loadCopyTarget(toNative(exec, transfer.dest), transfer.dest, dst);
		if (refuseCopy(src, dst, opts)) return false;
		return copyEntry(src, dst, opts);
	}

	static int builtin_cp(Executor& exec, const std::vector<std::string>& args) {
		CpOptions opts;
		if (!parseCpArgs(args, opts)) return 1;

		std::vector<Transfer> plan;
		if (!planTransfers(exec, "cp", opts.target, opts.operands, plan)) return 1;

		int status = 0;
		for (const Transfer& transfer : plan) {
			if (!copyOne(exec, transfer, opts)) status = 1;
		}

		return status;
	}

	void registerCopyBuiltins(Executor& exec) {
		exec.registerBuiltin("cp", builtin_cp);
		exec.registerBuiltin("mv", builtin_mv);
	}

}  // namespace wbsh

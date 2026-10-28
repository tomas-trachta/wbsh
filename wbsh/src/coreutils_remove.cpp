/**
 * @file coreutils_remove.cpp
 * @brief rm, with GNU's refusal to remove `.`, `..` and the root.
 *
 * Links are never followed: a symlink or junction, dangling or not, is
 * removed itself and its target is left alone, also inside `rm -r`.
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
			kOptAlways,
			kOptOnce,
			kOptInteractive,
			kOptRecursive,
			kOptDir,
			kOptVerbose,
			kOptOneFileSystem,
			kOptPreserveRoot,
			kOptNoPreserveRoot,
		};

		enum class Prompt { Never, WriteProtected, Always };

		enum class Outcome { Removed, Kept, Failed };

		struct RmOptions {
			bool recursive = false;
			bool force = false;
			bool dirs = false;
			bool verbose = false;
			bool prompt_once = false;
			bool preserve_root = true;
			Prompt prompt = Prompt::WriteProtected;
			std::vector<std::string> operands;
		};

	}  // namespace remove_detail

	using remove_detail::Outcome;
	using remove_detail::Prompt;
	using remove_detail::RmOptions;

	static const std::vector<OptDef>& rmOptionDefs() {
		static const std::vector<OptDef> defs = {
			{ remove_detail::kOptForce,          'f', "force",            OptArg::None },
			{ remove_detail::kOptAlways,         'i', nullptr,            OptArg::None },
			{ remove_detail::kOptOnce,           'I', nullptr,            OptArg::None },
			{ remove_detail::kOptInteractive,    0,   "interactive",      OptArg::Optional },
			{ remove_detail::kOptRecursive,      'r', "recursive",        OptArg::None },
			{ remove_detail::kOptRecursive,      'R', nullptr,            OptArg::None },
			{ remove_detail::kOptDir,            'd', "dir",              OptArg::None },
			{ remove_detail::kOptVerbose,        'v', "verbose",          OptArg::None },
			{ remove_detail::kOptOneFileSystem,  0,   "one-file-system",  OptArg::None },
			{ remove_detail::kOptPreserveRoot,   0,   "preserve-root",    OptArg::Optional },
			{ remove_detail::kOptNoPreserveRoot, 0,   "no-preserve-root", OptArg::None },
		};
		return defs;
	}

	static void setPromptMode(RmOptions& opts, Prompt prompt, bool once) {
		opts.prompt = prompt;
		opts.prompt_once = once;
		opts.force = false;
	}

	static bool applyInteractiveWhen(const OptHit& hit, RmOptions& opts) {
		const std::string& when = hit.value;
		if (!hit.has_value || when == "always" || when == "yes") {
			setPromptMode(opts, Prompt::Always, false);
			return true;
		}

		if (when == "once") {
			setPromptMode(opts, Prompt::WriteProtected, true);
			return true;
		}

		if (when == "never" || when == "no" || when == "none") {
			opts.prompt = Prompt::Never;
			opts.prompt_once = false;
			return true;
		}

		perr("rm", "invalid argument '" + when + "' for '--interactive'");
		return false;
	}

	static void applyForce(RmOptions& opts) {
		opts.force = true;
		opts.prompt = Prompt::Never;
		opts.prompt_once = false;
	}

	static bool applyRmOption(const OptHit& hit, RmOptions& opts) {
		switch (hit.id) {
			case remove_detail::kOptForce:
				applyForce(opts);
				return true;
			case remove_detail::kOptAlways:
				setPromptMode(opts, Prompt::Always, false);
				return true;
			case remove_detail::kOptOnce:
				setPromptMode(opts, Prompt::WriteProtected, true);
				return true;
			case remove_detail::kOptInteractive:
				return applyInteractiveWhen(hit, opts);
			case remove_detail::kOptRecursive:
				opts.recursive = true;
				return true;
			case remove_detail::kOptDir:
				opts.dirs = true;
				return true;
			case remove_detail::kOptVerbose:
				opts.verbose = true;
				return true;
			case remove_detail::kOptPreserveRoot:
				opts.preserve_root = true;
				return true;
			case remove_detail::kOptNoPreserveRoot:
				opts.preserve_root = false;
				return true;
			default:
				return true;
		}
	}

	static bool parseRmArgs(const std::vector<std::string>& args, RmOptions& opts) {
		OptScanResult scanned;
		if (!scanOptions("rm", args, rmOptionDefs(), scanned)) return false;

		for (const OptHit& hit : scanned.hits) {
			if (!applyRmOption(hit, opts)) return false;
		}

		opts.operands = scanned.operands;
		return true;
	}

	static bool askUser(const std::string& question) {
		std::fprintf(stderr, "rm: %s? ", question.c_str());
		std::fflush(stderr);

		char answer[256] = {};
		if (std::fgets(answer, sizeof(answer), stdin) == nullptr) return false;

		std::size_t i = 0;
		while (answer[i] == ' ' || answer[i] == '\t') ++i;
		return answer[i] == 'y' || answer[i] == 'Y';
	}

	static void reportCannotRemove(const std::string& shown, const std::string& reason) {
		std::fprintf(stderr, "wbsh: rm: cannot remove '%s': %s\n", shown.c_str(), reason.c_str());
	}

	static bool isNotFound(const std::error_code& ec) {
		return ec == std::errc::no_such_file_or_directory;
	}

	static Outcome failUnlessForcedAway(const std::string& shown, const std::error_code& ec,
	                                    const RmOptions& opts) {
		if (opts.force && isNotFound(ec)) return Outcome::Removed;

		reportCannotRemove(shown, posixErrorText(ec));
		return Outcome::Failed;
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

		if (!opts.recursive || !opts.preserve_root || !isFilesystemRoot(native)) return false;

		std::fprintf(stderr, "wbsh: rm: it is dangerous to operate recursively on '%s'\n",
			operand.c_str());
		std::fprintf(stderr, "wbsh: rm: use --no-preserve-root to override this failsafe\n");
		return true;
	}

	static bool hasReadOnlyAttribute(const fs::path& native) {
#ifdef _WIN32
		const DWORD attributes = ::GetFileAttributesW(native.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0;
#else
		(void)native;
		return false;
#endif /* _WIN32 */
	}

	static bool clearReadOnly(const fs::path& native) {
#ifdef _WIN32
		if (!hasReadOnlyAttribute(native)) return false;

		const DWORD attributes = ::GetFileAttributesW(native.c_str());
		return ::SetFileAttributesW(native.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY) != 0;
#else
		(void)native;
		return false;
#endif /* _WIN32 */
	}

	static bool isRealDirectory(const fs::file_status& status) {
		return status.type() == fs::file_type::directory;
	}

	static std::string fileKind(const fs::path& native, const fs::file_status& status) {
		if (isLinkStatus(status)) return "symbolic link";
		if (isRealDirectory(status)) return "directory";
		if (status.type() != fs::file_type::regular) return "file";

		std::error_code ec;
		return fs::file_size(native, ec) == 0 && !ec ? "regular empty file" : "regular file";
	}

	static bool wantsPrompt(const fs::path& native, const fs::file_status& status,
	                        const RmOptions& opts, bool& write_protected) {
		write_protected = !isLinkStatus(status) && hasReadOnlyAttribute(native);
		if (opts.prompt == Prompt::Always) return true;
		if (opts.prompt == Prompt::Never || !write_protected) return false;
		return isConsoleStream(stdin);
	}

	static bool confirmRemoval(const fs::path& native, const std::string& shown,
	                           const fs::file_status& status, const RmOptions& opts) {
		bool write_protected = false;
		if (!wantsPrompt(native, status, opts, write_protected)) return true;

		const std::string protection = write_protected ? "write-protected " : "";
		return askUser("remove " + protection + fileKind(native, status) + " '" + shown + "'");
	}

	bool removeFileOrEmptyDirectory(const fs::path& native, std::error_code& ec) {
		if (fs::remove(native, ec)) return true;
		if (ec != std::errc::permission_denied || !clearReadOnly(native)) return false;

		ec.clear();
		return fs::remove(native, ec);
	}

	static void announceRemoved(const std::string& shown, const fs::file_status& status,
	                            const RmOptions& opts) {
		if (!opts.verbose) return;

		const char* what = isRealDirectory(status) ? "removed directory" : "removed";
		std::printf("%s '%s'\n", what, shown.c_str());
		std::fflush(stdout);
	}

	static Outcome removeEntry(const fs::path& native, const std::string& shown,
	                           const fs::file_status& status, const RmOptions& opts) {
		if (!confirmRemoval(native, shown, status, opts)) return Outcome::Kept;

		std::error_code ec;
		if (removeFileOrEmptyDirectory(native, ec)) {
			announceRemoved(shown, status, opts);
			return Outcome::Removed;
		}

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

	static std::string childShownName(const std::string& shown, const fs::path& child) {
		const std::string name = pathToUtf8(child.filename());
		if (!shown.empty() && isSeparator(shown.back())) return shown + name;
		return shown + "/" + name;
	}

	static Outcome listChildren(const fs::path& native, const std::string& shown,
	                            const RmOptions& opts, std::vector<fs::path>& children) {
		std::error_code ec;
		fs::directory_iterator it(native, ec);
		if (ec) return failUnlessForcedAway(shown, ec, opts);

		for (; it != fs::directory_iterator(); it.increment(ec)) {
			children.push_back(longPathSafe(it->path()));
		}

		if (!ec) return Outcome::Removed;
		return failUnlessForcedAway(shown, ec, opts);
	}

	static Outcome combineOutcomes(Outcome so_far, Outcome next) {
		if (so_far == Outcome::Failed || next == Outcome::Failed) return Outcome::Failed;
		if (so_far == Outcome::Kept || next == Outcome::Kept) return Outcome::Kept;
		return Outcome::Removed;
	}

	static Outcome removeTree(const fs::path& native, const std::string& shown,
	                          const RmOptions& opts);

	static Outcome removeChildren(const std::vector<fs::path>& children, const std::string& shown,
	                              const RmOptions& opts) {
		Outcome outcome = Outcome::Removed;
		for (const fs::path& child : children) {
			const Outcome child_outcome = removeTree(child, childShownName(shown, child), opts);
			outcome = combineOutcomes(outcome, child_outcome);
		}

		return outcome;
	}

	static Outcome removeDirectoryTree(const fs::path& native, const std::string& shown,
	                                   const fs::file_status& status, const RmOptions& opts) {
		std::vector<fs::path> children;
		const Outcome listed = listChildren(native, shown, opts, children);
		if (listed != Outcome::Removed) return listed;

		const bool descend = children.empty() || opts.prompt != Prompt::Always
			|| askUser("descend into directory '" + shown + "'");
		if (!descend) return Outcome::Kept;

		const Outcome children_outcome = removeChildren(children, shown, opts);
		if (children_outcome != Outcome::Removed) return children_outcome;
		return removeEntry(native, shown, status, opts);
	}

	static Outcome removeTree(const fs::path& native, const std::string& shown,
	                          const RmOptions& opts) {
		std::error_code ec;
		fs::file_status status;
		if (!queryLinkStatus(native, status, ec)) return failUnlessForcedAway(shown, ec, opts);

		if (isRealDirectory(status)) return removeDirectoryTree(native, shown, status, opts);
		return removeEntry(native, shown, status, opts);
	}

	static Outcome removeOperand(Executor& exec, const std::string& operand,
	                             const RmOptions& opts) {
		const fs::path native = toNative(exec, operand);
		const bool may_remove_dirs = opts.recursive || opts.dirs;
		if (may_remove_dirs && refuseDotOrRoot(operand, native, opts)) return Outcome::Failed;

		std::error_code ec;
		fs::file_status status;
		if (!queryLinkStatus(native, status, ec)) return failUnlessForcedAway(operand, ec, opts);

		if (opts.recursive) return removeTree(native, operand, opts);
		if (isRealDirectory(status) && !opts.dirs) {
			reportCannotRemove(operand, "Is a directory");
			return Outcome::Failed;
		}

		return removeEntry(native, operand, status, opts);
	}

	static bool confirmOnce(const RmOptions& opts) {
		const std::size_t count = opts.operands.size();
		if (!opts.prompt_once || (!opts.recursive && count <= 3)) return true;

		std::string question = "remove " + std::to_string(count)
			+ (count == 1 ? " argument" : " arguments");
		if (opts.recursive) question += " recursively";
		return askUser(question);
	}

	static int builtin_rm(Executor& exec, const std::vector<std::string>& args) {
		RmOptions opts;
		if (!parseRmArgs(args, opts)) return 1;

		if (opts.operands.empty()) {
			if (opts.force) return 0;
			perr("rm", "missing operand");
			return 1;
		}

		if (!confirmOnce(opts)) return 0;

		int status = 0;
		for (const std::string& operand : opts.operands) {
			if (removeOperand(exec, operand, opts) == Outcome::Failed) status = 1;
		}

		return status;
	}

	void registerRemoveBuiltin(Executor& exec) {
		exec.registerBuiltin("rm", builtin_rm);
	}

}  // namespace wbsh

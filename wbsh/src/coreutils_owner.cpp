/**
 * @file coreutils_owner.cpp
 * @brief chown, chgrp: NTFS owner and group.
 *
 * Owner and group are the file's owner and primary-group SIDs. An owner
 * or group that already is the requested one is never rewritten, so the
 * `chown "$(id -un)" FILE` lines of install scripts succeed for files the
 * user created. Giving a file to somebody else needs SeRestorePrivilege,
 * which only an elevated token holds; without it Windows refuses and the
 * command fails with EPERM, as an unprivileged chown does on Linux.
 */

#include "coreutils_internal.h"

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "executor.h"
#include "fileowner.h"
#include "optscan.h"
#include "pathconv.h"

namespace wbsh {

	namespace fs = std::filesystem;

	namespace owner_detail {

		enum OptId {
			kOptChanges,
			kOptSilent,
			kOptVerbose,
			kOptRecursive,
			kOptNoDereference,
			kOptDereference,
			kOptFollowArguments,
			kOptFollowAll,
			kOptFollowNone,
			kOptFrom,
			kOptReference,
			kOptPreserveRoot,
			kOptNoPreserveRoot,
		};

		enum class Report { Errors, Changes, Everything };

		enum class Traversal { None, Arguments, All };

		struct OwnerOptions {
			Report report = Report::Errors;
			Traversal traversal = Traversal::None;
			bool silent = false;
			bool recursive = false;
			bool dereference = true;
			bool dereference_given = false;
			bool preserve_root = false;
			bool has_from = false;
			std::string from;
			std::string reference;
			std::vector<std::string> operands;
		};

		struct OwnerSpec {
			bool has_owner = false;
			bool has_group = false;
			Account owner;
			Account group;
		};

		struct OwnerJob {
			const char* tool = "chown";
			OwnerOptions options;
			OwnerSpec target;
			OwnerSpec from;
		};

		struct Visit {
			std::string shown;
			fs::path native;
			bool follow = true;
		};

	}  // namespace owner_detail

	using owner_detail::OwnerJob;
	using owner_detail::OwnerOptions;
	using owner_detail::OwnerSpec;
	using owner_detail::Report;
	using owner_detail::Traversal;
	using owner_detail::Visit;

	static std::string inQuotes(const std::string& text) {
		return "'" + text + "'";
	}

	static const std::vector<OptDef>& ownerOptionDefs() {
		static const std::vector<OptDef> defs = {
			{ owner_detail::kOptChanges,         'c', "changes",          OptArg::None },
			{ owner_detail::kOptSilent,          'f', "silent",           OptArg::None },
			{ owner_detail::kOptSilent,          0,   "quiet",            OptArg::None },
			{ owner_detail::kOptVerbose,         'v', "verbose",          OptArg::None },
			{ owner_detail::kOptRecursive,       'R', "recursive",        OptArg::None },
			{ owner_detail::kOptNoDereference,   'h', "no-dereference",   OptArg::None },
			{ owner_detail::kOptDereference,     0,   "dereference",      OptArg::None },
			{ owner_detail::kOptFollowArguments, 'H', nullptr,            OptArg::None },
			{ owner_detail::kOptFollowAll,       'L', nullptr,            OptArg::None },
			{ owner_detail::kOptFollowNone,      'P', nullptr,            OptArg::None },
			{ owner_detail::kOptFrom,            0,   "from",             OptArg::Required },
			{ owner_detail::kOptReference,       0,   "reference",        OptArg::Required },
			{ owner_detail::kOptPreserveRoot,    0,   "preserve-root",    OptArg::None },
			{ owner_detail::kOptNoPreserveRoot,  0,   "no-preserve-root", OptArg::None },
		};
		return defs;
	}

	static void applyDereferenceOption(const OptHit& hit, OwnerOptions& options) {
		options.dereference_given = true;
		options.dereference = hit.id == owner_detail::kOptDereference;
	}

	static bool applyReportOption(const OptHit& hit, OwnerOptions& options) {
		if (hit.id == owner_detail::kOptChanges) options.report = Report::Changes;
		else if (hit.id == owner_detail::kOptVerbose) options.report = Report::Everything;
		else if (hit.id == owner_detail::kOptSilent) options.silent = true;
		else return false;
		return true;
	}

	static bool applyTraversalOption(const OptHit& hit, OwnerOptions& options) {
		if (hit.id == owner_detail::kOptFollowArguments) options.traversal = Traversal::Arguments;
		else if (hit.id == owner_detail::kOptFollowAll) options.traversal = Traversal::All;
		else if (hit.id == owner_detail::kOptFollowNone) options.traversal = Traversal::None;
		else return false;
		return true;
	}

	static void applyOwnerOption(const OptHit& hit, OwnerOptions& options) {
		if (applyReportOption(hit, options) || applyTraversalOption(hit, options)) return;

		switch (hit.id) {
			case owner_detail::kOptRecursive:
				options.recursive = true;
				break;
			case owner_detail::kOptPreserveRoot:
				options.preserve_root = true;
				break;
			case owner_detail::kOptNoPreserveRoot:
				options.preserve_root = false;
				break;
			case owner_detail::kOptReference:
				options.reference = hit.value;
				break;
			case owner_detail::kOptFrom:
				options.has_from = true;
				options.from = hit.value;
				break;
			default:
				applyDereferenceOption(hit, options);
				break;
		}
	}

	static bool parseOwnerArgs(const char* tool, const std::vector<std::string>& args,
	                           OwnerOptions& options) {
		OptScanResult scanned;
		if (!scanOptions(tool, args, ownerOptionDefs(), scanned)) return false;

		for (const OptHit& hit : scanned.hits) applyOwnerOption(hit, options);
		options.operands = scanned.operands;
		return true;
	}

	static bool checkOperands(const char* tool, const OwnerOptions& options) {
		const std::size_t needed = options.reference.empty() ? 2 : 1;
		if (options.operands.size() >= needed) return true;

		if (options.operands.empty()) perr(tool, "missing operand");
		else perr(tool, "missing operand after " + inQuotes(options.operands.back()));
		return false;
	}

	static bool resolveSpecPart(const std::string& name, bool& has, Account& account) {
		if (name.empty()) return true;

		has = true;
		return resolveAccount(name, account);
	}

	static std::size_t specSeparator(const std::string& spec) {
		const std::size_t colon = spec.find(':');
		if (colon != std::string::npos) return colon;

		Account whole;
		if (resolveAccount(spec, whole)) return std::string::npos;
		return spec.find('.');
	}

	static bool parseOwnerSpec(const char* tool, const std::string& spec, OwnerSpec& parsed) {
		const std::size_t separator = specSeparator(spec);
		const std::string owner = spec.substr(0, separator);
		const std::string group = separator == std::string::npos ? "" : spec.substr(separator + 1);
		if (!resolveSpecPart(owner, parsed.has_owner, parsed.owner)) {
			perr(tool, "invalid user: " + inQuotes(spec));
			return false;
		}

		if (!resolveSpecPart(group, parsed.has_group, parsed.group)) {
			perr(tool, "invalid group: " + inQuotes(spec));
			return false;
		}

		const bool login_group = separator != std::string::npos && group.empty();
		if (login_group && parsed.has_owner) {
			parsed.has_group = true;
			parsed.group = parsed.owner;
		}

		return true;
	}

	static bool parseGroupSpec(const std::string& spec, OwnerSpec& parsed) {
		if (spec.empty()) return true;

		parsed.has_group = true;
		if (resolveAccount(spec, parsed.group)) return true;

		perr("chgrp", "invalid group: " + inQuotes(spec));
		return false;
	}

	static bool takeReference(Executor& exec, OwnerJob& job) {
		Ownership reference;
		std::error_code ec;
		if (!readOwnership(toNative(exec, job.options.reference), true, reference, ec)) {
			perr(job.tool, "failed to get attributes of " + inQuotes(job.options.reference) + ": "
				+ posixErrorText(ec));
			return false;
		}

		job.target.has_group = true;
		job.target.group = reference.group;
		if (std::string(job.tool) == "chgrp") return true;

		job.target.has_owner = true;
		job.target.owner = reference.owner;
		return true;
	}

	static bool takeTarget(Executor& exec, OwnerJob& job) {
		if (!job.options.reference.empty()) return takeReference(exec, job);

		const std::string spec = job.options.operands.front();
		job.options.operands.erase(job.options.operands.begin());
		if (std::string(job.tool) == "chgrp") return parseGroupSpec(spec, job.target);
		return parseOwnerSpec(job.tool, spec, job.target);
	}

	static bool settleRecursiveDereference(const char* tool, OwnerOptions& options) {
		if (!options.recursive || options.traversal != Traversal::None) return true;
		if (options.dereference_given && options.dereference) {
			perr(tool, "-R --dereference requires either -H or -L");
			return false;
		}

		options.dereference = false;
		return true;
	}

	static bool prepareJob(Executor& exec, const std::vector<std::string>& args, OwnerJob& job) {
		if (!parseOwnerArgs(job.tool, args, job.options)) return false;
		if (!checkOperands(job.tool, job.options)) return false;
		if (!takeTarget(exec, job)) return false;
		if (job.options.has_from && !parseOwnerSpec(job.tool, job.options.from, job.from)) {
			return false;
		}

		return settleRecursiveDereference(job.tool, job.options);
	}

	static bool matchesFrom(const OwnerSpec& from, const Ownership& current) {
		if (from.has_owner && !sameAccount(from.owner, current.owner)) return false;
		return !from.has_group || sameAccount(from.group, current.group);
	}

	static std::string ownerGroupText(const OwnerSpec& spec, const Ownership& names) {
		if (spec.has_owner && spec.has_group) return names.owner.name + ":" + names.group.name;
		if (spec.has_owner) return names.owner.name;
		return spec.has_group ? names.group.name : "";
	}

	static Ownership requestedNames(const OwnerSpec& target) {
		Ownership names;
		names.owner = target.owner;
		names.group = target.group;
		return names;
	}

	static void describeChange(const OwnerJob& job, const std::string& shown,
	                           const Ownership& before, bool changed) {
		const OwnerSpec& target = job.target;
		const std::string old_text = ownerGroupText(target, before);
		const std::string new_text = ownerGroupText(target, requestedNames(target));
		const char* what = target.has_owner || !target.has_group ? "ownership" : "group";
		if (!target.has_owner && !target.has_group) {
			std::printf("ownership of %s retained\n", inQuotes(shown).c_str());
			return;
		}

		if (changed) {
			std::printf("changed %s of %s from %s to %s\n", what, inQuotes(shown).c_str(),
				old_text.c_str(), new_text.c_str());
			return;
		}

		std::printf("%s of %s retained as %s\n", what, inQuotes(shown).c_str(), new_text.c_str());
	}

	static void reportFailure(const OwnerJob& job, const std::string& shown,
	                          const Ownership& before, const std::error_code& ec) {
		const char* what = job.target.has_owner ? "ownership" : "group";
		if (!job.options.silent) {
			perr(job.tool, std::string("changing ") + what + " of " + inQuotes(shown) + ": "
				+ posixErrorText(ec));
		}

		if (job.options.report != Report::Everything) return;
		std::printf("failed to change %s of %s from %s to %s\n", what, inQuotes(shown).c_str(),
			ownerGroupText(job.target, before).c_str(),
			ownerGroupText(job.target, requestedNames(job.target)).c_str());
	}

	static bool applyTo(const OwnerJob& job, const Visit& visit) {
		Ownership before;
		std::error_code ec;
		if (!readOwnership(visit.native, visit.follow, before, ec)) {
			if (!job.options.silent) {
				perr(job.tool, "cannot access " + inQuotes(visit.shown) + ": "
					+ posixErrorText(ec));
			}

			return false;
		}

		const OwnerSpec& target = job.target;
		const bool applies = matchesFrom(job.from, before);
		OwnershipChange change;
		change.follow = visit.follow;
		if (applies && target.has_owner && !sameAccount(target.owner, before.owner)) {
			change.owner = &target.owner;
		}

		if (applies && target.has_group && !sameAccount(target.group, before.group)) {
			change.group = &target.group;
		}

		const bool changed = change.owner != nullptr || change.group != nullptr;
		if (changed && !writeOwnership(visit.native, change, ec)) {
			reportFailure(job, visit.shown, before, ec);
			return false;
		}

		const Report report = job.options.report;
		if (report == Report::Everything || (report == Report::Changes && changed)) {
			describeChange(job, visit.shown, before, changed);
		}

		return true;
	}

	static std::string childShown(const std::string& dir, const fs::path& child) {
		const std::string name = pathToUtf8(child.filename());
		if (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) return dir + name;
		return dir + "/" + name;
	}

	static bool descends(const fs::path& native, bool follow) {
		std::error_code ec;
		const fs::file_status status = follow ? fs::status(native, ec)
			: fs::symlink_status(native, ec);
		return !ec && status.type() == fs::file_type::directory;
	}

	static bool applyTree(const OwnerJob& job, const Visit& visit);

	static bool applyChildren(const OwnerJob& job, const Visit& dir) {
		std::error_code ec;
		fs::directory_iterator it(dir.native, ec);
		if (ec) {
			if (!job.options.silent) {
				perr(job.tool, "cannot read directory " + inQuotes(dir.shown) + ": "
					+ posixErrorText(ec));
			}

			return false;
		}

		bool ok = true;
		const bool follow = job.options.traversal == Traversal::All;
		for (; it != fs::directory_iterator(); it.increment(ec)) {
			const Visit child{ childShown(dir.shown, it->path()), it->path(), follow };
			if (!applyTree(job, child)) ok = false;
		}

		return ok;
	}

	static bool applyTree(const OwnerJob& job, const Visit& visit) {
		bool ok = applyTo(job, visit);
		if (!job.options.recursive || !descends(visit.native, visit.follow)) return ok;

		if (!applyChildren(job, visit)) ok = false;
		return ok;
	}

	static bool isFilesystemRoot(const fs::path& native) {
		std::error_code ec;
		const fs::path absolute = fs::weakly_canonical(native, ec);
		return !ec && absolute == absolute.root_path();
	}

	static bool refuseRoot(const OwnerJob& job, const Visit& visit) {
		const bool guarded = job.options.recursive && job.options.preserve_root;
		if (!guarded || !isFilesystemRoot(visit.native)) return false;

		perr(job.tool, "it is dangerous to operate recursively on " + inQuotes(visit.shown));
		perr(job.tool, "use --no-preserve-root to override this failsafe");
		return true;
	}

	static bool followsOperand(const OwnerOptions& options) {
		if (!options.recursive) return options.dereference;
		return options.traversal != Traversal::None;
	}

	static int runOwnerJob(Executor& exec, const OwnerJob& job) {
		const OwnershipPrivileges privileges;
		int status = 0;
		for (const std::string& operand : job.options.operands) {
			const Visit visit{ operand, toNative(exec, operand), followsOperand(job.options) };
			if (refuseRoot(job, visit) || !applyTree(job, visit)) status = 1;
		}

		std::fflush(stdout);
		return status;
	}

	static int runOwnerTool(Executor& exec, const char* tool,
	                        const std::vector<std::string>& args) {
		OwnerJob job;
		job.tool = tool;
		if (!prepareJob(exec, args, job)) return 1;
		return runOwnerJob(exec, job);
	}

	static int builtin_chown(Executor& exec, const std::vector<std::string>& args) {
		return runOwnerTool(exec, "chown", args);
	}

	static int builtin_chgrp(Executor& exec, const std::vector<std::string>& args) {
		return runOwnerTool(exec, "chgrp", args);
	}

	void registerOwnerBuiltins(Executor& exec) {
		exec.registerBuiltin("chown", builtin_chown);
		exec.registerBuiltin("chgrp", builtin_chgrp);
	}

}  // namespace wbsh

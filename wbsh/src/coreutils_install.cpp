/**
 * @file coreutils_install.cpp
 * @brief install: copy files and set attributes.
 *
 * The forms and options of GNU install. A mode maps onto the file the way
 * chmod's does: the owner write bit is the read-only attribute, the one
 * permission a Windows file carries; directories keep theirs. Modes are
 * evaluated from 0 as GNU does, so `-m 644` leaves a file writable and
 * `-m 444` or `-m +x` makes it read-only. Owner and group go through
 * fileowner.h; -s runs `strip` (or --strip-program) only when one is on
 * PATH, since a missing strip must not break `make install`.
 */

#include "coreutils_internal.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif /* _WIN32 */

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "executor.h"
#include "fileowner.h"
#include "filetimes.h"
#include "numparse.h"
#include "optscan.h"
#include "pathconv.h"

namespace wbsh {

	namespace fs = std::filesystem;

	namespace install_detail {

		enum OptId {
			kOptBackup,
			kOptSuffix,
			kOptCompare,
			kOptIgnored,
			kOptDirectory,
			kOptLeading,
			kOptGroup,
			kOptMode,
			kOptOwner,
			kOptPreserveTimes,
			kOptStrip,
			kOptStripProgram,
			kOptTargetDir,
			kOptNoTargetDir,
			kOptVerbose,
		};

		enum class Backup { None, Simple, Existing, Numbered };

		struct InstallOptions {
			bool directories = false;
			bool leading = false;
			bool compare = false;
			bool preserve_times = false;
			bool strip = false;
			bool verbose = false;
			bool no_target_dir = false;
			bool has_target_dir = false;
			bool backup_requested = false;
			bool has_owner = false;
			bool has_group = false;
			bool writable = true;
			std::string backup_control;
			std::string suffix;
			std::string target_dir;
			std::string strip_program;
			std::string mode;
			std::string owner_name;
			std::string group_name;
			std::vector<std::string> operands;
		};

		struct InstallJob {
			Executor* exec = nullptr;
			InstallOptions options;
			Backup backup = Backup::None;
			Account owner;
			Account group;
			std::string strip_path;
		};

		struct Transfer {
			std::string source;
			std::string dest;
		};

		struct FlagMember {
			int id;
			bool InstallOptions::*member;
		};

		struct ModeClause {
			unsigned who = 0;
			char op = 0;
			unsigned bits = 0;
		};

	}  // namespace install_detail

	using install_detail::Backup;
	using install_detail::FlagMember;
	using install_detail::InstallJob;
	using install_detail::InstallOptions;
	using install_detail::ModeClause;
	using install_detail::Transfer;

	static const unsigned kModeWhoAll = 0777;
	static const unsigned kOwnerWrite = 0200;
	static const unsigned long kMaxOctalMode = 07777;

	static std::string inQuotes(const std::string& text) {
		return "'" + text + "'";
	}

	static const std::vector<OptDef>& installOptionDefs() {
		static const std::vector<OptDef> defs = {
			{ install_detail::kOptBackup,        'b', nullptr,               OptArg::None },
			{ install_detail::kOptBackup,        0,   "backup",              OptArg::Optional },
			{ install_detail::kOptSuffix,        'S', "suffix",              OptArg::Required },
			{ install_detail::kOptCompare,       'C', "compare",             OptArg::None },
			{ install_detail::kOptIgnored,       'c', nullptr,               OptArg::None },
			{ install_detail::kOptIgnored,       0,   "preserve-context",    OptArg::None },
			{ install_detail::kOptIgnored,       'Z', "context",             OptArg::Optional },
			{ install_detail::kOptDirectory,     'd', "directory",           OptArg::None },
			{ install_detail::kOptLeading,       'D', nullptr,               OptArg::None },
			{ install_detail::kOptGroup,         'g', "group",               OptArg::Required },
			{ install_detail::kOptMode,          'm', "mode",                OptArg::Required },
			{ install_detail::kOptOwner,         'o', "owner",               OptArg::Required },
			{ install_detail::kOptPreserveTimes, 'p', "preserve-timestamps", OptArg::None },
			{ install_detail::kOptStrip,         's', "strip",               OptArg::None },
			{ install_detail::kOptStripProgram,  0,   "strip-program",       OptArg::Required },
			{ install_detail::kOptTargetDir,     't', "target-directory",    OptArg::Required },
			{ install_detail::kOptNoTargetDir,   'T', "no-target-directory", OptArg::None },
			{ install_detail::kOptVerbose,       'v', "verbose",             OptArg::None },
		};
		return defs;
	}

	static bool applyFlagOption(const OptHit& hit, InstallOptions& options) {
		static const FlagMember kFlags[] = {
			{ install_detail::kOptCompare,       &InstallOptions::compare },
			{ install_detail::kOptDirectory,     &InstallOptions::directories },
			{ install_detail::kOptLeading,       &InstallOptions::leading },
			{ install_detail::kOptPreserveTimes, &InstallOptions::preserve_times },
			{ install_detail::kOptStrip,         &InstallOptions::strip },
			{ install_detail::kOptNoTargetDir,   &InstallOptions::no_target_dir },
			{ install_detail::kOptVerbose,       &InstallOptions::verbose },
		};

		if (hit.id == install_detail::kOptIgnored) return true;
		for (const FlagMember& flag : kFlags) {
			if (flag.id != hit.id) continue;
			options.*flag.member = true;
			return true;
		}

		return false;
	}

	static void applyValueOption(const OptHit& hit, InstallOptions& options) {
		switch (hit.id) {
			case install_detail::kOptBackup:
				options.backup_requested = true;
				if (hit.has_value) options.backup_control = hit.value;
				break;
			case install_detail::kOptSuffix:
				options.backup_requested = true;
				options.suffix = hit.value;
				break;
			case install_detail::kOptGroup:
				options.has_group = true;
				options.group_name = hit.value;
				break;
			case install_detail::kOptOwner:
				options.has_owner = true;
				options.owner_name = hit.value;
				break;
			case install_detail::kOptTargetDir:
				options.has_target_dir = true;
				options.target_dir = hit.value;
				break;
			case install_detail::kOptStripProgram:
				options.strip_program = hit.value;
				break;
			default:
				options.mode = hit.value;
				break;
		}
	}

	static bool parseInstallArgs(const std::vector<std::string>& args, InstallOptions& options) {
		OptScanResult scanned;
		if (!scanOptions("install", args, installOptionDefs(), scanned)) return false;

		for (const OptHit& hit : scanned.hits) {
			if (!applyFlagOption(hit, options)) applyValueOption(hit, options);
		}

		options.operands = scanned.operands;
		return true;
	}

	static unsigned whoBits(char c) {
		switch (c) {
			case 'u':
				return 04700;
			case 'g':
				return 02070;
			case 'o':
				return 01007;
			case 'a':
				return 07777;
			default:
				return 0;
		}
	}

	static unsigned permissionBits(char c, unsigned mode) {
		switch (c) {
			case 'r':
				return 0444;
			case 'w':
				return 0222;
			case 'x':
				return 0111;
			case 'X':
				return (mode & 0111) != 0 ? 0111 : 0;
			case 's':
				return 06000;
			case 't':
				return 01000;
			default:
				return 0;
		}
	}

	static unsigned copiedBits(char c, unsigned mode) {
		const unsigned shift = c == 'u' ? 6 : c == 'g' ? 3 : 0;
		const unsigned rwx = (mode >> shift) & 07;
		return rwx * 0111;
	}

	static unsigned applyClause(const ModeClause& clause, unsigned mode) {
		const unsigned affected = (clause.who == 0 ? kModeWhoAll : clause.who) & clause.bits;
		if (clause.op == '+') return mode | affected;
		if (clause.op == '-') return mode & ~affected;

		const unsigned cleared = mode & ~(clause.who == 0 ? 07777u : clause.who);
		return cleared | affected;
	}

	static bool readClauseBits(const std::string& text, std::size_t& at, unsigned mode,
	                           unsigned& bits) {
		bits = 0;
		const bool copies = at < text.size() && whoBits(text[at]) != 0 && text[at] != 'a';
		if (copies) {
			bits = copiedBits(text[at++], mode);
			return true;
		}

		for (; at < text.size() && text[at] != ',' && text[at] != '+' && text[at] != '-'
				&& text[at] != '='; ++at) {
			const unsigned one = permissionBits(text[at], mode);
			if (one == 0 && text[at] != 'X') return false;
			bits |= one;
		}

		return true;
	}

	static bool applySymbolicPart(const std::string& text, std::size_t& at, unsigned& mode) {
		ModeClause clause;
		for (; at < text.size() && whoBits(text[at]) != 0; ++at) clause.who |= whoBits(text[at]);
		if (at >= text.size() || std::strchr("+-=", text[at]) == nullptr) return false;

		while (at < text.size() && std::strchr("+-=", text[at]) != nullptr) {
			clause.op = text[at++];
			if (!readClauseBits(text, at, mode, clause.bits)) return false;
			mode = applyClause(clause, mode);
		}

		return at >= text.size() || text[at] == ',';
	}

	static bool evaluateSymbolicMode(const std::string& text, unsigned& mode) {
		mode = 0;
		std::size_t at = 0;
		while (at < text.size()) {
			if (!applySymbolicPart(text, at, mode)) return false;
			if (at < text.size()) ++at;
		}

		return !text.empty() && text.back() != ',';
	}

	static bool evaluateMode(const std::string& text, unsigned& mode) {
		unsigned long octal = 0;
		std::size_t used = 0;
		const bool digits = !text.empty()
			&& text.find_first_not_of("01234567") == std::string::npos;
		if (digits) {
			if (!parseUL(text, octal, 8, &used) || octal > kMaxOctalMode) return false;
			mode = static_cast<unsigned>(octal);
			return true;
		}

		return evaluateSymbolicMode(text, mode);
	}

	bool parseFileMode(const std::string& text, unsigned& mode) {
		return evaluateMode(text, mode);
	}

	static bool settleMode(InstallOptions& options) {
		if (options.mode.empty()) return true;

		unsigned mode = 0;
		if (!evaluateMode(options.mode, mode)) {
			perr("install", "invalid mode " + inQuotes(options.mode));
			return false;
		}

		options.writable = (mode & kOwnerWrite) != 0;
		return true;
	}

	static bool matchesControl(const std::string& given, const char* name) {
		return !given.empty() && std::string(name).compare(0, given.size(), given) == 0;
	}

	static bool backupFromControl(const std::string& control, Backup& backup) {
		if (matchesControl(control, "none") || matchesControl(control, "off")) {
			backup = Backup::None;
		} else if (matchesControl(control, "simple") || matchesControl(control, "never")) {
			backup = Backup::Simple;
		} else if (matchesControl(control, "existing") || matchesControl(control, "nil")) {
			backup = Backup::Existing;
		} else if (matchesControl(control, "numbered") || control == "t") {
			backup = Backup::Numbered;
		} else {
			return false;
		}

		return true;
	}

	static bool settleBackup(Executor& exec, InstallJob& job) {
		InstallOptions& options = job.options;
		if (options.suffix.empty()) options.suffix = exec.env().get("SIMPLE_BACKUP_SUFFIX");
		if (options.suffix.empty()) options.suffix = "~";
		if (!options.backup_requested) return true;

		std::string control = options.backup_control;
		if (control.empty()) control = exec.env().get("VERSION_CONTROL");
		if (control.empty()) control = "existing";
		if (backupFromControl(control, job.backup)) return true;

		perr("install", "invalid argument " + inQuotes(control) + " for 'backup type'");
		return false;
	}

	static bool settleAccounts(InstallJob& job) {
		const InstallOptions& options = job.options;
		if (options.has_owner && !resolveAccount(options.owner_name, job.owner)) {
			perr("install", "invalid user " + inQuotes(options.owner_name));
			return false;
		}

		if (options.has_group && !resolveAccount(options.group_name, job.group)) {
			perr("install", "invalid group " + inQuotes(options.group_name));
			return false;
		}

		return true;
	}

	static bool settleStrip(Executor& exec, InstallJob& job) {
		const InstallOptions& options = job.options;
		if (!options.strip) return true;

		const std::string program = options.strip_program.empty() ? "strip" : options.strip_program;
		job.strip_path = exec.findExecutable(program);
		if (!job.strip_path.empty() || options.strip_program.empty()) return true;

		perr("install", "cannot run " + inQuotes(program) + ": No such file or directory");
		return false;
	}

	static bool prepareInstall(Executor& exec, const std::vector<std::string>& args,
	                           InstallJob& job) {
		job.exec = &exec;
		if (!parseInstallArgs(args, job.options)) return false;
		if (job.options.directories && job.options.has_target_dir) {
			perr("install", "target directory not allowed when installing a directory");
			return false;
		}

		return settleMode(job.options) && settleBackup(exec, job) && settleAccounts(job)
			&& settleStrip(exec, job);
	}

	static bool isSeparator(char c) {
		return c == '/' || c == '\\';
	}

	static std::vector<std::string> pathPrefixes(const std::string& path) {
		std::vector<std::string> prefixes;
		for (std::size_t at = 1; at <= path.size(); ++at) {
			const bool boundary = at == path.size() || isSeparator(path[at]);
			if (boundary && !isSeparator(path[at - 1])) prefixes.push_back(path.substr(0, at));
		}

		return prefixes;
	}

	static bool createOneDirectory(const InstallJob& job, const std::string& shown) {
		const fs::path native = toNative(*job.exec, shown);
		std::error_code ec;
		if (fs::is_directory(native, ec)) return true;

		std::error_code probe;
		if (!fs::create_directory(native, ec) && !fs::is_directory(native, probe)) {
			if (!ec) ec = std::make_error_code(std::errc::file_exists);
			perr("install", "cannot create directory " + inQuotes(shown) + ": "
				+ posixErrorText(ec));
			return false;
		}

		if (job.options.verbose) {
			std::printf("install: creating directory %s\n", inQuotes(shown).c_str());
		}

		return true;
	}

	static bool createDirectoryChain(const InstallJob& job, const std::string& path) {
		for (const std::string& prefix : pathPrefixes(path)) {
			if (!createOneDirectory(job, prefix)) return false;
		}

		return true;
	}

	static std::string parentOf(const std::string& path) {
		std::size_t end = path.size();
		while (end > 0 && isSeparator(path[end - 1])) --end;
		while (end > 0 && !isSeparator(path[end - 1])) --end;
		while (end > 1 && isSeparator(path[end - 1])) --end;
		return path.substr(0, end);
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

	static bool applyOwnership(const InstallJob& job, const fs::path& native,
	                           const std::string& shown) {
		if (!job.options.has_owner && !job.options.has_group) return true;

		Ownership current;
		std::error_code ec;
		OwnershipChange change;
		const bool known = readOwnership(native, true, current, ec);
		const bool owner_differs = !known || !sameAccount(current.owner, job.owner);
		const bool group_differs = !known || !sameAccount(current.group, job.group);
		if (job.options.has_owner && owner_differs) change.owner = &job.owner;
		if (job.options.has_group && group_differs) change.group = &job.group;
		if (writeOwnership(native, change, ec)) return true;

		perr("install", "cannot change ownership of " + inQuotes(shown) + ": "
			+ posixErrorText(ec));
		return false;
	}

	static bool installDirectories(const InstallJob& job) {
		if (job.options.operands.empty()) {
			perr("install", "missing file operand");
			return false;
		}

		bool ok = true;
		for (const std::string& dir : job.options.operands) {
			if (!createDirectoryChain(job, dir)) {
				ok = false;
				continue;
			}

			if (!applyOwnership(job, toNative(*job.exec, dir), dir)) ok = false;
		}

		return ok;
	}

	static bool isDirectoryOperand(const InstallJob& job, const std::string& operand) {
		std::error_code ec;
		return fs::is_directory(toNative(*job.exec, operand), ec);
	}

	static bool planIntoDirectory(const InstallJob& job, const std::string& dir,
	                              const std::vector<std::string>& sources,
	                              std::vector<Transfer>& plan) {
		if (!isDirectoryOperand(job, dir)) {
			perr("install", "target " + inQuotes(dir) + " is not a directory");
			return false;
		}

		for (const std::string& source : sources) {
			plan.push_back({ source, joinShown(dir, baseName(source)) });
		}

		return true;
	}

	static bool planWithTargetDirectory(const InstallJob& job, std::vector<Transfer>& plan) {
		const InstallOptions& options = job.options;
		if (options.no_target_dir) {
			perr("install", "cannot combine --target-directory (-t) and "
				"--no-target-directory (-T)");
			return false;
		}

		if (options.operands.empty()) {
			perr("install", "missing file operand");
			return false;
		}

		if (options.leading && !createDirectoryChain(job, options.target_dir)) return false;
		return planIntoDirectory(job, options.target_dir, options.operands, plan);
	}

	static bool checkTwoOrMore(const std::vector<std::string>& operands) {
		if (operands.empty()) {
			perr("install", "missing file operand");
			return false;
		}

		if (operands.size() == 1) {
			perr("install", "missing destination file operand after " + inQuotes(operands[0]));
			return false;
		}

		return true;
	}

	static bool planTransfers(const InstallJob& job, std::vector<Transfer>& plan) {
		const InstallOptions& options = job.options;
		if (options.has_target_dir) return planWithTargetDirectory(job, plan);
		if (!checkTwoOrMore(options.operands)) return false;

		if (options.no_target_dir && options.operands.size() > 2) {
			perr("install", "extra operand " + inQuotes(options.operands[2]));
			return false;
		}

		const std::string& dest = options.operands.back();
		const std::vector<std::string> sources(options.operands.begin(),
			options.operands.end() - 1);
		const bool into_dir = !options.no_target_dir
			&& (sources.size() > 1 || isDirectoryOperand(job, dest));
		if (into_dir) return planIntoDirectory(job, dest, sources, plan);

		plan.push_back({ sources[0], dest });
		return true;
	}

	static bool checkSource(const Transfer& transfer, const fs::path& native) {
		std::error_code ec;
		const fs::file_status status = fs::status(native, ec);
		if (ec || !fs::exists(status)) {
			if (!ec) ec = std::make_error_code(std::errc::no_such_file_or_directory);
			perr("install", "cannot stat " + inQuotes(transfer.source) + ": " + posixErrorText(ec));
			return false;
		}

		if (!fs::is_directory(status)) return true;

		perr("install", "omitting directory " + inQuotes(transfer.source));
		return false;
	}

	static bool sameFile(const fs::path& a, const fs::path& b) {
		FileIdentity first;
		FileIdentity second;
		return queryFileIdentity(a, first) && queryFileIdentity(b, second) && first == second;
	}

	static bool sameContents(const fs::path& a, const fs::path& b) {
		std::error_code ec;
		if (fs::file_size(a, ec) != fs::file_size(b, ec) || ec) return false;

		std::FILE* first = openUtf8(pathToUtf8(a), "rb");
		std::FILE* second = openUtf8(pathToUtf8(b), "rb");
		bool same = first != nullptr && second != nullptr;
		char left[16384];
		char right[16384];
		while (same) {
			const std::size_t got = std::fread(left, 1, sizeof(left), first);
			same = std::fread(right, 1, sizeof(right), second) == got
				&& std::memcmp(left, right, got) == 0;
			if (got == 0) break;
		}

		if (first != nullptr) std::fclose(first);
		if (second != nullptr) std::fclose(second);
		return same;
	}

	static bool isReadOnly(const fs::path& native) {
#ifdef _WIN32
		const DWORD attributes = ::GetFileAttributesW(longPathSafe(native).c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0;
#else
		static_cast<void>(native);
		return false;
#endif /* _WIN32 */
	}

	static bool ownershipMatches(const InstallJob& job, const fs::path& native) {
		if (!job.options.has_owner && !job.options.has_group) return true;

		Ownership current;
		std::error_code ec;
		if (!readOwnership(native, true, current, ec)) return false;
		if (job.options.has_owner && !sameAccount(current.owner, job.owner)) return false;
		return !job.options.has_group || sameAccount(current.group, job.group);
	}

	static bool unchangedTarget(const InstallJob& job, const fs::path& source,
	                            const fs::path& dest) {
		std::error_code ec;
		if (!job.options.compare || !fs::is_regular_file(dest, ec)) return false;
		if (isReadOnly(dest) == job.options.writable) return false;
		return ownershipMatches(job, dest) && sameContents(source, dest);
	}

	static std::string numberedBackupName(const std::string& dest, const fs::path& native,
	                                      bool& found_any) {
		const std::string prefix = pathToUtf8(native.filename()) + ".~";
		unsigned long highest = 0;
		std::error_code ec;
		fs::path dir = native.parent_path();
		if (dir.empty()) dir = ".";
		fs::directory_iterator it(dir, ec);
		for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
			const std::string name = pathToUtf8(it->path().filename());
			if (name.compare(0, prefix.size(), prefix) != 0 || name.back() != '~') continue;

			unsigned long number = 0;
			const std::string digits = name.substr(prefix.size(), name.size() - prefix.size() - 1);
			if (!parseUL(digits, number)) continue;
			found_any = true;
			if (number > highest) highest = number;
		}

		return dest + ".~" + std::to_string(highest + 1) + "~";
	}

	static std::string backupName(const InstallJob& job, const std::string& dest,
	                              const fs::path& native) {
		if (job.backup == Backup::Simple) return dest + job.options.suffix;

		bool found_any = false;
		const std::string numbered = numberedBackupName(dest, native, found_any);
		if (job.backup == Backup::Numbered || found_any) return numbered;
		return dest + job.options.suffix;
	}

	static bool clearReadOnly(const fs::path& native) {
#ifdef _WIN32
		const std::wstring wide = longPathSafe(native).wstring();
		const DWORD attributes = ::GetFileAttributesW(wide.c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES) return false;
		if ((attributes & FILE_ATTRIBUTE_READONLY) == 0) return true;
		return ::SetFileAttributesW(wide.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY) != 0;
#else
		static_cast<void>(native);
		return true;
#endif /* _WIN32 */
	}

	static bool moveAside(const InstallJob& job, const Transfer& transfer, const fs::path& dest,
	                      std::string& backup) {
		std::error_code ec;
		if (job.backup == Backup::None || !fs::exists(dest, ec)) return true;

		backup = backupName(job, transfer.dest, dest);
		const fs::path native = toNative(*job.exec, backup);
		if (fs::exists(native, ec)) {
			clearReadOnly(native);
			fs::remove(native, ec);
		}

		fs::rename(dest, native, ec);
		if (!ec) return true;

		perr("install", "cannot backup " + inQuotes(transfer.dest) + ": " + posixErrorText(ec));
		return false;
	}

	static bool copyContents(const Transfer& transfer, const fs::path& source,
	                         const fs::path& dest) {
		std::error_code ec;
		if (fs::exists(dest, ec)) {
			clearReadOnly(dest);
			fs::remove(dest, ec);
		}

#ifdef _WIN32
		const std::wstring from = longPathSafe(source).wstring();
		const std::wstring to = longPathSafe(dest).wstring();
		if (::CopyFileW(from.c_str(), to.c_str(), FALSE) != 0) return true;
		ec = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
#else
		if (fs::copy_file(source, dest, fs::copy_options::overwrite_existing, ec)) return true;
#endif /* _WIN32 */
		perr("install", "cannot create regular file " + inQuotes(transfer.dest) + ": "
			+ posixErrorText(ec));
		return false;
	}

	static void stampTimes(const InstallJob& job, const fs::path& source, const fs::path& dest) {
		std::error_code ec;
		if (!job.options.preserve_times) {
			fs::last_write_time(dest, fs::file_time_type::clock::now(), ec);
			return;
		}

		FileTimes times;
		if (readFileTimes(source, true, times)) writeFileTimes(dest, &times.access, &times.modify);
	}

	static bool stripTarget(const InstallJob& job, const fs::path& dest) {
		if (job.strip_path.empty()) return true;

		const std::string target = pathToUtf8(dest);
		const int status = job.exec->runExternal({ job.strip_path, target }, {});
		if (status == 0) return true;

		perr("install", "strip process terminated abnormally");
		return false;
	}

	static bool setWritable(const fs::path& native, bool writable) {
#ifdef _WIN32
		const std::wstring wide = longPathSafe(native).wstring();
		DWORD attributes = ::GetFileAttributesW(wide.c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES) return false;

		if (writable) attributes &= ~FILE_ATTRIBUTE_READONLY;
		else          attributes |= FILE_ATTRIBUTE_READONLY;
		return ::SetFileAttributesW(wide.c_str(), attributes) != 0;
#else
		static_cast<void>(native);
		static_cast<void>(writable);
		return true;
#endif /* _WIN32 */
	}

	static void announce(const Transfer& transfer, const std::string& backup) {
		std::printf("%s -> %s", inQuotes(transfer.source).c_str(), inQuotes(transfer.dest).c_str());
		if (!backup.empty()) std::printf(" (backup: %s)", inQuotes(backup).c_str());
		std::printf("\n");
	}

	static bool prepareDestination(const InstallJob& job, const Transfer& transfer,
	                               const fs::path& source, const fs::path& dest) {
		if (sameFile(source, dest)) {
			perr("install", inQuotes(transfer.source) + " and " + inQuotes(transfer.dest)
				+ " are the same file");
			return false;
		}

		std::error_code ec;
		if (fs::is_directory(fs::symlink_status(dest, ec))) {
			perr("install", "cannot overwrite directory " + inQuotes(transfer.dest)
				+ " with non-directory");
			return false;
		}

		const std::string parent = parentOf(transfer.dest);
		const bool needs_parent = job.options.leading && !job.options.has_target_dir;
		return !needs_parent || parent.empty() || createDirectoryChain(job, parent);
	}

	static bool installFile(const InstallJob& job, const Transfer& transfer) {
		const fs::path source = toNative(*job.exec, transfer.source);
		const fs::path dest = toNative(*job.exec, transfer.dest);
		if (!checkSource(transfer, source)) return false;
		if (!prepareDestination(job, transfer, source, dest)) return false;
		if (unchangedTarget(job, source, dest)) return true;

		std::string backup;
		if (!moveAside(job, transfer, dest, backup)) return false;
		if (!copyContents(transfer, source, dest)) return false;

		bool ok = stripTarget(job, dest);
		stampTimes(job, source, dest);
		if (!setWritable(dest, job.options.writable)) ok = false;
		if (!applyOwnership(job, dest, transfer.dest)) ok = false;
		if (job.options.verbose) announce(transfer, backup);
		return ok;
	}

	static bool installFiles(const InstallJob& job) {
		std::vector<Transfer> plan;
		if (!planTransfers(job, plan)) return false;

		bool ok = true;
		for (const Transfer& transfer : plan) {
			if (!installFile(job, transfer)) ok = false;
		}

		return ok;
	}

	static int builtin_install(Executor& exec, const std::vector<std::string>& args) {
		InstallJob job;
		if (!prepareInstall(exec, args, job)) return 1;

		const OwnershipPrivileges privileges;
		const bool ok = job.options.directories ? installDirectories(job) : installFiles(job);
		std::fflush(stdout);
		return ok ? 0 : 1;
	}

	void registerInstallBuiltin(Executor& exec) {
		exec.registerBuiltin("install", builtin_install);
	}

}  // namespace wbsh

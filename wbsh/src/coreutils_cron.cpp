/**
 * @file coreutils_cron.cpp
 * @brief crontab: install, list, edit, check and remove the user's crontab; registers crond too.
 *
 * Messages and exit statuses follow cronie, -c DIR comes from busybox.
 * crond runs every job as the account it runs under, so -u only takes
 * the current user.
 */

#include "coreutils_internal.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "cron_internal.h"
#include "cron_schedule.h"
#include "executor.h"
#include "numparse.h"
#include "optscan.h"
#include "pathconv.h"

namespace wbsh {

	namespace fs = std::filesystem;

	namespace cron_detail {

		enum CrontabOptionId {
			kCrontabUser = 1,
			kCrontabList,
			kCrontabEdit,
			kCrontabRemove,
			kCrontabAsk,
			kCrontabDirectory,
			kCrontabTest,
			kCrontabNextRuns,
			kCrontabHelp,
		};

		enum class CrontabAction { Replace, List, Edit, Remove, Test, NextRuns, Help };

		enum class EditResult { Installed, Unchanged, Failed, Invalid };

		struct CrontabOptions {
			CrontabAction action = CrontabAction::Replace;
			bool action_given = false;
			bool ask = false;
			std::string user;
			std::string directory;
			std::string test_file;
			std::vector<std::string> operands;
		};

		struct CrontabTarget {
			std::string user;
			fs::path path;
		};

		static const char* const kWeekdayAbbreviations[] = {
			"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat",
		};

		static const int kDefaultRunCount = 5;
		static const int kSecondsPerMinute = 60;
		static const int kMinutesPerHour = 60;

	}  // namespace cron_detail

	using cron_detail::CrontabAction;
	using cron_detail::CrontabOptions;
	using cron_detail::CrontabTarget;
	using cron_detail::EditResult;

	static void printCrontabUsage(std::FILE* stream) {
		std::fputs(
			"usage: crontab [-c DIR] [-u USER] FILE | -\n"
			"       crontab [-c DIR] [-u USER] -l | -e | -r [-i]\n"
			"       crontab -T FILE\n"
			"\n"
			"  FILE, -    install FILE (- for standard input) as the crontab\n"
			"  -l         list the crontab\n"
			"  -e         edit it with $VISUAL, $EDITOR or vi\n"
			"  -r         remove it; with -i, ask first\n"
			"  -T FILE    only check the syntax of FILE\n"
			"  -u USER    whose crontab (only the current user's on Windows)\n"
			"  -c DIR     crontab directory (default %APPDATA%\\wbsh\\crontabs)\n", stream);
	}

	static bool usageError(const std::string& message) {
		perr("crontab", "usage error: " + message);
		printCrontabUsage(stderr);
		return false;
	}

	static const std::vector<OptDef>& crontabOptionDefs() {
		static const std::vector<OptDef> defs = {
			{ cron_detail::kCrontabUser,      'u', nullptr,     OptArg::Required },
			{ cron_detail::kCrontabList,      'l', nullptr,     OptArg::None     },
			{ cron_detail::kCrontabEdit,      'e', nullptr,     OptArg::None     },
			{ cron_detail::kCrontabRemove,    'r', nullptr,     OptArg::None     },
			{ cron_detail::kCrontabAsk,       'i', nullptr,     OptArg::None     },
			{ cron_detail::kCrontabDirectory, 'c', nullptr,     OptArg::Required },
			{ cron_detail::kCrontabTest,      'T', nullptr,     OptArg::Required },
			{ cron_detail::kCrontabNextRuns,  0,   "next-runs", OptArg::None     },
			{ cron_detail::kCrontabHelp,      0,   "help",      OptArg::None     },
		};
		return defs;
	}

	static bool chooseAction(CrontabOptions& options, CrontabAction action) {
		if (options.action_given && options.action != action) {
			return usageError("only one operation permitted");
		}

		options.action = action;
		options.action_given = true;
		return true;
	}

	static CrontabAction actionOfOption(int id) {
		switch (id) {
			case cron_detail::kCrontabList:     return CrontabAction::List;
			case cron_detail::kCrontabEdit:     return CrontabAction::Edit;
			case cron_detail::kCrontabRemove:   return CrontabAction::Remove;
			case cron_detail::kCrontabNextRuns: return CrontabAction::NextRuns;
			case cron_detail::kCrontabHelp:     return CrontabAction::Help;
			default:                            return CrontabAction::Replace;
		}
	}

	static bool applyHit(const OptHit& hit, CrontabOptions& options) {
		switch (hit.id) {
			case cron_detail::kCrontabUser:
				options.user = hit.value;
				return true;
			case cron_detail::kCrontabDirectory:
				options.directory = hit.value;
				return true;
			case cron_detail::kCrontabAsk:
				options.ask = true;
				return true;
			case cron_detail::kCrontabTest:
				options.test_file = hit.value;
				return chooseAction(options, CrontabAction::Test);
			default:
				return chooseAction(options, actionOfOption(hit.id));
		}
	}

	static bool checkOperands(const CrontabOptions& options) {
		const std::size_t count = options.operands.size();
		if (options.action == CrontabAction::NextRuns || options.action == CrontabAction::Help) {
			return true;
		}

		if (options.action != CrontabAction::Replace) {
			return count == 0 || usageError("no arguments permitted after this option");
		}

		if (count == 0) return usageError("file name or - (for stdin) must be specified");
		return count == 1 || usageError("only one file can be installed");
	}

	static bool parseCrontabArgs(const std::vector<std::string>& args, CrontabOptions& options) {
		OptScanResult scanned;
		if (!scanOptions("crontab", args, crontabOptionDefs(), scanned)) {
			printCrontabUsage(stderr);
			return false;
		}

		for (const OptHit& hit : scanned.hits) {
			if (!applyHit(hit, options)) return false;
		}

		options.operands = scanned.operands;
		return checkOperands(options);
	}

	static std::string lowerAscii(std::string text) {
		for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return text;
	}

	static bool resolveTarget(Executor& exec, const CrontabOptions& options,
	                          CrontabTarget& target) {
		target.user = currentUserName(exec);
		if (!options.user.empty() && lowerAscii(options.user) != lowerAscii(target.user)) {
			perr("crontab", "must be privileged to use -u");
			return false;
		}

		target.path = crontabDirectory(exec, options.directory) / utf8ToPath(target.user);
		return true;
	}

	static void reportFileError(const std::string& name, const std::error_code& ec) {
		perr("crontab", name + ": " + posixErrorText(ec));
	}

	static bool isMissing(const std::error_code& ec) {
		return ec == std::errc::no_such_file_or_directory;
	}

	static std::string readStream(std::FILE* stream) {
		std::string text;
		char buffer[4096];
		std::size_t got = 0;
		while ((got = std::fread(buffer, 1, sizeof(buffer), stream)) > 0) text.append(buffer, got);
		return text;
	}

	static bool readInput(Executor& exec, const std::string& operand, std::string& text) {
		if (operand == "-") {
			text = readStream(stdin);
			return true;
		}

		std::error_code ec;
		if (readWholeFile(toNative(exec, operand), text, ec)) return true;

		reportFileError(operand, ec);
		return false;
	}

	static void reportSyntaxErrors(const std::string& name, const CronTable& table) {
		for (const CronSyntaxError& error : table.errors) {
			std::fprintf(stderr, "\"%s\":%zu: %s\n", name.c_str(), error.line,
				error.message.c_str());
		}

		std::fflush(stderr);
	}

	static std::string withFinalNewline(std::string text) {
		if (!text.empty() && text.back() != '\n') text.push_back('\n');
		return text;
	}

	static bool installCrontab(const CrontabTarget& target, const std::string& text) {
		std::error_code ec;
		if (writeWholeFile(target.path, withFinalNewline(text), ec)) return true;

		reportFileError(pathToUtf8(target.path), ec);
		return false;
	}

	static int replaceCrontab(Executor& exec, const CrontabTarget& target,
	                          const std::string& operand) {
		std::string text;
		if (!readInput(exec, operand, text)) return 1;

		const CronTable table = parseCrontab(text);
		if (!table.errors.empty()) {
			reportSyntaxErrors(operand, table);
			std::fputs("errors in crontab file, can't install.\n", stderr);
			return 1;
		}

		return installCrontab(target, text) ? 0 : 1;
	}

	static int reportNoCrontab(const CrontabTarget& target) {
		perr("crontab", "no crontab for " + target.user);
		return 1;
	}

	static int listCrontab(const CrontabTarget& target) {
		std::string text;
		std::error_code ec;
		if (!readWholeFile(target.path, text, ec)) {
			if (isMissing(ec)) return reportNoCrontab(target);
			reportFileError(pathToUtf8(target.path), ec);
			return 1;
		}

		std::fwrite(text.data(), 1, text.size(), stdout);
		std::fflush(stdout);
		return 0;
	}

	static bool readAnswer(char& answer) {
		char line[256] = {};
		if (std::fgets(line, sizeof(line), stdin) == nullptr) return false;

		answer = static_cast<char>(std::tolower(static_cast<unsigned char>(line[0])));
		return true;
	}

	static bool askYesNo(const std::string& question, const char* again) {
		std::fflush(stdout);
		std::fputs(question.c_str(), stderr);
		for (;;) {
			std::fflush(stderr);
			char answer = 0;
			if (!readAnswer(answer)) return false;
			if (answer == 'y' || answer == 'n') return answer == 'y';
			std::fputs(again, stderr);
		}
	}

	static int removeCrontab(const CrontabTarget& target, bool ask) {
		std::error_code ec;
		if (!fs::exists(target.path, ec)) return reportNoCrontab(target);

		const std::string question = "crontab: really delete " + target.user
			+ "'s crontab? (y/n) ";
		if (ask && !askYesNo(question, "Please enter Y or N: ")) return 0;

		if (fs::remove(target.path, ec)) return 0;
		if (!ec || isMissing(ec)) return reportNoCrontab(target);

		reportFileError(pathToUtf8(target.path), ec);
		return 1;
	}

	static bool loadForEditing(const CrontabTarget& target, std::string& text) {
		std::error_code ec;
		if (readWholeFile(target.path, text, ec)) return true;

		if (isMissing(ec)) {
			perr("crontab", "no crontab for " + target.user + " - using an empty one");
			text.clear();
			return true;
		}

		reportFileError(pathToUtf8(target.path), ec);
		return false;
	}

	static bool createEditFile(Executor& exec, const std::string& text, fs::path& temp) {
		const fs::path directory = shellTempDirectory(exec);
		std::FILE* file = createSiblingTempFile(directory / "crontab", "crontab.", temp);
		if (file == nullptr) {
			perr("crontab", "cannot create a temporary file for editing");
			return false;
		}

		const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
		const bool closed = std::fclose(file) == 0;
		if (written && closed) return true;

		perr("crontab", "cannot write " + pathToUtf8(temp));
		std::error_code ec;
		fs::remove(temp, ec);
		return false;
	}

	static std::string editorCommand(Executor& exec) {
		for (const char* name : { "VISUAL", "EDITOR" }) {
			const std::string value = exec.env().get(name);
			if (!value.empty()) return value;
		}

		return "vi";
	}

	static int runEditor(Executor& exec, const fs::path& file) {
		const std::string editor = editorCommand(exec);
		const std::string posix_path = exec.pathConv().toPosix(pathToUtf8(file));
		const int status = exec.executeText(editor + " " + quoteShellArgv({ posix_path }),
			"crontab");
		exec.clearFlow();
		std::fflush(stdout);
		if (status == 0) return 0;

		perr("crontab", "\"" + editor + "\" exited with status " + std::to_string(status));
		return status;
	}

	static void discardEditFile(const fs::path& temp) {
		std::error_code ec;
		fs::remove(temp, ec);
	}

	static std::string shownPath(Executor& exec, const fs::path& path) {
		return exec.pathConv().toPosix(pathToUtf8(path));
	}

	static EditResult takeEdit(Executor& exec, const CrontabTarget& target, const fs::path& temp,
	                           const std::string& original) {
		std::string edited;
		std::error_code ec;
		if (runEditor(exec, temp) != 0 || !readWholeFile(temp, edited, ec)) {
			return EditResult::Failed;
		}

		if (edited == original) {
			perr("crontab", "no changes made to crontab");
			return EditResult::Unchanged;
		}

		perr("crontab", "installing new crontab");
		const CronTable table = parseCrontab(edited);
		if (table.errors.empty()) {
			return installCrontab(target, edited) ? EditResult::Installed : EditResult::Failed;
		}

		reportSyntaxErrors(shownPath(exec, temp), table);
		std::fputs("errors in crontab file, can't install.\n", stderr);
		return EditResult::Invalid;
	}

	// cronie's loop: an edit with errors can be taken up again, as often
	// as the user likes, before the edits are left behind for them.
	static int editUntilInstalled(Executor& exec, const CrontabTarget& target,
	                              const fs::path& temp, const std::string& original) {
		EditResult result = takeEdit(exec, target, temp, original);
		while (result == EditResult::Invalid) {
			if (!askYesNo("Do you want to retry the same edit? (y/n) ", "Enter Y or N\n")) {
				perr("crontab", "edits left in " + shownPath(exec, temp));
				return 1;
			}

			result = takeEdit(exec, target, temp, original);
		}

		discardEditFile(temp);
		return result == EditResult::Failed ? 1 : 0;
	}

	static int editCrontab(Executor& exec, const CrontabTarget& target) {
		std::string original;
		if (!loadForEditing(target, original)) return 1;

		fs::path temp;
		if (!createEditFile(exec, original, temp)) return 1;

		return editUntilInstalled(exec, target, temp, original);
	}

	static int testCrontab(Executor& exec, const std::string& file) {
		std::string text;
		if (!readInput(exec, file, text)) return 1;

		const CronTable table = parseCrontab(text);
		reportSyntaxErrors(file, table);
		if (!table.errors.empty()) {
			std::fputs("Invalid crontab file. Syntax issues were found.\n", stderr);
			return 1;
		}

		std::fputs("No syntax issues were found in the crontab file.\n", stderr);
		return 0;
	}

	static void printRun(const CronZone& zone, std::int64_t run) {
		const CivilTime civil = zone.civilAt(run);
		const std::int64_t offset_minutes = (secondsFromCivil(civil) - run)
			/ cron_detail::kSecondsPerMinute;
		const std::int64_t magnitude = offset_minutes < 0 ? -offset_minutes : offset_minutes;
		std::printf("%04d-%02d-%02d %02d:%02d %s %c%02lld%02lld\n", civil.year, civil.month,
			civil.day, civil.hour, civil.minute,
			cron_detail::kWeekdayAbbreviations[weekdayOf(civil)], offset_minutes < 0 ? '-' : '+',
			static_cast<long long>(magnitude / cron_detail::kMinutesPerHour),
			static_cast<long long>(magnitude % cron_detail::kMinutesPerHour));
	}

	static std::unique_ptr<CronZone> zoneOperand(const std::vector<std::string>& operands) {
		if (operands.size() < 4) return localCronZone();

		std::unique_ptr<CronZone> zone = namedCronZone(operands[3]);
		if (zone == nullptr) perr("crontab", "unknown time zone: " + operands[3]);
		return zone;
	}

	static bool parseRunCount(const std::vector<std::string>& operands, int& count) {
		count = cron_detail::kDefaultRunCount;
		if (operands.size() < 3) return true;

		std::size_t used = 0;
		if (parseInt(operands[2], count, 10, &used) && used == operands[2].size() && count > 0) {
			return true;
		}

		perr("crontab", "invalid count: " + operands[2]);
		return false;
	}

	static bool parseNextRunsArgs(const std::vector<std::string>& operands,
	                              CronSchedule& schedule, CivilTime& start, int& count) {
		if (operands.size() < 2 || operands.size() > 4) {
			perr("crontab", "usage: crontab --next-runs SPEC 'YYYY-MM-DD HH:MM' [COUNT [ZONE]]");
			return false;
		}

		std::string error;
		if (!parseCronSchedule(operands[0], schedule, error)) {
			perr("crontab", operands[0] + ": " + error);
			return false;
		}

		if (!parseCivilMinute(operands[1], start)) {
			perr("crontab", "invalid time: " + operands[1]);
			return false;
		}

		return parseRunCount(operands, count);
	}

	// The test suite's window on the schedule code: the next COUNT runs
	// of SPEC after a local time, in the machine's zone or a named one.
	static int printNextRuns(const std::vector<std::string>& operands) {
		CronSchedule schedule;
		CivilTime start;
		int count = 0;
		if (!parseNextRunsArgs(operands, schedule, start, count)) return 1;

		const std::unique_ptr<CronZone> zone = zoneOperand(operands);
		if (zone == nullptr) return 1;

		std::int64_t instant = instantOfCivil(*zone, start);
		for (int i = 0; i < count; ++i) {
			if (!nextCronRun(schedule, instant, *zone, instant)) {
				std::puts("never");
				break;
			}

			printRun(*zone, instant);
		}

		std::fflush(stdout);
		return 0;
	}

	static int runCrontabAction(Executor& exec, const CrontabOptions& options) {
		CrontabTarget target;
		if (!resolveTarget(exec, options, target)) return 1;

		switch (options.action) {
			case CrontabAction::List:   return listCrontab(target);
			case CrontabAction::Remove: return removeCrontab(target, options.ask);
			case CrontabAction::Edit:   return editCrontab(exec, target);
			default:                    return replaceCrontab(exec, target, options.operands[0]);
		}
	}

	static int builtin_crontab(Executor& exec, const std::vector<std::string>& args) {
		CrontabOptions options;
		if (!parseCrontabArgs(args, options)) return 1;

		switch (options.action) {
			case CrontabAction::Help:
				printCrontabUsage(stdout);
				return 0;
			case CrontabAction::NextRuns:
				return printNextRuns(options.operands);
			case CrontabAction::Test:
				return testCrontab(exec, options.test_file);
			default:
				return runCrontabAction(exec, options);
		}
	}

	void registerCronBuiltins(Executor& exec) {
		exec.registerBuiltin("crontab", builtin_crontab);
		exec.registerBuiltin("crond",   runCrond);
	}

}  // namespace wbsh

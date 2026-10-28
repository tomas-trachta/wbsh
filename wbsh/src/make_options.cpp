/**
 * @file make_options.cpp
 * @brief make's command line: GNU make's options, `VAR=value` operands and
 *        goals, the MAKEFLAGS a parent passes down, and the MAKEFLAGS this
 *        make passes on.
 */

#include "make_internal.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "numparse.h"
#include "optscan.h"

namespace wbsh::make_detail {

	enum MakeOptionId {
		kOptIgnored = 1, kOptAlwaysMake, kOptDirectory, kOptEnvOverrides, kOptEval, kOptFile,
		kOptHelp, kOptIgnoreErrors, kOptIncludeDir, kOptJobs, kOptKeepGoing, kOptDryRun,
		kOptOldFile, kOptPrintDatabase, kOptQuestion, kOptNoBuiltinRules, kOptNoBuiltinVars,
		kOptSilent, kOptStop, kOptTouch, kOptVersion, kOptPrintDirectory, kOptNoPrintDirectory,
		kOptNewFile, kOptWarnUndefined,
	};

	static const std::vector<OptDef>& makeOptionDefs() {
		static const std::vector<OptDef> defs = {
			{ kOptIgnored,          'b', nullptr,                     OptArg::None },
			{ kOptIgnored,          'm', nullptr,                     OptArg::None },
			{ kOptAlwaysMake,       'B', "always-make",               OptArg::None },
			{ kOptDirectory,        'C', "directory",                 OptArg::Required },
			{ kOptIgnored,          'd', nullptr,                     OptArg::None },
			{ kOptIgnored,          0,   "debug",                     OptArg::Optional },
			{ kOptEnvOverrides,     'e', "environment-overrides",     OptArg::None },
			{ kOptEval,             'E', "eval",                      OptArg::Required },
			{ kOptFile,             'f', "file",                      OptArg::Required },
			{ kOptFile,             0,   "makefile",                  OptArg::Required },
			{ kOptHelp,             'h', "help",                      OptArg::None },
			{ kOptIgnoreErrors,     'i', "ignore-errors",             OptArg::None },
			{ kOptIncludeDir,       'I', "include-dir",               OptArg::Required },
			{ kOptJobs,             'j', "jobs",                      OptArg::Optional },
			{ kOptKeepGoing,        'k', "keep-going",                OptArg::None },
			{ kOptIgnored,          'l', "load-average",              OptArg::Optional },
			{ kOptIgnored,          'L', "check-symlink-times",       OptArg::None },
			{ kOptDryRun,           'n', "just-print",                OptArg::None },
			{ kOptDryRun,           0,   "dry-run",                   OptArg::None },
			{ kOptDryRun,           0,   "recon",                     OptArg::None },
			{ kOptOldFile,          'o', "old-file",                  OptArg::Required },
			{ kOptOldFile,          0,   "assume-old",                OptArg::Required },
			{ kOptIgnored,          'O', "output-sync",               OptArg::Optional },
			{ kOptPrintDatabase,    'p', "print-data-base",           OptArg::None },
			{ kOptQuestion,         'q', "question",                  OptArg::None },
			{ kOptNoBuiltinRules,   'r', "no-builtin-rules",          OptArg::None },
			{ kOptNoBuiltinVars,    'R', "no-builtin-variables",      OptArg::None },
			{ kOptSilent,           's', "silent",                    OptArg::None },
			{ kOptSilent,           0,   "quiet",                     OptArg::None },
			{ kOptStop,             'S', "no-keep-going",             OptArg::None },
			{ kOptStop,             0,   "stop",                      OptArg::None },
			{ kOptTouch,            't', "touch",                     OptArg::None },
			{ kOptIgnored,          0,   "trace",                     OptArg::None },
			{ kOptVersion,          'v', "version",                   OptArg::None },
			{ kOptPrintDirectory,   'w', "print-directory",           OptArg::None },
			{ kOptNoPrintDirectory, 0,   "no-print-directory",        OptArg::None },
			{ kOptNewFile,          'W', "what-if",                   OptArg::Required },
			{ kOptNewFile,          0,   "new-file",                  OptArg::Required },
			{ kOptNewFile,          0,   "assume-new",                OptArg::Required },
			{ kOptWarnUndefined,    0,   "warn-undefined-variables",  OptArg::None },
			{ kOptIgnored,          0,   "jobserver-auth",            OptArg::Optional },
			{ kOptIgnored,          0,   "jobserver-fds",             OptArg::Optional },
			{ kOptIgnored,          0,   "jobserver-style",           OptArg::Optional },
			{ kOptIgnored,          0,   "sync-mutex",                OptArg::Optional },
			{ kOptIgnored,          0,   "shuffle",                   OptArg::Optional },
		};
		return defs;
	}

	static bool isNumber(const std::string& text) {
		if (text.empty()) return false;
		for (const char c : text) {
			if (std::isdigit(static_cast<unsigned char>(c)) == 0) return false;
		}

		return true;
	}

	// getopt only takes an optional value attached; GNU make also accepts
	// `-j 4` and `-l 2`, so a separate number is glued on first.
	static std::vector<std::string> attachOptionalNumbers(const std::vector<std::string>& args) {
		std::vector<std::string> joined;
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			const bool takes_number = arg == "-j" || arg == "-l" || arg == "--jobs";
			if (takes_number && i + 1 < args.size() && isNumber(args[i + 1])) {
				joined.push_back(arg + (arg[1] == '-' ? "=" : "") + args[++i]);
				continue;
			}

			joined.push_back(arg);
		}

		return joined;
	}

	static void applyJobs(const OptHit& hit, MakeOptions& options) {
		int jobs = 0;
		options.jobs = hit.has_value && parseInt(hit.value, jobs) && jobs > 0 ? jobs : 0;
	}

	static void applyValueOption(const OptHit& hit, MakeOptions& options) {
		switch (hit.id) {
			case kOptDirectory:
				options.directories.push_back(hit.value);
				break;
			case kOptEval:
				options.evals.push_back(hit.value);
				break;
			case kOptFile:
				options.makefiles.push_back(hit.value);
				break;
			case kOptIncludeDir:
				options.include_dirs.push_back(hit.value);
				break;
			case kOptOldFile:
				options.old_files.push_back(hit.value);
				break;
			case kOptNewFile:
				options.new_files.push_back(hit.value);
				break;
			case kOptJobs:
				applyJobs(hit, options);
				break;
			default:
				break;
		}
	}

	struct FlagOption {
		int id;
		bool MakeOptions::*field;
		bool value;
	};

	static const FlagOption kFlagOptions[] = {
		{ kOptAlwaysMake,       &MakeOptions::always_make,           true },
		{ kOptEnvOverrides,     &MakeOptions::environment_overrides, true },
		{ kOptHelp,             &MakeOptions::show_help,             true },
		{ kOptIgnoreErrors,     &MakeOptions::ignore_errors,         true },
		{ kOptKeepGoing,        &MakeOptions::keep_going,            true },
		{ kOptDryRun,           &MakeOptions::dry_run,               true },
		{ kOptPrintDatabase,    &MakeOptions::print_database,        true },
		{ kOptQuestion,         &MakeOptions::question,              true },
		{ kOptNoBuiltinRules,   &MakeOptions::no_builtin_rules,      true },
		{ kOptNoBuiltinVars,    &MakeOptions::no_builtin_variables,  true },
		{ kOptSilent,           &MakeOptions::silent,                true },
		{ kOptStop,             &MakeOptions::keep_going,            false },
		{ kOptTouch,            &MakeOptions::touch,                 true },
		{ kOptVersion,          &MakeOptions::show_version,          true },
		{ kOptPrintDirectory,   &MakeOptions::print_directory,       true },
		{ kOptNoPrintDirectory, &MakeOptions::no_print_directory,    true },
		{ kOptWarnUndefined,    &MakeOptions::warn_undefined,        true },
	};

	static bool applyFlagOption(int id, MakeOptions& options) {
		for (const FlagOption& flag : kFlagOptions) {
			if (flag.id != id) continue;

			options.*flag.field = flag.value;
			return true;
		}

		return false;
	}

	bool parseMakeArgs(const std::vector<std::string>& args, MakeOptions& options) {
		OptScanResult scanned;
		if (!scanOptions("make", attachOptionalNumbers(args), makeOptionDefs(), scanned)) return false;

		for (const OptHit& hit : scanned.hits) {
			if (!applyFlagOption(hit.id, options)) applyValueOption(hit, options);
		}

		for (const std::string& operand : scanned.operands) {
			if (isCommandLineAssignment(operand)) options.assignments.push_back(operand);
			else options.goals.push_back(operand);
		}

		if (options.no_builtin_variables) options.no_builtin_rules = true;
		return true;
	}

	static std::vector<std::string> splitEscapedWords(const std::string& text) {
		std::vector<std::string> words;
		std::string current;
		for (std::size_t i = 0; i < text.size(); ++i) {
			const char c = text[i];
			if (c == '\\' && i + 1 < text.size() && (isBlank(text[i + 1]) || text[i + 1] == '\\')) {
				current.push_back(text[++i]);
				continue;
			}

			if (!isBlank(c)) {
				current.push_back(c);
				continue;
			}

			if (!current.empty()) words.push_back(current);
			current.clear();
		}

		if (!current.empty()) words.push_back(current);
		return words;
	}

	struct MakeflagsWords {
		std::vector<std::string> switches;
		std::vector<std::string> assignments;
	};

	// GNU make writes the letters as a first word without a dash and lets
	// switches follow the `--` that starts the variable list.
	static MakeflagsWords sortMakeflags(const std::string& value) {
		MakeflagsWords sorted;
		const std::vector<std::string> words = splitEscapedWords(value);
		for (std::size_t i = 0; i < words.size(); ++i) {
			const std::string& word = words[i];
			if (word == "--") continue;

			if (word[0] == '-') sorted.switches.push_back(word);
			else if (isCommandLineAssignment(word)) sorted.assignments.push_back(word);
			else if (i == 0) sorted.switches.push_back("-" + word);
		}

		return sorted;
	}

	bool absorbMakeflags(const std::string& value, MakeOptions& options) {
		const MakeflagsWords sorted = sortMakeflags(value);
		std::vector<std::string> words = sorted.switches;
		words.push_back("--");
		words.insert(words.end(), sorted.assignments.begin(), sorted.assignments.end());

		MakeOptions parsed = options;
		parsed.goals.clear();
		if (!parseMakeArgs(words, parsed)) return false;

		parsed.goals = options.goals;
		options = parsed;
		return true;
	}

	static void appendNew(std::vector<std::string>& into, const std::vector<std::string>& from) {
		for (const std::string& item : from) {
			if (std::find(into.begin(), into.end(), item) == into.end()) into.push_back(item);
		}
	}

	// What a makefile adds to MAKEFLAGS: only its switches count, the
	// command-line variables in it are the ones already in effect.
	bool absorbMakefileSwitches(const std::string& value, MakeOptions& options) {
		MakeOptions parsed = options;
		parsed.include_dirs.clear();
		parsed.old_files.clear();
		parsed.new_files.clear();
		if (!parseMakeArgs(sortMakeflags(value).switches, parsed)) return false;

		std::vector<std::string> include_dirs = options.include_dirs;
		std::vector<std::string> old_files = options.old_files;
		std::vector<std::string> new_files = options.new_files;
		appendNew(include_dirs, parsed.include_dirs);
		appendNew(old_files, parsed.old_files);
		appendNew(new_files, parsed.new_files);
		parsed.include_dirs = include_dirs;
		parsed.old_files = old_files;
		parsed.new_files = new_files;
		parsed.evals = options.evals;
		parsed.makefiles = options.makefiles;
		parsed.directories = options.directories;
		options = parsed;
		return true;
	}

	static std::string escapeBlanks(const std::string& text) {
		std::string escaped;
		for (const char c : text) {
			if (isBlank(c) || c == '\\') escaped.push_back('\\');
			escaped.push_back(c);
		}

		return escaped;
	}

	static std::string flagLetters(const MakeOptions& options) {
		std::string letters;
		if (options.always_make) letters += 'B';
		if (options.environment_overrides) letters += 'e';
		if (options.ignore_errors) letters += 'i';
		if (options.keep_going) letters += 'k';
		if (options.dry_run) letters += 'n';
		if (options.question) letters += 'q';
		if (options.no_builtin_rules) letters += 'r';
		if (options.no_builtin_variables) letters += 'R';
		if (options.silent) letters += 's';
		if (options.touch) letters += 't';
		if (options.print_directory) letters += 'w';
		return letters;
	}

	// As in GNU make: the single-letter flags as one word without a dash,
	// then the other options, then `--` and the command-line variables; a
	// value without letters starts with a blank.
	std::string composeMakeflags(const MakeOptions& options) {
		std::vector<std::string> words;
		const std::string letters = flagLetters(options);
		words.push_back(letters);
		for (const std::string& dir : options.include_dirs) words.push_back("-I" + escapeBlanks(dir));
		if (options.no_print_directory) words.push_back("--no-print-directory");
		if (options.warn_undefined) words.push_back("--warn-undefined-variables");
		if (!options.assignments.empty()) words.push_back("--");
		for (const std::string& assignment : options.assignments) {
			words.push_back(escapeBlanks(assignment));
		}

		if (words.size() == 1) return letters;

		std::string joined = letters;
		for (std::size_t i = 1; i < words.size(); ++i) joined += " " + words[i];
		return joined;
	}

}  // namespace wbsh::make_detail

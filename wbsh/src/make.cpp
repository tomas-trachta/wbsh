/**
 * @file make.cpp
 * @brief make: maintain targets from a makefile, as GNU make 4.4 does (the
 *        reading, expansion, rules and recipes live in the make_*.cpp files).
 *
 * Recipe lines run in a fresh wbsh (or the shell SHELL names), so a
 * recursive `$(MAKE)` is this builtin again in a child shell, which learns
 * its depth and the flags from MAKELEVEL and MAKEFLAGS.
 */

#include "coreutils_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "executor.h"
#include "interrupt.h"
#include "make_internal.h"
#include "numparse.h"
#include "pathconv.h"

namespace wbsh {

	using make_detail::Make;
	using make_detail::MakeOptions;

	static const int kStatusError = 2;
	static const int kMaxRestarts = 10;

	static const char* const kDefaultMakefiles[] = { "GNUmakefile", "makefile", "Makefile" };

	static const char* const kUsage =
		"Usage: make [options] [target] ...\n"
		"Options:\n"
		"  -B, --always-make           Unconditionally make all targets.\n"
		"  -C DIRECTORY                Change to DIRECTORY before doing anything.\n"
		"  -e, --environment-overrides Environment variables override makefiles.\n"
		"  -E STRING, --eval=STRING    Evaluate STRING as a makefile statement.\n"
		"  -f FILE, --file=FILE        Read FILE as a makefile.\n"
		"  -i, --ignore-errors         Ignore errors from recipes.\n"
		"  -I DIRECTORY                Search DIRECTORY for included makefiles.\n"
		"  -j [N], --jobs[=N]          Accepted; jobs run one at a time.\n"
		"  -k, --keep-going            Keep going when some targets can't be made.\n"
		"  -n, --just-print, --dry-run Don't actually run any recipe; just print them.\n"
		"  -o FILE, --old-file=FILE    Consider FILE to be very old and don't remake it.\n"
		"  -p, --print-data-base       Print make's internal database.\n"
		"  -q, --question              Run no recipe; exit status says if up to date.\n"
		"  -r, --no-builtin-rules      Disable the built-in implicit rules.\n"
		"  -R, --no-builtin-variables  Disable the built-in variable settings.\n"
		"  -s, --silent, --quiet       Don't echo recipes.\n"
		"  -S, --no-keep-going         Turns off -k.\n"
		"  -t, --touch                 Touch targets instead of remaking them.\n"
		"  -v, --version               Print the version number of make and exit.\n"
		"  -w, --print-directory       Print the current directory.\n"
		"  --no-print-directory        Turn off -w, even if it was turned on implicitly.\n"
		"  -W FILE, --what-if=FILE     Consider FILE to be infinitely new.\n"
		"  --warn-undefined-variables  Warn when an undefined variable is referenced.\n";

	static int makeLevel(Executor& exec) {
		int level = 0;
		if (!parseInt(exec.env().get("MAKELEVEL"), level) || level < 0) return 0;
		return level;
	}

	static bool collectOptions(Executor& exec, const std::vector<std::string>& args,
	                           MakeOptions& options) {
		const std::string inherited = exec.env().get("MAKEFLAGS");
		if (!inherited.empty() && !make_detail::absorbMakeflags(inherited, options)) return false;

		return make_detail::parseMakeArgs(args, options);
	}

	static bool isImportSkipped(const std::string& name) {
		return name == "SHELL" || name == "MAKEFLAGS" || name == "MFLAGS" || name == "MAKELEVEL"
			|| name == "MAKE_RESTARTS" || name == "CURDIR";
	}

	static void importEnvironment(Make& make) {
		const Environment& env = make.exec.env();
		const make_detail::Origin origin = make.options.environment_overrides
			? make_detail::Origin::EnvironmentOverride
			: make_detail::Origin::Environment;
		for (const auto& entry : env.vars()) {
			if (!env.isExported(entry.first)) continue;

			make.base_environment.emplace_back(entry.first, entry.second);
			if (isImportSkipped(entry.first)) continue;

			make_detail::Variable variable;
			variable.value = entry.second;
			variable.origin = origin;
			variable.export_mode = make_detail::ExportMode::Export;
			make_detail::defineVariable(make.globals, entry.first, variable);
		}
	}

	static std::string doubledDollars(const std::string& text) {
		std::string doubled;
		for (const char c : text) {
			doubled.push_back(c);
			if (c == '$') doubled.push_back('$');
		}

		return doubled;
	}

	// MAKEFLAGS is recursive, so a `$` in a command-line value is stored
	// doubled and a recipe's environment carries it as it was typed.
	static void defineMakeflags(Make& make) {
		const std::string flags = make_detail::composeMakeflags(make.options);
		make_detail::Variable variable;
		variable.value = doubledDollars(flags);
		variable.origin = make_detail::Origin::File;
		variable.export_mode = make_detail::ExportMode::Export;
		make_detail::defineVariable(make.globals, "MAKEFLAGS", variable);
		const std::string switches = make_detail::trimBlanks(flags.substr(0, flags.find(" -- ")));
		const bool dashed = switches.empty() || switches[0] == '-';
		make_detail::setGlobal(make, "MFLAGS", dashed ? switches : "-" + switches,
			make_detail::Origin::File);
	}

	static void defineRunVariables(Make& make) {
		using make_detail::Origin;
		using make_detail::setGlobal;
		setGlobal(make, "CURDIR", make.exec.env().get("PWD"), Origin::File);
		setGlobal(make, "MAKELEVEL", std::to_string(make.level), Origin::File);
		setGlobal(make, "MAKECMDGOALS", make_detail::joinWords(make.options.goals), Origin::File);
		setGlobal(make, ".INCLUDE_DIRS", make_detail::joinWords(make.options.include_dirs),
			Origin::Default);
		if (make.restarts > 0) setGlobal(make, "MAKE_RESTARTS", std::to_string(make.restarts),
			Origin::File);
		defineMakeflags(make);
	}

	static void setupDatabase(Make& make) {
		importEnvironment(make);
		make_detail::defineDefaultVariables(make);
		defineRunVariables(make);
		for (const std::string& assignment : make.options.assignments) {
			make_detail::applyCommandLineAssignment(make, assignment);
		}
	}

	static std::vector<std::string> directoryNames() {
		std::vector<std::string> names;
		std::error_code ec;
		for (std::filesystem::directory_iterator it(".", ec), end; !ec && it != end; it.increment(ec)) {
			names.push_back(pathToUtf8(it->path().filename()));
		}

		return names;
	}

	// The file system ignores case, so `makefile` would find a `Makefile`;
	// the name as it is spelled on disk is the one messages show.
	static std::string defaultMakefile() {
		const std::vector<std::string> names = directoryNames();
		for (const char* candidate : kDefaultMakefiles) {
			if (std::find(names.begin(), names.end(), candidate) != names.end()) return candidate;
		}

		for (const char* candidate : kDefaultMakefiles) {
			for (const std::string& name : names) {
				if (_stricmp(name.c_str(), candidate) == 0) return name;
			}
		}

		return {};
	}

	// The makefiles named or found are remade like included ones, so a rule
	// such as `Makefile: Makefile.in` takes effect.
	static void rememberMakefile(Make& make, const std::string& name) {
		make_detail::IncludedMakefile record;
		record.name = name;
		make.includes.push_back(record);
	}

	static bool readNamedMakefile(Make& make, const std::string& name) {
		if (make_detail::readMakefile(make, name)) {
			if (name != "-") rememberMakefile(make, name);
			return !make.fatal;
		}

		if (make.fatal) return false;

		make_detail::reportError(make, name + ": No such file or directory");
		make_detail::reportFatal(make, make_detail::Location{},
			"No rule to make target '" + name + "'");
		return false;
	}

	static bool readMakefiles(Make& make) {
		make_detail::readBuiltinRules(make);
		make.where = make_detail::Location{};
		for (const std::string& text : make.options.evals) make_detail::evalText(make, text);
		if (make.fatal) return false;

		if (!make.options.makefiles.empty()) {
			for (const std::string& name : make.options.makefiles) {
				if (!readNamedMakefile(make, name)) return false;
			}

			return true;
		}

		const std::string found = defaultMakefile();
		if (found.empty()) return true;

		make_detail::readMakefile(make, found);
		rememberMakefile(make, found);
		return !make.fatal;
	}

	// A makefile may add to MAKEFLAGS (`MAKEFLAGS += -s`); what it adds
	// takes effect once reading is done, as in GNU make.
	static void absorbMakefileFlags(Make& make) {
		const make_detail::Variable* flags = make_detail::findGlobal(make, "MAKEFLAGS");
		if (flags == nullptr) return;

		const std::string value = make_detail::expand(make, flags->value, make_detail::ExpandContext{});
		MakeOptions updated = make.options;
		if (!make_detail::absorbMakefileSwitches(value, updated)) return;

		if (updated.no_builtin_rules && !make.options.no_builtin_rules) make.suffixes.clear();
		make.options = updated;
		defineMakeflags(make);
	}

	static void finishReading(Make& make) {
		absorbMakefileFlags(make);
		make_detail::convertSuffixRules(make);
		for (const std::string& name : make.options.old_files) {
			make_detail::enterFile(make, name).assume_old = true;
		}

		for (const std::string& name : make.options.new_files) {
			make_detail::enterFile(make, name).assume_new = true;
		}
	}

	static std::vector<std::string> chooseGoals(Make& make) {
		if (!make.options.goals.empty()) return make.options.goals;

		const make_detail::Variable* goal = make_detail::findGlobal(make, ".DEFAULT_GOAL");
		const std::string name = goal == nullptr ? std::string() : make_detail::trimBlanks(
			make_detail::expand(make, goal->value, make_detail::ExpandContext{}));
		if (!name.empty()) return make_detail::splitWords(name);

		const make_detail::Variable* list = make_detail::findGlobal(make, "MAKEFILE_LIST");
		const bool read_any = list != nullptr && !make_detail::trimBlanks(list->value).empty();
		make_detail::reportFatal(make, make_detail::Location{}, read_any
			? "No targets"
			: "No targets specified and no makefile found");
		return {};
	}

	static int buildGoals(Make& make) {
		const std::vector<std::string> goals = chooseGoals(make);
		if (goals.empty()) return kStatusError;

		const int status = make_detail::updateGoals(make, goals);
		make_detail::removeIntermediates(make);
		return status;
	}

	static int runOnce(Make& make, bool& restart) {
		setupDatabase(make);
		if (!readMakefiles(make)) return kStatusError;

		finishReading(make);
		if (!make_detail::remakeIncludedMakefiles(make, restart)) return kStatusError;
		if (restart) return 0;

		const int status = buildGoals(make);
		if (make.options.print_database) make_detail::printDatabase(make);
		return status;
	}

	static int runMake(Executor& exec, const MakeOptions& options, int level) {
		for (int restarts = 0;; ++restarts) {
			Make make(exec);
			make.options = options;
			make.level = level;
			make.restarts = restarts;
			make.self_path = selfExecutablePath();

			bool restart = false;
			const int status = runOnce(make, restart);
			if (make.interrupted) return 130;
			if (!restart || restarts >= kMaxRestarts) return status;
		}
	}

	static std::string prefixFor(int level) {
		return level == 0 ? "make" : "make[" + std::to_string(level) + "]";
	}

	static bool printsDirectory(const MakeOptions& options, int level) {
		if (options.no_print_directory) return false;
		if (options.print_directory) return true;

		return !options.silent && (!options.directories.empty() || level > 0);
	}

	static void announceDirectory(Executor& exec, int level, const char* verb) {
		std::printf("%s: %s directory '%s'\n", prefixFor(level).c_str(), verb,
			exec.env().get("PWD").c_str());
		std::fflush(stdout);
	}

	static bool enterDirectories(Executor& exec, const MakeOptions& options, int level) {
		for (const std::string& dir : options.directories) {
			std::string error;
			if (changeDirectory(exec, dir, error)) continue;

			std::fflush(stdout);
			std::fprintf(stderr, "%s: *** %s: No such file or directory.  Stop.\n",
				prefixFor(level).c_str(), dir.c_str());
			return false;
		}

		return true;
	}

	static int runInDirectories(Executor& exec, const MakeOptions& options, int level) {
		if (!enterDirectories(exec, options, level)) return kStatusError;

		const bool announce = printsDirectory(options, level);
		if (announce) announceDirectory(exec, level, "Entering");
		const int status = runMake(exec, options, level);
		if (announce) announceDirectory(exec, level, "Leaving");
		return status;
	}

	static void restoreDirectory(Executor& exec, const std::string& pwd, const std::string& oldpwd) {
		std::string error;
		if (exec.env().get("PWD") == pwd) return;

		changeDirectory(exec, pwd, error);
		exec.env().set("OLDPWD", oldpwd);
	}

	static int builtin_make(Executor& exec, const std::vector<std::string>& args) {
		MakeOptions options;
		if (!collectOptions(exec, args, options)) return kStatusError;

		if (options.show_help) {
			std::fputs(kUsage, stdout);
			return 0;
		}

		if (options.show_version) {
			std::puts("GNU Make-compatible make (wbsh) 4.4.1");
			return 0;
		}

		const ScopedCtrlCCapture capture;
		const std::string pwd = exec.env().get("PWD");
		const std::string oldpwd = exec.env().get("OLDPWD");
		const int status = runInDirectories(exec, options, makeLevel(exec));
		restoreDirectory(exec, pwd, oldpwd);
		return status;
	}

	void registerMakeBuiltin(Executor& exec) {
		exec.registerBuiltin("make", builtin_make);
		exec.registerBuiltin("gmake", builtin_make);
	}

}  // namespace wbsh

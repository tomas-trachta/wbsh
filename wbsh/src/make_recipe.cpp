/**
 * @file make_recipe.cpp
 * @brief Running a target's recipe: every line expanded first, split into
 *        commands with their `@` `-` `+` prefixes, echoed, run in a fresh
 *        shell (one shell for all of them under .ONESHELL), errors reported
 *        and ignored or acted on, the target deleted when it must be, and
 *        `-t`'s touching instead.
 */

#include "make_internal.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "coreutils_internal.h"
#include "dateparse.h"
#include "filetimes.h"
#include "interrupt.h"

namespace wbsh::make_detail {

	namespace fs = std::filesystem;

	struct Command {
		std::string text;
		Location where;
		bool silent = false;
		bool ignore = false;
		bool recurse = false;
	};

	struct CommandFlags {
		bool silent = false;
		bool ignore = false;
		bool recurse = false;
	};

	static std::size_t takePrefixes(const std::string& text, CommandFlags& flags) {
		std::size_t i = 0;
		for (; i < text.size(); ++i) {
			const char c = text[i];
			if (c == '@') flags.silent = true;
			else if (c == '-') flags.ignore = true;
			else if (c == '+') flags.recurse = true;
			else if (!isBlank(c)) break;
		}

		return i;
	}

	static bool mentionsMake(const std::string& text) {
		return text.find("$(MAKE)") != std::string::npos || text.find("${MAKE}") != std::string::npos;
	}

	bool recipeRecurses(const Recipe& recipe) {
		for (const RecipeLine& line : recipe.lines) {
			CommandFlags flags;
			takePrefixes(line.text, flags);
			if (flags.recurse || mentionsMake(line.text)) return true;
		}

		return false;
	}

	static std::vector<std::string> splitCommands(const std::string& text) {
		std::vector<std::string> pieces;
		std::size_t start = 0;
		for (std::size_t i = 0; i < text.size(); ++i) {
			if (text[i] != '\n' || (i > 0 && text[i - 1] == '\\')) continue;

			pieces.push_back(text.substr(start, i - start));
			start = i + 1;
		}

		pieces.push_back(text.substr(start));
		return pieces;
	}

	static void addCommands(const RecipeLine& line, const std::string& expanded,
	                        std::vector<Command>& commands) {
		CommandFlags line_flags;
		takePrefixes(line.text, line_flags);
		line_flags.recurse = line_flags.recurse || mentionsMake(line.text);
		for (const std::string& piece : splitCommands(expanded)) {
			CommandFlags flags = line_flags;
			const std::size_t start = takePrefixes(piece, flags);
			Command command;
			command.text = piece.substr(start);
			command.where = line.where;
			command.silent = flags.silent;
			command.ignore = flags.ignore;
			command.recurse = flags.recurse;
			commands.push_back(command);
		}
	}

	static std::vector<Command> expandRecipe(Make& make, const RecipeJob& job) {
		std::vector<Command> commands;
		const Location saved = make.where;
		for (const RecipeLine& line : job.recipe.lines) {
			make.where = line.where;
			const std::string expanded = expand(make, line.text, job.ctx);
			if (make.fatal) break;
			addCommands(line, expanded, commands);
		}

		make.where = saved;
		return commands;
	}

	static std::vector<Command> joinForOneShell(const std::vector<Command>& commands) {
		if (commands.empty()) return commands;

		Command script = commands.front();
		for (std::size_t i = 1; i < commands.size(); ++i) {
			script.text += "\n" + commands[i].text;
			script.recurse = script.recurse || commands[i].recurse;
		}

		return { script };
	}

	static bool matchesPreciousPattern(const Make& make, const std::string& name) {
		std::string stem;
		for (const std::string& pattern : make.precious_patterns) {
			if (matchPattern(pattern, name, stem)) return true;
		}

		return false;
	}

	static void deleteTargetIfChanged(Make& make, const FileNode& file, const FileTime& before) {
		if (file.phony || file.precious || matchesPreciousPattern(make, file.name)) return;

		const FileTime now = statFile(make, file.name);
		if (!now.exists || now == before) return;

		std::error_code ec;
		if (fs::is_directory(toNative(make.exec, file.name), ec)) return;

		reportError(make, "*** Deleting file '" + file.name + "'");
		fs::remove(toNative(make.exec, file.name), ec);
	}

	static std::string errorPlace(const Command& command, const FileNode& file) {
		std::string place = command.where.file;
		if (command.where.line > 0) place += ":" + std::to_string(command.where.line);
		return "[" + place + ": " + file.name + "]";
	}

	struct RecipeRun {
		Make& make;
		const RecipeJob& job;
		FileTime before;
		ShellProgram shell;
		NameValueList environment;
		bool prepared = false;
		bool counted = false;
	};

	static bool isSilent(const RecipeRun& run, const Command& command) {
		const Make& make = run.make;
		return command.silent || make.options.silent || make.special.silent_all || run.job.file.silent;
	}

	static bool isIgnored(const RecipeRun& run, const Command& command) {
		const Make& make = run.make;
		return command.ignore || make.options.ignore_errors || make.special.ignore_all
			|| run.job.file.ignore_errors;
	}

	static bool shouldExecute(const Make& make, const Command& command) {
		const bool pretending = make.options.dry_run || make.options.question || make.options.touch;
		return !pretending || command.recurse;
	}

	static void prepareShell(RecipeRun& run) {
		if (run.prepared) return;

		run.prepared = true;
		run.shell = resolveShell(run.make, run.job.ctx);
		run.environment = childEnvironment(run.make, run.job.ctx);
	}

	static UpdateStatus interrupted(RecipeRun& run, const Command& command) {
		deleteTargetIfChanged(run.make, run.job.file, run.before);
		reportError(run.make, "*** " + errorPlace(command, run.job.file) + " Interrupt");
		run.make.interrupted = true;
		return UpdateStatus::Failed;
	}

	static UpdateStatus failed(RecipeRun& run, const Command& command, int status) {
		const std::string place = errorPlace(command, run.job.file);
		if (isIgnored(run, command)) {
			reportError(run.make, place + " Error " + std::to_string(status) + " (ignored)");
			return UpdateStatus::Success;
		}

		reportError(run.make, "*** " + place + " Error " + std::to_string(status));
		if (run.make.special.delete_on_error) deleteTargetIfChanged(run.make, run.job.file, run.before);
		return UpdateStatus::Failed;
	}

	static UpdateStatus runCommand(RecipeRun& run, const Command& command) {
		Make& make = run.make;
		if (trimBlanks(command.text).empty()) return UpdateStatus::Success;

		if (!run.counted) ++make.commands_started;
		run.counted = true;
		if (!isSilent(run, command) || make.options.dry_run) printLine(command.text);
		if (!shouldExecute(make, command)) return UpdateStatus::Success;

		prepareShell(run);
		const ShellResult result = runShellLine(make, run.shell, command.text, run.environment);
		if (takeCtrlC() || result.control_c) return interrupted(run, command);
		if (result.status == 0) return UpdateStatus::Success;
		if (make.options.question) return UpdateStatus::Question;

		return failed(run, command, result.status);
	}

	UpdateStatus runRecipe(Make& make, const RecipeJob& job) {
		std::vector<Command> commands = expandRecipe(make, job);
		if (make.fatal) return UpdateStatus::Failed;
		if (make.special.oneshell) commands = joinForOneShell(commands);

		RecipeRun run{ make, job, statFile(make, job.file.name) };
		for (const Command& command : commands) {
			const UpdateStatus status = runCommand(run, command);
			if (status != UpdateStatus::Success) return status;
		}

		return UpdateStatus::Success;
	}

	static DateValue currentDate() {
		const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
		const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count();
		DateValue date;
		date.seconds = static_cast<long long>(nanos / 1000000000LL);
		date.nanos = static_cast<long>(nanos % 1000000000LL);
		return date;
	}

	void touchTarget(Make& make, FileNode& file) {
		if (file.phony) return;

		++make.commands_started;
		if (!make.options.silent) printLine("touch " + file.name);
		if (make.options.dry_run) return;

		std::FILE* created = fopenNative(make.exec, file.name, "ab");
		if (created != nullptr) std::fclose(created);

		const DateValue now = currentDate();
		writeFileTimes(toNative(make.exec, file.name), &now, &now);
	}

}  // namespace wbsh::make_detail

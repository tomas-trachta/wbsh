#pragma once

/**
 * @file completion.h
 * @brief bash's programmable completion: the specs `complete` stores, the
 *        matches a spec generates, and the `complete`, `compgen` and
 *        `compopt` builtins.
 *
 * A spec generates matches the way bash's gen_compspec_completions does:
 * its actions (-A, -f, -d, ...), -G glob, -W word list -- all filtered by
 * the word being completed -- then the -F function's COMPREPLY and the -C
 * command's output lines, then -X removes matches, and -P / -S wrap them.
 */

#include <string>
#include <vector>

namespace wbsh {

	class Executor;

	struct CompletionSpec {
		std::vector<std::string> actions;   ///< -A names; -f is "file", -d "directory", ...
		std::vector<std::string> options;   ///< -o names: default, filenames, nospace, ...
		std::string glob;                   ///< -G
		std::string words;                  ///< -W, expanded each time it is used
		std::string prefix;                 ///< -P
		std::string suffix;                 ///< -S
		std::string filter;                 ///< -X
		std::string command;                ///< -C
		std::string function;               ///< -F

		bool hasOption(const std::string& name) const;
	};

	/** The names bash files the -D, -E and -I specs under. */
	extern const char* const kDefaultCompletionName;
	extern const char* const kEmptyCompletionName;
	extern const char* const kInitialCompletionName;

	/** The command a completion is for, with the word before the one completed. */
	struct CompletionRequest {
		std::string command;
		std::string word;
		std::string previous;
	};

	/** The matches of a spec, and the -o options in force -- compopt may change them. */
	struct CompletionResult {
		std::vector<std::string> matches;
		std::vector<std::string> options;
		bool file_names = false;   ///< file names: quoted and directories marked when inserted
		int function_status = 0;   ///< the -F function's exit status

		bool hasOption(const std::string& name) const;
	};

	/**
	 * Runs @p spec for @p request. The caller sets COMP_LINE, COMP_WORDS
	 * and the rest before a spec with -F or -C is run.
	 */
	CompletionResult generateCompletions(Executor& exec, const CompletionSpec& spec,
	                                     const CompletionRequest& request);

	/** Whether @p text starts with @p prefix, ignoring case when completion-ignore-case is on. */
	bool completionMatches(const std::string& text, const std::string& prefix);

	/** `complete -p`'s line for the spec of @p name, which `complete` reads back. */
	std::string completionSpecText(const std::string& name, const CompletionSpec& spec);

	/** Every spec as `complete` commands, for a self-spawned subshell to run. */
	std::string serializeCompletionSpecs(const Executor& exec);

	/** Registers `complete`, `compgen` and `compopt`. */
	void registerCompletionBuiltins(Executor& exec);

}  // namespace wbsh

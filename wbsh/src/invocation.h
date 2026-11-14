#pragma once

/**
 * @file invocation.h
 * @brief The shell's own command line, read the way bash reads it.
 *
 * Single-letter options are bash's (`-c`, `-s`, `-i`, `-l`, the `set`
 * letters, `-o NAME`, `-O NAME`) and may be clustered; the command
 * string of `-c` is the first operand after the options, so
 * `wbsh -c -l 'cmd'` and `wbsh -ec 'cmd'` work as in bash. wbsh's own
 * debugging dumps are long options only.
 */

#include <string>
#include <vector>

#include "startup.h"

namespace wbsh {

	enum class InvocationAction {
		RunScript,
		RunInteractive,
		PrintHelp,
		PrintAgentInfo,
		PrintVersion,
	};

	enum class InvocationSource {
		Unset,
		CommandString,
		File,
		Stdin,
	};

	struct Invocation {
		InvocationAction action = InvocationAction::RunScript;
		InvocationSource source = InvocationSource::Unset;
		std::string command;
		std::string file_path;
		std::vector<std::string> operands;    ///< For -c: `$0` then `$1`..; otherwise `$1`..
		std::string shell_name = "wbsh";      ///< argv[0]'s base name without `.exe`.
		ShellStartup startup;
		bool interactive     = false;
		bool force_run       = false;
		bool show_tokens     = false;
		bool show_ast        = false;
		bool hide_ast        = false;
		bool show_expansions = false;
		bool json            = false;    ///< `--json`: report the run as one JSON object.
	};

	/// Fills @p out; on a bad command line returns false with bash's message.
	bool parseInvocation(int argc, char** argv, Invocation& out, std::string& out_error);

	/// argv[0]'s base name without `.exe`, which bash's `$0` and error
	/// prefix use: a copy run as `bash.exe` calls itself `bash`.
	std::string invokedShellName(const std::string& argv0);

	/// A dump (`--ast`, `--tokens`, `--expand`) replaces running unless `--run` asks for both.
	bool invocationExecutes(const Invocation& invocation);

	/// A dump shows the AST unless `--no-ast`; a run only with `--ast`.
	bool invocationShowsAst(const Invocation& invocation);

}  // namespace wbsh

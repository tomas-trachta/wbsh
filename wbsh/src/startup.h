#pragma once

/**
 * @file startup.h
 * @brief What the command line asks a new shell to do before its first
 *        command: options to set and startup files to read.
 */

#include <string>
#include <vector>

namespace wbsh {

	class Executor;

	struct ShellStartup {
		std::vector<std::string> set_words;   ///< Handed to `set` as-is: `-e`, `+u`, `-o pipefail`.
		std::vector<std::string> shopts_on;
		std::vector<std::string> shopts_off;
		bool        login        = false;
		bool        read_profile = true;
		bool        read_rc      = true;
		std::string rcfile;                   ///< Replaces `~/.wbshrc` when set.
		bool        forced_interactive = false;   ///< `-i` given, which keeps `-n`.
	};

	void applyStartupOptions(Executor& exec, const ShellStartup& startup);

	/// A login shell reads `~/.wbsh_profile`, never the `~/.profile` written for
	/// Git Bash or Linux.
	void sourceLoginProfile(Executor& exec, const ShellStartup& startup);

	void sourceRcFile(Executor& exec, const ShellStartup& startup);

	/// A non-interactive shell outside POSIX mode reads the file `$BASH_ENV`
	/// names, after expanding parameters and command substitutions in it.
	void sourceBashEnv(Executor& exec);

}  // namespace wbsh

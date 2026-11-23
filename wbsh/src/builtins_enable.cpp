/**
 * @file builtins_enable.cpp
 * @brief The `enable` builtin: turn builtins off and on again.
 *
 * Many coreutils are builtins in wbsh, so `enable -n ls` is how a user
 * gets the `ls` on PATH instead.
 */

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "diagnostics.h"
#include "executor.h"

namespace wbsh::enable_detail {

	// POSIX special builtins, what `enable -s` lists; bash counts `source`
	// as one, as it does `.`.
	static const char* const kSpecialBuiltins[] = {
		".", ":", "break", "continue", "eval", "exec", "exit", "export",
		"readonly", "return", "set", "shift", "source", "times", "trap", "unset",
	};

	struct Options {
		bool disable = false;
		bool all = false;
		bool special = false;
		bool unload = false;    ///< `-d`: delete a loaded builtin
		std::vector<std::string> names;
	};

	static bool isSpecialBuiltin(const std::string& name) {
		return isPosixSpecialBuiltin(name);
	}

	static void printUsage() {
		std::fprintf(stderr, "enable: usage: enable [-a] [-dnps] [-f filename] [name ...]\n");
	}

	static bool applyFlag(char flag, Options& options) {
		switch (flag) {
			case 'n':
				options.disable = true;
				return true;
			case 'a':
				options.all = true;
				return true;
			case 's':
				options.special = true;
				return true;
			case 'p':
				return true;
			case 'd':
				options.unload = true;
				return true;
			case 'f':
				shellErrorf("enable: -%c: loadable builtins are not supported\n",
					flag);
				return false;
			default:
				shellErrorf("enable: -%c: invalid option\n", flag);
				printUsage();
				return false;
		}
	}

	static bool parseOptions(const std::vector<std::string>& args, Options& options) {
		std::size_t i = 0;
		while (i < args.size() && args[i].size() > 1 && args[i][0] == '-') {
			const std::string& word = args[i++];
			if (word == "--") break;

			for (std::size_t k = 1; k < word.size(); ++k) {
				if (!applyFlag(word[k], options)) return false;
			}
		}

		options.names.assign(args.begin() + static_cast<std::ptrdiff_t>(i), args.end());
		return true;
	}

	static bool listed(const Executor& exec, const std::string& name, const Options& options) {
		if (options.special && !isSpecialBuiltin(name)) return false;
		if (options.all) return true;

		const bool enabled = exec.isBuiltin(name);
		return options.disable ? !enabled : enabled;
	}

	static int listBuiltins(const Executor& exec, const Options& options) {
		std::vector<std::string> names = exec.allBuiltinNames();
		std::sort(names.begin(), names.end());
		for (const std::string& name : names) {
			if (!listed(exec, name, options)) continue;

			std::printf("enable %s%s\n", exec.isBuiltin(name) ? "" : "-n ", name.c_str());
		}

		std::fflush(stdout);
		return 0;
	}

	static int switchBuiltins(Executor& exec, const Options& options) {
		int status = 0;
		for (const std::string& name : options.names) {
			if (!exec.isKnownBuiltin(name)) {
				shellErrorf("enable: %s: not a shell builtin\n", name.c_str());
				status = 1;
				continue;
			}

			exec.setBuiltinEnabled(name, !options.disable);
		}

		return status;
	}

	// No builtin is ever loaded, so none can be deleted.
	static int unloadBuiltins(const Executor& exec, const Options& options) {
		for (const std::string& name : options.names) {
			const char* problem = exec.isKnownBuiltin(name) ? "not dynamically loaded"
				: "not a shell builtin";
			shellErrorf("enable: %s: %s\n", name.c_str(), problem);
		}

		return 1;
	}

	static int builtin_enable(Executor& exec, const std::vector<std::string>& args) {
		Options options;
		if (!parseOptions(args, options)) return 2;
		if (options.names.empty()) return listBuiltins(exec, options);
		if (options.unload) return unloadBuiltins(exec, options);

		return switchBuiltins(exec, options);
	}

}  // namespace wbsh::enable_detail

namespace wbsh {

	bool isPosixSpecialBuiltin(const std::string& name) {
		for (const char* special : enable_detail::kSpecialBuiltins) {
			if (name == special) return true;
		}

		return false;
	}

	void registerEnableBuiltin(Executor& exec) {
		exec.registerBuiltin("enable", enable_detail::builtin_enable);
	}

}  // namespace wbsh

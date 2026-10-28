/**
 * @file builtins_trap.cpp
 * @brief The `trap` builtin: setting, resetting and listing traps.
 *
 * Traps are stored under the bare condition name (`INT`, `EXIT`, `ERR`);
 * listings show signals with bash's `SIG` prefix and in bash's order:
 * EXIT, the signals by number, then DEBUG, ERR and RETURN.
 */

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include "executor.h"
#include "numparse.h"

namespace wbsh::trap_detail {

	// Linux numbering, which bash scripts and `kill -l` output assume.
	static const char* const kSignals[] = {
		"HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "BUS", "FPE", "KILL", "USR1",
		"SEGV", "USR2", "PIPE", "ALRM", "TERM", "STKFLT", "CHLD", "CONT", "STOP", "TSTP",
		"TTIN", "TTOU", "URG", "XCPU", "XFSZ", "VTALRM", "PROF", "WINCH", "IO", "PWR",
		"SYS",
	};
	static const int kSignalCount = static_cast<int>(sizeof(kSignals) / sizeof(kSignals[0]));
	static const char* const kPseudoSignals[] = { "DEBUG", "ERR", "RETURN" };
	static const int kSignalsPerListLine = 5;

	static std::string upper(std::string text) {
		for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		return text;
	}

	static std::string signalByNumber(int number) {
		if (number == 0) return "EXIT";
		if (number < 1 || number > kSignalCount) return {};
		return kSignals[number - 1];
	}

	static bool isKnownName(const std::string& name) {
		if (name == "EXIT") return true;
		for (const char* pseudo : kPseudoSignals) {
			if (name == pseudo) return true;
		}

		for (const char* signal : kSignals) {
			if (name == signal) return true;
		}

		return false;
	}

	// `INT`, `SIGINT`, `sigint` and `2` all name the same trap; empty when
	// @p spec names none.
	static std::string conditionName(const std::string& spec) {
		int number = 0;
		if (parseInt(spec, number)) return signalByNumber(number);

		std::string name = upper(spec);
		if (name.size() > 3 && name.compare(0, 3, "SIG") == 0) name = name.substr(3);
		return isKnownName(name) ? name : std::string();
	}

	static std::string displayName(const std::string& name) {
		if (name == "EXIT") return name;
		for (const char* pseudo : kPseudoSignals) {
			if (name == pseudo) return name;
		}

		return "SIG" + name;
	}

	static std::string quoted(const std::string& action) {
		std::string out = "'";
		for (const char c : action) {
			if (c == '\'') out += "'\\''";
			else out.push_back(c);
		}

		return out + "'";
	}

	static void printTrap(const Executor& exec, const std::string& name) {
		const auto& traps = exec.listedTraps();
		const auto found = traps.find(name);
		if (found == traps.end()) return;

		std::printf("trap -- %s %s\n", quoted(found->second).c_str(), displayName(name).c_str());
	}

	static std::vector<std::string> listingOrder() {
		std::vector<std::string> names = { "EXIT" };
		for (const char* signal : kSignals) names.push_back(signal);
		for (const char* pseudo : kPseudoSignals) names.push_back(pseudo);
		return names;
	}

	static void reportInvalid(const std::string& spec) {
		std::fprintf(stderr, "wbsh: trap: %s: invalid signal specification\n", spec.c_str());
	}

	static int printTraps(const Executor& exec, const std::vector<std::string>& specs) {
		if (specs.empty()) {
			for (const std::string& name : listingOrder()) printTrap(exec, name);
			return 0;
		}

		int status = 0;
		for (const std::string& spec : specs) {
			const std::string name = conditionName(spec);
			if (name.empty()) {
				reportInvalid(spec);
				status = 1;
				continue;
			}

			printTrap(exec, name);
		}

		return status;
	}

	static int listSignals() {
		for (int number = 1; number <= kSignalCount; ++number) {
			const bool line_end = number % kSignalsPerListLine == 0 || number == kSignalCount;
			std::printf("%2d) SIG%s%s", number, kSignals[number - 1], line_end ? "\n" : "\t");
		}

		return 0;
	}

	static int applyToSignals(Executor& exec, const std::string* action,
			const std::vector<std::string>& specs) {
		int status = 0;
		for (const std::string& spec : specs) {
			const std::string name = conditionName(spec);
			if (name.empty()) {
				reportInvalid(spec);
				status = 1;
				continue;
			}

			if (action == nullptr) exec.clearTrap(name);
			else exec.setTrap(name, *action);
		}

		return status;
	}

	static bool isUnsignedNumber(const std::string& text) {
		if (text.empty()) return false;

		for (const char c : text) {
			if (std::isdigit(static_cast<unsigned char>(c)) == 0) return false;
		}

		return true;
	}

	// `trap - SIG...`, `trap SIG` and POSIX's `trap N SIG...` (an unsigned
	// number first) reset; anything else installs the first word.
	static int setOrReset(Executor& exec, const std::vector<std::string>& operands) {
		const std::string& first = operands[0];
		const std::vector<std::string> conditions(operands.begin() + 1, operands.end());
		if (first == "-") return applyToSignals(exec, nullptr, conditions);
		if (operands.size() == 1 || isUnsignedNumber(first)) {
			return applyToSignals(exec, nullptr, operands);
		}

		return applyToSignals(exec, &first, conditions);
	}

	struct Options {
		bool list_signals = false;
		bool print = false;
		std::vector<std::string> operands;
	};

	static void parseOptions(const std::vector<std::string>& args, Options& options) {
		std::size_t i = 0;
		while (i < args.size() && args[i].size() > 1 && args[i][0] == '-') {
			const std::string& word = args[i];
			if (word == "--") {
				++i;
				break;
			}

			if (word == "-l") options.list_signals = true;
			else if (word == "-p") options.print = true;
			else if (word == "-lp" || word == "-pl") options.list_signals = options.print = true;
			else break;
			++i;
		}

		options.operands.assign(args.begin() + static_cast<std::ptrdiff_t>(i), args.end());
	}

	static int builtin_trap(Executor& exec, const std::vector<std::string>& args) {
		Options options;
		parseOptions(args, options);
		if (options.list_signals) return listSignals();
		if (options.print || options.operands.empty()) return printTraps(exec, options.operands);

		return setOrReset(exec, options.operands);
	}

}  // namespace wbsh::trap_detail

namespace wbsh {

	void registerTrapBuiltin(Executor& exec) {
		exec.registerBuiltin("trap", trap_detail::builtin_trap);
	}

}  // namespace wbsh

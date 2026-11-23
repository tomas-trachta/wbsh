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

#include "diagnostics.h"
#include "executor.h"
#include "numparse.h"
#include "signals.h"

namespace wbsh::trap_detail {

	static const char* const kPseudoSignals[] = { "DEBUG", "ERR", "RETURN" };
	static const int kSignalsPerListLine = 5;

	static std::string upper(std::string text) {
		for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		return text;
	}

	static bool isPseudoSignal(const std::string& name) {
		if (name == "EXIT") return true;
		for (const char* pseudo : kPseudoSignals) {
			if (name == pseudo) return true;
		}

		return false;
	}

	// `INT`, `SIGINT`, `sigint` and `2` all name the same trap; empty when
	// @p spec names none.
	static std::string conditionName(const std::string& spec) {
		const std::string name = upper(spec);
		if (isPseudoSignal(name)) return name;

		int number = 0;
		if (!parseSignalSpec(spec, number)) return std::string();
		return number == 0 ? std::string("EXIT") : std::string(signalName(number));
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

	// `-P` prints the action alone, `-p` the command that would set it.
	static void printTrap(const Executor& exec, const std::string& name, bool action_only) {
		const auto& traps = exec.listedTraps();
		const auto found = traps.find(name);
		if (found == traps.end()) return;

		if (action_only) {
			std::printf("%s\n", found->second.c_str());
			return;
		}

		std::printf("trap -- %s %s\n", quoted(found->second).c_str(), displayName(name).c_str());
	}

	static std::vector<std::string> listingOrder() {
		std::vector<std::string> names = { "EXIT" };
		for (int number = 1; number <= signalCount(); ++number) names.push_back(signalName(number));
		for (const char* pseudo : kPseudoSignals) names.push_back(pseudo);
		return names;
	}

	static void reportInvalid(const std::string& spec) {
		shellErrorf("trap: %s: invalid signal specification\n", spec.c_str());
	}

	static int printTraps(const Executor& exec, const std::vector<std::string>& specs,
			bool action_only) {
		if (specs.empty()) {
			for (const std::string& name : listingOrder()) printTrap(exec, name, action_only);
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

			printTrap(exec, name, action_only);
		}

		return status;
	}

	static int listSignals() {
		const int count = signalCount();
		for (int number = 1; number <= count; ++number) {
			const bool line_end = number % kSignalsPerListLine == 0 || number == count;
			std::printf("%2d) SIG%s%s", number, signalName(number), line_end ? "\n" : "\t");
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

	static void printUsage() {
		std::fprintf(stderr, "trap: usage: trap [-Plp] [[action] signal_spec ...]\n");
	}

	// As bash decides it: a first word that is a signal number resets every
	// operand; outside POSIX mode a lone signal name resets it; a lone word
	// that is no signal is a usage error; otherwise the first word is the
	// action, and `-` resets.
	static bool resetsAll(Executor& exec, const std::vector<std::string>& operands) {
		const std::string& first = operands[0];
		const bool is_signal = !conditionName(first).empty();
		if (isUnsignedNumber(first) && is_signal) return true;

		return !exec.env().options().posix && operands.size() == 1 && first != "-" && is_signal;
	}

	static int setOrReset(Executor& exec, const std::vector<std::string>& operands) {
		if (resetsAll(exec, operands)) return applyToSignals(exec, nullptr, operands);
		if (operands.size() == 1) {
			printUsage();
			return 2;
		}

		const std::string& first = operands[0];
		const std::vector<std::string> conditions(operands.begin() + 1, operands.end());
		if (first == "-") return applyToSignals(exec, nullptr, conditions);

		return applyToSignals(exec, &first, conditions);
	}

	struct Options {
		bool list_signals = false;
		bool print = false;
		bool print_actions = false;
		std::vector<std::string> operands;
	};

	static bool reportInvalidOption(char letter) {
		shellErrorf("trap: -%c: invalid option\n", letter);
		printUsage();
		return false;
	}

	static bool applyOptionLetters(const std::string& word, Options& options) {
		for (std::size_t k = 1; k < word.size(); ++k) {
			if (word[k] == 'l') options.list_signals = true;
			else if (word[k] == 'p') options.print = true;
			else if (word[k] == 'P') options.print_actions = true;
			else return reportInvalidOption(word[k]);
		}

		return true;
	}

	static bool optionsConflict(const Options& options) {
		if (options.print && options.print_actions) {
			shellErrorf("trap: cannot specify both -p and -P\n");
			return true;
		}

		if (options.print_actions && options.operands.empty() && !options.list_signals) {
			shellErrorf("trap: -P requires at least one signal name\n");
			return true;
		}

		return false;
	}

	static bool parseOptions(const std::vector<std::string>& args, Options& options) {
		std::size_t i = 0;
		while (i < args.size() && args[i].size() > 1 && args[i][0] == '-') {
			const std::string& word = args[i++];
			if (word == "--") break;
			if (!applyOptionLetters(word, options)) return false;
		}

		options.operands.assign(args.begin() + static_cast<std::ptrdiff_t>(i), args.end());
		return true;
	}

	static int builtin_trap(Executor& exec, const std::vector<std::string>& args) {
		Options options;
		if (!parseOptions(args, options) || optionsConflict(options)) return 2;
		if (options.list_signals) return listSignals();
		if (options.print || options.print_actions || options.operands.empty()) {
			return printTraps(exec, options.operands, options.print_actions);
		}

		return setOrReset(exec, options.operands);
	}

}  // namespace wbsh::trap_detail

namespace wbsh {

	std::string Executor::serializeListedTraps() const {
		std::string text;
		for (const std::string& name : trap_detail::listingOrder()) {
			const auto found = listedTraps().find(name);
			if (found == listedTraps().end()) continue;

			text += "trap -- " + trap_detail::quoted(found->second) + " " + name + "\n";
		}

		return text;
	}

	void registerTrapBuiltin(Executor& exec) {
		exec.registerBuiltin("trap", trap_detail::builtin_trap);
	}

}  // namespace wbsh

/**
 * @file builtins_fc.cpp
 * @brief The `fc` builtin: list, edit and run again commands from the history.
 *
 * Commands are named as bash names them: a history number, a negative
 * offset back from the current command, or the start of a command. In an
 * interactive shell the `fc` line itself is the newest history entry, so
 * offsets count from it and it never lists itself; a command `fc` runs
 * again takes its place in the history.
 */

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif /* _WIN32 */

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "diagnostics.h"
#include "executor.h"
#include "numparse.h"
#include "pathconv.h"

namespace wbsh::fc_detail {

	static const long long kDefaultListLength = 16;
	static const char* const kDefaultEditor = "notepad";
	static const char* const kUsage =
		"usage: fc [-e ename] [-lnr] [first] [last] or fc -s [pat=rep] [command]";

	struct Options {
		bool list = false;
		bool numbers = true;
		bool reverse = false;
		bool rerun = false;
		std::string editor;
		std::vector<std::string> operands;
	};

	static void printErr(const std::string& message) {
		shellErrorf("fc: %s\n", message.c_str());
	}

	static bool looksNegative(const std::string& arg) {
		return arg.size() > 1 && arg[0] == '-' && std::isdigit(static_cast<unsigned char>(arg[1]));
	}

	static bool takeFlags(const std::string& flags, Options& options) {
		for (std::size_t i = 1; i < flags.size(); ++i) {
			switch (flags[i]) {
			case 'l': options.list = true;     break;
			case 'n': options.numbers = false; break;
			case 'r': options.reverse = true;  break;
			case 's': options.rerun = true;    break;
			default:
				printErr("-" + std::string(1, flags[i]) + ": invalid option");
				return false;
			}
		}

		return true;
	}

	// Options end at the first operand, `--`, or a negative number, which
	// is an operand (`fc -l -5`).
	static bool parseOptions(const std::vector<std::string>& args, Options& options) {
		std::size_t i = 0;
		for (; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg == "--") {
				++i;
				break;
			}

			if (arg.size() < 2 || arg[0] != '-' || looksNegative(arg)) break;
			if (arg == "-e") {
				if (i + 1 >= args.size()) {
					printErr("-e: option requires an argument");
					return false;
				}

				options.editor = args[++i];
				continue;
			}

			if (!takeFlags(arg, options)) return false;
		}

		options.operands.assign(args.begin() + static_cast<std::ptrdiff_t>(i), args.end());
		if (options.editor == "-") options.rerun = true;
		return true;
	}

	static const long long kInvalid = LLONG_MIN;
	static const long long kNotFound = LLONG_MIN + 2;
	static const int kListing = 1;
	static const int kFirst = 2;

	// The history as bash's fc reads it, by index from 0: the newest
	// command before the `fc` line, and the newest entry of all.
	struct HistoryView {
		const std::vector<std::string>& entries;
		long long last;
		long long real_last;
		long long base;
	};

	static HistoryView viewOf(Executor& exec) {
		const std::vector<std::string>& history = exec.history();
		const long long total = static_cast<long long>(history.size());
		const long long added = exec.currentCommandInHistory() && total > 0 ? 1 : 0;
		const long long real_last = total > 0 ? total - 1 : 0;
		return { history, total - 1 - added, real_last, static_cast<long long>(exec.historyBase()) };
	}

	static bool startsWith(const std::string& text, const std::string& prefix) {
		return text.compare(0, prefix.size(), prefix) == 0;
	}

	// The number at the start of a spec, after an optional minus, as atoi
	// reads it.
	static bool leadingNumber(const std::string& spec, long long& sign, long long& value) {
		sign = !spec.empty() && spec[0] == '-' ? -1 : 1;
		std::size_t i = sign < 0 ? 1 : 0;
		const std::size_t start = i;
		value = 0;
		for (; i < spec.size() && std::isdigit(static_cast<unsigned char>(spec[i])); ++i) {
			if (value < LLONG_MAX / 100) value = value * 10 + (spec[i] - '0');
		}

		return i > start;
	}

	// Zero or less counts back from the newest command; a history number
	// outside the list takes the oldest entry when it starts a range and
	// the newest otherwise. `-0` is the `fc` line itself, only to list.
	static long long numberedIndex(const HistoryView& view, long long sign, long long value,
			int mode) {
		if (value == 0 && sign < 0) return (mode & kListing) != 0 ? view.real_last : kInvalid;

		const long long number = value * sign;
		if (number < 0) return (std::max)(number + view.last + 1, 0LL);
		if (number == 0) return view.last;

		const long long index = number - view.base;
		if (index < 0 || index >= view.last) return (mode & kFirst) != 0 ? 0 : view.last;

		return index;
	}

	static long long searchedIndex(const HistoryView& view, const std::string& spec) {
		for (long long i = view.last; i >= 0; --i) {
			if (startsWith(view.entries[static_cast<std::size_t>(i)], spec)) return i;
		}

		return kNotFound;
	}

	// bash's fc_gethnum: the index a spec names, or the newest command
	// when there is none.
	static long long historyIndex(const HistoryView& view, const std::string* spec, int mode) {
		if (view.last < 0) return -1;
		if (spec == nullptr) return view.last;

		long long sign = 1;
		long long value = 0;
		if (leadingNumber(*spec, sign, value)) return numberedIndex(view, sign, value, mode);

		return searchedIndex(view, *spec);
	}

	struct Range {
		long long first = 0;
		long long last = 0;
	};

	// Listing ends at the newest command, 16 back without operands; editing
	// takes the newest, or the one `first` names.
	static Range requestedRange(const HistoryView& view, const Options& options) {
		const std::vector<std::string>& ops = options.operands;
		const long long newest = (std::max)(view.last, 0LL);
		if (ops.empty()) {
			const long long back = options.list ? kDefaultListLength - 1 : 0;
			return { (std::max)(newest - back, 0LL), newest };
		}

		const int listing = options.list ? kListing : 0;
		Range range;
		range.first = historyIndex(view, &ops[0], listing | kFirst);
		if (ops.size() > 1) {
			range.last = historyIndex(view, &ops[1], listing);
		} else if (range.first == view.real_last) {
			range.last = options.list ? view.real_last : range.first;
		} else {
			range.last = options.list ? newest : range.first;
		}

		return range;
	}

	static bool reportsBadRange(const Range& range) {
		const auto either = [&range](long long code) {
			return range.first == code || range.last == code;
		};
		if (either(kInvalid)) {
			printErr("history specification out of range");
			return true;
		}

		if (either(kNotFound)) {
			printErr("no command found");
			return true;
		}

		return false;
	}

	static void clampToList(Range& range) {
		range.first = (std::max)(range.first, 0LL);
		range.last = (std::max)(range.last, 0LL);
	}

	// A range named backwards runs backwards, as does any range with -r.
	static std::vector<std::size_t> orderedIndexes(Range range, bool reverse, std::size_t total) {
		if (range.last < range.first) {
			std::swap(range.first, range.last);
			reverse = true;
		}

		std::vector<std::size_t> indexes;
		for (long long i = range.first; i <= range.last; ++i) {
			if (static_cast<std::size_t>(i) < total) indexes.push_back(static_cast<std::size_t>(i));
		}

		if (reverse) std::reverse(indexes.begin(), indexes.end());
		return indexes;
	}

	static void listEntries(Executor& exec, const std::vector<std::size_t>& indexes,
			const Options& options) {
		const HistoryView view = viewOf(exec);
		const char* separator = exec.env().options().posix ? "\t" : "\t ";
		for (const std::size_t i : indexes) {
			const long long number = static_cast<long long>(i) + view.base;
			const std::string label = options.numbers ? std::to_string(number) : std::string();
			std::printf("%s%s%s\n", label.c_str(), separator, view.entries[i].c_str());
		}
	}

	// `pat=rep` words before the command replace every `pat` in it.
	static std::string substituted(std::string command, const std::vector<std::string>& pairs) {
		for (const std::string& pair : pairs) {
			const std::size_t equals = pair.find('=');
			const std::string pattern = pair.substr(0, equals);
			const std::string replacement = pair.substr(equals + 1);
			if (pattern.empty()) continue;

			std::size_t at = 0;
			while ((at = command.find(pattern, at)) != std::string::npos) {
				command.replace(at, pattern.size(), replacement);
				at += replacement.size();
			}
		}

		return command;
	}

	// The command takes the place of the `fc` line in the history, is
	// shown, and runs.
	static int runCommands(Executor& exec, const std::string& commands) {
		std::fprintf(stderr, "%s\n", commands.c_str());
		std::fflush(stderr);

		if (exec.currentCommandInHistory()) exec.replaceLastHistoryEntry(commands);
		return exec.executeText(commands, "fc");
	}

	static int rerun(Executor& exec, const Options& options) {
		std::vector<std::string> pairs;
		std::vector<std::string> rest;
		for (const std::string& operand : options.operands) {
			const bool pair = rest.empty() && operand.find('=') != std::string::npos;
			(pair ? pairs : rest).push_back(operand);
		}

		const HistoryView view = viewOf(exec);
		const long long index = historyIndex(view, rest.empty() ? nullptr : &rest[0], 0);
		if (index < 0) {
			printErr("no command found");
			return 1;
		}

		if (exec.env().options().posix && rest.size() > 1) {
			printErr("too many arguments");
			return 1;
		}

		const std::string& command = view.entries[static_cast<std::size_t>(index)];
		return runCommands(exec, substituted(command, pairs));
	}

	static std::string temporaryFile() {
#ifdef _WIN32
		wchar_t dir[MAX_PATH + 1] = {};
		if (::GetTempPathW(MAX_PATH + 1, dir) == 0) return {};

		return wideToUtf8(dir) + "wbsh-fc-" + std::to_string(::GetCurrentProcessId()) + ".sh";
#else
		return "/tmp/wbsh-fc.sh";
#endif /* _WIN32 */
	}

	static std::string editorFor(Executor& exec, const Options& options) {
		if (!options.editor.empty()) return options.editor;

		for (const char* name : { "FCEDIT", "EDITOR" }) {
			const std::string value = exec.env().get(name);
			if (!value.empty()) return value;
		}

		return kDefaultEditor;
	}

	static std::string singleQuoted(const std::string& text) {
		std::string quoted = "'";
		for (const char letter : text) {
			if (letter == '\'') quoted += "'\\''";
			else quoted.push_back(letter);
		}

		return quoted + "'";
	}

	static std::string fileText(const std::string& path) {
		std::ifstream file(utf8ToPath(path), std::ios::binary);
		std::stringstream text;
		text << file.rdbuf();

		std::string content = text.str();
		while (!content.empty() && (content.back() == '\n' || content.back() == '\r')) {
			content.pop_back();
		}

		return content;
	}

	static void writeEntries(Executor& exec, const std::string& path,
			const std::vector<std::size_t>& indexes) {
		std::ofstream file(utf8ToPath(path), std::ios::binary);
		for (const std::size_t i : indexes) file << exec.history()[i] << '\n';
	}

	// "When not listing, the fc command that caused the editing shall not
	// be entered into the history list": the range stops at the new end.
	static bool dropFcLine(Executor& exec, Range& range) {
		if (!exec.dropCurrentHistoryEntry()) return false;

		const long long last = static_cast<long long>(exec.history().size()) - 1;
		if (range.last >= last) range.last = last;
		else if (range.first >= last) range.first = last;

		return true;
	}

	// The commands go to a file for the editor; whatever it leaves there
	// runs and takes the `fc` line's place, unless the editor failed.
	static int editAndRun(Executor& exec, const Options& options, Range range) {
		const bool dropped = dropFcLine(exec, range);
		clampToList(range);
		const std::string path = temporaryFile();
		writeEntries(exec, path, orderedIndexes(range, options.reverse, exec.history().size()));

		const std::string posix_path = exec.pathConv().toPosix(path);
		const int edited = exec.executeText(editorFor(exec, options) + " "
			+ singleQuoted(posix_path), "fc");
		const std::string commands = fileText(path);
		std::error_code ec;
		std::filesystem::remove(utf8ToPath(path), ec);
		if (edited != 0) return 1;
		if (commands.empty()) return 0;

		if (dropped) exec.addHistoryEntry(commands);
		return runCommands(exec, commands);
	}

	static int builtin_fc(Executor& exec, const std::vector<std::string>& args) {
		Options options;
		if (!parseOptions(args, options)) {
			std::fprintf(stderr, "fc: %s\n", kUsage);
			return 2;
		}

		if (options.rerun) return rerun(exec, options);

		Range range = requestedRange(viewOf(exec), options);
		if (reportsBadRange(range)) return 1;
		if (!options.list) return editAndRun(exec, options, range);

		clampToList(range);
		listEntries(exec, orderedIndexes(range, options.reverse, exec.history().size()), options);
		return 0;
	}

}  // namespace wbsh::fc_detail

namespace wbsh {

	void registerFcBuiltin(Executor& exec) {
		exec.registerBuiltin("fc", fc_detail::builtin_fc);
	}

}  // namespace wbsh

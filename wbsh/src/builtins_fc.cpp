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
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "executor.h"
#include "numparse.h"
#include "pathconv.h"

namespace wbsh::fc_detail {

	static const std::size_t kDefaultListLength = 16;
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
		std::fprintf(stderr, "wbsh: fc: %s\n", message.c_str());
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

	// The history as fc sees it: the entries before the current command,
	// numbered from 1.
	struct HistoryView {
		const std::vector<std::string>& entries;
		std::size_t count;   // entries older than the current command
	};

	static HistoryView viewOf(Executor& exec) {
		const std::vector<std::string>& history = exec.history();
		const bool holds_current = exec.currentCommandInHistory() && !history.empty();
		return { history, holds_current ? history.size() - 1 : history.size() };
	}

	static bool startsWith(const std::string& text, const std::string& prefix) {
		return text.compare(0, prefix.size(), prefix) == 0;
	}

	// A number, an offset back from the current command, or the newest
	// command starting with the text. 0 when nothing matches; a number
	// past the newest entry is out of range.
	static bool resolveSpec(const HistoryView& view, const std::string& spec, std::size_t& number) {
		long long value = 0;
		if (parseLL(spec, value)) {
			const long long current = static_cast<long long>(view.count) + 1;
			const long long resolved = value <= 0 ? current + value : value;
			if (resolved > static_cast<long long>(view.count) || view.count == 0) return false;

			number = resolved < 1 ? 1 : static_cast<std::size_t>(resolved);
			return true;
		}

		for (std::size_t i = view.count; i > 0; --i) {
			if (!startsWith(view.entries[i - 1], spec)) continue;

			number = i;
			return true;
		}

		number = 0;
		return true;
	}

	static bool resolveOrReport(const HistoryView& view, const std::string& spec,
	                            std::size_t& number) {
		if (!resolveSpec(view, spec, number)) {
			printErr("history specification out of range");
			return false;
		}

		if (number == 0) {
			printErr("no command found");
			return false;
		}

		return true;
	}

	struct Range {
		std::size_t first = 0;
		std::size_t last = 0;
	};

	// Listing starts 16 commands back and ends at the newest; editing
	// takes the newest, and a single command when only `first` is named.
	static bool resolveRange(const HistoryView& view, const Options& options, Range& range) {
		const std::vector<std::string>& ops = options.operands;
		const std::size_t back = options.list ? kDefaultListLength : 1;
		const std::string first_spec = ops.empty() ? "-" + std::to_string(back) : ops[0];
		const std::string last_spec = ops.size() > 1 ? ops[1]
			: (options.list || ops.empty() ? "-1" : first_spec);

		return resolveOrReport(view, first_spec, range.first)
		    && resolveOrReport(view, last_spec, range.last);
	}

	static void listRange(const HistoryView& view, const Options& options, Range range) {
		bool descending = range.first > range.last;
		if (options.reverse) descending = !descending;
		const std::size_t low = (std::min)(range.first, range.last);
		const std::size_t high = (std::max)(range.first, range.last);

		for (std::size_t k = 0; k <= high - low; ++k) {
			const std::size_t number = descending ? high - k : low + k;
			const std::string label = options.numbers ? std::to_string(number) : std::string();
			std::printf("%s\t %s\n", label.c_str(), view.entries[number - 1].c_str());
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
		std::size_t number = 0;
		if (!resolveOrReport(view, rest.empty() ? "-1" : rest[0], number)) return 1;

		return runCommands(exec, substituted(view.entries[number - 1], pairs));
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

	static void writeRange(const std::string& path, const HistoryView& view, Range range) {
		std::ofstream file(utf8ToPath(path), std::ios::binary);
		const bool descending = range.first > range.last;
		const std::size_t low = (std::min)(range.first, range.last);
		const std::size_t high = (std::max)(range.first, range.last);
		for (std::size_t k = 0; k <= high - low; ++k) {
			file << view.entries[(descending ? high - k : low + k) - 1] << '\n';
		}
	}

	// The commands go to a file for the editor; whatever it leaves there
	// runs, unless the editor failed.
	static int editAndRun(Executor& exec, const Options& options, const Range& range) {
		const std::string path = temporaryFile();
		writeRange(path, viewOf(exec), range);

		const std::string posix_path = exec.pathConv().toPosix(path);
		const int edited = exec.executeText(editorFor(exec, options) + " "
			+ singleQuoted(posix_path), "fc");
		const std::string commands = fileText(path);
		std::error_code ec;
		std::filesystem::remove(utf8ToPath(path), ec);
		if (edited != 0) return edited;
		if (commands.empty()) return 0;

		return runCommands(exec, commands);
	}

	static int builtin_fc(Executor& exec, const std::vector<std::string>& args) {
		Options options;
		if (!parseOptions(args, options)) {
			printErr(kUsage);
			return 2;
		}

		if (options.rerun) return rerun(exec, options);

		const HistoryView view = viewOf(exec);
		Range range;
		if (!resolveRange(view, options, range)) return 1;

		if (options.list) {
			listRange(view, options, range);
			return 0;
		}

		return editAndRun(exec, options, range);
	}

}  // namespace wbsh::fc_detail

namespace wbsh {

	void registerFcBuiltin(Executor& exec) {
		exec.registerBuiltin("fc", fc_detail::builtin_fc);
	}

}  // namespace wbsh

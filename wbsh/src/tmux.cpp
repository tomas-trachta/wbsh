/**
 * @file tmux.cpp
 * @brief `tmux` -- hands the session to a terminal that hosts panes.
 *
 * There is no server and nothing to attach to: wbshterm draws the panes
 * itself and keeps them only as long as its window lives. This tells it
 * to start; everything after that is the prefix key. The subcommands
 * are refused rather than half-answered, because a `tmux attach` that
 * silently did nothing would be worse than one that says why it cannot.
 */

#include "coreutils_internal.h"
#include "executor.h"
#include "termreq.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace wbsh {

	static bool terminalHostsPanes() {
		const char* flag = std::getenv("WBSHTERM_PANES");
		return flag != nullptr && *flag != '0';
	}

	static void printTmuxUsage() {
		std::fputs(
			"usage: tmux\n"
			"\n"
			"Starts pane mode in a terminal that hosts panes itself. Panes are\n"
			"then driven from the prefix key, Ctrl-B unless the terminal's\n"
			"config says otherwise:\n"
			"\n"
			"  prefix %       split into columns, side by side\n"
			"  prefix \"       split into rows, one above the other\n"
			"  prefix arrows  move the focus that way\n"
			"  prefix o       focus the next pane\n"
			"  prefix z       zoom the focused pane, or put it back\n"
			"  prefix x       close the focused pane\n"
			"  prefix C-b     send Ctrl-B on to the shell\n",
			stdout);
	}

	static int builtin_tmux(Executor&, const std::vector<std::string>& args) {
		if (!args.empty() && (args[0] == "-h" || args[0] == "--help")) {
			printTmuxUsage();
			return 0;
		}

		if (!args.empty()) {
			perr("tmux", "no server here -- the terminal hosts the panes, so only"
				" a bare `tmux` works. See `tmux --help`.");
			return 1;
		}

		if (!terminalHostsPanes()) {
			perr("tmux", "this terminal does not host panes; run wbshterm");
			return 1;
		}

		writeTerminalRequest("\x1b]1337;tmux;attach\a");
		return 0;
	}

	void registerTmuxBuiltin(Executor& exec) {
		exec.registerBuiltin("tmux", builtin_tmux);
	}

}  // namespace wbsh

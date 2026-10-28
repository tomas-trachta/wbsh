#pragma once

/**
 * @file gitprompt.h
 * @brief The state of the git repository around the cwd, for the prompt's `\g`.
 *
 * The branch and an operation in progress (merge, rebase, ...) come from
 * files under the git dir. Whether the work tree is dirty takes
 * `git status`, which is slow in a big repository: it runs git directly
 * (no cmd.exe) on a thread of its own, the prompt waits for it only
 * briefly, and otherwise shows the state last seen for that repository
 * until the new one arrives and the prompt is drawn again.
 */

#include <functional>
#include <string>

namespace wbsh {

	class Environment;
	class PathConv;

	struct GitInfo {
		std::string branch;   // empty outside a repository
		std::string state;    // merging, rebasing, ..., unstaged, staged, untracked or empty
	};

	/**
	 * The repository's state for a prompt being built now, with git found
	 * on @p env's PATH. With @p can_redraw false (no terminal to redraw
	 * on) it waits for `git status` however long it takes.
	 */
	GitInfo gitPromptInfo(const Environment& env, const PathConv& conv, bool can_redraw);

	/**
	 * Called on the status thread when a state arrives that differs from
	 * the one the shown prompt was built with; the prompt should be built
	 * again, which then picks the new state up without another run.
	 */
	void setGitStatusListener(std::function<void()> listener);

}  // namespace wbsh

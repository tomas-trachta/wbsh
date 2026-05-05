#pragma once

/**
 * @file setup.h
 * @brief Process-environment seeding and child-shell state inheritance.
 *
 * Bridges the host process's environment block into the shell's
 * Environment object on startup, and re-hydrates inherited shell
 * state (aliases, functions, locals) when wbsh is launched as a
 * child of another wbsh.
 */

#include "environment.h"
#include "executor.h"

namespace wbsh {

	/**
	 * @brief Seed @p env from the host process environment.
	 *
	 * Normalises the inherited environment for shell use: PATH is
	 * translated to POSIX form, HOME / PWD are set, and the well-known
	 * install dirs of git / docker are prepended so they're
	 * discoverable even when wbsh was launched from Explorer (whose
	 * inherited PATH may miss them).
	 */
	void prepareEnv(Environment& env);

	/**
	 * @brief Re-hydrate inherited shell state from `WBSH_*` env vars.
	 *
	 * When wbsh is launched as a child of another wbsh, the parent
	 * stashes alias / function / local-name state in `WBSH_*` env
	 * vars (since the OS environment can only carry strings). This
	 * drains those stashes back into the live Executor so the child
	 * sees the same shell state.
	 */
	void absorbInheritedState(Environment& env, Executor& exec);

}  // namespace wbsh

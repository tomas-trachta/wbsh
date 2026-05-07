#pragma once

/**
 * @file coreutils_internal.h
 * @brief Internal split-points between coreutils translation units.
 *
 * coreutils.cpp is the main bundled-coreutils entry; some self-contained
 * subsystems (the `bc` calculator, future archive / encoding splits) live
 * in their own files and expose only a per-category register function so
 * that registerCoreutils() can wire them in without seeing their internals.
 */

namespace wbsh {

	class Executor;

	/// Wire the `bc` builtin into `exec`. Defined in coreutils_bc.cpp.
	void registerBcBuiltin(Executor& exec);

	/// Wire the `curl` builtin into `exec`. Defined in coreutils_curl.cpp.
	void registerCurlBuiltin(Executor& exec);

	/// Wire `md5sum` / `sha1sum` / `sha256sum` / `sha512sum` into `exec`.
	/// Defined in coreutils_hash.cpp.
	void registerHashBuiltins(Executor& exec);

}  // namespace wbsh

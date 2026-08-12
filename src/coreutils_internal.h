#pragma once

/**
 * @file coreutils_internal.h
 * @brief Internal split-points between coreutils translation units.
 *
 * coreutils.cpp is the main bundled-coreutils entry; self-contained
 * subsystems (text processing, archives, encodings, the `bc`
 * calculator, `curl`, hashing) live in their own files and expose only
 * a per-category register function so that registerCoreutils() can
 * wire them in without seeing their internals.
 */

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace wbsh {

	class Executor;

	void perr(const std::string& cmd, const std::string& path,
	          const std::error_code& ec);
	void perr(const std::string& cmd, const std::string& msg);

	/// Translate an arg-style path (POSIX-or-Win32, UTF-8) to a native
	/// filesystem::path without mojibake on non-ASCII names.
	std::filesystem::path toNative(Executor& exec, const std::string& p);

	/// fopen on a UTF-8, POSIX-or-Win32 path (`_wfopen` on Windows).
	std::FILE* fopenNative(Executor& exec, const std::string& p, const char* mode);

	/// Append every line of `path` ("-" = stdin) to `lines`, stripping
	/// the trailing `\n` / `\r\n`. Returns false if the file can't be
	/// opened (errno is set).
	bool readAllLines(Executor& exec, const std::string& path,
	                  std::vector<std::string>& lines);

	void registerTextBuiltins(Executor& exec);

	void registerEncodingBuiltins(Executor& exec);

	void registerArchiveBuiltins(Executor& exec);

	void registerBcBuiltin(Executor& exec);

	void registerCurlBuiltin(Executor& exec);

	void registerHashBuiltins(Executor& exec);

	void registerFzfBuiltin(Executor& exec);

}  // namespace wbsh

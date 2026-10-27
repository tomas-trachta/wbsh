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

#include <cstdint>
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

	/// zlib / gzip / zip CRC-32; pass 0 to start, the previous result to continue.
	std::uint32_t crc32Update(std::uint32_t crc, const std::uint8_t* data, std::size_t length);

	/// $USER, then $USERNAME, then "user".
	std::string currentUserName(Executor& exec);

	/// True only for a real console: _isatty also says yes for NUL.
	bool isConsoleStream(std::FILE* stream);

	/// Visible console window size; false when stdout is not a console.
	bool queryConsoleSize(int& columns, int& rows);

	/// A count with an optional GNU size suffix: b (512), kB / KB (1000), K / KiB
	/// (1024), and the same for M, G, T, P, E; dd's c (1) and w (2) as well.
	bool parseByteSize(const std::string& text, unsigned long long& bytes);

	/// `sleep`-style interval: a number with an optional s / m / h / d unit.
	bool parseDurationSeconds(const std::string& arg, double& seconds);

	void registerHeadTailBuiltins(Executor& exec);

	void registerTextBuiltins(Executor& exec);

	void registerTrBuiltin(Executor& exec);

	void registerSortBuiltin(Executor& exec);

	void registerEncodingBuiltins(Executor& exec);

	void registerDumpBuiltins(Executor& exec);

	void registerArchiveBuiltins(Executor& exec);

	void registerCompressBuiltins(Executor& exec);

	void registerTarBuiltin(Executor& exec);

	void registerPatchBuiltin(Executor& exec);

	void registerPagerBuiltins(Executor& exec);

	void registerWgetBuiltin(Executor& exec);

	void registerMiscBuiltins(Executor& exec);

	void registerCpioBuiltins(Executor& exec);

	void registerSedBuiltin(Executor& exec);

	void registerGrepBuiltins(Executor& exec);

	void registerBcBuiltin(Executor& exec);

	void registerCurlBuiltin(Executor& exec);

	void registerHashBuiltins(Executor& exec);

	void registerProcessBuiltins(Executor& exec);

	void registerSystemExtraBuiltins(Executor& exec);

	void registerFileOpsBuiltins(Executor& exec);

	void registerFilterBuiltins(Executor& exec);

	void registerFzfBuiltin(Executor& exec);
	void registerTmuxBuiltin(Executor& exec);
	void registerUtilsBuiltin(Executor& exec);

}  // namespace wbsh

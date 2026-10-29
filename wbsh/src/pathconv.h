#pragma once

/**
 * @file pathconv.h
 * @brief UTF-8 ↔ native path conversion and POSIX ↔ Win32 path translation.
 *
 * Two distinct concerns live in this file:
 *
 * 1. **Encoding helpers.** wbsh keeps all paths internally as UTF-8.
 *    The MSVC implementation of `std::filesystem::path(const std::string&)`
 *    and `path::string()` interprets / produces narrow strings using
 *    the active narrow codepage (CP_ACP), not UTF-8 — so a UTF-8 byte
 *    sequence like the one for "Tomáš" round-trips through CP_ACP
 *    and lands on disk as mojibake (`TomÃ¡Å¡`). The helpers below
 *    route through a wide string so the bytes survive intact.
 *
 * 2. **PathConv** — a Cygwin/MSYS-style mount table that maps
 *    `/c/Users/...` ↔ `C:\Users\...` and friends, used everywhere
 *    POSIX paths cross into native Windows APIs and back.
 */

#include <cstdio>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace wbsh {

	std::wstring utf8ToWide(const std::string& s);
	std::string  wideToUtf8(const std::wstring& w);

	/// The running executable's own path (native form, any length);
	/// "wbsh.exe" when Windows cannot tell it.
	std::string selfExecutablePath();

	/// Builds the path from the wide form, bypassing the CP_ACP
	/// interpretation that `path(std::string)` applies on MSVC. A path
	/// longer than plain Win32 calls take comes back in its `\\?\` form,
	/// so std::filesystem works on it. A short relative name stays relative
	/// (it may be a component still to be joined); see longPathSafe.
	std::filesystem::path utf8ToPath(const std::string& s);

	/// Reads the native (wide) form, bypassing the CP_ACP downgrade
	/// that `path::string()` applies on MSVC. A `\\?\` prefix is dropped,
	/// so a long path reads back the way the user writes it.
	std::string pathToUtf8(const std::filesystem::path& p);

	/// utf8ToWide for a file name handed straight to a Win32 / CRT file
	/// API: a path longer than plain Win32 calls take gets its `\\?\` form.
	std::wstring utf8ToFileWide(const std::string& s);

	/// @p p in its `\\?\` (or `\\?\UNC\`) form, absolute and normalised, when
	/// it is too long for Win32 calls without one (MAX_PATH less room for an
	/// 8.3 name, counted after resolving it against the current directory);
	/// otherwise @p p unchanged. Recursive walkers pass the paths they build
	/// through this; the children of a prefixed path inherit the prefix.
	std::filesystem::path longPathSafe(const std::filesystem::path& p);

	/// @p p without a `\\?\` prefix: the form the current directory has to
	/// be set with, because child processes inherit it.
	std::filesystem::path withoutExtendedPrefix(const std::filesystem::path& p);

	/// @p p with 8.3 short names expanded to the long ones (GetLongPathName);
	/// @p p unchanged when it cannot be resolved. A child started in a
	/// directory past MAX_PATH is given its short form, and reads it back
	/// through this.
	std::filesystem::path expandShortNames(const std::filesystem::path& p);

	/// `fopen` on a UTF-8 path (`_wfopen` on Windows, long paths included).
	std::FILE* openUtf8(const std::string& utf8_path, const char* mode);

	/// `stat` on a Win32-form path that also sees Windows App Execution
	/// Aliases (0-byte APPEXECLINK reparse points such as winget.exe):
	/// `::stat` and `std::filesystem::status` try to open those and
	/// fail, so the fallback synthesises the entry from file attributes.
	bool statPath(const std::string& win32_path, struct stat& info);

	/// True for an AF_UNIX socket file (Windows 10 1803 and later): a
	/// reparse point with the AF_UNIX tag, which std::filesystem and
	/// `stat` take for an unreadable regular file. Not followed if a link.
	bool isUnixSocketFile(const std::filesystem::path& native);

	/// The last-write time of @p native from its directory entry, which
	/// a socket file has although it cannot be opened.
	bool entryWriteTime(const std::filesystem::path& native,
	                    std::filesystem::file_time_type& time);

	/**
	 * @brief Bidirectional POSIX ↔ Win32 path translation.
	 *
	 * Modelled on the MSYS / Cygwin "mount table" approach. Default
	 * mounts:
	 *
	 *   /a, /b, ..., /z              -> A:\, B:\, ..., Z:\         (drive letters)
	 *   /tmp                         -> %TEMP%
	 *   /dev/null                    -> NUL
	 *   /dev/stdin / stdout / stderr -> CONIN$ / CONOUT$ / CONOUT$ (best-effort)
	 *
	 * The class is cheap to copy; it stores a small table of mount
	 * entries.
	 */
	class PathConv {
	public:
		PathConv();

		/// Idempotent: already-Windows paths (drive letter, UNC, or
		/// backslashes) are returned unchanged.
		std::string toWin32(const std::string& p) const;

		/// Idempotent.
		std::string toPosix(const std::string& p) const;

		/**
		 * @brief Translate to Win32 form, then collapse to 8.3 short
		 *        path when possible.
		 *
		 * Example: `C:\Users\Tomáš` → `C:\Users\TOMA~1`. Used for
		 * `HOME` and similar env vars passed to MinGW-built children
		 * (Git-for-Windows et al.) whose ANSI `getenv` reads the env
		 * block through CP_ACP and silently mangles non-ASCII
		 * characters that the codepage can't represent. Short paths
		 * are pure ASCII, so they survive any transcoding.
		 *
		 * Falls back to the long Win32 form when the path is already
		 * ASCII, the path doesn't exist, or short-name generation is
		 * disabled on the volume (`fsutil 8dot3name`).
		 */
		std::string toWin32Short(const std::string& p) const;

		static bool isWin32Absolute(const std::string& p);
		static bool isPosixAbsolute(const std::string& p);
		static bool looksLikeWin32(const std::string& s);

		std::string pathListWin32ToPosix(const std::string& list) const;
		std::string pathListPosixToWin32(const std::string& list) const;

		/// Heuristic for native callees: true when the argument looks
		/// like a POSIX absolute path and not a flag, a URL, or a
		/// single-char arg like `/`.
		bool argLooksTranslatable(const std::string& arg) const;

		/// Returns the Win32 form when the argument looks translatable,
		/// otherwise the input unchanged. `--opt=PATH` / `-X=PATH` get
		/// only their value side translated.
		std::string translateArg(const std::string& arg) const;

	private:
		struct Mount {
			std::string posix;      ///< Exact (e.g. `/dev/null`) or prefix (e.g. `/c`).
			std::string win32;      ///< Native counterpart.
			bool exact;             ///< True for exact match, false for prefix match.
		};

		const Mount* findMount(const std::string& p) const;
		bool applyMount(const std::string& p, std::string& out) const;

		std::vector<Mount> mounts_;
	};

}  // namespace wbsh

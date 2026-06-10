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
#include <vector>

namespace wbsh {

	std::wstring utf8ToWide(const std::string& s);
	std::string  wideToUtf8(const std::wstring& w);

	/**
	 * @brief Build a `std::filesystem::path` from a UTF-8 string.
	 *
	 * On Windows, converts UTF-8 → wide first and constructs the path
	 * from the wide string, avoiding the active-codepage
	 * interpretation that `path(string)` would otherwise apply.
	 */
	std::filesystem::path utf8ToPath(const std::string& s);

	/**
	 * @brief Encode a `std::filesystem::path` as UTF-8.
	 *
	 * On Windows reads the path's native (wide) representation and
	 * converts to UTF-8, avoiding the codepage downgrade that
	 * `path::string()` does on MSVC.
	 */
	std::string pathToUtf8(const std::filesystem::path& p);

	/**
	 * @brief `fopen` wrapper that takes a UTF-8 path.
	 *
	 * On Windows uses `_wfopen` on the wide form so non-ASCII
	 * filenames don't get downgraded through the active codepage.
	 */
	std::FILE* openUtf8(const std::string& utf8_path, const char* mode);

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

		/**
		 * @brief Translate a POSIX-style path to a Windows path.
		 *
		 * Idempotent: already-Windows paths (drive letter, UNC, or
		 * backslashes) are returned unchanged.
		 */
		std::string toWin32(const std::string& p) const;

		/// Translate a Windows path to POSIX form. Idempotent.
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

		/**
		 * @brief Heuristic: should @p arg be translated for a native callee?
		 *
		 * Yes when it looks like a POSIX absolute path (starts with
		 * `/`), is not a flag (`-...`), is not a URL (no `://`),
		 * and is not a single-char arg like `/`.
		 */
		bool argLooksTranslatable(const std::string& arg) const;

		/**
		 * @brief Translate a single argument for a native Win32 callee.
		 *
		 * Returns the Win32 form when the argument looks
		 * translatable, otherwise the input unchanged. Arguments of
		 * the form `--opt=PATH` or `-X=PATH` get only their value
		 * side translated.
		 */
		std::string translateArg(const std::string& arg) const;

	private:
		struct Mount {
			std::string posix;      ///< Exact (e.g. `/dev/null`) or prefix (e.g. `/c`).
			std::string win32;      ///< Native counterpart.
			bool exact;             ///< True for exact match, false for prefix match.
		};

		/// Try @p p against every mount; on success fills @p out, returns true.
		bool applyMount(const std::string& p, std::string& out) const;

		std::vector<Mount> mounts_;
	};

}  // namespace wbsh

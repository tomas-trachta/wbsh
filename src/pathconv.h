#pragma once

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace wbsh {

	// ---- UTF-8 <-> native (wide / fs::path) conversion utilities ----
	//
	// wbsh stores all paths and strings internally as UTF-8. The MSVC
	// implementation of `std::filesystem::path(const std::string&)` and
	// `path::string()` interprets / produces narrow strings using the
	// **active narrow codepage** (CP_ACP), not UTF-8 — so a UTF-8 byte
	// sequence like the one for "Tomáš" round-trips through the active
	// codepage and lands on disk as mojibake (`TomÃ¡Å¡`). These helpers
	// route through a wide string so the bytes survive intact on Windows.

	std::wstring utf8ToWide(const std::string& s);
	std::string  wideToUtf8(const std::wstring& w);

	// Build a `std::filesystem::path` from a UTF-8 string. On Windows
	// converts UTF-8 -> wide and constructs the path from the wide string,
	// avoiding the active-codepage interpretation of `path(string)`.
	std::filesystem::path utf8ToPath(const std::string& s);

	// Encode a `std::filesystem::path` as UTF-8. On Windows reads the
	// path's native (wide) representation and converts to UTF-8, avoiding
	// the codepage downgrade that `path::string()` does on MSVC.
	std::string pathToUtf8(const std::filesystem::path& p);

	// fopen wrapper that takes a UTF-8 path. On Windows uses `_wfopen` on
	// the wide form so non-ASCII filenames don't get downgraded through
	// the active codepage. `mode` is ASCII (e.g. "rb", "wb", "ab").
	std::FILE* openUtf8(const std::string& utf8_path, const char* mode);

	// Bidirectional POSIX <-> Win32 path translation, modelled on the
	// MSYS / Cygwin "mount table" approach. Default mounts:
	//
	//   /a, /b, ..., /z              -> A:\, B:\, ..., Z:\         (drive letters)
	//   /tmp                         -> %TEMP%
	//   /dev/null                    -> NUL
	//   /dev/stdin / stdout / stderr -> CONIN$ / CONOUT$ / CONOUT$ (best-effort)
	//
	// The class is cheap to copy; it stores a small table of mount entries.
	class PathConv {
	public:
		PathConv();

		// Translate a POSIX-style path to a Windows path. Idempotent:
		// already-Windows paths (drive letter, UNC, or backslashes) are
		// returned unchanged.
		std::string toWin32(const std::string& p) const;

		// Translate a Windows path to POSIX form. Idempotent.
		std::string toPosix(const std::string& p) const;

		// Translate to Win32 form, then collapse to the 8.3 short-path form
		// (e.g., "C:\Users\Tomáš" -> "C:\Users\TOMA~1") when possible. Used
		// for HOME and similar env vars passed to MinGW-built children
		// (Git-for-Windows et al.) whose ANSI `getenv` reads the env block
		// through the active codepage (CP_ACP) and silently mangles
		// non-ASCII characters that the codepage can't represent. Short
		// paths are pure ASCII, so they survive any transcoding.
		// Falls back to the long Win32 form when the path is already ASCII,
		// the path doesn't exist, or short-name generation is disabled on
		// the volume (`fsutil 8dot3name`).
		std::string toWin32Short(const std::string& p) const;

		// Quick classification helpers.
		static bool isWin32Absolute(const std::string& p);
		static bool isPosixAbsolute(const std::string& p);
		static bool looksLikeWin32(const std::string& s);  // contains backslash or X:

		// PATH-list conversion (entries separated by ':' for POSIX, ';' for
		// Windows). Each entry is independently translated.
		std::string pathListWin32ToPosix(const std::string& list) const;
		std::string pathListPosixToWin32(const std::string& list) const;

		// Heuristic: should this argument be translated when passed to a
		// native Win32 executable? Yes when it looks like a POSIX absolute
		// path (starts with `/`) and not a flag (doesn't start with `-`),
		// not a URL (no `://`), and not a single-char arg like `/`.
		bool argLooksTranslatable(const std::string& arg) const;

		// Translate a single argument for a native Win32 callee. If the
		// argument looks translatable, returns the Win32 form; otherwise
		// returns the input unchanged. Arguments of the form `--opt=PATH`
		// or `-X=PATH` get only their value side translated.
		std::string translateArg(const std::string& arg) const;

	private:
		struct Mount {
			std::string posix;   // exact (e.g. "/dev/null") or prefix (e.g. "/c")
			std::string win32;
			bool exact;
		};

		// Try to match `p` against any mount. On success, fills `out` and
		// returns true.
		bool applyMount(const std::string& p, std::string& out) const;

		std::vector<Mount> mounts_;
	};

}  // namespace wbsh

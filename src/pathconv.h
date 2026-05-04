#pragma once

#include <string>
#include <vector>

namespace wbsh {

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

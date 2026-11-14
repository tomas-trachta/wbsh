#pragma once

/**
 * @file filemode.h
 * @brief POSIX permission bits for Windows files: the one model chmod,
 *        test, ls, stat, find and tar share.
 *
 * The owner write bit is the read-only attribute. The other bits come from
 * the mode chmod left in the file's `wbsh.mode` stream when it differs from
 * the default; without one, a directory is 0755 and a file is 0755 when its
 * name ends in .exe .com .bat .cmd .ps1 or .sh or it starts with `#!` or
 * `MZ`, else 0644.
 */

#include <filesystem>
#include <string>
#include <system_error>

namespace wbsh {

	/// The rwxrwxrwx bits of an existing file or directory (links followed).
	unsigned filePermissions(const std::filesystem::path& native, bool is_directory,
	                         bool read_only);

	/// What `test -x` asks: the owner execute bit alone, without reading attributes.
	bool hasOwnerExecute(const std::filesystem::path& native, bool is_directory);

	/// The read-only attribute follows the owner write bit of @p mode; the rest
	/// is stored only when it differs from the default. The modification time
	/// is kept.
	bool setFilePermissions(const std::filesystem::path& native, unsigned mode,
	                        std::error_code& ec);

	/// Applies an octal or symbolic (`u+x,go-w`, `a=rX`, `g=u`) mode to @p mode.
	/// S_IFDIR in @p mode makes `X` apply; @p umask limits clauses without a who.
	bool applyModeText(const std::string& text, unsigned umask, unsigned& mode);

}  // namespace wbsh

#pragma once

/**
 * @file file_links.h
 * @brief Paths printed on screen as Ctrl+click targets: finding the one
 *        under the pointer, its :line and :column, and the file it names.
 *
 * Compilers, linters and grep print `src/main.cpp:42:7`, MSVC prints
 * `main.cpp(42,7)`, and an MSYS tool prints `/c/Users/me/x.txt`; all of
 * them come back as one path with its line and column. Everything here is
 * text in, text out, so it can be checked without a window.
 */

#include <string>
#include <vector>

namespace wbshterm {

	struct FileLink {
		std::string path;   ///< as printed, without its line and column
		int line   = 0;     ///< 0 when none was given
		int column = 0;
	};

	/**
	 * @brief The path-like word that covers @p column of a row.
	 *
	 * @p cells holds one UTF-8 string per cell, empty for the second half
	 * of a wide character. False when the cell is blank or holds no word
	 * that could be a path.
	 */
	bool findFileLink(const std::vector<std::string>& cells, int column, FileLink& out_link);

	/**
	 * @brief The file a printed path names, as a Windows path with forward
	 *        slashes: relative paths are taken from @p directory (the
	 *        shell's OSC 7 one), `~` from @p home, `/c/...` as `C:/...`.
	 */
	std::string resolveLinkPath(const std::string& path, const std::string& directory,
		const std::string& home);

	/**
	 * @brief The editor command line with {file}, {line} and {column}
	 *        filled in; a missing line or column is 1.
	 */
	std::string editorCommandFor(const std::string& editor, const std::string& file,
		const FileLink& link);

	/** Files Windows would run rather than show: programs, scripts, shortcuts. */
	bool linkRunsSomething(const std::string& path);

} /* namespace wbshterm */

#pragma once

/**
 * @file procfs.h
 * @brief A read-only /proc made from Windows' own process and system data.
 *
 * Like the generator devices it exists inside the shell only: redirections,
 * globs, `test` and the bundled commands see it, external programs do not.
 * A file's text is made when it is opened and read through a pipe, and its
 * size reads as 0, as Linux reports it.
 *
 *     /proc/cpuinfo meminfo uptime loadavg version
 *     /proc/self -> <the shell's pid>
 *     /proc/<pid>/cmdline comm status exe
 *     /proc/<self>/cwd environ fd/<n>
 */

#include <cstdio>
#include <string>
#include <vector>

namespace wbsh {

	class PathConv;

	enum class ProcType {
		Missing,
		Directory,
		File,
		Link,
		/// The path leads out of /proc (through exe, cwd or a file's fd);
		/// ProcNode::target is where, as a POSIX path.
		Outside,
	};

	struct ProcNode {
		ProcType type = ProcType::Missing;
		/// The path with every link inside /proc resolved.
		std::string canonical;
		/// A link's target as readlink prints it, or the path an Outside leads to.
		std::string target;
		/// A file reached through /proc/self/fd/<n> that is no disk file: n.
		int fd = -1;
	};

	/// True for /proc and everything below it, after `.`, `..` and `//` are folded.
	bool isProcPath(const std::string& posix_path);

	/// What @p posix_path names; a last component that is a link is
	/// followed only when @p follow_last.
	ProcNode lookupProcPath(const PathConv& paths, const std::string& posix_path,
	                        bool follow_last);

	/// The names in a /proc directory (links followed), or none.
	std::vector<std::string> listProcDirectory(const PathConv& paths,
	                                           const std::string& posix_path);

	/**
	 * @brief A descriptor reading @p node (not an Outside one), opened
	 *        with the _open @p flags.
	 * @return -1 with errno set: ENOENT, EISDIR, or EACCES for writing.
	 */
	int openProcNode(const PathConv& paths, const ProcNode& node, int flags);

	/// openProcNode for an fopen @p mode; nullptr with errno set.
	std::FILE* fopenProcNode(const PathConv& paths, const ProcNode& node, const char* mode);

}  // namespace wbsh

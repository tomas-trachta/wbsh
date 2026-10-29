#pragma once

/**
 * @file procfs_internal.h
 * @brief What the /proc resolver (procfs.cpp) and the makers of its file
 *        texts (procfs_text.cpp) share.
 */

#include <string>
#include <vector>

namespace wbsh {

	class PathConv;

}  // namespace wbsh

namespace wbsh::procfs_detail {

	std::string cpuInfoText();
	std::string memInfoText();
	std::string uptimeFileText();
	std::string loadAverageText();
	std::string versionText();

	/// cmdline, comm, status or (this process only) environ of @p pid.
	std::string processFileText(unsigned long pid, const std::string& name);

	/// Ascending.
	std::vector<unsigned long> processIds();

	bool processExists(unsigned long pid);

	/// POSIX path of @p pid's executable; empty when Windows will not say.
	std::string processImagePath(const PathConv& paths, unsigned long pid);

	std::string currentDirectoryPath(const PathConv& paths);

	/// The shell's open descriptors, ascending.
	std::vector<int> openDescriptors();

	/**
	 * @brief What descriptor @p fd is open on, as Linux's fd links read:
	 *        a POSIX path, `pipe:[N]`, `/dev/tty` or `/dev/null`.
	 * @param[out] disk_path The path when it is a file on disk, else empty.
	 * @return Empty when @p fd is not open.
	 */
	std::string descriptorTarget(const PathConv& paths, int fd, std::string& disk_path);

}  // namespace wbsh::procfs_detail

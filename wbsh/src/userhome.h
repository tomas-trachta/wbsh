#pragma once

/**
 * @file userhome.h
 * @brief Users' home directories, which `~name` expands to.
 *
 * Windows has no password database; a user's home is the profile path
 * recorded for the account's SID under ProfileList in the registry, or
 * the profiles directory joined with the name when no profile exists yet.
 */

#include <string>
#include <vector>

namespace wbsh {

	class PathConv;

	/**
	 * @brief The home directory of the user account @p user, as a POSIX path.
	 * @return Empty when no user has that name (a group does not count).
	 *         Answers are kept per name for the life of the process.
	 */
	std::string userHomeDirectory(const PathConv& paths, const std::string& user);

	/// Names of the users with a profile on this machine, sorted.
	std::vector<std::string> profileUserNames();

}  // namespace wbsh

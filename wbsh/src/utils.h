#pragma once

/**
 * @file utils.h
 * @brief Loading third-party utils into the shell through the SDK.
 */

#include <string>
#include <vector>

namespace wbsh {

	class Executor;

	struct UtilInfo {
		std::string name;
		std::string version;
		std::string summary;
	};

	/**
	 * @brief Loads every util found beside the exe and under %APPDATA%.
	 *
	 * Silent when there is nothing to load and when wbshsdk.dll is not
	 * there at all, which is the ordinary case for a bare wbsh.exe: the
	 * SDK is a component of an install that has utils, not a requirement.
	 * Complaints about a util that will not load go to stderr.
	 */
	void loadUtils(Executor& exec);

	/** Adds the `utils` command, which lists what loaded. */
	void registerUtilsBuiltin(Executor& exec);

	/** What loaded, for `utils` to print. Empty until loadUtils has run. */
	const std::vector<UtilInfo>& loadedUtils();

}  // namespace wbsh

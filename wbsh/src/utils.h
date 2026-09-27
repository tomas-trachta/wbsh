#pragma once

/**
 * @file utils.h
 * @brief Loading third-party utils into the shell through the SDK.
 */

#include <string>
#include <vector>

/**
 * @brief The candidates a util's completion callback adds to.
 *
 * Declared at file scope because the SDK header names it there: a util
 * only ever sees it as an opaque pointer.
 */
struct WbshCompletion {
	std::vector<std::string> items;
};

namespace wbsh {

	class Executor;

	struct UtilInfo {
		std::string name;
		std::string version;
		std::string summary;
		std::string path;
	};

	/**
	 * @brief Loads every util found beside the exe, under %APPDATA%, and in
	 *        each folder named by WBSH_PLUGINS.
	 *
	 * Silent when there is nothing to load and when wbshsdk.dll is not
	 * there at all, which is the ordinary case for a bare wbsh.exe: the
	 * SDK is a component of an install that has utils, not a requirement.
	 * Complaints about a util that will not load go to stderr and are
	 * kept for `utils` to show again.
	 */
	void loadUtils(Executor& exec);

	/** Adds the `utils` command: list, `load <dll>`, `reload`. */
	void registerUtilsBuiltin(Executor& exec);

	/** What loaded, for `utils` to print. Empty until loadUtils has run. */
	const std::vector<UtilInfo>& loadedUtils();

	/** Every refusal since the last load or reload, one line each. */
	const std::vector<std::string>& utilLoadFailures();

}  // namespace wbsh

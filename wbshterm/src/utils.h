#pragma once

/**
 * @file utils.h
 * @brief Loading third-party utils into the terminal through the SDK.
 */

#include <string>

namespace wbshterm {

	/**
	 * @brief Loads every util found beside the exe and under %APPDATA%.
	 *
	 * The terminal is a second host for the same DLLs the shell loads, so
	 * a util that only registers commands is loaded here too and simply
	 * adds nothing. Silent when there is no SDK beside the exe.
	 */
	void loadUtils();

	/** How many utils loaded, for the about box and the tests. */
	int loadedUtilCount();

	/**
	 * @brief What every loaded segment wants in the status bar, joined.
	 *
	 * Asked for while painting, so each segment is expected to hand back
	 * something it already has rather than go and work it out.
	 */
	std::string utilSegmentText();

}  /* namespace wbshterm */

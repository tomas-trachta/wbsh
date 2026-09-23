#pragma once

/**
 * @file fetch.h
 * @brief The startup panel: a logo, what this machine is, and the palette.
 */

#include "config.h"

#include <string>
#include <vector>

namespace wbshterm {

	/** One "Label: value" row of the panel. */
	struct FetchRow {
		std::string label;
		std::string value;
	};

	struct FetchInfo {
		std::string user;
		std::string host;
		std::vector<FetchRow> rows;
	};

	/** Reads the machine and the session; everything here can be slow or absent. */
	FetchInfo gatherFetchInfo(const Config& config);

	/**
	 * @brief Lays the logo beside the rows and adds the palette strip.
	 *
	 * Colours are ordinary SGR codes rather than the theme's own values, so
	 * the panel takes on whatever theme is active when it is displayed.
	 */
	std::string renderFetchPanel(const FetchInfo& info);

	/** Writes the panel to standard output, for `wbshterm --fetch`. */
	int printFetchPanel(const Config& config);

} /* namespace wbshterm */

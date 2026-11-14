#pragma once

/**
 * @file globsort.h
 * @brief Ordering pathname-expansion results the way bash 5.3's `GLOBSORT`
 *        asks: by name, size, a time stamp, blocks or numeric value, or not
 *        at all, a leading `-` reversing the order.
 */

#include <string>
#include <vector>

#include "pathconv.h"

namespace wbsh {

	/// Sort @p matches as @p globsort (the value of `GLOBSORT`) says; an
	/// empty or unknown value sorts by name ascending.
	void sortGlobMatches(std::vector<std::string>& matches, const std::string& globsort,
	                     const PathConv& pc);

}  // namespace wbsh

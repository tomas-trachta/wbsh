#pragma once

/**
 * @file awk.h
 * @brief Built-in awk(1) implementation.
 *
 * Pragmatic AWK subset bundled with wbsh so scripts that rely on awk
 * work out of the box without an external `awk.exe`. The program
 * source is read from `-f FILE`, the first non-option positional, or
 * stdin (`-`).
 *
 * @see awk.cpp for the supported feature surface.
 */

#include "executor.h"

#include <string>
#include <vector>

namespace wbsh {

	/**
	 * @brief Entry point for the bundled `awk` builtin.
	 *
	 * Supports BEGIN/END, /regex/ and expression patterns, $0..$NF,
	 * NR/NF/FS/OFS/ORS/FILENAME, the common statement set and
	 * scalar/array operators.
	 *
	 * @return 0 on success, non-zero on error.
	 */
	int builtin_awk(Executor& exec, const std::vector<std::string>& args);

}  // namespace wbsh

#pragma once

#include "executor.h"

#include <string>
#include <vector>

namespace wbsh {

	// Pragmatic awk subset. Reads the program from `-f FILE`, the first
	// non-option positional, or stdin if `-`. Supports BEGIN/END, /regex/
	// and expression patterns, $0..$NF, NR/NF/FS/OFS/ORS/FILENAME, common
	// statements and builtins. Returns 0 on success, non-zero on error.
	int builtin_awk(Executor& exec, const std::vector<std::string>& args);

}  // namespace wbsh

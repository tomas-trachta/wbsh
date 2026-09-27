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
 * Coverage:
 *   - BEGIN { } and END { } blocks (multiple of each, in source order)
 *   - /regex/ patterns, expression patterns, range patterns (e1, e2)
 *   - $0..$NF accessors, including assignment to $N
 *   - Variables: NR, NF, FS, OFS, ORS, FILENAME
 *   - All scalar operators: + - * / % ^, == != < <= > >=, && || !,
 *     ~ !~, ?:, =, +=, -=, *=, /=, %=, ++, --
 *   - String concat by juxtaposition
 *   - Statements: if/else, while, do/while, for(;;), for(k in arr),
 *     break, continue, next, exit, delete, return-only-from-builtins
 *   - Builtins: print, printf, sprintf, length, substr, index, split,
 *     sub, gsub, match, tolower, toupper, getline (basic forms), system
 *   - Associative arrays (single-dim subscripts; multi-dim via SUBSEP join)
 *
 * Out of scope: user-defined functions, gawk extensions, the more
 * obscure builtins (asort, asorti, mktime, strftime, etc.).
 */

#include "executor.h"

#include <string>
#include <vector>

namespace wbsh {

	int builtin_awk(Executor& exec, const std::vector<std::string>& args);

}  // namespace wbsh

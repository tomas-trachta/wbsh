#pragma once

/**
 * @file awk.h
 * @brief Built-in awk(1) implementation.
 *
 * POSIX awk bundled with wbsh so scripts that rely on awk work out of
 * the box without an external `awk.exe`. The program source is read
 * from `-f FILE` (repeatable) or the first operand.
 *
 * Coverage:
 *   - BEGIN / END blocks, regex, expression and range patterns
 *   - user-defined functions (recursion, local parameters, arrays by reference)
 *   - fields with assignment to $N / NF, strnum comparison semantics
 *   - NR NF FNR FS OFS ORS RS (one char, paragraph mode "", or a regex)
 *     FILENAME SUBSEP CONVFMT OFMT RSTART RLENGTH ENVIRON ARGC ARGV
 *   - every POSIX operator, getline in all its forms (`cmd | getline` too),
 *     print / printf with `>`, `>>` and `|` redirections, close, fflush, system
 *   - string, math and I/O builtins; `var=value` operands and `-v` with escapes
 *
 * Commands (`cmd | getline`, `print | cmd`, system()) run in a wbsh subshell.
 * Out of scope: gawk extensions beyond regex RS and `**` (asort, strftime, ...).
 */

#include "executor.h"

#include <string>
#include <vector>

namespace wbsh {

	int builtin_awk(Executor& exec, const std::vector<std::string>& args);

}  // namespace wbsh

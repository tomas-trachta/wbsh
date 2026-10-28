#pragma once

/**
 * @file testops.h
 * @brief The unary and file-comparison operators shared by `test` / `[`
 *        and `[[ ]]`.
 *
 * Windows has no permission bits or special files, so the checks map onto
 * what wbsh itself would do: `-x` is true for what it can run (a directory,
 * a `.exe` / `.com` / `.bat` / `.cmd` file, or a file starting with `#!`
 * or an `MZ` header), `-L` sees symlinks and junctions, `-c` the `/dev`
 * devices, `-p` named pipes and `/dev/fd/N` on a pipe.
 */

#include <string>

namespace wbsh {

	class Executor;

	/// One of `-a -b -c -d -e -f -g -h -k -L -N -O -G -p -r -s -S -t -u -w -x -z -n -v -o`.
	bool isUnaryTestOperator(const std::string& op);
	bool evalUnaryTestOperator(Executor& exec, char op, const std::string& operand);

	/// `-nt`, `-ot`, `-ef`.
	bool isFileCompareOperator(const std::string& op);
	bool evalFileCompare(Executor& exec, const std::string& op, const std::string& lhs,
		const std::string& rhs);

}  // namespace wbsh

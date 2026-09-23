#pragma once

/**
 * @file selftest.h
 * @brief Headless checks: the parser, the grid, and one live pty session.
 */

#include <string>

namespace wbshterm {

	/**
	 * @brief Runs every check and writes a report to @p report_path.
	 *
	 * @return true when all checks passed. The report lists one line per
	 *         check either way, so a failure is readable without a
	 *         debugger.
	 */
	bool runSelfTest(const std::wstring& report_path, const std::wstring& shell_command_line);

} /* namespace wbshterm */

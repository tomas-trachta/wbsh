#pragma once

/**
 * @file script.h
 * @brief Non-interactive (single-source) execution entry point.
 *
 * Used by the `wbsh -c <cmd>` and `wbsh <file>` CLI paths. Switches
 * between debugging dumps (tokens / AST / expanded words) and the
 * real executor based on the flags passed in.
 */

#include <string>

namespace wbsh {

	int runOnSource(const std::string& src,
	                bool show_tokens,
	                bool show_ast,
	                bool do_expand,
	                bool do_run,
	                const std::string& script_name = "");

}  // namespace wbsh

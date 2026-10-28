/**
 * @file make_report.cpp
 * @brief make's diagnostics, worded the way GNU make words them.
 */

#include "make_internal.h"

#include <cstdio>
#include <string>

namespace wbsh::make_detail {

	std::string programName(const Make& make) {
		if (make.level == 0) return "make";
		return "make[" + std::to_string(make.level) + "]";
	}

	static std::string locationPrefix(const Make& make, const Location& where) {
		if (where.file.empty()) return programName(make) + ": ";
		return where.file + ":" + std::to_string(where.line) + ": ";
	}

	static void writeStderr(const std::string& text) {
		std::fflush(stdout);
		std::fputs(text.c_str(), stderr);
		std::fflush(stderr);
	}

	void reportFatal(Make& make, const Location& where, const std::string& message) {
		if (make.fatal) return;

		make.fatal = true;
		writeStderr(locationPrefix(make, where) + "*** " + message + ".  Stop.\n");
	}

	void reportWarning(const Make& make, const Location& where, const std::string& message) {
		writeStderr(locationPrefix(make, where) + message + "\n");
	}

	void reportError(const Make& make, const std::string& message) {
		writeStderr(programName(make) + ": " + message + "\n");
	}

	void reportMessage(const Make& make, const std::string& message) {
		printLine(programName(make) + ": " + message);
	}

	void printLine(const std::string& text) {
		std::fputs(text.c_str(), stdout);
		std::fputc('\n', stdout);
		std::fflush(stdout);
	}

}  // namespace wbsh::make_detail

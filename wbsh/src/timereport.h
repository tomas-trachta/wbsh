#pragma once

/**
 * @file timereport.h
 * @brief What the `time` keyword measures and prints: elapsed, user and
 *        system time, formatted the way bash formats TIMEFORMAT.
 */

#include <cstdint>
#include <string>

namespace wbsh {

	/// Milliseconds. User and system time are the shell's own plus that of
	/// every child process it has waited for, as getrusage counts children.
	struct CpuSample {
		std::int64_t real_ms = 0;
		std::int64_t user_ms = 0;
		std::int64_t system_ms = 0;
	};

	/// bash's default TIMEFORMAT, used while the variable is unset.
	extern const char* const kTimeFormatDefault;

	/// The format of `time -p`, which ignores TIMEFORMAT.
	extern const char* const kTimeFormatPosix;

	CpuSample sampleCpuTimes();

	/// Adds the CPU time of a child that has exited to what sampleCpuTimes
	/// reports; @p process is its HANDLE, still open.
	void accountChildProcess(void* process);

	/// The report for the span from @p start to @p end with a trailing
	/// newline: `%[p][l]R`, `%[p][l]U`, `%[p][l]S`, `%P` and `%%`.
	std::string formatTimeReport(const std::string& format, const CpuSample& start,
	                             const CpuSample& end);

}  // namespace wbsh

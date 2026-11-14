/**
 * @file timereport.cpp
 * @brief What the `time` keyword measures and prints: elapsed, user and
 *        system time, formatted the way bash formats TIMEFORMAT.
 */

#include "timereport.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif /* _WIN32 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace wbsh {

	const char* const kTimeFormatDefault = "\nreal\t%3lR\nuser\t%3lU\nsys\t%3lS";
	const char* const kTimeFormatPosix = "real %2R\nuser %2U\nsys %2S";

	namespace timereport_detail {

		struct Directive {
			int precision = 3;
			bool long_form = false;
			char conversion = 0;
		};

	}  // namespace timereport_detail

	using timereport_detail::Directive;

	static const std::int64_t kTicksPerMilli = 10000;
	static const std::int64_t kMillisPerSecond = 1000;
	static const std::int64_t kSecondsPerMinute = 60;
	static const std::int64_t kFullCpuHundredths = 10000;
	static const int kMaxPrecision = 3;

	static std::atomic<std::int64_t> child_user_ticks{ 0 };
	static std::atomic<std::int64_t> child_system_ticks{ 0 };

#ifdef _WIN32
	static std::int64_t fileTimeTicks(const FILETIME& time) {
		const std::uint64_t ticks =
			(static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
		return static_cast<std::int64_t>(ticks);
	}

	static bool processTicks(HANDLE process, std::int64_t& user, std::int64_t& system) {
		FILETIME created;
		FILETIME exited;
		FILETIME kernel;
		FILETIME user_time;
		if (::GetProcessTimes(process, &created, &exited, &kernel, &user_time) == 0) return false;

		user = fileTimeTicks(user_time);
		system = fileTimeTicks(kernel);
		return true;
	}
#endif /* _WIN32 */

	void accountChildProcess(void* process) {
#ifdef _WIN32
		std::int64_t user = 0;
		std::int64_t system = 0;
		if (!processTicks(static_cast<HANDLE>(process), user, system)) return;

		child_user_ticks += user;
		child_system_ticks += system;
#else
		(void)process;
#endif /* _WIN32 */
	}

	static std::int64_t steadyMillis() {
		const auto since = std::chrono::steady_clock::now().time_since_epoch();
		return std::chrono::duration_cast<std::chrono::milliseconds>(since).count();
	}

	CpuSample sampleCpuTimes() {
		CpuSample sample;
		sample.real_ms = steadyMillis();

		std::int64_t user = child_user_ticks.load();
		std::int64_t system = child_system_ticks.load();
#ifdef _WIN32
		std::int64_t own_user = 0;
		std::int64_t own_system = 0;
		if (processTicks(::GetCurrentProcess(), own_user, own_system)) {
			user += own_user;
			system += own_system;
		}
#endif /* _WIN32 */

		sample.user_ms = user / kTicksPerMilli;
		sample.system_ms = system / kTicksPerMilli;
		return sample;
	}

	// bash truncates rather than rounds: 0.0019 s at precision 2 is "0.00".
	static void appendFraction(std::string& out, std::int64_t millis, int precision) {
		out.push_back('.');
		std::int64_t scale = kMillisPerSecond;
		for (int digit = 0; digit < precision; ++digit) {
			scale /= 10;
			out.push_back(static_cast<char>('0' + (millis / scale) % 10));
		}
	}

	static std::string formatSeconds(std::int64_t millis, int precision, bool long_form) {
		std::int64_t seconds = millis / kMillisPerSecond;
		std::string out;
		if (long_form) {
			out += std::to_string(seconds / kSecondsPerMinute) + "m";
			seconds %= kSecondsPerMinute;
		}

		out += std::to_string(seconds);
		if (precision > 0) appendFraction(out, millis % kMillisPerSecond, precision);
		if (long_form) out.push_back('s');
		return out;
	}

	// Hundredths of a percent, capped at 100% as bash caps it.
	static std::string formatCpuPercent(std::int64_t real, std::int64_t cpu) {
		std::int64_t hundredths = real > 0 ? cpu * kFullCpuHundredths / real : 0;
		if (hundredths > kFullCpuHundredths) hundredths = kFullCpuHundredths;

		const std::int64_t as_millis = hundredths / 100 * kMillisPerSecond + hundredths % 100 * 10;
		return formatSeconds(as_millis, 2, false);
	}

	static Directive readDirective(const std::string& format, std::size_t& i) {
		Directive directive;
		if (i < format.size() && format[i] >= '0' && format[i] <= '9') {
			directive.precision = format[i] - '0';
			if (directive.precision > kMaxPrecision) directive.precision = kMaxPrecision;
			++i;
		}

		if (i < format.size() && format[i] == 'l') {
			directive.long_form = true;
			++i;
		}

		if (i < format.size()) directive.conversion = format[i++];
		return directive;
	}

	static bool expandDirective(const Directive& directive, const CpuSample& span,
	                            std::string& out) {
		switch (directive.conversion) {
		case 'R':
			out += formatSeconds(span.real_ms, directive.precision, directive.long_form);
			return true;
		case 'U':
			out += formatSeconds(span.user_ms, directive.precision, directive.long_form);
			return true;
		case 'S':
			out += formatSeconds(span.system_ms, directive.precision, directive.long_form);
			return true;
		case 'P':
			out += formatCpuPercent(span.real_ms, span.user_ms + span.system_ms);
			return true;
		case '%':
			out.push_back('%');
			return true;
		default:
			return false;
		}
	}

	static CpuSample spanBetween(const CpuSample& start, const CpuSample& end) {
		CpuSample span;
		span.real_ms = end.real_ms - start.real_ms;
		span.user_ms = end.user_ms - start.user_ms;
		span.system_ms = end.system_ms - start.system_ms;
		return span;
	}

	std::string formatTimeReport(const std::string& format, const CpuSample& start,
	                             const CpuSample& end) {
		const CpuSample span = spanBetween(start, end);
		std::string out;
		std::size_t i = 0;
		while (i < format.size()) {
			if (format[i] != '%') {
				out.push_back(format[i++]);
				continue;
			}

			const std::size_t directive_start = i++;
			const Directive directive = readDirective(format, i);
			if (!expandDirective(directive, span, out)) {
				out += format.substr(directive_start, i - directive_start);
			}
		}

		out.push_back('\n');
		return out;
	}

}  // namespace wbsh

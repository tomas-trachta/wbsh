#pragma once

/**
 * @file cron_schedule.h
 * @brief Crontab text and the times it names: parsing, and the next run after an instant.
 *
 * Everything here is pure: wall-clock time comes from a CronZone, so the
 * same code runs against the machine's zone in crond and against any
 * named zone in the tests.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace wbsh {

	/// Bit n of each mask is value n; weekdays count from Sunday = 0.
	struct CronSchedule {
		std::uint64_t minutes = 0;
		std::uint32_t hours = 0;
		std::uint32_t days = 0;
		std::uint32_t months = 0;
		std::uint32_t weekdays = 0;
		bool day_star = false;
		bool weekday_star = false;
		bool minute_or_hour_star = false;
		bool at_reboot = false;
	};

	struct CivilTime {
		int year = 1970;
		int month = 1;
		int day = 1;
		int hour = 0;
		int minute = 0;
		int second = 0;
	};

	/// The wall clock of one time zone at a UTC instant (seconds since 1970).
	class CronZone {
	public:
		virtual ~CronZone() = default;
		virtual CivilTime civilAt(std::int64_t instant) const = 0;
	};

	using CronVariables = std::vector<std::pair<std::string, std::string>>;

	/// One job line: @c text as written, @c command and @c input after the
	/// `%` rule, and the NAME=value lines above it.
	struct CronEntry {
		CronSchedule schedule;
		std::string text;
		std::string command;
		std::string input;
		CronVariables variables;
		std::size_t line = 0;
	};

	struct CronSyntaxError {
		std::size_t line = 0;
		std::string message;
	};

	struct CronTable {
		std::vector<CronEntry> entries;
		std::vector<CronSyntaxError> errors;
	};

	/// Every job and environment line of a crontab; lines that fail to parse
	/// are left out and reported, the rest still load.
	CronTable parseCrontab(const std::string& text);

	/// Five time fields or an @keyword, with nothing after them.
	bool parseCronSchedule(const std::string& spec, CronSchedule& schedule, std::string& error);

	/// Seconds since 1970 of @p civil read as UTC, and back.
	std::int64_t secondsFromCivil(const CivilTime& civil);
	CivilTime civilFromSeconds(std::int64_t seconds);

	/// 0 = Sunday.
	int weekdayOf(const CivilTime& civil);

	/// The instant @p civil first shows in @p zone, or the end of the gap it
	/// falls into when the clock skips it.
	std::int64_t instantOfCivil(const CronZone& zone, const CivilTime& civil);

	/**
	 * @brief The first run of @p schedule strictly after @p after.
	 *
	 * As cronie handles daylight saving: a time the clock skips runs as the
	 * clock jumps, and a repeated time runs once, except that jobs whose
	 * minute or hour field starts with `*` skip the gap and run in both
	 * passes of a repeated hour. False for @reboot and for dates that never
	 * come (30 February).
	 */
	bool nextCronRun(const CronSchedule& schedule, std::int64_t after, const CronZone& zone,
	                 std::int64_t& run);

	/// Whether @p schedule runs in the minute starting at @p minute_start.
	bool isCronDue(const CronSchedule& schedule, std::int64_t minute_start, const CronZone& zone);

}  // namespace wbsh

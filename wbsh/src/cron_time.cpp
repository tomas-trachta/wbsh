/**
 * @file cron_time.cpp
 * @brief The next run of a cron schedule: calendar arithmetic and daylight-saving placement.
 *
 * The search walks local calendar days, and on each matching day the
 * schedule's minutes in order. A day whose UTC offset stays the same
 * maps local times to instants by subtraction; only the day or two a
 * year around a clock change asks the zone about each minute.
 */

#include "cron_schedule.h"

#include <algorithm>
#include <cstdint>

namespace wbsh {

	namespace cron_detail {

		static const std::int64_t kMinute = 60;
		static const std::int64_t kHour = 3600;
		static const std::int64_t kDay = 86400;
		static const int kMinutesPerHour = 60;
		static const int kHoursPerDay = 24;
		static const int kDaysPerWeek = 7;

		// Long enough for 29 February after a skipped leap year (2096 to 2104).
		static const int kSearchDays = 366 * 8 + 2;

		struct Placement {
			std::int64_t first = 0;
			std::int64_t second = 0;
			bool repeated = false;
			bool skipped = false;
		};

		struct Search {
			const CronSchedule& schedule;
			const CronZone& zone;
			std::int64_t after;
		};

		struct DayClock {
			bool steady = false;
			std::int64_t offset = 0;
		};

		enum class Scan { Continue, Stop };

	}  // namespace cron_detail

	using cron_detail::DayClock;
	using cron_detail::Placement;
	using cron_detail::Scan;
	using cron_detail::Search;

	static std::int64_t floorDiv(std::int64_t value, std::int64_t divisor) {
		const std::int64_t quotient = value / divisor;
		return quotient * divisor > value ? quotient - 1 : quotient;
	}

	// Howard Hinnant's days_from_civil / civil_from_days.
	static std::int64_t daysFromDate(int year, int month, int day) {
		const std::int64_t shifted_year = year - (month <= 2 ? 1 : 0);
		const std::int64_t era = floorDiv(shifted_year, 400);
		const std::int64_t year_of_era = shifted_year - era * 400;
		const std::int64_t day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
		const std::int64_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100
			+ day_of_year;
		return era * 146097 + day_of_era - 719468;
	}

	static CivilTime dateFromDays(std::int64_t days) {
		const std::int64_t shifted = days + 719468;
		const std::int64_t era = floorDiv(shifted, 146097);
		const std::int64_t day_of_era = shifted - era * 146097;
		const std::int64_t year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524
			- day_of_era / 146096) / 365;
		const std::int64_t day_of_year = day_of_era
			- (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
		const std::int64_t month_index = (5 * day_of_year + 2) / 153;

		CivilTime civil;
		civil.day = static_cast<int>(day_of_year - (153 * month_index + 2) / 5 + 1);
		civil.month = static_cast<int>(month_index < 10 ? month_index + 3 : month_index - 9);
		civil.year = static_cast<int>(year_of_era + era * 400 + (civil.month <= 2 ? 1 : 0));
		return civil;
	}

	std::int64_t secondsFromCivil(const CivilTime& civil) {
		return daysFromDate(civil.year, civil.month, civil.day) * cron_detail::kDay
			+ civil.hour * cron_detail::kHour + civil.minute * cron_detail::kMinute + civil.second;
	}

	CivilTime civilFromSeconds(std::int64_t seconds) {
		const std::int64_t days = floorDiv(seconds, cron_detail::kDay);
		const std::int64_t rest = seconds - days * cron_detail::kDay;

		CivilTime civil = dateFromDays(days);
		civil.hour = static_cast<int>(rest / cron_detail::kHour);
		civil.minute = static_cast<int>(rest % cron_detail::kHour / cron_detail::kMinute);
		civil.second = static_cast<int>(rest % cron_detail::kMinute);
		return civil;
	}

	static int weekdayOfDays(std::int64_t days) {
		const std::int64_t thursday_based = days + 4;
		const std::int64_t week = cron_detail::kDaysPerWeek;
		return static_cast<int>(thursday_based - floorDiv(thursday_based, week) * week);
	}

	int weekdayOf(const CivilTime& civil) {
		return weekdayOfDays(daysFromDate(civil.year, civil.month, civil.day));
	}

	static std::int64_t localSecondsAt(const CronZone& zone, std::int64_t instant) {
		return secondsFromCivil(zone.civilAt(instant));
	}

	static std::int64_t offsetAt(const CronZone& zone, std::int64_t instant) {
		return localSecondsAt(zone, instant) - instant;
	}

	// The first instant in (early, late] whose wall clock has passed
	// @p local: where a clock change jumps over it.
	static std::int64_t gapEnd(const CronZone& zone, std::int64_t local, std::int64_t early,
	                           std::int64_t late) {
		while (late - early > 1) {
			const std::int64_t middle = early + (late - early) / 2;
			if (localSecondsAt(zone, middle) > local) late = middle;
			else early = middle;
		}

		return late;
	}

	static Placement placeLocal(const CronZone& zone, std::int64_t local) {
		const std::int64_t offset_before = offsetAt(zone, local - cron_detail::kDay);
		const std::int64_t offset_after = offsetAt(zone, local + cron_detail::kDay);
		const std::int64_t early = local - std::max(offset_before, offset_after);
		const std::int64_t late = local - std::min(offset_before, offset_after);
		const bool early_fits = localSecondsAt(zone, early) == local;
		const bool late_fits = late != early && localSecondsAt(zone, late) == local;

		Placement placement;
		if (!early_fits && !late_fits) {
			placement.skipped = true;
			placement.first = gapEnd(zone, local, early, late);
			return placement;
		}

		placement.first = early_fits ? early : late;
		placement.repeated = early_fits && late_fits;
		placement.second = late;
		return placement;
	}

	std::int64_t instantOfCivil(const CronZone& zone, const CivilTime& civil) {
		return placeLocal(zone, secondsFromCivil(civil)).first;
	}

	static bool hasBit(std::uint64_t mask, int bit) {
		return ((mask >> bit) & 1) != 0;
	}

	static bool dayMatches(const CronSchedule& schedule, std::int64_t days) {
		const CivilTime date = dateFromDays(days);
		if (!hasBit(schedule.months, date.month)) return false;

		const bool day_fits = hasBit(schedule.days, date.day);
		const bool weekday_fits = hasBit(schedule.weekdays, weekdayOfDays(days));
		if (schedule.day_star || schedule.weekday_star) return day_fits && weekday_fits;
		return day_fits || weekday_fits;
	}

	static DayClock clockOfDay(const CronZone& zone, std::int64_t day_start) {
		const std::int64_t before = offsetAt(zone, day_start - cron_detail::kDay);
		const std::int64_t after = offsetAt(zone, day_start + 2 * cron_detail::kDay);

		DayClock clock;
		clock.steady = before == after;
		clock.offset = before;
		return clock;
	}

	static Placement placeOnDay(const Search& search, const DayClock& clock, std::int64_t local) {
		if (!clock.steady) return placeLocal(search.zone, local);

		Placement placement;
		placement.first = local - clock.offset;
		return placement;
	}

	static void considerInstant(const Search& search, std::int64_t instant, bool& found,
	                            std::int64_t& best) {
		if (instant <= search.after) return;
		if (found && instant >= best) return;

		best = instant;
		found = true;
	}

	// Later minutes of the day never start before this one does, so once
	// one starts at or after the best run so far nothing later can beat it.
	static Scan considerMinute(const Search& search, const Placement& placement, bool& found,
	                           std::int64_t& best) {
		if (found && placement.first >= best) return Scan::Stop;

		const bool wild = search.schedule.minute_or_hour_star;
		if (!placement.skipped || !wild) considerInstant(search, placement.first, found, best);
		if (placement.repeated && wild) considerInstant(search, placement.second, found, best);
		return Scan::Continue;
	}

	static bool earliestRunOnDay(const Search& search, std::int64_t days, std::int64_t& best) {
		const std::int64_t day_start = days * cron_detail::kDay;
		const DayClock clock = clockOfDay(search.zone, day_start);
		bool found = false;
		for (int hour = 0; hour < cron_detail::kHoursPerDay; ++hour) {
			if (!hasBit(search.schedule.hours, hour)) continue;

			for (int minute = 0; minute < cron_detail::kMinutesPerHour; ++minute) {
				if (!hasBit(search.schedule.minutes, minute)) continue;

				const std::int64_t local = day_start + hour * cron_detail::kHour
					+ minute * cron_detail::kMinute;
				const Placement placement = placeOnDay(search, clock, local);
				if (considerMinute(search, placement, found, best) == Scan::Stop) return true;
			}
		}

		return found;
	}

	bool nextCronRun(const CronSchedule& schedule, std::int64_t after, const CronZone& zone,
	                 std::int64_t& run) {
		if (schedule.at_reboot) return false;

		const Search search{ schedule, zone, after };
		const std::int64_t first_day = floorDiv(localSecondsAt(zone, after), cron_detail::kDay);
		for (int i = 0; i < cron_detail::kSearchDays; ++i) {
			if (!dayMatches(schedule, first_day + i)) continue;
			if (earliestRunOnDay(search, first_day + i, run)) return true;
		}

		return false;
	}

	bool isCronDue(const CronSchedule& schedule, std::int64_t minute_start, const CronZone& zone) {
		std::int64_t run = 0;
		if (!nextCronRun(schedule, minute_start - 1, zone, run)) return false;
		return run < minute_start + cron_detail::kMinute;
	}

}  // namespace wbsh

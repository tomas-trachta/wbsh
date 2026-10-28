/**
 * @file cron_zone.cpp
 * @brief Windows time zones as CronZones, and the "YYYY-MM-DD HH:MM" times the tests name.
 *
 * Conversions go through the zone's dynamic DST data as a whole: the
 * rules of the instant's UTC year are wrong for the hours around New
 * Year whose local date is already in the next (or still in the last)
 * year, when the rules change between the two (Volgograd, 2020-21).
 */

#include "cron_internal.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif /* _WIN32 */

#include <cstdint>
#include <cwchar>
#include <memory>
#include <string>

#include "numparse.h"
#include "pathconv.h"

namespace wbsh {

	namespace cron_detail {

		static const std::int64_t kEpochAsFileTimeSeconds = 11644473600;
		static const std::int64_t kFileTimeTicksPerSecond = 10000000;

#ifdef _WIN32

		class WindowsZone : public CronZone {
		public:
			WindowsZone(const DYNAMIC_TIME_ZONE_INFORMATION& info, bool daylight_disabled)
				: info_(info), daylight_disabled_(daylight_disabled) {}

			CivilTime civilAt(std::int64_t instant) const override;

		private:
			DYNAMIC_TIME_ZONE_INFORMATION info_;
			bool daylight_disabled_;
		};

#endif /* _WIN32 */

		class UtcZone : public CronZone {
		public:
			CivilTime civilAt(std::int64_t instant) const override {
				return civilFromSeconds(instant);
			}
		};

	}  // namespace cron_detail

#ifdef _WIN32

	using cron_detail::WindowsZone;

	static SYSTEMTIME systemTimeOf(std::int64_t instant) {
		const std::uint64_t ticks = static_cast<std::uint64_t>(
			instant + cron_detail::kEpochAsFileTimeSeconds) * cron_detail::kFileTimeTicksPerSecond;

		FILETIME file_time{};
		file_time.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFu);
		file_time.dwHighDateTime = static_cast<DWORD>(ticks >> 32);

		SYSTEMTIME system_time{};
		::FileTimeToSystemTime(&file_time, &system_time);
		return system_time;
	}

	static CivilTime civilOf(const SYSTEMTIME& time) {
		CivilTime civil;
		civil.year = time.wYear;
		civil.month = time.wMonth;
		civil.day = time.wDay;
		civil.hour = time.wHour;
		civil.minute = time.wMinute;
		civil.second = time.wSecond;
		return civil;
	}

	static TIME_ZONE_INFORMATION standardTimeOnly(const DYNAMIC_TIME_ZONE_INFORMATION& info) {
		TIME_ZONE_INFORMATION rules{};
		rules.Bias = info.Bias;
		rules.StandardBias = info.StandardBias;
		std::wmemcpy(rules.StandardName, info.StandardName, 32);
		return rules;
	}

	static bool toZoneTime(const DYNAMIC_TIME_ZONE_INFORMATION& info, bool daylight_disabled,
	                       const SYSTEMTIME& utc, SYSTEMTIME& local) {
		if (!daylight_disabled) {
			return ::SystemTimeToTzSpecificLocalTimeEx(&info, &utc, &local) != 0;
		}

		const TIME_ZONE_INFORMATION standard = standardTimeOnly(info);
		return ::SystemTimeToTzSpecificLocalTime(&standard, &utc, &local) != 0;
	}

	CivilTime WindowsZone::civilAt(std::int64_t instant) const {
		const SYSTEMTIME utc = systemTimeOf(instant);
		SYSTEMTIME local{};
		if (!toZoneTime(info_, daylight_disabled_, utc, local)) return civilFromSeconds(instant);
		return civilOf(local);
	}

	std::unique_ptr<CronZone> localCronZone() {
		DYNAMIC_TIME_ZONE_INFORMATION info{};
		if (::GetDynamicTimeZoneInformation(&info) == TIME_ZONE_ID_INVALID) {
			return std::make_unique<cron_detail::UtcZone>();
		}

		return std::make_unique<WindowsZone>(info, info.DynamicDaylightTimeDisabled != FALSE);
	}

	std::unique_ptr<CronZone> namedCronZone(const std::string& key) {
		const std::wstring wanted = utf8ToWide(key);
		DYNAMIC_TIME_ZONE_INFORMATION info{};
		for (DWORD index = 0; ::EnumDynamicTimeZoneInformation(index, &info) == ERROR_SUCCESS;
		     ++index) {
			if (_wcsicmp(info.TimeZoneKeyName, wanted.c_str()) != 0) continue;
			return std::make_unique<WindowsZone>(info, false);
		}

		return nullptr;
	}

#else

	std::unique_ptr<CronZone> localCronZone() {
		return std::make_unique<cron_detail::UtcZone>();
	}

	std::unique_ptr<CronZone> namedCronZone(const std::string& key) {
		if (key == "UTC") return std::make_unique<cron_detail::UtcZone>();
		return nullptr;
	}

#endif /* _WIN32 */

	static bool takeNumber(const std::string& text, std::size_t start, std::size_t length,
	                       int& value) {
		for (std::size_t i = start; i < start + length; ++i) {
			if (text[i] < '0' || text[i] > '9') return false;
		}

		return parseInt(text.substr(start, length), value);
	}

	static bool hasSeparators(const std::string& text) {
		return text.size() == 16 && text[4] == '-' && text[7] == '-'
			&& (text[10] == ' ' || text[10] == 'T') && text[13] == ':';
	}

	static bool isRealMinute(const CivilTime& civil) {
		const CivilTime round_trip = civilFromSeconds(secondsFromCivil(civil));
		return civil.month >= 1 && civil.month <= 12 && civil.hour < 24 && civil.minute < 60
			&& round_trip.day == civil.day && round_trip.month == civil.month;
	}

	bool parseCivilMinute(const std::string& text, CivilTime& civil) {
		if (!hasSeparators(text)) return false;

		const bool numeric = takeNumber(text, 0, 4, civil.year)
			&& takeNumber(text, 5, 2, civil.month) && takeNumber(text, 8, 2, civil.day)
			&& takeNumber(text, 11, 2, civil.hour) && takeNumber(text, 14, 2, civil.minute);
		civil.second = 0;
		return numeric && isRealMinute(civil);
	}

}  // namespace wbsh

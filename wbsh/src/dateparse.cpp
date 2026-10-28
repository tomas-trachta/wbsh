/**
 * @file dateparse.cpp
 * @brief The date strings `touch -d` and `touch -t` take.
 */

#include "dateparse.h"

#include <cctype>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "numparse.h"

namespace wbsh::dateparse_detail {

	static const long long kSecondsPerMinute = 60;
	static const long long kSecondsPerHour = 3600;
	static const int kNanoDigits = 9;

	struct Parts {
		bool has_date = false;
		int year = 0;
		int month = 0;
		int day = 0;
		bool has_time = false;
		int hour = 0;
		int minute = 0;
		int second = 0;
		long nanos = 0;
		bool has_zone = false;
		int zone_minutes = 0;
		long long relative_seconds = 0;
		int relative_days = 0;
		int relative_months = 0;
	};

	struct Unit {
		const char* name;
		long long seconds;
		int days;
		int months;
	};

	static const Unit kUnits[] = {
		{ "sec",       1,                 0,  0 },
		{ "second",    1,                 0,  0 },
		{ "min",       kSecondsPerMinute, 0,  0 },
		{ "minute",    kSecondsPerMinute, 0,  0 },
		{ "hour",      kSecondsPerHour,   0,  0 },
		{ "day",       0,                 1,  0 },
		{ "week",      0,                 7,  0 },
		{ "fortnight", 0,                 14, 0 },
		{ "month",     0,                 0,  1 },
		{ "year",      0,                 0,  12 },
	};

	static std::string lowered(const std::string& text) {
		std::string out = text;
		for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return out;
	}

	static std::vector<std::string> splitWords(const std::string& text) {
		std::vector<std::string> words;
		std::string word;
		for (const char c : text) {
			if (std::isspace(static_cast<unsigned char>(c)) == 0) {
				word.push_back(c);
				continue;
			}

			if (!word.empty()) words.push_back(lowered(word));
			word.clear();
		}

		if (!word.empty()) words.push_back(lowered(word));
		return words;
	}

	static bool isDigits(const std::string& text) {
		if (text.empty()) return false;

		for (const char c : text) {
			if (std::isdigit(static_cast<unsigned char>(c)) == 0) return false;
		}

		return true;
	}

	static bool digitsValue(const std::string& text, int& value) {
		return isDigits(text) && parseInt(text, value);
	}

	// `.5` is 500000000 ns; digits past nanoseconds are dropped.
	static bool parseFraction(const std::string& digits, long& nanos) {
		if (!isDigits(digits)) return false;

		std::string padded = digits.substr(0, kNanoDigits);
		padded.append(kNanoDigits - padded.size(), '0');
		int value = 0;
		if (!parseInt(padded, value)) return false;

		nanos = value;
		return true;
	}

	static bool parseZone(const std::string& text, Parts& parts) {
		if (text == "z" || text == "utc" || text == "gmt") {
			parts.has_zone = true;
			parts.zone_minutes = 0;
			return true;
		}

		if (text.size() < 3 || (text[0] != '+' && text[0] != '-')) return false;

		std::string digits = text.substr(1);
		if (digits.size() == 5 && digits[2] == ':') digits.erase(2, 1);
		int hours = 0;
		int minutes = 0;
		if (digits.size() == 2 && !digitsValue(digits, hours)) return false;
		if (digits.size() == 4 && (!digitsValue(digits.substr(0, 2), hours)
				|| !digitsValue(digits.substr(2), minutes))) return false;
		if (digits.size() != 2 && digits.size() != 4) return false;

		parts.has_zone = true;
		parts.zone_minutes = (text[0] == '-' ? -1 : 1) * (hours * 60 + minutes);
		return true;
	}

	static std::size_t zoneStart(const std::string& text) {
		const std::size_t sign = text.find_first_of("+-");
		if (sign != std::string::npos) return sign;
		if (!text.empty() && text.back() == 'z') return text.size() - 1;
		return std::string::npos;
	}

	static bool parseClock(const std::string& clock, Parts& parts) {
		const std::size_t dot = clock.find_first_of(".,");
		const std::string whole = clock.substr(0, dot);
		const bool fraction = dot != std::string::npos;
		if (fraction && !parseFraction(clock.substr(dot + 1), parts.nanos)) return false;

		const std::size_t first = whole.find(':');
		if (first == std::string::npos) return false;

		const std::size_t second = whole.find(':', first + 1);
		const std::string minutes = whole.substr(first + 1, second == std::string::npos
			? std::string::npos : second - first - 1);
		if (!digitsValue(whole.substr(0, first), parts.hour)) return false;
		if (!digitsValue(minutes, parts.minute)) return false;
		if (second != std::string::npos && !digitsValue(whole.substr(second + 1), parts.second)) {
			return false;
		}

		parts.has_time = parts.hour < 24 && parts.minute < 60 && parts.second < 61;
		return parts.has_time;
	}

	static bool parseTime(const std::string& text, Parts& parts) {
		const std::size_t zone = zoneStart(text);
		if (zone != std::string::npos && !parseZone(text.substr(zone), parts)) return false;

		return parseClock(text.substr(0, zone), parts);
	}

	static bool parseDate(const std::string& text, Parts& parts) {
		const std::size_t t = text.find('t');
		const std::string date = text.substr(0, t);
		const std::size_t first = date.find('-');
		const std::size_t second = date.find('-', first == std::string::npos ? 0 : first + 1);
		if (first == std::string::npos || second == std::string::npos) return false;

		if (!digitsValue(date.substr(0, first), parts.year)) return false;
		if (!digitsValue(date.substr(first + 1, second - first - 1), parts.month)) return false;
		if (!digitsValue(date.substr(second + 1), parts.day)) return false;
		if (parts.month < 1 || parts.month > 12 || parts.day < 1 || parts.day > 31) return false;

		parts.has_date = true;
		return t == std::string::npos || parseTime(text.substr(t + 1), parts);
	}

	static const Unit* findUnit(std::string word) {
		if (word.size() > 1 && word.back() == 's') word.pop_back();
		for (const Unit& unit : kUnits) {
			if (word == unit.name) return &unit;
		}

		return nullptr;
	}

	static void addRelative(const Unit& unit, long long count, Parts& parts) {
		parts.relative_seconds += unit.seconds * count;
		parts.relative_days += static_cast<int>(unit.days * count);
		parts.relative_months += static_cast<int>(unit.months * count);
	}

	static bool parseCount(const std::string& word, long long& count) {
		const std::string digits = !word.empty() && word[0] == '+' ? word.substr(1) : word;
		return parseLL(digits, count);
	}

	static bool unitFollows(const std::vector<std::string>& words, std::size_t i) {
		long long count = 0;
		if (!parseCount(words[i], count) || i + 1 >= words.size()) return false;

		return findUnit(words[i + 1]) != nullptr;
	}

	// `N unit [ago]`, `+N unit`, or a unit alone meaning one of it.
	static bool parseRelative(const std::vector<std::string>& words, std::size_t& i, Parts& parts) {
		long long count = 1;
		const bool numbered = parseCount(words[i], count);
		if (numbered && i + 1 >= words.size()) return false;

		const Unit* unit = findUnit(words[numbered ? i + 1 : i]);
		if (unit == nullptr) return false;

		if (numbered) ++i;
		const bool ago = i + 1 < words.size() && words[i + 1] == "ago";
		if (ago) ++i;

		addRelative(*unit, ago ? -count : count, parts);
		return true;
	}

	static bool looksLikeDate(const std::string& word) {
		return word.size() >= 8 && isDigits(word.substr(0, 4)) && word[4] == '-';
	}

	static bool parseWord(const std::vector<std::string>& words, std::size_t& i, Parts& parts) {
		const std::string& word = words[i];
		if (word == "now" || word == "today") return true;
		if (word == "yesterday" || word == "tomorrow") {
			parts.relative_days += word == "yesterday" ? -1 : 1;
			return true;
		}

		if (looksLikeDate(word)) return !parts.has_date && parseDate(word, parts);
		if (unitFollows(words, i)) return parseRelative(words, i, parts);
		if (parseZone(word, parts)) return true;
		if (word.find(':') != std::string::npos) return parseTime(word, parts);
		return parseRelative(words, i, parts);
	}

	static void applyAbsolute(const Parts& parts, std::tm& tm) {
		if (parts.has_date) {
			tm.tm_year = parts.year - 1900;
			tm.tm_mon = parts.month - 1;
			tm.tm_mday = parts.day;
		}

		if (parts.has_date || parts.has_time) {
			tm.tm_hour = parts.hour;
			tm.tm_min = parts.minute;
			tm.tm_sec = parts.second;
		}
	}

	static std::tm localParts(std::time_t when) {
		std::tm tm{};
#ifdef _WIN32
		localtime_s(&tm, &when);
#else
		localtime_r(&when, &tm);
#endif
		return tm;
	}

	static std::time_t utcEpoch(std::tm& tm) {
#ifdef _WIN32
		return _mkgmtime(&tm);
#else
		return timegm(&tm);
#endif
	}

	static bool resolve(const Parts& parts, std::time_t now, DateValue& out) {
		std::tm tm = localParts(now);
		applyAbsolute(parts, tm);
		tm.tm_mon += parts.relative_months;
		tm.tm_mday += parts.relative_days;
		tm.tm_isdst = -1;

		const std::time_t base = parts.has_zone ? utcEpoch(tm) : std::mktime(&tm);
		if (base == static_cast<std::time_t>(-1)) return false;

		const long long zone_seconds = parts.has_zone ? parts.zone_minutes * kSecondsPerMinute : 0;
		out.seconds = static_cast<long long>(base) - zone_seconds + parts.relative_seconds;
		out.nanos = parts.nanos;
		return true;
	}

	static bool parseEpoch(const std::string& text, DateValue& out) {
		const std::size_t dot = text.find('.');
		const std::size_t length = dot == std::string::npos ? std::string::npos : dot - 1;
		const std::string whole = text.substr(1, length);
		if (!parseLL(whole, out.seconds)) return false;
		if (dot == std::string::npos) return true;

		return parseFraction(text.substr(dot + 1), out.nanos);
	}

}  // namespace wbsh::dateparse_detail

namespace wbsh {

	bool parseDateString(const std::string& text, std::time_t now, DateValue& out) {
		using namespace dateparse_detail;

		const std::vector<std::string> words = splitWords(text);
		if (words.size() == 1 && words[0][0] == '@') return parseEpoch(words[0], out);

		Parts parts;
		for (std::size_t i = 0; i < words.size(); ++i) {
			if (!parseWord(words, i, parts)) return false;
		}

		return resolve(parts, now, out);
	}

	static int centuryYear(int two_digits) {
		return two_digits < 69 ? 2000 + two_digits : 1900 + two_digits;
	}

	bool parseTouchStamp(const std::string& text, std::time_t now, DateValue& out) {
		using namespace dateparse_detail;

		const std::size_t dot = text.find('.');
		const std::string digits = text.substr(0, dot);
		const bool known_length = digits.size() == 8 || digits.size() == 10 || digits.size() == 12;
		if (!isDigits(digits) || !known_length) return false;

		Parts parts;
		parts.has_date = true;
		parts.has_time = true;
		const std::size_t year_digits = digits.size() - 8;
		std::tm today = localParts(now);
		parts.year = today.tm_year + 1900;
		if (year_digits == 4) parseInt(digits.substr(0, 4), parts.year);
		if (year_digits == 2) {
			int two = 0;
			parseInt(digits.substr(0, 2), two);
			parts.year = centuryYear(two);
		}

		const std::string rest = digits.substr(year_digits);
		parseInt(rest.substr(0, 2), parts.month);
		parseInt(rest.substr(2, 2), parts.day);
		parseInt(rest.substr(4, 2), parts.hour);
		parseInt(rest.substr(6, 2), parts.minute);
		const bool seconds = dot != std::string::npos;
		if (seconds && !digitsValue(text.substr(dot + 1), parts.second)) return false;
		if (parts.month < 1 || parts.month > 12 || parts.day < 1 || parts.day > 31) return false;

		return resolve(parts, now, out);
	}

}  // namespace wbsh

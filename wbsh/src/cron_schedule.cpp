/**
 * @file cron_schedule.cpp
 * @brief Crontab lines: the five time fields, @keywords, NAME=value lines and the `%` rule.
 *
 * Follows cronie: a field is a list of `*`, numbers, names and ranges,
 * each with an optional `/step` (a lone `N/step` runs from N to the
 * field's end), 7 is Sunday as well as 0, and a field that starts with
 * `*` counts as unrestricted for the day-of-month / day-of-week rule.
 */

#include "cron_schedule.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "numparse.h"

namespace wbsh {

	namespace cron_detail {

		struct FieldSpec {
			int low;
			int high;
			const char* const* names;
			int first_named;
			const char* error;
		};

		static const char* const kMonthNames[] = {
			"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec",
			nullptr,
		};

		static const char* const kWeekdayNames[] = {
			"sun", "mon", "tue", "wed", "thu", "fri", "sat", nullptr,
		};

		static const int kSundayAgain = 7;

		static const FieldSpec kMinuteField  = { 0, 59, nullptr, 0, "bad minute" };
		static const FieldSpec kHourField    = { 0, 23, nullptr, 0, "bad hour" };
		static const FieldSpec kDayField     = { 1, 31, nullptr, 0, "bad day-of-month" };
		static const FieldSpec kMonthField   = { 1, 12, kMonthNames, 1, "bad month" };
		static const FieldSpec kWeekdayField = { 0, kSundayAgain, kWeekdayNames, 0,
			"bad day-of-week" };

		struct Shortcut {
			const char* name;
			const char* fields;
		};

		static const Shortcut kShortcuts[] = {
			{ "@yearly",   "0 0 1 1 *" },
			{ "@annually", "0 0 1 1 *" },
			{ "@monthly",  "0 0 1 * *" },
			{ "@weekly",   "0 0 * * 0" },
			{ "@daily",    "0 0 * * *" },
			{ "@midnight", "0 0 * * *" },
			{ "@hourly",   "0 * * * *" },
		};

		struct Cursor {
			const std::string& text;
			std::size_t pos = 0;
		};

		struct Range {
			int low = 0;
			int high = 0;
			int step = 1;
		};

		struct FieldMasks {
			std::uint64_t minutes = 0;
			std::uint64_t hours = 0;
			std::uint64_t days = 0;
			std::uint64_t months = 0;
			std::uint64_t weekdays = 0;
			bool minute_star = false;
			bool hour_star = false;
			bool month_star = false;
		};

	}  // namespace cron_detail

	using cron_detail::Cursor;
	using cron_detail::FieldMasks;
	using cron_detail::FieldSpec;
	using cron_detail::Range;

	static bool isBlank(char c) {
		return c == ' ' || c == '\t';
	}

	static bool atEnd(const Cursor& cursor) {
		return cursor.pos >= cursor.text.size();
	}

	static void skipBlanks(Cursor& cursor) {
		while (!atEnd(cursor) && isBlank(cursor.text[cursor.pos])) ++cursor.pos;
	}

	static std::string takeWord(Cursor& cursor) {
		skipBlanks(cursor);
		const std::size_t start = cursor.pos;
		while (!atEnd(cursor) && !isBlank(cursor.text[cursor.pos])) ++cursor.pos;
		return cursor.text.substr(start, cursor.pos - start);
	}

	static std::string lowerAscii(std::string text) {
		for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return text;
	}

	static bool startsWithDigit(const std::string& text) {
		return !text.empty() && std::isdigit(static_cast<unsigned char>(text[0])) != 0;
	}

	static bool parseDigits(const std::string& text, int& value) {
		std::size_t used = 0;
		return startsWithDigit(text) && parseInt(text, value, 10, &used) && used == text.size();
	}

	static bool parseName(const std::string& text, const FieldSpec& field, int& value) {
		if (field.names == nullptr) return false;

		const std::string lower = lowerAscii(text);
		for (int i = 0; field.names[i] != nullptr; ++i) {
			if (lower != field.names[i]) continue;
			value = field.first_named + i;
			return true;
		}

		return false;
	}

	static bool parseValue(const std::string& text, const FieldSpec& field, int& value) {
		if (startsWithDigit(text)) return parseDigits(text, value);
		return parseName(text, field, value);
	}

	static bool parseBounds(const std::string& text, const FieldSpec& field, bool stepped,
	                        Range& range) {
		if (text == "*") {
			range.low = field.low;
			range.high = field.high;
			return true;
		}

		const std::size_t dash = text.find('-');
		if (dash == std::string::npos) {
			if (!parseValue(text, field, range.low)) return false;
			range.high = stepped ? field.high : range.low;
			return true;
		}

		return parseValue(text.substr(0, dash), field, range.low)
			&& parseValue(text.substr(dash + 1), field, range.high);
	}

	// `fri-sun` ends on Sunday 0; read it as the 7 that ends the week.
	static bool normalizeRange(Range& range, const FieldSpec& field) {
		if (field.high == cron_detail::kSundayAgain && range.high == 0 && range.low > 0) {
			range.high = cron_detail::kSundayAgain;
		}

		return range.low >= field.low && range.high <= field.high && range.low <= range.high;
	}

	static bool parseElement(const std::string& element, const FieldSpec& field,
	                         std::uint64_t& mask) {
		const std::size_t slash = element.find('/');
		Range range;
		if (slash != std::string::npos && !parseDigits(element.substr(slash + 1), range.step)) {
			return false;
		}

		if (range.step < 1) return false;
		if (!parseBounds(element.substr(0, slash), field, slash != std::string::npos, range)) {
			return false;
		}

		if (!normalizeRange(range, field)) return false;

		for (int value = range.low; value <= range.high; value += range.step) {
			mask |= std::uint64_t{ 1 } << value;
		}

		return true;
	}

	static bool parseField(const std::string& word, const FieldSpec& field, std::uint64_t& mask) {
		if (word.empty()) return false;

		std::size_t start = 0;
		for (;;) {
			const std::size_t comma = word.find(',', start);
			const std::string element = word.substr(start, comma - start);
			if (element.empty() || !parseElement(element, field, mask)) return false;
			if (comma == std::string::npos) return true;
			start = comma + 1;
		}
	}

	static bool takeField(Cursor& cursor, const FieldSpec& field, std::uint64_t& mask, bool& star,
	                      std::string& error) {
		const std::string word = takeWord(cursor);
		star = !word.empty() && word[0] == '*';
		if (parseField(word, field, mask)) return true;

		error = field.error;
		return false;
	}

	static std::uint32_t foldSunday(std::uint64_t weekdays) {
		const std::uint64_t sunday_again = std::uint64_t{ 1 } << cron_detail::kSundayAgain;
		if ((weekdays & sunday_again) != 0) weekdays |= 1;
		return static_cast<std::uint32_t>(weekdays & ~sunday_again);
	}

	static void storeMasks(const FieldMasks& masks, CronSchedule& schedule) {
		schedule.minutes = masks.minutes;
		schedule.hours = static_cast<std::uint32_t>(masks.hours);
		schedule.days = static_cast<std::uint32_t>(masks.days);
		schedule.months = static_cast<std::uint32_t>(masks.months);
		schedule.weekdays = foldSunday(masks.weekdays);
		schedule.minute_or_hour_star = masks.minute_star || masks.hour_star;
	}

	static bool parseTimeFields(Cursor& cursor, CronSchedule& schedule, std::string& error) {
		FieldMasks masks;
		const bool parsed =
			takeField(cursor, cron_detail::kMinuteField, masks.minutes, masks.minute_star, error)
			&& takeField(cursor, cron_detail::kHourField, masks.hours, masks.hour_star, error)
			&& takeField(cursor, cron_detail::kDayField, masks.days, schedule.day_star, error)
			&& takeField(cursor, cron_detail::kMonthField, masks.months, masks.month_star, error)
			&& takeField(cursor, cron_detail::kWeekdayField, masks.weekdays,
				schedule.weekday_star, error);
		if (!parsed) return false;

		storeMasks(masks, schedule);
		return true;
	}

	static bool parseShortcut(const std::string& word, CronSchedule& schedule,
	                          std::string& error) {
		if (word == "@reboot") {
			schedule.at_reboot = true;
			return true;
		}

		for (const auto& shortcut : cron_detail::kShortcuts) {
			if (word != shortcut.name) continue;
			const std::string fields = shortcut.fields;
			Cursor cursor{ fields };
			return parseTimeFields(cursor, schedule, error);
		}

		error = "bad time specifier";
		return false;
	}

	static bool parseScheduleAt(Cursor& cursor, CronSchedule& schedule, std::string& error) {
		skipBlanks(cursor);
		if (!atEnd(cursor) && cursor.text[cursor.pos] == '@') {
			return parseShortcut(takeWord(cursor), schedule, error);
		}

		return parseTimeFields(cursor, schedule, error);
	}

	// An unescaped `%` ends the command; the text after it is the job's
	// standard input, every further `%` a newline. `\%` is a plain `%`.
	static void splitPercent(const std::string& raw, CronEntry& entry) {
		bool in_input = false;
		for (std::size_t i = 0; i < raw.size(); ++i) {
			std::string& target = in_input ? entry.input : entry.command;
			if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '%') {
				target.push_back('%');
				++i;
			} else if (raw[i] != '%') {
				target.push_back(raw[i]);
			} else if (in_input) {
				target.push_back('\n');
			} else {
				in_input = true;
			}
		}

		if (!entry.input.empty() && entry.input.back() != '\n') entry.input.push_back('\n');
	}

	static bool parseEntry(const std::string& line, CronEntry& entry, std::string& error) {
		Cursor cursor{ line };
		if (!parseScheduleAt(cursor, entry.schedule, error)) return false;

		skipBlanks(cursor);
		entry.text = line.substr(cursor.pos);
		if (entry.text.empty()) {
			error = "bad command";
			return false;
		}

		splitPercent(entry.text, entry);
		return true;
	}

	static std::string withoutTrailingBlanks(std::string text) {
		while (!text.empty() && isBlank(text.back())) text.pop_back();
		return text;
	}

	static std::string withoutQuotes(const std::string& text) {
		const bool quoted = text.size() >= 2 && (text[0] == '"' || text[0] == '\'')
			&& text.back() == text[0];
		return quoted ? text.substr(1, text.size() - 2) : text;
	}

	static bool parseVariable(const std::string& line, std::pair<std::string, std::string>& out) {
		Cursor cursor{ line };
		skipBlanks(cursor);
		const std::size_t start = cursor.pos;
		while (!atEnd(cursor) && !isBlank(line[cursor.pos]) && line[cursor.pos] != '=') {
			++cursor.pos;
		}

		if (cursor.pos == start) return false;

		out.first = line.substr(start, cursor.pos - start);
		skipBlanks(cursor);
		if (atEnd(cursor) || line[cursor.pos] != '=') return false;

		++cursor.pos;
		skipBlanks(cursor);
		out.second = withoutQuotes(withoutTrailingBlanks(line.substr(cursor.pos)));
		return true;
	}

	static void setVariable(CronVariables& variables,
	                        std::pair<std::string, std::string> variable) {
		for (auto& existing : variables) {
			if (existing.first != variable.first) continue;
			existing.second = std::move(variable.second);
			return;
		}

		variables.push_back(std::move(variable));
	}

	static bool isIgnorable(const std::string& line) {
		Cursor cursor{ line };
		skipBlanks(cursor);
		return atEnd(cursor) || line[cursor.pos] == '#';
	}

	static void parseLine(const std::string& line, std::size_t number, CronVariables& variables,
	                      CronTable& table) {
		if (isIgnorable(line)) return;

		std::pair<std::string, std::string> variable;
		if (parseVariable(line, variable)) {
			setVariable(variables, std::move(variable));
			return;
		}

		CronEntry entry;
		std::string error;
		if (!parseEntry(line, entry, error)) {
			table.errors.push_back({ number, error });
			return;
		}

		entry.line = number;
		entry.variables = variables;
		table.entries.push_back(std::move(entry));
	}

	static std::size_t bodyStart(const std::string& text) {
		return text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
	}

	CronTable parseCrontab(const std::string& text) {
		CronTable table;
		CronVariables variables;
		std::size_t number = 0;
		for (std::size_t start = bodyStart(text); start < text.size();) {
			std::size_t end = text.find('\n', start);
			if (end == std::string::npos) end = text.size();

			std::string line = text.substr(start, end - start);
			if (!line.empty() && line.back() == '\r') line.pop_back();
			parseLine(line, ++number, variables, table);
			start = end + 1;
		}

		return table;
	}

	bool parseCronSchedule(const std::string& spec, CronSchedule& schedule, std::string& error) {
		Cursor cursor{ spec };
		if (!parseScheduleAt(cursor, schedule, error)) return false;

		skipBlanks(cursor);
		if (atEnd(cursor)) return true;

		error = "unexpected text after the schedule";
		return false;
	}

}  // namespace wbsh

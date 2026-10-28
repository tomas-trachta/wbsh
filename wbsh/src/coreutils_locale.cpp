/**
 * @file coreutils_locale.cpp
 * @brief locale: the active locale settings.
 *
 * Settings come from LC_ALL, LC_<CATEGORY> and LANG as glibc's locale
 * reads them. Where glibc would fall back to POSIX, the Windows user
 * locale stands in (cs-CZ becomes cs_CZ.UTF-8), since a Windows shell
 * rarely has LANG set. Keyword values for C and POSIX are glibc's; for
 * any other locale they come from GetLocaleInfoEx without the user's
 * Control Panel overrides (glibc's locale data is fixed too), with Windows
 * date and time pictures turned into strftime formats.
 */

#include "coreutils_internal.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif /* _WIN32 */

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cwctype>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "executor.h"
#include "optscan.h"
#include "pathconv.h"

namespace wbsh {

#ifdef _WIN32

	namespace locale_detail {

		enum OptId { kOptAll, kOptCharmaps, kOptCategory, kOptKeyword, kOptVerbose };

		enum class Kind { Text, Number, Grouping };

		enum class Source {
			Fixed,
			Info,
			InfoList,
			Charmap,
			Grouping,
			DateTime,
			Picture,
			AmPmPicture,
			FirstWeekday,
			IntlSymbol,
			PositivePrecedes,
			PositiveSeparated,
			NegativePrecedes,
			NegativeSeparated,
			NegativeSignPosition,
			PositiveSignPosition,
			Measurement,
			PaperHeight,
			PaperWidth,
			DialingCode,
		};

		struct Keyword {
			const char* name;
			int category;
			Kind kind;
			Source source;
			const char* c_value;
			LCTYPE types[12];
			int type_count;
		};

		struct LocaleName {
			std::string text;
			std::wstring windows;
			std::string charmap;
			bool is_c = false;
			bool valid = true;
		};

		struct LocaleOptions {
			bool all = false;
			bool charmaps = false;
			bool category_names = false;
			bool keyword_names = false;
			std::vector<std::string> operands;
		};

	}  // namespace locale_detail

	using locale_detail::Keyword;
	using locale_detail::Kind;
	using locale_detail::LocaleName;
	using locale_detail::LocaleOptions;
	using locale_detail::Source;

	static const char* const kCategories[] = {
		"LC_CTYPE", "LC_NUMERIC", "LC_TIME", "LC_COLLATE", "LC_MONETARY", "LC_MESSAGES",
		"LC_PAPER", "LC_NAME", "LC_ADDRESS", "LC_TELEPHONE", "LC_MEASUREMENT", "LC_IDENTIFICATION",
	};

	static const int kCtype = 0;
	static const int kNumeric = 1;
	static const int kTime = 2;
	static const int kMonetary = 4;
	static const int kMessages = 5;
	static const int kPaper = 6;
	static const int kName = 7;
	static const int kAddress = 8;
	static const int kTelephone = 9;
	static const int kMeasurement = 10;
	static const int kIdentification = 11;

	static const char* const kAsciiCharmap = "ANSI_X3.4-1968";

	static const char* const kCharmaps[] = {
		"ANSI_X3.4-1968", "BIG5", "CP1250", "CP1251", "CP1252", "CP1253", "CP1254", "CP1255",
		"CP1256", "CP1257", "CP1258", "CP437", "CP850", "CP852", "CP866", "CP932", "CP949",
		"EUC-JP", "EUC-KR", "GB18030", "GBK", "ISO-8859-1", "ISO-8859-15", "ISO-8859-2",
		"ISO-8859-5", "ISO-8859-7", "KOI8-R", "KOI8-U", "MACINTOSH", "SHIFT_JIS", "UTF-8",
	};

	static const Keyword kKeywords[] = {
		{ "charmap", kCtype, Kind::Text, Source::Charmap, "", {}, 0 },
		{ "decimal_point", kNumeric, Kind::Text, Source::Info, ".", { LOCALE_SDECIMAL }, 1 },
		{ "thousands_sep", kNumeric, Kind::Text, Source::Info, "", { LOCALE_STHOUSAND }, 1 },
		{ "grouping", kNumeric, Kind::Grouping, Source::Grouping, "-1", { LOCALE_SGROUPING }, 1 },
		{ "numeric-codeset", kNumeric, Kind::Text, Source::Charmap, "", {}, 0 },
		{ "abday", kTime, Kind::Text, Source::InfoList, "Sun;Mon;Tue;Wed;Thu;Fri;Sat",
			{ LOCALE_SABBREVDAYNAME7, LOCALE_SABBREVDAYNAME1, LOCALE_SABBREVDAYNAME2,
			  LOCALE_SABBREVDAYNAME3, LOCALE_SABBREVDAYNAME4, LOCALE_SABBREVDAYNAME5,
			  LOCALE_SABBREVDAYNAME6 }, 7 },
		{ "day", kTime, Kind::Text, Source::InfoList,
			"Sunday;Monday;Tuesday;Wednesday;Thursday;Friday;Saturday",
			{ LOCALE_SDAYNAME7, LOCALE_SDAYNAME1, LOCALE_SDAYNAME2, LOCALE_SDAYNAME3,
			  LOCALE_SDAYNAME4, LOCALE_SDAYNAME5, LOCALE_SDAYNAME6 }, 7 },
		{ "abmon", kTime, Kind::Text, Source::InfoList,
			"Jan;Feb;Mar;Apr;May;Jun;Jul;Aug;Sep;Oct;Nov;Dec",
			{ LOCALE_SABBREVMONTHNAME1, LOCALE_SABBREVMONTHNAME2, LOCALE_SABBREVMONTHNAME3,
			  LOCALE_SABBREVMONTHNAME4, LOCALE_SABBREVMONTHNAME5, LOCALE_SABBREVMONTHNAME6,
			  LOCALE_SABBREVMONTHNAME7, LOCALE_SABBREVMONTHNAME8, LOCALE_SABBREVMONTHNAME9,
			  LOCALE_SABBREVMONTHNAME10, LOCALE_SABBREVMONTHNAME11, LOCALE_SABBREVMONTHNAME12 },
			12 },
		{ "mon", kTime, Kind::Text, Source::InfoList,
			"January;February;March;April;May;June;July;August;September;October;November;December",
			{ LOCALE_SMONTHNAME1, LOCALE_SMONTHNAME2, LOCALE_SMONTHNAME3, LOCALE_SMONTHNAME4,
			  LOCALE_SMONTHNAME5, LOCALE_SMONTHNAME6, LOCALE_SMONTHNAME7, LOCALE_SMONTHNAME8,
			  LOCALE_SMONTHNAME9, LOCALE_SMONTHNAME10, LOCALE_SMONTHNAME11, LOCALE_SMONTHNAME12 },
			12 },
		{ "am_pm", kTime, Kind::Text, Source::InfoList, "AM;PM",
			{ LOCALE_S1159, LOCALE_S2359 }, 2 },
		{ "d_t_fmt", kTime, Kind::Text, Source::DateTime, "%a %b %e %H:%M:%S %Y",
			{ LOCALE_SLONGDATE, LOCALE_STIMEFORMAT }, 2 },
		{ "d_fmt", kTime, Kind::Text, Source::Picture, "%m/%d/%y", { LOCALE_SSHORTDATE }, 1 },
		{ "t_fmt", kTime, Kind::Text, Source::Picture, "%H:%M:%S", { LOCALE_STIMEFORMAT }, 1 },
		{ "t_fmt_ampm", kTime, Kind::Text, Source::AmPmPicture, "%I:%M:%S %p",
			{ LOCALE_STIMEFORMAT }, 1 },
		{ "first_weekday", kTime, Kind::Number, Source::FirstWeekday, "1",
			{ LOCALE_IFIRSTDAYOFWEEK }, 1 },
		{ "time-codeset", kTime, Kind::Text, Source::Charmap, "", {}, 0 },
		{ "int_curr_symbol", kMonetary, Kind::Text, Source::IntlSymbol, "",
			{ LOCALE_SINTLSYMBOL }, 1 },
		{ "currency_symbol", kMonetary, Kind::Text, Source::Info, "", { LOCALE_SCURRENCY }, 1 },
		{ "mon_decimal_point", kMonetary, Kind::Text, Source::Info, "",
			{ LOCALE_SMONDECIMALSEP }, 1 },
		{ "mon_thousands_sep", kMonetary, Kind::Text, Source::Info, "",
			{ LOCALE_SMONTHOUSANDSEP }, 1 },
		{ "mon_grouping", kMonetary, Kind::Grouping, Source::Grouping, "-1",
			{ LOCALE_SMONGROUPING }, 1 },
		{ "positive_sign", kMonetary, Kind::Text, Source::Info, "", { LOCALE_SPOSITIVESIGN }, 1 },
		{ "negative_sign", kMonetary, Kind::Text, Source::Info, "", { LOCALE_SNEGATIVESIGN }, 1 },
		{ "int_frac_digits", kMonetary, Kind::Number, Source::Info, "-1",
			{ LOCALE_IINTLCURRDIGITS }, 1 },
		{ "frac_digits", kMonetary, Kind::Number, Source::Info, "-1", { LOCALE_ICURRDIGITS }, 1 },
		{ "p_cs_precedes", kMonetary, Kind::Number, Source::PositivePrecedes, "-1", {}, 0 },
		{ "p_sep_by_space", kMonetary, Kind::Number, Source::PositiveSeparated, "-1", {}, 0 },
		{ "n_cs_precedes", kMonetary, Kind::Number, Source::NegativePrecedes, "-1", {}, 0 },
		{ "n_sep_by_space", kMonetary, Kind::Number, Source::NegativeSeparated, "-1", {}, 0 },
		{ "p_sign_posn", kMonetary, Kind::Number, Source::PositiveSignPosition, "-1", {}, 0 },
		{ "n_sign_posn", kMonetary, Kind::Number, Source::NegativeSignPosition, "-1", {}, 0 },
		{ "monetary-codeset", kMonetary, Kind::Text, Source::Charmap, "", {}, 0 },
		{ "yesexpr", kMessages, Kind::Text, Source::Fixed, "^[yY]", {}, 0 },
		{ "noexpr", kMessages, Kind::Text, Source::Fixed, "^[nN]", {}, 0 },
		{ "yesstr", kMessages, Kind::Text, Source::Fixed, "", {}, 0 },
		{ "nostr", kMessages, Kind::Text, Source::Fixed, "", {}, 0 },
		{ "messages-codeset", kMessages, Kind::Text, Source::Charmap, "", {}, 0 },
		{ "height", kPaper, Kind::Number, Source::PaperHeight, "297", {}, 0 },
		{ "width", kPaper, Kind::Number, Source::PaperWidth, "210", {}, 0 },
		{ "paper-codeset", kPaper, Kind::Text, Source::Charmap, "", {}, 0 },
		{ "name_fmt", kName, Kind::Text, Source::Fixed, "%p%t%g%t%m%t%f", {}, 0 },
		{ "name-codeset", kName, Kind::Text, Source::Charmap, "", {}, 0 },
		{ "country_name", kAddress, Kind::Text, Source::Info, "",
			{ LOCALE_SENGLISHCOUNTRYNAME }, 1 },
		{ "country_ab2", kAddress, Kind::Text, Source::Info, "", { LOCALE_SISO3166CTRYNAME }, 1 },
		{ "country_ab3", kAddress, Kind::Text, Source::Info, "", { LOCALE_SISO3166CTRYNAME2 }, 1 },
		{ "lang_name", kAddress, Kind::Text, Source::Info, "", { LOCALE_SNATIVELANGUAGENAME }, 1 },
		{ "lang_ab", kAddress, Kind::Text, Source::Info, "", { LOCALE_SISO639LANGNAME }, 1 },
		{ "address-codeset", kAddress, Kind::Text, Source::Charmap, "", {}, 0 },
		{ "tel_int_fmt", kTelephone, Kind::Text, Source::Fixed, "+%c %a %l", {}, 0 },
		{ "int_prefix", kTelephone, Kind::Text, Source::DialingCode, "", {}, 0 },
		{ "telephone-codeset", kTelephone, Kind::Text, Source::Charmap, "", {}, 0 },
		{ "measurement", kMeasurement, Kind::Number, Source::Measurement, "1",
			{ LOCALE_IMEASURE }, 1 },
		{ "measurement-codeset", kMeasurement, Kind::Text, Source::Charmap, "", {}, 0 },
		{ "title", kIdentification, Kind::Text, Source::Info, "ISO/IEC 14652 i18n FDCC-set",
			{ LOCALE_SENGLISHDISPLAYNAME }, 1 },
		{ "language", kIdentification, Kind::Text, Source::Info, "",
			{ LOCALE_SENGLISHLANGUAGENAME }, 1 },
		{ "territory", kIdentification, Kind::Text, Source::Info, "ISO",
			{ LOCALE_SENGLISHCOUNTRYNAME }, 1 },
		{ "revision", kIdentification, Kind::Text, Source::Fixed, "1.0", {}, 0 },
		{ "identification-codeset", kIdentification, Kind::Text, Source::Charmap, "", {}, 0 },
	};

	struct CurrencyLayout {
		int precedes;
		int separated;
		int sign_position;
	};

	static const CurrencyLayout kNegativeLayouts[] = {
		{ 1, 0, 0 }, { 1, 0, 1 }, { 1, 0, 4 }, { 1, 0, 2 }, { 0, 0, 0 }, { 0, 0, 1 },
		{ 0, 0, 3 }, { 0, 0, 2 }, { 0, 1, 1 }, { 1, 1, 1 }, { 0, 1, 2 }, { 1, 1, 2 },
		{ 1, 1, 4 }, { 0, 1, 3 }, { 1, 1, 0 }, { 0, 1, 0 },
	};

	static const CurrencyLayout kPositiveLayouts[] = {
		{ 1, 0, 1 }, { 0, 0, 1 }, { 1, 1, 1 }, { 0, 1, 1 },
	};

	static std::string upperAlnum(const std::string& text) {
		std::string out;
		for (const char c : text) {
			if (std::isalnum(static_cast<unsigned char>(c)) == 0) continue;
			out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
		}

		return out;
	}

	static std::string charmapNamed(const std::string& codeset) {
		const std::string key = upperAlnum(codeset);
		if (key == "UTF8") return "UTF-8";
		if (key == "ASCII" || key == "USASCII") return kAsciiCharmap;
		for (const char* charmap : kCharmaps) {
			if (upperAlnum(charmap) == key) return charmap;
		}

		return std::string();
	}

	static std::wstring windowsScript(const std::string& modifier) {
		if (modifier == "latin") return L"-Latn";
		if (modifier == "cyrillic") return L"-Cyrl";
		return std::wstring();
	}

	static std::string posixLocaleName(const std::wstring& windows) {
		const std::string text = wideToUtf8(windows);
		std::vector<std::string> parts;
		std::size_t start = 0;
		while (start <= text.size()) {
			const std::size_t dash = std::min(text.find('-', start), text.size());
			parts.push_back(text.substr(start, dash - start));
			start = dash + 1;
		}

		const std::string& region = parts.back();
		const bool alpha_region = region.size() == 2
			&& std::isupper(static_cast<unsigned char>(region[0])) != 0;
		if (parts.size() < 2 || parts.size() > 3 || !alpha_region) return std::string();

		std::string modifier;
		if (parts.size() == 3 && parts[1] == "Latn") modifier = "@latin";
		if (parts.size() == 3 && parts[1] == "Cyrl") modifier = "@cyrillic";
		return parts[0] + "_" + region + ".UTF-8" + modifier;
	}

	static std::string userDefaultLocale() {
		wchar_t name[LOCALE_NAME_MAX_LENGTH];
		if (::GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) == 0) return "C.UTF-8";

		const std::string posix = posixLocaleName(name);
		return posix.empty() ? "C.UTF-8" : posix;
	}

	static std::wstring lowerWide(const std::wstring& text) {
		std::wstring out = text;
		for (wchar_t& c : out) c = static_cast<wchar_t>(std::towlower(c));
		return out;
	}

	static BOOL CALLBACK collectLocaleName(LPWSTR name, DWORD, LPARAM context) {
		std::set<std::wstring>& names = *reinterpret_cast<std::set<std::wstring>*>(context);
		names.insert(name);
		return TRUE;
	}

	// IsValidLocaleName accepts any well-formed tag (xx-QQ included) since
	// Windows 10 constructs locales on demand, so validity is membership in
	// the list Windows enumerates.
	static const std::set<std::wstring>& windowsLocales() {
		static const std::set<std::wstring> names = [] {
			std::set<std::wstring> found;
			::EnumSystemLocalesEx(collectLocaleName, LOCALE_WINDOWS,
				reinterpret_cast<LPARAM>(&found), nullptr);
			return found;
		}();
		return names;
	}

	static bool isKnownWindowsLocale(const std::wstring& name) {
		const std::wstring wanted = lowerWide(name);
		for (const std::wstring& known : windowsLocales()) {
			if (lowerWide(known) == wanted) return true;
		}

		return false;
	}

	static void splitLocaleName(const std::string& text, std::string& base, std::string& codeset,
	                            std::string& modifier) {
		const std::size_t at = text.find('@');
		modifier = at == std::string::npos ? "" : text.substr(at + 1);
		const std::string head = text.substr(0, at);
		const std::size_t dot = head.find('.');
		codeset = dot == std::string::npos ? "" : head.substr(dot + 1);
		base = head.substr(0, dot);
	}

	static LocaleName describeLocale(const std::string& text) {
		LocaleName locale;
		locale.text = text;
		std::string base;
		std::string codeset;
		std::string modifier;
		splitLocaleName(text, base, codeset, modifier);
		locale.charmap = codeset.empty() ? "UTF-8" : charmapNamed(codeset);
		locale.is_c = base == "C" || base == "POSIX";
		if (locale.is_c && codeset.empty()) locale.charmap = kAsciiCharmap;
		if (locale.is_c) {
			locale.valid = !locale.charmap.empty() && modifier.empty();
			return locale;
		}

		const std::size_t underscore = base.find('_');
		std::wstring windows = utf8ToWide(base.substr(0, underscore)) + windowsScript(modifier);
		if (underscore != std::string::npos) {
			windows += L"-" + utf8ToWide(base.substr(underscore + 1));
		}

		locale.windows = windows;
		locale.valid = !locale.charmap.empty() && !base.empty()
			&& isKnownWindowsLocale(windows);
		return locale;
	}

	static bool inEnvironment(Executor& exec, const std::string& name) {
		return exec.env().has(name) && exec.env().isExported(name);
	}

	static std::string environmentValue(Executor& exec, const std::string& name) {
		return inEnvironment(exec, name) ? exec.env().get(name) : std::string();
	}

	static std::string effectiveLocaleText(Executor& exec, int category) {
		const std::string all = environmentValue(exec, "LC_ALL");
		if (!all.empty()) return all;

		const std::string own = environmentValue(exec, kCategories[category]);
		if (!own.empty()) return own;

		const std::string lang = environmentValue(exec, "LANG");
		return lang.empty() ? userDefaultLocale() : lang;
	}

	static void printLocaleVariables(Executor& exec) {
		const std::string all = environmentValue(exec, "LC_ALL");
		const std::string lang = environmentValue(exec, "LANG");
		const std::string implied = !all.empty() ? all : !lang.empty() ? lang : userDefaultLocale();
		std::printf("LANG=%s\n", lang.c_str());
		std::printf("LANGUAGE=%s\n", environmentValue(exec, "LANGUAGE").c_str());
		for (const char* category : kCategories) {
			const bool set = inEnvironment(exec, category);
			if (!all.empty() || !set) std::printf("%s=\"%s\"\n", category, implied.c_str());
			else std::printf("%s=%s\n", category, environmentValue(exec, category).c_str());
		}

		std::printf("LC_ALL=%s\n", all.c_str());
	}

	static void warnUnusableLocales(Executor& exec) {
		bool all_valid = true;
		for (int category = 0; category < static_cast<int>(std::size(kCategories)); ++category) {
			const bool valid = describeLocale(effectiveLocaleText(exec, category)).valid;
			if (valid) continue;

			all_valid = false;
			if (category != kCtype && category != kMessages) continue;
			perr("locale", std::string("Cannot set ") + kCategories[category]
				+ " to default locale: No such file or directory");
		}

		if (all_valid) return;

		perr("locale", "Cannot set LC_ALL to default locale: No such file or directory");
	}

	static std::string infoText(const std::wstring& locale, LCTYPE type) {
		wchar_t buffer[256];
		const int length = ::GetLocaleInfoEx(locale.c_str(), type | LOCALE_NOUSEROVERRIDE, buffer,
			256);
		if (length <= 1) return std::string();
		return wideToUtf8(std::wstring(buffer, static_cast<std::size_t>(length - 1)));
	}

	static int infoNumber(const std::wstring& locale, LCTYPE type) {
		DWORD value = 0;
		const LCTYPE query = type | LOCALE_NOUSEROVERRIDE | LOCALE_RETURN_NUMBER;
		const int got = ::GetLocaleInfoEx(locale.c_str(), query, reinterpret_cast<LPWSTR>(&value),
			sizeof(value) / sizeof(wchar_t));
		return got == 0 ? -1 : static_cast<int>(value);
	}

	static std::string infoList(const std::wstring& locale, const Keyword& keyword) {
		std::string joined;
		for (int k = 0; k < keyword.type_count; ++k) {
			if (k > 0) joined += ";";
			joined += infoText(locale, keyword.types[k]);
		}

		return joined;
	}

	static std::string groupingText(const std::string& windows) {
		std::vector<std::string> sizes;
		std::size_t start = 0;
		while (start <= windows.size()) {
			const std::size_t semicolon = std::min(windows.find(';', start), windows.size());
			sizes.push_back(windows.substr(start, semicolon - start));
			start = semicolon + 1;
		}

		const bool repeats = sizes.size() > 1 && sizes.back() == "0";
		if (repeats) sizes.pop_back();
		if (repeats && sizes.size() == 1) sizes.push_back(sizes[0]);
		if (!repeats) sizes.push_back("-1");

		std::string text;
		for (const std::string& size : sizes) text += (text.empty() ? "" : ";") + size;
		return text;
	}

	static std::size_t runLength(const std::string& picture, std::size_t at) {
		std::size_t end = at;
		while (end < picture.size() && picture[end] == picture[at]) ++end;
		return end - at;
	}

	static std::string pictureField(char letter, std::size_t count) {
		const bool single = count == 1;
		switch (letter) {
			case 'd':
				return count >= 4 ? "%A" : count == 3 ? "%a" : single ? "%-d" : "%d";
			case 'M':
				return count >= 4 ? "%B" : count == 3 ? "%b" : single ? "%-m" : "%m";
			case 'y':
				return count <= 2 ? "%y" : "%Y";
			case 'h':
				return single ? "%-I" : "%I";
			case 'H':
				return single ? "%-H" : "%H";
			case 'm':
				return single ? "%-M" : "%M";
			case 's':
				return single ? "%-S" : "%S";
			case 't':
				return "%p";
			case 'g':
				return "";
			default:
				return std::string(count, letter);
		}
	}

	static std::size_t appendQuoted(const std::string& picture, std::size_t at, std::string& out) {
		std::size_t end = at + 1;
		while (end < picture.size() && picture[end] != '\'') {
			out += picture[end] == '%' ? std::string("%%") : std::string(1, picture[end]);
			++end;
		}

		if (end == at + 1 && end < picture.size()) out += "'";
		return end + 1;
	}

	static std::string strftimeFromPicture(const std::string& picture) {
		std::string out;
		std::size_t at = 0;
		while (at < picture.size()) {
			if (picture[at] == '\'') {
				at = appendQuoted(picture, at, out);
				continue;
			}

			if (picture[at] == '%') {
				out += "%%";
				++at;
				continue;
			}

			const std::size_t count = runLength(picture, at);
			out += pictureField(picture[at], count);
			at += count;
		}

		return out;
	}

	static std::string amPmPicture(const std::wstring& locale) {
		const std::string picture = infoText(locale, LOCALE_STIMEFORMAT);
		if (picture.find('t') != std::string::npos) return strftimeFromPicture(picture);
		return "%I:%M:%S %p";
	}

	static std::string paperDimension(const std::wstring& locale, bool height) {
		switch (infoNumber(locale, LOCALE_IPAPERSIZE)) {
			case 1:
				return height ? "279" : "216";
			case 5:
				return height ? "356" : "216";
			case 8:
				return height ? "420" : "297";
			default:
				return height ? "297" : "210";
		}
	}

	static std::string dialingCode(const std::wstring& locale) {
		const int geo = infoNumber(locale, LOCALE_IGEOID);
		wchar_t code[32];
		if (geo < 0 || ::GetGeoInfoW(geo, GEO_DIALINGCODE, code, 32, 0) <= 1) return std::string();
		return wideToUtf8(code);
	}

	static bool isNegativeLayout(Source source) {
		return source == Source::NegativePrecedes || source == Source::NegativeSeparated
			|| source == Source::NegativeSignPosition;
	}

	static int layoutField(const CurrencyLayout& layout, Source source) {
		if (source == Source::PositivePrecedes || source == Source::NegativePrecedes) {
			return layout.precedes;
		}

		const bool sign = source == Source::NegativeSignPosition
			|| source == Source::PositiveSignPosition;
		if (sign) return layout.sign_position;
		return layout.separated;
	}

	static std::string currencyLayout(const std::wstring& locale, Source source) {
		const bool negative = isNegativeLayout(source);
		const int pattern = infoNumber(locale, negative ? LOCALE_INEGCURR : LOCALE_ICURRENCY);
		const int limit = negative ? 16 : 4;
		if (pattern < 0 || pattern >= limit) return "-1";

		const CurrencyLayout& layout = negative ? kNegativeLayouts[pattern]
			: kPositiveLayouts[pattern];
		return std::to_string(layoutField(layout, source));
	}

	static std::string pictureValue(const std::wstring& locale, const Keyword& keyword) {
		if (keyword.source == Source::AmPmPicture) return amPmPicture(locale);
		if (keyword.source == Source::Picture) {
			return strftimeFromPicture(infoText(locale, keyword.types[0]));
		}

		return strftimeFromPicture(infoText(locale, keyword.types[0])) + " "
			+ strftimeFromPicture(infoText(locale, keyword.types[1]));
	}

	static std::string numericInfoValue(const std::wstring& locale, const Keyword& keyword) {
		const int value = infoNumber(locale, keyword.types[0]);
		switch (keyword.source) {
			case Source::FirstWeekday:
				return std::to_string(value < 0 ? 1 : (value + 1) % 7 + 1);
			case Source::Measurement:
				return value == 1 ? "2" : "1";
			default:
				return std::to_string(value);
		}
	}

	static bool computedValue(const LocaleName& locale, const Keyword& keyword,
	                          std::string& value) {
		const std::wstring& name = locale.windows;
		switch (keyword.source) {
			case Source::Fixed:
				value = keyword.c_value;
				return true;
			case Source::InfoList:
				value = infoList(name, keyword);
				return true;
			case Source::Grouping:
				value = groupingText(infoText(name, keyword.types[0]));
				return true;
			case Source::IntlSymbol:
				value = infoText(name, keyword.types[0]) + " ";
				return true;
			case Source::PaperHeight:
			case Source::PaperWidth:
				value = paperDimension(name, keyword.source == Source::PaperHeight);
				return true;
			case Source::DialingCode:
				value = dialingCode(name);
				return true;
			case Source::DateTime:
			case Source::Picture:
			case Source::AmPmPicture:
				value = pictureValue(name, keyword);
				return true;
			default:
				return false;
		}
	}

	static std::string windowsValue(const LocaleName& locale, const Keyword& keyword) {
		std::string value;
		if (computedValue(locale, keyword, value)) return value;
		if (keyword.source != Source::Info && keyword.source != Source::FirstWeekday
				&& keyword.source != Source::Measurement) {
			return currencyLayout(locale.windows, keyword.source);
		}

		if (keyword.kind == Kind::Number) return numericInfoValue(locale.windows, keyword);
		return infoText(locale.windows, keyword.types[0]);
	}

	static std::string keywordValue(const LocaleName& locale, const Keyword& keyword) {
		if (keyword.source == Source::Charmap) return locale.charmap;
		if (locale.is_c || !locale.valid) return keyword.c_value;
		return windowsValue(locale, keyword);
	}

	static void printKeyword(Executor& exec, const Keyword& keyword, const LocaleOptions& options) {
		const LocaleName locale = describeLocale(effectiveLocaleText(exec, keyword.category));
		const std::string value = keywordValue(locale, keyword);
		if (options.category_names) std::printf("%s\n", kCategories[keyword.category]);
		if (!options.keyword_names) {
			std::printf("%s\n", value.c_str());
			return;
		}

		if (keyword.kind == Kind::Text) std::printf("%s=\"%s\"\n", keyword.name, value.c_str());
		else std::printf("%s=%s\n", keyword.name, value.c_str());
	}

	static bool printCategory(Executor& exec, const std::string& name,
	                          const LocaleOptions& options) {
		int category = -1;
		for (int k = 0; k < static_cast<int>(std::size(kCategories)); ++k) {
			if (name == kCategories[k]) category = k;
		}

		if (category < 0) return false;

		LocaleOptions inner = options;
		inner.category_names = false;
		if (options.category_names) std::printf("%s\n", name.c_str());
		for (const Keyword& keyword : kKeywords) {
			if (keyword.category == category) printKeyword(exec, keyword, inner);
		}

		return true;
	}

	static void printOperand(Executor& exec, const std::string& name,
	                         const LocaleOptions& options) {
		if (printCategory(exec, name, options)) return;

		const bool codeset_alias = name == "codeset";
		for (const Keyword& keyword : kKeywords) {
			if (name == keyword.name || (codeset_alias && keyword.source == Source::Charmap)) {
				printKeyword(exec, keyword, options);
				return;
			}
		}
	}

	static void printAllLocales() {
		std::set<std::string> names;
		for (const std::wstring& windows : windowsLocales()) {
			const std::string posix = posixLocaleName(windows);
			if (!posix.empty()) names.insert(posix);
		}

		std::printf("C\nC.UTF-8\nPOSIX\n");
		for (const std::string& name : names) std::printf("%s\n", name.c_str());
	}

	static void printCharmaps() {
		for (const char* charmap : kCharmaps) std::printf("%s\n", charmap);
	}

	static const std::vector<OptDef>& localeOptionDefs() {
		static const std::vector<OptDef> defs = {
			{ locale_detail::kOptAll,      'a', "all-locales",   OptArg::None },
			{ locale_detail::kOptCharmaps, 'm', "charmaps",      OptArg::None },
			{ locale_detail::kOptCategory, 'c', "category-name", OptArg::None },
			{ locale_detail::kOptKeyword,  'k', "keyword-name",  OptArg::None },
			{ locale_detail::kOptVerbose,  'v', "verbose",       OptArg::None },
		};
		return defs;
	}

	static bool parseLocaleArgs(const std::vector<std::string>& args, LocaleOptions& options) {
		OptScanResult scanned;
		if (!scanOptions("locale", args, localeOptionDefs(), scanned)) return false;

		for (const OptHit& hit : scanned.hits) {
			if (hit.id == locale_detail::kOptAll) options.all = true;
			else if (hit.id == locale_detail::kOptCharmaps) options.charmaps = true;
			else if (hit.id == locale_detail::kOptCategory) options.category_names = true;
			else if (hit.id == locale_detail::kOptKeyword) options.keyword_names = true;
		}

		options.operands = scanned.operands;
		return true;
	}

	static int builtin_locale(Executor& exec, const std::vector<std::string>& args) {
		LocaleOptions options;
		if (!parseLocaleArgs(args, options)) return 1;

		if (options.all) printAllLocales();
		if (options.charmaps) printCharmaps();
		if (options.all || options.charmaps) return 0;

		warnUnusableLocales(exec);
		if (options.operands.empty()) printLocaleVariables(exec);
		for (const std::string& operand : options.operands) printOperand(exec, operand, options);
		return 0;
	}

	void registerLocaleBuiltin(Executor& exec) {
		exec.registerBuiltin("locale", builtin_locale);
	}

#else

	void registerLocaleBuiltin(Executor&) {}

#endif /* _WIN32 */

}  // namespace wbsh

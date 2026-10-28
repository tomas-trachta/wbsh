/**
 * @file timefmt.cpp
 * @brief strftime with the GNU extensions shell scripts use.
 */

#include "timefmt.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace wbsh {

	// The conversions MSVC's strftime handles; anything else would reach
	// its invalid-parameter handler, so it is never passed through.
	static const char* const kRuntimeConversions = "aAbBcCdDeFgGhHIjmMprRSTuUVwWxXyYzZ";

	static std::tm localParts(std::time_t epoch) {
		std::tm parts{};
#ifdef _WIN32
		if (localtime_s(&parts, &epoch) != 0) parts = std::tm{};
#else
		localtime_r(&epoch, &parts);
#endif
		return parts;
	}

	static std::string runtimeConversion(char conversion, const std::tm& parts) {
		const char format[] = { '%', conversion, '\0' };
		char text[256];
		const std::size_t length = std::strftime(text, sizeof(text), format, &parts);
		return std::string(text, length);
	}

	static std::string spacePadded(int value) {
		char text[16];
		std::snprintf(text, sizeof(text), "%2d", value);
		return text;
	}

	static int twelveHour(int hour) {
		const int value = hour % 12;
		return value == 0 ? 12 : value;
	}

	static bool conversionText(char conversion, const std::tm& parts, std::time_t epoch,
			std::string& text) {
		switch (conversion) {
			case 's':
				text = std::to_string(static_cast<long long>(epoch));
				return true;
			case 'k':
				text = spacePadded(parts.tm_hour);
				return true;
			case 'l':
				text = spacePadded(twelveHour(parts.tm_hour));
				return true;
			case 'P':
				text = parts.tm_hour < 12 ? "am" : "pm";
				return true;
			case 'n':
				text = "\n";
				return true;
			case 't':
				text = "\t";
				return true;
			case '%':
				text = "%";
				return true;
			default:
				if (conversion == '\0' || std::strchr(kRuntimeConversions, conversion) == nullptr) {
					return false;
				}

				text = runtimeConversion(conversion, parts);
				return true;
		}
	}

	static void replaceLeadingPadding(std::string& text, char from, char to) {
		for (std::size_t i = 0; i + 1 < text.size() && text[i] == from; ++i) text[i] = to;
	}

	static void stripLeadingPadding(std::string& text) {
		std::size_t start = 0;
		while (start + 1 < text.size() && (text[start] == '0' || text[start] == ' ')) ++start;
		text.erase(0, start);
	}

	static void upperCase(std::string& text) {
		for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	}

	static void applyPaddingFlag(char flag, std::string& text) {
		switch (flag) {
			case '-':
				stripLeadingPadding(text);
				break;
			case '_':
				replaceLeadingPadding(text, '0', ' ');
				break;
			case '0':
				replaceLeadingPadding(text, ' ', '0');
				break;
			case '^':
				upperCase(text);
				break;
			default:
				break;
		}
	}

	static bool isPaddingFlag(char c) {
		return c == '-' || c == '_' || c == '0' || c == '^';
	}

	std::string formatLocalTime(const std::string& format, std::time_t epoch) {
		const std::tm parts = localParts(epoch);
		std::string out;
		for (std::size_t i = 0; i < format.size(); ++i) {
			if (format[i] != '%' || i + 1 >= format.size()) {
				out.push_back(format[i]);
				continue;
			}

			const std::size_t start = i;
			const char flag = isPaddingFlag(format[i + 1]) ? format[++i] : '\0';
			const char conversion = i + 1 < format.size() ? format[++i] : '\0';

			std::string text;
			if (!conversionText(conversion, parts, epoch, text)) {
				out.append(format, start, i + 1 - start);
				continue;
			}

			applyPaddingFlag(flag, text);
			out += text;
		}

		return out;
	}

}  // namespace wbsh

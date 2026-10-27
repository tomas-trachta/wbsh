/**
 * @file awk_value.cpp
 * @brief awk number / string conversions, string escapes and printf formatting.
 */

#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "awk_internal.h"
#include "numparse.h"

namespace wbsh {

	namespace awk_detail {

		// Below 2^53 every integral double converts to long long exactly.
		static const double kExactIntegerLimit = 9007199254740992.0;

		static bool isSpace(char c) {
			return std::isspace(static_cast<unsigned char>(c)) != 0;
		}

		static bool isDigit(char c) {
			return std::isdigit(static_cast<unsigned char>(c)) != 0;
		}

		static std::size_t skipSpaces(const std::string& text, std::size_t i) {
			while (i < text.size() && isSpace(text[i])) ++i;
			return i;
		}

		static std::size_t skipDigitsFrom(const std::string& text, std::size_t i) {
			while (i < text.size() && isDigit(text[i])) ++i;
			return i;
		}

		static bool isSign(const std::string& text, std::size_t i) {
			return i < text.size() && (text[i] == '+' || text[i] == '-');
		}

		static std::size_t skipExponent(const std::string& text, std::size_t i) {
			if (i >= text.size() || (text[i] != 'e' && text[i] != 'E')) return i;

			std::size_t digits = i + 1;
			if (isSign(text, digits)) ++digits;
			const std::size_t end = skipDigitsFrom(text, digits);
			return end > digits ? end : i;
		}

		static std::size_t scanDecimal(const std::string& text, std::size_t start) {
			std::size_t i = isSign(text, start) ? start + 1 : start;
			const std::size_t integer_end = skipDigitsFrom(text, i);
			bool any_digit = integer_end > i;

			i = integer_end;
			if (i < text.size() && text[i] == '.') {
				const std::size_t fraction_end = skipDigitsFrom(text, i + 1);
				any_digit = any_digit || fraction_end > i + 1;
				i = fraction_end;
			}

			if (!any_digit) return 0;
			return skipExponent(text, i) - start;
		}

		static bool equalsIgnoringCase(const std::string& text, std::size_t pos, const char* word) {
			for (std::size_t i = 0; word[i] != '\0'; ++i) {
				if (pos + i >= text.size()) return false;

				const auto c = static_cast<unsigned char>(text[pos + i]);
				if (std::tolower(c) != word[i]) return false;
			}

			return true;
		}

		// gawk reads "+inf" / "-nan" but, without the sign, "inf" is just a word.
		static std::size_t scanSignedSpecial(const std::string& text, std::size_t start) {
			if (!isSign(text, start)) return 0;
			if (equalsIgnoringCase(text, start + 1, "inf")) return 4;
			if (equalsIgnoringCase(text, start + 1, "nan")) return 4;
			return 0;
		}

		static std::size_t scanAwkNumber(const std::string& text, std::size_t start) {
			const std::size_t special = scanSignedSpecial(text, start);
			if (special != 0) return special;
			return scanDecimal(text, start);
		}

		static bool hasNegativeExponent(const std::string& number_text) {
			const std::size_t e = number_text.find_first_of("eE");
			return e != std::string::npos && number_text.find('-', e) != std::string::npos;
		}

		double parseAwkNumber(const std::string& number_text) {
			double value = 0.0;
			if (parseDouble(number_text, value)) return value;
			if (hasNegativeExponent(number_text)) return 0.0;
			return number_text[0] == '-' ? -HUGE_VAL : HUGE_VAL;
		}

		double stringToNumber(const std::string& text) {
			const std::size_t start = skipSpaces(text, 0);
			const std::size_t length = scanAwkNumber(text, start);
			if (length == 0) return 0.0;
			return parseAwkNumber(text.substr(start, length));
		}

		bool looksNumeric(const std::string& text, double& value) {
			const std::size_t start = skipSpaces(text, 0);
			const std::size_t length = scanAwkNumber(text, start);
			if (length == 0) return false;
			if (skipSpaces(text, start + length) != text.size()) return false;

			value = parseAwkNumber(text.substr(start, length));
			return true;
		}

		static std::string specialNumberText(double number) {
			if (std::isnan(number)) return std::signbit(number) ? "-nan" : "+nan";
			return number < 0 ? "-inf" : "+inf";
		}

		static bool isExactInteger(double number) {
			return number == std::floor(number) && std::fabs(number) < kExactIntegerLimit;
		}

		std::string numberToString(double number, const std::string& format) {
			if (!std::isfinite(number)) return specialNumberText(number);

			if (isExactInteger(number)) return std::to_string(static_cast<long long>(number));

			char buffer[400];
			if (number == std::floor(number)) {
				std::snprintf(buffer, sizeof(buffer), "%.0f", number);
				return buffer;
			}

			if (format == kDefaultNumberFormat) {
				std::snprintf(buffer, sizeof(buffer), kDefaultNumberFormat, number);
				return buffer;
			}

			const std::vector<AwkValue> args = { AwkValue::str(format), AwkValue::num(number) };
			return formatPrintf(args, kDefaultNumberFormat);
		}

		static bool isOctalDigit(char c) {
			return c >= '0' && c <= '7';
		}

		static int hexDigitValue(char c) {
			if (isDigit(c)) return c - '0';

			const int lower = std::tolower(static_cast<unsigned char>(c));
			if (lower >= 'a' && lower <= 'f') return lower - 'a' + 10;
			return -1;
		}

		static bool simpleEscape(char escaped, char& out) {
			switch (escaped) {
				case 'n':  out = '\n'; return true;
				case 't':  out = '\t'; return true;
				case 'r':  out = '\r'; return true;
				case 'a':  out = '\a'; return true;
				case 'b':  out = '\b'; return true;
				case 'f':  out = '\f'; return true;
				case 'v':  out = '\v'; return true;
				case '\\': out = '\\'; return true;
				case '"':  out = '"';  return true;
				case '/':  out = '/';  return true;
				default:   return false;
			}
		}

		static std::size_t appendOctalEscape(const std::string& text, std::size_t i,
				std::string& out) {
			int value = 0;
			std::size_t end = i;
			while (end < text.size() && end < i + 3 && isOctalDigit(text[end])) {
				value = value * 8 + (text[end] - '0');
				++end;
			}

			out.push_back(static_cast<char>(value));
			return end;
		}

		static std::size_t appendHexEscape(const std::string& text, std::size_t i,
				std::string& out) {
			int value = 0;
			std::size_t end = i;
			while (end < text.size() && end < i + 2 && hexDigitValue(text[end]) >= 0) {
				value = value * 16 + hexDigitValue(text[end]);
				++end;
			}

			if (end == i) {
				out += "\\x";
				return i;
			}

			out.push_back(static_cast<char>(value));
			return end;
		}

		// Appends the escape starting after the backslash at text[i - 1] and
		// returns the index just past it.
		static std::size_t appendEscape(const std::string& text, std::size_t i, std::string& out) {
			const char escaped = text[i];
			char simple = 0;
			if (simpleEscape(escaped, simple)) {
				out.push_back(simple);
				return i + 1;
			}

			if (isOctalDigit(escaped)) return appendOctalEscape(text, i, out);
			if (escaped == 'x') return appendHexEscape(text, i + 1, out);

			out.push_back('\\');
			out.push_back(escaped);
			return i + 1;
		}

		std::string expandEscapes(const std::string& text) {
			std::string out;
			out.reserve(text.size());

			std::size_t i = 0;
			while (i < text.size()) {
				if (text[i] == '\\' && i + 1 < text.size()) {
					i = appendEscape(text, i + 1, out);
					continue;
				}

				out.push_back(text[i]);
				++i;
			}

			return out;
		}

		template <typename T>
		static std::string formatOne(const std::string& spec, T value) {
			const int size = std::snprintf(nullptr, 0, spec.c_str(), value);
			if (size <= 0) return std::string();

			std::string out(static_cast<std::size_t>(size) + 1, '\0');
			std::snprintf(&out[0], out.size(), spec.c_str(), value);
			out.resize(static_cast<std::size_t>(size));
			return out;
		}

		static long long clampToLongLong(double value) {
			if (std::isnan(value)) return 0;
			if (value >= 9.2233720368547758e18) return LLONG_MAX;
			if (value <= -9.2233720368547758e18) return LLONG_MIN;
			return static_cast<long long>(value);
		}

		static std::string withLongLongModifier(const std::string& spec, char conversion) {
			return spec.substr(0, spec.size() - 1) + "ll" + conversion;
		}

		static std::string formatCharDirective(const std::string& spec, const AwkValue& value,
				const std::string& convfmt) {
			const std::string text = value.is_number ? std::string() : value.asString(convfmt);
			if (!text.empty()) {
				const auto first = static_cast<unsigned char>(text[0]);
				return formatOne(spec, static_cast<int>(first));
			}

			return formatOne(spec, static_cast<int>(clampToLongLong(value.asNumber()) & 0xFF));
		}

		static std::string formatIntegerDirective(const std::string& spec, char conversion,
				const AwkValue& value) {
			const std::string modified = withLongLongModifier(spec, conversion);
			const long long number = clampToLongLong(value.asNumber());
			if (conversion == 'd' || conversion == 'i') return formatOne(modified, number);
			return formatOne(modified, static_cast<unsigned long long>(number));
		}

		static bool isFloatConversion(char conversion) {
			return std::strchr("fFeEgGaA", conversion) != nullptr;
		}

		static std::string formatPrintfDirective(const std::string& spec, char conversion,
				const AwkValue& value, const std::string& convfmt) {
			if (conversion == 's') return formatOne(spec, value.asString(convfmt).c_str());
			if (conversion == 'c') return formatCharDirective(spec, value, convfmt);
			if (std::strchr("diouxX", conversion) != nullptr) {
				return formatIntegerDirective(spec, conversion, value);
			}

			if (isFloatConversion(conversion)) return formatOne(spec, value.asNumber());
			return spec;
		}

		struct PrintfCursor {
			const std::vector<AwkValue>& args;
			const std::string& convfmt;
			std::size_t next_arg = 1;

			AwkValue take() {
				if (next_arg >= args.size()) return AwkValue::str("");
				return args[next_arg++];
			}
		};

		// Copies a width or precision into the spec, reading a '*' from the arguments.
		static void appendDirectiveNumber(const std::string& fmt, std::size_t& i, std::string& spec,
				PrintfCursor& cursor) {
			if (i < fmt.size() && fmt[i] == '*') {
				spec += std::to_string(clampToLongLong(cursor.take().asNumber()));
				++i;
				return;
			}

			while (i < fmt.size() && isDigit(fmt[i])) spec.push_back(fmt[i++]);
		}

		// Reads the directive at fmt[i] == '%' up to its conversion character;
		// false when the format ends first.
		static bool readDirectiveSpec(const std::string& fmt, std::size_t& i, std::string& spec,
				PrintfCursor& cursor) {
			spec = "%";
			++i;
			while (i < fmt.size() && std::strchr("-+0 #", fmt[i]) != nullptr) {
				spec.push_back(fmt[i++]);
			}

			appendDirectiveNumber(fmt, i, spec, cursor);
			if (i < fmt.size() && fmt[i] == '.') {
				spec.push_back(fmt[i++]);
				appendDirectiveNumber(fmt, i, spec, cursor);
			}

			while (i < fmt.size() && std::strchr("hlLqjzt", fmt[i]) != nullptr) ++i;
			return i < fmt.size();
		}

		static void appendDirective(const std::string& fmt, std::size_t& i, std::string& out,
				PrintfCursor& cursor) {
			const std::size_t start = i;
			std::string spec;
			if (!readDirectiveSpec(fmt, i, spec, cursor)) {
				out.append(fmt, start, std::string::npos);
				return;
			}

			const char conversion = fmt[i++];
			if (conversion == '%') {
				out.push_back('%');
				return;
			}

			spec.push_back(conversion);
			out += formatPrintfDirective(spec, conversion, cursor.take(), cursor.convfmt);
		}

		std::string formatPrintf(const std::vector<AwkValue>& args, const std::string& convfmt) {
			if (args.empty()) return std::string();

			const std::string fmt = args[0].asString(convfmt);
			PrintfCursor cursor{ args, convfmt };
			std::string out;
			std::size_t i = 0;
			while (i < fmt.size()) {
				if (fmt[i] != '%') {
					out.push_back(fmt[i++]);
					continue;
				}

				appendDirective(fmt, i, out, cursor);
			}

			return out;
		}

	}  // namespace awk_detail

}  // namespace wbsh

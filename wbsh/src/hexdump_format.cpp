/**
 * @file hexdump_format.cpp
 * @brief Parsing and rendering of hexdump `-e` / `-f` format strings, with
 *        util-linux's rules for byte counts, padding of the final block and
 *        the dropped trailing blank of a repeated unit's last round.
 */

#include "hexdump_format.h"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "numparse.h"

namespace wbsh {

	namespace hexdump_detail {

		struct UnitParse {
			const std::string& body;
			std::size_t at;
			HexdumpUnit& unit;
			std::string& error;
		};

		struct ConversionSpec {
			std::string flags;
			std::string width;
			std::string precision;
			bool has_precision = false;
		};

	}  // namespace hexdump_detail

	using hexdump_detail::ConversionSpec;
	using hexdump_detail::UnitParse;

	static const std::size_t kMaxSpecDigits = 6;

	static const char* const kControlNames[] = {
		"nul", "soh", "stx", "etx", "eot", "enq", "ack", "bel",
		"bs",  "ht",  "lf",  "vt",  "ff",  "cr",  "so",  "si",
		"dle", "dc1", "dc2", "dc3", "dc4", "nak", "syn", "etb",
		"can", "em",  "sub", "esc", "fs",  "gs",  "rs",  "us",
	};

	static bool isBlank(char c) {
		return std::isspace(static_cast<unsigned char>(c)) != 0;
	}

	static bool isDigit(char c) {
		return c >= '0' && c <= '9';
	}

	static bool isFlag(char c) {
		return c == '#' || c == '-' || c == '+' || c == ' ' || c == '0';
	}

	static bool isPrintableByte(unsigned char byte) {
		return byte >= 0x20 && byte < 0x7F;
	}

	static std::size_t digitsValue(const std::string& digits) {
		unsigned long long value = 0;
		parseULL(digits, value);
		return static_cast<std::size_t>(value);
	}

	static void skipBlanks(const std::string& text, std::size_t& at) {
		while (at < text.size() && isBlank(text[at])) ++at;
	}

	static bool readCount(const std::string& text, std::size_t& at, std::size_t& count) {
		if (at >= text.size() || !isDigit(text[at])) return false;

		count = 0;
		while (at < text.size() && isDigit(text[at])) {
			count = count * 10 + static_cast<std::size_t>(text[at++] - '0');
		}

		return true;
	}

	static char escapedChar(char letter) {
		switch (letter) {
		case 'a': return '\a';
		case 'b': return '\b';
		case 'f': return '\f';
		case 'n': return '\n';
		case 'r': return '\r';
		case 't': return '\t';
		case 'v': return '\v';
		default:  return letter;
		}
	}

	static std::string unescape(const std::string& raw) {
		std::string text;
		for (std::size_t k = 0; k < raw.size(); ++k) {
			if (raw[k] != '\\' || k + 1 == raw.size()) {
				text.push_back(raw[k]);
				continue;
			}

			text.push_back(escapedChar(raw[++k]));
		}

		return text;
	}

	static bool parseUnitCounts(const std::string& text, std::size_t& at, HexdumpUnit& unit) {
		if (readCount(text, at, unit.repeat)) {
			unit.repeat_given = true;
			if (at < text.size() && text[at] != '/' && !isBlank(text[at])) return false;
			skipBlanks(text, at);
		}

		if (at < text.size() && text[at] == '/') {
			++at;
			skipBlanks(text, at);
		}

		if (readCount(text, at, unit.byte_count)) {
			unit.byte_count_given = unit.byte_count != 0;
			skipBlanks(text, at);
		}

		return true;
	}

	static bool readQuoted(const std::string& text, std::size_t& at, std::string& body) {
		if (at >= text.size() || text[at] != '"') return false;

		const std::size_t close = text.find('"', at + 1);
		if (close == std::string::npos) return false;

		body = unescape(text.substr(at + 1, close - at - 1));
		at = close + 1;
		return true;
	}

	static bool followedBy(const UnitParse& parse, char next) {
		return parse.at + 1 < parse.body.size() && parse.body[parse.at + 1] == next;
	}

	static std::string literalText(UnitParse& parse) {
		std::string text;
		while (parse.at < parse.body.size()) {
			const char c = parse.body[parse.at];
			const bool doubled = c == '%' && followedBy(parse, '%');
			if (c == '%' && !doubled) break;

			text.push_back(c);
			parse.at += doubled ? 2 : 1;
		}

		return text;
	}

	static void readDigits(UnitParse& parse, std::string& digits) {
		while (parse.at < parse.body.size() && isDigit(parse.body[parse.at])) {
			digits.push_back(parse.body[parse.at++]);
		}
	}

	static ConversionSpec readSpec(UnitParse& parse) {
		ConversionSpec spec;
		while (parse.at < parse.body.size() && isFlag(parse.body[parse.at])) {
			spec.flags.push_back(parse.body[parse.at++]);
		}

		readDigits(parse, spec.width);
		if (parse.at < parse.body.size() && parse.body[parse.at] == '.') {
			spec.has_precision = true;
			++parse.at;
			readDigits(parse, spec.precision);
		}

		return spec;
	}

	static bool fail(UnitParse& parse, const std::string& message) {
		parse.error = message;
		return false;
	}

	static bool badCount(UnitParse& parse, const std::string& conversion) {
		return fail(parse, "bad byte count for conversion character " + conversion);
	}

	static bool singleByte(UnitParse& parse, HexdumpPrint& print, const std::string& conversion) {
		const bool wider = parse.unit.byte_count_given && parse.unit.byte_count != 1;
		if (wider) return badCount(parse, conversion);

		print.byte_count = 1;
		return true;
	}

	static bool integerBytes(UnitParse& parse, HexdumpPrint& print, char letter) {
		const std::size_t given = parse.unit.byte_count_given ? parse.unit.byte_count : 4;
		const bool supported = given == 1 || given == 2 || given == 4 || given == 8;
		if (!supported) return badCount(parse, std::string(1, letter));

		print.byte_count = given;
		return true;
	}

	static bool floatBytes(UnitParse& parse, HexdumpPrint& print, char letter) {
		const std::size_t given = parse.unit.byte_count_given ? parse.unit.byte_count : 8;
		if (given != 4 && given != 8) return badCount(parse, std::string(1, letter));

		print.byte_count = given;
		return true;
	}

	static bool stringBytes(UnitParse& parse, HexdumpPrint& print, const ConversionSpec& spec) {
		if (parse.unit.byte_count_given) {
			print.byte_count = parse.unit.byte_count;
			return true;
		}

		if (!spec.has_precision || spec.precision.empty()) {
			return fail(parse, "%s requires a precision or a byte count");
		}

		print.byte_count = digitsValue(spec.precision);
		return true;
	}

	static bool parsePlainConversion(UnitParse& parse, HexdumpPrint& print,
	                                 const ConversionSpec& spec, char letter) {
		print.letter = letter;
		switch (letter) {
		case 'c':
			print.conversion = HexdumpConversion::Character;
			return singleByte(parse, print, "c");
		case 'd':
		case 'i':
			print.conversion = HexdumpConversion::Signed;
			return integerBytes(parse, print, letter);
		case 'o':
		case 'u':
		case 'x':
		case 'X':
			print.conversion = HexdumpConversion::Unsigned;
			return integerBytes(parse, print, letter);
		case 'e':
		case 'E':
		case 'f':
		case 'g':
		case 'G':
			print.conversion = HexdumpConversion::Float;
			return floatBytes(parse, print, letter);
		case 's':
			print.conversion = HexdumpConversion::String;
			return stringBytes(parse, print, spec);
		default:
			return fail(parse, std::string("bad conversion character %") + letter);
		}
	}

	static char nextLetter(UnitParse& parse) {
		return parse.at < parse.body.size() ? parse.body[parse.at++] : '\0';
	}

	static bool parseAddress(UnitParse& parse, HexdumpPrint& print, char kind) {
		const char radix = nextLetter(parse);
		if (radix != 'd' && radix != 'o' && radix != 'x') {
			return fail(parse, std::string("bad conversion character %_") + kind + radix);
		}

		print.conversion = HexdumpConversion::Address;
		print.letter = radix;
		if (kind == 'A') parse.unit.end_only = true;
		return true;
	}

	static bool parseUnderscoreConversion(UnitParse& parse, HexdumpPrint& print) {
		const char kind = nextLetter(parse);
		const std::string shown = std::string("_") + kind;
		switch (kind) {
		case 'a':
		case 'A':
			return parseAddress(parse, print, kind);
		case 'c':
			print.conversion = HexdumpConversion::Escaped;
			return singleByte(parse, print, shown);
		case 'p':
			print.conversion = HexdumpConversion::Printable;
			return singleByte(parse, print, shown);
		case 'u':
			print.conversion = HexdumpConversion::Named;
			return singleByte(parse, print, shown);
		default:
			return fail(parse, "bad conversion character %" + shown);
		}
	}

	static bool parseConversion(UnitParse& parse, HexdumpPrint& print) {
		++parse.at;
		const ConversionSpec spec = readSpec(parse);
		if (spec.width.size() > kMaxSpecDigits || spec.precision.size() > kMaxSpecDigits) {
			return fail(parse, "bad format {" + parse.body + "}");
		}

		const std::string precision = spec.has_precision ? "." + spec.precision : std::string();
		print.spec = "%" + spec.flags + spec.width + precision;
		print.width = digitsValue(spec.width);
		const char letter = nextLetter(parse);
		if (letter == '_') return parseUnderscoreConversion(parse, print);
		return parsePlainConversion(parse, print, spec, letter);
	}

	static bool parseUnitBody(UnitParse& parse) {
		std::size_t data_conversions = 0;
		while (parse.at < parse.body.size()) {
			HexdumpPrint print;
			print.text = literalText(parse);
			if (parse.at < parse.body.size() && !parseConversion(parse, print)) return false;

			const bool counted = print.conversion != HexdumpConversion::Text
				&& print.conversion != HexdumpConversion::Address;
			if (counted && parse.unit.byte_count_given && ++data_conversions > 1) {
				return fail(parse, "byte count with multiple conversion characters");
			}

			parse.unit.prints.push_back(print);
		}

		if (parse.unit.byte_count_given) return true;
		for (const HexdumpPrint& print : parse.unit.prints) {
			parse.unit.byte_count += print.byte_count;
		}

		return true;
	}

	static bool parseUnit(const std::string& text, std::size_t& at, HexdumpUnit& unit,
	                      std::string& error) {
		std::string body;
		if (!parseUnitCounts(text, at, unit) || !readQuoted(text, at, body)) {
			error = "bad format {" + text + "}";
			return false;
		}

		UnitParse parse{ body, 0, unit, error };
		return parseUnitBody(parse);
	}

	bool addHexdumpFormat(HexdumpProgram& program, const std::string& text, std::string& error) {
		HexdumpFormat format;
		std::size_t at = 0;
		for (skipBlanks(text, at); at < text.size(); skipBlanks(text, at)) {
			HexdumpUnit unit;
			if (!parseUnit(text, at, unit, error)) return false;
			format.units.push_back(unit);
		}

		program.formats.push_back(format);
		return true;
	}

	static std::size_t formatByteCount(const HexdumpFormat& format) {
		std::size_t total = 0;
		for (const HexdumpUnit& unit : format.units) total += unit.repeat * unit.byte_count;
		return total;
	}

	static void stretchLastUnit(HexdumpFormat& format, std::size_t block_size) {
		if (format.units.empty()) return;

		HexdumpUnit& last = format.units.back();
		if (format.byte_count >= block_size || last.repeat_given || last.byte_count == 0) return;
		last.repeat += (block_size - format.byte_count) / last.byte_count;
	}

	static void markTrimmedRound(HexdumpUnit& unit) {
		if (unit.repeat <= 1 || unit.prints.empty()) return;

		const HexdumpPrint& last = unit.prints.back();
		unit.trims_last_round = last.conversion == HexdumpConversion::Text
			&& !last.text.empty() && isBlank(last.text.back());
	}

	void finishHexdumpProgram(HexdumpProgram& program) {
		program.block_size = 0;
		for (HexdumpFormat& format : program.formats) {
			format.byte_count = formatByteCount(format);
			if (format.byte_count > program.block_size) program.block_size = format.byte_count;
		}

		for (HexdumpFormat& format : program.formats) {
			stretchLastUnit(format, program.block_size);
			for (HexdumpUnit& unit : format.units) markTrimmedRound(unit);
		}
	}

	static std::string formatted(const char* format, ...) {
		va_list args;
		va_start(args, format);
		va_list again;
		va_copy(again, args);
		const int length = std::vsnprintf(nullptr, 0, format, args);
		va_end(args);

		std::string text(length > 0 ? static_cast<std::size_t>(length) : 0, '\0');
		if (length > 0) std::vsnprintf(&text[0], text.size() + 1, format, again);
		va_end(again);
		return text;
	}

	static unsigned long long loadValue(const unsigned char* bytes, std::size_t size) {
		unsigned long long value = 0;
		for (std::size_t k = size; k > 0; --k) value = (value << 8) | bytes[k - 1];
		return value;
	}

	static long long loadSigned(const unsigned char* bytes, std::size_t size) {
		const unsigned long long value = loadValue(bytes, size);
		if (size >= 8) return static_cast<long long>(value);

		const unsigned long long sign = 1ULL << (size * 8 - 1);
		if ((value & sign) == 0) return static_cast<long long>(value);
		return static_cast<long long>(value | ~((sign << 1) - 1));
	}

	static double loadFloat(const unsigned char* bytes, std::size_t size) {
		if (size == 4) {
			float value = 0.0f;
			std::memcpy(&value, bytes, sizeof(value));
			return value;
		}

		double value = 0.0;
		std::memcpy(&value, bytes, sizeof(value));
		return value;
	}

	static std::string withSuffix(const HexdumpPrint& print, const char* suffix) {
		return print.spec + suffix;
	}

	static std::string escapedCell(const HexdumpPrint& print, unsigned char byte) {
		static const unsigned char kEscaped[] = { '\0', '\a', '\b', '\f', '\n', '\r', '\t', '\v' };
		static const char* const kNames[] = {
			"\\0", "\\a", "\\b", "\\f", "\\n", "\\r", "\\t", "\\v",
		};
		for (std::size_t k = 0; k < sizeof(kEscaped); ++k) {
			if (byte == kEscaped[k]) {
				return formatted(withSuffix(print, "s").c_str(), kNames[k]);
			}
		}

		if (isPrintableByte(byte)) return formatted(withSuffix(print, "c").c_str(), byte);
		return formatted(withSuffix(print, "s").c_str(), formatted("%03o", byte).c_str());
	}

	static std::string namedCell(const HexdumpPrint& print, unsigned char byte) {
		if (byte < 0x20) return formatted(withSuffix(print, "s").c_str(), kControlNames[byte]);
		if (byte == 0x7F) return formatted(withSuffix(print, "s").c_str(), "del");
		if (isPrintableByte(byte)) return formatted(withSuffix(print, "c").c_str(), byte);
		return formatted(withSuffix(print, "x").c_str(), byte);
	}

	static std::string printableCell(const HexdumpPrint& print, unsigned char byte) {
		return formatted(withSuffix(print, "c").c_str(), isPrintableByte(byte) ? byte : '.');
	}

	static std::string stringCell(const HexdumpPrint& print, const unsigned char* bytes) {
		const char* start = reinterpret_cast<const char*>(bytes);
		const std::string text(start, ::strnlen(start, print.byte_count));
		return formatted(withSuffix(print, "s").c_str(), text.c_str());
	}

	static std::string integerCell(const HexdumpPrint& print, const unsigned char* bytes) {
		if (print.conversion == HexdumpConversion::Signed) {
			return formatted(withSuffix(print, "lld").c_str(), loadSigned(bytes, print.byte_count));
		}

		const std::string format = print.spec + "ll" + print.letter;
		return formatted(format.c_str(), loadValue(bytes, print.byte_count));
	}

	static std::string floatCell(const HexdumpPrint& print, const unsigned char* bytes) {
		const std::string format = print.spec + print.letter;
		return formatted(format.c_str(), loadFloat(bytes, print.byte_count));
	}

	static std::string addressCell(const HexdumpPrint& print, std::uint64_t address) {
		const std::string format = print.spec + "ll" + print.letter;
		if (print.letter == 'd') return formatted(format.c_str(), static_cast<long long>(address));
		return formatted(format.c_str(), static_cast<unsigned long long>(address));
	}

	static std::string conversionCell(const HexdumpPrint& print, const unsigned char* bytes,
	                                  std::uint64_t address) {
		const unsigned char byte = bytes[0];
		switch (print.conversion) {
		case HexdumpConversion::Address:   return addressCell(print, address);
		case HexdumpConversion::Character: return formatted(withSuffix(print, "c").c_str(), byte);
		case HexdumpConversion::Escaped:   return escapedCell(print, byte);
		case HexdumpConversion::Named:     return namedCell(print, byte);
		case HexdumpConversion::String:    return stringCell(print, bytes);
		case HexdumpConversion::Printable: return printableCell(print, byte);
		case HexdumpConversion::Float:     return floatCell(print, bytes);
		case HexdumpConversion::Signed:
		case HexdumpConversion::Unsigned:  return integerCell(print, bytes);
		default:                           return std::string();
		}
	}

	static void renderPrint(const HexdumpPrint& print, const HexdumpBlock& block,
	                        std::size_t offset, bool trim, std::string& out) {
		if (print.conversion == HexdumpConversion::Text) {
			out.append(print.text, 0, print.text.size() - (trim ? 1 : 0));
			return;
		}

		out += print.text;
		const std::uint64_t address = block.address + offset;
		if (block.final && address >= block.data_end) {
			out.append(print.width, ' ');
			return;
		}

		out += conversionCell(print, block.data + offset, address);
	}

	static void renderUnitRound(const HexdumpUnit& unit, bool last_round, const HexdumpBlock& block,
	                            std::size_t& offset, std::string& out) {
		for (std::size_t k = 0; k < unit.prints.size(); ++k) {
			const bool trim = last_round && unit.trims_last_round && k + 1 == unit.prints.size();
			renderPrint(unit.prints[k], block, offset, trim, out);
			offset += unit.prints[k].byte_count;
		}
	}

	static void renderFormat(const HexdumpFormat& format, const HexdumpBlock& block,
	                         std::string& out) {
		std::size_t offset = 0;
		for (const HexdumpUnit& unit : format.units) {
			if (unit.end_only) return;
			for (std::size_t round = unit.repeat; round > 0; --round) {
				renderUnitRound(unit, round == 1, block, offset, out);
			}
		}
	}

	void renderHexdumpBlock(const HexdumpProgram& program, const HexdumpBlock& block,
	                        std::string& out) {
		for (const HexdumpFormat& format : program.formats) renderFormat(format, block, out);
	}

	static const HexdumpUnit* endUnit(const HexdumpProgram& program) {
		const HexdumpUnit* found = nullptr;
		for (const HexdumpFormat& format : program.formats) {
			for (const HexdumpUnit& unit : format.units) {
				if (unit.end_only) found = &unit;
			}
		}

		return found;
	}

	void renderHexdumpEnd(const HexdumpProgram& program, std::uint64_t end_address,
	                      std::string& out) {
		const HexdumpUnit* unit = endUnit(program);
		if (unit == nullptr) return;

		for (const HexdumpPrint& print : unit->prints) {
			if (print.conversion == HexdumpConversion::Text) out += print.text;
			if (print.conversion != HexdumpConversion::Address) continue;
			out += print.text;
			out += addressCell(print, end_address);
		}
	}

}  // namespace wbsh

/**
 * @file utf8.cpp
 * @brief UTF-8 decoding, case mapping and POSIX character classes.
 */

#include "utf8.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <cwctype>
#endif /* _WIN32 */

#include <cctype>
#include <cstring>

namespace wbsh {

	namespace utf8_detail {

		static const char32_t kStrayByteBase = 0xDC00;
		static const char32_t kLastBmp = 0xFFFF;

		enum ClassBits : unsigned {
			kUpper  = 1u << 0,
			kLower  = 1u << 1,
			kDigit  = 1u << 2,
			kSpace  = 1u << 3,
			kPunct  = 1u << 4,
			kCntrl  = 1u << 5,
			kBlank  = 1u << 6,
			kXdigit = 1u << 7,
			kAlpha  = 1u << 8,
		};

		static std::size_t sequenceLength(unsigned char lead) {
			if (lead < 0x80) return 1;
			if (lead >= 0xC2 && lead <= 0xDF) return 2;
			if (lead >= 0xE0 && lead <= 0xEF) return 3;
			if (lead >= 0xF0 && lead <= 0xF4) return 4;
			return 0;
		}

		static bool isContinuation(unsigned char byte) {
			return (byte & 0xC0) == 0x80;
		}

		static Utf8Char strayByte(unsigned char byte) {
			return { kStrayByteBase + byte, 1 };
		}

		static bool isStrayCode(char32_t code) {
			return code >= kStrayByteBase + 0x80 && code <= kStrayByteBase + 0xFF;
		}

		static unsigned asciiClassBits(char32_t code) {
			const int c = static_cast<int>(code);
			unsigned bits = 0;
			if (std::isupper(c))  bits |= kUpper;
			if (std::islower(c))  bits |= kLower;
			if (std::isalpha(c))  bits |= kAlpha;
			if (std::isdigit(c))  bits |= kDigit;
			if (std::isspace(c))  bits |= kSpace;
			if (std::ispunct(c))  bits |= kPunct;
			if (std::iscntrl(c))  bits |= kCntrl;
			if (std::isxdigit(c)) bits |= kXdigit;
			if (c == ' ' || c == '\t') bits |= kBlank;
			return bits;
		}

#ifdef _WIN32
		static unsigned wideClassBits(char32_t code) {
			const wchar_t wide = static_cast<wchar_t>(code);
			WORD type = 0;
			if (!GetStringTypeW(CT_CTYPE1, &wide, 1, &type)) return 0;

			unsigned bits = 0;
			if (type & C1_UPPER) bits |= kUpper;
			if (type & C1_LOWER) bits |= kLower;
			if (type & C1_ALPHA) bits |= kAlpha;
			if (type & C1_SPACE) bits |= kSpace;
			if (type & C1_PUNCT) bits |= kPunct;
			if (type & C1_CNTRL) bits |= kCntrl;
			if (type & C1_BLANK) bits |= kBlank;
			return bits;
		}

		static char32_t mapWide(char32_t code, bool upper) {
			wchar_t wide = static_cast<wchar_t>(code);
			if (upper) CharUpperBuffW(&wide, 1);
			else CharLowerBuffW(&wide, 1);
			return wide;
		}
#else
		static unsigned wideClassBits(char32_t code) {
			const wint_t wide = static_cast<wint_t>(code);
			unsigned bits = 0;
			if (std::iswupper(wide)) bits |= kUpper;
			if (std::iswlower(wide)) bits |= kLower;
			if (std::iswalpha(wide)) bits |= kAlpha;
			if (std::iswspace(wide)) bits |= kSpace;
			if (std::iswpunct(wide)) bits |= kPunct;
			if (std::iswcntrl(wide)) bits |= kCntrl;
			if (std::iswblank(wide)) bits |= kBlank;
			return bits;
		}

		static char32_t mapWide(char32_t code, bool upper) {
			const wint_t wide = static_cast<wint_t>(code);
			return static_cast<char32_t>(upper ? std::towupper(wide) : std::towlower(wide));
		}
#endif /* _WIN32 */

		// Characters outside the BMP count as printable non-letters.
		static unsigned classBits(char32_t code) {
			if (code < 0x80) return asciiClassBits(code);
			if (isStrayCode(code)) return 0;
			if (code > kLastBmp) return kPunct;
			return wideClassBits(code);
		}

		static char32_t mapCase(char32_t code, bool upper) {
			if (code < 0x80) {
				const int c = static_cast<int>(code);
				return static_cast<char32_t>(upper ? std::toupper(c) : std::tolower(c));
			}

			if (isStrayCode(code) || code > kLastBmp) return code;
			if (code >= 0xD800 && code <= 0xDFFF) return code;
			return mapWide(code, upper);
		}

		struct NamedClass {
			const char* name;
			unsigned any_of;
		};

		static const NamedClass kNamedClasses[] = {
			{ "alpha",  kAlpha },
			{ "digit",  kDigit },
			{ "alnum",  kAlpha | kDigit },
			{ "upper",  kUpper },
			{ "lower",  kLower },
			{ "space",  kSpace },
			{ "blank",  kBlank },
			{ "punct",  kPunct },
			{ "cntrl",  kCntrl },
			{ "xdigit", kXdigit },
			{ "graph",  kAlpha | kDigit | kPunct },
			{ "word",   kAlpha | kDigit },
		};

		static const NamedClass* findClass(const std::string& name) {
			for (const NamedClass& entry : kNamedClasses) {
				if (name == entry.name) return &entry;
			}

			return nullptr;
		}

		static bool isPrintable(char32_t code) {
			if (isStrayCode(code)) return false;
			return (classBits(code) & kCntrl) == 0;
		}

	}  // namespace utf8_detail

	Utf8Char decodeUtf8(const std::string& text, std::size_t pos, std::size_t end) {
		using namespace utf8_detail;
		const auto lead = static_cast<unsigned char>(text[pos]);
		const std::size_t length = sequenceLength(lead);
		if (length == 1) return { lead, 1 };
		if (length == 0 || pos + length > end) return strayByte(lead);

		char32_t code = lead & (0x7F >> length);
		for (std::size_t k = 1; k < length; ++k) {
			const auto byte = static_cast<unsigned char>(text[pos + k]);
			if (!isContinuation(byte)) return strayByte(lead);

			code = (code << 6) | (byte & 0x3F);
		}

		return { code, length };
	}

	std::size_t utf8Count(const std::string& text) {
		std::size_t count = 0;
		for (std::size_t pos = 0; pos < text.size(); pos += utf8CharLength(text, pos)) ++count;
		return count;
	}

	std::size_t utf8Advance(const std::string& text, std::size_t pos, std::size_t count) {
		while (count > 0 && pos < text.size()) {
			pos += utf8CharLength(text, pos);
			--count;
		}

		return pos < text.size() ? pos : text.size();
	}

	void appendUtf8(std::string& out, char32_t code) {
		if (utf8_detail::isStrayCode(code)) {
			out.push_back(static_cast<char>(code - utf8_detail::kStrayByteBase));
		} else if (code < 0x80) {
			out.push_back(static_cast<char>(code));
		} else if (code < 0x800) {
			out.push_back(static_cast<char>(0xC0 | (code >> 6)));
			out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
		} else if (code < 0x10000) {
			out.push_back(static_cast<char>(0xE0 | (code >> 12)));
			out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
		} else {
			out.push_back(static_cast<char>(0xF0 | (code >> 18)));
			out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
		}
	}

	char32_t upperCode(char32_t code) {
		return utf8_detail::mapCase(code, true);
	}

	char32_t lowerCode(char32_t code) {
		return utf8_detail::mapCase(code, false);
	}

	bool isInCharClass(const std::string& name, char32_t code) {
		if (name == "print") return utf8_detail::isPrintable(code);
		if (name == "word" && code == '_') return true;

		const utf8_detail::NamedClass* entry = utf8_detail::findClass(name);
		if (entry == nullptr) return false;
		return (utf8_detail::classBits(code) & entry->any_of) != 0;
	}

	bool isKnownCharClass(const std::string& name) {
		return name == "print" || utf8_detail::findClass(name) != nullptr;
	}

}  // namespace wbsh

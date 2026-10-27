/**
 * @file utf8.cpp
 * @brief UTF-8 encoding and the UTF-16 conversions the window needs.
 */

#include "utf8.h"

#include <windows.h>

namespace wbshterm {

	static bool isEncodable(char32_t code) {
		return code <= 0x10FFFF && (code < 0xD800 || code > 0xDFFF);
	}

	static char continuationByte(char32_t code, int shift) {
		return static_cast<char>(0x80 | ((code >> shift) & 0x3F));
	}

	void appendUtf8(std::string& out, char32_t code) {
		if (!isEncodable(code)) code = 0xFFFD;

		if (code < 0x80) {
			out.push_back(static_cast<char>(code));
		} else if (code < 0x800) {
			out.push_back(static_cast<char>(0xC0 | (code >> 6)));
			out.push_back(continuationByte(code, 0));
		} else if (code < 0x10000) {
			out.push_back(static_cast<char>(0xE0 | (code >> 12)));
			out.push_back(continuationByte(code, 6));
			out.push_back(continuationByte(code, 0));
		} else {
			out.push_back(static_cast<char>(0xF0 | (code >> 18)));
			out.push_back(continuationByte(code, 12));
			out.push_back(continuationByte(code, 6));
			out.push_back(continuationByte(code, 0));
		}
	}

	static bool isContinuationByte(char byte) {
		return (static_cast<unsigned char>(byte) & 0xC0) == 0x80;
	}

	void popLastUtf8(std::string& text) {
		while (!text.empty() && isContinuationByte(text.back())) text.pop_back();
		if (!text.empty()) text.pop_back();
	}

	std::wstring widenUtf8(const std::string& text) {
		if (text.empty()) return std::wstring();

		const int size = static_cast<int>(text.size());
		const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), size, nullptr, 0);
		if (needed <= 0) return std::wstring();

		std::wstring wide(static_cast<std::size_t>(needed), L'\0');
		::MultiByteToWideChar(CP_UTF8, 0, text.data(), size, wide.data(), needed);
		return wide;
	}

	std::string narrowUtf16(const std::wstring& text) {
		if (text.empty()) return std::string();

		const int size = static_cast<int>(text.size());
		const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), size, nullptr, 0,
			nullptr, nullptr);
		if (needed <= 0) return std::string();

		std::string bytes(static_cast<std::size_t>(needed), '\0');
		::WideCharToMultiByte(CP_UTF8, 0, text.data(), size, bytes.data(), needed, nullptr,
			nullptr);
		return bytes;
	}

	static bool isHighSurrogate(wchar_t unit) {
		return unit >= 0xD800 && unit <= 0xDBFF;
	}

	static bool isLowSurrogate(wchar_t unit) {
		return unit >= 0xDC00 && unit <= 0xDFFF;
	}

	static char32_t joinedSurrogates(wchar_t high, wchar_t low) {
		return 0x10000 + ((static_cast<char32_t>(high) - 0xD800) << 10)
			+ (static_cast<char32_t>(low) - 0xDC00);
	}

	bool SurrogateJoiner::take(wchar_t unit, char32_t& out_code) {
		const wchar_t high = high_;
		high_ = 0;

		if (isHighSurrogate(unit)) {
			high_ = unit;
			return false;
		}

		if (isLowSurrogate(unit)) {
			if (high == 0) return false;

			out_code = joinedSurrogates(high, unit);
			return true;
		}

		out_code = static_cast<char32_t>(unit);
		return true;
	}

} /* namespace wbshterm */

/**
 * @file utf8.cpp
 * @brief UTF-8 encoding and the UTF-16 conversion the renderer needs.
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

} /* namespace wbshterm */

#pragma once

/**
 * @file utf8.h
 * @brief UTF-8 encoding and the UTF-16 conversions the window needs.
 */

#include <string>

namespace wbshterm {

	/** Code points past U+10FFFF and lone surrogates go out as U+FFFD. */
	void appendUtf8(std::string& out, char32_t code);

	/** Drops the whole last character, not just its final byte. */
	void popLastUtf8(std::string& text);

	std::wstring widenUtf8(const std::string& text);

	/** Converts the whole string at once, so a surrogate pair becomes one character. */
	std::string narrowUtf16(const std::wstring& text);

	/**
	 * @brief Joins UTF-16 units that arrive one message at a time into code points.
	 *
	 * WM_CHAR delivers a character outside the BMP (an emoji from Win+.,
	 * an IME) as two messages, high surrogate first. A surrogate with no
	 * partner is dropped.
	 */
	class SurrogateJoiner {
	public:
		/** False while a high surrogate waits for its low half. */
		bool take(wchar_t unit, char32_t& out_code);

	private:
		wchar_t high_ = 0;
	};

} /* namespace wbshterm */

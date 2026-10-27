#pragma once

/**
 * @file utf8.h
 * @brief UTF-8 encoding and the UTF-16 conversion the renderer needs.
 */

#include <string>

namespace wbshterm {

	/** Code points past U+10FFFF and lone surrogates go out as U+FFFD. */
	void appendUtf8(std::string& out, char32_t code);

	/** Drops the whole last character, not just its final byte. */
	void popLastUtf8(std::string& text);

	std::wstring widenUtf8(const std::string& text);

} /* namespace wbshterm */

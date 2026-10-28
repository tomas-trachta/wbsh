#pragma once

/**
 * @file utf8.h
 * @brief Character-wise access to UTF-8 strings for the shell's string operators.
 *
 * `${#x}`, `${x:o:n}`, `?` and `[...]` in patterns and case conversion
 * count characters, not bytes. A byte that does not start a valid
 * sequence is a character of its own, decoded to 0xDC00 + byte so
 * distinct stray bytes stay distinct.
 */

#include <cstddef>
#include <string>

namespace wbsh {

	struct Utf8Char {
		char32_t code;
		std::size_t length;
	};

	/// The character at @p pos, not reading at or past @p end.
	Utf8Char decodeUtf8(const std::string& text, std::size_t pos, std::size_t end);

	inline Utf8Char decodeUtf8(const std::string& text, std::size_t pos) {
		return decodeUtf8(text, pos, text.size());
	}

	/// Bytes of the character at @p pos; 0 at or past @p end.
	inline std::size_t utf8CharLength(const std::string& text, std::size_t pos, std::size_t end) {
		if (pos >= end) return 0;

		return decodeUtf8(text, pos, end).length;
	}

	inline std::size_t utf8CharLength(const std::string& text, std::size_t pos) {
		return utf8CharLength(text, pos, text.size());
	}

	std::size_t utf8Count(const std::string& text);

	/// Byte offset @p count characters after @p pos, clamped to the end.
	std::size_t utf8Advance(const std::string& text, std::size_t pos, std::size_t count);

	void appendUtf8(std::string& out, char32_t code);

	char32_t upperCode(char32_t code);
	char32_t lowerCode(char32_t code);

	/// Whether @p code belongs to the POSIX class @p name (`alpha`, `digit`, ...).
	bool isInCharClass(const std::string& name, char32_t code);

	bool isKnownCharClass(const std::string& name);

}  // namespace wbsh

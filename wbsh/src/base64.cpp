/**
 * @file base64.cpp
 * @brief RFC 4648 base64 encoding and decoding.
 */

#include "base64.h"

namespace wbsh {

	static const char* const kBase64Alphabet =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	static void appendBase64Group(std::string& out, const std::vector<unsigned char>& in,
			std::size_t i) {
		const std::size_t size = in.size();
		unsigned int triple = static_cast<unsigned int>(in[i]) << 16;
		if (i + 1 < size) triple |= static_cast<unsigned int>(in[i + 1]) << 8;
		if (i + 2 < size) triple |= static_cast<unsigned int>(in[i + 2]);

		out.push_back(kBase64Alphabet[(triple >> 18) & 0x3F]);
		out.push_back(kBase64Alphabet[(triple >> 12) & 0x3F]);
		out.push_back((i + 1 < size) ? kBase64Alphabet[(triple >> 6) & 0x3F] : '=');
		out.push_back((i + 2 < size) ? kBase64Alphabet[triple & 0x3F] : '=');
	}

	std::string base64Encode(const std::vector<unsigned char>& in) {
		std::string out;
		out.reserve(((in.size() + 2) / 3) * 4);
		for (std::size_t i = 0; i < in.size(); i += 3) appendBase64Group(out, in, i);
		return out;
	}

	static void buildBase64DecodeTable(int (&table)[256]) {
		for (int i = 0; i < 256; ++i) table[i] = -1;
		for (int i = 0; i < 64; ++i) table[static_cast<unsigned char>(kBase64Alphabet[i])] = i;
	}

	static bool isBase64Whitespace(char c) {
		return c == '\n' || c == '\r' || c == ' ' || c == '\t';
	}

	std::vector<unsigned char> base64Decode(const std::string& in, bool& ok) {
		ok = true;
		int table[256];
		buildBase64DecodeTable(table);

		std::vector<unsigned char> out;
		int accumulator = 0;
		int bits = 0;
		for (char c : in) {
			if (isBase64Whitespace(c)) continue;
			if (c == '=') break;

			const int digit = table[static_cast<unsigned char>(c)];
			if (digit < 0) {
				ok = false;
				return out;
			}

			accumulator = (accumulator << 6) | digit;
			bits += 6;
			if (bits >= 8) {
				bits -= 8;
				out.push_back(static_cast<unsigned char>((accumulator >> bits) & 0xFF));
			}
		}

		return out;
	}

}  // namespace wbsh

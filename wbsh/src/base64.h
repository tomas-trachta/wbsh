#pragma once

/**
 * @file base64.h
 * @brief RFC 4648 base64, shared by the base64/uuencode tools and `wbsh --ec`.
 */

#include <string>
#include <vector>

namespace wbsh {

	std::string base64Encode(const std::vector<unsigned char>& in);

	/// Skips whitespace and stops at the first `=`, so missing padding is fine;
	/// any other character outside the alphabet clears @p ok.
	std::vector<unsigned char> base64Decode(const std::string& in, bool& ok);

}  // namespace wbsh

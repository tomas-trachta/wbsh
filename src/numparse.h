#pragma once

/**
 * @file numparse.h
 * @brief Error-as-value numeric parsing helpers.
 *
 * Thin wrappers over the C `strto*` family with the semantics of the
 * throwing `std::stoi` / `std::stoll` / `std::stoul` / `std::stod`
 * calls they replace: leading whitespace and sign are accepted and a
 * numeric prefix is parsed. They return false (instead of throwing)
 * when no conversion is possible or the value is out of range.
 *
 * Pass @p consumed to detect trailing garbage: it receives the number
 * of characters consumed, so `consumed == s.size()` means the whole
 * string was numeric (the idiom previously written as
 * `std::stoll(s, &idx); idx == s.size()`).
 */

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <string>

namespace wbsh {

	/**
	 * @brief Parse a signed long long prefix of @p s (like std::stoll).
	 *
	 * @param s        Input text.
	 * @param out      Receives the value; untouched on failure.
	 * @param base     Numeric base; 0 enables 0x/0 prefix detection.
	 * @param consumed Optional: number of characters consumed.
	 * @return true on success; false when nothing numeric was found or
	 *         the value is out of range.
	 */
	inline bool parseLL(const std::string& s, long long& out, int base = 10,
	                    std::size_t* consumed = nullptr) {
		const char* begin = s.c_str();
		char* end = nullptr;
		errno = 0;
		const long long v = std::strtoll(begin, &end, base);
		if (end == begin || errno == ERANGE) return false;
		out = v;
		if (consumed) *consumed = static_cast<std::size_t>(end - begin);
		return true;
	}

	/// Parse an int prefix of @p s (like std::stoi, int range enforced).
	inline bool parseInt(const std::string& s, int& out, int base = 10,
	                     std::size_t* consumed = nullptr) {
		long long v = 0;
		if (!parseLL(s, v, base, consumed)) return false;
		if (v < INT_MIN || v > INT_MAX) return false;
		out = static_cast<int>(v);
		return true;
	}

	/// Parse an unsigned long prefix of @p s (like std::stoul).
	inline bool parseUL(const std::string& s, unsigned long& out, int base = 10,
	                    std::size_t* consumed = nullptr) {
		const char* begin = s.c_str();
		char* end = nullptr;
		errno = 0;
		const unsigned long v = std::strtoul(begin, &end, base);
		if (end == begin || errno == ERANGE) return false;
		out = v;
		if (consumed) *consumed = static_cast<std::size_t>(end - begin);
		return true;
	}

	/// Parse a double prefix of @p s (like std::stod).
	inline bool parseDouble(const std::string& s, double& out,
	                        std::size_t* consumed = nullptr) {
		const char* begin = s.c_str();
		char* end = nullptr;
		errno = 0;
		const double v = std::strtod(begin, &end);
		if (end == begin || errno == ERANGE) return false;
		out = v;
		if (consumed) *consumed = static_cast<std::size_t>(end - begin);
		return true;
	}

}  // namespace wbsh

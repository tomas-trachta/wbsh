#pragma once

/**
 * @file numparse.h
 * @brief Error-as-value numeric parsing helpers.
 *
 * Thin wrappers over the C `strto*` family with the semantics of the
 * throwing `std::stoi` / `std::stoll` / `std::stoul` / `std::stod`
 * calls they replace: leading whitespace and sign are accepted and a
 * numeric prefix is parsed. They return false (instead of throwing)
 * when no conversion is possible or the value is out of range, and
 * leave `out` untouched in that case.
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

	inline bool parseLL(const std::string& s, long long& out, int base = 10,
	                    std::size_t* consumed = nullptr) {
		const char* begin = s.c_str();
		char* end = nullptr;
		errno = 0;
		const long long value = std::strtoll(begin, &end, base);
		if (end == begin || errno == ERANGE) return false;

		out = value;
		if (consumed != nullptr) *consumed = static_cast<std::size_t>(end - begin);
		return true;
	}

	inline bool parseInt(const std::string& s, int& out, int base = 10,
	                     std::size_t* consumed = nullptr) {
		long long value = 0;
		if (!parseLL(s, value, base, consumed)) return false;
		if (value < INT_MIN || value > INT_MAX) return false;

		out = static_cast<int>(value);
		return true;
	}

	inline bool parseUL(const std::string& s, unsigned long& out, int base = 10,
	                    std::size_t* consumed = nullptr) {
		const char* begin = s.c_str();
		char* end = nullptr;
		errno = 0;
		const unsigned long value = std::strtoul(begin, &end, base);
		if (end == begin || errno == ERANGE) return false;

		out = value;
		if (consumed != nullptr) *consumed = static_cast<std::size_t>(end - begin);
		return true;
	}

	inline bool parseDouble(const std::string& s, double& out,
	                        std::size_t* consumed = nullptr) {
		const char* begin = s.c_str();
		char* end = nullptr;
		errno = 0;
		const double value = std::strtod(begin, &end);
		if (end == begin || errno == ERANGE) return false;

		out = value;
		if (consumed != nullptr) *consumed = static_cast<std::size_t>(end - begin);
		return true;
	}

}  // namespace wbsh

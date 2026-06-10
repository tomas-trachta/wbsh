#pragma once

/**
 * @file strscan.h
 * @brief Forward-only cursor for hand-rolled text scanning.
 *
 * Replaces chained find()/npos index juggling. Each call either
 * advances the cursor and returns true, or returns false and leaves
 * the scanner untouched, so steps chain with `&&` and a failed parse
 * never half-advances. The scanner views src[pos, end) and does not
 * own the string; the string must outlive it.
 */

#include <cstddef>
#include <cstring>
#include <string>

namespace wbsh {

	struct StrScan {
		const std::string& src;
		std::size_t pos = 0;
		std::size_t end;

		explicit StrScan(const std::string& s) : src(s), end(s.size()) {}

		std::string rest() const { return src.substr(pos, end - pos); }

		void skipSpaces() {
			while (pos < end && (src[pos] == ' ' || src[pos] == '\t')) ++pos;
		}

		bool consume(const char* lit) {
			const std::size_t n = std::strlen(lit);
			if (end - pos < n || src.compare(pos, n, lit) != 0) return false;
			pos += n;
			return true;
		}

		bool skipPast(const char* token) {
			const std::size_t n = std::strlen(token);
			const std::size_t at = src.find(token, pos);
			if (at == std::string::npos || at + n > end) return false;
			pos = at + n;
			return true;
		}

		/// Shrink the view so scanning stops before the next `c`.
		bool stopAt(char c) {
			const std::size_t at = src.find(c, pos);
			if (at == std::string::npos || at >= end) return false;
			end = at;
			return true;
		}

		bool readUpTo(char c, std::string& out) {
			const std::size_t at = src.find(c, pos);
			if (at == std::string::npos || at >= end) return false;
			out.assign(src, pos, at - pos);
			pos = at + 1;
			return true;
		}

		bool readUpTo(const char* token, std::string& out) {
			const std::size_t n = std::strlen(token);
			const std::size_t at = src.find(token, pos);
			if (at == std::string::npos || at + n > end) return false;
			out.assign(src, pos, at - pos);
			pos = at + n;
			return true;
		}

		/// Contents of the next `"..."` pair, skipping anything before it.
		bool readQuoted(std::string& out) {
			const std::size_t open = src.find('"', pos);
			if (open == std::string::npos || open >= end) return false;
			const std::size_t close = src.find('"', open + 1);
			if (close == std::string::npos || close >= end) return false;
			out.assign(src, open + 1, close - open - 1);
			pos = close + 1;
			return true;
		}
	};

}  // namespace wbsh

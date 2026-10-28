/**
 * @file ed_address.cpp
 * @brief ed's addresses: line numbers, `.` `$` marks, regex searches,
 *        offsets, and the `,` / `;` / `%` ranges built from them.
 */

#include "ed_internal.h"

#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace wbsh {

	namespace ed_detail {

		static bool isDigit(char c) {
			return c >= '0' && c <= '9';
		}

		void skipBlanks(CommandText& cmd) {
			while (cmd.peek() == ' ' || cmd.peek() == '\t') cmd.take();
		}

		// A delimiter used as a literal must not keep a special meaning.
		static std::string literalDelimiter(char delimiter) {
			const bool special = std::strchr(".*[]^$\\", delimiter) != nullptr
				|| std::isalnum(static_cast<unsigned char>(delimiter)) != 0;
			if (special) return std::string("[") + delimiter + "]";
			return std::string(1, delimiter);
		}

		static bool closesClass(const CommandText& cmd, char kind) {
			return cmd.peek() == kind && cmd.pos + 1 < cmd.text.size()
				&& cmd.text[cmd.pos + 1] == ']';
		}

		static void copyBracket(CommandText& cmd, std::string& pattern) {
			pattern.push_back(cmd.take());
			if (cmd.peek() == '^') pattern.push_back(cmd.take());
			if (cmd.peek() == ']') pattern.push_back(cmd.take());

			while (!cmd.atEnd() && cmd.peek() != ']' && cmd.peek() != '\n') {
				const char c = cmd.take();
				pattern.push_back(c);
				const bool opens_class = c == '[' && std::strchr(":.=", cmd.peek()) != nullptr;
				if (!opens_class || cmd.atEnd()) continue;

				const char kind = cmd.take();
				pattern.push_back(kind);
				while (!cmd.atEnd() && !closesClass(cmd, kind)) pattern.push_back(cmd.take());
			}

			if (cmd.peek() == ']') pattern.push_back(cmd.take());
		}

		// True when the closing delimiter was there (and is consumed).
		bool parsePattern(CommandText& cmd, char delimiter, std::string& pattern) {
			pattern.clear();
			while (!cmd.atEnd() && cmd.peek() != '\n') {
				const char c = cmd.peek();
				if (c == delimiter) {
					cmd.take();
					return true;
				}

				if (c == '[') {
					copyBracket(cmd, pattern);
					continue;
				}

				cmd.take();
				if (c != '\\' || cmd.atEnd() || cmd.peek() == '\n') {
					pattern.push_back(c);
					continue;
				}

				const char escaped = cmd.take();
				if (escaped == delimiter) pattern += literalDelimiter(delimiter);
				else pattern += std::string("\\") + escaped;
			}

			return false;
		}

		bool compilePattern(EdState& st, const std::string& pattern) {
			if (pattern.empty()) {
				if (!st.has_regex) return fail(st, "No previous pattern");
				return true;
			}

			PosixRegex regex;
			RegexOptions options;
			options.syntax = RegexSyntax::Basic;
			if (!regex.compile(pattern, options)) return fail(st, regex.error());

			st.regex = regex;
			st.has_regex = true;
			return true;
		}

		bool lineMatches(const EdState& st, std::size_t line) {
			return st.regex.test(st.lines[line - 1].text);
		}

		static bool searchLines(EdState& st, bool forward, std::size_t& found) {
			const std::size_t total = lastLine(st);
			std::size_t line = st.current;
			for (std::size_t step = 0; step < total; ++step) {
				if (forward) line = line >= total ? 1 : line + 1;
				else line = line <= 1 ? total : line - 1;

				if (lineMatches(st, line)) {
					found = line;
					return true;
				}
			}

			return fail(st, "No match");
		}

		static bool searchAddress(EdState& st, CommandText& cmd, long long& value) {
			const char delimiter = cmd.take();
			std::string pattern;
			parsePattern(cmd, delimiter, pattern);
			if (!compilePattern(st, pattern)) return false;

			std::size_t found = 0;
			if (!searchLines(st, delimiter == '/', found)) return false;
			value = static_cast<long long>(found);
			return true;
		}

		static long long takeNumber(CommandText& cmd) {
			long long value = 0;
			while (isDigit(cmd.peek())) {
				value = value * 10 + (cmd.take() - '0');
				if (value > 1000000000000LL) value = 1000000000000LL;
			}

			return value;
		}

		static bool markAddress(EdState& st, CommandText& cmd, long long& value) {
			cmd.take();
			const char mark = cmd.take();
			if (mark < 'a' || mark > 'z') return fail(st, "Invalid mark character");

			std::size_t line = 0;
			if (!lineOfMark(st, mark, line)) return fail(st, "Invalid address");
			value = static_cast<long long>(line);
			return true;
		}

		static bool baseAddress(EdState& st, CommandText& cmd, long long& value, bool& have) {
			const char c = cmd.peek();
			have = true;
			if (c == '.') {
				cmd.take();
				value = static_cast<long long>(st.current);
			} else if (c == '$') {
				cmd.take();
				value = static_cast<long long>(lastLine(st));
			} else if (isDigit(c)) {
				value = takeNumber(cmd);
			} else if (c == '\'') {
				return markAddress(st, cmd, value);
			} else if (c == '/' || c == '?') {
				return searchAddress(st, cmd, value);
			} else if (c == '+' || c == '-' || c == '^') {
				value = static_cast<long long>(st.current);
			} else {
				have = false;
			}

			return true;
		}

		static bool startsOffset(CommandText& cmd, bool have) {
			const std::size_t saved = cmd.pos;
			skipBlanks(cmd);
			const char c = cmd.peek();
			const bool offset = c == '+' || c == '-' || c == '^' || (have && isDigit(c));
			if (!offset) cmd.pos = saved;
			return offset;
		}

		static void applyOffsets(CommandText& cmd, long long& value, bool have) {
			while (startsOffset(cmd, have)) {
				const char c = cmd.peek();
				if (isDigit(c)) {
					value += takeNumber(cmd);
					continue;
				}

				cmd.take();
				const long long amount = isDigit(cmd.peek()) ? takeNumber(cmd) : 1;
				value += c == '+' ? amount : -amount;
			}
		}

		bool parseAddress(EdState& st, CommandText& cmd, std::size_t& address, bool& have) {
			skipBlanks(cmd);
			long long value = 0;
			if (!baseAddress(st, cmd, value, have)) return false;
			if (!have) return true;

			applyOffsets(cmd, value, have);
			if (value < 0 || value > static_cast<long long>(lastLine(st))) {
				return fail(st, "Invalid address");
			}

			address = static_cast<std::size_t>(value);
			return true;
		}

		static void storeRange(const std::vector<std::size_t>& list, Range& range) {
			range.count = list.size() < 2 ? list.size() : 2;
			if (list.empty()) return;

			range.second = list.back();
			range.first = list.size() >= 2 ? list[list.size() - 2] : list.back();
		}

		// `,` alone is 1,$ and `;` alone is .;$; a missing second address
		// repeats the first; `;` makes the first address current.
		bool parseRange(EdState& st, CommandText& cmd, Range& range) {
			std::vector<std::size_t> list;
			std::size_t value = 0;
			bool have = false;
			skipBlanks(cmd);
			if (cmd.peek() == '%') {
				cmd.take();
				cmd.text.insert(cmd.pos, ",");
			}

			if (!parseAddress(st, cmd, value, have)) return false;
			if (have) list.push_back(value);

			while (cmd.peek() == ',' || cmd.peek() == ';') {
				const char separator = cmd.take();
				const bool lone = !have;
				if (lone) list.push_back(separator == ',' ? 1 : st.current);
				if (separator == ';') st.current = list.back();
				if (!parseAddress(st, cmd, value, have)) return false;

				if (have) list.push_back(value);
				else list.push_back(lone ? lastLine(st) : list.back());
				have = true;
			}

			storeRange(list, range);
			return true;
		}

	}  // namespace ed_detail

}  // namespace wbsh

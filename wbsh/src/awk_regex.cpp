/**
 * @file awk_regex.cpp
 * @brief awk regex compilation (literal fast path or posixregex.h) and the dynamic-regex cache.
 */

#include "awk_regex.h"

#include <cctype>
#include <cstring>

namespace wbsh {

	namespace awk_detail {

		static bool isRegexOperator(char c) {
			return std::strchr(".[]()*+?{}|^$", c) != nullptr;
		}

		static bool escapedAt(const std::string& pattern, std::size_t pos) {
			std::size_t backslashes = 0;
			while (pos > backslashes && pattern[pos - backslashes - 1] == '\\') ++backslashes;
			return backslashes % 2 == 1;
		}

		// An escaped letter or digit is a class, an assertion or a numeric escape
		// (\w, \y, \101) except for \n and \t; gawk's \< \> \` \' are assertions too.
		static bool literalEscape(char escaped, char& out) {
			if (escaped == 'n') {
				out = '\n';
				return true;
			}

			if (escaped == 't') {
				out = '\t';
				return true;
			}

			if (std::isalnum(static_cast<unsigned char>(escaped))) return false;
			if (std::strchr("<>`'", escaped) != nullptr) return false;

			out = escaped;
			return true;
		}

		static bool literalNeedle(const std::string& pattern, std::size_t begin, std::size_t end,
				std::string& needle) {
			for (std::size_t i = begin; i < end; ++i) {
				const char c = pattern[i];
				if (c == '\\') {
					char escaped = 0;
					if (i + 1 >= end || !literalEscape(pattern[++i], escaped)) return false;
					needle.push_back(escaped);
					continue;
				}

				if (isRegexOperator(c)) return false;
				needle.push_back(c);
			}

			return true;
		}

		static bool extractLiteral(const std::string& pattern, AwkRegex& out) {
			std::size_t begin = 0;
			std::size_t end = pattern.size();
			const bool anchor_start = end > 0 && pattern[0] == '^';
			if (anchor_start) begin = 1;

			const bool anchor_end = end > begin && pattern[end - 1] == '$'
				&& !escapedAt(pattern, end - 1);
			if (anchor_end) --end;

			std::string needle;
			if (!literalNeedle(pattern, begin, end, needle)) return false;

			out.literal = true;
			out.anchor_start = anchor_start;
			out.anchor_end = anchor_end;
			out.needle = std::move(needle);
			return true;
		}

		bool compileAwkRegex(AwkRegex& out, const std::string& pattern) {
			out = AwkRegex();
			if (extractLiteral(pattern, out)) {
				out.ok = true;
				return true;
			}

			RegexOptions options;
			options.syntax = RegexSyntax::Awk;
			out.ok = out.regex.compile(pattern, options);
			return out.ok;
		}

		static bool findAnchoredStart(const AwkRegex& re, const std::string& subject,
				std::size_t from, std::size_t& pos) {
			if (from != 0) return false;
			if (re.anchor_end && subject != re.needle) return false;
			if (subject.compare(0, re.needle.size(), re.needle) != 0) return false;

			pos = 0;
			return true;
		}

		static bool findAnchoredEnd(const AwkRegex& re, const std::string& subject,
				std::size_t from, std::size_t& pos) {
			const std::size_t length = re.needle.size();
			if (subject.size() < length || subject.size() - length < from) return false;
			if (subject.compare(subject.size() - length, length, re.needle) != 0) return false;

			pos = subject.size() - length;
			return true;
		}

		static bool findLiteral(const AwkRegex& re, const std::string& subject, std::size_t from,
				std::size_t& pos) {
			if (re.anchor_start) return findAnchoredStart(re, subject, from, pos);
			if (re.anchor_end) return findAnchoredEnd(re, subject, from, pos);

			const std::size_t found = subject.find(re.needle, from);
			if (found == std::string::npos) return false;

			pos = found;
			return true;
		}

		bool awkRegexSearch(const AwkRegex& re, const std::string& subject) {
			if (!re.ok) return false;

			std::size_t pos = 0;
			if (re.literal) return findLiteral(re, subject, 0, pos);
			return re.regex.test(subject);
		}

		bool awkRegexFind(const AwkRegex& re, const std::string& subject, std::size_t from,
				std::size_t& pos, std::size_t& length) {
			if (!re.ok || from > subject.size()) return false;
			if (re.literal) {
				length = re.needle.size();
				return findLiteral(re, subject, from, pos);
			}

			RegexMatch match;
			if (!re.regex.search(subject, from, match)) return false;

			pos = match.start(0);
			length = match.length(0);
			return true;
		}

		std::shared_ptr<const AwkRegex> RegexCache::get(const std::string& pattern) {
			const auto found = index.find(pattern);
			if (found != index.end()) {
				entries.splice(entries.begin(), entries, found->second);
				return found->second->second;
			}

			auto compiled = std::make_shared<AwkRegex>();
			compileAwkRegex(*compiled, pattern);
			entries.emplace_front(pattern, compiled);
			index[pattern] = entries.begin();
			if (entries.size() > capacity) {
				index.erase(entries.back().first);
				entries.pop_back();
			}

			return compiled;
		}

	}  // namespace awk_detail

}  // namespace wbsh

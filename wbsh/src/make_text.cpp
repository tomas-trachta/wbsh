/**
 * @file make_text.cpp
 * @brief Word lists, `%` patterns, file name parts and the scanning of
 *        `$(...)` references that the rest of make is built on.
 */

#include "make_internal.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace wbsh::make_detail {

	bool isBlank(char c) {
		return c == ' ' || c == '\t';
	}

	static bool isSpaceChar(char c) {
		return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
	}

	std::vector<std::string> splitWords(const std::string& text) {
		std::vector<std::string> words;
		std::size_t i = 0;
		while (i < text.size()) {
			while (i < text.size() && isSpaceChar(text[i])) ++i;
			const std::size_t start = i;
			while (i < text.size() && !isSpaceChar(text[i])) ++i;
			if (i > start) words.push_back(text.substr(start, i - start));
		}

		return words;
	}

	std::string joinWords(const std::vector<std::string>& words) {
		std::string joined;
		for (const std::string& word : words) {
			if (!joined.empty()) joined.push_back(' ');
			joined += word;
		}

		return joined;
	}

	std::string trimBlanks(const std::string& text) {
		std::size_t start = 0;
		std::size_t end = text.size();
		while (start < end && isSpaceChar(text[start])) ++start;
		while (end > start && isSpaceChar(text[end - 1])) --end;
		return text.substr(start, end - start);
	}

	std::size_t skipBlanks(const std::string& text, std::size_t from) {
		while (from < text.size() && isBlank(text[from])) ++from;
		return from;
	}

	// Only the bracket that opened the reference nests, as in GNU make:
	// `$(a {b)` ends at the `)`.
	std::size_t findReferenceEnd(const std::string& text, std::size_t dollar) {
		const char open = text[dollar + 1];
		const char close = open == '(' ? ')' : '}';
		int depth = 0;
		for (std::size_t i = dollar + 2; i < text.size(); ++i) {
			if (text[i] == open) ++depth;
			if (text[i] != close) continue;
			if (depth == 0) return i;
			--depth;
		}

		return std::string::npos;
	}

	bool opensReference(const std::string& text, std::size_t i) {
		return text[i] == '$' && i + 1 < text.size() && (text[i + 1] == '(' || text[i + 1] == '{');
	}

	std::size_t findUnquoted(const std::string& text, const std::string& wanted, std::size_t from) {
		for (std::size_t i = from; i < text.size(); ++i) {
			const char c = text[i];
			if (opensReference(text, i)) {
				const std::size_t end = findReferenceEnd(text, i);
				if (end == std::string::npos) return std::string::npos;
				i = end;
				continue;
			}

			if (c == '$') {
				++i;
				continue;
			}

			if (c == '\\' && i + 1 < text.size() && wanted.find(text[i + 1]) != std::string::npos) {
				++i;
				continue;
			}

			if (wanted.find(c) != std::string::npos) return i;
		}

		return std::string::npos;
	}

	bool matchPattern(const std::string& pattern, const std::string& text, std::string& stem) {
		const std::size_t percent = pattern.find('%');
		if (percent == std::string::npos) {
			stem.clear();
			return pattern == text;
		}

		const std::size_t suffix_length = pattern.size() - percent - 1;
		if (text.size() < percent + suffix_length) return false;
		if (text.compare(0, percent, pattern, 0, percent) != 0) return false;

		const std::size_t suffix_start = text.size() - suffix_length;
		if (text.compare(suffix_start, suffix_length, pattern, percent + 1, suffix_length) != 0) {
			return false;
		}

		stem = text.substr(percent, suffix_start - percent);
		return true;
	}

	std::string substituteStem(const std::string& pattern, const std::string& stem) {
		const std::size_t percent = pattern.find('%');
		if (percent == std::string::npos) return pattern;

		return pattern.substr(0, percent) + stem + pattern.substr(percent + 1);
	}

	static std::string patsubstWord(const std::string& pattern, const std::string& replacement,
	                                const std::string& word) {
		std::string stem;
		if (!matchPattern(pattern, word, stem)) return word;
		if (pattern.find('%') == std::string::npos) return replacement;

		return substituteStem(replacement, stem);
	}

	std::string patsubstText(const std::string& pattern, const std::string& replacement,
	                         const std::string& text) {
		std::vector<std::string> words = splitWords(text);
		for (std::string& word : words) word = patsubstWord(pattern, replacement, word);
		return joinWords(words);
	}

	static std::size_t lastSlash(const std::string& word) {
		return word.find_last_of("/\\");
	}

	std::string directoryOf(const std::string& word) {
		const std::size_t slash = lastSlash(word);
		if (slash == std::string::npos) return "./";

		return word.substr(0, slash + 1);
	}

	std::string notDirectoryOf(const std::string& word) {
		const std::size_t slash = lastSlash(word);
		if (slash == std::string::npos) return word;

		return word.substr(slash + 1);
	}

	bool hasGlobMeta(const std::string& word) {
		return word.find_first_of("*?[") != std::string::npos;
	}

	std::vector<std::string> uniqueWords(const std::vector<std::string>& words) {
		std::vector<std::string> unique;
		std::unordered_set<std::string> seen;
		for (const std::string& word : words) {
			if (seen.insert(word).second) unique.push_back(word);
		}

		return unique;
	}

}  // namespace wbsh::make_detail

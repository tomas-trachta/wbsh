/**
 * @file awk_fields.cpp
 * @brief awk records and fields: field splitting, $N access and $0 reconstruction.
 */

#include <string>
#include <vector>

#include "awk_interp.h"
#include "regexutil.h"

namespace wbsh {

	namespace awk_detail {

		// A single space as FS is special in awk: fields are split on runs
		// of blanks and newlines, and leading/trailing ones are ignored.
		static bool isWhitespaceSeparator(const std::string& separator) {
			return separator == " ";
		}

		static bool isFieldBlank(char c) {
			return c == ' ' || c == '\t' || c == '\n';
		}

		std::vector<std::string> splitOnBlanks(const std::string& text) {
			std::vector<std::string> parts;
			const std::size_t size = text.size();
			std::size_t i = 0;
			while (i < size) {
				while (i < size && isFieldBlank(text[i])) ++i;
				if (i >= size) break;

				const std::size_t start = i;
				while (i < size && !isFieldBlank(text[i])) ++i;
				parts.push_back(text.substr(start, i - start));
			}

			return parts;
		}

		std::vector<std::string> splitOnChar(const std::string& text, char separator) {
			std::vector<std::string> parts;
			std::size_t start = 0;
			for (;;) {
				const std::size_t found = text.find(separator, start);
				if (found == std::string::npos) break;

				parts.push_back(text.substr(start, found - start));
				start = found + 1;
			}

			parts.push_back(text.substr(start));
			return parts;
		}

		static bool searchFrom(const std::string& text, std::size_t pos, const std::regex& re,
				std::smatch& match) {
			const auto flags = pos == 0
				? std::regex_constants::match_default
				: std::regex_constants::match_prev_avail;
			return searchRegexFlags(text.cbegin() + static_cast<std::ptrdiff_t>(pos), text.cend(),
				re, match, flags);
		}

		std::vector<std::string> splitByRegex(const std::string& text, const std::regex& re) {
			std::vector<std::string> parts;
			std::size_t pos = 0;
			for (;;) {
				std::smatch match;
				if (!searchFrom(text, pos, re, match)) break;

				const auto offset = static_cast<std::size_t>(match.position(0));
				const auto length = static_cast<std::size_t>(match.length(0));
				// A zero-width separator would re-match in place forever; stop and
				// let the tail below become the final field.
				if (length == 0) break;

				parts.push_back(text.substr(pos, offset));
				pos += offset + length;
			}

			parts.push_back(text.substr(pos));
			return parts;
		}

		static std::vector<std::string> splitIntoCharacters(const std::string& text) {
			std::vector<std::string> parts;
			parts.reserve(text.size());
			for (char c : text) parts.emplace_back(1, c);
			return parts;
		}

		std::vector<std::string> Interpreter::splitWithSeparator(const std::string& text,
				const std::string& separator) {
			if (text.empty()) return {};
			if (isWhitespaceSeparator(separator)) return splitOnBlanks(text);
			if (separator.empty()) return splitIntoCharacters(text);
			if (separator.size() == 1) return splitOnChar(text, separator[0]);

			std::regex storage;
			const std::regex* re = dynamicRegex(separator, storage);
			if (re == nullptr) return { text };
			return splitByRegex(text, *re);
		}

		const std::regex* Interpreter::cachedRegex(CachedRegex& cache, const std::string& pattern) {
			if (!cache.valid || cache.pattern != pattern) {
				cache.pattern = pattern;
				cache.ok = compileRegex(cache.regex, pattern);
				cache.valid = true;
			}

			return cache.ok ? &cache.regex : nullptr;
		}

		static std::string bracketEscaped(char c) {
			const std::string text(1, c);
			if (c == '\\' || c == ']' || c == '[' || c == '^' || c == '-') return "\\" + text;
			return text;
		}

		// With RS = "" a newline separates fields whatever FS is.
		std::string Interpreter::paragraphFieldPattern() const {
			if (FS.size() == 1) return "[" + bracketEscaped(FS[0]) + "\\n]";
			return "(?:" + FS + ")|\\n";
		}

		std::vector<std::string> Interpreter::recordParts() {
			if (record.empty()) return {};
			if (isWhitespaceSeparator(FS)) return splitOnBlanks(record);
			if (FS.size() == 1 && !RS.empty()) return splitOnChar(record, FS[0]);

			const std::regex* re = cachedRegex(fs_regex, RS.empty() ? paragraphFieldPattern() : FS);
			if (re == nullptr) return { record };
			return splitByRegex(record, *re);
		}

		void Interpreter::splitRecord() {
			std::vector<std::string> parts = recordParts();
			fields.clear();
			fields.reserve(parts.size());
			for (auto& part : parts) fields.push_back(AwkValue::input(std::move(part)));
		}

		void Interpreter::setRecord(std::string text) {
			record = std::move(text);
			splitRecord();
		}

		void Interpreter::rebuildRecord() {
			record.clear();
			for (std::size_t i = 0; i < fields.size(); ++i) {
				if (i != 0) record += OFS;
				record += text(fields[i]);
			}
		}

		AwkValue Interpreter::getField(int index) {
			if (index == 0) return AwkValue::input(record);
			if (index < 0) return AwkValue::str("");
			if (static_cast<std::size_t>(index) > fields.size()) return AwkValue::str("");
			return fields[static_cast<std::size_t>(index) - 1];
		}

		void Interpreter::growFields(std::size_t count) {
			if (count > fields.size()) fields.resize(count, AwkValue::str(""));
		}

		void Interpreter::setField(int index, AwkValue value) {
			if (index == 0) {
				setRecord(text(value));
				return;
			}

			if (index < 0) return;

			const auto slot = static_cast<std::size_t>(index);
			growFields(slot);
			fields[slot - 1] = std::move(value);
			rebuildRecord();
		}

		void Interpreter::setFieldCount(int count) {
			if (count < 0) count = 0;
			fields.resize(static_cast<std::size_t>(count), AwkValue::str(""));
			rebuildRecord();
		}

	}  // namespace awk_detail

}  // namespace wbsh

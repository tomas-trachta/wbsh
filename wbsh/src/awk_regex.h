#pragma once

/**
 * @file awk_regex.h
 * @brief Compiled awk regular expressions with a literal-text fast path, and
 *        the LRU cache for dynamic (string) regexes.
 */

#include <cstddef>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "posixregex.h"

namespace wbsh {

	namespace awk_detail {

		// A pattern without regex operators (optionally anchored with ^ / $) is
		// matched with plain string search instead of a compiled regex.
		struct AwkRegex {
			bool ok = false;
			bool literal = false;
			bool anchor_start = false;
			bool anchor_end = false;
			std::string needle;
			PosixRegex regex;
		};

		/// False when @p pattern is not a valid regular expression.
		bool compileAwkRegex(AwkRegex& out, const std::string& pattern);

		bool awkRegexSearch(const AwkRegex& re, const std::string& subject);

		/// The leftmost match starting at or after @p from, with the text before
		/// @p from still visible to anchors and word boundaries.
		bool awkRegexFind(const AwkRegex& re, const std::string& subject, std::size_t from,
			std::size_t& pos, std::size_t& length);

		struct RegexCache {
			using Entry = std::pair<std::string, std::shared_ptr<const AwkRegex>>;

			std::size_t capacity = 64;
			std::list<Entry> entries;
			std::unordered_map<std::string, std::list<Entry>::iterator> index;

			/// The compiled @p pattern (check `ok`), most recently used first.
			std::shared_ptr<const AwkRegex> get(const std::string& pattern);
		};

	}  // namespace awk_detail

}  // namespace wbsh

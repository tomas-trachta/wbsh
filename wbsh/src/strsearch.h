#pragma once

/**
 * @file strsearch.h
 * @brief Literal substring search: Boyer-Moore-Horspool for one needle,
 *        Aho-Corasick for several, both with optional ASCII case folding.
 */

#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace wbsh {

	struct LiteralMatch {
		std::size_t start = 0;
		std::size_t length = 0;
	};

	/// Rejects candidate matches (grep -w / -x); a null filter accepts all.
	using LiteralFilter = bool (*)(const void* context, const std::string& text,
	                              LiteralMatch match);

	using FoldTable = std::array<unsigned char, 256>;

	namespace strsearch_detail {

		struct AcNode {
			std::vector<std::pair<unsigned char, int>> children;
			int fail = 0;
			int output = 0;
			std::size_t depth = 0;
			bool terminal = false;
		};

		struct Candidate {
			LiteralFilter filter = nullptr;
			const void* context = nullptr;
			const std::string* text = nullptr;
			LiteralMatch best;
			bool found = false;
		};

	}  // namespace strsearch_detail

	class LiteralSet {
	public:
		void assign(const std::vector<std::string>& needles, bool ignore_case);

		/// Leftmost-longest accepted occurrence of any needle at or after @p from.
		bool find(const std::string& text, std::size_t from, LiteralMatch& match,
		          LiteralFilter filter = nullptr, const void* context = nullptr) const;

	private:
		std::size_t horspool(const std::string& text, std::size_t from) const;
		bool findSingle(const std::string& text, std::size_t from,
		                strsearch_detail::Candidate& candidate) const;
		bool findMany(const std::string& text, std::size_t from,
		              strsearch_detail::Candidate& candidate) const;
		void reportEndingAt(int state, std::size_t end,
		                    strsearch_detail::Candidate& candidate) const;
		int step(int state, unsigned char c) const;
		int child(int state, unsigned char c) const;
		void addNeedle(const std::string& needle);
		void linkFailures();

		FoldTable fold_{};
		std::array<std::size_t, 256> skip_{};
		std::array<int, 256> root_{};
		std::string needle_;
		std::vector<strsearch_detail::AcNode> nodes_;
		std::size_t longest_ = 0;
		bool single_ = true;
		bool has_empty_ = false;
	};

}  // namespace wbsh

/**
 * @file strsearch.cpp
 * @brief Horspool and Aho-Corasick literal search.
 */

#include "strsearch.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>

namespace wbsh {

	using strsearch_detail::AcNode;
	using strsearch_detail::Candidate;

	static void fillFoldTable(FoldTable& fold, bool ignore_case) {
		for (int c = 0; c < 256; ++c) {
			const int folded = ignore_case ? std::tolower(c) : c;
			fold[static_cast<std::size_t>(c)] = static_cast<unsigned char>(folded);
		}
	}

	static std::string foldText(const std::string& text, const FoldTable& fold) {
		std::string out = text;
		for (char& c : out) c = static_cast<char>(fold[static_cast<unsigned char>(c)]);
		return out;
	}

	static bool accepts(const Candidate& candidate, LiteralMatch match) {
		if (candidate.filter == nullptr) return true;
		return candidate.filter(candidate.context, *candidate.text, match);
	}

	static void consider(Candidate& candidate, std::size_t start, std::size_t length) {
		const LiteralMatch match{ start, length };
		const bool better = !candidate.found || start < candidate.best.start
			|| (start == candidate.best.start && length > candidate.best.length);
		if (!better || !accepts(candidate, match)) return;

		candidate.best = match;
		candidate.found = true;
	}

	void LiteralSet::assign(const std::vector<std::string>& needles, bool ignore_case) {
		fillFoldTable(fold_, ignore_case);
		single_ = needles.size() == 1;
		has_empty_ = false;
		longest_ = 0;
		nodes_.assign(1, AcNode());
		if (single_) {
			needle_ = foldText(needles.front(), fold_);
			skip_.fill(std::max<std::size_t>(needle_.size(), 1));
			for (std::size_t i = 0; i + 1 < needle_.size(); ++i) {
				skip_[static_cast<unsigned char>(needle_[i])] = needle_.size() - 1 - i;
			}
			return;
		}

		for (const std::string& needle : needles) addNeedle(foldText(needle, fold_));
		linkFailures();
	}

	void LiteralSet::addNeedle(const std::string& needle) {
		longest_ = std::max(longest_, needle.size());
		if (needle.empty()) {
			has_empty_ = true;
			return;
		}

		int state = 0;
		for (const char c : needle) {
			int next = child(state, static_cast<unsigned char>(c));
			if (next < 0) {
				next = static_cast<int>(nodes_.size());
				AcNode& parent = nodes_[static_cast<std::size_t>(state)];
				parent.children.emplace_back(static_cast<unsigned char>(c), next);
				AcNode created;
				created.depth = parent.depth + 1;
				nodes_.push_back(created);
			}
			state = next;
		}

		nodes_[static_cast<std::size_t>(state)].terminal = true;
	}

	int LiteralSet::child(int state, unsigned char c) const {
		for (const auto& edge : nodes_[static_cast<std::size_t>(state)].children) {
			if (edge.first == c) return edge.second;
		}

		return -1;
	}

	void LiteralSet::linkFailures() {
		root_.fill(0);
		std::deque<int> queue;
		for (const auto& edge : nodes_.front().children) {
			root_[edge.first] = edge.second;
			queue.push_back(edge.second);
		}

		while (!queue.empty()) {
			const int state = queue.front();
			queue.pop_front();
			for (const auto& edge : nodes_[static_cast<std::size_t>(state)].children) {
				AcNode& next = nodes_[static_cast<std::size_t>(edge.second)];
				next.fail = step(nodes_[static_cast<std::size_t>(state)].fail, edge.first);
				const AcNode& fallback = nodes_[static_cast<std::size_t>(next.fail)];
				next.output = fallback.terminal ? next.fail : fallback.output;
				queue.push_back(edge.second);
			}
		}
	}

	int LiteralSet::step(int state, unsigned char c) const {
		for (;;) {
			if (state == 0) return root_[c];
			const int next = child(state, c);
			if (next >= 0) return next;
			state = nodes_[static_cast<std::size_t>(state)].fail;
		}
	}

	std::size_t LiteralSet::horspool(const std::string& text, std::size_t from) const {
		const std::size_t length = needle_.size();
		const std::size_t size = text.size();
		if (length == 0) return from <= size ? from : std::string::npos;

		const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
		const auto last = static_cast<unsigned char>(needle_.back());
		for (std::size_t at = from; at + length <= size;) {
			const unsigned char c = fold_[bytes[at + length - 1]];
			if (c == last) {
				const auto* needle = reinterpret_cast<const unsigned char*>(needle_.data());
				std::size_t k = 0;
				while (k + 1 < length && fold_[bytes[at + k]] == needle[k]) ++k;
				if (k + 1 == length) return at;
			}
			at += skip_[c];
		}

		return std::string::npos;
	}

	bool LiteralSet::findSingle(const std::string& text, std::size_t from,
	                            Candidate& candidate) const {
		for (std::size_t at = from; at <= text.size();) {
			const std::size_t start = horspool(text, at);
			if (start == std::string::npos) return false;

			consider(candidate, start, needle_.size());
			if (candidate.found) return true;
			at = start + 1;
		}

		return false;
	}

	void LiteralSet::reportEndingAt(int state, std::size_t end, Candidate& candidate) const {
		const AcNode& node = nodes_[static_cast<std::size_t>(state)];
		int hit = node.terminal ? state : node.output;
		while (hit > 0) {
			const AcNode& match = nodes_[static_cast<std::size_t>(hit)];
			consider(candidate, end - match.depth, match.depth);
			hit = match.output;
		}
	}

	bool LiteralSet::findMany(const std::string& text, std::size_t from,
	                          Candidate& candidate) const {
		const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
		int state = 0;
		for (std::size_t at = from; at < text.size(); ++at) {
			if (has_empty_) consider(candidate, at, 0);
			if (candidate.found && at + 1 > candidate.best.start + longest_) break;

			state = step(state, fold_[bytes[at]]);
			reportEndingAt(state, at + 1, candidate);
		}

		if (has_empty_ && from <= text.size()) consider(candidate, text.size(), 0);
		return candidate.found;
	}

	bool LiteralSet::find(const std::string& text, std::size_t from, LiteralMatch& match,
	                      LiteralFilter filter, const void* context) const {
		Candidate candidate;
		candidate.filter = filter;
		candidate.context = context;
		candidate.text = &text;
		const bool found = single_ ? findSingle(text, from, candidate)
			: findMany(text, from, candidate);
		if (found) match = candidate.best;
		return found;
	}

}  // namespace wbsh

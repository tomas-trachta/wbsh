/**
 * @file picker.cpp
 * @brief Matching, ordering, and the handful of keys the overlay takes.
 */

#include "picker.h"

#include <algorithm>
#include <cctype>

namespace wbshterm {

	static const std::size_t kMaxItems = 50000;

	static char lowered(char letter) {
		return static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
	}

	static bool atWordBoundary(const std::string& text, std::size_t at) {
		if (at == 0) return true;

		const char previous = text[at - 1];
		return previous == '/' || previous == '\\' || previous == '_' || previous == '-'
			|| previous == '.' || previous == ' ';
	}

	// Scoring only the leftmost match misses the run a reader means: "scr"
	// against src/screen.cpp should find the adjacent letters, not the first
	// scattered ones. Every starting position is tried and the best kept.
	static bool scoreFrom(const std::string& query, const std::string& text, std::size_t start,
			int& out_score) {
		int score = 0;
		std::size_t at = start;
		bool previous_matched = false;

		for (char wanted : query) {
			while (at < text.size() && lowered(text[at]) != lowered(wanted)) {
				at++;
				previous_matched = false;
			}

			if (at == text.size()) return false;

			score += 10;
			if (previous_matched) score += 8;
			if (atWordBoundary(text, at)) score += 6;
			if (at < 8) score += 2;

			previous_matched = true;
			at++;
		}

		out_score = score;
		return true;
	}

	bool fuzzyScore(const std::string& query, const std::string& text, int& out_score) {
		if (query.empty()) {
			out_score = 0;
			return true;
		}

		int best = 0;
		bool found = false;

		for (std::size_t start = 0; start < text.size(); ++start) {
			if (lowered(text[start]) != lowered(query[0])) continue;

			int score = 0;
			if (!scoreFrom(query, text, start, score)) break;

			if (!found || score > best) best = score;
			found = true;
		}

		if (!found) return false;

		// A short name matching the same letters is the better answer.
		out_score = best - static_cast<int>(text.size() / 8);
		return true;
	}

	void Picker::begin(const std::string& prompt) {
		items_.clear();
		matches_.clear();
		query_.clear();
		prompt_     = prompt.empty() ? std::string("pick") : prompt;
		selected_   = 0;
		collecting_ = true;
		active_     = false;
	}

	void Picker::addItem(const std::string& text) {
		if (!collecting_ || items_.size() >= kMaxItems) return;
		items_.push_back(text);
	}

	void Picker::finish() {
		collecting_ = false;
		active_     = !items_.empty();
		refilter();
	}

	void Picker::cancel() {
		collecting_ = false;
		active_     = false;
		items_.clear();
		matches_.clear();
		query_.clear();
	}

	const std::string& Picker::item(int index) const {
		static const std::string nothing;
		if (index < 0 || static_cast<std::size_t>(index) >= items_.size()) return nothing;
		return items_[static_cast<std::size_t>(index)];
	}

	std::string Picker::chosen() const {
		if (matches_.empty()) return std::string();
		if (selected_ < 0 || static_cast<std::size_t>(selected_) >= matches_.size()) {
			return std::string();
		}

		return item(matches_[static_cast<std::size_t>(selected_)]);
	}

	void Picker::typeCharacter(char32_t code) {
		if (code < 0x20 || code == 0x7F) return;

		if (code < 0x80) query_.push_back(static_cast<char>(code));
		else query_.push_back('?');

		refilter();
	}

	void Picker::backspace() {
		if (query_.empty()) return;

		query_.pop_back();
		refilter();
	}

	void Picker::clearQuery() {
		query_.clear();
		refilter();
	}

	void Picker::moveSelection(int delta) {
		if (matches_.empty()) return;

		const int last = static_cast<int>(matches_.size()) - 1;
		selected_ = std::min(std::max(selected_ + delta, 0), last);
	}

	void Picker::refilter() {
		struct Scored {
			int index;
			int score;
		};

		std::vector<Scored> scored;
		for (std::size_t i = 0; i < items_.size(); ++i) {
			int score = 0;
			if (fuzzyScore(query_, items_[i], score)) {
				scored.push_back({ static_cast<int>(i), score });
			}
		}

		std::stable_sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
			return a.score > b.score;
		});

		matches_.clear();
		for (const Scored& entry : scored) matches_.push_back(entry.index);
		selected_ = 0;
	}

} /* namespace wbshterm */

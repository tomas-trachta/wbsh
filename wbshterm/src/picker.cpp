/**
 * @file picker.cpp
 * @brief Matching, ordering, and the handful of keys the overlay takes.
 */

#include "picker.h"

#include <algorithm>

namespace wbshterm {

	// Deep trees run past a hundred thousand entries once node_modules is
	// in them, and a list that stops early silently hides whole projects:
	// the one the reader wants may sort after the cut.
	static const std::size_t kMaxItems = 200000;

	// Plain ASCII folding, not std::tolower: this runs once per character of
	// every candidate on every keystroke, and locale folding of a UTF-8 byte
	// is both slower and meaningless.
	static char lowered(char letter) {
		return (letter >= 'A' && letter <= 'Z') ? static_cast<char>(letter + 32) : letter;
	}

	static bool isLowerAscii(unsigned char letter) {
		return letter >= 'a' && letter <= 'z';
	}

	static bool isUpperAscii(unsigned char letter) {
		return letter >= 'A' && letter <= 'Z';
	}

	static bool atWordBoundary(const std::string& text, std::size_t at) {
		if (at == 0) return true;

		const unsigned char previous = static_cast<unsigned char>(text[at - 1]);
		const unsigned char current  = static_cast<unsigned char>(text[at]);
		if (previous == '/' || previous == '\\' || previous == '_'
			|| previous == '-' || previous == '.' || previous == ' ') {
			return true;
		}

		return isLowerAscii(previous) && isUpperAscii(current);
	}

	// Reaching a character costs what it takes to get there: a run of
	// adjacent letters earns, a jump over unrelated text pays for the
	// distance. Without that price, "wbsh" scores as well scattered through
	// Josha_paid/website/index.html as it does against wbsh/src/screen.cpp.
	static int scoreCharacter(const std::string& text, std::size_t at,
			std::size_t previous_at, bool previous_matched) {
		int score = 1;
		if (atWordBoundary(text, at)) score += 8;
		if (!previous_matched) return score;

		const std::size_t gap = at - previous_at - 1;
		return score + (gap == 0 ? 5 : -static_cast<int>(gap));
	}

	static bool scoreFrom(const std::string& query, const std::string& text, std::size_t start,
			int& out_score) {
		std::size_t at = start;
		std::size_t previous_at = 0;
		bool previous_matched = false;
		int score = 0;

		for (char wanted : query) {
			while (at < text.size() && lowered(text[at]) != lowered(wanted)) at++;
			if (at == text.size()) return false;

			score += scoreCharacter(text, at, previous_at, previous_matched);

			previous_at = at;
			previous_matched = true;
			at++;
		}

		out_score = score;
		return true;
	}

	// Scoring only the leftmost match misses the run a reader means: "scr"
	// against src/screen.cpp should find the adjacent letters, not the first
	// scattered ones. Every starting position is tried and the best kept.
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
			if (!scoreFrom(query, text, start, score)) continue;

			if (!found || score > best) best = score;
			found = true;
		}

		if (!found) return false;

		// A shorter name matching the same letters is the better answer, but
		// only as a tie-breaker: a deep path that really matches still beats
		// a short one that barely does.
		out_score = best - static_cast<int>(text.size() / 32);
		return true;
	}

	void Picker::begin(const std::string& prompt) {
		items_.clear();
		matches_.clear();
		query_.clear();
		truncated_ = false;
		prompt_     = prompt.empty() ? std::string("pick") : prompt;
		selected_   = 0;
		collecting_ = true;
		active_     = false;
	}

	void Picker::addItem(const std::string& text) {
		if (!collecting_) return;

		if (items_.size() >= kMaxItems) {
			truncated_ = true;
			return;
		}

		items_.push_back(text);
	}

	void Picker::finish() {
		collecting_ = false;
		active_     = !items_.empty();
		refilter(false);
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

		refilter(true);
	}

	void Picker::backspace() {
		if (query_.empty()) return;

		query_.pop_back();
		refilter(false);
	}

	void Picker::clearQuery() {
		query_.clear();
		refilter(false);
	}

	void Picker::moveSelection(int delta) {
		if (matches_.empty()) return;

		const int last = static_cast<int>(matches_.size()) - 1;
		selected_ = std::min(std::max(selected_ + delta, 0), last);
	}

	void Picker::scoreInto(const std::vector<int>& candidates, std::vector<Scored>& out) const {
		out.clear();
		for (int index : candidates) {
			int score = 0;
			if (fuzzyScore(query_, items_[static_cast<std::size_t>(index)], score)) {
				out.push_back({ index, score });
			}
		}
	}

	std::vector<int> Picker::everyItem() const {
		std::vector<int> all(items_.size());
		for (std::size_t i = 0; i < items_.size(); ++i) all[i] = static_cast<int>(i);
		return all;
	}

	// Typing only ever narrows, so a longer query is scored against what
	// already matched rather than the whole list again. On a tree of a
	// hundred thousand paths that is the difference between a pause on every
	// keystroke and none.
	void Picker::refilter(bool narrowing) {
		std::vector<Scored> scored;
		scoreInto(narrowing ? matches_ : everyItem(), scored);

		std::stable_sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
			return a.score > b.score;
		});

		matches_.clear();
		for (const Scored& entry : scored) matches_.push_back(entry.index);
		selected_ = 0;
	}

} /* namespace wbshterm */

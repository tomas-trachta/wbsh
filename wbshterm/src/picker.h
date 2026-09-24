#pragma once

/**
 * @file picker.h
 * @brief The overlay list the shell asks the terminal to show.
 */

#include <string>
#include <vector>

namespace wbshterm {

	/**
	 * @brief A filterable list, with no drawing and no window in it.
	 *
	 * The shell sends the items; typing narrows them; Enter answers with
	 * the highlighted one. Everything here is ordinary state so the
	 * matching and the keys can be tested without a terminal.
	 */
	class Picker {
	public:
		void begin(const std::string& prompt);
		void addItem(const std::string& text);
		void finish();
		void cancel();

		bool collecting() const { return collecting_; }
		bool active() const { return active_; }

		void typeCharacter(char32_t code);
		void backspace();
		void clearQuery();
		void moveSelection(int delta);

		const std::string& prompt() const { return prompt_; }
		const std::string& query() const { return query_; }
		const std::vector<int>& matches() const { return matches_; }
		int selected() const { return selected_; }
		std::size_t itemCount() const { return items_.size(); }

		/** True when the shell offered more than the list holds. */
		bool truncated() const { return truncated_; }
		const std::string& item(int index) const;

		/** The highlighted item, or "" when nothing matches. */
		std::string chosen() const;

	private:
		struct Scored {
			int index;
			int score;
		};

		void refilter(bool narrowing);
		void scoreInto(const std::vector<int>& candidates,
			std::vector<Scored>& out) const;
		std::vector<int> everyItem() const;

		std::vector<std::string> items_;
		std::vector<int>         matches_;
		std::string              prompt_;
		std::string              query_;
		int                      selected_   = 0;
		bool                     truncated_  = false;
		bool                     collecting_ = false;
		bool                     active_     = false;
	};

	/**
	 * @brief Fuzzy subsequence match, scored so the best lands on top.
	 *
	 * Earlier matches, runs of adjacent characters and matches on word
	 * boundaries all score higher. Returns false when @p query is not a
	 * subsequence of @p text at all.
	 */
	bool fuzzyScore(const std::string& query, const std::string& text, int& out_score);

} /* namespace wbshterm */

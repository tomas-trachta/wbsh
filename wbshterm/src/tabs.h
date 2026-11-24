#pragma once

/**
 * @file tabs.h
 * @brief The window's tabs: each one a split tree of its own, one shown at a time.
 */

#include "pane_tree.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace wbshterm {

	/** One tab: its panes, laid out over the same client area as every other tab. */
	struct Tab {
		PaneTree panes;
	};

	/**
	 * @brief Owns the tabs in strip order and knows which one is shown.
	 *
	 * Order and selection only; nothing here starts or stops a shell. A
	 * removed tab is handed back so the window decides when its panes go.
	 */
	class TabList {
	public:
		bool empty() const { return tabs_.empty(); }
		std::size_t count() const { return tabs_.size(); }
		std::size_t activeIndex() const { return active_; }

		Tab& active() { return *tabs_[active_]; }
		const Tab& active() const { return *tabs_[active_]; }
		Tab& at(std::size_t index) { return *tabs_[index]; }
		const Tab& at(std::size_t index) const { return *tabs_[index]; }

		/** Puts @p tab at @p index, or at the end past it, and shows it. */
		Tab& insert(std::size_t index, std::unique_ptr<Tab> tab);

		void activate(std::size_t index);

		/** Shows the tab @p delta places along, wrapping at either end. */
		void step(int delta);

		/** Moves a tab to @p to; the tab that was shown stays shown. */
		void move(std::size_t from, std::size_t to);

		/**
		 * @brief Takes a tab out of the strip.
		 *
		 * Removing the shown tab shows the one that slides into its place,
		 * or the one before it when it was the last.
		 */
		std::unique_ptr<Tab> remove(std::size_t index);

		/** Where @p tab sits, or count() when it is not in the list. */
		std::size_t indexOf(const Tab* tab) const;

	private:
		std::vector<std::unique_ptr<Tab>> tabs_;
		std::size_t                       active_ = 0;
	};

} /* namespace wbshterm */

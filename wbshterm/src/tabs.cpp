/**
 * @file tabs.cpp
 * @brief Tab order and selection.
 */

#include "tabs.h"

#include <algorithm>

namespace wbshterm {

	Tab& TabList::insert(std::size_t index, std::unique_ptr<Tab> tab) {
		const std::size_t at = std::min(index, tabs_.size());
		tabs_.insert(tabs_.begin() + static_cast<std::ptrdiff_t>(at), std::move(tab));

		active_ = at;
		return *tabs_[at];
	}

	void TabList::activate(std::size_t index) {
		if (index < tabs_.size()) active_ = index;
	}

	void TabList::step(int delta) {
		if (tabs_.empty()) return;

		const auto count = static_cast<long long>(tabs_.size());
		const long long moved = (static_cast<long long>(active_) + delta) % count;
		active_ = static_cast<std::size_t>(moved < 0 ? moved + count : moved);
	}

	void TabList::move(std::size_t from, std::size_t to) {
		if (from >= tabs_.size() || to >= tabs_.size() || from == to) return;

		const Tab* shown = tabs_[active_].get();
		std::unique_ptr<Tab> moving = std::move(tabs_[from]);
		tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(from));
		tabs_.insert(tabs_.begin() + static_cast<std::ptrdiff_t>(to), std::move(moving));

		active_ = indexOf(shown);
	}

	std::unique_ptr<Tab> TabList::remove(std::size_t index) {
		if (index >= tabs_.size()) return nullptr;

		std::unique_ptr<Tab> removed = std::move(tabs_[index]);
		tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(index));

		if (index < active_) --active_;
		if (active_ >= tabs_.size()) active_ = tabs_.empty() ? 0 : tabs_.size() - 1;

		return removed;
	}

	std::size_t TabList::indexOf(const Tab* tab) const {
		for (std::size_t index = 0; index < tabs_.size(); ++index) {
			if (tabs_[index].get() == tab) return index;
		}

		return tabs_.size();
	}

} /* namespace wbshterm */

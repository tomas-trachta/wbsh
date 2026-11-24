/**
 * @file tab_strip.cpp
 * @brief Where each tab lands, and which part of the strip the pointer is over.
 */

#include "tab_strip.h"

#include <algorithm>
#include <cmath>

namespace wbshterm {

	static const float kNewGap      = 4.0f;
	static const float kNewInset    = 4.0f;
	static const float kTitleLead   = 12.0f;
	static const float kTitleTail   = 10.0f;
	static const float kCloseMargin = 8.0f;
	static const float kCloseGap    = 4.0f;

	bool operator==(const TabHit& left, const TabHit& right) {
		return left.part == right.part && left.index == right.index;
	}

	bool operator!=(const TabHit& left, const TabHit& right) {
		return !(left == right);
	}

	static bool rectHolds(const D2D1_RECT_F& rect, float x, float y) {
		return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
	}

	void TabStrip::setTabs(int count, int active) {
		count_  = std::max(count, 0);
		active_ = std::min(std::max(active, 0), std::max(count_ - 1, 0));
	}

	float TabStrip::firstLeft() const {
		return bounds_.left + metrics_.lead;
	}

	float TabStrip::tabTop() const {
		return std::min(bounds_.top + metrics_.top_inset, bounds_.bottom);
	}

	float TabStrip::tabWidth() const {
		if (count_ == 0) return 0.0f;

		const float room = bounds_.right - firstLeft() - metrics_.new_width - kNewGap
			- metrics_.drag_room;
		const float share = room / static_cast<float>(count_);
		return std::floor(std::min(std::max(share, metrics_.min_width), metrics_.max_width));
	}

	D2D1_RECT_F TabStrip::tabRect(int index) const {
		const float width = tabWidth();
		const float left = firstLeft() + width * static_cast<float>(index);
		return D2D1::RectF(left, tabTop(), left + width, bounds_.bottom);
	}

	D2D1_RECT_F TabStrip::closeRect(int index) const {
		const D2D1_RECT_F tab = tabRect(index);
		const float size = metrics_.close_size;
		const float middle = (tab.top + tab.bottom) * 0.5f;
		const float right = tab.right - kCloseMargin;
		return D2D1::RectF(right - size, middle - size * 0.5f, right, middle + size * 0.5f);
	}

	D2D1_RECT_F TabStrip::titleRect(int index) const {
		const D2D1_RECT_F tab = tabRect(index);
		const float right = closeFits(index)
			? closeRect(index).left - kCloseGap
			: tab.right - kTitleTail;
		return D2D1::RectF(tab.left + kTitleLead, tab.top, std::max(right, tab.left), tab.bottom);
	}

	D2D1_RECT_F TabStrip::newRect() const {
		const float left = count_ > 0 ? tabRect(count_ - 1).right + kNewGap : firstLeft();
		return D2D1::RectF(left, tabTop() + kNewInset, left + metrics_.new_width,
			bounds_.bottom - kNewInset);
	}

	bool TabStrip::closeFits(int index) const {
		return index == active_ || tabWidth() >= metrics_.close_width;
	}

	bool TabStrip::closeShown(int index) const {
		if (!closeFits(index)) return false;

		return index == active_ || (hovered_.part != TabPart::None && hovered_.index == index);
	}

	bool TabStrip::holdsPoint(float x, float y) const {
		return rectHolds(bounds_, x, y);
	}

	TabHit TabStrip::hitAt(float x, float y) const {
		TabHit hit;
		if (!holdsPoint(x, y) || y < tabTop()) return hit;

		if (rectHolds(newRect(), x, y)) {
			hit.part = TabPart::New;
			return hit;
		}

		for (int index = 0; index < count_; ++index) {
			if (!rectHolds(tabRect(index), x, y)) continue;

			const bool on_close = closeFits(index) && rectHolds(closeRect(index), x, y);
			hit.part  = on_close ? TabPart::Close : TabPart::Tab;
			hit.index = index;
			return hit;
		}

		return hit;
	}

	int TabStrip::slotAt(float x) const {
		const float width = tabWidth();
		if (count_ == 0 || width <= 0.0f) return 0;

		const int slot = static_cast<int>(std::floor((x - firstLeft()) / width));
		return std::min(std::max(slot, 0), count_ - 1);
	}

	bool TabStrip::setHovered(const TabHit& hit) {
		if (hovered_ == hit) return false;

		hovered_ = hit;
		return true;
	}

} /* namespace wbshterm */

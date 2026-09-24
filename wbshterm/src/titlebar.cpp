/**
 * @file titlebar.cpp
 * @brief Where each light lands, and which one the pointer is over.
 */

#include "titlebar.h"

namespace wbshterm {

	static const TitleButton kOrder[] = {
		TitleButton::Close,
		TitleButton::Minimize,
		TitleButton::Zoom,
	};

	// A light is small, and chasing a 12px circle with a mouse is not the
	// point of it; the reachable area is the gap between them as well.
	static const float kHitSlop = 4.0f;

	int titleButtonCount() {
		return static_cast<int>(sizeof(kOrder) / sizeof(kOrder[0]));
	}

	TitleButton titleButtonByIndex(int index) {
		if (index < 0 || index >= titleButtonCount()) return TitleButton::None;

		return kOrder[index];
	}

	static int indexOf(TitleButton which) {
		for (int index = 0; index < titleButtonCount(); ++index) {
			if (kOrder[index] == which) return index;
		}

		return -1;
	}

	float TitleBar::clusterWidth() const {
		const float count = static_cast<float>(titleButtonCount());
		return metrics_.margin * 2.0f + count * metrics_.diameter
			+ (count - 1.0f) * metrics_.gap;
	}

	D2D1_ELLIPSE TitleBar::circleOf(TitleButton which) const {
		const int index = indexOf(which);
		if (index < 0) return D2D1::Ellipse(D2D1::Point2F(0.0f, 0.0f), 0.0f, 0.0f);

		const float radius = metrics_.diameter * 0.5f;
		const float step = (metrics_.diameter + metrics_.gap)
			* static_cast<float>(index);
		const float x = metrics_.on_right
			? bounds_.right - metrics_.margin - radius - step
			: bounds_.left + metrics_.margin + radius + step;
		const float y = (bounds_.top + bounds_.bottom) * 0.5f;

		return D2D1::Ellipse(D2D1::Point2F(x, y), radius, radius);
	}

	TitleButton TitleBar::buttonAt(float x, float y) const {
		if (!holdsPoint(x, y)) return TitleButton::None;

		const float reach = metrics_.diameter * 0.5f + kHitSlop;
		for (int index = 0; index < titleButtonCount(); ++index) {
			const D2D1_ELLIPSE circle = circleOf(kOrder[index]);
			const float dx = x - circle.point.x;
			const float dy = y - circle.point.y;
			if (dx * dx + dy * dy <= reach * reach) return kOrder[index];
		}

		return TitleButton::None;
	}

	bool TitleBar::holdsPoint(float x, float y) const {
		return x >= bounds_.left && x < bounds_.right
			&& y >= bounds_.top && y < bounds_.bottom;
	}

	bool TitleBar::setHovered(TitleButton which) {
		if (hovered_ == which) return false;

		hovered_ = which;
		return true;
	}

} /* namespace wbshterm */

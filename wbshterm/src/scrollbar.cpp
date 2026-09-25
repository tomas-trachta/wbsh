/**
 * @file scrollbar.cpp
 * @brief Thumb arithmetic: rows to pixels on the way out, back on a drag.
 */

#include "scrollbar.h"

#include <algorithm>
#include <cmath>

namespace wbshterm {

	static const float kThumbWidth = 6.0f;
	static const float kEdgeInset  = 2.0f;
	static const float kMinThumb   = 24.0f;
	static const float kHitSlop    = 4.0f;

	static float trackHeight(const ScrollbarShape& shape) {
		return shape.track.bottom - shape.track.top;
	}

	static D2D1_RECT_F trackFor(const ScrollbarFrame& frame, const Screen& screen) {
		const float right = frame.bounds.right - kEdgeInset;
		const float top   = frame.bounds.top + frame.padding;
		const float rows  = static_cast<float>(screen.rows());

		return D2D1::RectF(right - kThumbWidth, top, right, top + rows * frame.cell_height);
	}

	static float thumbLength(const ScrollbarShape& shape, const Screen& screen) {
		const float visible = static_cast<float>(screen.rows());
		const float total   = static_cast<float>(screen.totalRows());
		const float natural = trackHeight(shape) * visible / total;

		return std::min(std::max(natural, kMinThumb), trackHeight(shape));
	}

	static float thumbTravel(const ScrollbarShape& shape) {
		return trackHeight(shape) - thumbHeight(shape);
	}

	static D2D1_RECT_F thumbFor(const ScrollbarShape& shape, float length, const Screen& screen,
			const TerminalView& view) {
		const float hidden   = static_cast<float>(screen.totalRows() - screen.rows());
		const float fraction = static_cast<float>(view.topRow(screen)) / hidden;
		const float top      = shape.track.top + (trackHeight(shape) - length) * fraction;

		return D2D1::RectF(shape.track.left, top, shape.track.right, top + length);
	}

	ScrollbarShape scrollbarShape(const ScrollbarFrame& frame, const Screen& screen,
			const TerminalView& view) {
		ScrollbarShape shape;
		if (screen.onAltScreen() || screen.totalRows() <= screen.rows()) return shape;

		shape.track = trackFor(frame, screen);
		if (trackHeight(shape) <= 0.0f) return shape;

		shape.thumb   = thumbFor(shape, thumbLength(shape, screen), screen, view);
		shape.present = true;
		return shape;
	}

	bool scrollbarHolds(const ScrollbarShape& shape, float x, float y) {
		if (!shape.present) return false;

		return x >= shape.track.left - kHitSlop && x <= shape.track.right + kHitSlop
			&& y >= shape.track.top && y <= shape.track.bottom;
	}

	bool thumbHolds(const ScrollbarShape& shape, float x, float y) {
		return scrollbarHolds(shape, x, y)
			&& y >= shape.thumb.top && y <= shape.thumb.bottom;
	}

	float thumbHeight(const ScrollbarShape& shape) {
		return shape.thumb.bottom - shape.thumb.top;
	}

	int rowForThumbTop(const ScrollbarShape& shape, float thumb_top, const Screen& screen) {
		const float travel = thumbTravel(shape);
		if (travel <= 0.0f) return 0;

		const float fraction = std::min(std::max((thumb_top - shape.track.top) / travel, 0.0f),
			1.0f);
		const float hidden = static_cast<float>(screen.totalRows() - screen.rows());

		return static_cast<int>(std::lround(fraction * hidden));
	}

} /* namespace wbshterm */

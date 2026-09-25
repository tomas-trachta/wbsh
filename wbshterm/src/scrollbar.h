#pragma once

/**
 * @file scrollbar.h
 * @brief A thin thumb in a pane's right margin, showing where the view is.
 */

#include "screen.h"
#include "view.h"

#include <d2d1.h>

namespace wbshterm {

	/** Where the bar sits in a pane; nothing when there is no history. */
	struct ScrollbarShape {
		D2D1_RECT_F track{};
		D2D1_RECT_F thumb{};
		bool        present = false;
	};

	/** The pane's rectangle and the grid inside it, as the layout placed them. */
	struct ScrollbarFrame {
		D2D1_RECT_F bounds{};
		float       padding     = 0.0f;
		float       cell_height = 0.0f;
	};

	ScrollbarShape scrollbarShape(const ScrollbarFrame& frame, const Screen& screen,
		const TerminalView& view);

	/** True over the bar, with some slack to the side for a thin target. */
	bool scrollbarHolds(const ScrollbarShape& shape, float x, float y);

	bool thumbHolds(const ScrollbarShape& shape, float x, float y);

	float thumbHeight(const ScrollbarShape& shape);

	/** The absolute row to show at the top so the thumb's top is at @p thumb_top. */
	int rowForThumbTop(const ScrollbarShape& shape, float thumb_top, const Screen& screen);

} /* namespace wbshterm */

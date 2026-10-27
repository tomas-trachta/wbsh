/**
 * @file mouse.cpp
 * @brief Mouse arithmetic kept apart from the window: wheel deltas to lines.
 */

#include "mouse.h"

#include <windows.h>

namespace wbshterm {

	static bool oppositeSigns(int left, int right) {
		return (left < 0 && right > 0) || (left > 0 && right < 0);
	}

	int WheelAccumulator::linesFor(int delta, int lines_per_notch) {
		if (lines_per_notch <= 0) {
			remainder_ = 0;
			return 0;
		}

		if (oppositeSigns(delta, remainder_)) remainder_ = 0;

		const int total = remainder_ + delta * lines_per_notch;
		const int lines = total / WHEEL_DELTA;
		remainder_ = total - lines * WHEEL_DELTA;
		return lines;
	}

} /* namespace wbshterm */

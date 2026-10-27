#pragma once

/**
 * @file mouse.h
 * @brief Mouse arithmetic kept apart from the window: wheel deltas to lines.
 */

namespace wbshterm {

	/**
	 * @brief Turns wheel deltas into whole lines to scroll.
	 *
	 * A precision touchpad sends many deltas far below one notch
	 * (WHEEL_DELTA); what falls short of a line is kept for the next
	 * event instead of being thrown away, and dropped when the direction
	 * turns so a reversed swipe answers at once.
	 */
	class WheelAccumulator {
	public:
		/** Positive deltas, and the lines returned for them, scroll back into history. */
		int linesFor(int delta, int lines_per_notch);

		void reset() { remainder_ = 0; }

	private:
		int remainder_ = 0;
	};

} /* namespace wbshterm */

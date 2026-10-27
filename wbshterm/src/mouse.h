#pragma once

/**
 * @file mouse.h
 * @brief Mouse arithmetic kept apart from the window: pixels to DIPs, wheel deltas to
 *        lines, and the reports a program that asked for the mouse is sent.
 */

#include <string>

namespace wbshterm {

	/** Which events a program asked for: DECSET 9, 1000, 1002 or 1003. */
	enum class MouseTracking {
		kOff,
		kX10,
		kClicks,
		kDrags,
		kMotion,
	};

	/** How a report spells itself: X10 bytes, 1005 UTF-8, or 1006 SGR. */
	enum class MouseEncoding {
		kDefault,
		kUtf8,
		kSgr,
	};

	struct MouseModes {
		MouseTracking tracking         = MouseTracking::kOff;
		MouseEncoding encoding         = MouseEncoding::kDefault;
		bool          alternate_scroll = true;
	};

	enum class MouseButton {
		kLeft,
		kMiddle,
		kRight,
		kNone,
		kWheelUp,
		kWheelDown,
		kWheelLeft,
		kWheelRight,
	};

	enum class MouseAction {
		kPress,
		kRelease,
		kMotion,
	};

	/** One event in a pane; the cell is zero-based from the grid's top-left. */
	struct MouseReport {
		MouseAction action  = MouseAction::kPress;
		MouseButton button  = MouseButton::kLeft;
		int         column  = 0;
		int         row     = 0;
		bool        shift   = false;
		bool        alt     = false;
		bool        control = false;
	};

	/** Whether @p tracking asks for @p report; motion carries kNone when no button is held. */
	bool mouseTrackingWants(MouseTracking tracking, const MouseReport& report);

	/** The bytes for @p report; empty when @p encoding cannot reach its cell. */
	std::string encodeMouseReport(const MouseModes& modes, const MouseReport& report);

	/** A mouse position in pixels at @p dpi as DIPs; a dpi of 0 counts as 96. */
	float pixelsToDips(int pixels, unsigned int dpi);

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

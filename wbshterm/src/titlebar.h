#pragma once

/**
 * @file titlebar.h
 * @brief The window's own caption: where the lights sit, and what is under
 *        the pointer.
 *
 * Geometry and hover state only. Nothing here knows about Win32 or
 * Direct2D, so the hit-testing is checked without a window.
 */

#include <d2d1.h>

namespace wbshterm {

	enum class TitleButton {
		None,
		Close,
		Minimize,
		Zoom,
	};

	/**
	 * @brief The three lights, outermost first.
	 *
	 * Close leads, so on the right it ends up furthest out, where
	 * Windows puts close, and on the left where macOS does.
	 */
	int titleButtonCount();
	TitleButton titleButtonByIndex(int index);

	/** Sizes in DIPs, before any DPI scaling the window applies. */
	struct TitleBarMetrics {
		float height   = 38.0f;
		float diameter = 12.0f;
		float gap      = 8.0f;
		float margin   = 14.0f;
		bool  on_right = true;
	};

	class TitleBar {
	public:
		void applyMetrics(const TitleBarMetrics& metrics) { metrics_ = metrics; }
		float height() const { return metrics_.height; }

		void setBounds(const D2D1_RECT_F& bounds) { bounds_ = bounds; }
		const D2D1_RECT_F& bounds() const { return bounds_; }

		D2D1_ELLIPSE circleOf(TitleButton which) const;

		/** What the lights take from their end, margin included. */
		float clusterWidth() const;

		/** The light under the pointer, with a little room around each. */
		TitleButton buttonAt(float x, float y) const;
		bool holdsPoint(float x, float y) const;

		/** Returns true when the hover moved, which is when to repaint. */
		bool setHovered(TitleButton which);
		TitleButton hovered() const { return hovered_; }

		void setPressed(TitleButton which) { pressed_ = which; }
		TitleButton pressed() const { return pressed_; }

		void setActive(bool active) { active_ = active; }
		bool active() const { return active_; }

	private:
		TitleBarMetrics metrics_;
		D2D1_RECT_F     bounds_{};
		TitleButton     hovered_ = TitleButton::None;
		TitleButton     pressed_ = TitleButton::None;
		bool            active_  = true;
	};

} /* namespace wbshterm */

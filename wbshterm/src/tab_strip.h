#pragma once

/**
 * @file tab_strip.h
 * @brief Where each tab, its close mark and the new-tab button sit, and
 *        what the pointer is over.
 *
 * Geometry and hover state only, like titlebar.h: no Win32 and no
 * Direct2D beyond the rectangle type, so the hit-testing is checked
 * without a window.
 */

#include <d2d1.h>

namespace wbshterm {

	enum class TabPart {
		None,
		Tab,
		Close,
		New,
	};

	/** A part of the strip, and the tab it belongs to (-1 for none or New). */
	struct TabHit {
		TabPart part  = TabPart::None;
		int     index = -1;
	};

	bool operator==(const TabHit& left, const TabHit& right);
	bool operator!=(const TabHit& left, const TabHit& right);

	/**
	 * @brief Sizes in DIPs.
	 *
	 * Tabs share the room evenly between min_width and max_width. The
	 * room above them and drag_room after the new-tab button stay empty,
	 * so a caption full of tabs can still be dragged. A tab narrower
	 * than close_width shows its close mark only while it is the active one.
	 */
	struct TabStripMetrics {
		float top_inset   = 7.0f;
		float lead        = 8.0f;
		float min_width   = 28.0f;
		float max_width   = 220.0f;
		float close_width = 72.0f;
		float close_size  = 18.0f;
		float new_width   = 32.0f;
		float drag_room   = 46.0f;
	};

	class TabStrip {
	public:
		void applyMetrics(const TabStripMetrics& metrics) { metrics_ = metrics; }

		void setBounds(const D2D1_RECT_F& bounds) { bounds_ = bounds; }
		const D2D1_RECT_F& bounds() const { return bounds_; }

		void setTabs(int count, int active);
		int count() const { return count_; }
		int active() const { return active_; }

		float tabWidth() const;
		D2D1_RECT_F tabRect(int index) const;
		D2D1_RECT_F titleRect(int index) const;
		D2D1_RECT_F closeRect(int index) const;
		D2D1_RECT_F newRect() const;

		/** Whether a tab is wide enough for its close mark, or is the active one. */
		bool closeFits(int index) const;

		/** Drawn on the active tab and the one under the pointer. */
		bool closeShown(int index) const;

		bool holdsPoint(float x, float y) const;
		TabHit hitAt(float x, float y) const;

		/** The tab whose place @p x is over, held to the first and last; for dragging. */
		int slotAt(float x) const;

		/** Returns true when the hover moved, which is when to repaint. */
		bool setHovered(const TabHit& hit);
		const TabHit& hovered() const { return hovered_; }

		void setPressed(const TabHit& hit) { pressed_ = hit; }
		const TabHit& pressed() const { return pressed_; }

	private:
		float firstLeft() const;
		float tabTop() const;

		TabStripMetrics metrics_;
		D2D1_RECT_F     bounds_{};
		int             count_  = 0;
		int             active_ = 0;
		TabHit          hovered_;
		TabHit          pressed_;
	};

} /* namespace wbshterm */

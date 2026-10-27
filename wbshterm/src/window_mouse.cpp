/**
 * @file window_mouse.cpp
 * @brief The mouse for programs that asked for it: reports, the wheel as keys, and
 *        what the middle button does when nobody did.
 */

#include "window.h"

#include <windowsx.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace wbshterm {

	static bool keyHeld(int virtual_key) {
		return (::GetKeyState(virtual_key) & 0x8000) != 0;
	}

	static int buttonBit(MouseButton button) {
		return 1 << static_cast<int>(button);
	}

	static MouseButton heldButton(WPARAM wparam) {
		if ((wparam & MK_LBUTTON) != 0) return MouseButton::kLeft;
		if ((wparam & MK_MBUTTON) != 0) return MouseButton::kMiddle;
		if ((wparam & MK_RBUTTON) != 0) return MouseButton::kRight;
		return MouseButton::kNone;
	}

	static MouseButton wheelButton(int notches, bool tilt) {
		if (tilt) return notches > 0 ? MouseButton::kWheelRight : MouseButton::kWheelLeft;
		return notches > 0 ? MouseButton::kWheelUp : MouseButton::kWheelDown;
	}

	static std::string arrowKey(bool up, bool application_cursor) {
		const std::string prefix = application_cursor ? "\x1bO" : "\x1b[";
		return prefix + (up ? "A" : "B");
	}

	// Shift is the way past a program that took the mouse: held, clicks
	// select, the right button opens the menu and the wheel scrolls.
	bool TerminalWindow::mouseReportsTo(const Pane& pane) const {
		if (keyHeld(VK_SHIFT)) return false;
		return pane.screen().mouseModes().tracking != MouseTracking::kOff;
	}

	bool TerminalWindow::mouseLeafAlive() const {
		if (mouse_leaf_ == nullptr) return false;

		const std::vector<PaneNode*> leaves = panes_.leaves();
		return std::find(leaves.begin(), leaves.end(), mouse_leaf_) != leaves.end();
	}

	MouseReport TerminalWindow::mouseReportAt(const PaneNode& leaf, D2D1_POINT_2F mouse,
			MouseAction action, MouseButton button) const {
		const GridPoint cell = cellAtDips(leaf, mouse);

		MouseReport report;
		report.action  = action;
		report.button  = button;
		report.column  = cell.column;
		report.row     = cell.row;
		report.shift   = keyHeld(VK_SHIFT);
		report.alt     = keyHeld(VK_MENU);
		report.control = keyHeld(VK_CONTROL);
		return report;
	}

	void TerminalWindow::sendMouseReport(const PaneNode& leaf, const MouseReport& report) {
		Pane& pane = *leaf.pane();
		const MouseModes& modes = pane.screen().mouseModes();
		if (!mouseTrackingWants(modes.tracking, report)) return;

		const std::string bytes = encodeMouseReport(modes, report);
		if (bytes.empty()) return;

		pane.view().scrollToBottom(pane.screen());
		pane.session().writeInput(bytes.data(), bytes.size());
	}

	// Buttons pressed together belong to the pane the first one went down
	// on, and so do the drags and releases after it, wherever they happen.
	bool TerminalWindow::reportMousePress(MouseButton button, LPARAM lparam) {
		const D2D1_POINT_2F point = mouseDips(lparam);
		PaneNode* leaf = mouse_buttons_ != 0 && mouseLeafAlive()
			? mouse_leaf_
			: reportingLeafAt(point);
		if (leaf == nullptr) return false;

		panes_.focusOn(leaf);
		mouse_leaf_     = leaf;
		mouse_buttons_ |= buttonBit(button);
		motion_leaf_    = leaf;
		motion_cell_    = cellAtDips(*leaf, point);
		if (::GetCapture() != window_) ::SetCapture(window_);

		sendMouseReport(*leaf, mouseReportAt(*leaf, point, MouseAction::kPress, button));
		::InvalidateRect(window_, nullptr, FALSE);
		return true;
	}

	bool TerminalWindow::reportMouseRelease(MouseButton button, LPARAM lparam) {
		if ((mouse_buttons_ & buttonBit(button)) == 0) return false;

		mouse_buttons_ &= ~buttonBit(button);
		if (mouseLeafAlive()) {
			sendMouseReport(*mouse_leaf_,
				mouseReportAt(*mouse_leaf_, mouseDips(lparam), MouseAction::kRelease, button));
		}

		if (mouse_buttons_ == 0 && ::GetCapture() == window_) ::ReleaseCapture();
		return true;
	}

	// A button held that no report went out for is a selection or a drag
	// of the window's own, and the motion is left to it.
	bool TerminalWindow::reportMouseMotion(WPARAM wparam, LPARAM lparam) {
		const bool any_button = (wparam & (MK_LBUTTON | MK_MBUTTON | MK_RBUTTON)) != 0;
		if (mouse_buttons_ == 0 && any_button) return false;

		const D2D1_POINT_2F point = mouseDips(lparam);
		const PaneNode* leaf = mouse_buttons_ != 0 && mouseLeafAlive()
			? mouse_leaf_
			: reportingLeafAt(point);
		if (leaf == nullptr) return mouse_buttons_ != 0;

		const GridPoint cell = cellAtDips(*leaf, point);
		const bool same_cell = cell.row == motion_cell_.row && cell.column == motion_cell_.column;
		if (leaf == motion_leaf_ && same_cell) return true;

		motion_leaf_ = leaf;
		motion_cell_ = cell;
		sendMouseReport(*leaf, mouseReportAt(*leaf, point, MouseAction::kMotion,
			heldButton(wparam)));
		return true;
	}

	// A wheel report is one notch, however many lines the control panel
	// would scroll for it.
	bool TerminalWindow::reportWheel(const PaneNode& leaf, D2D1_POINT_2F point, int delta,
			bool tilt) {
		if (!mouseReportsTo(*leaf.pane()) || pointerOverScrollbar(point.x, point.y)) return false;

		WheelAccumulator& accumulator = tilt ? tilt_ : wheel_;
		const int notches = accumulator.linesFor(delta, 1);
		const MouseReport report = mouseReportAt(leaf, point, MouseAction::kPress,
			wheelButton(notches, tilt));
		for (int notch = 0; notch < std::abs(notches); ++notch) sendMouseReport(leaf, report);
		return true;
	}

	// The view has no sideways scroll, so only a program that took the
	// mouse hears the tilt wheel.
	void TerminalWindow::onMouseTilt(WPARAM wparam, LPARAM lparam) {
		POINT where = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
		::ScreenToClient(window_, &where);

		const D2D1_POINT_2F point = dipsOf(where);
		const PaneNode* leaf = panes_.leafAt(point.x, point.y);
		if (leaf != nullptr) reportWheel(*leaf, point, GET_WHEEL_DELTA_WPARAM(wparam), true);
	}

	// The alternate screen keeps no history to scroll back into, so the
	// wheel goes to the program as arrow keys: less, man and a pager in
	// git scroll with it (DECSET 1007, on unless a program turns it off).
	bool TerminalWindow::alternateScrollTakes(Pane& pane, int lines) {
		const Screen& screen = pane.screen();
		if (!screen.onAltScreen() || !screen.mouseModes().alternate_scroll) return false;

		const std::string key = arrowKey(lines > 0, screen.applicationCursorKeys());
		std::string bytes;
		for (int line = 0; line < std::abs(lines); ++line) bytes += key;

		pane.session().writeInput(bytes.data(), bytes.size());
		return true;
	}

	// With no program asking for it, the middle button pastes, as it does
	// in most terminals that grew up on X.
	void TerminalWindow::onMiddleClick(LPARAM lparam) {
		PaneNode* leaf = leafFromMouse(lparam);
		if (leaf == nullptr) return;

		panes_.focusOn(leaf);
		pasteFromClipboard();
	}

	void TerminalWindow::forgetMouseReporting() {
		mouse_leaf_    = nullptr;
		mouse_buttons_ = 0;
	}

} /* namespace wbshterm */

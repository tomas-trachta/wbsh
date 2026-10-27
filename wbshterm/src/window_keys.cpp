/**
 * @file window_keys.cpp
 * @brief Keys as input records (win32-input-mode), focus reports, and where the IME opens.
 */

#include "window.h"

#include <imm.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#pragma comment(lib, "imm32.lib")

namespace wbshterm {

	static const LPARAM kExtendedKeyBit = 1 << 24;

	static bool held(int virtual_key) {
		return (::GetKeyState(virtual_key) & 0x8000) != 0;
	}

	static bool toggled(int virtual_key) {
		return (::GetKeyState(virtual_key) & 0x1) != 0;
	}

	static Win32KeyState currentKeyState(bool enhanced) {
		Win32KeyState state;
		state.left_alt      = held(VK_LMENU);
		state.right_alt     = held(VK_RMENU);
		state.left_control  = held(VK_LCONTROL);
		state.right_control = held(VK_RCONTROL);
		state.shift         = held(VK_SHIFT);
		state.num_lock      = toggled(VK_NUMLOCK);
		state.scroll_lock   = toggled(VK_SCROLL);
		state.caps_lock     = toggled(VK_CAPITAL);
		state.enhanced      = enhanced;
		return state;
	}

	// The message's lParam holds the scan code in bits 16-23, the repeat
	// count in the low word and the extended-key flag in bit 24.
	static Win32KeyEvent keyEventFor(unsigned int virtual_key, LPARAM lparam, bool down) {
		Win32KeyEvent event;
		event.virtual_key   = virtual_key;
		event.scan_code     = static_cast<unsigned int>((lparam >> 16) & 0xFF);
		event.down          = down;
		event.repeat        = down ? std::max(1u, static_cast<unsigned int>(lparam & 0xFFFF)) : 1u;
		event.control_state = win32ControlState(currentKeyState((lparam & kExtendedKeyBit) != 0));
		return event;
	}

	static unsigned int keySlot(unsigned int virtual_key) {
		return virtual_key & 0xFF;
	}

	// The search box, the picker and a pane command after the prefix read
	// characters themselves, so keys go to them the usual way meanwhile.
	bool TerminalWindow::win32InputTakesKeys() const {
		if (panes_.focused() == nullptr || !focused().screen().win32InputMode()) return false;
		return !search_.active() && !focused().picker().active() && !prefix_pending_;
	}

	// TranslateMessage has already posted the character a key makes by
	// the time its WM_KEYDOWN arrives, and posted messages are read before
	// the next key, so a look at the queue tells whether one is coming.
	bool TerminalWindow::characterFollows() const {
		MSG next{};
		if (::PeekMessageW(&next, window_, WM_CHAR, WM_CHAR, PM_NOREMOVE) != 0) return true;
		return ::PeekMessageW(&next, window_, WM_SYSCHAR, WM_SYSCHAR, PM_NOREMOVE) != 0;
	}

	// A key that makes a character goes out with it, as one record, once
	// the character arrives; what AltGr or a dead key produced is carried
	// as the layout made it. Alt+F4 stays the window's.
	bool TerminalWindow::sendWin32KeyDown(const KeyPress& press, LPARAM lparam) {
		if (press.virtual_key == VK_F4 && press.alt && !press.control) return false;

		const Win32KeyEvent event = keyEventFor(press.virtual_key, lparam, true);
		keys_down_sent_.set(keySlot(event.virtual_key));

		if (characterFollows()) {
			pending_key_         = event;
			pending_key_waiting_ = true;
			pending_key_used_    = false;
			return true;
		}

		writeWin32Key(event, !isModifierKey(event.virtual_key));
		return true;
	}

	void TerminalWindow::sendWin32Character(wchar_t unit) {
		if (pending_key_waiting_) sendKeyedCharacter(unit);
		else sendKeylessCharacter(unit);
	}

	void TerminalWindow::sendKeyedCharacter(wchar_t unit) {
		Win32KeyEvent event = pending_key_;
		event.unicode = static_cast<unsigned int>(unit);
		pending_key_used_ = true;
		writeWin32Key(event, true);
	}

	// A character with no key behind it (an IME's result, the emoji panel)
	// goes out as a press and a release of no key at all, the way the
	// console itself reports one. Both halves of a surrogate pair go down
	// before either comes up, so a reader joining them never meets a
	// release in between.
	void TerminalWindow::sendKeylessCharacter(wchar_t unit) {
		Win32KeyEvent event;
		event.control_state = win32ControlState(currentKeyState(false));
		event.unicode       = static_cast<unsigned int>(unit);
		writeWin32Key(event, true);

		if (unit >= 0xD800 && unit < 0xDC00) {
			keyless_high_ = unit;
			return;
		}

		event.down = false;
		if (keyless_high_ != 0) {
			Win32KeyEvent high = event;
			high.unicode = static_cast<unsigned int>(keyless_high_);
			writeWin32Key(high, false);
			keyless_high_ = 0;
		}

		writeWin32Key(event, false);
	}

	// Typing scrolls back to the prompt and drops the selection, as it
	// always has; a modifier going down or any key coming up must not, or
	// Ctrl+Shift+C would lose the selection to its own Ctrl.
	void TerminalWindow::writeWin32Key(const Win32KeyEvent& event, bool typing) {
		const std::string bytes = encodeWin32Key(event);
		if (typing) {
			sendBytes(bytes.data(), bytes.size());
			return;
		}

		focused().session().writeInput(bytes.data(), bytes.size());
	}

	void TerminalWindow::flushPendingKey() {
		if (!pending_key_waiting_) return;

		pending_key_waiting_ = false;
		if (pending_key_used_) return;

		writeWin32Key(pending_key_, !isModifierKey(pending_key_.virtual_key));
	}

	// Only keys whose press went out as a record get their release, so a
	// key the window kept for itself never reaches the program half done.
	bool TerminalWindow::onKeyUp(WPARAM key, LPARAM lparam) {
		flushPendingKey();

		const unsigned int virtual_key = static_cast<unsigned int>(key);
		if (!keys_down_sent_.test(keySlot(virtual_key))) return false;

		keys_down_sent_.reset(keySlot(virtual_key));
		if (panes_.focused() == nullptr || !focused().screen().win32InputMode()) return false;

		writeWin32Key(keyEventFor(virtual_key, lparam, false), false);
		return true;
	}

	// Keys held while the focus goes elsewhere come up in another window;
	// the program is told they did, or it would think them held forever.
	void TerminalWindow::releaseHeldKeys() {
		flushPendingKey();
		if (panes_.focused() == nullptr || !focused().screen().win32InputMode()) {
			keys_down_sent_.reset();
			return;
		}

		for (unsigned int slot = 0; slot < keys_down_sent_.size(); ++slot) {
			if (!keys_down_sent_.test(slot)) continue;

			const UINT scan = ::MapVirtualKeyW(slot, MAPVK_VK_TO_VSC);
			writeWin32Key(keyEventFor(slot, static_cast<LPARAM>(scan) << 16, false), false);
		}

		keys_down_sent_.reset();
	}

	void TerminalWindow::noteFocusMessage(UINT message) {
		if (message == WM_SETFOCUS) has_focus_ = true;
		if (message != WM_KILLFOCUS) return;

		has_focus_ = false;
		releaseHeldKeys();
	}

	Pane* TerminalWindow::livePane(const Pane* pane) const {
		if (pane == nullptr) return nullptr;

		for (PaneNode* leaf : panes_.leaves()) {
			if (leaf->pane() == pane) return leaf->pane();
		}

		return nullptr;
	}

	void TerminalWindow::reportFocus(Pane& pane, bool focused) {
		if (!pane.screen().focusEvents()) return;

		const std::string bytes = focused ? "\x1b[I" : "\x1b[O";
		pane.session().writeInput(bytes.data(), bytes.size());
	}

	// Focus is the window's keyboard focus and, inside it, the focused
	// pane: switching panes tells the one left behind and the one entered.
	void TerminalWindow::syncFocusReports() {
		const PaneNode* leaf = panes_.focused();
		Pane* now = has_focus_ && leaf != nullptr ? leaf->pane() : nullptr;
		if (now == focus_reported_) return;

		Pane* before = livePane(focus_reported_);
		focus_reported_ = now;
		if (before != nullptr) reportFocus(*before, false);
		if (now != nullptr) reportFocus(*now, true);
	}

	static LONG pixelsOfDips(float dips, UINT dpi) {
		return static_cast<LONG>(std::lround(dips * static_cast<float>(dpi)
			/ static_cast<float>(USER_DEFAULT_SCREEN_DPI)));
	}

	POINT TerminalWindow::cursorPixels() const {
		const PaneNode& leaf = *panes_.focused();
		const Screen& screen = leaf.pane()->screen();
		const CellMetrics& cell = renderer_.metrics();

		const int absolute = screen.scrollbackRows() + screen.cursor().row;
		const int row = std::min(std::max(absolute - leaf.pane()->view().topRow(screen), 0),
			screen.rows() - 1);
		const float x = leaf.bounds().left + renderer_.padding()
			+ static_cast<float>(screen.cursor().column) * cell.width;
		const float y = leaf.bounds().top + renderer_.padding()
			+ static_cast<float>(row) * cell.height;
		return { pixelsOfDips(x, dpi_), pixelsOfDips(y, dpi_) };
	}

	static void setImeFont(HIMC context, const std::wstring& family, LONG height) {
		LOGFONTW font{};
		font.lfHeight  = -height;
		font.lfCharSet = DEFAULT_CHARSET;
		family.copy(font.lfFaceName, LF_FACESIZE - 1);
		::ImmSetCompositionFontW(context, &font);
	}

	// The IME draws its composition and its candidates in windows of its
	// own; told where the cursor is, they open on it rather than in the
	// window's corner, and the candidates keep clear of the line typed on.
	void TerminalWindow::positionImeWindow() {
		if (panes_.focused() == nullptr) return;

		const HIMC context = ::ImmGetContext(window_);
		if (context == nullptr) return;

		const POINT at = cursorPixels();
		const LONG width  = pixelsOfDips(renderer_.metrics().width, dpi_);
		const LONG height = pixelsOfDips(renderer_.metrics().height, dpi_);

		COMPOSITIONFORM composition{};
		composition.dwStyle      = CFS_POINT;
		composition.ptCurrentPos = at;
		::ImmSetCompositionWindow(context, &composition);

		CANDIDATEFORM candidates{};
		candidates.dwIndex      = 0;
		candidates.dwStyle      = CFS_EXCLUDE;
		candidates.ptCurrentPos = at;
		candidates.rcArea       = { at.x, at.y, at.x + width, at.y + height };
		::ImmSetCandidateWindow(context, &candidates);

		setImeFont(context, config_.font.family, height);
		::ImmReleaseContext(window_, context);
	}

} /* namespace wbshterm */

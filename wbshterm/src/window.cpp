/**
 * @file window.cpp
 * @brief Window creation, the message handlers, and key encoding.
 */

#include "window.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

namespace wbshterm {

	static const wchar_t kClassName[] = L"wbshtermWindow";
	static const UINT    kMessagePtyData = WM_APP + 1;
	static const UINT_PTR kTimerBlink = 1;
	static const UINT_PTR kTimerConfig = 2;
	static const UINT     kBlinkMs = 530;
	static const UINT     kConfigPollMs = 1000;
	static const UINT_PTR kTimerResize = 3;
	static const UINT     kResizeSettleMs = 80;
	static const int     kWheelLines = 3;
	static const float   kDividerSlop = 3.0f;

	static std::string encodeUtf8(wchar_t character) {
		char bytes[8] = {};
		const int length = ::WideCharToMultiByte(CP_UTF8, 0, &character, 1, bytes,
			static_cast<int>(sizeof(bytes)), nullptr, nullptr);
		return length > 0 ? std::string(bytes, static_cast<std::size_t>(length)) : std::string();
	}

	static bool keyIsDown(int virtual_key) {
		return (::GetKeyState(virtual_key) & 0x8000) != 0;
	}

	static KeyPress currentKeyPress(WPARAM key) {
		KeyPress press;
		press.virtual_key = static_cast<unsigned int>(key);
		press.control     = keyIsDown(VK_CONTROL);
		press.alt         = keyIsDown(VK_MENU);
		press.shift       = keyIsDown(VK_SHIFT);
		return press;
	}

	// Windows turns some of the keys handled here into a character message
	// as well; those need swallowing, and the rest must not set the flag or
	// it eats the next thing typed.
	static bool alsoProducesCharacter(const KeyPress& press) {
		switch (press.virtual_key) {
		case VK_SPACE:
		case VK_TAB:
		case VK_BACK:
		case VK_RETURN:
		case VK_ESCAPE:
			return true;
		default:
			return press.virtual_key >= '0' && press.virtual_key <= 'Z';
		}
	}

	static bool isPasteShortcut(const KeyPress& press) {
		if (press.virtual_key == 'V' && press.control) return true;
		return press.virtual_key == VK_INSERT && press.shift && !press.control;
	}

	static std::string clipboardText() {
		if (!::IsClipboardFormatAvailable(CF_UNICODETEXT)) return std::string();
		if (!::OpenClipboard(nullptr)) return std::string();

		std::string text;
		const HANDLE handle = ::GetClipboardData(CF_UNICODETEXT);
		const auto* wide = handle != nullptr
			? static_cast<const wchar_t*>(::GlobalLock(handle))
			: nullptr;

		if (wide != nullptr) {
			const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0,
				nullptr, nullptr);
			if (needed > 1) {
				text.resize(static_cast<std::size_t>(needed) - 1);
				::WideCharToMultiByte(CP_UTF8, 0, wide, -1, text.data(), needed, nullptr, nullptr);
			}

			::GlobalUnlock(handle);
		}

		::CloseClipboard();
		return text;
	}


	Pane& TerminalWindow::focused() {
		return *panes_.focused()->pane();
	}

	const Pane& TerminalWindow::focused() const {
		return *panes_.focused()->pane();
	}

	// OSC 7 reports a Windows path with its slashes the POSIX way round;
	// CreateProcess wants them back.
	static std::wstring nativeDirectory(const std::string& reported) {
		if (reported.empty()) return std::wstring();

		const int needed = ::MultiByteToWideChar(CP_UTF8, 0, reported.c_str(),
			static_cast<int>(reported.size()), nullptr, 0);
		if (needed <= 0) return std::wstring();

		std::wstring wide(static_cast<std::size_t>(needed), L'\0');
		::MultiByteToWideChar(CP_UTF8, 0, reported.c_str(),
			static_cast<int>(reported.size()), wide.data(), needed);

		for (wchar_t& letter : wide) {
			if (letter == L'/') letter = L'\\';
		}

		return wide;
	}

	// A new pane opens where the focused one is, the way a tmux split
	// inherits the current path rather than starting back at home.
	//
	// It is laid out before it is started so its shell draws its first
	// prompt at the width it will keep, rather than at one it never had.
	bool TerminalWindow::startPane(Pane& pane, std::string& out_error) {
		ShellCommand shell;
		shell.command_line = command_line_;
		shell.working_directory = directory_hint_;

		pane.screen().setTmuxHandler(this);
		pane.session().wakeWith(window_, kMessagePtyData);
		if (!pane.start(shell, out_error)) return false;

		pane.screen().setScrollbackLimit(config_.scrollback_lines);

		// The startup panel belongs to the session, not to every pane in
		// it: a split wants a prompt, not the system information again.
		::SetEnvironmentVariableW(L"WBSH_INIT_COMMAND", nullptr);
		return true;
	}

	void TerminalWindow::splitFocused(SplitAxis axis) {
		directory_hint_ = nativeDirectory(focused().screen().workingDirectory());

		auto added = std::make_unique<Pane>();
		Pane* fresh = added.get();
		panes_.splitFocused(axis, std::move(added));
		layoutPanes();

		std::string error;
		if (!startPane(*fresh, error)) {
			panes_.close(panes_.focused());
			layoutPanes();
		}

		commitGridSizes();
		syncTitle();
		::InvalidateRect(window_, nullptr, FALSE);
	}

	void TerminalWindow::closeFocused() {
		if (!panes_.close(panes_.focused())) {
			::DestroyWindow(window_);
			return;
		}

		layoutPanes();
		commitGridSizes();
		syncTitle();
		::InvalidateRect(window_, nullptr, FALSE);
	}

	void TerminalWindow::stopEveryPane() {
		for (PaneNode* leaf : panes_.leaves()) leaf->pane()->session().stop();
	}

	void TerminalWindow::applyScrollbackLimit() {
		for (PaneNode* leaf : panes_.leaves()) {
			leaf->pane()->screen().setScrollbackLimit(config_.scrollback_lines);
		}
	}


	// Typing `tmux` is how pane mode starts, so until then the prefix key
	// belongs to the shell: Ctrl-B is a line-editing key right up to the
	// moment it is not. The pty hears about the row the status bar takes
	// on the resize timer, because this runs inside the parse of the very
	// bytes that asked for it.
	void TerminalWindow::tmuxAttach() {
		if (tmux_mode_) return;

		tmux_mode_ = true;
		layoutPanes();
		scheduleGridCommit();
		::InvalidateRect(window_, nullptr, FALSE);
	}

	float TerminalWindow::statusHeight() const {
		if (!tmux_mode_ || !config_.panes.status) return 0.0f;

		return renderer_.metrics().height;
	}

	static std::string lastPathSegment(const std::string& path) {
		const std::size_t cut = path.find_last_of("/\\");
		if (cut == std::string::npos) return path;

		return path.substr(cut + 1);
	}

	static std::string paneName(const Pane& pane) {
		const std::string name = lastPathSegment(pane.screen().workingDirectory());
		return name.empty() ? "wbsh" : name;
	}

	// tmux's own shape: the session on the left, then every window with the
	// active one starred. Panes stand in for windows; there is one session.
	std::string TerminalWindow::statusLeft() const {
		std::string text = "[wbsh]";

		const std::vector<PaneNode*> leaves = panes_.leaves();
		for (std::size_t i = 0; i < leaves.size(); ++i) {
			text += " " + std::to_string(i) + ":" + paneName(*leaves[i]->pane());
			text += leaves[i] == panes_.focused() ? "*" : " ";
		}

		if (panes_.zoomed()) text += " Z";
		return text;
	}

	static std::string computerName() {
		wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
		DWORD length = MAX_COMPUTERNAME_LENGTH + 1;
		if (::GetComputerNameW(name, &length) == 0) return std::string();

		std::string narrow(length, '\0');
		::WideCharToMultiByte(CP_UTF8, 0, name, static_cast<int>(length),
			narrow.data(), static_cast<int>(length), nullptr, nullptr);
		return narrow;
	}

	std::string TerminalWindow::statusRight() const {
		return "\"" + computerName() + "\" " + shown_clock_;
	}

	static std::string clockText() {
		static const char* const kMonths[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
			"Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

		SYSTEMTIME now{};
		::GetLocalTime(&now);

		char text[32] = {};
		std::snprintf(text, sizeof(text), "%02u:%02u %02u-%s-%02u",
			now.wHour, now.wMinute, now.wDay,
			kMonths[(now.wMonth - 1) % 12], now.wYear % 100);
		return text;
	}

	// The clock only moves once a minute, so the window is only repainted
	// for it once a minute rather than on every blink.
	bool TerminalWindow::statusClockChanged() {
		if (statusHeight() == 0.0f) return false;

		const std::string now = clockText();
		if (now == shown_clock_) return false;

		shown_clock_ = now;
		return true;
	}


	// DWMWA_WINDOW_CORNER_PREFERENCE and DWMWCP_ROUNDSMALL. Windows 11
	// rounds the corners from this; Windows 10 rejects it and stays square,
	// which is why nothing here depends on the call having worked.
	static const DWORD kCornerPreference = 33;
	static const DWORD kRoundSmall = 3;
	static const int   kCornerRadius = 9;

	// Windows 11 rounds the corners itself: antialiased, shadowed, and
	// nothing to maintain. Windows 10 refuses the attribute, and the
	// only rounded silhouette to be had there is one cut out of the
	// window. That edge is hard, but at this radius it reads as a
	// rounded corner rather than as a staircase.
	//
	// A maximised window is square on both, the way every other one is.
	void TerminalWindow::applyCornerRegion() {
		if (dwm_rounds_corners_ || !customFrame()) return;
		if (::IsIconic(window_) != 0) return;

		if (::IsZoomed(window_) != 0) {
			::SetWindowRgn(window_, nullptr, TRUE);
			return;
		}

		RECT window{};
		::GetWindowRect(window_, &window);

		const int width = window.right - window.left;
		const int height = window.bottom - window.top;
		if (width <= 0 || height <= 0) return;

		const HRGN region = ::CreateRoundRectRgn(0, 0, width + 1, height + 1,
			kCornerRadius * 2, kCornerRadius * 2);
		::SetWindowRgn(window_, region, TRUE);
	}

	bool TerminalWindow::customFrame() const {
		return config_.titlebar.custom;
	}

	float TerminalWindow::titleHeight() const {
		return customFrame() ? static_cast<float>(config_.titlebar.height) : 0.0f;
	}

	int TerminalWindow::resizeBorder() const {
		return ::GetSystemMetrics(SM_CXSIZEFRAME) + ::GetSystemMetrics(SM_CXPADDEDBORDER);
	}

	// Keeping the proposed rectangle whole is what takes the caption and the
	// border away. Maximised, Windows deliberately oversizes the window by
	// the frame, so that much has to be given back or the grid spills off
	// every edge of the screen.
	LRESULT TerminalWindow::onCalcSize(WPARAM wparam, LPARAM lparam) {
		if (wparam == FALSE) return ::DefWindowProcW(window_, WM_NCCALCSIZE, wparam, lparam);
		if (::IsZoomed(window_) == 0) return 0;

		auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
		const int grip = resizeBorder();
		params->rgrc[0].left   += grip;
		params->rgrc[0].top    += grip;
		params->rgrc[0].right  -= grip;
		params->rgrc[0].bottom -= grip;
		return 0;
	}

	// Nothing is left to grab once the frame is gone, so the edges are
	// handed back by hand. Maximised there is nothing to resize.
	LRESULT TerminalWindow::frameEdgeAt(int x, int y, const RECT& client) const {
		if (::IsZoomed(window_) != 0) return HTNOWHERE;

		const int grip = resizeBorder();
		const bool left = x < grip;
		const bool right = x >= client.right - grip;
		const bool top = y < grip;
		const bool bottom = y >= client.bottom - grip;

		if (top && left) return HTTOPLEFT;
		if (top && right) return HTTOPRIGHT;
		if (bottom && left) return HTBOTTOMLEFT;
		if (bottom && right) return HTBOTTOMRIGHT;
		if (top) return HTTOP;
		if (bottom) return HTBOTTOM;
		if (left) return HTLEFT;
		if (right) return HTRIGHT;

		return HTNOWHERE;
	}

	// A light answers HTCLIENT so the ordinary mouse messages reach it; the
	// rest of the bar answers HTCAPTION, which is what hands dragging,
	// double-click-to-zoom and Aero Snap back to Windows for free.
	LRESULT TerminalWindow::onHitTest(LPARAM lparam) {
		POINT where = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
		::ScreenToClient(window_, &where);

		RECT client{};
		::GetClientRect(window_, &client);

		const LRESULT edge = frameEdgeAt(where.x, where.y, client);
		if (edge != HTNOWHERE) return edge;

		const float x = static_cast<float>(where.x);
		const float y = static_cast<float>(where.y);
		if (title_bar_.buttonAt(x, y) != TitleButton::None) return HTCLIENT;
		if (title_bar_.holdsPoint(x, y)) return HTCAPTION;

		return HTCLIENT;
	}

	void TerminalWindow::runTitleButton(TitleButton which) {
		switch (which) {
		case TitleButton::Close:
			::PostMessageW(window_, WM_CLOSE, 0, 0);
			return;
		case TitleButton::Minimize:
			::ShowWindow(window_, SW_MINIMIZE);
			return;
		case TitleButton::Zoom:
			::ShowWindow(window_, ::IsZoomed(window_) != 0 ? SW_RESTORE : SW_MAXIMIZE);
			return;
		default:
			return;
		}
	}

	bool TerminalWindow::titleBarTakesPress(LPARAM lparam) {
		const TitleButton which = title_bar_.buttonAt(
			static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
		if (which == TitleButton::None) return false;

		title_bar_.setPressed(which);
		::SetCapture(window_);
		::InvalidateRect(window_, nullptr, FALSE);
		return true;
	}

	// Acting on release, and only when the pointer is still on the light it
	// went down on, is what lets a press be taken back by sliding off it.
	bool TerminalWindow::titleBarTakesRelease(LPARAM lparam) {
		const TitleButton pressed = title_bar_.pressed();
		if (pressed == TitleButton::None) return false;

		title_bar_.setPressed(TitleButton::None);
		if (::GetCapture() == window_) ::ReleaseCapture();
		::InvalidateRect(window_, nullptr, FALSE);

		const TitleButton under = title_bar_.buttonAt(
			static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
		if (under == pressed) runTitleButton(pressed);

		return true;
	}

	void TerminalWindow::armMouseLeave() {
		TRACKMOUSEEVENT track{};
		track.cbSize = sizeof(track);
		track.dwFlags = TME_LEAVE;
		track.hwndTrack = window_;
		::TrackMouseEvent(&track);
	}

	void TerminalWindow::trackTitleHover(LPARAM lparam) {
		const TitleButton which = title_bar_.buttonAt(
			static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
		if (!title_bar_.setHovered(which)) return;

		if (which != TitleButton::None) armMouseLeave();
		::InvalidateRect(window_, nullptr, FALSE);
	}

	void TerminalWindow::clearTitleHover() {
		if (!title_bar_.setHovered(TitleButton::None)) return;

		::InvalidateRect(window_, nullptr, FALSE);
	}

	void TerminalWindow::setWindowActive(bool active) {
		title_bar_.setActive(active);
		::InvalidateRect(window_, nullptr, FALSE);
	}

	// The taskbar keeps the whole path; the caption shows the leaf, the way
	// a Mac window is named for the folder rather than the route to it.
	std::wstring TerminalWindow::captionText() const {
		const std::size_t cut = shown_title_.find_last_of(L"/\\");
		if (cut == std::wstring::npos || cut + 1 >= shown_title_.size()) return shown_title_;

		return shown_title_.substr(cut + 1);
	}

	void TerminalWindow::layoutPanes() {
		if (!target_) return;

		const D2D1_SIZE_F size = target_->GetSize();
		const CellMetrics& cell = renderer_.metrics();

		TitleBarMetrics caption;
		caption.height   = titleHeight();
		caption.on_right = config_.titlebar.on_right;
		title_bar_.applyMetrics(caption);
		title_bar_.setBounds(D2D1::RectF(0.0f, 0.0f, size.width, titleHeight()));

		status_bounds_ = D2D1::RectF(0.0f, size.height - statusHeight(),
			size.width, size.height);

		PaneMetrics metrics;
		metrics.client      = D2D1::RectF(0.0f, titleHeight(), size.width,
			status_bounds_.top);
		metrics.cell_width  = cell.width;
		metrics.cell_height = cell.height;
		metrics.padding     = renderer_.padding();
		metrics.divider     = static_cast<float>(config_.panes.divider);
		panes_.layout(metrics);
	}

	// Resizing a pseudoconsole makes its child repaint everything it shows,
	// so a drag lets the pointer settle before any pty hears a new size.
	void TerminalWindow::scheduleGridCommit() {
		::SetTimer(window_, kTimerResize, kResizeSettleMs, nullptr);
	}

	void TerminalWindow::commitGridSizes() {
		::KillTimer(window_, kTimerResize);
		if (::IsIconic(window_) != 0) return;

		for (PaneNode* leaf : panes_.leaves()) leaf->pane()->commitGridSize();
	}


	static bool isModifierKey(unsigned int key) {
		switch (key) {
		case VK_SHIFT:
		case VK_LSHIFT:
		case VK_RSHIFT:
		case VK_CONTROL:
		case VK_LCONTROL:
		case VK_RCONTROL:
		case VK_MENU:
		case VK_LMENU:
		case VK_RMENU:
		case VK_LWIN:
		case VK_RWIN:
			return true;
		default:
			return false;
		}
	}

	// Which key carries a pane command is decided by the character it
	// produces, not its virtual key, so | and - stay where the keycaps say
	// they are on a layout that is not American.
	static bool mayCarryCharacter(unsigned int key) {
		if (key >= '0' && key <= 'Z') return true;

		return key >= VK_OEM_1 && key <= VK_OEM_102;
	}

	static bool arrowDirection(unsigned int key, PaneDirection& out_direction) {
		switch (key) {
		case VK_LEFT:  out_direction = PaneDirection::Left;  return true;
		case VK_RIGHT: out_direction = PaneDirection::Right; return true;
		case VK_UP:    out_direction = PaneDirection::Up;    return true;
		case VK_DOWN:  out_direction = PaneDirection::Down;  return true;
		default:       return false;
		}
	}

	static bool sameKey(const KeyPress& press, const KeyPress& binding) {
		return press.virtual_key == binding.virtual_key
			&& press.control == binding.control
			&& press.alt == binding.alt
			&& press.shift == binding.shift;
	}

	void TerminalWindow::focusNeighbour(PaneDirection direction) {
		PaneNode* leaf = panes_.neighbour(direction);
		if (leaf == nullptr) return;

		panes_.focusOn(leaf);
		syncTitle();
		::InvalidateRect(window_, nullptr, FALSE);
	}

	void TerminalWindow::focusNextPane() {
		const std::vector<PaneNode*> leaves = panes_.leaves();
		if (leaves.size() < 2) return;

		auto at = std::find(leaves.begin(), leaves.end(), panes_.focused());
		if (at == leaves.end()) return;

		++at;
		panes_.focusOn(at == leaves.end() ? leaves.front() : *at);
		syncTitle();
		::InvalidateRect(window_, nullptr, FALSE);
	}

	void TerminalWindow::toggleZoom() {
		panes_.toggleZoom();
		layoutPanes();
		commitGridSizes();
		::InvalidateRect(window_, nullptr, FALSE);
	}

	void TerminalWindow::runPaneCommand(wchar_t character) {
		switch (character) {
		case L'|':
		case L'%':
			splitFocused(SplitAxis::Columns);
			return;
		case L'"':
		case L'-':
			splitFocused(SplitAxis::Rows);
			return;
		case L'x': closeFocused(); return;
		case L'z': toggleZoom(); return;
		case L'o': focusNextPane(); return;
		case L'h': focusNeighbour(PaneDirection::Left); return;
		case L'j': focusNeighbour(PaneDirection::Down); return;
		case L'k': focusNeighbour(PaneDirection::Up); return;
		case L'l': focusNeighbour(PaneDirection::Right); return;
		default:   return;
		}
	}

	// tmux's shape: a prefix key, then one key that means a pane command.
	// The prefix twice over sends it on, so the shell can still see it.
	bool TerminalWindow::paneKeyTaken(const KeyPress& press) {
		if (!tmux_mode_) return false;

		if (!prefix_pending_) {
			if (!sameKey(press, prefix_)) return false;

			prefix_pending_ = true;
			swallow_next_char_ = true;
			return true;
		}

		if (isModifierKey(press.virtual_key)) return true;

		if (sameKey(press, prefix_)) {
			prefix_pending_ = false;
			return false;
		}

		PaneDirection direction = PaneDirection::Left;
		if (arrowDirection(press.virtual_key, direction)) {
			prefix_pending_ = false;
			focusNeighbour(direction);
			return true;
		}

		prefix_pending_ = mayCarryCharacter(press.virtual_key);
		return true;
	}

	bool TerminalWindow::paneCharacterTaken(wchar_t character) {
		if (!tmux_mode_ || !prefix_pending_) return false;

		prefix_pending_ = false;
		runPaneCommand(character);
		return true;
	}

	PaneCanvas TerminalWindow::canvasFor(const PaneNode& leaf) const {
		PaneCanvas canvas;
		canvas.target  = target_.Get();
		canvas.bounds  = leaf.bounds();
		canvas.screen  = &leaf.pane()->screen();
		canvas.view    = &leaf.pane()->view();
		canvas.focused = &leaf == panes_.focused();
		return canvas;
	}

	void TerminalWindow::paintPanes() {
		const std::vector<PaneNode*> leaves = panes_.leaves();

		for (PaneNode* leaf : leaves) {
			if (leaf->bounds().right <= leaf->bounds().left) continue;

			const PaneCanvas canvas = canvasFor(*leaf);
			renderer_.draw(canvas);
			renderer_.drawPicker(canvas, leaf->pane()->picker());
			leaf->pane()->screen().clearDirty();
		}

		for (const PaneDivider& divider : panes_.dividers()) {
			renderer_.drawDivider(target_.Get(), divider.bounds);
		}

		if (config_.panes.focus_border && leaves.size() > 1) {
			renderer_.drawFocusBorder(target_.Get(), panes_.focused()->bounds());
		}

		if (statusHeight() > 0.0f) {
			renderer_.drawStatusBar(target_.Get(), status_bounds_, statusLeft(),
				statusRight());
		}

		if (!customFrame()) return;

		TitleBarCanvas caption;
		caption.target = target_.Get();
		caption.bar    = &title_bar_;
		caption.title  = captionText();
		caption.zoomed = ::IsZoomed(window_) != 0;
		renderer_.drawTitleBar(caption);
	}

	PaneNode* TerminalWindow::leafFromMouse(LPARAM lparam) const {
		return panes_.leafAt(static_cast<float>(GET_X_LPARAM(lparam)),
			static_cast<float>(GET_Y_LPARAM(lparam)));
	}

	GridPoint TerminalWindow::pointInLeaf(const PaneNode& leaf, LPARAM lparam) const {
		const CellMetrics& cell = renderer_.metrics();
		const Screen& screen = leaf.pane()->screen();
		const float x = static_cast<float>(GET_X_LPARAM(lparam)) - leaf.bounds().left
			- renderer_.padding();
		const float y = static_cast<float>(GET_Y_LPARAM(lparam)) - leaf.bounds().top
			- renderer_.padding();

		const int column = std::min(std::max(static_cast<int>(x / cell.width), 0),
			screen.columns() - 1);
		const int viewport_row = std::min(std::max(static_cast<int>(y / cell.height), 0),
			screen.rows() - 1);

		GridPoint point;
		point.row    = leaf.pane()->view().topRow(screen) + viewport_row;
		point.column = column;
		return point;
	}

	bool TerminalWindow::beginDividerDrag(LPARAM lparam) {
		PaneNode* branch = panes_.dividerAt(static_cast<float>(GET_X_LPARAM(lparam)),
			static_cast<float>(GET_Y_LPARAM(lparam)), kDividerSlop);
		if (branch == nullptr) return false;

		dragging_ = branch;
		::SetCapture(window_);
		return true;
	}

	void TerminalWindow::continueDividerDrag(LPARAM lparam) {
		panes_.dragDivider(dragging_, static_cast<float>(GET_X_LPARAM(lparam)),
			static_cast<float>(GET_Y_LPARAM(lparam)));
		layoutPanes();
		::InvalidateRect(window_, nullptr, FALSE);
	}

	// The window class asks for an I-beam; over a divider it has to become
	// a sizing cursor or there is nothing telling the hands it can move.
	bool TerminalWindow::onSetCursor() {
		POINT where{};
		if (::GetCursorPos(&where) == 0 || ::ScreenToClient(window_, &where) == 0) return false;

		if (title_bar_.holdsPoint(static_cast<float>(where.x), static_cast<float>(where.y))
			|| where.y >= static_cast<int>(status_bounds_.top)) {
			::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
			return true;
		}

		const PaneNode* branch = dragging_ != nullptr
			? dragging_
			: panes_.dividerAt(static_cast<float>(where.x), static_cast<float>(where.y),
				kDividerSlop);
		if (branch == nullptr) return false;

		::SetCursor(::LoadCursorW(nullptr,
			branch->axis() == SplitAxis::Columns ? IDC_SIZEWE : IDC_SIZENS));
		return true;
	}

	bool TerminalWindow::create(const std::wstring& command_line, const Config& config,
			const std::wstring& config_path, std::string& out_error) {
		command_line_ = command_line;
		config_       = config;
		config_path_  = config_path;
		config_stamp_ = settingsStamp(config_path, config_.theme_name);

		if (!renderer_.create(config_, out_error)) {
			Config fallback = config_;
			fallback.font.family = L"Consolas";
			if (!renderer_.create(fallback, out_error)) return false;
			config_ = fallback;
		}

		if (!registerClass(out_error)) return false;
		if (!createWindow(out_error)) return false;
		if (!createTarget(out_error)) return false;

		parseKeyBinding(config_.panes.prefix, prefix_);

		auto first = std::make_unique<Pane>();
		Pane* only = first.get();
		panes_.adopt(std::move(first));
		layoutPanes();

		if (!startPane(*only, out_error)) return false;

		applyWindowSettings();

		::SetTimer(window_, kTimerBlink, kBlinkMs, nullptr);
		::SetTimer(window_, kTimerConfig, kConfigPollMs, nullptr);

		::ShowWindow(window_, SW_SHOW);
		::UpdateWindow(window_);
		return true;
	}

	// Opacity needs a layered window; setting it at 1.0 too would cost a
	// redirection surface for nothing.
	void TerminalWindow::applyWindowSettings() {
		const LONG_PTR style = ::GetWindowLongPtrW(window_, GWL_EXSTYLE);
		if (config_.window.opacity >= 0.999f) {
			::SetWindowLongPtrW(window_, GWL_EXSTYLE, style & ~WS_EX_LAYERED);
			return;
		}

		::SetWindowLongPtrW(window_, GWL_EXSTYLE, style | WS_EX_LAYERED);
		const float clamped = config_.window.opacity < 0.2f ? 0.2f : config_.window.opacity;
		::SetLayeredWindowAttributes(window_, 0, static_cast<BYTE>(clamped * 255.0f), LWA_ALPHA);
	}

	void TerminalWindow::reloadConfigIfChanged() {
		if (config_path_.empty()) return;

		const unsigned long long stamp = settingsStamp(config_path_, config_.theme_name);
		if (stamp == config_stamp_) return;
		config_stamp_ = stamp;

		Config reloaded;
		std::string error;
		if (!loadConfig(config_path_, reloaded, error)) return;

		const bool font_changed = reloaded.font.family != config_.font.family
			|| reloaded.font.size != config_.font.size
			|| reloaded.font.line_height != config_.font.line_height;

		config_ = reloaded;
		if (font_changed && !renderer_.create(config_, error)) return;

		renderer_.applyConfig(config_);
		applyFrameAppearance();
		parseKeyBinding(config_.panes.prefix, prefix_);
		applyScrollbackLimit();
		applyWindowSettings();
		onResize();
		::InvalidateRect(window_, nullptr, FALSE);
	}

	void TerminalWindow::onTimer(WPARAM timer) {
		if (timer == kTimerConfig) {
			reloadConfigIfChanged();
			return;
		}

		if (timer == kTimerResize) {
			commitGridSizes();
			::InvalidateRect(window_, nullptr, FALSE);
			return;
		}

		if (statusClockChanged()) ::InvalidateRect(window_, nullptr, FALSE);

		if (!config_.cursor.blink) {
			cursor_phase_ = true;
			return;
		}

		cursor_phase_ = !cursor_phase_;
		renderer_.setCursorVisible(cursor_phase_);
		::InvalidateRect(window_, nullptr, FALSE);
	}

	bool TerminalWindow::registerClass(std::string& out_error) {
		WNDCLASSEXW description{};
		description.cbSize        = sizeof(description);
		description.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
		description.lpfnWndProc   = &TerminalWindow::windowProc;
		description.hInstance     = ::GetModuleHandleW(nullptr);
		description.hCursor       = ::LoadCursorW(nullptr, IDC_IBEAM);
		description.lpszClassName = kClassName;

		if (::RegisterClassExW(&description) == 0
			&& ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
			out_error = "RegisterClassEx failed";
			return false;
		}

		return true;
	}

	bool TerminalWindow::createWindow(std::string& out_error) {
		const CellMetrics& cell = renderer_.metrics();
		const float pad = 2.0f * static_cast<float>(config_.window.padding);
		RECT wanted = { 0, 0,
			static_cast<LONG>(cell.width * static_cast<float>(config_.window.columns) + pad),
			static_cast<LONG>(cell.height * static_cast<float>(config_.window.rows) + pad) };

		// A custom frame puts the caption inside the client area, so the
		// window has to be that much taller to leave the grid its rows.
		if (customFrame()) wanted.bottom += static_cast<LONG>(titleHeight());
		else ::AdjustWindowRect(&wanted, WS_OVERLAPPEDWINDOW, FALSE);

		window_ = ::CreateWindowExW(0, kClassName, L"wbsh", WS_OVERLAPPEDWINDOW,
			CW_USEDEFAULT, CW_USEDEFAULT, wanted.right - wanted.left, wanted.bottom - wanted.top,
			nullptr, nullptr, ::GetModuleHandleW(nullptr), this);
		if (window_ == nullptr) {
			out_error = "CreateWindowEx failed";
			return false;
		}

		applyFrameAppearance();
		return true;
	}

	// The border DWM still draws around a custom frame is the one seam
	// left, so it takes the theme's own background and disappears into
	// it. The shadow needs a sliver of frame extended back under the
	// client area, or a window with no caption casts none.
	void TerminalWindow::applyFrameAppearance() {
		const BOOL dark = TRUE;
		::DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

		const std::uint32_t chrome = config_.palette.background;
		const COLORREF frame = RGB((chrome >> 16) & 0xFF, (chrome >> 8) & 0xFF, chrome & 0xFF);
		::DwmSetWindowAttribute(window_, DWMWA_CAPTION_COLOR, &frame, sizeof(frame));
		::DwmSetWindowAttribute(window_, DWMWA_BORDER_COLOR, &frame, sizeof(frame));

		const DWORD corners = kRoundSmall;
		dwm_rounds_corners_ = SUCCEEDED(::DwmSetWindowAttribute(window_, kCornerPreference,
			&corners, sizeof(corners)));

		if (!customFrame()) return;

		// The sliver of frame that buys a shadow is drawn by DWM in its own
		// colour, and the attribute that would recolour it is Windows 11's
		// too. On Windows 10 that leaves an accent-coloured hairline across
		// the top of a window that has no caption to justify it, so there
		// the frame stays where it is and the cut-out corners stand alone.
		const MARGINS shadow = { 0, 0, dwm_rounds_corners_ ? 1 : 0, 0 };
		::DwmExtendFrameIntoClientArea(window_, &shadow);
		::SetWindowPos(window_, nullptr, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
	}

	bool TerminalWindow::createTarget(std::string& out_error) {
		RECT client{};
		::GetClientRect(window_, &client);

		const D2D1_SIZE_U pixels = D2D1::SizeU(
			static_cast<UINT32>(client.right - client.left),
			static_cast<UINT32>(client.bottom - client.top));

		const HRESULT hr = renderer_.factory()->CreateHwndRenderTarget(
			D2D1::RenderTargetProperties(),
			D2D1::HwndRenderTargetProperties(window_, pixels),
			target_.ReleaseAndGetAddressOf());
		if (FAILED(hr)) {
			out_error = "CreateHwndRenderTarget failed";
			return false;
		}

		return true;
	}

	LRESULT CALLBACK TerminalWindow::windowProc(HWND window, UINT message, WPARAM wparam,
			LPARAM lparam) {
		if (message == WM_NCCREATE) {
			auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
			::SetWindowLongPtrW(window, GWLP_USERDATA,
				reinterpret_cast<LONG_PTR>(create->lpCreateParams));
			auto* self = static_cast<TerminalWindow*>(create->lpCreateParams);
			self->window_ = window;
		}

		auto* self = reinterpret_cast<TerminalWindow*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
		if (self == nullptr) return ::DefWindowProcW(window, message, wparam, lparam);

		return self->handleMessage(message, wparam, lparam);
	}

	LRESULT TerminalWindow::handleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
		switch (message) {
		case WM_PAINT:       onPaint(); return 0;
		case WM_SIZE:
			if (wparam != SIZE_MINIMIZED) onResize();
			return 0;
		case WM_CHAR:
		case WM_SYSCHAR:     onText(static_cast<wchar_t>(wparam)); return 0;
		case WM_KEYDOWN:
		case WM_SYSKEYDOWN:
			if (onKeyDown(wparam)) return 0;
			break;
		case WM_DPICHANGED:  onDpiChanged(wparam, lparam); return 0;
		case WM_MOUSEWHEEL:  onMouseWheel(wparam, lparam); return 0;
		case WM_EXITSIZEMOVE: commitGridSizes(); return 0;
		case WM_SETCURSOR:
			if (LOWORD(lparam) == HTCLIENT && onSetCursor()) return TRUE;
			break;
		case WM_LBUTTONDOWN:
		case WM_LBUTTONDBLCLK: onMouseDown(lparam); return 0;
		case WM_MOUSEMOVE:   onMouseMove(wparam, lparam); return 0;
		case WM_LBUTTONUP:   onMouseUp(lparam); return 0;
		case WM_NCCALCSIZE:
			if (customFrame()) return onCalcSize(wparam, lparam);
			break;
		case WM_NCHITTEST:
			if (customFrame()) return onHitTest(lparam);
			break;
		case WM_NCMOUSEMOVE:
		case WM_MOUSELEAVE: clearTitleHover(); break;
		case WM_ACTIVATE:
			setWindowActive(LOWORD(wparam) != WA_INACTIVE);
			break;
		case WM_CAPTURECHANGED: onCaptureLost(); return 0;
		case WM_CONTEXTMENU: onContextMenu(lparam); return 0;
		case WM_ERASEBKGND:  return 1;
		case WM_TIMER:       onTimer(wparam); return 0;
		case kMessagePtyData: onPtyData(); return 0;
		case WM_CLOSE:       ::DestroyWindow(window_); return 0;
		case WM_DESTROY:
			stopEveryPane();
			::PostQuitMessage(0);
			return 0;
		default:
			break;
		}

		return ::DefWindowProcW(window_, message, wparam, lparam);
	}

	// Two clicks in the same cell inside the double-click time select a
	// word, three select the line; Windows only tells us about the second.
	int TerminalWindow::clickCountAt(GridPoint point) {
		const DWORD now = ::GetTickCount();
		const bool same_place = point.row == last_click_.row && point.column == last_click_.column;
		const bool in_time = now - last_click_time_ <= ::GetDoubleClickTime();

		click_count_ = (same_place && in_time) ? click_count_ + 1 : 1;
		last_click_ = point;
		last_click_time_ = now;
		return click_count_;
	}

	void TerminalWindow::onMouseDown(LPARAM lparam) {
		if (titleBarTakesPress(lparam)) return;
		if (beginDividerDrag(lparam)) return;

		PaneNode* leaf = leafFromMouse(lparam);
		if (leaf == nullptr) return;

		panes_.focusOn(leaf);

		const GridPoint point = pointInLeaf(*leaf, lparam);
		const int clicks = clickCountAt(point);
		TerminalView& view = leaf->pane()->view();
		const Screen& screen = leaf->pane()->screen();

		if (clicks >= 3) view.selectLine(point, screen);
		else if (clicks == 2) view.selectWord(point, screen);
		else view.beginSelection(point);

		::SetCapture(window_);
		::InvalidateRect(window_, nullptr, FALSE);
	}

	void TerminalWindow::onMouseMove(WPARAM wparam, LPARAM lparam) {
		trackTitleHover(lparam);

		if ((wparam & MK_LBUTTON) == 0) return;
		if (title_bar_.pressed() != TitleButton::None) return;

		if (dragging_ != nullptr) {
			continueDividerDrag(lparam);
			return;
		}

		if (!focused().view().selecting()) return;

		focused().view().extendSelection(pointInLeaf(*panes_.focused(), lparam));
		::InvalidateRect(window_, nullptr, FALSE);
	}

	void TerminalWindow::onMouseUp(LPARAM lparam) {
		if (titleBarTakesRelease(lparam)) return;

		const bool was_dragging = dragging_ != nullptr;
		if (::GetCapture() == window_) ::ReleaseCapture();

		if (was_dragging) return;

		focused().view().endSelection();
	}

	// Releasing the capture raises this, and so does losing it to
	// something else mid-drag; either way the drag has to end here or
	// the divider keeps following the pointer and no pty is resized.
	void TerminalWindow::onCaptureLost() {
		if (dragging_ == nullptr) return;

		dragging_ = nullptr;
		commitGridSizes();
		::InvalidateRect(window_, nullptr, FALSE);
	}

	// The wheel turns the pane under the pointer, not the focused one:
	// reading a pane while another works is the reason for splitting.
	void TerminalWindow::onMouseWheel(WPARAM wparam, LPARAM lparam) {
		const int notches = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
		if (notches == 0) return;

		POINT where = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
		::ScreenToClient(window_, &where);

		PaneNode* leaf = panes_.leafAt(static_cast<float>(where.x),
			static_cast<float>(where.y));
		Pane& pane = leaf != nullptr ? *leaf->pane() : focused();

		pane.view().scrollBy(notches * kWheelLines, pane.screen());
		::InvalidateRect(window_, nullptr, FALSE);
	}

	// Ctrl+PageUp / Ctrl+PageDown walk the prompts the shell marked, which
	// is how a long scrollback becomes navigable rather than a wall.
	bool TerminalWindow::jumpToCommand(bool backwards) {
		const int row = focused().view().neighbouringCommandRow(focused().screen(), backwards);
		if (row < 0) return false;

		focused().view().scrollToRow(row, focused().screen());
		return true;
	}

	void TerminalWindow::copyLastCommandOutput() {
		const std::vector<CommandBlock>& blocks = focused().screen().commandBlocks();
		for (auto block = blocks.rbegin(); block != blocks.rend(); ++block) {
			if (!block->finished || block->output_row < 0) continue;

			focused().view().selectBlockOutput(*block, focused().screen());
			copySelection();
			::InvalidateRect(window_, nullptr, FALSE);
			return;
		}
	}

	void TerminalWindow::copySelection() {
		const std::string text = focused().view().selectedText(focused().screen());
		if (text.empty()) return;

		const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
			static_cast<int>(text.size()), nullptr, 0);
		if (needed <= 0 || !::OpenClipboard(window_)) return;

		::EmptyClipboard();
		const HGLOBAL block = ::GlobalAlloc(GMEM_MOVEABLE,
			(static_cast<std::size_t>(needed) + 1) * sizeof(wchar_t));
		if (block != nullptr) {
			auto* wide = static_cast<wchar_t*>(::GlobalLock(block));
			::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
				wide, needed);
			wide[needed] = L'\0';
			::GlobalUnlock(block);
			::SetClipboardData(CF_UNICODETEXT, block);
		}

		::CloseClipboard();
	}

	// Scrolling and copying belong to the window, not the shell, so these
	// are taken before the key encoder ever sees them.
	bool TerminalWindow::handleViewShortcut(const KeyPress& press) {
		const int page = std::max(1, focused().screen().rows() - 1);

		if (press.control && !press.shift && press.virtual_key == VK_PRIOR) {
			if (!jumpToCommand(true)) return true;
		} else if (press.control && !press.shift && press.virtual_key == VK_NEXT) {
			if (!jumpToCommand(false)) return true;
		} else if (press.shift && !press.control && press.virtual_key == VK_PRIOR) {
			focused().view().scrollBy(page, focused().screen());
		} else if (press.shift && !press.control && press.virtual_key == VK_NEXT) {
			focused().view().scrollBy(-page, focused().screen());
		} else if (press.shift && press.control && press.virtual_key == VK_UP) {
			focused().view().scrollBy(1, focused().screen());
		} else if (press.shift && press.control && press.virtual_key == VK_DOWN) {
			focused().view().scrollBy(-1, focused().screen());
		} else if (press.control && !press.alt
				&& (press.virtual_key == VK_OEM_PLUS || press.virtual_key == VK_ADD
					|| press.virtual_key == VK_OEM_MINUS || press.virtual_key == VK_SUBTRACT
					|| press.virtual_key == '0')) {
			stepFontSize(press.virtual_key);
			return true;
		} else if (press.control && press.shift && press.virtual_key == 'C') {
			copySelection();
			return true;
		} else if (press.control && press.virtual_key == VK_INSERT) {
			copySelection();
			return true;
		} else {
			return false;
		}

		::InvalidateRect(window_, nullptr, FALSE);
		return true;
	}

	// The menu is where customising lives for anyone who does not want to
	// open a config file; every choice is written back to that file so it
	// still applies tomorrow.
	// A caption of our own drops the system menu that comes with a real
	// one, and Move, Size and the keyboard ways to close go with it. Alt+Space
	// is deliberately left alone: readline binds it, and a terminal that
	// swallowed it would be taking a key its shell was already using.
	void TerminalWindow::showSystemMenu(int screen_x, int screen_y) {
		HMENU menu = ::GetSystemMenu(window_, FALSE);
		if (menu == nullptr) return;

		const bool zoomed = ::IsZoomed(window_) != 0;
		const UINT when_zoomed = zoomed ? MF_ENABLED : (MF_DISABLED | MF_GRAYED);
		const UINT when_normal = zoomed ? (MF_DISABLED | MF_GRAYED) : MF_ENABLED;

		::EnableMenuItem(menu, SC_RESTORE, MF_BYCOMMAND | when_zoomed);
		::EnableMenuItem(menu, SC_MOVE, MF_BYCOMMAND | when_normal);
		::EnableMenuItem(menu, SC_SIZE, MF_BYCOMMAND | when_normal);
		::EnableMenuItem(menu, SC_MAXIMIZE, MF_BYCOMMAND | when_normal);
		::EnableMenuItem(menu, SC_MINIMIZE, MF_BYCOMMAND | MF_ENABLED);
		::EnableMenuItem(menu, SC_CLOSE, MF_BYCOMMAND | MF_ENABLED);

		const int command = ::TrackPopupMenu(menu,
			TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, screen_x, screen_y, 0,
			window_, nullptr);
		if (command == 0) return;

		::PostMessageW(window_, WM_SYSCOMMAND, static_cast<WPARAM>(command), 0);
	}

	bool TerminalWindow::pointIsOnCaption(int screen_x, int screen_y) const {
		if (!customFrame()) return false;

		POINT where = { screen_x, screen_y };
		if (::ScreenToClient(window_, &where) == 0) return false;

		return title_bar_.holdsPoint(static_cast<float>(where.x), static_cast<float>(where.y));
	}

	void TerminalWindow::onContextMenu(LPARAM lparam) {
		POINT where = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
		if (pointIsOnCaption(where.x, where.y)) {
			showSystemMenu(where.x, where.y);
			return;
		}

		if (where.x == -1 && where.y == -1) {
			RECT client{};
			::GetClientRect(window_, &client);
			where.x = client.right / 2;
			where.y = client.bottom / 2;
			::ClientToScreen(window_, &where);
		}

		const std::vector<std::string> themes =
			availableThemeNames(themesDirectory(config_path_));

		HMENU menu = buildTerminalMenu(config_, themes, focused().view().hasSelection(),
			!focused().screen().commandBlocks().empty());
		const int command = ::TrackPopupMenu(menu,
			TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, where.x, where.y, 0, window_, nullptr);
		::DestroyMenu(menu);

		if (command != 0) runMenuChoice(menuChoiceFor(command, themes));
	}

	void TerminalWindow::runMenuChoice(const MenuChoice& choice) {
		if (choice.action == MenuAction::Copy) {
			copySelection();
			return;
		}

		if (choice.action == MenuAction::Paste) {
			pasteFromClipboard();
			return;
		}

		if (choice.action == MenuAction::OpenConfigFile) {
			openConfigFile();
			return;
		}

		if (choice.action == MenuAction::OpenThemesFolder) {
			openThemesFolder();
			return;
		}

		if (choice.action == MenuAction::CopyLastOutput) {
			copyLastCommandOutput();
			return;
		}

		if (!applyMenuChoice(choice, themesDirectory(config_path_), config_)) return;

		std::string section;
		std::string key;
		std::string value;
		if (!config_path_.empty() && settingForChoice(choice, config_, section, key, value)) {
			updateConfigValue(config_path_, section, key, value);
			config_stamp_ = settingsStamp(config_path_, config_.theme_name);
		}

		applyChangedConfig(choice.action == MenuAction::SetFontSize);
	}

	void TerminalWindow::applyChangedConfig(bool font_changed) {
		std::string error;
		if (font_changed && !renderer_.create(config_, error)) return;

		renderer_.applyConfig(config_);
		renderer_.setCursorVisible(true);
		cursor_phase_ = true;
		applyWindowSettings();
		onResize();
		::InvalidateRect(window_, nullptr, FALSE);
	}

	void TerminalWindow::openThemesFolder() {
		const std::wstring directory = themesDirectory(config_path_);
		if (!ensureThemesDirectory(directory)) return;

		::ShellExecuteW(window_, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	}

	void TerminalWindow::openConfigFile() {
		if (config_path_.empty()) return;
		if (configStamp(config_path_) == 0) writeDefaultConfig(config_path_);

		::ShellExecuteW(window_, L"open", config_path_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	}

	void TerminalWindow::stepFontSize(unsigned int virtual_key) {
		const float previous = config_.font.size;

		if (virtual_key == '0') config_.font.size = 11.0f;
		else if (virtual_key == VK_OEM_MINUS || virtual_key == VK_SUBTRACT) {
			config_.font.size -= 1.0f;
		} else {
			config_.font.size += 1.0f;
		}

		config_.font.size = std::min(std::max(config_.font.size, 6.0f), 48.0f);
		if (config_.font.size == previous) return;

		if (!config_path_.empty()) {
			updateConfigValue(config_path_, "font", "size",
				std::to_string(static_cast<int>(config_.font.size)));
			config_stamp_ = settingsStamp(config_path_, config_.theme_name);
		}

		applyChangedConfig(true);
	}

	void TerminalWindow::onPaint() {
		PAINTSTRUCT paint{};
		::BeginPaint(window_, &paint);

		if (target_) {
			target_->BeginDraw();
			paintPanes();
			if (target_->EndDraw() == D2DERR_RECREATE_TARGET) {
				std::string ignored;
				createTarget(ignored);
			}
		}

		::EndPaint(window_, &paint);
	}

	// A minimised window reports a client area of nothing, and laying
	// panes out into nothing collapses every grid to a single cell --
	// which the commit would then hand to the shells, reflowing them to
	// one column behind a window nobody can see.
	void TerminalWindow::onResize() {
		if (!target_) return;

		RECT client{};
		::GetClientRect(window_, &client);
		if (client.right <= client.left || client.bottom <= client.top) return;

		target_->Resize(D2D1::SizeU(
			static_cast<UINT32>(client.right - client.left),
			static_cast<UINT32>(client.bottom - client.top)));

		applyCornerRegion();
		layoutPanes();
		scheduleGridCommit();
		::InvalidateRect(window_, nullptr, FALSE);
	}

	void TerminalWindow::onDpiChanged(WPARAM wparam, LPARAM lparam) {
		const UINT dpi = LOWORD(wparam);
		if (target_) target_->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));

		const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
		::SetWindowPos(window_, nullptr, suggested->left, suggested->top,
			suggested->right - suggested->left, suggested->bottom - suggested->top,
			SWP_NOZORDER | SWP_NOACTIVATE);
	}

	void TerminalWindow::onPtyData() {
		bool changed = false;
		for (PaneNode* leaf : panes_.leaves()) {
			if (!leaf->pane()->session().drainOutput()) continue;

			leaf->pane()->view().followOutput(leaf->pane()->screen());
			changed = true;
		}

		if (changed) {
			syncTitle();
			::InvalidateRect(window_, nullptr, FALSE);
		}

		closeExitedPanes();
	}

	// Collected first and closed after, because closing a pane destroys
	// the node the loop would otherwise still be standing on.
	void TerminalWindow::closeExitedPanes() {
		std::vector<PaneNode*> finished;
		for (PaneNode* leaf : panes_.leaves()) {
			if (!leaf->pane()->session().childRunning()) finished.push_back(leaf);
		}

		if (finished.empty()) return;

		for (PaneNode* leaf : finished) {
			leaf->pane()->session().drainOutput();
			if (panes_.close(leaf)) continue;

			::DestroyWindow(window_);
			return;
		}

		layoutPanes();
		commitGridSizes();
		syncTitle();
		::InvalidateRect(window_, nullptr, FALSE);
	}

	void TerminalWindow::syncTitle() {
		const std::string& directory = focused().screen().workingDirectory();
		const std::string& title = directory.empty()
			? focused().screen().title()
			: directory;
		if (title.empty()) return;

		const int needed = ::MultiByteToWideChar(CP_UTF8, 0, title.c_str(),
			static_cast<int>(title.size()), nullptr, 0);
		if (needed <= 0) return;

		std::wstring wide(static_cast<std::size_t>(needed), L'\0');
		::MultiByteToWideChar(CP_UTF8, 0, title.c_str(), static_cast<int>(title.size()),
			wide.data(), needed);
		const std::size_t count = panes_.leaves().size();
		if (count > 1) wide += L"  [" + std::to_wstring(count) + L" panes]";
		if (wide == shown_title_) return;

		shown_title_ = wide;
		::SetWindowTextW(window_, wide.c_str());
		if (customFrame()) ::InvalidateRect(window_, nullptr, FALSE);
	}

	KeyModes TerminalWindow::currentModes() const {
		KeyModes modes;
		modes.application_cursor = focused().screen().applicationCursorKeys();
		modes.bracketed_paste    = focused().screen().bracketedPaste();
		return modes;
	}

	// Alt-held characters arrive as WM_SYSCHAR and go out ESC-prefixed,
	// which is how a terminal spells Meta.
	void TerminalWindow::onText(wchar_t character) {
		if (swallow_next_char_) {
			swallow_next_char_ = false;
			return;
		}

		if (paneCharacterTaken(character)) return;

		if (focused().picker().active()) {
			focused().picker().typeCharacter(character);
			::InvalidateRect(window_, nullptr, FALSE);
			return;
		}

		std::string bytes = encodeUtf8(character);
		if (bytes.empty()) return;

		if (keyIsDown(VK_MENU)) bytes.insert(bytes.begin(), '\x1b');
		sendBytes(bytes.data(), bytes.size());
	}

	// The shell is blocked waiting for a line, so every answer ends with a
	// carriage return; an empty one means the user backed out.
	void TerminalWindow::answerPick(const std::string& choice) {
		focused().picker().cancel();

		const std::string reply = choice + "\r";
		focused().session().writeInput(reply.data(), reply.size());
		::InvalidateRect(window_, nullptr, FALSE);
	}

	bool TerminalWindow::pickerTakesKey(WPARAM key) {
		if (!focused().picker().active()) return false;

		switch (key) {
		case VK_RETURN: answerPick(focused().picker().chosen()); return true;
		case VK_ESCAPE: answerPick(std::string()); return true;
		case VK_UP:     focused().picker().moveSelection(-1); break;
		case VK_DOWN:   focused().picker().moveSelection(1); break;
		case VK_PRIOR:  focused().picker().moveSelection(-10); break;
		case VK_NEXT:   focused().picker().moveSelection(10); break;
		case VK_BACK:   focused().picker().backspace(); break;
		default:        return false;
		}

		::InvalidateRect(window_, nullptr, FALSE);
		return true;
	}

	bool TerminalWindow::onKeyDown(WPARAM key) {
		if (pickerTakesKey(key)) {
			swallow_next_char_ = true;
			return true;
		}

		const KeyPress press = currentKeyPress(key);
		swallow_next_char_ = false;

		if (paneKeyTaken(press)) return true;

		if (handleViewShortcut(press)) {
			swallow_next_char_ = alsoProducesCharacter(press);
			return true;
		}

		if (isPasteShortcut(press)) {
			pasteFromClipboard();
			swallow_next_char_ = alsoProducesCharacter(press);
			return true;
		}

		const std::string bytes = encodeKeyPress(press, currentModes());
		if (bytes.empty()) return false;

		sendBytes(bytes.data(), bytes.size());
		swallow_next_char_ = alsoProducesCharacter(press);
		return true;
	}

	void TerminalWindow::pasteFromClipboard() {
		const std::string text = clipboardText();
		if (text.empty()) return;

		const std::string bytes = encodePaste(text, currentModes());
		sendBytes(bytes.data(), bytes.size());
	}

	void TerminalWindow::sendBytes(const char* data, std::size_t length) {
		if (length == 0) return;

		focused().view().scrollToBottom();
		focused().view().clearSelection();
		cursor_phase_ = true;
		renderer_.setCursorVisible(true);
		::InvalidateRect(window_, nullptr, FALSE);
		focused().session().writeInput(data, length);
	}

	int TerminalWindow::runMessageLoop() {
		MSG message{};
		while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
			::TranslateMessage(&message);
			::DispatchMessageW(&message);
		}

		return static_cast<int>(message.wParam);
	}

} /* namespace wbshterm */

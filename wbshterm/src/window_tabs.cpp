/**
 * @file window_tabs.cpp
 * @brief The window's tabs: opening, closing and switching them, the strip
 *        that shows them, and the keys and clicks that drive it.
 *
 * Every tab is laid out over the same client area and every pane in every
 * tab is read as its output arrives; only the shown tab is painted and
 * takes the keyboard and the mouse.
 */

#include "window.h"

#include "utf8.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

namespace wbshterm {

	namespace tab_detail {

		static const float kBandHeight   = 36.0f;
		static const float kBandTopInset = 6.0f;

		static const int kMenuNewTab      = 1;
		static const int kMenuDuplicate   = 2;
		static const int kMenuCloseRight  = 3;
		static const int kMenuCloseOthers = 4;
		static const int kMenuClose       = 5;

		static bool keyHeld(int virtual_key) {
			return (::GetKeyState(virtual_key) & 0x8000) != 0;
		}

		static bool isSlash(char letter) {
			return letter == '/' || letter == '\\';
		}

		// The folder the shell is in names its tab, as it names the caption;
		// a program's own title stands in until the shell has said where it is.
		static std::string tabName(const Screen& screen) {
			std::string directory = screen.workingDirectory();
			while (directory.size() > 1 && isSlash(directory.back())) directory.pop_back();

			const std::size_t cut = directory.find_last_of("/\\");
			std::string name = cut == std::string::npos ? directory : directory.substr(cut + 1);
			if (name.empty()) name = directory;
			if (name.empty()) name = screen.title();

			return name.empty() ? "wbsh" : name;
		}

		// On a layout with AltGr, Ctrl+Alt and a digit is also the layout's
		// dead key, already waiting for its letter by the time the key
		// reaches the window; a space spends it, so the next letter typed
		// does not come out accented.
		static void spendDeadKey() {
			BYTE state[256] = {};
			wchar_t text[8] = {};
			::ToUnicode(VK_SPACE, ::MapVirtualKeyW(VK_SPACE, MAPVK_VK_TO_VSC), state, text,
				static_cast<int>(std::size(text)), 0);
		}

		static HMENU buildTabMenu(std::size_t index, std::size_t count) {
			const UINT after  = index + 1 < count ? MF_ENABLED : MF_GRAYED;
			const UINT others = count > 1 ? MF_ENABLED : MF_GRAYED;

			HMENU menu = ::CreatePopupMenu();
			::AppendMenuW(menu, MF_STRING, kMenuNewTab, L"New tab\tCtrl+Shift+T");
			::AppendMenuW(menu, MF_STRING, kMenuDuplicate, L"Duplicate tab\tCtrl+Shift+D");
			::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
			::AppendMenuW(menu, MF_STRING | after, kMenuCloseRight, L"Close tabs to the right");
			::AppendMenuW(menu, MF_STRING | others, kMenuCloseOthers, L"Close other tabs");
			::AppendMenuW(menu, MF_STRING, kMenuClose, L"Close tab");
			return menu;
		}

	} /* namespace tab_detail */

	// Messages arrive while CreateWindowEx is still running, before the
	// first tab exists; until then the window answers with no panes at all.
	PaneTree& TerminalWindow::panes() {
		return tabs_.empty() ? no_panes_ : tabs_.active().panes;
	}

	const PaneTree& TerminalWindow::panes() const {
		return tabs_.empty() ? no_panes_ : tabs_.active().panes;
	}

	std::vector<PaneNode*> TerminalWindow::everyLeaf() const {
		std::vector<PaneNode*> leaves;

		for (std::size_t index = 0; index < tabs_.count(); ++index) {
			const std::vector<PaneNode*> tab = tabs_.at(index).panes.leaves();
			leaves.insert(leaves.end(), tab.begin(), tab.end());
		}

		return leaves;
	}

	// Turning tabs off leaves a window that already has several showing
	// them, so none of its sessions drops out of sight.
	bool TerminalWindow::tabStripShown() const {
		return config_.tabs.enabled || tabs_.count() > 1;
	}

	// The custom caption has room for the tabs; the system frame does not,
	// so there they get a band of their own above the grid.
	float TerminalWindow::tabBandHeight() const {
		return tabStripShown() && !customFrame() ? tab_detail::kBandHeight : 0.0f;
	}

	void TerminalWindow::layoutTabStrip(const D2D1_SIZE_F& size) {
		TabStripMetrics metrics;
		D2D1_RECT_F bounds{};

		if (tabStripShown() && customFrame()) {
			const float lights = title_bar_.clusterWidth();
			bounds = config_.titlebar.on_right
				? D2D1::RectF(0.0f, 0.0f, size.width - lights, titleHeight())
				: D2D1::RectF(lights, 0.0f, size.width, titleHeight());
		} else if (tabStripShown()) {
			metrics.top_inset = tab_detail::kBandTopInset;
			bounds = D2D1::RectF(0.0f, 0.0f, size.width, tabBandHeight());
		}

		tab_strip_.applyMetrics(metrics);
		tab_strip_.setBounds(bounds);
		syncTabStrip();
	}

	void TerminalWindow::syncTabStrip() {
		tab_strip_.setTabs(static_cast<int>(tabs_.count()), static_cast<int>(tabs_.activeIndex()));
	}

	std::vector<std::string> TerminalWindow::tabNames() const {
		std::vector<std::string> names;

		for (std::size_t index = 0; index < tabs_.count(); ++index) {
			const PaneNode* leaf = tabs_.at(index).panes.focused();
			names.push_back(leaf != nullptr ? tab_detail::tabName(leaf->pane()->screen()) : "wbsh");
		}

		return names;
	}

	bool TerminalWindow::tabNamesChanged() const {
		return tabStripShown() && tabNames() != shown_tab_names_;
	}

	void TerminalWindow::paintTabStrip() {
		if (!tabStripShown()) return;

		TabStripCanvas canvas;
		canvas.target        = target_.Get();
		canvas.strip         = &tab_strip_;
		canvas.backdrop      = customFrame() ? D2D1_RECT_F{} : tab_strip_.bounds();
		canvas.window_active = title_bar_.active();

		shown_tab_names_ = tabNames();
		for (const std::string& name : shown_tab_names_) canvas.titles.push_back(widenUtf8(name));

		renderer_.drawTabStrip(canvas);
	}

	// A tab is laid out before its shell starts, as a split is, so the
	// first prompt is drawn at the width it keeps. The window's first tab
	// stays even when its shell would not start: it is all there is.
	bool TerminalWindow::openTab(const std::wstring& directory, std::size_t index,
			std::string& out_error) {
		auto tab = std::make_unique<Tab>();
		auto first = std::make_unique<Pane>();
		Pane* only = first.get();
		tab->panes.adopt(std::move(first));

		leaveShownTab();
		tabs_.insert(index, std::move(tab));
		layoutPanes();

		directory_hint_ = directory;
		if (startPane(*only, out_error)) return true;
		if (tabs_.count() == 1) return false;

		discardTab(tabs_.activeIndex());
		layoutPanes();
		return false;
	}

	// A new tab starts where a new window would; duplicating one is how a
	// tab starts where its shell is.
	void TerminalWindow::openNewTab() {
		if (!config_.tabs.enabled) return;

		std::string error;
		openTab(std::wstring(), tabs_.count(), error);
		showActiveTab();
	}

	void TerminalWindow::openDuplicateTab(std::size_t index) {
		if (!config_.tabs.enabled || index >= tabs_.count()) return;

		const PaneNode* leaf = tabs_.at(index).panes.focused();
		const std::wstring directory = leaf != nullptr
			? nativeDirectory(leaf->pane()->screen().workingDirectory())
			: std::wstring();

		std::string error;
		openTab(directory, index + 1, error);
		showActiveTab();
	}

	// The last tab is not taken out: the window closes around it, so the
	// session file can still name it.
	void TerminalWindow::closeTabAt(std::size_t index) {
		if (index >= tabs_.count()) return;

		if (tabs_.count() == 1) {
			::DestroyWindow(window_);
			return;
		}

		discardTab(index);
		showActiveTab();
	}

	void TerminalWindow::closeTabsExcept(std::size_t kept) {
		if (kept >= tabs_.count()) return;

		for (std::size_t index = tabs_.count(); index-- > 0;) {
			if (index != kept) discardTab(index);
		}

		showActiveTab();
	}

	void TerminalWindow::closeTabsAfter(std::size_t index) {
		while (tabs_.count() > index + 1) discardTab(tabs_.count() - 1);

		showActiveTab();
	}

	// The retained grids go first, while their screens still exist; the
	// shells stop as the tab is destroyed. A drag in progress ends, since
	// the place it started from may no longer hold the same tab.
	void TerminalWindow::discardTab(std::size_t index) {
		if (index >= tabs_.count()) return;
		if (index == tabs_.activeIndex()) leaveShownTab();

		endTabDrag();

		for (PaneNode* leaf : tabs_.at(index).panes.leaves()) {
			renderer_.forgetGrid(&leaf->pane()->screen());
		}

		tabs_.remove(index);
	}

	void TerminalWindow::activateTab(std::size_t index) {
		if (index >= tabs_.count() || index == tabs_.activeIndex()) return;

		leaveShownTab();
		tabs_.activate(index);
		showActiveTab();
	}

	void TerminalWindow::stepTab(int delta) {
		if (tabs_.count() < 2) return;

		leaveShownTab();
		tabs_.step(delta);
		showActiveTab();
	}

	void TerminalWindow::shiftTab(int delta) {
		const long long last = static_cast<long long>(tabs_.count()) - 1;
		const long long wanted = static_cast<long long>(tabs_.activeIndex()) + delta;
		const long long target = std::min(std::max(wanted, 0LL), last);

		tabs_.move(tabs_.activeIndex(), static_cast<std::size_t>(target));
		syncTabStrip();
		::InvalidateRect(window_, nullptr, FALSE);
	}

	// What belongs to the tab being left behind: keys its program was told
	// are down come back up, a search in it stops, and the mouse lets go of
	// its panes.
	void TerminalWindow::leaveShownTab() {
		if (tabs_.empty()) return;

		releaseHeldKeys();
		search_.close();
		forgetMouseReporting();
		dragging_        = nullptr;
		scrolling_       = nullptr;
		scrollbar_hover_ = nullptr;
		motion_leaf_     = nullptr;
		wheel_pane_      = nullptr;
		menu_row_        = -1;
	}

	// Every tab is already laid out at the window's size, so showing one
	// resizes no pseudoconsole unless the strip itself came or went.
	void TerminalWindow::showActiveTab() {
		layoutPanes();
		commitGridSizes();
		syncTitle();
		accessibility_.textChanged();
		::InvalidateRect(window_, nullptr, FALSE);
	}

	// Windows Terminal's keys. Ctrl+Shift+W closes the focused pane, which
	// is the whole tab until it has been split.
	bool TerminalWindow::tabKeyTaken(const KeyPress& press) {
		if (!tabStripShown()) return false;
		if (classicTabKeyTaken(press)) return true;

		const bool control_shift = press.control && press.shift && !press.alt;
		std::size_t number = 0;

		if (control_shift && press.virtual_key == 'T') {
			openNewTab();
		} else if (control_shift && press.virtual_key == 'D') {
			openDuplicateTab(tabs_.activeIndex());
		} else if (control_shift && press.virtual_key == 'W') {
			closeFocused();
		} else if (press.control && !press.alt && press.virtual_key == VK_TAB) {
			stepTab(press.shift ? -1 : 1);
		} else if (control_shift && press.virtual_key == VK_PRIOR) {
			shiftTab(-1);
		} else if (control_shift && press.virtual_key == VK_NEXT) {
			shiftTab(1);
		} else if (tabNumberKey(press, number)) {
			tab_detail::spendDeadKey();
			activateTab(number);
		} else {
			return false;
		}

		return true;
	}

	// A browser's keys: Ctrl+T, Ctrl+W, and Ctrl and a digit, with 9 the
	// last tab however many there are. AltGr is Ctrl+Alt, so it never
	// reaches here.
	bool TerminalWindow::classicTabKeyTaken(const KeyPress& press) {
		if (!config_.tabs.classic_keys) return false;
		if (!press.control || press.shift || press.alt) return false;

		if (press.virtual_key == 'T') {
			openNewTab();
			return true;
		}

		if (press.virtual_key == 'W') {
			closeFocused();
			return true;
		}

		if (press.virtual_key < '1' || press.virtual_key > '9') return false;

		const std::size_t number = static_cast<std::size_t>(press.virtual_key - '1');
		activateTab(press.virtual_key == '9' ? tabs_.count() - 1 : number);
		return true;
	}

	// Ctrl+Alt and a digit, but never with the right Alt where that is
	// AltGr: there the same chord types the layout's own character.
	bool TerminalWindow::tabNumberKey(const KeyPress& press, std::size_t& out_index) const {
		if (!press.control || !press.alt || press.shift) return false;
		if (press.virtual_key < '1' || press.virtual_key > '9') return false;

		const bool altgr = tab_detail::keyHeld(VK_RMENU)
			&& config_.keyboard.right_alt == RightAltRole::AltGr;
		if (altgr) return false;

		out_index = static_cast<std::size_t>(press.virtual_key - '1');
		return true;
	}

	// tmux's windows are these tabs: c opens one, n and p walk them, a
	// digit picks one counting from zero, and & closes the shown one.
	bool TerminalWindow::runTabCommand(char32_t character) {
		if (character >= U'0' && character <= U'9') {
			activateTab(static_cast<std::size_t>(character - U'0'));
			return true;
		}

		switch (character) {
		case U'c': openNewTab(); return true;
		case U'n': stepTab(1); return true;
		case U'p': stepTab(-1); return true;
		case U'&': closeTabAt(tabs_.activeIndex()); return true;
		default:   return false;
		}
	}

	// A tab is shown the moment it is pressed, and dragging it from there
	// moves it along the strip. The close mark and the new-tab button act
	// on release, so a press on them can be taken back by sliding off.
	bool TerminalWindow::tabStripTakesPress(LPARAM lparam) {
		const D2D1_POINT_2F point = mouseDips(lparam);
		if (!tab_strip_.holdsPoint(point.x, point.y)) return false;

		const TabHit hit = tab_strip_.hitAt(point.x, point.y);
		if (hit.part == TabPart::None) return true;

		if (hit.part == TabPart::Tab) {
			activateTab(static_cast<std::size_t>(hit.index));
			tab_dragging_ = hit.index;
		}

		tab_strip_.setPressed(hit);
		::SetCapture(window_);
		::InvalidateRect(window_, nullptr, FALSE);
		return true;
	}

	bool TerminalWindow::tabStripTakesRelease(LPARAM lparam) {
		const TabHit pressed = tab_strip_.pressed();
		if (pressed.part == TabPart::None) return false;

		endTabDrag();
		if (::GetCapture() == window_) ::ReleaseCapture();
		::InvalidateRect(window_, nullptr, FALSE);

		const D2D1_POINT_2F point = mouseDips(lparam);
		if (tab_strip_.hitAt(point.x, point.y) == pressed) runTabButton(pressed);

		return true;
	}

	void TerminalWindow::runTabButton(const TabHit& hit) {
		if (hit.part == TabPart::Close) closeTabAt(static_cast<std::size_t>(hit.index));
		if (hit.part == TabPart::New) openNewTab();
	}

	bool TerminalWindow::tabStripTakesMotion(WPARAM wparam, LPARAM lparam) {
		if (tab_strip_.pressed().part == TabPart::None) return false;

		if (tab_dragging_ >= 0 && (wparam & MK_LBUTTON) != 0) continueTabDrag(lparam);
		return true;
	}

	// The tab under the pointer trades places with the one being dragged,
	// which then sits under the pointer itself, so the strip settles.
	void TerminalWindow::continueTabDrag(LPARAM lparam) {
		const int slot = tab_strip_.slotAt(mouseDips(lparam).x);
		if (slot == tab_dragging_) return;

		tabs_.move(static_cast<std::size_t>(tab_dragging_), static_cast<std::size_t>(slot));
		tab_dragging_ = slot;
		tab_strip_.setPressed(TabHit{ TabPart::Tab, slot });
		syncTabStrip();
		::InvalidateRect(window_, nullptr, FALSE);
	}

	void TerminalWindow::endTabDrag() {
		tab_dragging_ = -1;
		tab_strip_.setPressed(TabHit());
	}

	// The middle button closes the tab under it, as it does in a browser.
	bool TerminalWindow::tabStripTakesMiddle(LPARAM lparam) {
		const D2D1_POINT_2F point = mouseDips(lparam);
		const TabHit hit = tab_strip_.hitAt(point.x, point.y);

		if (hit.part == TabPart::Tab || hit.part == TabPart::Close) {
			closeTabAt(static_cast<std::size_t>(hit.index));
			return true;
		}

		return tab_strip_.holdsPoint(point.x, point.y);
	}

	void TerminalWindow::trackTabHover(LPARAM lparam) {
		const D2D1_POINT_2F point = mouseDips(lparam);
		const TabHit hit = tab_strip_.hitAt(point.x, point.y);
		if (!tab_strip_.setHovered(hit)) return;

		if (hit.part != TabPart::None) armMouseLeave();
		::InvalidateRect(window_, nullptr, FALSE);
	}

	void TerminalWindow::clearTabHover() {
		if (!tab_strip_.setHovered(TabHit())) return;

		::InvalidateRect(window_, nullptr, FALSE);
	}

	// A menu opened from the keyboard arrives at -1,-1 and belongs to the
	// terminal, not to whichever tab that point happens to fall on.
	bool TerminalWindow::tabMenuTaken(int screen_x, int screen_y) {
		if (screen_x == -1 && screen_y == -1) return false;

		POINT where = { screen_x, screen_y };
		if (::ScreenToClient(window_, &where) == 0) return false;

		const D2D1_POINT_2F point = dipsOf(where);
		const TabHit hit = tab_strip_.hitAt(point.x, point.y);
		if (hit.part != TabPart::Tab && hit.part != TabPart::Close) return false;

		const auto index = static_cast<std::size_t>(hit.index);
		HMENU menu = tab_detail::buildTabMenu(index, tabs_.count());
		const UINT flags = TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY;
		const int command = ::TrackPopupMenu(menu, flags, screen_x, screen_y, 0, window_,
			nullptr);
		::DestroyMenu(menu);

		runTabMenuCommand(command, index);
		return true;
	}

	// A shell that ended while the menu was open may have taken its tab
	// with it, so the index is checked again before anything acts on it.
	void TerminalWindow::runTabMenuCommand(int command, std::size_t index) {
		if (index >= tabs_.count()) return;

		switch (command) {
		case tab_detail::kMenuNewTab:      openNewTab(); return;
		case tab_detail::kMenuDuplicate:   openDuplicateTab(index); return;
		case tab_detail::kMenuCloseRight:  closeTabsAfter(index); return;
		case tab_detail::kMenuCloseOthers: closeTabsExcept(index); return;
		case tab_detail::kMenuClose:       closeTabAt(index); return;
		default:                           return;
		}
	}

} /* namespace wbshterm */

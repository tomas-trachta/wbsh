#pragma once

/**
 * @file window.h
 * @brief The terminal window: pty bytes in, painted grid out.
 */

#include "config.h"
#include "keymap.h"
#include "menu.h"
#include "mouse.h"
#include "pane_tree.h"
#include "render.h"
#include "script.h"
#include "search.h"
#include "scrollbar.h"
#include "sysinfo.h"
#include "uia_host.h"
#include "utf8.h"
#include "view.h"

#include <bitset>
#include <memory>
#include <string>
#include <vector>

namespace wbshterm {

	/**
	 * @brief One window hosting a tree of panes, each with its own shell.
	 *
	 * Everything except the pty read happens on the thread that calls
	 * create(): the reader posts a message and this class drains the
	 * bytes, so the grid is only ever touched here.
	 */
	class TerminalWindow : public TmuxHandler, public ScriptActions, public UiaHost {
	public:
		bool create(const std::wstring& command_line, const Config& config,
			const std::wstring& config_path, std::string& out_error);
		int runMessageLoop();

		void recordTo(const std::wstring& path) { record_path_ = path; }

		void tmuxAttach() override;

		void newWindow() override;
		void copySelected() override;
		void pasteClipboard() override;
		void copyLastOutput() override;
		void sendText(const std::string& text) override;
		void scrollLines(int lines) override;
		void fontSizeStep(int delta) override;
		void fontSizeReset() override;
		bool setTheme(const std::string& name) override;
		void openSearch() override;
		void jumpCommand(bool backwards) override;
		void splitPane(SplitAxis axis) override;
		void closePane() override;
		void zoomPane() override;
		void movePane(PaneMove move) override;
		void reloadSettings() override;

		HWND  uiaWindow() const override { return window_; }
		Pane* uiaFocusedPane() const override;
		bool  uiaPaneAlive(const Pane* pane) const override;
		UiaPaneGeometry uiaGeometry(const Pane& pane) const override;
		Pane* uiaPaneAt(float x, float y) const override;
		void  uiaRepaint() override;

	private:
		static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam,
			LPARAM lparam);
		static LRESULT CALLBACK keyboardHook(int code, WPARAM wparam, LPARAM lparam);
		LRESULT handleMessage(UINT message, WPARAM wparam, LPARAM lparam);
		LRESULT routeMessage(UINT message, WPARAM wparam, LPARAM lparam);

		bool registerClass(std::string& out_error);
		bool createWindow(std::string& out_error);
		bool createTarget(std::string& out_error);
		void applyFrameAppearance();
		void applyCornerRegion();
		bool customFrame() const;
		float titleHeight() const;
		int resizeBorder() const;
		LRESULT onCalcSize(WPARAM wparam, LPARAM lparam);
		LRESULT onHitTest(LPARAM lparam);
		LRESULT frameEdgeAt(int x, int y, const RECT& client) const;
		void runTitleButton(TitleButton which);
		bool titleBarTakesPress(LPARAM lparam);
		bool titleBarTakesRelease(LPARAM lparam);
		void armMouseLeave();
		void trackTitleHover(LPARAM lparam);
		void clearTitleHover();
		void trackScrollbarHover(LPARAM lparam);
		void clearScrollbarHover();
		void setWindowActive(bool active);
		std::wstring captionText() const;
		void applyWindowSettings();
		void reloadConfigIfChanged();
		void onTimer(WPARAM timer);
		void onBlinkTick();
		bool advanceCursorBlink();
		void showCursorSolid();
		bool anyPaneHoldingPaint();
		void repaintAfterOutput();
		void disarmSyncGrace();

		Pane& focused();
		const Pane& focused() const;
		bool startPane(Pane& pane, std::string& out_error);
		void splitFocused(SplitAxis axis);
		void closeFocused();
		void focusNeighbour(PaneDirection direction);
		void focusNextPane();
		void toggleZoom();
		bool paneKeyTaken(const KeyPress& press);
		bool paneCharacterTaken(char32_t character);
		void runPaneCommand(char32_t character);
		float statusHeight() const;
		bool tmuxBarShown() const;
		std::vector<StatusSegment> statusLeft();
		std::vector<StatusSegment> statusRight();
		StatusContext statusContext() const;
		void appendScriptError(std::vector<StatusSegment>& segments) const;
		void loadScript();
		bool scriptKeyTaken(const KeyPress& press);
		void openScriptFile();
		void announceStartupFetch() const;
		void armScriptTimer();
		void onScriptTick();
		void showBasePalette();
		bool statusClockChanged();
		void armSystemTimer();
		void armNewWindowHotkey();
		void disarmNewWindowHotkey();
		void refreshSystemInfo();
		void layoutPanes();
		void scheduleGridCommit();
		void commitGridSizes();
		PaneCanvas canvasFor(const PaneNode& leaf) const;

		void onPaint();
		void paintPanes();
		void onResize();
		void onPtyData();
		void closeExitedPanes();
		void onText(wchar_t unit);
		void onCharacter(char32_t character);
		bool onKeyMessage(UINT message, WPARAM wparam, LPARAM lparam);
		bool onKeyDown(WPARAM key, LPARAM lparam);
		bool onKeyUp(WPARAM key, LPARAM lparam);
		bool win32InputTakesKeys() const;
		bool characterFollows() const;
		bool sendWin32KeyDown(const KeyPress& press, LPARAM lparam);
		void sendWin32Character(wchar_t unit);
		void sendKeyedCharacter(wchar_t unit);
		void sendKeylessCharacter(wchar_t unit);
		void writeWin32Key(const Win32KeyEvent& event, bool typing);
		void flushPendingKey();
		void releaseHeldKeys();
		void noteFocusMessage(UINT message);
		void syncFocusReports();
		Pane* livePane(const Pane* pane) const;
		void reportFocus(Pane& pane, bool focused);
		POINT cursorPixels() const;
		void positionImeWindow();
		bool rightAltTakesKey(const KeyPress& press);
		bool pickerTakesKey(WPARAM key);
		bool searchTakesKey(const KeyPress& press);
		void runSearch(bool backwards);
		void continueSearch();
		void showSearchHit(Pane& pane, const SearchHit& hit);
		void settleHistory(Pane& pane);
		void answerPick(const std::string& choice);
		void pasteFromClipboard();
		void onDropFiles(WPARAM wparam);
		KeyModes currentModes() const;
		void onDpiChanged(WPARAM wparam, LPARAM lparam);
		void onMouseWheel(WPARAM wparam, LPARAM lparam);
		void onMouseDown(LPARAM lparam);
		void onMouseMove(WPARAM wparam, LPARAM lparam);
		void onMouseUp(LPARAM lparam);
		void onCaptureLost();
		bool onMouseMessage(UINT message, WPARAM wparam, LPARAM lparam);
		void onMouseTilt(WPARAM wparam, LPARAM lparam);
		void onMiddleClick(LPARAM lparam);
		bool mouseReportsTo(const Pane& pane) const;
		PaneNode* reportingLeafAt(D2D1_POINT_2F point) const;
		bool mouseLeafAlive() const;
		GridPoint cellAtDips(const PaneNode& leaf, D2D1_POINT_2F mouse) const;
		MouseReport mouseReportAt(const PaneNode& leaf, D2D1_POINT_2F mouse,
			MouseAction action, MouseButton button) const;
		void sendMouseReport(const PaneNode& leaf, const MouseReport& report);
		bool reportMousePress(MouseButton button, LPARAM lparam);
		bool reportMouseRelease(MouseButton button, LPARAM lparam);
		bool reportMouseMotion(WPARAM wparam, LPARAM lparam);
		bool reportWheel(const PaneNode& leaf, D2D1_POINT_2F point, int delta, bool tilt);
		bool alternateScrollTakes(Pane& pane, int lines);
		void forgetMouseReporting();
		bool beginDividerDrag(LPARAM lparam);
		void continueDividerDrag(LPARAM lparam);
		ScrollbarShape scrollbarOf(const PaneNode& leaf) const;
		bool scrollbarLit(const PaneNode& leaf) const;
		bool beginScrollbarDrag(LPARAM lparam);
		void continueScrollbarDrag(LPARAM lparam);
		void endScrollbarDrag();
		bool pointerOverScrollbar(float x, float y) const;
		bool onSetCursor();
		void showSystemMenu(int screen_x, int screen_y);
		bool pointIsOnCaption(int screen_x, int screen_y) const;
		void onContextMenu(LPARAM lparam);
		void runMenuChoice(const MenuChoice& choice);
		void applyChangedConfig(bool font_changed);
		void openConfigFile();
		void openThemesFolder();
		void stepFontSize(unsigned int virtual_key);
		void applyFontSize(float size);
		bool handleViewShortcut(const KeyPress& press);
		void copySelection();
		bool jumpToCommand(bool backwards);
		void copyLastCommandOutput();
		D2D1_POINT_2F dipsOf(POINT pixels) const;
		D2D1_POINT_2F mouseDips(LPARAM lparam) const;
		void applyWindowDpi(UINT dpi);
		void sizeWindowForDpi(const RECT& client_dips);
		PaneNode* leafFromMouse(LPARAM lparam) const;
		GridPoint pointInLeaf(const PaneNode& leaf, LPARAM lparam) const;
		int clickCountAt(GridPoint point);
		void stopEveryPane();
		void applyScrollbackLimit();

		void sendBytes(const char* data, std::size_t length);
		void syncTitle();

		HWND         window_ = nullptr;
		Renderer     renderer_;
		PaneTree     panes_;
		std::wstring command_line_;
		std::wstring record_path_;
		std::wstring shown_title_;
		Config       config_;
		std::wstring config_path_;
		ScriptHost   script_;
		bool         title_scripted_ = false;
		unsigned long long script_started_ = 0;
		bool         palette_scripted_ = false;
		Palette      shown_palette_;
		unsigned long long config_stamp_ = 0;
		bool         cursor_phase_ = true;
		GridPoint    last_click_;
		DWORD        last_click_time_ = 0;
		int          click_count_     = 0;
		UINT         dpi_ = USER_DEFAULT_SCREEN_DPI;
		bool         swallow_next_char_ = false;
		SurrogateJoiner surrogates_;
		WheelAccumulator wheel_;
		const Pane*  wheel_pane_ = nullptr;
		WheelAccumulator tilt_;
		PaneNode*    mouse_leaf_ = nullptr;
		int          mouse_buttons_ = 0;
		const PaneNode* motion_leaf_ = nullptr;
		GridPoint    motion_cell_;
		PaneNode*    dragging_ = nullptr;
		PaneNode*    scrolling_ = nullptr;
		PaneNode*    scrollbar_hover_ = nullptr;
		float        scroll_grab_ = 0.0f;
		std::wstring directory_hint_;
		std::string  shown_clock_;
		std::vector<StatusSegment> shown_system_;
		SystemMonitor monitor_;
		D2D1_RECT_F  status_bounds_{};
		TitleBar     title_bar_;
		bool         dwm_rounds_corners_ = false;
		bool         tmux_mode_ = false;
		bool         sync_grace_armed_ = false;
		KeyPress     prefix_;

		static HHOOK    hook_;
		static HWND     hook_owner_;
		static KeyPress hook_key_;
		static bool     hook_key_held_;
		bool         prefix_pending_ = false;
		SearchBox    search_;
		const Pane*  search_pane_ = nullptr;
		Win32KeyEvent pending_key_;
		bool         pending_key_waiting_ = false;
		bool         pending_key_used_    = false;
		wchar_t      keyless_high_        = 0;
		std::bitset<256> keys_down_sent_;
		bool         has_focus_ = false;
		const Pane*  focus_reported_ = nullptr;
		Accessibility accessibility_;

		Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
	};

} /* namespace wbshterm */

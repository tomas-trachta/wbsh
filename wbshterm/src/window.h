#pragma once

/**
 * @file window.h
 * @brief The terminal window: pty bytes in, painted grid out.
 */

#include "config.h"
#include "keymap.h"
#include "menu.h"
#include "pane_tree.h"
#include "render.h"
#include "search.h"
#include "scrollbar.h"
#include "sysinfo.h"
#include "view.h"

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
	class TerminalWindow : public TmuxHandler {
	public:
		bool create(const std::wstring& command_line, const Config& config,
			const std::wstring& config_path, std::string& out_error);
		int runMessageLoop();

		void tmuxAttach() override;

	private:
		static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam,
			LPARAM lparam);
		LRESULT handleMessage(UINT message, WPARAM wparam, LPARAM lparam);

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
		void showCursorSolid();
		bool anyPaneMidFrame() const;
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
		bool paneCharacterTaken(wchar_t character);
		void runPaneCommand(wchar_t character);
		float statusHeight() const;
		bool tmuxBarShown() const;
		std::vector<StatusSegment> statusLeft() const;
		std::vector<StatusSegment> statusRight() const;
		bool statusClockChanged();
		void armSystemTimer();
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
		void onText(wchar_t character);
		bool onKeyDown(WPARAM key);
		bool rightAltTakesKey(const KeyPress& press);
		bool pickerTakesKey(WPARAM key);
		bool searchTakesKey(const KeyPress& press);
		void runSearch(bool backwards);
		void settleHistory(Pane& pane);
		void answerPick(const std::string& choice);
		void pasteFromClipboard();
		KeyModes currentModes() const;
		void onDpiChanged(WPARAM wparam, LPARAM lparam);
		void onMouseWheel(WPARAM wparam, LPARAM lparam);
		void onMouseDown(LPARAM lparam);
		void onMouseMove(WPARAM wparam, LPARAM lparam);
		void onMouseUp(LPARAM lparam);
		void onCaptureLost();
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
		bool handleViewShortcut(const KeyPress& press);
		void copySelection();
		bool jumpToCommand(bool backwards);
		void copyLastCommandOutput();
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
		std::wstring shown_title_;
		Config       config_;
		std::wstring config_path_;
		unsigned long long config_stamp_ = 0;
		bool         cursor_phase_ = true;
		GridPoint    last_click_;
		DWORD        last_click_time_ = 0;
		int          click_count_     = 0;
		bool         swallow_next_char_ = false;
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
		bool         prefix_pending_ = false;
		SearchBox    search_;

		Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
	};

} /* namespace wbshterm */

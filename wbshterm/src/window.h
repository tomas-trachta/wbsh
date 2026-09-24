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
		void applyDarkTitleBar();
		void applyWindowSettings();
		void reloadConfigIfChanged();
		void onTimer(WPARAM timer);

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
		std::string statusLeft() const;
		std::string statusRight() const;
		bool statusClockChanged();
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
		bool pickerTakesKey(WPARAM key);
		void answerPick(const std::string& choice);
		void pasteFromClipboard();
		KeyModes currentModes() const;
		void onDpiChanged(WPARAM wparam, LPARAM lparam);
		void onMouseWheel(WPARAM wparam, LPARAM lparam);
		void onMouseDown(LPARAM lparam);
		void onMouseMove(WPARAM wparam, LPARAM lparam);
		void onMouseUp();
		void onCaptureLost();
		bool beginDividerDrag(LPARAM lparam);
		void continueDividerDrag(LPARAM lparam);
		bool onSetCursor();
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
		std::wstring directory_hint_;
		std::string  shown_clock_;
		D2D1_RECT_F  status_bounds_{};
		bool         tmux_mode_ = false;
		KeyPress     prefix_;
		bool         prefix_pending_ = false;

		Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
	};

} /* namespace wbshterm */

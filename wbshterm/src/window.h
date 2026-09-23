#pragma once

/**
 * @file window.h
 * @brief The terminal window: pty bytes in, painted grid out.
 */

#include "config.h"
#include "keymap.h"
#include "menu.h"
#include "picker.h"
#include "render.h"
#include "session.h"
#include "view.h"

#include <string>
#include <vector>

namespace wbshterm {

	/**
	 * @brief One window hosting one shell.
	 *
	 * Everything except the pty read happens on the thread that calls
	 * create(): the reader posts a message and this class drains the
	 * bytes, so the grid is only ever touched here.
	 */
	class TerminalWindow : public PickHandler {
	public:
		bool create(const std::wstring& command_line, const Config& config,
			const std::wstring& config_path, std::string& out_error);
		int runMessageLoop();

		void pickBegin(const std::string& prompt) override;
		void pickItem(const std::string& text) override;
		void pickEnd() override;
		void pickCancel() override;

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

		void onPaint();
		void onResize();
		void onPtyData();
		void onText(wchar_t character);
		bool onKeyDown(WPARAM key);
		bool pickerTakesKey(WPARAM key);
		void answerPick(const std::string& choice);
		void pasteFromClipboard();
		KeyModes currentModes() const;
		void onDpiChanged(WPARAM wparam, LPARAM lparam);
		void onMouseWheel(WPARAM wparam);
		void onMouseDown(LPARAM lparam);
		void onMouseMove(WPARAM wparam, LPARAM lparam);
		void onMouseUp();
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
		void syncWorkingDirectory();
		GridPoint pointFromMouse(LPARAM lparam) const;
		int clickCountAt(GridPoint point);
		void closeIfChildExited();

		void sendBytes(const char* data, std::size_t length);
		void syncTitle();
		void gridSizeFromClient(int& out_columns, int& out_rows) const;

		HWND         window_ = nullptr;
		Renderer     renderer_;
		Session      session_;
		std::wstring command_line_;
		std::wstring shown_title_;
		Config       config_;
		std::wstring config_path_;
		unsigned long long config_stamp_ = 0;
		bool         cursor_phase_ = true;
		TerminalView view_;
		Picker       picker_;
		GridPoint    last_click_;
		DWORD        last_click_time_ = 0;
		int          click_count_     = 0;
		bool         swallow_next_char_ = false;

		Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
	};

} /* namespace wbshterm */

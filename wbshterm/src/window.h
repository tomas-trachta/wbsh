#pragma once

/**
 * @file window.h
 * @brief The terminal window: pty bytes in, painted grid out.
 */

#include "keymap.h"
#include "render.h"
#include "session.h"
#include "view.h"

#include <string>

namespace wbshterm {

	/**
	 * @brief One window hosting one shell.
	 *
	 * Everything except the pty read happens on the thread that calls
	 * create(): the reader posts a message and this class drains the
	 * bytes, so the grid is only ever touched here.
	 */
	class TerminalWindow {
	public:
		bool create(const std::wstring& command_line, std::string& out_error);
		int runMessageLoop();

	private:
		static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam,
			LPARAM lparam);
		LRESULT handleMessage(UINT message, WPARAM wparam, LPARAM lparam);

		bool registerClass(std::string& out_error);
		bool createWindow(std::string& out_error);
		bool createTarget(std::string& out_error);
		void applyDarkTitleBar();

		void onPaint();
		void onResize();
		void onPtyData();
		void onText(wchar_t character);
		bool onKeyDown(WPARAM key);
		void pasteFromClipboard();
		KeyModes currentModes() const;
		void onDpiChanged(WPARAM wparam, LPARAM lparam);
		void onMouseWheel(WPARAM wparam);
		void onMouseDown(LPARAM lparam);
		void onMouseMove(WPARAM wparam, LPARAM lparam);
		void onMouseUp();
		bool handleViewShortcut(const KeyPress& press);
		void copySelection();
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
		TerminalView view_;
		GridPoint    last_click_;
		DWORD        last_click_time_ = 0;
		int          click_count_     = 0;
		bool         swallow_next_char_ = false;

		Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
	};

} /* namespace wbshterm */

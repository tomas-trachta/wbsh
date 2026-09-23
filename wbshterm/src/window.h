#pragma once

/**
 * @file window.h
 * @brief The terminal window: pty bytes in, painted grid out.
 */

#include "render.h"
#include "session.h"

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
		void onSpecialKey(WPARAM key);
		void onDpiChanged(WPARAM wparam, LPARAM lparam);
		void closeIfChildExited();

		void sendBytes(const char* data, std::size_t length);
		void syncTitle();
		void gridSizeFromClient(int& out_columns, int& out_rows) const;

		HWND         window_ = nullptr;
		Renderer     renderer_;
		Session      session_;
		std::wstring command_line_;
		std::wstring shown_title_;

		Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
	};

} /* namespace wbshterm */

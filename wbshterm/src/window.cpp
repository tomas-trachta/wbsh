/**
 * @file window.cpp
 * @brief Window creation, the message handlers, and key encoding.
 */

#include "window.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>

#pragma comment(lib, "dwmapi.lib")

namespace wbshterm {

	static const wchar_t kClassName[] = L"wbshtermWindow";
	static const UINT    kMessagePtyData = WM_APP + 1;
	static const float   kDefaultPointSize = 11.0f;
	static const int     kDefaultColumns = 100;
	static const int     kDefaultRows = 30;
	static const int     kWheelLines = 3;

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

	bool TerminalWindow::create(const std::wstring& command_line, std::string& out_error) {
		command_line_ = command_line;

		if (!renderer_.create(L"Cascadia Mono", kDefaultPointSize, out_error)
			&& !renderer_.create(L"Consolas", kDefaultPointSize, out_error)) {
			return false;
		}

		if (!registerClass(out_error)) return false;
		if (!createWindow(out_error)) return false;
		if (!createTarget(out_error)) return false;

		int columns = kDefaultColumns;
		int rows = kDefaultRows;
		gridSizeFromClient(columns, rows);

		session_.wakeWith(window_, kMessagePtyData);
		if (!session_.start(command_line_, columns, rows, out_error)) return false;

		::ShowWindow(window_, SW_SHOW);
		::UpdateWindow(window_);
		return true;
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
		RECT wanted = { 0, 0,
			static_cast<LONG>(cell.width * kDefaultColumns),
			static_cast<LONG>(cell.height * kDefaultRows) };
		::AdjustWindowRect(&wanted, WS_OVERLAPPEDWINDOW, FALSE);

		window_ = ::CreateWindowExW(0, kClassName, L"wbsh", WS_OVERLAPPEDWINDOW,
			CW_USEDEFAULT, CW_USEDEFAULT, wanted.right - wanted.left, wanted.bottom - wanted.top,
			nullptr, nullptr, ::GetModuleHandleW(nullptr), this);
		if (window_ == nullptr) {
			out_error = "CreateWindowEx failed";
			return false;
		}

		applyDarkTitleBar();
		return true;
	}

	void TerminalWindow::applyDarkTitleBar() {
		const BOOL dark = TRUE;
		::DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

		const COLORREF frame = RGB(0x1E, 0x1E, 0x1E);
		::DwmSetWindowAttribute(window_, DWMWA_CAPTION_COLOR, &frame, sizeof(frame));
		::DwmSetWindowAttribute(window_, DWMWA_BORDER_COLOR, &frame, sizeof(frame));
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

	void TerminalWindow::gridSizeFromClient(int& out_columns, int& out_rows) const {
		if (!target_) return;

		const D2D1_SIZE_F size = target_->GetSize();
		const CellMetrics& cell = renderer_.metrics();

		out_columns = static_cast<int>(size.width / cell.width);
		out_rows    = static_cast<int>(size.height / cell.height);
		if (out_columns < 1) out_columns = 1;
		if (out_rows < 1) out_rows = 1;
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
		case WM_SIZE:        onResize(); return 0;
		case WM_CHAR:
		case WM_SYSCHAR:     onText(static_cast<wchar_t>(wparam)); return 0;
		case WM_KEYDOWN:
		case WM_SYSKEYDOWN:
			if (onKeyDown(wparam)) return 0;
			break;
		case WM_DPICHANGED:  onDpiChanged(wparam, lparam); return 0;
		case WM_MOUSEWHEEL:  onMouseWheel(wparam); return 0;
		case WM_LBUTTONDOWN:
		case WM_LBUTTONDBLCLK: onMouseDown(lparam); return 0;
		case WM_MOUSEMOVE:   onMouseMove(wparam, lparam); return 0;
		case WM_LBUTTONUP:   onMouseUp(); return 0;
		case WM_ERASEBKGND:  return 1;
		case kMessagePtyData: onPtyData(); return 0;
		case WM_CLOSE:       ::DestroyWindow(window_); return 0;
		case WM_DESTROY:
			session_.stop();
			::PostQuitMessage(0);
			return 0;
		default:
			break;
		}

		return ::DefWindowProcW(window_, message, wparam, lparam);
	}

	GridPoint TerminalWindow::pointFromMouse(LPARAM lparam) const {
		const CellMetrics& cell = renderer_.metrics();
		const float x = static_cast<float>(GET_X_LPARAM(lparam));
		const float y = static_cast<float>(GET_Y_LPARAM(lparam));

		const int column = std::min(std::max(static_cast<int>(x / cell.width), 0),
			session_.screen().columns() - 1);
		const int viewport_row = std::min(std::max(static_cast<int>(y / cell.height), 0),
			session_.screen().rows() - 1);

		GridPoint point;
		point.row    = view_.topRow(session_.screen()) + viewport_row;
		point.column = column;
		return point;
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
		const GridPoint point = pointFromMouse(lparam);
		const int clicks = clickCountAt(point);

		if (clicks >= 3) view_.selectLine(point, session_.screen());
		else if (clicks == 2) view_.selectWord(point, session_.screen());
		else view_.beginSelection(point);

		::SetCapture(window_);
		::InvalidateRect(window_, nullptr, FALSE);
	}

	void TerminalWindow::onMouseMove(WPARAM wparam, LPARAM lparam) {
		if (!view_.selecting() || (wparam & MK_LBUTTON) == 0) return;

		view_.extendSelection(pointFromMouse(lparam));
		::InvalidateRect(window_, nullptr, FALSE);
	}

	void TerminalWindow::onMouseUp() {
		if (::GetCapture() == window_) ::ReleaseCapture();
		view_.endSelection();
	}

	void TerminalWindow::onMouseWheel(WPARAM wparam) {
		const int notches = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
		if (notches == 0) return;

		view_.scrollBy(notches * kWheelLines, session_.screen());
		::InvalidateRect(window_, nullptr, FALSE);
	}

	void TerminalWindow::copySelection() {
		const std::string text = view_.selectedText(session_.screen());
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
		const int page = std::max(1, session_.screen().rows() - 1);

		if (press.shift && !press.control && press.virtual_key == VK_PRIOR) {
			view_.scrollBy(page, session_.screen());
		} else if (press.shift && !press.control && press.virtual_key == VK_NEXT) {
			view_.scrollBy(-page, session_.screen());
		} else if (press.shift && press.control && press.virtual_key == VK_UP) {
			view_.scrollBy(1, session_.screen());
		} else if (press.shift && press.control && press.virtual_key == VK_DOWN) {
			view_.scrollBy(-1, session_.screen());
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

	void TerminalWindow::onPaint() {
		PAINTSTRUCT paint{};
		::BeginPaint(window_, &paint);

		if (target_) {
			target_->BeginDraw();
			renderer_.draw(target_.Get(), session_.screen(), view_);
			if (target_->EndDraw() == D2DERR_RECREATE_TARGET) {
				std::string ignored;
				createTarget(ignored);
			}

			session_.screen().clearDirty();
		}

		::EndPaint(window_, &paint);
	}

	void TerminalWindow::onResize() {
		if (!target_) return;

		RECT client{};
		::GetClientRect(window_, &client);
		target_->Resize(D2D1::SizeU(
			static_cast<UINT32>(client.right - client.left),
			static_cast<UINT32>(client.bottom - client.top)));

		int columns = kDefaultColumns;
		int rows = kDefaultRows;
		gridSizeFromClient(columns, rows);

		if (columns != session_.screen().columns() || rows != session_.screen().rows()) {
			session_.resize(columns, rows);
		}

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
		if (session_.drainOutput()) {
			view_.followOutput(session_.screen());
			syncTitle();
			::InvalidateRect(window_, nullptr, FALSE);
		}

		closeIfChildExited();
	}

	void TerminalWindow::closeIfChildExited() {
		if (session_.childRunning()) return;

		session_.drainOutput();
		::DestroyWindow(window_);
	}

	void TerminalWindow::syncTitle() {
		const std::string& title = session_.screen().title();
		if (title.empty()) return;

		const int needed = ::MultiByteToWideChar(CP_UTF8, 0, title.c_str(),
			static_cast<int>(title.size()), nullptr, 0);
		if (needed <= 0) return;

		std::wstring wide(static_cast<std::size_t>(needed), L'\0');
		::MultiByteToWideChar(CP_UTF8, 0, title.c_str(), static_cast<int>(title.size()),
			wide.data(), needed);
		if (wide == shown_title_) return;

		shown_title_ = wide;
		::SetWindowTextW(window_, wide.c_str());
	}

	KeyModes TerminalWindow::currentModes() const {
		KeyModes modes;
		modes.application_cursor = session_.screen().applicationCursorKeys();
		modes.bracketed_paste    = session_.screen().bracketedPaste();
		return modes;
	}

	// Alt-held characters arrive as WM_SYSCHAR and go out ESC-prefixed,
	// which is how a terminal spells Meta.
	void TerminalWindow::onText(wchar_t character) {
		if (swallow_next_char_) {
			swallow_next_char_ = false;
			return;
		}

		std::string bytes = encodeUtf8(character);
		if (bytes.empty()) return;

		if (keyIsDown(VK_MENU)) bytes.insert(bytes.begin(), '\x1b');
		sendBytes(bytes.data(), bytes.size());
	}

	bool TerminalWindow::onKeyDown(WPARAM key) {
		const KeyPress press = currentKeyPress(key);
		swallow_next_char_ = false;

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

		view_.scrollToBottom();
		view_.clearSelection();
		::InvalidateRect(window_, nullptr, FALSE);
		session_.writeInput(data, length);
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

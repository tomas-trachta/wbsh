/**
 * @file window.cpp
 * @brief Window creation, the message handlers, and key encoding.
 */

#include "window.h"

#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

namespace wbshterm {

	static const wchar_t kClassName[] = L"wbshtermWindow";
	static const UINT    kMessagePtyData = WM_APP + 1;
	static const float   kDefaultPointSize = 11.0f;
	static const int     kDefaultColumns = 100;
	static const int     kDefaultRows = 30;

	static std::string encodeUtf8(wchar_t character) {
		char bytes[8] = {};
		const int length = ::WideCharToMultiByte(CP_UTF8, 0, &character, 1, bytes,
			static_cast<int>(sizeof(bytes)), nullptr, nullptr);
		return length > 0 ? std::string(bytes, static_cast<std::size_t>(length)) : std::string();
	}

	// M2 owns the full encoder; this is the subset that makes the window
	// usable by hand — history, line motion, and the keys wbsh's editor
	// reads most.
	static const char* specialKeySequence(WPARAM key) {
		switch (key) {
		case VK_UP:     return "\x1b[A";
		case VK_DOWN:   return "\x1b[B";
		case VK_RIGHT:  return "\x1b[C";
		case VK_LEFT:   return "\x1b[D";
		case VK_HOME:   return "\x1b[H";
		case VK_END:    return "\x1b[F";
		case VK_INSERT: return "\x1b[2~";
		case VK_DELETE: return "\x1b[3~";
		case VK_PRIOR:  return "\x1b[5~";
		case VK_NEXT:   return "\x1b[6~";
		default:        return nullptr;
		}
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
		description.style         = CS_HREDRAW | CS_VREDRAW;
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
		case WM_CHAR:        onText(static_cast<wchar_t>(wparam)); return 0;
		case WM_KEYDOWN:     onSpecialKey(wparam); return 0;
		case WM_DPICHANGED:  onDpiChanged(wparam, lparam); return 0;
		case WM_ERASEBKGND:  return 1;
		case kMessagePtyData: onPtyData(); return 0;
		case WM_CLOSE:       ::DestroyWindow(window_); return 0;
		case WM_DESTROY:
			session_.stop();
			::PostQuitMessage(0);
			return 0;
		default:
			return ::DefWindowProcW(window_, message, wparam, lparam);
		}
	}

	void TerminalWindow::onPaint() {
		PAINTSTRUCT paint{};
		::BeginPaint(window_, &paint);

		if (target_) {
			target_->BeginDraw();
			renderer_.draw(target_.Get(), session_.screen());
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

	void TerminalWindow::onText(wchar_t character) {
		const std::string bytes = encodeUtf8(character);
		sendBytes(bytes.data(), bytes.size());
	}

	void TerminalWindow::onSpecialKey(WPARAM key) {
		const char* sequence = specialKeySequence(key);
		if (sequence == nullptr) return;

		sendBytes(sequence, std::char_traits<char>::length(sequence));
	}

	void TerminalWindow::sendBytes(const char* data, std::size_t length) {
		if (length == 0) return;
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

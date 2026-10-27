/**
 * @file pane.cpp
 * @brief The leaf: a session, its view, and the overlay it can raise.
 */

#include "pane.h"

#include <windows.h>

namespace wbshterm {

	Pane::Pane() {
		session_.screen().setPickHandler(this);
		session_.screen().setClipboardHandler(this);
	}

	bool Pane::start(const ShellCommand& shell, std::string& out_error) {
		return session_.start(shell, wanted_columns_, wanted_rows_, out_error);
	}

	void Pane::wantGridSize(int columns, int rows) {
		wanted_columns_ = columns;
		wanted_rows_    = rows;
	}

	bool Pane::gridSizePending() const {
		return wanted_columns_ != screen().columns() || wanted_rows_ != screen().rows();
	}

	// Resizing a pseudoconsole makes the child repaint its whole screen, so
	// the grid follows the rectangle only once the dragging has stopped.
	void Pane::commitGridSize() {
		if (!gridSizePending()) return;

		session_.resize(wanted_columns_, wanted_rows_);
		view_.clearSelection();
		view_.followOutput(screen());
	}

	// A pick arrives while the pty bytes that carried it are being parsed,
	// and that drain repaints the window, so none of these ask for one.
	void Pane::pickBegin(const std::string& prompt) {
		picker_.begin(prompt);
	}

	void Pane::pickItem(const std::string& text) {
		picker_.addItem(text);
	}

	void Pane::pickEnd() {
		picker_.finish();
	}

	void Pane::pickCancel() {
		picker_.cancel();
	}

	static HGLOBAL wideCopyOf(const std::string& utf8) {
		const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
			static_cast<int>(utf8.size()), nullptr, 0);
		if (needed < 0) return nullptr;

		const HGLOBAL block = ::GlobalAlloc(GMEM_MOVEABLE,
			(static_cast<std::size_t>(needed) + 1) * sizeof(wchar_t));
		if (block == nullptr) return nullptr;

		auto* wide = static_cast<wchar_t*>(::GlobalLock(block));
		::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide,
			needed);
		wide[needed] = L'\0';
		::GlobalUnlock(block);
		return block;
	}

	// OSC 52 from the shell: what a program copies lands on the system
	// clipboard, the way it would in a local xterm. The clipboard needs
	// an owner window, or SetClipboardData refuses the data.
	void Pane::clipboardWrite(const std::string& utf8) {
		const HWND owner = session_.wakeWindow();
		if (owner == nullptr) return;

		const HGLOBAL block = wideCopyOf(utf8);
		if (block == nullptr) return;

		if (!::OpenClipboard(owner)) {
			::GlobalFree(block);
			return;
		}

		::EmptyClipboard();
		if (::SetClipboardData(CF_UNICODETEXT, block) == nullptr) ::GlobalFree(block);
		::CloseClipboard();
	}

} /* namespace wbshterm */

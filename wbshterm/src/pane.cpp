/**
 * @file pane.cpp
 * @brief The leaf: a session, its view, and the overlay it can raise.
 */

#include "pane.h"

namespace wbshterm {

	Pane::Pane() {
		session_.screen().setPickHandler(this);
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

} /* namespace wbshterm */

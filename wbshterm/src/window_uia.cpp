/**
 * @file window_uia.cpp
 * @brief The window's answers to the UI Automation provider: panes and where they sit.
 */

#include "window.h"

namespace wbshterm {

	Pane* TerminalWindow::uiaFocusedPane() const {
		const PaneNode* leaf = panes_.focused();
		return leaf != nullptr ? leaf->pane() : nullptr;
	}

	bool TerminalWindow::uiaPaneAlive(const Pane* pane) const {
		return livePane(pane) != nullptr;
	}

	UiaPaneGeometry TerminalWindow::uiaGeometry(const Pane& pane) const {
		UiaPaneGeometry geometry;
		geometry.cell_width  = renderer_.metrics().width;
		geometry.cell_height = renderer_.metrics().height;
		geometry.dpi         = dpi_;

		for (const PaneNode* leaf : panes_.leaves()) {
			if (leaf->pane() != &pane) continue;

			geometry.left = leaf->bounds().left + renderer_.padding();
			geometry.top  = leaf->bounds().top + renderer_.padding();
		}

		return geometry;
	}

	Pane* TerminalWindow::uiaPaneAt(float x, float y) const {
		const PaneNode* leaf = panes_.leafAt(x, y);
		return leaf != nullptr ? leaf->pane() : nullptr;
	}

	void TerminalWindow::uiaRepaint() {
		::InvalidateRect(window_, nullptr, FALSE);
	}

} /* namespace wbshterm */

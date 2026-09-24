#pragma once

/**
 * @file pane.h
 * @brief One shell in a rectangle: session, view, and overlay.
 */

#include "picker.h"
#include "pty.h"
#include "screen.h"
#include "session.h"
#include "view.h"

#include <string>

namespace wbshterm {

	/**
	 * @brief A leaf of the split layout: a shell, what it shows, and where.
	 *
	 * Holds its Session by value, which pins a pane in memory: a Session
	 * hands its Screen a pointer to itself. Panes are owned through
	 * pointers and are never copied, moved, or held by value in a
	 * container.
	 */
	class Pane : public PickHandler {
	public:
		Pane();

		/** Opens the shell at the grid size the layout last asked for. */
		bool start(const ShellCommand& shell, std::string& out_error);

		Session&            session() { return session_; }
		Screen&             screen() { return session_.screen(); }
		const Screen&       screen() const { return session_.screen(); }
		TerminalView&       view() { return view_; }
		const TerminalView& view() const { return view_; }
		Picker&             picker() { return picker_; }
		const Picker&       picker() const { return picker_; }

		/** The grid its rectangle affords; the pty only hears on commit. */
		void wantGridSize(int columns, int rows);
		bool gridSizePending() const;
		void commitGridSize();

		void pickBegin(const std::string& prompt) override;
		void pickItem(const std::string& text) override;
		void pickEnd() override;
		void pickCancel() override;

	private:
		Session      session_;
		TerminalView view_;
		Picker       picker_;
		int          wanted_columns_ = 80;
		int          wanted_rows_    = 24;
	};

} /* namespace wbshterm */

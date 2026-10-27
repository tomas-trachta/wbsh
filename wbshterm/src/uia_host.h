#pragma once

/**
 * @file uia_host.h
 * @brief What the window and the UI Automation provider know of each other.
 */

#include "pane.h"

#include <windows.h>

namespace wbshterm {

	/** Where a pane's grid sits in the window: its first cell, in DIPs. */
	struct UiaPaneGeometry {
		float        left        = 0.0f;
		float        top         = 0.0f;
		float        cell_width  = 1.0f;
		float        cell_height = 1.0f;
		unsigned int dpi         = USER_DEFAULT_SCREEN_DPI;
	};

	/** What the provider needs from the window it speaks for. */
	class UiaHost {
	public:
		virtual ~UiaHost() = default;

		virtual HWND  uiaWindow() const = 0;
		virtual Pane* uiaFocusedPane() const = 0;
		virtual bool  uiaPaneAlive(const Pane* pane) const = 0;
		virtual UiaPaneGeometry uiaGeometry(const Pane& pane) const = 0;

		/** The pane under a point in the window's DIPs; nullptr between panes. */
		virtual Pane* uiaPaneAt(float x, float y) const = 0;
		virtual void  uiaRepaint() = 0;
	};

	class UiaRoot;

	/** The window's half: answering WM_GETOBJECT and telling listeners what changed. */
	class Accessibility {
	public:
		Accessibility() = default;
		~Accessibility();

		Accessibility(const Accessibility&)            = delete;
		Accessibility& operator=(const Accessibility&) = delete;

		bool answerGetObject(UiaHost* host, WPARAM wparam, LPARAM lparam, LRESULT& out_result);

		/** New output: the text changed, and the caret most likely moved with it. */
		void textChanged();
		void selectionChanged();

		void disconnect(HWND window);

	private:
		UiaRoot* root_ = nullptr;
	};

} /* namespace wbshterm */

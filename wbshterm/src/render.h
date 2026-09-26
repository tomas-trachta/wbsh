#pragma once

/**
 * @file render.h
 * @brief Direct2D painting of a Screen onto any render target.
 */

#include "config.h"
#include "font.h"
#include "picker.h"
#include "search.h"
#include "screen.h"
#include "scrollbar.h"
#include "titlebar.h"
#include "view.h"

#include <d2d1_1.h>
#include <wrl/client.h>

#include "status.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace wbshterm {

	/** One pane's rectangle in a target, and what it shows there. */
	struct PaneCanvas {
		ID2D1RenderTarget*  target  = nullptr;
		D2D1_RECT_F         bounds{};
		const Screen*       screen  = nullptr;
		const TerminalView* view    = nullptr;
		bool                focused = true;
	};

	/**
	 * @brief The bar along the bottom: segments at each end, and its look.
	 *
	 * Both ends are given in segments so that, when the window is too
	 * narrow for all of them, whole segments go rather than half a word.
	 * In pane mode the bar takes tmux's green, so typing `tmux` shows;
	 * the rest of the time it is a quiet strip in the theme's own colours.
	 */
	struct StatusBarCanvas {
		ID2D1RenderTarget*         target = nullptr;
		D2D1_RECT_F                bounds{};
		std::vector<StatusSegment> left;
		std::vector<StatusSegment> right;
		bool                       tmux = false;
	};

	/** The colours one status bar is painted in, settled once per frame. */
	struct StatusInk {
		std::uint32_t fill = 0;
		std::uint32_t edge = 0;
		std::uint32_t text = 0;
		float         label_alpha   = 1.0f;
		float         value_alpha   = 1.0f;
		float         divider_alpha = 1.0f;
		bool          tinted        = true;
	};

	/** The window's own caption: what it says, and how it is feeling. */
	struct TitleBarCanvas {
		ID2D1RenderTarget* target = nullptr;
		const TitleBar*    bar    = nullptr;
		std::wstring       title;
		bool               zoomed = false;
	};

	/**
	 * @brief Draws grids. The target is supplied per paint, so the same
	 *        renderer serves a window and an off-screen snapshot.
	 */
	class Renderer {
	public:
		bool create(const Config& config, std::string& out_error);

		/** Re-reads colours, padding and cursor style; fonts need create(). */
		void applyConfig(const Config& config);

		ID2D1Factory* factory() const { return factory_.Get(); }
		const CellMetrics& metrics() const { return font_.metrics(); }
		const Palette& palette() const { return config_.palette; }
		float padding() const { return static_cast<float>(config_.window.padding); }

		/** Cursor drawing alternates with this; the window drives the phase. */
		void setCursorVisible(bool visible) { cursor_phase_ = visible; }

		void draw(const PaneCanvas& canvas);

		/** Paints the overlay over a drawn grid; does nothing when inactive. */
		void drawPicker(const PaneCanvas& canvas, const Picker& picker);

		/** A one-line prompt in the pane's top-right corner; nothing when closed. */
		void drawSearchBox(const PaneCanvas& canvas, const SearchBox& search);

		/** A lit bar is one under the pointer or being dragged. */
		void drawScrollbar(const PaneCanvas& canvas, const ScrollbarShape& shape, bool lit);

		void drawDivider(ID2D1RenderTarget* target, const D2D1_RECT_F& bounds);
		void drawStatusBar(const StatusBarCanvas& canvas);
		void drawTitleBar(const TitleBarCanvas& canvas);
		void drawFocusBorder(ID2D1RenderTarget* target, const D2D1_RECT_F& bounds);

	private:
		bool prepareBrush(ID2D1RenderTarget* target);

		/** Where the pane's top-left cell sits inside its bounds. */
		D2D1_POINT_2F originOf(const PaneCanvas& canvas) const;

		void drawRowBackgrounds(const PaneCanvas& canvas, int absolute_row, int viewport_row);
		void drawRowText(const PaneCanvas& canvas, int absolute_row, int viewport_row);
		void drawRun(const PaneCanvas& canvas, const std::wstring& text, const Cell& style,
			int row, int column);
		IDWriteTextLayout* layoutFor(const std::wstring& text, bool bold, bool italic);
		void drawCursor(const PaneCanvas& canvas);
		void drawBlockCursor(const PaneCanvas& canvas, float left, float top);
		void drawPickerRow(const PaneCanvas& canvas, const std::wstring& text, float top,
			float width, bool highlighted);
		void drawStatusBackdrop(const StatusBarCanvas& canvas, const StatusInk& ink);
		float drawStatusRow(const StatusBarCanvas& canvas, const StatusInk& ink,
			const std::vector<StatusSegment>& segments, std::size_t first, float left);
		float drawStatusSegment(const StatusBarCanvas& canvas, const StatusInk& ink,
			const StatusSegment& segment, float left);
		float drawStatusRun(const StatusBarCanvas& canvas, const std::string& text, float left);
		void drawStatusDivider(const StatusBarCanvas& canvas, const StatusInk& ink, float center);
		void drawTitleLights(const TitleBarCanvas& canvas);
		void drawTitleText(const TitleBarCanvas& canvas);
		void drawTitleGlyph(const TitleBarCanvas& canvas, TitleButton which);
		void drawZoomArrows(const TitleBarCanvas& canvas, const D2D1_ELLIPSE& circle,
			float reach);
		void drawPickerMatches(const PaneCanvas& canvas, const Picker& picker, float top,
			float width, int visible_rows);

		std::uint32_t backgroundFor(const PaneCanvas& canvas, int absolute_row,
			int column) const;
		std::uint32_t resolveColor(std::uint32_t color, std::uint32_t fallback) const;
		std::uint32_t resolveForeground(const Cell& cell) const;
		std::uint32_t resolveBackground(const Cell& cell) const;
		void setBrushColor(std::uint32_t rgb, float alpha);

		using LayoutCache =
			std::unordered_map<std::wstring, Microsoft::WRL::ComPtr<IDWriteTextLayout>>;

		Microsoft::WRL::ComPtr<ID2D1Factory>         factory_;
		Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
		LayoutCache                                  layouts_[4];
		ID2D1RenderTarget*                           brush_owner_ = nullptr;
		FontSet                                      font_;
		Config                                       config_;
		bool                                         cursor_phase_ = true;
		bool                                         color_fonts_  = false;
		D2D1_DRAW_TEXT_OPTIONS                       text_options_ =
			D2D1_DRAW_TEXT_OPTIONS_CLIP;
	};

} /* namespace wbshterm */

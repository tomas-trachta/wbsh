#pragma once

/**
 * @file render.h
 * @brief Direct2D painting of a Screen onto any render target.
 */

#include "config.h"
#include "font.h"
#include "picker.h"
#include "screen.h"
#include "titlebar.h"
#include "view.h"

#include <d2d1_1.h>
#include <wrl/client.h>

#include <string>

namespace wbshterm {

	/** One pane's rectangle in a target, and what it shows there. */
	struct PaneCanvas {
		ID2D1RenderTarget*  target  = nullptr;
		D2D1_RECT_F         bounds{};
		const Screen*       screen  = nullptr;
		const TerminalView* view    = nullptr;
		bool                focused = true;
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

		void drawDivider(ID2D1RenderTarget* target, const D2D1_RECT_F& bounds);
		void drawStatusBar(ID2D1RenderTarget* target, const D2D1_RECT_F& bounds,
			const std::string& left, const std::string& right);
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
		void drawCursor(const PaneCanvas& canvas);
		void drawBlockCursor(const PaneCanvas& canvas, float left, float top);
		void drawPickerRow(const PaneCanvas& canvas, const std::wstring& text, float top,
			float width, bool highlighted);
		void drawStatusText(ID2D1RenderTarget* target, const D2D1_RECT_F& bounds,
			const std::string& text, bool to_the_right);
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

		Microsoft::WRL::ComPtr<ID2D1Factory>         factory_;
		Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
		ID2D1RenderTarget*                           brush_owner_ = nullptr;
		FontSet                                      font_;
		Config                                       config_;
		bool                                         cursor_phase_ = true;
		bool                                         color_fonts_  = false;
		D2D1_DRAW_TEXT_OPTIONS                       text_options_ =
			D2D1_DRAW_TEXT_OPTIONS_CLIP;
	};

} /* namespace wbshterm */

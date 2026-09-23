#pragma once

/**
 * @file render.h
 * @brief Direct2D painting of a Screen onto any render target.
 */

#include "config.h"
#include "font.h"
#include "screen.h"
#include "view.h"

#include <d2d1_1.h>
#include <wrl/client.h>

#include <string>

namespace wbshterm {

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

		void draw(ID2D1RenderTarget* target, const Screen& screen, const TerminalView& view);

	private:
		bool prepareBrush(ID2D1RenderTarget* target);

		void drawRowBackgrounds(ID2D1RenderTarget* target, const Screen& screen,
			const TerminalView& view, int absolute_row, int viewport_row);
		void drawRowText(ID2D1RenderTarget* target, const Screen& screen,
			const TerminalView& view, int absolute_row, int viewport_row);
		void drawRun(ID2D1RenderTarget* target, const std::wstring& text, const Cell& style,
			int row, int column);
		void drawCursor(ID2D1RenderTarget* target, const Screen& screen, const TerminalView& view);

		std::uint32_t backgroundFor(const Screen& screen, const TerminalView& view,
			int absolute_row, int column) const;
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

#pragma once

/**
 * @file render.h
 * @brief Direct2D painting of a Screen onto any render target.
 */

#include "font.h"
#include "screen.h"

#include <d2d1.h>
#include <wrl/client.h>

#include <string>

namespace wbshterm {

	struct Theme {
		std::uint32_t background = 0x1E1E1E;
		std::uint32_t foreground = 0xD4D4D4;
		std::uint32_t cursor     = 0xD4D4D4;
	};

	/**
	 * @brief Draws grids. The target is supplied per paint, so the same
	 *        renderer serves a window and an off-screen snapshot.
	 */
	class Renderer {
	public:
		bool create(const std::wstring& font_family, float point_size, std::string& out_error);

		ID2D1Factory* factory() const { return factory_.Get(); }
		const CellMetrics& metrics() const { return font_.metrics(); }
		const Theme& theme() const { return theme_; }

		void draw(ID2D1RenderTarget* target, const Screen& screen);

	private:
		void drawRowBackgrounds(ID2D1RenderTarget* target, const Screen& screen, int row);
		void drawRowText(ID2D1RenderTarget* target, const Screen& screen, int row);
		void drawRun(ID2D1RenderTarget* target, const std::wstring& text, const Cell& style,
			int row, int column);
		void drawCursor(ID2D1RenderTarget* target, const Screen& screen);

		std::uint32_t resolveForeground(const Cell& cell) const;
		std::uint32_t resolveBackground(const Cell& cell) const;
		void setBrushColor(std::uint32_t rgb, float alpha);

		Microsoft::WRL::ComPtr<ID2D1Factory>             factory_;
		Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>     brush_;
		ID2D1RenderTarget*                               brush_owner_ = nullptr;
		FontSet                                          font_;
		Theme                                            theme_;
	};

} /* namespace wbshterm */

/**
 * @file render_tabs.cpp
 * @brief The tab strip: a tab per session, the shown one in the grid's own
 *        colour so it reads as the top of the terminal below it, the rest
 *        quiet on the caption, and the new-tab button after them.
 */

#include "render.h"

#include <cmath>

namespace wbshterm {

	namespace tabs_detail {

		static const float kCornerRadius   = 7.0f;
		static const float kTabGap         = 1.0f;
		static const float kCaptionTint    = 0.05f;
		static const float kEdgeTint       = 0.16f;
		static const float kHoverTint      = 0.10f;
		static const float kSeparatorTint  = 0.20f;
		static const float kSeparatorInset = 9.0f;
		static const float kGlyphHoverTint = 0.20f;
		static const float kGlyphPressTint = 0.28f;
		static const float kGlyphRadius    = 4.0f;
		static const float kCrossReach     = 3.5f;
		static const float kPlusReach      = 5.0f;
		static const float kGlyphStroke    = 1.2f;

		static D2D1_POINT_2F centreOf(const D2D1_RECT_F& rect) {
			return D2D1::Point2F((rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f);
		}

		static bool tabIsLit(const TabStrip& strip, int index) {
			const TabHit& hovered = strip.hovered();
			return index == strip.active()
				|| (hovered.part != TabPart::None && hovered.index == index);
		}

		static float titleInk(bool active, bool window_active) {
			if (active) return window_active ? 0.90f : 0.65f;
			return window_active ? 0.60f : 0.42f;
		}

	} /* namespace tabs_detail */

	void Renderer::drawTabStrip(const TabStripCanvas& canvas) {
		if (canvas.strip == nullptr || !prepareBrush(canvas.target)) return;

		const D2D1_RECT_F& bounds = canvas.strip->bounds();
		if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;

		drawTabBackdrop(canvas);

		canvas.target->PushAxisAlignedClip(bounds, D2D1_ANTIALIAS_MODE_ALIASED);
		drawTabSeparators(canvas);
		for (int index = 0; index < canvas.strip->count(); ++index) drawTab(canvas, index);
		drawNewTabButton(canvas);
		canvas.target->PopAxisAlignedClip();
	}

	// The band under a system frame is the caption the window does not
	// have: the same shade, and the same hairline parting it from the grid.
	void Renderer::drawTabBackdrop(const TabStripCanvas& canvas) {
		const D2D1_RECT_F& band = canvas.backdrop;
		if (band.bottom <= band.top) return;

		const Palette& palette = config_.palette;
		setBrushColor(blend(palette.background, palette.foreground, tabs_detail::kCaptionTint),
			1.0f);
		canvas.target->FillRectangle(band, brush_.Get());

		setBrushColor(blend(palette.background, palette.foreground, tabs_detail::kEdgeTint), 1.0f);
		canvas.target->FillRectangle(
			D2D1::RectF(band.left, band.bottom - 1.0f, band.right, band.bottom), brush_.Get());
	}

	// A hairline parts two quiet tabs; beside a lit one its fill does that.
	void Renderer::drawTabSeparators(const TabStripCanvas& canvas) {
		const TabStrip& strip = *canvas.strip;
		const Palette& palette = config_.palette;
		setBrushColor(blend(palette.background, palette.foreground, tabs_detail::kSeparatorTint),
			1.0f);

		for (int index = 0; index + 1 < strip.count(); ++index) {
			if (tabs_detail::tabIsLit(strip, index) || tabs_detail::tabIsLit(strip, index + 1)) {
				continue;
			}

			const D2D1_RECT_F tab = strip.tabRect(index);
			const float x = std::floor(tab.right);
			canvas.target->FillRectangle(D2D1::RectF(x, tab.top + tabs_detail::kSeparatorInset,
				x + 1.0f, tab.bottom - tabs_detail::kSeparatorInset), brush_.Get());
		}
	}

	// The shown tab's shape runs on past the bottom of the strip, where the
	// clip cuts it square, so only its top corners are round and it opens
	// straight into the grid below.
	void Renderer::drawTab(const TabStripCanvas& canvas, int index) {
		const TabStrip& strip = *canvas.strip;
		const bool active = index == strip.active();

		if (tabs_detail::tabIsLit(strip, index)) {
			const Palette& palette = config_.palette;
			const D2D1_RECT_F tab = strip.tabRect(index);
			const D2D1_RECT_F shape = D2D1::RectF(tab.left + tabs_detail::kTabGap, tab.top,
				tab.right - tabs_detail::kTabGap, tab.bottom + tabs_detail::kCornerRadius);
			const float radius = tabs_detail::kCornerRadius;

			setBrushColor(active
				? palette.background
				: blend(palette.background, palette.foreground, tabs_detail::kHoverTint), 1.0f);
			canvas.target->FillRoundedRectangle(D2D1::RoundedRect(shape, radius, radius),
				brush_.Get());
		}

		drawTabTitle(canvas, index);
		if (strip.closeShown(index)) drawTabClose(canvas, index);
	}

	void Renderer::drawTabTitle(const TabStripCanvas& canvas, int index) {
		IDWriteTextFormat* format = font_.tabFormat();
		const auto slot = static_cast<std::size_t>(index);
		if (format == nullptr || slot >= canvas.titles.size()) return;

		const D2D1_RECT_F box = canvas.strip->titleRect(index);
		const std::wstring& title = canvas.titles[slot];
		if (box.right <= box.left || title.empty()) return;

		const bool active = index == canvas.strip->active();
		setBrushColor(blend(config_.palette.background, config_.palette.foreground,
			tabs_detail::titleInk(active, canvas.window_active)), 1.0f);
		canvas.target->DrawText(title.c_str(), static_cast<UINT32>(title.size()), format, box,
			brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
	}

	void Renderer::drawTabGlyphBackdrop(const TabStripCanvas& canvas, const D2D1_RECT_F& box,
			const TabHit& which) {
		const TabStrip& strip = *canvas.strip;
		const bool pressed = strip.pressed() == which;
		if (!pressed && strip.hovered() != which) return;

		const float tint = pressed ? tabs_detail::kGlyphPressTint : tabs_detail::kGlyphHoverTint;
		const float radius = tabs_detail::kGlyphRadius;
		setBrushColor(blend(config_.palette.background, config_.palette.foreground, tint), 1.0f);
		canvas.target->FillRoundedRectangle(D2D1::RoundedRect(box, radius, radius), brush_.Get());
	}

	void Renderer::drawTabClose(const TabStripCanvas& canvas, int index) {
		const TabStrip& strip = *canvas.strip;
		const D2D1_RECT_F box = strip.closeRect(index);
		drawTabGlyphBackdrop(canvas, box, TabHit{ TabPart::Close, index });

		const D2D1_POINT_2F centre = tabs_detail::centreOf(box);
		const float reach = tabs_detail::kCrossReach;
		const bool active = index == strip.active();
		setBrushColor(blend(config_.palette.background, config_.palette.foreground,
			tabs_detail::titleInk(active, canvas.window_active)), 1.0f);

		canvas.target->DrawLine(D2D1::Point2F(centre.x - reach, centre.y - reach),
			D2D1::Point2F(centre.x + reach, centre.y + reach), brush_.Get(),
			tabs_detail::kGlyphStroke);
		canvas.target->DrawLine(D2D1::Point2F(centre.x - reach, centre.y + reach),
			D2D1::Point2F(centre.x + reach, centre.y - reach), brush_.Get(),
			tabs_detail::kGlyphStroke);
	}

	void Renderer::drawNewTabButton(const TabStripCanvas& canvas) {
		const D2D1_RECT_F box = canvas.strip->newRect();
		drawTabGlyphBackdrop(canvas, box, TabHit{ TabPart::New, -1 });

		const D2D1_POINT_2F centre = tabs_detail::centreOf(box);
		const float reach = tabs_detail::kPlusReach;
		setBrushColor(blend(config_.palette.background, config_.palette.foreground,
			tabs_detail::titleInk(false, canvas.window_active)), 1.0f);

		canvas.target->DrawLine(D2D1::Point2F(centre.x - reach, centre.y),
			D2D1::Point2F(centre.x + reach, centre.y), brush_.Get(), tabs_detail::kGlyphStroke);
		canvas.target->DrawLine(D2D1::Point2F(centre.x, centre.y - reach),
			D2D1::Point2F(centre.x, centre.y + reach), brush_.Get(), tabs_detail::kGlyphStroke);
	}

} /* namespace wbshterm */

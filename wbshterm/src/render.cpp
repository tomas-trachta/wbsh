/**
 * @file render.cpp
 * @brief Row backgrounds, then glyph runs, then the cursor.
 */

#include "render.h"

#include "utf8.h"

#include <algorithm>
#include <cmath>

#pragma comment(lib, "d2d1.lib")

namespace wbshterm {

	static const float kDimAlpha = 0.65f;

	// Shaping a run is the dear part of a frame, and most runs are the
	// same text as the frame before: a prompt, a status line, a panel
	// that has not moved. Their layouts are kept in two generations: a
	// full one is set aside as the older, and whatever is still drawn
	// from it moves back, so only layouts nobody asked for are dropped.
	static const std::size_t kLayoutCacheLimit = 2048;

	// One retained grid per pane; past this many, panes that closed are
	// forgotten along with the rest.
	static const std::size_t kMaxGridCaches = 32;

	static D2D1_COLOR_F toColorF(std::uint32_t rgb, float alpha) {
		const float red   = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f;
		const float green = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f;
		const float blue  = static_cast<float>(rgb & 0xFF) / 255.0f;
		return D2D1::ColorF(red, green, blue, alpha);
	}

	static void appendUtf16(std::wstring& text, char32_t code) {
		if (code < 0x10000) {
			text.push_back(static_cast<wchar_t>(code));
			return;
		}

		const char32_t offset = code - 0x10000;
		text.push_back(static_cast<wchar_t>(0xD800 + (offset >> 10)));
		text.push_back(static_cast<wchar_t>(0xDC00 + (offset & 0x3FF)));
	}

	static void appendCellText(std::wstring& text, const Screen& screen, const Cell& cell) {
		const std::u32string* cluster = screen.clusterOf(cell);
		if (cluster == nullptr) {
			appendUtf16(text, screen.baseCodeOf(cell));
			return;
		}

		for (const char32_t code : *cluster) appendUtf16(text, code);
	}

	bool Renderer::create(const Config& config, std::string& out_error) {
		config_ = config;

		if (!factory_) {
			const HRESULT hr = ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
				factory_.GetAddressOf());
			if (FAILED(hr)) {
				out_error = "D2D1CreateFactory failed";
				return false;
			}
		}

		for (LayoutCache& cache : layouts_) cache.clear();
		for (LayoutCache& cache : older_layouts_) cache.clear();
		++generation_;
		return font_.create(config.font, out_error);
	}

	static std::size_t layoutIndex(bool bold, bool italic) {
		return (bold ? 1u : 0u) + (italic ? 2u : 0u);
	}

	IDWriteTextLayout* Renderer::layoutFor(const std::wstring& text, bool bold, bool italic) {
		const std::size_t index = layoutIndex(bold, italic);
		LayoutCache& cache = layouts_[index];
		const auto found = cache.find(text);
		if (found != cache.end()) return found->second.Get();

		if (cache.size() >= kLayoutCacheLimit) {
			older_layouts_[index] = std::move(cache);
			cache.clear();
		}

		LayoutCache& older = older_layouts_[index];
		const auto kept = older.find(text);
		if (kept != older.end()) {
			IDWriteTextLayout* revived = cache.emplace(text, kept->second).first->second.Get();
			older.erase(kept);
			return revived;
		}

		const CellMetrics& cell_box = font_.metrics();
		Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
		const HRESULT hr = font_.factory()->CreateTextLayout(text.c_str(),
			static_cast<UINT32>(text.size()), font_.format(bold, italic),
			cell_box.width * static_cast<float>(text.size() + 2), cell_box.height,
			layout.GetAddressOf());
		if (FAILED(hr)) return nullptr;

		return cache.emplace(text, layout).first->second.Get();
	}

	void Renderer::applyConfig(const Config& config) {
		config_ = config;
		++generation_;
	}

	// A palette slot is resolved here rather than when the text was written,
	// so a theme change repaints everything already on screen.
	std::uint32_t Renderer::resolveColor(std::uint32_t color, std::uint32_t fallback) const {
		if (color == kDefaultColor) return fallback;
		if ((color & 0xFF000000u) != kPaletteColor) return color;

		return config_.palette.ansi[color & 0x0Fu];
	}

	std::uint32_t Renderer::resolveForeground(const Cell& cell) const {
		const std::uint32_t color = resolveColor(cell.foreground, config_.palette.foreground);
		if ((cell.attributes & kAttrReverse) == 0) return color;

		return resolveColor(cell.background, config_.palette.background);
	}

	std::uint32_t Renderer::resolveBackground(const Cell& cell) const {
		const std::uint32_t color = resolveColor(cell.background, config_.palette.background);
		if ((cell.attributes & kAttrReverse) == 0) return color;

		return resolveColor(cell.foreground, config_.palette.foreground);
	}

	void Renderer::setBrushColor(std::uint32_t rgb, float alpha) {
		brush_->SetColor(toColorF(rgb, alpha));
	}

	bool Renderer::prepareBrush(ID2D1RenderTarget* target) {
		if (brush_owner_ == target && brush_) return true;

		brush_.Reset();
		if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),
				brush_.ReleaseAndGetAddressOf()))) {
			return false;
		}

		brush_owner_ = target;

		Microsoft::WRL::ComPtr<ID2D1DeviceContext> context;
		color_fonts_ = SUCCEEDED(target->QueryInterface(IID_PPV_ARGS(context.GetAddressOf())));
		text_options_ = color_fonts_
			? (D2D1_DRAW_TEXT_OPTIONS_CLIP | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT)
			: D2D1_DRAW_TEXT_OPTIONS_CLIP;
		return true;
	}

	D2D1_POINT_2F Renderer::originOf(const PaneCanvas& canvas) const {
		return D2D1::Point2F(canvas.bounds.left + padding(), canvas.bounds.top + padding());
	}

	// The clip is not decoration: a run is drawn into a box two cells wider
	// than its text so glyph overhang survives, which would otherwise reach
	// into the pane next door.
	void Renderer::draw(const PaneCanvas& canvas) {
		if (!prepareBrush(canvas.target)) return;

		canvas.target->PushAxisAlignedClip(canvas.bounds, D2D1_ANTIALIAS_MODE_ALIASED);
		if (!drawRetainedGrid(canvas) && prepareBrush(canvas.target)) drawGrid(canvas, true);

		if (prepareBrush(canvas.target)) drawCursor(canvas);
		canvas.target->PopAxisAlignedClip();
	}

	void Renderer::drawGrid(const PaneCanvas& canvas, bool whole) {
		if (whole) canvas.target->Clear(toColorF(config_.palette.background, 1.0f));

		const Screen& screen = *canvas.screen;
		const int top = canvas.view->topRow(screen);
		for (int row = 0; row < screen.rows(); ++row) {
			if (whole || screen.rowDirty(top + row - screen.scrollbackRows())) {
				drawRow(canvas, top + row, row);
			}
		}
	}

	// Each row is drawn inside its own strip, so redrawing one row alone
	// leaves exactly what redrawing them all would.
	void Renderer::drawRow(const PaneCanvas& canvas, int absolute_row, int viewport_row) {
		const CellMetrics& cell_box = font_.metrics();
		const float top = originOf(canvas).y + static_cast<float>(viewport_row) * cell_box.height;
		const D2D1_RECT_F strip = D2D1::RectF(canvas.bounds.left, top, canvas.bounds.right,
			top + cell_box.height);

		canvas.target->PushAxisAlignedClip(strip, D2D1_ANTIALIAS_MODE_ALIASED);
		canvas.target->Clear(toColorF(config_.palette.background, 1.0f));
		drawRowBackgrounds(canvas, absolute_row, viewport_row);
		drawRowText(canvas, absolute_row, viewport_row);
		canvas.target->PopAxisAlignedClip();
	}

	Renderer::GridCache* Renderer::gridCacheFor(const PaneCanvas& canvas) {
		if (grid_caches_.size() > kMaxGridCaches) grid_caches_.clear();

		GridCache& cache = grid_caches_[canvas.screen];
		const D2D1_SIZE_F size = D2D1::SizeF(canvas.bounds.right - canvas.bounds.left,
			canvas.bounds.bottom - canvas.bounds.top);
		float dpi_x = 0.0f;
		float dpi_y = 0.0f;
		canvas.target->GetDpi(&dpi_x, &dpi_y);

		const bool fits = cache.bitmap && cache.owner.Get() == canvas.target
			&& cache.size.width == size.width && cache.size.height == size.height
			&& cache.dpi == dpi_x;
		if (fits) return &cache;

		cache = GridCache();
		if (FAILED(canvas.target->CreateCompatibleRenderTarget(size,
				cache.bitmap.GetAddressOf()))) {
			grid_caches_.erase(canvas.screen);
			return nullptr;
		}

		cache.owner = canvas.target;
		cache.size  = size;
		cache.dpi   = dpi_x;
		return &cache;
	}

	// Rows the screen marked dirty are all that changed, unless something
	// the whole grid depends on moved: the view, the history under it, a
	// selection, or the colours and font.
	bool Renderer::gridCacheIsCurrent(const GridCache& cache, const PaneCanvas& canvas) const {
		const Screen& screen = *canvas.screen;
		return cache.generation == generation_
			&& cache.top == canvas.view->topRow(screen)
			&& cache.history == screen.scrollbackRows()
			&& cache.columns == screen.columns() && cache.rows == screen.rows()
			&& !cache.selection && !canvas.view->hasSelection();
	}

	void Renderer::rememberGrid(GridCache& cache, const PaneCanvas& canvas) const {
		const Screen& screen = *canvas.screen;
		cache.generation = generation_;
		cache.top        = canvas.view->topRow(screen);
		cache.history    = screen.scrollbackRows();
		cache.columns    = screen.columns();
		cache.rows       = screen.rows();
		cache.selection  = canvas.view->hasSelection();
	}

	// The grid is kept in a bitmap of the pane's size and only its damaged
	// rows are drawn again; the cursor goes on top each frame. Anything
	// that fails falls back to drawing the grid straight onto the target.
	bool Renderer::drawRetainedGrid(const PaneCanvas& canvas) {
		GridCache* cache = gridCacheFor(canvas);
		if (cache == nullptr) return false;

		PaneCanvas inner = canvas;
		inner.target = cache->bitmap.Get();
		inner.bounds = D2D1::RectF(0.0f, 0.0f, cache->size.width, cache->size.height);
		if (!prepareBrush(inner.target)) return false;

		inner.target->BeginDraw();
		drawGrid(inner, !gridCacheIsCurrent(*cache, canvas));
		if (FAILED(inner.target->EndDraw())) {
			grid_caches_.erase(canvas.screen);
			return false;
		}

		rememberGrid(*cache, canvas);
		return blitGrid(*cache, canvas);
	}

	bool Renderer::blitGrid(const GridCache& cache, const PaneCanvas& canvas) {
		Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
		if (FAILED(cache.bitmap->GetBitmap(bitmap.GetAddressOf()))) return false;
		if (!prepareBrush(canvas.target)) return false;

		canvas.target->DrawBitmap(bitmap.Get(), canvas.bounds, 1.0f,
			D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
		return true;
	}

	// Chrome colours are mixed from the palette rather than added to it,
	// so every theme file already on disk keeps working untouched.
	static std::uint32_t blendChannel(std::uint32_t from, std::uint32_t to, int shift,
			float amount) {
		const float start = static_cast<float>((from >> shift) & 0xFF);
		const float end   = static_cast<float>((to >> shift) & 0xFF);
		const auto mixed  = static_cast<std::uint32_t>(start + (end - start) * amount);

		return (mixed & 0xFF) << shift;
	}

	static std::uint32_t blend(std::uint32_t from, std::uint32_t to, float amount) {
		return blendChannel(from, to, 16, amount)
			| blendChannel(from, to, 8, amount)
			| blendChannel(from, to, 0, amount);
	}

	void Renderer::drawDivider(ID2D1RenderTarget* target, const D2D1_RECT_F& bounds) {
		if (!prepareBrush(target)) return;

		setBrushColor(blend(config_.palette.background, config_.palette.foreground, 0.25f),
			1.0f);
		target->FillRectangle(bounds, brush_.Get());
	}

	void Renderer::drawFocusBorder(ID2D1RenderTarget* target, const D2D1_RECT_F& bounds) {
		if (!prepareBrush(target)) return;

		setBrushColor(config_.palette.cursor, 1.0f);
		target->DrawRectangle(
			D2D1::RectF(bounds.left + 0.5f, bounds.top + 0.5f, bounds.right - 0.5f,
				bounds.bottom - 0.5f), brush_.Get(), 1.0f);
	}

	std::uint32_t Renderer::backgroundFor(const PaneCanvas& canvas, int absolute_row,
			int column) const {
		if (canvas.view->isSelected(absolute_row, column)) return config_.palette.selection;
		return resolveBackground(canvas.screen->cellAt(absolute_row, column));
	}

	// A plus on the total says the shell offered more than the list took,
	// so an absent entry reads as cut off rather than missing.
	static std::string pickerHeader(const Picker& picker) {
		const std::string total = std::to_string(picker.itemCount())
			+ (picker.truncated() ? "+" : "");

		return "  " + picker.prompt() + " > " + picker.query()
			+ "    [" + std::to_string(picker.matches().size()) + "/" + total + "]";
	}

	void Renderer::drawPickerRow(const PaneCanvas& canvas, const std::wstring& text, float top,
			float width, bool highlighted) {
		const CellMetrics& cell = font_.metrics();
		const float left = originOf(canvas).x;

		if (highlighted) {
			setBrushColor(config_.palette.selection, 1.0f);
			canvas.target->FillRectangle(
				D2D1::RectF(left, top, left + width, top + cell.height), brush_.Get());
		}

		setBrushColor(config_.palette.foreground, 1.0f);
		canvas.target->DrawText(text.c_str(), static_cast<UINT32>(text.size()),
			font_.format(false, false),
			D2D1::RectF(left, top, left + width, top + cell.height), brush_.Get(), text_options_);
	}

	void Renderer::drawPickerMatches(const PaneCanvas& canvas, const Picker& picker, float top,
			float width, int visible_rows) {
		const CellMetrics& cell = font_.metrics();
		const int first = std::max(0, picker.selected() - visible_rows + 1);

		for (int row = 0; row < visible_rows; ++row) {
			const int match = first + row;
			if (match >= static_cast<int>(picker.matches().size())) break;

			const bool current = match == picker.selected();
			const std::string line = (current ? "> " : "  ")
				+ picker.item(picker.matches()[static_cast<std::size_t>(match)]);
			drawPickerRow(canvas, widenUtf8(line),
				top + static_cast<float>(row + 1) * cell.height, width, current);
		}
	}

	// The overlay sits at the bottom, over whatever the grid was showing, so
	// the command being typed stays visible above it.
	void Renderer::drawPicker(const PaneCanvas& canvas, const Picker& picker) {
		if (!picker.active() || !prepareBrush(canvas.target)) return;

		const CellMetrics& cell = font_.metrics();
		const D2D1_POINT_2F origin = originOf(canvas);
		const int visible_rows = std::min(static_cast<int>(picker.matches().size()), 10);

		const float width = static_cast<float>(canvas.screen->columns()) * cell.width;
		const float height = static_cast<float>(visible_rows + 1) * cell.height;
		const float top = origin.y
			+ static_cast<float>(canvas.screen->rows()) * cell.height - height;

		canvas.target->PushAxisAlignedClip(canvas.bounds, D2D1_ANTIALIAS_MODE_ALIASED);

		setBrushColor(config_.palette.background, 1.0f);
		canvas.target->FillRectangle(
			D2D1::RectF(origin.x, top, origin.x + width, top + height), brush_.Get());

		setBrushColor(config_.palette.cursor, 1.0f);
		canvas.target->DrawLine(D2D1::Point2F(origin.x, top),
			D2D1::Point2F(origin.x + width, top), brush_.Get(), 1.0f);

		drawPickerRow(canvas, widenUtf8(pickerHeader(picker)), top, width, false);
		drawPickerMatches(canvas, picker, top, width, visible_rows);

		canvas.target->PopAxisAlignedClip();
	}

	// The box sits over the top-right corner, away from the prompt at the
	// bottom, and reads as a question until a match answers it.
	void Renderer::drawSearchBox(const PaneCanvas& canvas, const SearchBox& search) {
		if (!search.active() || !prepareBrush(canvas.target)) return;

		const CellMetrics& cell = font_.metrics();
		const D2D1_POINT_2F origin = originOf(canvas);
		const std::wstring text = search.caption();
		const float width = static_cast<float>(text.size() + 2) * cell.width;
		const float right = origin.x + static_cast<float>(canvas.screen->columns()) * cell.width;
		const float left  = std::max(origin.x, right - width);
		const float top   = origin.y;

		canvas.target->PushAxisAlignedClip(canvas.bounds, D2D1_ANTIALIAS_MODE_ALIASED);

		setBrushColor(config_.palette.selection, 1.0f);
		canvas.target->FillRectangle(D2D1::RectF(left, top, right, top + cell.height),
			brush_.Get());

		setBrushColor(config_.palette.foreground, 1.0f);
		canvas.target->DrawText(text.c_str(), static_cast<UINT32>(text.size()),
			font_.format(false, false),
			D2D1::RectF(left + cell.width, top, right, top + cell.height), brush_.Get(),
			text_options_);

		canvas.target->PopAxisAlignedClip();
	}

	// The bar lives in the margin, so the grid keeps every column; a
	// rounded thumb in the text's own colour is enough to read the depth
	// of history without a track behind it until the pointer arrives.
	void Renderer::drawScrollbar(const PaneCanvas& canvas, const ScrollbarShape& shape,
			bool lit) {
		if (!shape.present || !prepareBrush(canvas.target)) return;

		const float radius = (shape.track.right - shape.track.left) * 0.5f;
		const std::uint32_t ink = blend(config_.palette.background,
			config_.palette.foreground, 0.55f);

		canvas.target->PushAxisAlignedClip(canvas.bounds, D2D1_ANTIALIAS_MODE_ALIASED);

		if (lit) {
			setBrushColor(ink, 0.15f);
			canvas.target->FillRoundedRectangle(D2D1::RoundedRect(shape.track, radius, radius),
				brush_.Get());
		}

		setBrushColor(ink, lit ? 0.9f : 0.45f);
		canvas.target->FillRoundedRectangle(D2D1::RoundedRect(shape.thumb, radius, radius),
			brush_.Get());

		canvas.target->PopAxisAlignedClip();
	}

	// The bar sits a shade above the grid, the way the caption does at the
	// top, with the same hairline to part them; labels are kept quiet so
	// the figures read first. In pane mode it takes tmux's black on green
	// instead, and a theme's own green keeps that recognisable without
	// pinning the colour to one palette.
	static const float kStatusFillTint     = 0.05f;
	static const float kStatusEdgeTint     = 0.16f;
	static const float kStatusLabelAlpha   = 0.45f;
	static const float kStatusValueAlpha   = 0.85f;
	static const float kStatusDividerAlpha = 0.18f;
	static const float kTmuxLabelAlpha     = 0.65f;
	static const float kTmuxDividerAlpha   = 0.35f;
	static const float kDividerInset       = 0.28f;

	// Segments sit three columns apart: a space, a hairline, a space.
	static const std::size_t kSegmentGapCells = 3;

	static StatusInk quietInk(const Palette& palette) {
		StatusInk ink;
		ink.fill          = blend(palette.background, palette.foreground, kStatusFillTint);
		ink.edge          = blend(palette.background, palette.foreground, kStatusEdgeTint);
		ink.text          = palette.foreground;
		ink.label_alpha   = kStatusLabelAlpha;
		ink.value_alpha   = kStatusValueAlpha;
		ink.divider_alpha = kStatusDividerAlpha;
		ink.tinted        = true;
		return ink;
	}

	static StatusInk tmuxInk(const Palette& palette) {
		StatusInk ink;
		ink.fill          = palette.ansi[2];
		ink.edge          = palette.ansi[2];
		ink.text          = palette.background;
		ink.label_alpha   = kTmuxLabelAlpha;
		ink.value_alpha   = 1.0f;
		ink.divider_alpha = kTmuxDividerAlpha;
		ink.tinted        = false;
		return ink;
	}

	static std::size_t rowCells(const std::vector<StatusSegment>& segments, std::size_t first) {
		std::size_t cells = 0;
		for (std::size_t index = first; index < segments.size(); ++index) {
			if (index > first) cells += kSegmentGapCells;

			cells += segmentCells(segments[index]);
		}

		return cells;
	}

	// Segments are dropped from the front until the rest fits, the way
	// tmux cuts status-right: the clock at the far end stays, a util's
	// segment goes first.
	static std::size_t firstSegmentThatFits(const std::vector<StatusSegment>& segments,
			std::size_t cells) {
		std::size_t first = segments.size();
		while (first > 0 && rowCells(segments, first - 1) <= cells) --first;

		return first;
	}

	void Renderer::drawStatusBar(const StatusBarCanvas& canvas) {
		if (!prepareBrush(canvas.target)) return;

		const StatusInk ink = canvas.tmux ? tmuxInk(config_.palette) : quietInk(config_.palette);
		drawStatusBackdrop(canvas, ink);

		canvas.target->PushAxisAlignedClip(canvas.bounds, D2D1_ANTIALIAS_MODE_ALIASED);

		const float cell = font_.metrics().width;
		const float left_edge  = canvas.bounds.left + padding();
		const float right_edge = canvas.bounds.right - padding();
		const float taken = drawStatusRow(canvas, ink, canvas.left, 0, left_edge);

		const float gap = taken > left_edge ? static_cast<float>(kSegmentGapCells) * cell : 0.0f;
		const float room = right_edge - taken - gap;
		const auto cells = static_cast<std::size_t>(std::max(room, 0.0f) / cell);
		const std::size_t first = firstSegmentThatFits(canvas.right, cells);
		const float width = static_cast<float>(rowCells(canvas.right, first)) * cell;
		drawStatusRow(canvas, ink, canvas.right, first, right_edge - width);

		canvas.target->PopAxisAlignedClip();
	}

	void Renderer::drawStatusBackdrop(const StatusBarCanvas& canvas, const StatusInk& ink) {
		const D2D1_RECT_F& bounds = canvas.bounds;

		setBrushColor(ink.fill, 1.0f);
		canvas.target->FillRectangle(bounds, brush_.Get());

		setBrushColor(ink.edge, 1.0f);
		canvas.target->FillRectangle(
			D2D1::RectF(bounds.left, bounds.top, bounds.right, bounds.top + 1.0f), brush_.Get());
	}

	// Returns where the row ends, so the other end knows how far it may go.
	float Renderer::drawStatusRow(const StatusBarCanvas& canvas, const StatusInk& ink,
			const std::vector<StatusSegment>& segments, std::size_t first, float left) {
		const float gap = static_cast<float>(kSegmentGapCells) * font_.metrics().width;
		float x = left;

		for (std::size_t index = first; index < segments.size(); ++index) {
			if (index > first) {
				drawStatusDivider(canvas, ink, x + gap * 0.5f);
				x += gap;
			}

			x = drawStatusSegment(canvas, ink, segments[index], x);
		}

		return x;
	}

	float Renderer::drawStatusSegment(const StatusBarCanvas& canvas, const StatusInk& ink,
			const StatusSegment& segment, float left) {
		float x = left;
		if (!segment.label.empty()) {
			setBrushColor(ink.text, ink.label_alpha);
			x = drawStatusRun(canvas, segment.label, x) + font_.metrics().width;
		}

		if (ink.tinted && segment.tint != kTintPlain) {
			setBrushColor(config_.palette.ansi[segment.tint], 1.0f);
		} else {
			setBrushColor(ink.text, ink.value_alpha);
		}

		return drawStatusRun(canvas, segment.value, x);
	}

	float Renderer::drawStatusRun(const StatusBarCanvas& canvas, const std::string& text,
			float left) {
		const std::wstring wide = widenUtf8(text);
		const CellMetrics& cell = font_.metrics();
		const float width = static_cast<float>(wide.size()) * cell.width;

		canvas.target->DrawText(wide.c_str(), static_cast<UINT32>(wide.size()),
			font_.format(false, false),
			D2D1::RectF(left, canvas.bounds.top, left + width + cell.width, canvas.bounds.bottom),
			brush_.Get(), text_options_);

		return left + width;
	}

	// A hairline between segments, shorter than the row so it reads as a
	// mark rather than a wall.
	void Renderer::drawStatusDivider(const StatusBarCanvas& canvas, const StatusInk& ink,
			float center) {
		const float inset = (canvas.bounds.bottom - canvas.bounds.top) * kDividerInset;
		const float x = std::floor(center);

		setBrushColor(ink.text, ink.divider_alpha);
		canvas.target->FillRectangle(
			D2D1::RectF(x, canvas.bounds.top + inset, x + 1.0f, canvas.bounds.bottom - inset),
			brush_.Get());
	}

	// macOS's own three, and the grey they all go when the window is not
	// the one being used.
	static const std::uint32_t kCloseRed    = 0xFF5F57;
	static const std::uint32_t kMinimizeAmber = 0xFEBC2E;
	static const std::uint32_t kZoomGreen   = 0x28C840;

	static std::uint32_t lightColor(TitleButton which) {
		switch (which) {
		case TitleButton::Close:    return kCloseRed;
		case TitleButton::Minimize: return kMinimizeAmber;
		case TitleButton::Zoom:     return kZoomGreen;
		default:                    return kCloseRed;
		}
	}

	// A pressed light darkens, the way a pressed one does on a Mac.
	static std::uint32_t pressedShade(std::uint32_t color) {
		return blend(color, 0x000000, 0.25f);
	}

	void Renderer::drawTitleGlyph(const TitleBarCanvas& canvas, TitleButton which) {
		const D2D1_ELLIPSE circle = canvas.bar->circleOf(which);
		const float reach = circle.radiusX * 0.5f;
		const float x = circle.point.x;
		const float y = circle.point.y;

		setBrushColor(blend(lightColor(which), 0x000000, 0.62f), 1.0f);

		if (which == TitleButton::Minimize) {
			canvas.target->DrawLine(D2D1::Point2F(x - reach, y), D2D1::Point2F(x + reach, y),
				brush_.Get(), 1.3f);
			return;
		}

		if (which == TitleButton::Close) {
			canvas.target->DrawLine(D2D1::Point2F(x - reach, y - reach),
				D2D1::Point2F(x + reach, y + reach), brush_.Get(), 1.3f);
			canvas.target->DrawLine(D2D1::Point2F(x - reach, y + reach),
				D2D1::Point2F(x + reach, y - reach), brush_.Get(), 1.3f);
			return;
		}

		drawZoomArrows(canvas, circle, reach);
	}

	// Two right triangles at opposite corners with a clear diagonal band
	// between them: the Mac's zoom mark. They sit on the main diagonal
	// to grow the window and on the other one to put it back, which is
	// the whole of the difference between the two states.
	void Renderer::drawZoomArrows(const TitleBarCanvas& canvas, const D2D1_ELLIPSE& circle,
			float reach) {
		const float x = circle.point.x;
		const float y = circle.point.y;
		const float leg = reach * 1.3f;
		const float side = canvas.zoomed ? -1.0f : 1.0f;

		Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
		if (FAILED(factory_->CreatePathGeometry(path.GetAddressOf()))) return;

		Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
		if (FAILED(path->Open(sink.GetAddressOf()))) return;

		const float far_x = x - reach * side;
		sink->BeginFigure(D2D1::Point2F(far_x, y - reach), D2D1_FIGURE_BEGIN_FILLED);
		sink->AddLine(D2D1::Point2F(far_x + leg * side, y - reach));
		sink->AddLine(D2D1::Point2F(far_x, y - reach + leg));
		sink->EndFigure(D2D1_FIGURE_END_CLOSED);

		const float near_x = x + reach * side;
		sink->BeginFigure(D2D1::Point2F(near_x, y + reach), D2D1_FIGURE_BEGIN_FILLED);
		sink->AddLine(D2D1::Point2F(near_x - leg * side, y + reach));
		sink->AddLine(D2D1::Point2F(near_x, y + reach - leg));
		sink->EndFigure(D2D1_FIGURE_END_CLOSED);

		if (FAILED(sink->Close())) return;

		canvas.target->FillGeometry(path.Get(), brush_.Get());
	}

	void Renderer::drawTitleLights(const TitleBarCanvas& canvas) {
		const TitleBar& bar = *canvas.bar;
		const std::uint32_t asleep = blend(config_.palette.background,
			config_.palette.foreground, 0.22f);

		for (int index = 0; index < titleButtonCount(); ++index) {
			const TitleButton which = titleButtonByIndex(index);
			const bool lit = bar.active() || bar.hovered() != TitleButton::None;
			const std::uint32_t color = lit ? lightColor(which) : asleep;

			setBrushColor(bar.pressed() == which ? pressedShade(color) : color, 1.0f);
			canvas.target->FillEllipse(bar.circleOf(which), brush_.Get());

			if (bar.hovered() != TitleButton::None) drawTitleGlyph(canvas, which);
		}
	}

	// The caption sits a shade above the grid rather than beside it: one
	// surface, the way a Mac window reads, with a hairline to part them.
	void Renderer::drawTitleBar(const TitleBarCanvas& canvas) {
		if (canvas.bar == nullptr || !prepareBrush(canvas.target)) return;

		const D2D1_RECT_F& bounds = canvas.bar->bounds();
		if (bounds.bottom <= bounds.top) return;

		setBrushColor(blend(config_.palette.background, config_.palette.foreground, 0.05f),
			1.0f);
		canvas.target->FillRectangle(bounds, brush_.Get());

		setBrushColor(blend(config_.palette.background, config_.palette.foreground, 0.16f),
			1.0f);
		canvas.target->FillRectangle(
			D2D1::RectF(bounds.left, bounds.bottom - 1.0f, bounds.right, bounds.bottom),
			brush_.Get());

		drawTitleLights(canvas);
		drawTitleText(canvas);
	}

	void Renderer::drawTitleText(const TitleBarCanvas& canvas) {
		IDWriteTextFormat* format = font_.captionFormat();
		if (format == nullptr || canvas.title.empty()) return;

		const D2D1_RECT_F& bounds = canvas.bar->bounds();
		const float inset = canvas.bar->clusterWidth();

		setBrushColor(blend(config_.palette.background, config_.palette.foreground,
			canvas.bar->active() ? 0.72f : 0.40f), 1.0f);

		canvas.target->DrawText(canvas.title.c_str(),
			static_cast<UINT32>(canvas.title.size()), format,
			D2D1::RectF(bounds.left + inset, bounds.top, bounds.right - inset, bounds.bottom),
			brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
	}

	void Renderer::drawRowBackgrounds(const PaneCanvas& canvas, int absolute_row,
			int viewport_row) {
		const CellMetrics& cell_box = font_.metrics();
		const D2D1_POINT_2F origin = originOf(canvas);
		const int columns = canvas.screen->columns();
		int run_start = 0;

		for (int column = 0; column <= columns; ++column) {
			const bool at_end = column == columns;
			const std::uint32_t run_color = backgroundFor(canvas, absolute_row, run_start);
			if (!at_end && backgroundFor(canvas, absolute_row, column) == run_color) continue;

			if (run_color != config_.palette.background) {
				setBrushColor(run_color, 1.0f);
				const D2D1_RECT_F box = D2D1::RectF(
					origin.x + static_cast<float>(run_start) * cell_box.width,
					origin.y + static_cast<float>(viewport_row) * cell_box.height,
					origin.x + static_cast<float>(column) * cell_box.width,
					origin.y + static_cast<float>(viewport_row + 1) * cell_box.height);
				canvas.target->FillRectangle(box, brush_.Get());
			}

			run_start = column;
		}
	}

	// A cell drawn in the configured face advances exactly one cell, so a
	// run of them can be shaped as one. Wide characters, clusters and
	// anything a fallback face has to supply advance as far as that face
	// says, which drifts off the grid, so each is drawn in its own cells.
	bool Renderer::sitsOnGrid(const Screen& screen, const Cell& cell) const {
		if ((cell.attributes & kAttrWide) != 0) return false;
		if (screen.clusterOf(cell) != nullptr) return false;
		return font_.coversCharacter(cell.code);
	}

	void Renderer::drawRowText(const PaneCanvas& canvas, int absolute_row, int viewport_row) {
		const Screen& screen = *canvas.screen;
		std::wstring run;
		RunPlacement placement;
		placement.row = viewport_row;

		for (int column = 0; column < screen.columns(); ++column) {
			const Cell& cell = screen.cellAt(absolute_row, column);
			if ((cell.attributes & kAttrWideTail) != 0) continue;

			const bool on_grid = sitsOnGrid(screen, cell);
			const Cell& run_style = screen.cellAt(absolute_row, placement.column);
			if (!run.empty() && (!on_grid || !sameStyle(cell, run_style))) {
				drawRun(canvas, run, run_style, placement);
				run.clear();
			}

			if (!on_grid) {
				drawLoneCell(canvas, cell, viewport_row, column);
				continue;
			}

			if (run.empty()) placement.column = column;
			appendCellText(run, screen, cell);
			placement.cells = column - placement.column + 1;
		}

		if (!run.empty()) {
			drawRun(canvas, run, screen.cellAt(absolute_row, placement.column), placement);
		}
	}

	void Renderer::drawLoneCell(const PaneCanvas& canvas, const Cell& cell, int row,
			int column) {
		std::wstring text;
		appendCellText(text, *canvas.screen, cell);

		RunPlacement placement;
		placement.row     = row;
		placement.column  = column;
		placement.cells   = (cell.attributes & kAttrWide) != 0 ? 2 : 1;
		placement.centred = true;
		drawRun(canvas, text, cell, placement);
	}

	// A glyph narrower than its cells sits in their middle, the way a
	// fallback CJK or emoji face is meant to be seen in a monospace grid.
	static float centringOffset(IDWriteTextLayout* layout, float span) {
		DWRITE_TEXT_METRICS metrics{};
		if (FAILED(layout->GetMetrics(&metrics))) return 0.0f;
		return std::max(0.0f, (span - metrics.widthIncludingTrailingWhitespace) / 2.0f);
	}

	// A hyperlink the program did not underline itself gets a dashed line,
	// so it can be told apart from plain text before it is hovered.
	static std::uint16_t underlineFor(const Cell& style) {
		const auto shapes = static_cast<std::uint16_t>(
			style.attributes & (kAttrUnderline | kAttrUnderlineShapes));
		if (shapes != 0 || style.link == 0) return shapes;

		return static_cast<std::uint16_t>(kAttrUnderline | kAttrUnderlineDashed);
	}

	void Renderer::drawRun(const PaneCanvas& canvas, const std::wstring& text, const Cell& style,
			const RunPlacement& placement) {
		if ((style.attributes & kAttrInvisible) != 0) return;

		const std::uint16_t underline = underlineFor(style);
		if (text.find_first_not_of(L' ') == std::wstring::npos && underline == 0) return;

		const CellMetrics& cell_box = font_.metrics();
		const D2D1_POINT_2F origin = originOf(canvas);
		const float left  = origin.x + static_cast<float>(placement.column) * cell_box.width;
		const float top   = origin.y + static_cast<float>(placement.row) * cell_box.height;
		const float right = left + cell_box.width * static_cast<float>(placement.cells);
		const float alpha = (style.attributes & kAttrDim) != 0 ? kDimAlpha : 1.0f;

		setBrushColor(resolveForeground(style), alpha);

		IDWriteTextLayout* layout = layoutFor(text, (style.attributes & kAttrBold) != 0,
			(style.attributes & kAttrItalic) != 0);
		if (layout != nullptr) {
			const float shift = placement.centred ? centringOffset(layout, right - left) : 0.0f;
			canvas.target->DrawTextLayout(D2D1::Point2F(left + shift, top), layout,
				brush_.Get(), text_options_);
		}

		if (underline == 0) return;

		if (style.underline != kDefaultColor) {
			setBrushColor(resolveColor(style.underline, config_.palette.foreground), alpha);
		}

		drawUnderline(canvas.target, underline,
			D2D1::RectF(left, top + cell_box.ascent + 1.5f, right, top + cell_box.height));
	}

	void Renderer::drawCurlyUnderline(ID2D1RenderTarget* target, const D2D1_RECT_F& span) {
		const float amplitude = std::min(1.5f, (span.bottom - span.top) / 2.0f);
		const float middle = span.top + amplitude;
		const float step = 1.0f;

		for (float x = span.left; x < span.right; x += step) {
			const float next = std::min(x + step, span.right);
			target->DrawLine(D2D1::Point2F(x, middle - amplitude * std::sin(x * 0.75f)),
				D2D1::Point2F(next, middle - amplitude * std::sin(next * 0.75f)),
				brush_.Get(), 1.0f);
		}
	}

	void Renderer::drawBrokenUnderline(ID2D1RenderTarget* target, const D2D1_RECT_F& span,
			float on, float off) {
		for (float x = span.left; x < span.right; x += on + off) {
			target->FillRectangle(
				D2D1::RectF(x, span.top - 0.5f, std::min(x + on, span.right), span.top + 0.5f),
				brush_.Get());
		}
	}

	// span.top is where a plain underline sits; the space below it, down
	// to the cell's bottom, is what the double and curly shapes may use.
	void Renderer::drawUnderline(ID2D1RenderTarget* target, std::uint16_t attributes,
			const D2D1_RECT_F& span) {
		if ((attributes & kAttrUnderlineCurly) != 0) {
			drawCurlyUnderline(target, span);
			return;
		}

		if ((attributes & kAttrUnderlineDotted) != 0) {
			drawBrokenUnderline(target, span, 1.0f, 1.0f);
			return;
		}

		if ((attributes & kAttrUnderlineDashed) != 0) {
			drawBrokenUnderline(target, span, 3.0f, 2.0f);
			return;
		}

		target->DrawLine(D2D1::Point2F(span.left, span.top), D2D1::Point2F(span.right, span.top),
			brush_.Get(), 1.0f);
		if ((attributes & kAttrUnderlineDouble) == 0) return;

		const float second = std::min(span.top + 2.0f, span.bottom - 0.5f);
		target->DrawLine(D2D1::Point2F(span.left, second), D2D1::Point2F(span.right, second),
			brush_.Get(), 1.0f);
	}

	// A program's DECSCUSR wins over the configured style while it lasts.
	// Whether a blinking shape actually blinks is still the window's
	// call: it drives the phase, and only when blinking is configured.
	Renderer::CursorLook Renderer::cursorLookFor(CursorShape shape) const {
		switch (shape) {
		case CursorShape::kBlinkingBlock:     return { CursorStyle::Block, true };
		case CursorShape::kSteadyBlock:       return { CursorStyle::Block, false };
		case CursorShape::kBlinkingUnderline: return { CursorStyle::Underline, true };
		case CursorShape::kSteadyUnderline:   return { CursorStyle::Underline, false };
		case CursorShape::kBlinkingBar:       return { CursorStyle::Bar, true };
		case CursorShape::kSteadyBar:         return { CursorStyle::Bar, false };
		default:                              return { config_.cursor.style, true };
		}
	}

	// On the first half of a wide character the cursor covers both its
	// cells, as the character does.
	static int cursorCells(const Screen& screen) {
		const CursorState& cursor = screen.cursor();
		const Cell& cell = screen.cell(cursor.row, cursor.column);
		const bool wide = (cell.attributes & kAttrWide) != 0
			&& cursor.column + 1 < screen.columns();
		return wide ? 2 : 1;
	}

	// Scrolled back past the cursor's row there is no live cursor to show.
	// An unfocused pane gets a hollow box that never blinks, the way vim
	// and tmux mark theirs.
	void Renderer::drawCursor(const PaneCanvas& canvas) {
		const CursorState& cursor = canvas.screen->cursor();
		const int viewport_row = canvas.screen->scrollbackRows() + cursor.row
			- canvas.view->topRow(*canvas.screen);
		if (!cursor.visible || viewport_row < 0 || viewport_row >= canvas.screen->rows()) return;
		if (cursor.column >= canvas.screen->columns()) return;

		const CellMetrics& cell_box = font_.metrics();
		const D2D1_POINT_2F origin = originOf(canvas);
		const float left = origin.x + static_cast<float>(cursor.column) * cell_box.width;
		const float top  = origin.y + static_cast<float>(viewport_row) * cell_box.height;
		const float width = cell_box.width * static_cast<float>(cursorCells(*canvas.screen));

		setBrushColor(config_.palette.cursor, 1.0f);

		if (!canvas.focused) {
			canvas.target->DrawRectangle(
				D2D1::RectF(left + 0.5f, top + 0.5f, left + width - 0.5f,
					top + cell_box.height - 0.5f), brush_.Get(), 1.0f);
			return;
		}

		const CursorLook look = cursorLookFor(canvas.screen->cursorShape());
		if (look.blinks && !cursor_phase_) return;

		if (look.style == CursorStyle::Bar) {
			canvas.target->FillRectangle(
				D2D1::RectF(left, top, left + 2.0f, top + cell_box.height), brush_.Get());
			return;
		}

		if (look.style == CursorStyle::Underline) {
			canvas.target->FillRectangle(
				D2D1::RectF(left, top + cell_box.height - 2.0f, left + width,
					top + cell_box.height), brush_.Get());
			return;
		}

		drawBlockCursor(canvas, D2D1::RectF(left, top, left + width, top + cell_box.height));
	}

	void Renderer::drawBlockCursor(const PaneCanvas& canvas, const D2D1_RECT_F& box) {
		const CellMetrics& cell_box = font_.metrics();
		const CursorState& cursor = canvas.screen->cursor();

		canvas.target->FillRectangle(box, brush_.Get());

		const Cell& cell = canvas.screen->cell(cursor.row, cursor.column);
		if (cell.code == U' ') return;

		std::wstring text;
		appendCellText(text, *canvas.screen, cell);
		setBrushColor(resolveBackground(cell), 1.0f);

		IDWriteTextFormat* format = font_.format((cell.attributes & kAttrBold) != 0, false);
		canvas.target->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format,
			D2D1::RectF(box.left, box.top, box.left + cell_box.width * 3.0f, box.bottom),
			brush_.Get(), text_options_);
	}

} /* namespace wbshterm */

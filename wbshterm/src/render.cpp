/**
 * @file render.cpp
 * @brief Row backgrounds, then glyph runs, then the cursor.
 */

#include "render.h"

#include <algorithm>

#pragma comment(lib, "d2d1.lib")

namespace wbshterm {

	static const float kDimAlpha = 0.65f;

	static D2D1_COLOR_F toColorF(std::uint32_t rgb, float alpha) {
		const float red   = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f;
		const float green = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f;
		const float blue  = static_cast<float>(rgb & 0xFF) / 255.0f;
		return D2D1::ColorF(red, green, blue, alpha);
	}

	static bool sameStyle(const Cell& left, const Cell& right) {
		return left.foreground == right.foreground
			&& left.background == right.background
			&& left.attributes == right.attributes;
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

		return font_.create(config.font, out_error);
	}

	void Renderer::applyConfig(const Config& config) {
		config_ = config;
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
		canvas.target->Clear(toColorF(config_.palette.background, 1.0f));

		const int top = canvas.view->topRow(*canvas.screen);
		for (int row = 0; row < canvas.screen->rows(); ++row) {
			drawRowBackgrounds(canvas, top + row, row);
			drawRowText(canvas, top + row, row);
		}

		drawCursor(canvas);

		canvas.target->PopAxisAlignedClip();
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

	static std::wstring widenAscii(const std::string& text) {
		std::wstring wide;
		for (unsigned char letter : text) wide.push_back(static_cast<wchar_t>(letter));
		return wide;
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
			drawPickerRow(canvas, widenAscii(line),
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

		drawPickerRow(canvas, widenAscii(pickerHeader(picker)), top, width, false);
		drawPickerMatches(canvas, picker, top, width, visible_rows);

		canvas.target->PopAxisAlignedClip();
	}

	// tmux paints its status bar black on green, and a theme's own green
	// keeps that recognisable without pinning the colour to one palette.
	void Renderer::drawStatusBar(ID2D1RenderTarget* target, const D2D1_RECT_F& bounds,
			const std::string& left, const std::string& right) {
		if (!prepareBrush(target)) return;

		setBrushColor(config_.palette.ansi[2], 1.0f);
		target->FillRectangle(bounds, brush_.Get());

		setBrushColor(config_.palette.background, 1.0f);
		drawStatusText(target, bounds, left, false);
		drawStatusText(target, bounds, right, true);
	}

	void Renderer::drawStatusText(ID2D1RenderTarget* target, const D2D1_RECT_F& bounds,
			const std::string& text, bool to_the_right) {
		if (text.empty()) return;

		const std::wstring wide = widenAscii(text);
		const CellMetrics& cell = font_.metrics();
		const float width = static_cast<float>(wide.size()) * cell.width;
		const float left = to_the_right
			? bounds.right - padding() - width
			: bounds.left + padding();

		target->PushAxisAlignedClip(bounds, D2D1_ANTIALIAS_MODE_ALIASED);
		target->DrawText(wide.c_str(), static_cast<UINT32>(wide.size()),
			font_.format(false, false),
			D2D1::RectF(left, bounds.top, left + width + cell.width, bounds.bottom),
			brush_.Get(), text_options_);
		target->PopAxisAlignedClip();
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

	void Renderer::drawRowText(const PaneCanvas& canvas, int absolute_row, int viewport_row) {
		const Screen& screen = *canvas.screen;
		std::wstring run;
		int run_start = 0;

		for (int column = 0; column < screen.columns(); ++column) {
			const Cell& cell = screen.cellAt(absolute_row, column);
			if ((cell.attributes & kAttrWideTail) != 0) continue;

			if (!run.empty() && !sameStyle(cell, screen.cellAt(absolute_row, run_start))) {
				drawRun(canvas, run, screen.cellAt(absolute_row, run_start), viewport_row,
					run_start);
				run.clear();
			}

			if (run.empty()) run_start = column;
			appendUtf16(run, cell.code);
		}

		if (!run.empty()) {
			drawRun(canvas, run, screen.cellAt(absolute_row, run_start), viewport_row, run_start);
		}
	}

	void Renderer::drawRun(const PaneCanvas& canvas, const std::wstring& text, const Cell& style,
			int row, int column) {
		if ((style.attributes & kAttrInvisible) != 0) return;
		if (text.find_first_not_of(L' ') == std::wstring::npos
			&& (style.attributes & kAttrUnderline) == 0) {
			return;
		}

		const CellMetrics& cell_box = font_.metrics();
		const D2D1_POINT_2F origin = originOf(canvas);
		const float left = origin.x + static_cast<float>(column) * cell_box.width;
		const float top  = origin.y + static_cast<float>(row) * cell_box.height;
		const float alpha = (style.attributes & kAttrDim) != 0 ? kDimAlpha : 1.0f;

		setBrushColor(resolveForeground(style), alpha);

		const D2D1_RECT_F box = D2D1::RectF(left, top,
			left + cell_box.width * static_cast<float>(text.size() + 2),
			top + cell_box.height);

		IDWriteTextFormat* format = font_.format((style.attributes & kAttrBold) != 0,
			(style.attributes & kAttrItalic) != 0);
		canvas.target->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format, box,
			brush_.Get(), text_options_);

		if ((style.attributes & kAttrUnderline) == 0) return;

		const float line_y = top + cell_box.ascent + 1.5f;
		canvas.target->DrawLine(D2D1::Point2F(left, line_y),
			D2D1::Point2F(left + cell_box.width * static_cast<float>(text.size()), line_y),
			brush_.Get(), 1.0f);
	}

	// Scrolled back into history there is no live cursor to show: the one
	// on screen belongs to the bottom of the buffer. An unfocused pane gets
	// a hollow box that never blinks, the way vim and tmux mark theirs.
	void Renderer::drawCursor(const PaneCanvas& canvas) {
		const CursorState& cursor = canvas.screen->cursor();
		if (!cursor.visible || canvas.view->scrollOffset() != 0) return;
		if (cursor.row >= canvas.screen->rows()) return;
		if (cursor.column >= canvas.screen->columns()) return;

		const CellMetrics& cell_box = font_.metrics();
		const D2D1_POINT_2F origin = originOf(canvas);
		const float left = origin.x + static_cast<float>(cursor.column) * cell_box.width;
		const float top  = origin.y + static_cast<float>(cursor.row) * cell_box.height;

		setBrushColor(config_.palette.cursor, 1.0f);

		if (!canvas.focused) {
			canvas.target->DrawRectangle(
				D2D1::RectF(left + 0.5f, top + 0.5f, left + cell_box.width - 0.5f,
					top + cell_box.height - 0.5f), brush_.Get(), 1.0f);
			return;
		}

		if (!cursor_phase_) return;

		if (config_.cursor.style == CursorStyle::Bar) {
			canvas.target->FillRectangle(
				D2D1::RectF(left, top, left + 2.0f, top + cell_box.height), brush_.Get());
			return;
		}

		if (config_.cursor.style == CursorStyle::Underline) {
			canvas.target->FillRectangle(
				D2D1::RectF(left, top + cell_box.height - 2.0f, left + cell_box.width,
					top + cell_box.height), brush_.Get());
			return;
		}

		drawBlockCursor(canvas, left, top);
	}

	void Renderer::drawBlockCursor(const PaneCanvas& canvas, float left, float top) {
		const CellMetrics& cell_box = font_.metrics();
		const CursorState& cursor = canvas.screen->cursor();

		canvas.target->FillRectangle(
			D2D1::RectF(left, top, left + cell_box.width, top + cell_box.height), brush_.Get());

		const Cell& cell = canvas.screen->cell(cursor.row, cursor.column);
		if (cell.code == U' ') return;

		std::wstring text;
		appendUtf16(text, cell.code);
		setBrushColor(resolveBackground(cell), 1.0f);

		IDWriteTextFormat* format = font_.format((cell.attributes & kAttrBold) != 0, false);
		canvas.target->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format,
			D2D1::RectF(left, top, left + cell_box.width * 3.0f, top + cell_box.height),
			brush_.Get(), text_options_);
	}

} /* namespace wbshterm */

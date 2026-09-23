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

	void Renderer::draw(ID2D1RenderTarget* target, const Screen& screen,
			const TerminalView& view) {
		if (!prepareBrush(target)) return;

		target->Clear(toColorF(config_.palette.background, 1.0f));

		const int top = view.topRow(screen);
		for (int row = 0; row < screen.rows(); ++row) {
			drawRowBackgrounds(target, screen, view, top + row, row);
			drawRowText(target, screen, view, top + row, row);
		}

		drawCursor(target, screen, view);
	}

	std::uint32_t Renderer::backgroundFor(const Screen& screen, const TerminalView& view,
			int absolute_row, int column) const {
		if (view.isSelected(absolute_row, column)) return config_.palette.selection;
		return resolveBackground(screen.cellAt(absolute_row, column));
	}

	static std::wstring widenForPicker(const std::string& text) {
		std::wstring wide;
		for (unsigned char letter : text) wide.push_back(static_cast<wchar_t>(letter));
		return wide;
	}

	void Renderer::drawPickerRow(ID2D1RenderTarget* target, const std::wstring& text, float top,
			float width, bool highlighted) {
		const CellMetrics& cell = font_.metrics();
		const float left = padding();

		if (highlighted) {
			setBrushColor(config_.palette.selection, 1.0f);
			target->FillRectangle(D2D1::RectF(left, top, left + width, top + cell.height),
				brush_.Get());
		}

		setBrushColor(config_.palette.foreground, 1.0f);
		target->DrawText(text.c_str(), static_cast<UINT32>(text.size()), font_.format(false, false),
			D2D1::RectF(left, top, left + width, top + cell.height), brush_.Get(), text_options_);
	}

	// The overlay sits at the bottom, over whatever the grid was showing, so
	// the command being typed stays visible above it.
	void Renderer::drawPicker(ID2D1RenderTarget* target, const Screen& screen,
			const Picker& picker) {
		if (!picker.active() || !prepareBrush(target)) return;

		const CellMetrics& cell = font_.metrics();
		const int visible_rows = std::min(static_cast<int>(picker.matches().size()), 10);
		const int rows = visible_rows + 1;

		const float width = static_cast<float>(screen.columns()) * cell.width;
		const float height = static_cast<float>(rows) * cell.height;
		const float top = padding() + static_cast<float>(screen.rows()) * cell.height - height;

		setBrushColor(config_.palette.background, 1.0f);
		target->FillRectangle(
			D2D1::RectF(padding(), top, padding() + width, top + height), brush_.Get());

		setBrushColor(config_.palette.cursor, 1.0f);
		target->DrawLine(D2D1::Point2F(padding(), top), D2D1::Point2F(padding() + width, top),
			brush_.Get(), 1.0f);

		const std::string header = "  " + picker.prompt() + " > " + picker.query()
			+ "    [" + std::to_string(picker.matches().size()) + "/"
			+ std::to_string(picker.itemCount()) + "]";
		drawPickerRow(target, widenForPicker(header), top, width, false);

		const int first = std::max(0, picker.selected() - visible_rows + 1);
		for (int row = 0; row < visible_rows; ++row) {
			const int match = first + row;
			if (match >= static_cast<int>(picker.matches().size())) break;

			const bool current = match == picker.selected();
			const std::string line = (current ? "> " : "  ")
				+ picker.item(picker.matches()[static_cast<std::size_t>(match)]);
			drawPickerRow(target, widenForPicker(line),
				top + static_cast<float>(row + 1) * cell.height, width, current);
		}
	}

	void Renderer::drawRowBackgrounds(ID2D1RenderTarget* target, const Screen& screen,
			const TerminalView& view, int absolute_row, int viewport_row) {
		const CellMetrics& cell_box = font_.metrics();
		int run_start = 0;

		for (int column = 0; column <= screen.columns(); ++column) {
			const bool at_end = column == screen.columns();
			const std::uint32_t run_color = backgroundFor(screen, view, absolute_row, run_start);
			if (!at_end && backgroundFor(screen, view, absolute_row, column) == run_color) continue;

			if (run_color != config_.palette.background) {
				setBrushColor(run_color, 1.0f);
				const float pad = padding();
				const D2D1_RECT_F box = D2D1::RectF(
					pad + static_cast<float>(run_start) * cell_box.width,
					pad + static_cast<float>(viewport_row) * cell_box.height,
					pad + static_cast<float>(column) * cell_box.width,
					pad + static_cast<float>(viewport_row + 1) * cell_box.height);
				target->FillRectangle(box, brush_.Get());
			}

			run_start = column;
		}
	}

	void Renderer::drawRowText(ID2D1RenderTarget* target, const Screen& screen,
			const TerminalView& view, int absolute_row, int viewport_row) {
		std::wstring run;
		int run_start = 0;

		for (int column = 0; column < screen.columns(); ++column) {
			const Cell& cell = screen.cellAt(absolute_row, column);
			if ((cell.attributes & kAttrWideTail) != 0) continue;

			if (!run.empty() && !sameStyle(cell, screen.cellAt(absolute_row, run_start))) {
				drawRun(target, run, screen.cellAt(absolute_row, run_start), viewport_row,
					run_start);
				run.clear();
			}

			if (run.empty()) run_start = column;
			appendUtf16(run, cell.code);
		}

		if (!run.empty()) {
			drawRun(target, run, screen.cellAt(absolute_row, run_start), viewport_row, run_start);
		}

		(void)view;
	}

	void Renderer::drawRun(ID2D1RenderTarget* target, const std::wstring& text, const Cell& style,
			int row, int column) {
		if ((style.attributes & kAttrInvisible) != 0) return;
		if (text.find_first_not_of(L' ') == std::wstring::npos
			&& (style.attributes & kAttrUnderline) == 0) {
			return;
		}

		const CellMetrics& cell_box = font_.metrics();
		const float pad = padding();
		const float left = pad + static_cast<float>(column) * cell_box.width;
		const float top  = pad + static_cast<float>(row) * cell_box.height;
		const float alpha = (style.attributes & kAttrDim) != 0 ? kDimAlpha : 1.0f;

		setBrushColor(resolveForeground(style), alpha);

		const D2D1_RECT_F box = D2D1::RectF(left, top,
			left + cell_box.width * static_cast<float>(text.size() + 2),
			top + cell_box.height);

		IDWriteTextFormat* format = font_.format((style.attributes & kAttrBold) != 0,
			(style.attributes & kAttrItalic) != 0);
		target->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format, box, brush_.Get(),
			text_options_);

		if ((style.attributes & kAttrUnderline) == 0) return;

		const float line_y = top + cell_box.ascent + 1.5f;
		target->DrawLine(D2D1::Point2F(left, line_y),
			D2D1::Point2F(left + cell_box.width * static_cast<float>(text.size()), line_y),
			brush_.Get(), 1.0f);
	}

	// Scrolled back into history there is no live cursor to show: the one
	// on screen belongs to the bottom of the buffer.
	void Renderer::drawCursor(ID2D1RenderTarget* target, const Screen& screen,
			const TerminalView& view) {
		const CursorState& cursor = screen.cursor();
		if (!cursor.visible || view.scrollOffset() != 0) return;
		if (cursor.row >= screen.rows() || cursor.column >= screen.columns()) return;

		if (!cursor_phase_) return;

		const CellMetrics& cell_box = font_.metrics();
		const float pad = padding();
		const float left = pad + static_cast<float>(cursor.column) * cell_box.width;
		const float top  = pad + static_cast<float>(cursor.row) * cell_box.height;

		setBrushColor(config_.palette.cursor, 1.0f);

		if (config_.cursor.style == CursorStyle::Bar) {
			target->FillRectangle(
				D2D1::RectF(left, top, left + 2.0f, top + cell_box.height), brush_.Get());
			return;
		}

		if (config_.cursor.style == CursorStyle::Underline) {
			target->FillRectangle(
				D2D1::RectF(left, top + cell_box.height - 2.0f, left + cell_box.width,
					top + cell_box.height), brush_.Get());
			return;
		}

		target->FillRectangle(
			D2D1::RectF(left, top, left + cell_box.width, top + cell_box.height), brush_.Get());

		const Cell& cell = screen.cell(cursor.row, cursor.column);
		if (cell.code == U' ') return;

		std::wstring text;
		appendUtf16(text, cell.code);
		setBrushColor(resolveBackground(cell), 1.0f);

		IDWriteTextFormat* format = font_.format((cell.attributes & kAttrBold) != 0, false);
		target->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format,
			D2D1::RectF(left, top, left + cell_box.width * 3.0f, top + cell_box.height),
			brush_.Get(), text_options_);
	}

} /* namespace wbshterm */

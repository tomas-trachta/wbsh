/**
 * @file render.cpp
 * @brief Row backgrounds, then glyph runs, then the cursor.
 */

#include "render.h"

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

	bool Renderer::create(const std::wstring& font_family, float point_size,
			std::string& out_error) {
		const HRESULT hr = ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
			factory_.GetAddressOf());
		if (FAILED(hr)) {
			out_error = "D2D1CreateFactory failed";
			return false;
		}

		return font_.create(font_family, point_size, out_error);
	}

	std::uint32_t Renderer::resolveForeground(const Cell& cell) const {
		const std::uint32_t color = cell.foreground == kDefaultColor
			? theme_.foreground
			: cell.foreground;
		if ((cell.attributes & kAttrReverse) == 0) return color;

		return cell.background == kDefaultColor ? theme_.background : cell.background;
	}

	std::uint32_t Renderer::resolveBackground(const Cell& cell) const {
		const std::uint32_t color = cell.background == kDefaultColor
			? theme_.background
			: cell.background;
		if ((cell.attributes & kAttrReverse) == 0) return color;

		return cell.foreground == kDefaultColor ? theme_.foreground : cell.foreground;
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
		return true;
	}

	void Renderer::draw(ID2D1RenderTarget* target, const Screen& screen,
			const TerminalView& view) {
		if (!prepareBrush(target)) return;

		target->Clear(toColorF(theme_.background, 1.0f));

		const int top = view.topRow(screen);
		for (int row = 0; row < screen.rows(); ++row) {
			drawRowBackgrounds(target, screen, view, top + row, row);
			drawRowText(target, screen, view, top + row, row);
		}

		drawCursor(target, screen, view);
	}

	std::uint32_t Renderer::backgroundFor(const Screen& screen, const TerminalView& view,
			int absolute_row, int column) const {
		if (view.isSelected(absolute_row, column)) return theme_.selection;
		return resolveBackground(screen.cellAt(absolute_row, column));
	}

	void Renderer::drawRowBackgrounds(ID2D1RenderTarget* target, const Screen& screen,
			const TerminalView& view, int absolute_row, int viewport_row) {
		const CellMetrics& cell_box = font_.metrics();
		int run_start = 0;

		for (int column = 0; column <= screen.columns(); ++column) {
			const bool at_end = column == screen.columns();
			const std::uint32_t run_color = backgroundFor(screen, view, absolute_row, run_start);
			if (!at_end && backgroundFor(screen, view, absolute_row, column) == run_color) continue;

			if (run_color != theme_.background) {
				setBrushColor(run_color, 1.0f);
				const D2D1_RECT_F box = D2D1::RectF(
					static_cast<float>(run_start) * cell_box.width,
					static_cast<float>(viewport_row) * cell_box.height,
					static_cast<float>(column) * cell_box.width,
					static_cast<float>(viewport_row + 1) * cell_box.height);
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
		const float left = static_cast<float>(column) * cell_box.width;
		const float top  = static_cast<float>(row) * cell_box.height;
		const float alpha = (style.attributes & kAttrDim) != 0 ? kDimAlpha : 1.0f;

		setBrushColor(resolveForeground(style), alpha);

		const D2D1_RECT_F box = D2D1::RectF(left, top,
			left + cell_box.width * static_cast<float>(text.size()) + cell_box.width,
			top + cell_box.height);

		IDWriteTextFormat* format = font_.format((style.attributes & kAttrBold) != 0,
			(style.attributes & kAttrItalic) != 0);
		target->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format, box, brush_.Get(),
			D2D1_DRAW_TEXT_OPTIONS_CLIP);

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

		const CellMetrics& cell_box = font_.metrics();
		const float left = static_cast<float>(cursor.column) * cell_box.width;
		const float top  = static_cast<float>(cursor.row) * cell_box.height;

		setBrushColor(theme_.cursor, 1.0f);
		target->FillRectangle(
			D2D1::RectF(left, top, left + cell_box.width, top + cell_box.height), brush_.Get());

		const Cell& cell = screen.cell(cursor.row, cursor.column);
		if (cell.code == U' ') return;

		std::wstring text;
		appendUtf16(text, cell.code);
		setBrushColor(resolveBackground(cell), 1.0f);

		IDWriteTextFormat* format = font_.format((cell.attributes & kAttrBold) != 0, false);
		target->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format,
			D2D1::RectF(left, top, left + cell_box.width * 2.0f, top + cell_box.height),
			brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
	}

} /* namespace wbshterm */

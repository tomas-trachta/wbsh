/**
 * @file render_marks.cpp
 * @brief The marks the shell's OSC 133 blocks leave in a pane: a bar in the
 *        left margin beside each prompt, green or red by how the command
 *        ended, and how long it took at the right end of its command line.
 *
 * Both are drawn over the retained grid every frame rather than into it,
 * so a block that finishes needs no row of the grid redrawn.
 */

#include "render.h"

#include <cstdio>
#include <string>

namespace wbshterm {

	namespace marks_detail {

		static const float kBarLeft    = 0.25f;   // of the padding
		static const float kBarRight   = 0.65f;
		static const float kBarInset   = 0.15f;   // of the row height, top and bottom
		static const float kBarMinimum = 3.0f;    // padding below this has no bar
		static const float kRunningAlpha = 0.7f;
		static const float kLabelAlpha   = 0.6f;
		static const std::uint64_t kSecond = 1000;
		static const std::uint64_t kMinute = 60 * kSecond;
		static const std::uint64_t kHour   = 60 * kMinute;

		static std::wstring durationText(std::uint64_t ms) {
			wchar_t text[32] = {};
			if (ms < kSecond) {
				std::swprintf(text, 32, L"%llums", static_cast<unsigned long long>(ms));
			} else if (ms < kMinute) {
				std::swprintf(text, 32, L"%.1fs", static_cast<double>(ms) / 1000.0);
			} else if (ms < kHour) {
				std::swprintf(text, 32, L"%llum%02llus",
					static_cast<unsigned long long>(ms / kMinute),
					static_cast<unsigned long long>(ms % kMinute / kSecond));
			} else {
				std::swprintf(text, 32, L"%lluh%02llum",
					static_cast<unsigned long long>(ms / kHour),
					static_cast<unsigned long long>(ms % kHour / kMinute));
			}

			return text;
		}

		static bool durationShown(const CommandBlock& block, int threshold_ms) {
			if (threshold_ms < 0 || block.durationMs() == 0) return false;

			return block.durationMs() >= static_cast<std::uint64_t>(threshold_ms);
		}

		// "✗ 2  3.4s": the status only when it failed, the time only once
		// it passes the threshold.
		static std::wstring labelFor(const CommandBlock& block, int threshold_ms) {
			std::wstring label;
			if (block.exit_status > 0) label = L"✗ " + std::to_wstring(block.exit_status);

			if (!durationShown(block, threshold_ms)) return label;
			if (!label.empty()) label += L"  ";

			return label + durationText(block.durationMs());
		}

		static bool cellIsBlank(const Cell& cell) {
			return (cell.code == U' ' || cell.code == 0) && cell.background == kDefaultColor;
		}

		// The label only goes where the row is empty: it never covers
		// anything the shell or a program wrote.
		static bool tailIsBlank(const Screen& screen, int absolute_row, int cells) {
			const int first = screen.columns() - cells - 1;
			if (first < 0) return false;

			for (int column = first; column < screen.columns(); ++column) {
				if (!cellIsBlank(screen.cellAt(absolute_row, column))) return false;
			}

			return true;
		}

	}  // namespace marks_detail

	void Renderer::drawCommandMarks(const PaneCanvas& canvas) {
		const Screen& screen = *canvas.screen;
		if (!config_.shell.marks || screen.onAltScreen()) return;
		if (!prepareBrush(canvas.target)) return;

		const int top = canvas.view->topRow(screen);
		canvas.target->PushAxisAlignedClip(canvas.bounds, D2D1_ANTIALIAS_MODE_ALIASED);
		for (const CommandBlock& block : screen.commandBlocks()) {
			if (block.output_row < 0) continue;

			const int row = block.prompt_row - top;
			if (row + block.command_offset < 0 || row >= screen.rows()) continue;

			drawGutterMark(canvas, block, row);
			drawBlockLabel(canvas, block, row + block.command_offset);
		}

		canvas.target->PopAxisAlignedClip();
	}

	std::uint32_t Renderer::markColor(const CommandBlock& block) const {
		if (!block.finished) return config_.palette.ansi[4];

		return block.exit_status == 0 ? config_.palette.ansi[2] : config_.palette.ansi[1];
	}

	void Renderer::drawGutterMark(const PaneCanvas& canvas, const CommandBlock& block,
			int viewport_row) {
		const float padding = this->padding();
		if (padding < marks_detail::kBarMinimum || viewport_row < 0) return;

		const CellMetrics& cell = font_.metrics();
		const float top = originOf(canvas).y + static_cast<float>(viewport_row) * cell.height;
		const float rows = static_cast<float>(block.command_offset + 1);
		const float inset = cell.height * marks_detail::kBarInset;
		const D2D1_RECT_F bar = D2D1::RectF(
			canvas.bounds.left + padding * marks_detail::kBarLeft, top + inset,
			canvas.bounds.left + padding * marks_detail::kBarRight,
			top + rows * cell.height - inset);
		const float radius = (bar.right - bar.left) * 0.5f;

		setBrushColor(markColor(block), block.finished ? 1.0f : marks_detail::kRunningAlpha);
		canvas.target->FillRoundedRectangle(D2D1::RoundedRect(bar, radius, radius), brush_.Get());
	}

	void Renderer::drawBlockLabel(const PaneCanvas& canvas, const CommandBlock& block,
			int viewport_row) {
		const Screen& screen = *canvas.screen;
		if (!block.finished || viewport_row < 0 || viewport_row >= screen.rows()) return;

		const std::wstring label = marks_detail::labelFor(block, config_.shell.duration_after_ms);
		const int cells = static_cast<int>(label.size());
		const int absolute_row = canvas.view->topRow(screen) + viewport_row;
		if (label.empty() || !marks_detail::tailIsBlank(screen, absolute_row, cells)) return;

		const CellMetrics& cell = font_.metrics();
		const D2D1_POINT_2F origin = originOf(canvas);
		const float left = origin.x + static_cast<float>(screen.columns() - cells) * cell.width;
		const float top = origin.y + static_cast<float>(viewport_row) * cell.height;
		const D2D1_RECT_F box = D2D1::RectF(left, top,
			origin.x + static_cast<float>(screen.columns() + 1) * cell.width, top + cell.height);

		const bool failed = block.exit_status > 0;
		setBrushColor(failed ? config_.palette.ansi[1] : config_.palette.foreground,
			marks_detail::kLabelAlpha);
		canvas.target->DrawText(label.c_str(), static_cast<UINT32>(label.size()),
			font_.format(false, false), box, brush_.Get(), text_options_);
	}

} /* namespace wbshterm */

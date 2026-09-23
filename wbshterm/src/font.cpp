/**
 * @file font.cpp
 * @brief DirectWrite setup and cell measurement.
 */

#include "font.h"

#pragma comment(lib, "dwrite.lib")

namespace wbshterm {

	static const float kPointsPerInch = 72.0f;
	static const float kDipsPerInch   = 96.0f;

	static IDWriteTextFormat* pickFormat(
			const Microsoft::WRL::ComPtr<IDWriteTextFormat>* formats, bool bold, bool italic) {
		const int index = (bold ? 1 : 0) + (italic ? 2 : 0);
		return formats[index].Get();
	}

	bool FontSet::create(const std::wstring& family, float point_size, std::string& out_error) {
		const HRESULT hr = ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
			__uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(factory_.GetAddressOf()));
		if (FAILED(hr)) {
			out_error = "DWriteCreateFactory failed";
			return false;
		}

		const float size_dip = point_size * kDipsPerInch / kPointsPerInch;
		if (!createFormats(family, size_dip, out_error)) return false;
		return measureCell(out_error);
	}

	bool FontSet::createFormats(const std::wstring& family, float size_dip,
			std::string& out_error) {
		const DWRITE_FONT_WEIGHT weights[4] = {
			DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_WEIGHT_BOLD,
			DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_WEIGHT_BOLD,
		};

		const DWRITE_FONT_STYLE styles[4] = {
			DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STYLE_NORMAL,
			DWRITE_FONT_STYLE_ITALIC, DWRITE_FONT_STYLE_ITALIC,
		};

		for (int index = 0; index < 4; ++index) {
			const HRESULT hr = factory_->CreateTextFormat(family.c_str(), nullptr,
				weights[index], styles[index], DWRITE_FONT_STRETCH_NORMAL, size_dip, L"",
				formats_[index].ReleaseAndGetAddressOf());
			if (FAILED(hr)) {
				out_error = "CreateTextFormat failed (is the font installed?)";
				return false;
			}

			formats_[index]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
		}

		return true;
	}

	// Measured from a run of identical glyphs: one 'M' layout rounds its
	// advance to whole pixels, and the error compounds across a row.
	bool FontSet::measureCell(std::string& out_error) {
		static const wchar_t kSample[] = L"MMMMMMMMMM";
		static const UINT32  kSampleLength = 10;

		Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
		const HRESULT hr = factory_->CreateTextLayout(kSample, kSampleLength, formats_[0].Get(),
			10000.0f, 1000.0f, layout.GetAddressOf());
		if (FAILED(hr)) {
			out_error = "CreateTextLayout failed";
			return false;
		}

		DWRITE_TEXT_METRICS text{};
		DWRITE_LINE_METRICS line{};
		UINT32 line_count = 1;
		if (FAILED(layout->GetMetrics(&text))
			|| FAILED(layout->GetLineMetrics(&line, 1, &line_count))) {
			out_error = "text metrics unavailable";
			return false;
		}

		metrics_.width  = text.widthIncludingTrailingWhitespace / static_cast<float>(kSampleLength);
		metrics_.height = line.height;
		metrics_.ascent = line.baseline;
		return true;
	}

	IDWriteTextFormat* FontSet::format(bool bold, bool italic) const {
		return pickFormat(formats_, bold, italic);
	}

} /* namespace wbshterm */

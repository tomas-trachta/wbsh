/**
 * @file font.cpp
 * @brief DirectWrite setup and cell measurement.
 */

#include "font.h"

#include <algorithm>
#include <iterator>

#pragma comment(lib, "dwrite.lib")

namespace wbshterm {

	static const float kPointsPerInch = 72.0f;
	static const float kDipsPerInch   = 96.0f;

	static IDWriteTextFormat* pickFormat(
			const Microsoft::WRL::ComPtr<IDWriteTextFormat>* formats, bool bold, bool italic) {
		const int index = (bold ? 1 : 0) + (italic ? 2 : 0);
		return formats[index].Get();
	}

	static bool localizedName(IDWriteLocalizedStrings* names, std::wstring& out_name) {
		UINT32 index = 0;
		BOOL exists = FALSE;
		if (FAILED(names->FindLocaleName(L"en-us", &index, &exists)) || !exists) index = 0;

		UINT32 length = 0;
		if (FAILED(names->GetStringLength(index, &length))) return false;

		out_name.assign(static_cast<std::size_t>(length) + 1, L'\0');
		if (FAILED(names->GetString(index, out_name.data(), length + 1))) return false;

		out_name.resize(length);
		return !out_name.empty();
	}

	static bool familyIsMonospaced(IDWriteFontFamily* family) {
		Microsoft::WRL::ComPtr<IDWriteFont> font;
		if (FAILED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL,
				DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, font.GetAddressOf()))) {
			return false;
		}

		Microsoft::WRL::ComPtr<IDWriteFont1> font1;
		if (FAILED(font.As(&font1))) return false;

		return font1->IsMonospacedFont() != FALSE;
	}

	static bool familyName(IDWriteFontFamily* family, std::wstring& out_name) {
		Microsoft::WRL::ComPtr<IDWriteLocalizedStrings> names;
		if (FAILED(family->GetFamilyNames(names.GetAddressOf()))) return false;

		return localizedName(names.Get(), out_name);
	}

	static bool caseInsensitiveLess(const std::wstring& left, const std::wstring& right) {
		return ::CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_LESS_THAN;
	}

	std::vector<std::wstring> installedMonospaceFamilies() {
		std::vector<std::wstring> families;

		Microsoft::WRL::ComPtr<IDWriteFactory> factory;
		if (FAILED(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
				reinterpret_cast<IUnknown**>(factory.GetAddressOf())))) {
			return families;
		}

		Microsoft::WRL::ComPtr<IDWriteFontCollection> collection;
		if (FAILED(factory->GetSystemFontCollection(collection.GetAddressOf(), FALSE))) {
			return families;
		}

		const UINT32 count = collection->GetFontFamilyCount();
		for (UINT32 index = 0; index < count; ++index) {
			Microsoft::WRL::ComPtr<IDWriteFontFamily> family;
			if (FAILED(collection->GetFontFamily(index, family.GetAddressOf()))) continue;
			if (!familyIsMonospaced(family.Get())) continue;

			std::wstring name;
			if (familyName(family.Get(), name)) families.push_back(name);
		}

		std::sort(families.begin(), families.end(), caseInsensitiveLess);
		families.erase(std::unique(families.begin(), families.end()), families.end());
		return families;
	}

	bool FontSet::create(const FontSettings& settings, std::string& out_error) {
		const HRESULT hr = ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
			__uuidof(IDWriteFactory2), reinterpret_cast<IUnknown**>(factory_.GetAddressOf()));
		if (FAILED(hr)) {
			out_error = "DWriteCreateFactory failed";
			return false;
		}

		const float size_dip = settings.size * kDipsPerInch / kPointsPerInch;
		if (!createFormats(settings.family, size_dip, out_error)) return false;

		applyFallback(settings.fallback);
		createCaptionFormat();
		if (!measureCell(out_error)) return false;

		metrics_.height *= (settings.line_height > 0.1f ? settings.line_height : 1.0f);
		return true;
	}

	// The caption is chrome, not terminal text: a proportional UI face at a
	// fixed size reads as part of the window rather than part of the grid.
	// Failing to load one is not fatal -- the caption simply goes untitled.
	void FontSet::createCaptionFormat() {
		static const wchar_t* const kFamilies[] = {
			L"Segoe UI Variable Text", L"Segoe UI", L"Tahoma",
		};

		for (const wchar_t* family : kFamilies) {
			const HRESULT hr = factory_->CreateTextFormat(family, nullptr,
				DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
				DWRITE_FONT_STRETCH_NORMAL, 12.5f, L"", caption_.ReleaseAndGetAddressOf());
			if (FAILED(hr)) continue;

			caption_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
			caption_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
			caption_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
			caption_->SetTrimming(nullptr, nullptr);
			return;
		}

		caption_.Reset();
	}

	// A monospace face has no emoji or CJK, and DirectWrite's own fallback
	// does not reach them from a text format, so the mapping is spelled out:
	// the configured families first, then the system's list for the rest.
	void FontSet::applyFallback(const std::vector<std::wstring>& families) {
		if (families.empty()) return;

		Microsoft::WRL::ComPtr<IDWriteFactory2> factory2;
		if (FAILED(factory_.As(&factory2))) return;

		Microsoft::WRL::ComPtr<IDWriteFontFallbackBuilder> builder;
		if (FAILED(factory2->CreateFontFallbackBuilder(builder.GetAddressOf()))) return;

		static const DWRITE_UNICODE_RANGE kRanges[] = {
			{ 0x0E000, 0x0F8FF },   // private use: Nerd Font and Powerline icons
			{ 0x02190, 0x02BFF },   // arrows, symbols, geometric shapes
			{ 0x03000, 0x0D7FF },   // CJK and Hangul
			{ 0x0F900, 0x0FAFF },   // CJK compatibility
			{ 0x0FE00, 0x0FFEF },   // variation selectors, fullwidth forms
			{ 0x1F000, 0x1FAFF },   // emoji
		};

		for (const std::wstring& family : families) {
			const WCHAR* name = family.c_str();
			builder->AddMapping(kRanges, static_cast<UINT32>(std::size(kRanges)), &name, 1,
				nullptr, nullptr, nullptr, 1.0f);
		}

		Microsoft::WRL::ComPtr<IDWriteFontFallback> system_fallback;
		if (SUCCEEDED(factory2->GetSystemFontFallback(system_fallback.GetAddressOf()))) {
			builder->AddMappings(system_fallback.Get());
		}

		Microsoft::WRL::ComPtr<IDWriteFontFallback> fallback;
		if (FAILED(builder->CreateFontFallback(fallback.GetAddressOf()))) return;

		for (Microsoft::WRL::ComPtr<IDWriteTextFormat>& format : formats_) {
			Microsoft::WRL::ComPtr<IDWriteTextFormat1> format1;
			if (SUCCEEDED(format.As(&format1))) format1->SetFontFallback(fallback.Get());
		}
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

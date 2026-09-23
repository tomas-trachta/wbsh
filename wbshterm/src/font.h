#pragma once

/**
 * @file font.h
 * @brief The monospace face, and the cell box its glyphs sit in.
 */

#include <dwrite.h>
#include <wrl/client.h>

#include <string>

namespace wbshterm {

	struct CellMetrics {
		float width   = 8.0f;
		float height  = 16.0f;
		float ascent  = 12.0f;
	};

	/**
	 * @brief Four text formats (regular, bold, italic, bold italic).
	 *
	 * The cell box is measured from the face itself rather than assumed,
	 * so a different family or size stays aligned. Grapheme clustering
	 * and fallback for wide glyphs are M4's problem, not this class's.
	 */
	class FontSet {
	public:
		bool create(const std::wstring& family, float point_size, std::string& out_error);

		IDWriteTextFormat* format(bool bold, bool italic) const;
		IDWriteFactory* factory() const { return factory_.Get(); }
		const CellMetrics& metrics() const { return metrics_; }

	private:
		bool createFormats(const std::wstring& family, float size_dip, std::string& out_error);
		bool measureCell(std::string& out_error);

		Microsoft::WRL::ComPtr<IDWriteFactory>    factory_;
		Microsoft::WRL::ComPtr<IDWriteTextFormat> formats_[4];
		CellMetrics                               metrics_;
	};

} /* namespace wbshterm */

#pragma once

/**
 * @file font.h
 * @brief The monospace face, and the cell box its glyphs sit in.
 */

#include <dwrite_2.h>
#include <wrl/client.h>

#include "config.h"

#include <string>
#include <vector>

namespace wbshterm {

	/**
	 * @brief Every installed family that is monospaced, sorted by name.
	 *
	 * What the menu offers: a face that is not fixed-pitch would break the
	 * grid, so those are left out even though the config file would take
	 * them. Empty when DirectWrite cannot be reached at all.
	 */
	std::vector<std::wstring> installedMonospaceFamilies();

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
		bool create(const FontSettings& settings, std::string& out_error);

		IDWriteTextFormat* format(bool bold, bool italic) const;

		/** A proportional face for chrome, centred; null if none loaded. */
		IDWriteTextFormat* captionFormat() const { return caption_.Get(); }
		IDWriteFactory* factory() const { return factory_.Get(); }
		const CellMetrics& metrics() const { return metrics_; }

	private:
		bool createFormats(const std::wstring& family, float size_dip, std::string& out_error);
		void createCaptionFormat();
		void applyFallback(const std::vector<std::wstring>& families);
		bool measureCell(std::string& out_error);

		Microsoft::WRL::ComPtr<IDWriteFactory>    factory_;
		Microsoft::WRL::ComPtr<IDWriteTextFormat> formats_[4];
		Microsoft::WRL::ComPtr<IDWriteTextFormat> caption_;
		CellMetrics                               metrics_;
	};

} /* namespace wbshterm */

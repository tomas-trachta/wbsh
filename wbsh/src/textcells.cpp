/**
 * @file textcells.cpp
 * @brief Grapheme clusters and cell widths over UTF-8, from wbshterm's width tables.
 */

#include "textcells.h"

#include <algorithm>

#include "../../wbshterm/src/charwidth.h"

namespace wbsh {

	namespace textcells_detail {

		static const char32_t kReplacement = 0xFFFD;
		static const std::size_t kWidestCluster = 2;

		struct Decoded {
			char32_t code;
			std::size_t length;
		};

		struct Cluster {
			std::size_t end;
			std::size_t width;
		};

		static std::size_t sequenceLength(unsigned char lead) {
			if (lead < 0x80) return 1;
			if (lead >= 0xC2 && lead <= 0xDF) return 2;
			if (lead >= 0xE0 && lead <= 0xEF) return 3;
			if (lead >= 0xF0 && lead <= 0xF4) return 4;
			return 0;
		}

		static bool isContinuation(unsigned char byte) {
			return (byte & 0xC0) == 0x80;
		}

		static Decoded decodeAt(const std::string& text, std::size_t pos) {
			const auto lead = static_cast<unsigned char>(text[pos]);
			const std::size_t length = sequenceLength(lead);
			if (length == 0 || pos + length > text.size()) return { kReplacement, 1 };
			if (length == 1) return { lead, 1 };

			char32_t code = lead & (0x7F >> length);
			for (std::size_t k = 1; k < length; ++k) {
				const auto byte = static_cast<unsigned char>(text[pos + k]);
				if (!isContinuation(byte)) return { kReplacement, 1 };

				code = (code << 6) | (byte & 0x3F);
			}

			return { code, length };
		}

		static std::size_t cellsOf(char32_t code, int properties) {
			return static_cast<std::size_t>(wbshterm::clusterWidthOf(code, properties));
		}

		// A cluster whose first character takes no cell (a mark with
		// nothing to hang on) takes none at all, as on the console grid.
		static Cluster clusterAt(const std::string& text, std::size_t pos) {
			const Decoded first = decodeAt(text, pos);
			int last_class = wbshterm::codepointProperties(first.code);
			int join_state = 0;
			std::size_t width = cellsOf(first.code, last_class);
			const bool takes_cells = width > 0;

			std::size_t end = pos + first.length;
			while (end < text.size()) {
				const Decoded next = decodeAt(text, end);
				const int properties = wbshterm::codepointProperties(next.code);
				const int state = wbshterm::graphemeJoin(join_state, last_class, properties);
				if (state == wbshterm::kGraphemeBreak) break;

				join_state = state;
				last_class = properties;
				width += cellsOf(next.code, properties);
				end += next.length;
			}

			if (!takes_cells) width = 0;
			return { end, std::min(width, kWidestCluster) };
		}

	} /* namespace textcells_detail */

	TextCluster clusterAt(const std::string& text, std::size_t pos) {
		const textcells_detail::Cluster cluster = textcells_detail::clusterAt(text, pos);
		return { cluster.end, cluster.width };
	}

	std::size_t codePointLength(const std::string& text, std::size_t pos) {
		if (pos >= text.size()) return 0;

		return textcells_detail::decodeAt(text, pos).length;
	}

	std::size_t previousClusterStart(const std::string& text, std::size_t pos) {
		std::size_t start = 0;
		std::size_t at = 0;
		while (at < pos && at < text.size()) {
			start = at;
			at = textcells_detail::clusterAt(text, at).end;
		}

		return start;
	}

	std::size_t nextClusterEnd(const std::string& text, std::size_t pos) {
		std::size_t at = 0;
		while (at <= pos && at < text.size()) {
			at = textcells_detail::clusterAt(text, at).end;
		}

		return at;
	}

	std::size_t displayWidth(const std::string& text, std::size_t end) {
		std::size_t width = 0;
		std::size_t at = 0;
		while (at < end && at < text.size()) {
			const textcells_detail::Cluster cluster = textcells_detail::clusterAt(text, at);
			width += cluster.width;
			at = cluster.end;
		}

		return width;
	}

} /* namespace wbsh */

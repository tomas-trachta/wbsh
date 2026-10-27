/**
 * @file screen_cluster.cpp
 * @brief Grapheme clusters: marks, joiners and selectors join the cell before them.
 */

#include "screen.h"

#include "charwidth.h"
#include "utf8.h"

#include <algorithm>
#include <string>

namespace wbshterm {

	// A cluster past this many code points keeps what it has: nothing real
	// is that long, and a stream of marks must not grow one without end.
	static const std::size_t kMaxClusterLength = 32;

	// Cluster numbers stay valid for as long as history can hold them, so
	// the table only grows; once full, new clusters keep their base alone.
	static const std::size_t kMaxClusters = 1u << 16;

	// The pseudoconsole measures a whole cluster before it places it, so
	// a character the grid already placed widens when a later code point
	// (VS16, a second regional indicator, a Hangul jamo) makes the cluster
	// two cells wide, exactly where the console would have put it.
	bool Screen::writeChar(char32_t code) {
		const int properties = codepointProperties(code);
		if (joinsOpenCluster(properties)) {
			extendCluster(code, properties);
			return false;
		}

		return startCluster(code, properties);
	}

	bool Screen::joinsOpenCluster(int properties) {
		if (!cluster_.open) return false;

		const int state = graphemeJoin(cluster_.join_state, cluster_.last_class, properties);
		if (state == kGraphemeBreak) return false;

		cluster_.join_state = state;
		cluster_.last_class = properties;
		return true;
	}

	// A mark with no character before it to hang on takes no cell, as in
	// the console.
	bool Screen::startCluster(char32_t code, int properties) {
		closeCluster();

		const int width = std::min(2, clusterWidthOf(code, properties));
		if (width == 0) return false;

		const GridPlace place = placeGlyph(code, width);
		cluster_.open       = true;
		cluster_.row        = place.row;
		cluster_.column     = place.column;
		cluster_.width      = width;
		cluster_.join_state = 0;
		cluster_.last_class = properties;
		return true;
	}

	void Screen::extendCluster(char32_t code, int properties) {
		appendToClusterCell(code);

		const int width = std::min(2, cluster_.width + clusterWidthOf(code, properties));
		if (width > cluster_.width) widenCluster();
	}

	std::uint32_t Screen::internCluster(const std::u32string& text) {
		const auto found = cluster_ids_.find(text);
		if (found != cluster_ids_.end()) return found->second;
		if (clusters_.size() >= kMaxClusters) return 0;

		clusters_.push_back(text);
		const auto id = static_cast<std::uint32_t>(clusters_.size());
		cluster_ids_.emplace(text, id);
		return id;
	}

	void Screen::appendToClusterCell(char32_t code) {
		Cell cell = at(cluster_.row, cluster_.column);
		const std::u32string* existing = clusterOf(cell);
		std::u32string text = existing != nullptr ? *existing : std::u32string(1, cell.code);
		if (text.size() >= kMaxClusterLength) return;

		text.push_back(code);
		const std::uint32_t id = internCluster(text);
		if (id == 0) return;

		cell.code = kClusterCode | id;
		putCell(cluster_.row, cluster_.column, cell);
		markDirty(cluster_.row);
	}

	// The lead is placed again as a wide character where it stood; when it
	// stood in the last column it moves to the next row, the way the
	// console wraps a wide cluster that does not fit.
	void Screen::widenCluster() {
		const Cell lead = at(cluster_.row, cluster_.column);
		clearRow(cluster_.row, cluster_.column, cluster_.column);

		cursor_.row    = cluster_.row;
		cursor_.column = cluster_.column;
		wrap_pending_  = false;

		const GridPlace place = placeGlyph(lead.code, 2);
		cluster_.row    = place.row;
		cluster_.column = place.column;
		cluster_.width  = 2;
	}

	const std::u32string* Screen::clusterOf(const Cell& cell) const {
		if ((cell.code & kClusterCode) == 0) return nullptr;

		const std::size_t id = cell.code & ~kClusterCode;
		if (id == 0 || id > clusters_.size()) return nullptr;
		return &clusters_[id - 1];
	}

	char32_t Screen::baseCodeOf(const Cell& cell) const {
		const std::u32string* cluster = clusterOf(cell);
		if (cluster != nullptr) return cluster->front();
		return (cell.code & kClusterCode) != 0 ? U'?' : cell.code;
	}

	void Screen::appendCellText(std::string& out, const Cell& cell) const {
		if ((cell.attributes & kAttrWideTail) != 0) return;

		const std::u32string* cluster = clusterOf(cell);
		if (cluster == nullptr) {
			appendUtf8(out, baseCodeOf(cell));
			return;
		}

		for (char32_t code : *cluster) appendUtf8(out, code);
	}

} /* namespace wbshterm */

/**
 * @file pane_tree.cpp
 * @brief Splitting, collapsing, and slicing a rectangle into panes.
 */

#include "pane_tree.h"

#include <algorithm>
#include <cmath>

namespace wbshterm {

	static const float kMinRatio = 0.05f;
	static const float kMaxRatio = 0.95f;

	static bool containsPoint(const D2D1_RECT_F& bounds, float x, float y) {
		return x >= bounds.left && x < bounds.right && y >= bounds.top && y < bounds.bottom;
	}

	static float overlapOf(float low, float high, float other_low, float other_high) {
		return std::min(high, other_high) - std::max(low, other_low);
	}

	static int gridCount(float extent, float cell) {
		if (cell <= 0.0f) return 1;

		const int count = static_cast<int>(extent / cell);
		return count < 1 ? 1 : count;
	}

	PaneNode::PaneNode(std::unique_ptr<Pane> pane)
		: pane_(std::move(pane)) {
	}

	void PaneNode::setRatio(float ratio) {
		ratio_ = std::min(std::max(ratio, kMinRatio), kMaxRatio);
	}

	// The node keeps its identity so whoever points at it from above needs
	// no fixing up: its pane moves down into a new first child.
	PaneNode* PaneNode::splitInto(SplitAxis axis, std::unique_ptr<Pane> added) {
		auto kept  = std::make_unique<PaneNode>(std::move(pane_));
		auto fresh = std::make_unique<PaneNode>(std::move(added));

		PaneNode* result = fresh.get();
		kept->parent_  = this;
		fresh->parent_ = this;

		axis_   = axis;
		ratio_  = 0.5f;
		first_  = std::move(kept);
		second_ = std::move(fresh);
		return result;
	}

	void PaneTree::adopt(std::unique_ptr<Pane> only) {
		root_    = std::make_unique<PaneNode>(std::move(only));
		focused_ = root_.get();
		zoomed_  = nullptr;
		dividers_.clear();
	}

	void PaneTree::focusOn(PaneNode* leaf) {
		if (leaf == nullptr || !leaf->isLeaf()) return;

		focused_ = leaf;
	}

	void PaneTree::splitFocused(SplitAxis axis, std::unique_ptr<Pane> added) {
		if (focused_ == nullptr) {
			adopt(std::move(added));
			return;
		}

		focused_ = focused_->splitInto(axis, std::move(added));
		zoomed_  = nullptr;
		dividers_.clear();
	}

	// The sibling is lifted out and the focus retargeted before the owning
	// pointer is overwritten, because that overwrite is what destroys the
	// pane and joins its reader thread.
	bool PaneTree::close(PaneNode* leaf) {
		if (leaf == nullptr || !leaf->isLeaf() || leaf == root_.get()) return false;

		PaneNode* parent = leaf->parent_;
		std::unique_ptr<PaneNode> kept = parent->first_.get() == leaf
			? std::move(parent->second_)
			: std::move(parent->first_);

		if (focused_ == leaf) focused_ = firstLeafUnder(*kept);
		if (zoomed_ == leaf) zoomed_ = nullptr;
		dividers_.clear();

		PaneNode* grandparent = parent->parent_;
		kept->parent_ = grandparent;

		if (grandparent == nullptr) root_ = std::move(kept);
		else if (grandparent->first_.get() == parent) grandparent->first_ = std::move(kept);
		else grandparent->second_ = std::move(kept);

		return true;
	}

	void PaneTree::toggleZoom() {
		zoomed_ = zoomed_ != nullptr ? nullptr : focused_;
	}

	PaneNode* PaneTree::firstLeafUnder(PaneNode& node) const {
		if (node.isLeaf()) return &node;

		return firstLeafUnder(*node.first_);
	}

	void PaneTree::collectLeaves(PaneNode& node, std::vector<PaneNode*>& out) const {
		if (node.isLeaf()) {
			out.push_back(&node);
			return;
		}

		collectLeaves(*node.first_, out);
		collectLeaves(*node.second_, out);
	}

	std::vector<PaneNode*> PaneTree::leaves() const {
		std::vector<PaneNode*> found;
		if (root_) collectLeaves(*root_, found);

		return found;
	}

	void PaneTree::hideEveryPane() {
		const D2D1_RECT_F nothing = D2D1::RectF(0.0f, 0.0f, 0.0f, 0.0f);
		for (PaneNode* leaf : leaves()) leaf->setBounds(nothing);
	}

	void PaneTree::layout(const PaneMetrics& metrics) {
		metrics_ = metrics;
		dividers_.clear();
		if (!root_) return;

		if (zoomed_ != nullptr) {
			hideEveryPane();
			layoutNode(*zoomed_, metrics.client);
			return;
		}

		layoutNode(*root_, metrics.client);
	}

	void PaneTree::layoutNode(PaneNode& node, const D2D1_RECT_F& bounds) {
		node.setBounds(bounds);

		if (node.isLeaf()) {
			layoutLeaf(node, bounds);
			return;
		}

		D2D1_RECT_F first{};
		D2D1_RECT_F second{};
		splitBounds(node, bounds, first, second);

		PaneDivider divider;
		divider.axis   = node.axis();
		divider.branch = &node;
		divider.bounds = node.axis() == SplitAxis::Columns
			? D2D1::RectF(first.right, bounds.top, second.left, bounds.bottom)
			: D2D1::RectF(bounds.left, first.bottom, bounds.right, second.top);
		dividers_.push_back(divider);

		layoutNode(*node.first_, first);
		layoutNode(*node.second_, second);
	}

	void PaneTree::layoutLeaf(PaneNode& node, const D2D1_RECT_F& bounds) const {
		const float inset = 2.0f * metrics_.padding;
		node.pane()->wantGridSize(
			gridCount(bounds.right - bounds.left - inset, metrics_.cell_width),
			gridCount(bounds.bottom - bounds.top - inset, metrics_.cell_height));
	}

	// Snapping the first child to whole cells and letting the second take
	// the remainder keeps a sliver of a cell from opening beside a divider.
	float PaneTree::snapToCells(float extent, float cell) const {
		if (cell <= 0.0f) return extent;

		const float inner = extent - 2.0f * metrics_.padding;
		const float cells = std::floor(inner / cell);
		return std::max(cells, 1.0f) * cell + 2.0f * metrics_.padding;
	}

	float PaneTree::firstSpan(const PaneNode& node, float total) const {
		const bool columns = node.axis() == SplitAxis::Columns;
		const float cell = columns ? metrics_.cell_width : metrics_.cell_height;
		const float smallest = cell + 2.0f * metrics_.padding;

		const float wanted = snapToCells(total * node.ratio(), cell);
		if (wanted < smallest) return smallest;
		if (wanted > total - smallest) return std::max(smallest, total - smallest);

		return wanted;
	}

	void PaneTree::splitBounds(const PaneNode& node, const D2D1_RECT_F& bounds,
			D2D1_RECT_F& out_first, D2D1_RECT_F& out_second) const {
		const bool columns = node.axis() == SplitAxis::Columns;
		const float total = (columns ? bounds.right - bounds.left : bounds.bottom - bounds.top)
			- metrics_.divider;
		const float span = firstSpan(node, total);

		if (columns) {
			out_first  = D2D1::RectF(bounds.left, bounds.top, bounds.left + span, bounds.bottom);
			out_second = D2D1::RectF(out_first.right + metrics_.divider, bounds.top,
				bounds.right, bounds.bottom);
			return;
		}

		out_first  = D2D1::RectF(bounds.left, bounds.top, bounds.right, bounds.top + span);
		out_second = D2D1::RectF(bounds.left, out_first.bottom + metrics_.divider,
			bounds.right, bounds.bottom);
	}

	PaneNode* PaneTree::leafAt(float x, float y) const {
		for (PaneNode* leaf : leaves()) {
			if (containsPoint(leaf->bounds(), x, y)) return leaf;
		}

		return nullptr;
	}

	PaneNode* PaneTree::dividerAt(float x, float y, float slop) const {
		for (const PaneDivider& divider : dividers_) {
			const D2D1_RECT_F grown = D2D1::RectF(divider.bounds.left - slop,
				divider.bounds.top - slop, divider.bounds.right + slop,
				divider.bounds.bottom + slop);
			if (containsPoint(grown, x, y)) return divider.branch;
		}

		return nullptr;
	}

	static bool liesToward(PaneDirection direction, const D2D1_RECT_F& from,
			const D2D1_RECT_F& other) {
		switch (direction) {
		case PaneDirection::Left:
			return other.right <= from.left;
		case PaneDirection::Right:
			return other.left >= from.right;
		case PaneDirection::Up:
			return other.bottom <= from.top;
		case PaneDirection::Down:
			return other.top >= from.bottom;
		default:
			return false;
		}
	}

	static float overlapToward(PaneDirection direction, const D2D1_RECT_F& from,
			const D2D1_RECT_F& other) {
		const bool sideways = direction == PaneDirection::Left
			|| direction == PaneDirection::Right;

		return sideways
			? overlapOf(from.top, from.bottom, other.top, other.bottom)
			: overlapOf(from.left, from.right, other.left, other.right);
	}

	// Geometric rather than structural: the pane that shares the most edge
	// in that direction is the one the eye expects to land on.
	PaneNode* PaneTree::neighbour(PaneDirection direction) const {
		if (focused_ == nullptr) return nullptr;

		const D2D1_RECT_F from = focused_->bounds();
		PaneNode* best = nullptr;
		float best_overlap = 0.0f;

		for (PaneNode* leaf : leaves()) {
			if (leaf == focused_ || !liesToward(direction, from, leaf->bounds())) continue;

			const float overlap = overlapToward(direction, from, leaf->bounds());
			if (overlap <= best_overlap) continue;

			best = leaf;
			best_overlap = overlap;
		}

		return best;
	}

	void PaneTree::dragDivider(PaneNode* branch, float x, float y) {
		if (branch == nullptr || branch->isLeaf()) return;

		const D2D1_RECT_F& bounds = branch->bounds();
		const bool columns = branch->axis() == SplitAxis::Columns;
		const float total = (columns ? bounds.right - bounds.left : bounds.bottom - bounds.top)
			- metrics_.divider;
		if (total <= 0.0f) return;

		const float offset = columns ? x - bounds.left : y - bounds.top;
		branch->setRatio(offset / total);
	}

} /* namespace wbshterm */

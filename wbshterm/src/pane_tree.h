#pragma once

/**
 * @file pane_tree.h
 * @brief The split layout: a binary tree of panes over one client area.
 */

#include "pane.h"

#include <d2d1.h>
#include <memory>
#include <vector>

namespace wbshterm {

	enum class SplitAxis {
		Columns,
		Rows,
	};

	enum class PaneDirection {
		Left,
		Right,
		Up,
		Down,
	};

	/** Pixel sizes the layout needs; every rectangle follows from these. */
	struct PaneMetrics {
		D2D1_RECT_F client{};
		float       cell_width  = 0.0f;
		float       cell_height = 0.0f;
		float       padding     = 0.0f;
		float       divider     = 6.0f;
	};

	class PaneNode;

	struct PaneDivider {
		D2D1_RECT_F bounds{};
		SplitAxis   axis   = SplitAxis::Columns;
		PaneNode*   branch = nullptr;
	};

	/**
	 * @brief A leaf holds a pane, a branch holds two children and a ratio.
	 *
	 * Exactly one of the two is ever set. parent_ points back up the tree
	 * and owns nothing; only first_, second_ and pane_ own anything.
	 */
	class PaneNode {
	public:
		explicit PaneNode(std::unique_ptr<Pane> pane);

		bool  isLeaf() const { return pane_ != nullptr; }
		Pane* pane() const { return pane_.get(); }

		/** Turns this leaf into a branch over its old pane and @p added. */
		PaneNode* splitInto(SplitAxis axis, std::unique_ptr<Pane> added);

		SplitAxis axis() const { return axis_; }
		float ratio() const { return ratio_; }
		void  setRatio(float ratio);

		const D2D1_RECT_F& bounds() const { return bounds_; }
		void setBounds(const D2D1_RECT_F& bounds) { bounds_ = bounds; }

		PaneNode* first() const { return first_.get(); }
		PaneNode* second() const { return second_.get(); }
		PaneNode* parent() const { return parent_; }

	private:
		friend class PaneTree;

		std::unique_ptr<Pane>     pane_;
		std::unique_ptr<PaneNode> first_;
		std::unique_ptr<PaneNode> second_;
		PaneNode*                 parent_ = nullptr;
		SplitAxis                 axis_   = SplitAxis::Columns;
		float                     ratio_  = 0.5f;
		D2D1_RECT_F               bounds_{};
	};

	/**
	 * @brief Owns every pane and hands each one a rectangle.
	 *
	 * The tree owns its nodes through root_. focused_, zoomed_ and the
	 * branch a divider names are observers: close() retargets them before
	 * anything is destroyed.
	 */
	class PaneTree {
	public:
		void adopt(std::unique_ptr<Pane> only);

		PaneNode* focused() const { return focused_; }
		void focusOn(PaneNode* leaf);

		/** Puts @p added beside the focused pane and gives it the focus. */
		void splitFocused(SplitAxis axis, std::unique_ptr<Pane> added);

		/** Drops @p leaf; its sibling takes the space. False when it was the last. */
		bool close(PaneNode* leaf);

		void toggleZoom();
		bool zoomed() const { return zoomed_ != nullptr; }

		/** Assigns every pane a rectangle and the grid size that fits it. */
		void layout(const PaneMetrics& metrics);

		std::vector<PaneNode*> leaves() const;
		const std::vector<PaneDivider>& dividers() const { return dividers_; }

		PaneNode* leafAt(float x, float y) const;
		PaneNode* neighbour(PaneDirection direction) const;
		/** The branch a divider belongs to, which outlives the next layout. */
		PaneNode* dividerAt(float x, float y, float slop) const;

		/** Moves a branch's divider under the pointer, in client pixels. */
		void dragDivider(PaneNode* branch, float x, float y);

	private:
		void layoutNode(PaneNode& node, const D2D1_RECT_F& bounds);
		void layoutLeaf(PaneNode& node, const D2D1_RECT_F& bounds) const;
		void splitBounds(const PaneNode& node, const D2D1_RECT_F& bounds,
			D2D1_RECT_F& out_first, D2D1_RECT_F& out_second) const;
		float firstSpan(const PaneNode& node, float total) const;
		float snapToCells(float extent, float cell) const;
		void hideEveryPane();
		void collectLeaves(PaneNode& node, std::vector<PaneNode*>& out) const;
		PaneNode* firstLeafUnder(PaneNode& node) const;

		std::unique_ptr<PaneNode> root_;
		PaneNode*                 focused_ = nullptr;
		PaneNode*                 zoomed_  = nullptr;
		std::vector<PaneDivider>  dividers_;
		PaneMetrics               metrics_;
	};

} /* namespace wbshterm */

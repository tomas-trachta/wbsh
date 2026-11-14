#pragma once

/**
 * @file layout_store.h
 * @brief The panes of a closing window, and where each one's shell was,
 *        written down so the next start can lay them out again.
 *
 * The file is one line per node of the split tree, first child first:
 *
 *     wbshterm-layout 1
 *     split columns 0.500
 *     pane C:/Users/me/project
 *     pane C:/Users/me
 */

#include "pane_tree.h"

#include <memory>
#include <string>

namespace wbshterm {

	/** One node of a saved layout: a pane and its directory, or a split. */
	struct SavedPane {
		bool        leaf  = true;
		SplitAxis   axis  = SplitAxis::Columns;
		float       ratio = 0.5f;
		std::string directory;
		std::unique_ptr<SavedPane> first;
		std::unique_ptr<SavedPane> second;
	};

	/** The tree that @p any_leaf belongs to, with each pane's OSC 7 directory. */
	std::string layoutText(const PaneNode& any_leaf);

	/** False for a file this version did not write, or one cut short. */
	bool parseLayout(const std::string& text, SavedPane& out_root);

	/** The pane a saved subtree starts with: the one a split keeps in place. */
	const SavedPane& firstSavedPane(const SavedPane& node);

	/** session.txt beside the config file. */
	std::wstring layoutFilePath(const std::wstring& config_path);

} /* namespace wbshterm */

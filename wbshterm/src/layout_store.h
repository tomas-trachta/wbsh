#pragma once

/**
 * @file layout_store.h
 * @brief The tabs and panes of a closing window, and where each one's
 *        shell was, written down so the next start can lay them out again.
 *
 * The file is a line per tab, the one that was shown marked, each followed
 * by one line per node of its split tree, first child first:
 *
 *     wbshterm-layout 2
 *     tab active
 *     split columns 0.500
 *     pane C:/Users/me/project
 *     pane C:/Users/me
 *     tab
 *     pane C:/Users/me/notes
 *
 * Version 1 is the same tree with no tab lines, from before there were tabs.
 */

#include "pane_tree.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

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

	/** Every tab of a saved window, and the one that was shown. */
	struct SavedSession {
		std::vector<SavedPane> tabs;
		std::size_t            active = 0;
	};

	/** False for a file this version did not write, or one cut short. */
	bool parseLayout(const std::string& text, SavedPane& out_root);

	/** Each tab's tree with each pane's OSC 7 directory, given any one leaf of it. */
	std::string sessionText(const std::vector<const PaneNode*>& tab_leaves, std::size_t active);

	/** Reads a session, or a version 1 layout as a session of one tab. */
	bool parseSession(const std::string& text, SavedSession& out_session);

	/** The pane a saved subtree starts with: the one a split keeps in place. */
	const SavedPane& firstSavedPane(const SavedPane& node);

	/** session.txt beside the config file. */
	std::wstring layoutFilePath(const std::wstring& config_path);

} /* namespace wbshterm */

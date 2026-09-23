#pragma once

/**
 * @file snapshot.h
 * @brief Render a shell session to a PNG without opening a window.
 */

#include "screen.h"

#include <string>

namespace wbshterm {

	struct SnapshotRequest {
		std::wstring command_line;
		std::wstring output_path;
		std::wstring record_path;
		std::string  feed;
		int          columns    = 100;
		int          rows       = 30;
		unsigned int delay_ms   = 400;
		unsigned int settle_ms  = 600;
		unsigned int timeout_ms = 15000;

		/** Scroll back this many lines before painting, for scrollback shots. */
		int scroll_lines = 0;

		/** Selection to highlight, in absolute rows; ignored when unset. */
		bool select        = false;
		int  select_row    = 0;
		int  select_column = 0;
		int  select_to_row = 0;
		int  select_to_col = 0;
	};

	/**
	 * @brief Runs the shell, types @p feed, then paints the grid to a PNG.
	 *
	 * Waits for output to go quiet for settle_ms before painting, so the
	 * image shows a finished screen rather than a half-drawn one.
	 */
	bool renderSnapshot(const SnapshotRequest& request, std::string& out_error);

	/** Paints an already-parsed grid, for replays and goldens. */
	bool renderScreenToPng(const Screen& screen, const std::wstring& path, std::string& out_error);

} /* namespace wbshterm */

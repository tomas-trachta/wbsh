#pragma once

/**
 * @file replay.h
 * @brief Replay a recorded byte stream into a grid, with no shell involved.
 */

#include <string>

namespace wbshterm {

	struct ReplayRequest {
		std::wstring input_path;
		std::wstring text_path;
		std::wstring image_path;
		int          columns = 100;
		int          rows    = 30;
	};

	/**
	 * @brief Parses @p input_path into a grid, then writes what was asked for.
	 *
	 * Deterministic and offline, so a recording captured once can be a
	 * golden file: same bytes in, same grid text out.
	 */
	bool replayStream(const ReplayRequest& request, std::string& out_error);

} /* namespace wbshterm */

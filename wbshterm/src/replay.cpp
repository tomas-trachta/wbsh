/**
 * @file replay.cpp
 * @brief File in, grid out: the offline half of the test strategy.
 */

#include "replay.h"

#include "screen.h"
#include "snapshot.h"
#include "vtparse.h"

#include <cstdio>
#include <vector>

namespace wbshterm {

	static bool readWholeFile(const std::wstring& path, std::vector<char>& out_bytes,
			std::string& out_error) {
		FILE* file = nullptr;
		if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr) {
			out_error = "cannot open the recording";
			return false;
		}

		char buffer[8192];
		for (;;) {
			const std::size_t got = std::fread(buffer, 1, sizeof(buffer), file);
			if (got == 0) break;
			out_bytes.insert(out_bytes.end(), buffer, buffer + got);
		}

		std::fclose(file);
		return true;
	}

	static bool writeWholeFile(const std::wstring& path, const std::string& text,
			std::string& out_error) {
		FILE* file = nullptr;
		if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) {
			out_error = "cannot write the grid dump";
			return false;
		}

		std::fwrite(text.data(), 1, text.size(), file);
		std::fclose(file);
		return true;
	}

	bool replayStream(const ReplayRequest& request, std::string& out_error) {
		std::vector<char> bytes;
		if (!readWholeFile(request.input_path, bytes, out_error)) return false;

		Screen screen(request.columns, request.rows);
		VtParser parser(screen);
		parser.consume(bytes.data(), bytes.size());

		if (!request.text_path.empty()
			&& !writeWholeFile(request.text_path, screen.toText(), out_error)) {
			return false;
		}

		if (request.image_path.empty()) return true;
		return renderScreenToPng(screen, request.image_path, out_error);
	}

} /* namespace wbshterm */

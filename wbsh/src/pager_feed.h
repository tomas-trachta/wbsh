#pragma once

/**
 * @file pager_feed.h
 * @brief The pager's input: read on a background thread only as far as
 *        the pager has asked, so a pipe that never ends (`yes | less`)
 *        still shows its first page at once.
 */

#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace wbsh {

	struct PagerInput {
		std::FILE* file = nullptr;
		std::string header;
	};

	class PagerFeed {
	public:
		explicit PagerFeed(const std::vector<PagerInput>& inputs);
		~PagerFeed();

		PagerFeed(const PagerFeed&) = delete;
		PagerFeed& operator=(const PagerFeed&) = delete;

		/// Keep reading until at least @p lines lines have arrived.
		void want(std::size_t lines);
		void wantAll();

		/// Appends what arrived since the last call; true once the input ended.
		bool take(std::string& text);

		/// Signalled (a Win32 event) while there is something to take.
		void* event() const;

		struct Shared;

	private:
		std::unique_ptr<Shared> shared_;
		std::thread reader_;
	};

	/// Copies the inputs to stdout as they arrive; false once stdout fails.
	bool streamInputs(const std::vector<PagerInput>& inputs);

}  // namespace wbsh

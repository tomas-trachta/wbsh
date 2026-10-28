#include "pager_feed.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <io.h>
#endif /* _WIN32 */

#include <algorithm>
#include <condition_variable>
#include <mutex>

namespace wbsh {

#ifdef _WIN32

	static const unsigned kReadChunk = 64 * 1024;

	struct PagerFeed::Shared {
		std::mutex mutex;
		std::condition_variable wake;
		std::string text;
		std::size_t lines_read = 0;
		std::size_t wanted = 0;
		bool eof = false;
		bool cancelled = false;
		bool finished = false;
		HANDLE event = nullptr;
		std::vector<PagerInput> inputs;
	};

	namespace pager_feed_detail {

		static void publish(PagerFeed::Shared& shared, const char* data, std::size_t size) {
			std::lock_guard<std::mutex> lock(shared.mutex);
			shared.text.append(data, size);
			shared.lines_read += static_cast<std::size_t>(std::count(data, data + size, '\n'));
			::SetEvent(shared.event);
		}

		static bool waitForDemand(PagerFeed::Shared& shared) {
			std::unique_lock<std::mutex> lock(shared.mutex);
			shared.wake.wait(lock, [&shared]() { return shared.cancelled || shared.lines_read < shared.wanted; });
			return !shared.cancelled;
		}

		// _read returns whatever a pipe has, so a slow writer is shown as it writes.
		static bool readInput(PagerFeed::Shared& shared, const PagerInput& input) {
			publish(shared, input.header.data(), input.header.size());
			const int fd = _fileno(input.file);
			std::vector<char> buffer(kReadChunk);
			for (;;) {
				if (!waitForDemand(shared)) return false;
				const int got = _read(fd, buffer.data(), kReadChunk);
				if (got <= 0) return true;
				publish(shared, buffer.data(), static_cast<std::size_t>(got));
			}
		}

		static void readAll(PagerFeed::Shared& shared) {
			for (const PagerInput& input : shared.inputs) {
				if (!readInput(shared, input)) break;
			}

			std::lock_guard<std::mutex> lock(shared.mutex);
			shared.eof = true;
			shared.finished = true;
			::SetEvent(shared.event);
		}

	}  // namespace pager_feed_detail

	PagerFeed::PagerFeed(const std::vector<PagerInput>& inputs) : shared_(std::make_unique<Shared>()) {
		shared_->inputs = inputs;
		shared_->event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		Shared* shared = shared_.get();
		reader_ = std::thread([shared]() { pager_feed_detail::readAll(*shared); });
	}

	static bool readerFinished(PagerFeed::Shared& shared) {
		std::lock_guard<std::mutex> lock(shared.mutex);
		return shared.finished;
	}

	// The reader may sit in a blocking read of a pipe that never ends;
	// cancelling its synchronous I/O lets it see the request to stop.
	PagerFeed::~PagerFeed() {
		std::unique_lock<std::mutex> lock(shared_->mutex);
		shared_->cancelled = true;
		shared_->wake.notify_all();
		lock.unlock();

		const HANDLE thread = static_cast<HANDLE>(reader_.native_handle());
		while (!readerFinished(*shared_)) {
			::CancelSynchronousIo(thread);
			::Sleep(10);
		}

		reader_.join();
		::CloseHandle(shared_->event);
	}

	void PagerFeed::want(std::size_t lines) {
		std::lock_guard<std::mutex> lock(shared_->mutex);
		shared_->wanted = lines;
		shared_->wake.notify_all();
	}

	void PagerFeed::wantAll() {
		want(static_cast<std::size_t>(-1));
	}

	bool PagerFeed::take(std::string& text) {
		std::lock_guard<std::mutex> lock(shared_->mutex);
		text += shared_->text;
		shared_->text.clear();
		if (!shared_->eof) ::ResetEvent(shared_->event);
		return shared_->eof;
	}

	void* PagerFeed::event() const {
		return shared_->event;
	}

	bool streamInputs(const std::vector<PagerInput>& inputs) {
		std::vector<char> buffer(kReadChunk);
		for (const PagerInput& input : inputs) {
			if (std::fwrite(input.header.data(), 1, input.header.size(), stdout) != input.header.size()) return false;

			const int fd = _fileno(input.file);
			for (int got = 0; (got = _read(fd, buffer.data(), kReadChunk)) > 0;) {
				const auto size = static_cast<std::size_t>(got);
				if (std::fwrite(buffer.data(), 1, size, stdout) != size) return false;

				std::fflush(stdout);
			}
		}

		return std::fflush(stdout) == 0;
	}

#endif /* _WIN32 */

}  // namespace wbsh

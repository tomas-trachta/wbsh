#include "bytestream.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#ifdef _WIN32
#  include <io.h>
#else
#  include <unistd.h>
#  define _read read
#  define _fileno fileno
#endif /* _WIN32 */

namespace wbsh {

	static const std::size_t kInputChunk = 64 * 1024;
	static const std::size_t kWindowFlushBytes = 64 * 1024;
	static const std::size_t kQueueChunks = 8;

	std::size_t MemorySource::read(std::uint8_t* data, std::size_t size) {
		const std::size_t take = (std::min)(size, size_ - pos_);
		if (take != 0) std::memcpy(data, data_ + pos_, take);
		pos_ += take;
		return take;
	}

	// _read hands back whatever a pipe holds instead of waiting for a full
	// buffer the way fread does, so a slow writer's data is decoded at once.
	std::size_t FileSource::read(std::uint8_t* data, std::size_t size) {
		const unsigned want = static_cast<unsigned>((std::min)(size, static_cast<std::size_t>(kInputChunk)));
		const int got = _read(_fileno(file_), data, want);
		if (got < 0) failed_ = true;
		return got > 0 ? static_cast<std::size_t>(got) : 0;
	}

	bool VectorSink::write(const std::uint8_t* data, std::size_t size) {
		out_.insert(out_.end(), data, data + size);
		return true;
	}

	bool FileSink::write(const std::uint8_t* data, std::size_t size) {
		if (failed_) return false;
		failed_ = std::fwrite(data, 1, size, file_) != size || std::fflush(file_) != 0;
		return !failed_;
	}

	bool CountingSink::write(const std::uint8_t* data, std::size_t size) {
		count_ += size;
		return next_.write(data, size);
	}

	InputBuffer::InputBuffer(ByteSource& source) : source_(source), buffer_(kInputChunk) {}

	bool InputBuffer::refill(std::size_t want) {
		if (pos_ > 0) {
			std::memmove(buffer_.data(), buffer_.data() + pos_, end_ - pos_);
			consumed_ += pos_;
			end_ -= pos_;
			pos_ = 0;
		}

		if (buffer_.size() < want) buffer_.resize(want);
		while (end_ < want) {
			const std::size_t got = source_.read(buffer_.data() + end_, buffer_.size() - end_);
			if (got == 0) return false;
			end_ += got;
		}

		return true;
	}

	int InputBuffer::get() {
		if (pos_ == end_ && !refill(1)) return -1;
		return buffer_[pos_++];
	}

	bool InputBuffer::readExact(std::uint8_t* data, std::size_t size) {
		while (size > 0) {
			if (pos_ == end_ && !refill(1)) return false;

			const std::size_t take = (std::min)(size, end_ - pos_);
			std::memcpy(data, buffer_.data() + pos_, take);
			pos_ += take;
			data += take;
			size -= take;
		}

		return true;
	}

	std::size_t InputBuffer::peek(const std::uint8_t*& data, std::size_t want) {
		if (end_ - pos_ < want) refill(want);
		data = buffer_.data() + pos_;
		return (std::min)(want, end_ - pos_);
	}

	bool InputBuffer::atEnd() {
		return pos_ == end_ && !refill(1);
	}

	std::size_t InputBuffer::readSome(std::uint8_t* data, std::size_t size) {
		if (pos_ == end_ && !refill(1)) return 0;

		const std::size_t take = (std::min)(size, end_ - pos_);
		std::memcpy(data, buffer_.data() + pos_, take);
		pos_ += take;
		return take;
	}

	std::size_t BufferedSource::read(std::uint8_t* data, std::size_t size) {
		return input_.readSome(data, size);
	}

	LzWindow::LzWindow(ByteSink& sink, std::size_t window)
		: sink_(sink), ring_((std::max)(window, static_cast<std::size_t>(1))),
		  flush_at_((std::min)(ring_.size(), kWindowFlushBytes)) {}

	bool LzWindow::copy(std::size_t distance, std::size_t length) {
		if (distance == 0 || distance > history_) return false;
		for (std::size_t k = 0; k < length; ++k) put(back(distance));
		return true;
	}

	bool LzWindow::flush() {
		if (pending_ == 0) return ok_;

		const std::size_t size = ring_.size();
		const std::size_t start = (pos_ + size - pending_) % size;
		const std::size_t first = (std::min)(pending_, size - start);
		if (ok_) ok_ = sink_.write(ring_.data() + start, first);
		if (ok_ && first < pending_) ok_ = sink_.write(ring_.data(), pending_ - first);
		pending_ = 0;
		return ok_;
	}

	struct ProducerSource::Shared {
		std::mutex mutex;
		std::condition_variable changed;
		std::deque<std::vector<std::uint8_t>> chunks;
		std::vector<std::uint8_t> current;
		std::size_t current_pos = 0;
		bool finished = false;
		bool cancelled = false;
		bool ok = true;
		std::string error;
		std::thread worker;
	};

	namespace bytestream_detail {

		class QueueSink : public ByteSink {
		public:
			explicit QueueSink(ProducerSource::Shared& shared) : shared_(shared) {}

			bool write(const std::uint8_t* data, std::size_t size) override {
				std::unique_lock<std::mutex> lock(shared_.mutex);
				shared_.changed.wait(lock, [this]() {
					return shared_.cancelled || shared_.chunks.size() < kQueueChunks;
				});
				if (shared_.cancelled) return false;

				shared_.chunks.emplace_back(data, data + size);
				shared_.changed.notify_all();
				return true;
			}

		private:
			ProducerSource::Shared& shared_;
		};

	}  // namespace bytestream_detail

	ProducerSource::ProducerSource(Producer producer, void* context) : shared_(std::make_unique<Shared>()) {
		Shared* shared = shared_.get();
		shared->worker = std::thread([shared, producer, context]() {
			bytestream_detail::QueueSink sink(*shared);
			std::string error;
			const bool ok = producer(context, sink, error);

			std::lock_guard<std::mutex> lock(shared->mutex);
			shared->ok = ok || shared->cancelled;
			shared->error = error;
			shared->finished = true;
			shared->changed.notify_all();
		});
	}

	ProducerSource::~ProducerSource() {
		std::unique_lock<std::mutex> lock(shared_->mutex);
		shared_->cancelled = true;
		shared_->changed.notify_all();
		lock.unlock();
		if (shared_->worker.joinable()) shared_->worker.join();
	}

	std::size_t ProducerSource::read(std::uint8_t* data, std::size_t size) {
		Shared& shared = *shared_;
		std::unique_lock<std::mutex> lock(shared.mutex);
		while (shared.current_pos == shared.current.size()) {
			shared.changed.wait(lock, [&shared]() { return !shared.chunks.empty() || shared.finished; });
			if (shared.chunks.empty()) return 0;

			shared.current = std::move(shared.chunks.front());
			shared.chunks.pop_front();
			shared.current_pos = 0;
			shared.changed.notify_all();
		}

		const std::size_t take = (std::min)(size, shared.current.size() - shared.current_pos);
		std::memcpy(data, shared.current.data() + shared.current_pos, take);
		shared.current_pos += take;
		return take;
	}

	bool ProducerSource::failed() const {
		std::lock_guard<std::mutex> lock(shared_->mutex);
		return shared_->finished && !shared_->ok;
	}

	std::string ProducerSource::error() const {
		std::lock_guard<std::mutex> lock(shared_->mutex);
		return shared_->error;
	}

	template <typename Word>
	static std::array<Word, 256> reflectedCrcTable(Word polynomial) {
		std::array<Word, 256> table{};
		for (Word index = 0; index < 256; ++index) {
			Word entry = index;
			for (int bit = 0; bit < 8; ++bit) entry = (entry & 1) != 0 ? polynomial ^ (entry >> 1) : entry >> 1;
			table[static_cast<std::size_t>(index)] = entry;
		}

		return table;
	}

	std::uint32_t crc32Of(std::uint32_t crc, const std::uint8_t* data, std::size_t size) {
		static const std::array<std::uint32_t, 256> table =
			reflectedCrcTable<std::uint32_t>(0xEDB88320u);
		crc = ~crc;
		for (std::size_t k = 0; k < size; ++k) crc = table[(crc ^ data[k]) & 0xFF] ^ (crc >> 8);
		return ~crc;
	}

	std::uint64_t crc64Of(std::uint64_t crc, const std::uint8_t* data, std::size_t size) {
		static const std::array<std::uint64_t, 256> table =
			reflectedCrcTable<std::uint64_t>(0xC96C5795D7870F42ULL);
		crc = ~crc;
		for (std::size_t k = 0; k < size; ++k) crc = table[(crc ^ data[k]) & 0xFF] ^ (crc >> 8);
		return ~crc;
	}

}  // namespace wbsh

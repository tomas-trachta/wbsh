#pragma once

/**
 * @file bytestream.h
 * @brief Streaming byte plumbing for the codecs, tar and cpio: pull sources,
 *        push sinks, a buffered decoder input and the sliding LZ window.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace wbsh {

	class ByteSource {
	public:
		virtual ~ByteSource() = default;

		/// Up to @p size bytes; 0 only at the end of the data or on failure.
		virtual std::size_t read(std::uint8_t* data, std::size_t size) = 0;
		virtual bool failed() const { return false; }
		virtual std::string error() const { return std::string(); }
	};

	class ByteSink {
	public:
		virtual ~ByteSink() = default;

		/// False stops the producer: the consumer is gone or failed.
		virtual bool write(const std::uint8_t* data, std::size_t size) = 0;
	};

	class MemorySource : public ByteSource {
	public:
		MemorySource(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}
		std::size_t read(std::uint8_t* data, std::size_t size) override;

	private:
		const std::uint8_t* data_;
		std::size_t size_;
		std::size_t pos_ = 0;
	};

	class FileSource : public ByteSource {
	public:
		explicit FileSource(std::FILE* file) : file_(file) {}
		std::size_t read(std::uint8_t* data, std::size_t size) override;
		bool failed() const override { return failed_; }
		std::string error() const override { return "read error"; }

	private:
		std::FILE* file_;
		bool failed_ = false;
	};

	class VectorSink : public ByteSink {
	public:
		explicit VectorSink(std::vector<std::uint8_t>& out) : out_(out) {}
		bool write(const std::uint8_t* data, std::size_t size) override;

	private:
		std::vector<std::uint8_t>& out_;
	};

	class FileSink : public ByteSink {
	public:
		explicit FileSink(std::FILE* file) : file_(file) {}
		bool write(const std::uint8_t* data, std::size_t size) override;
		bool failed() const { return failed_; }

	private:
		std::FILE* file_;
		bool failed_ = false;
	};

	class NullSink : public ByteSink {
	public:
		bool write(const std::uint8_t*, std::size_t) override { return true; }
	};

	/// Counts the bytes that pass through on their way to the next sink.
	class CountingSink : public ByteSink {
	public:
		explicit CountingSink(ByteSink& next) : next_(next) {}
		bool write(const std::uint8_t* data, std::size_t size) override;
		std::uint64_t count() const { return count_; }

	private:
		ByteSink& next_;
		std::uint64_t count_ = 0;
	};

	/// Buffered input for the decoders: refills from the source on demand,
	/// so a decoder never reads further than the bytes it needs.
	class InputBuffer {
	public:
		explicit InputBuffer(ByteSource& source);

		/// Next byte, or -1 at the end of the data.
		int get();
		bool readExact(std::uint8_t* data, std::size_t size);

		/// Up to @p size bytes, 0 only at the end of the data.
		std::size_t readSome(std::uint8_t* data, std::size_t size);

		/// Makes up to @p want bytes visible without consuming them.
		std::size_t peek(const std::uint8_t*& data, std::size_t want);
		bool atEnd();
		std::uint64_t consumed() const { return consumed_ + pos_; }
		bool sourceFailed() const { return source_.failed(); }

	private:
		bool refill(std::size_t want);

		ByteSource& source_;
		std::vector<std::uint8_t> buffer_;
		std::size_t pos_ = 0;
		std::size_t end_ = 0;
		std::uint64_t consumed_ = 0;
	};

	/// Reads through an InputBuffer: lets a stage be stacked on a decoder's input.
	class BufferedSource : public ByteSource {
	public:
		explicit BufferedSource(InputBuffer& input) : input_(input) {}
		std::size_t read(std::uint8_t* data, std::size_t size) override;
		bool failed() const override { return input_.sourceFailed(); }

	private:
		InputBuffer& input_;
	};

	/// Sliding history for LZ decoders. Back-references read from a ring of
	/// @p window bytes; output reaches the sink in batches as it is produced.
	class LzWindow {
	public:
		LzWindow(ByteSink& sink, std::size_t window);

		void put(std::uint8_t byte) {
			ring_[pos_] = byte;
			if (++pos_ == ring_.size()) pos_ = 0;
			++total_;
			if (history_ < ring_.size()) ++history_;
			if (++pending_ == flush_at_) flush();
		}

		/// The byte @p distance positions back: 1 is the last byte written.
		std::uint8_t back(std::size_t distance) const {
			const std::size_t size = ring_.size();
			return ring_[(pos_ + size - distance) % size];
		}

		/// Copies @p length bytes from @p distance back; false if that reaches
		/// past the history.
		bool copy(std::size_t distance, std::size_t length);
		std::size_t history() const { return history_; }
		std::uint64_t total() const { return total_; }
		void forgetHistory() { history_ = 0; }
		bool flush();
		bool ok() const { return ok_; }

	private:
		ByteSink& sink_;
		std::vector<std::uint8_t> ring_;
		std::size_t pos_ = 0;
		std::size_t pending_ = 0;
		std::size_t history_ = 0;
		std::size_t flush_at_ = 0;
		std::uint64_t total_ = 0;
		bool ok_ = true;
	};

	/// Runs a producer (a decoder writing to a sink) on its own thread and
	/// hands its output to read() through a small bounded queue, so pull-style
	/// consumers such as tar can sit on a push-style decoder.
	class ProducerSource : public ByteSource {
	public:
		typedef bool (*Producer)(void* context, ByteSink& out, std::string& error);

		ProducerSource(Producer producer, void* context);
		~ProducerSource() override;

		std::size_t read(std::uint8_t* data, std::size_t size) override;
		bool failed() const override;
		std::string error() const override;

		struct Shared;

	private:
		std::unique_ptr<Shared> shared_;
	};

	/// zlib / gzip CRC-32; pass 0 to start, the previous result to continue.
	std::uint32_t crc32Of(std::uint32_t crc, const std::uint8_t* data, std::size_t size);

	/// The .xz CRC-64 (ECMA-182 polynomial, reflected); chained like crc32Of.
	std::uint64_t crc64Of(std::uint64_t crc, const std::uint8_t* data, std::size_t size);

}  // namespace wbsh

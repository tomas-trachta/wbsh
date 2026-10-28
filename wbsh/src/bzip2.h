#pragma once

/**
 * @file bzip2.h
 * @brief bzip2 stream decoder and encoder (Burrows-Wheeler, MTF and
 *        multi-table Huffman, as bzip2 1.0 writes them).
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "bytestream.h"

namespace wbsh {

	/// Decodes every concatenated bzip2 stream from @p in, block by block.
	/// On failure returns false and says why in @p error (empty when @p out
	/// stopped taking output).
	bool bunzip2Stream(InputBuffer& in, ByteSink& out, std::string& error);

	/// bunzip2Stream over a buffer, appending to @p out.
	bool bunzip2Data(const std::uint8_t* in, std::size_t size, std::vector<std::uint8_t>& out,
	                 std::string& error);

	/// Streaming encoder: one bzip2 stream; @p level 1-9 picks 100k-900k blocks.
	class Bzip2Encoder : public ByteSink {
	public:
		Bzip2Encoder(ByteSink& out, int level);
		~Bzip2Encoder() override;

		bool write(const std::uint8_t* data, std::size_t size) override;
		bool finish();

		struct State;

	private:
		bool flushRun();
		bool emitBlock();
		bool drainBytes();

		ByteSink& out_;
		std::unique_ptr<State> state_;
	};

	/// Appends one bzip2 stream for @p data.
	void bzip2Data(const std::uint8_t* data, std::size_t size, int level,
	               std::vector<std::uint8_t>& out);

}  // namespace wbsh

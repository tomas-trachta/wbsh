#pragma once

/**
 * @file deflate.h
 * @brief RFC 1951 (DEFLATE) encoder: hash-chain LZ77 with lazy matching,
 *        each block written as whichever of dynamic Huffman, fixed Huffman
 *        or stored is smallest.
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "bytestream.h"

namespace wbsh {

	/// Streaming encoder for one raw DEFLATE stream (no gzip / zlib wrapper).
	/// @p level runs 1 (fastest) to 9 (smallest); 0 stores. Memory stays
	/// bounded: input more than a window behind the encoder is dropped.
	class DeflateEncoder : public ByteSink {
	public:
		DeflateEncoder(ByteSink& out, int level);
		~DeflateEncoder() override;

		bool write(const std::uint8_t* data, std::size_t size) override;
		bool finish();

		struct State;

	private:
		bool tokenize(bool final_block);
		bool emitBlock(bool final_block);
		bool emitStored(bool final_block);
		bool drain();

		ByteSink& out_;
		std::unique_ptr<State> state_;
	};

	/// Appends the raw DEFLATE stream for @p data to @p out.
	void deflateRaw(const std::uint8_t* data, std::size_t size, int level,
	                std::vector<std::uint8_t>& out);

}  // namespace wbsh

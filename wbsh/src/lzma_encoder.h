#pragma once

/**
 * @file lzma_encoder.h
 * @brief LZMA encoder (lc=3 lp=0 pb=2) framed as an LZMA2 stream, the
 *        payload of an .xz block, or as a legacy .lzma file.
 */

#include <cstddef>
#include <cstdint>
#include <memory>

#include "bytestream.h"

namespace wbsh {

	struct LzmaPreset {
		std::uint32_t dictionary;
		std::uint32_t nice_length;
		std::uint32_t depth;
		bool optimal;
	};

	/// xz's preset for @p level 0-9: dictionary size, how hard the match
	/// finder looks, and whether the optimal parser picks the symbols.
	LzmaPreset lzmaPreset(int level, bool extreme);

	/// The LZMA2 dictionary-size property: the smallest that covers @p dictionary.
	std::uint8_t lzma2DictionaryByte(std::uint32_t dictionary);

	/// Streaming encoder for one LZMA2 stream, end marker included. Memory
	/// stays bounded by the dictionary whatever the data size.
	class Lzma2Encoder : public ByteSink {
	public:
		Lzma2Encoder(ByteSink& out, const LzmaPreset& preset);
		~Lzma2Encoder() override;

		bool write(const std::uint8_t* data, std::size_t size) override;
		bool finish();

		struct State;

	private:
		bool encodeAvailable(bool final_input);
		bool closeChunk();
		bool writeLzmaChunk();
		bool writeStoredChunks();

		ByteSink& out_;
		std::unique_ptr<State> state_;
	};

	/// Streaming encoder for one .lzma file: the header says the size is
	/// unknown and an end-of-payload marker closes the data, as xz writes it.
	class LzmaAloneEncoder : public ByteSink {
	public:
		LzmaAloneEncoder(ByteSink& out, const LzmaPreset& preset);
		~LzmaAloneEncoder() override;

		bool write(const std::uint8_t* data, std::size_t size) override;
		bool finish();

		struct State;

	private:
		bool drain();

		ByteSink& out_;
		std::unique_ptr<State> state_;
	};

}  // namespace wbsh

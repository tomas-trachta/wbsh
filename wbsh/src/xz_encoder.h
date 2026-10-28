#pragma once

/**
 * @file xz_encoder.h
 * @brief .xz stream writer: one LZMA2 block carrying a CRC32, CRC64 or
 *        SHA-256 check (or none), then the index and the stream footer.
 */

#include <cstddef>
#include <cstdint>
#include <memory>

#include "bytestream.h"
#include "lzma_encoder.h"

namespace wbsh {

	/// The check IDs as the stream flags store them.
	enum class XzCheck : std::uint8_t {
		None = 0x00,
		Crc32 = 0x01,
		Crc64 = 0x04,
		Sha256 = 0x0A,
	};

	/// Streaming encoder for one .xz stream. Empty input gets no block at
	/// all, as xz writes it.
	class XzEncoder : public ByteSink {
	public:
		XzEncoder(ByteSink& out, const LzmaPreset& preset, XzCheck check);
		~XzEncoder() override;

		bool write(const std::uint8_t* data, std::size_t size) override;
		bool finish();

		struct State;

	private:
		bool startBlock();
		bool finishBlock();
		bool writeIndexAndFooter();

		ByteSink& out_;
		std::unique_ptr<State> state_;
	};

}  // namespace wbsh

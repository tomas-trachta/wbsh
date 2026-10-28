/**
 * @file xz_encoder.cpp
 * @brief .xz container writing per the .xz file format 1.0.4: stream
 *        header, one block holding the LZMA2 data and its check, the index
 *        and the stream footer.
 */

#include "xz_encoder.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <bcrypt.h>
#  pragma comment(lib, "bcrypt.lib")
#endif /* _WIN32 */

#include <algorithm>
#include <vector>

namespace wbsh {

	namespace xz_encoder_detail {

		/// SHA-256 through CNG; ok() is false when no provider is available.
		class Sha256 {
		public:
			Sha256();
			~Sha256();
			Sha256(const Sha256&) = delete;
			Sha256& operator=(const Sha256&) = delete;

			bool ok() const { return hash_ != nullptr; }
			void update(const std::uint8_t* data, std::size_t size);
			std::vector<std::uint8_t> digest();

		private:
			void* algorithm_ = nullptr;
			void* hash_ = nullptr;
		};

		/// The block check over the uncompressed data, kept as it streams by.
		class RunningCheck {
		public:
			explicit RunningCheck(XzCheck kind);

			bool ok() const { return !sha256_ || sha256_->ok(); }
			void update(const std::uint8_t* data, std::size_t size);
			std::vector<std::uint8_t> value();

		private:
			XzCheck kind_;
			std::uint32_t crc32_ = 0;
			std::uint64_t crc64_ = 0;
			std::unique_ptr<Sha256> sha256_;
		};

	}  // namespace xz_encoder_detail

	using xz_encoder_detail::RunningCheck;
	using xz_encoder_detail::Sha256;

	static const std::uint8_t kXzMagic[6] = { 0xFD, '7', 'z', 'X', 'Z', 0x00 };
	static const std::uint8_t kFooterMagic[2] = { 'Y', 'Z' };
	static const std::uint8_t kLzma2FilterId = 0x21;
	static const std::size_t kSha256Size = 32;

#ifdef _WIN32
	Sha256::Sha256() {
		BCRYPT_ALG_HANDLE algorithm = nullptr;
		if (::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
			return;
		}

		algorithm_ = algorithm;

		BCRYPT_HASH_HANDLE hash = nullptr;
		if (::BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0) hash_ = hash;
	}

	Sha256::~Sha256() {
		if (hash_ != nullptr) ::BCryptDestroyHash(hash_);
		if (algorithm_ != nullptr) ::BCryptCloseAlgorithmProvider(algorithm_, 0);
	}

	// CNG takes a non-const pointer but only reads the data.
	void Sha256::update(const std::uint8_t* data, std::size_t size) {
		static const std::size_t kMaxPiece = 1u << 30;
		for (std::size_t offset = 0; offset < size; offset += kMaxPiece) {
			const auto piece = static_cast<ULONG>((std::min)(kMaxPiece, size - offset));
			::BCryptHashData(hash_, const_cast<PUCHAR>(data + offset), piece, 0);
		}
	}

	std::vector<std::uint8_t> Sha256::digest() {
		std::vector<std::uint8_t> value(kSha256Size, 0);
		::BCryptFinishHash(hash_, value.data(), static_cast<ULONG>(value.size()), 0);
		return value;
	}
#else
	Sha256::Sha256() = default;
	Sha256::~Sha256() = default;
	void Sha256::update(const std::uint8_t*, std::size_t) {}
	std::vector<std::uint8_t> Sha256::digest() { return std::vector<std::uint8_t>(kSha256Size, 0); }
#endif /* _WIN32 */

	static void appendLe(std::vector<std::uint8_t>& bytes, std::uint64_t value, int width) {
		for (int k = 0; k < width; ++k) {
			bytes.push_back(static_cast<std::uint8_t>(value >> (8 * k)));
		}
	}

	static void appendVli(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
		while (value >= 0x80) {
			bytes.push_back(static_cast<std::uint8_t>(value | 0x80));
			value >>= 7;
		}

		bytes.push_back(static_cast<std::uint8_t>(value));
	}

	static void appendCrc32(std::vector<std::uint8_t>& bytes, std::size_t from) {
		appendLe(bytes, crc32Of(0, bytes.data() + from, bytes.size() - from), 4);
	}

	static void padToFour(std::vector<std::uint8_t>& bytes) {
		while (bytes.size() % 4 != 0) bytes.push_back(0x00);
	}

	RunningCheck::RunningCheck(XzCheck kind) : kind_(kind) {
		if (kind == XzCheck::Sha256) sha256_ = std::make_unique<Sha256>();
	}

	void RunningCheck::update(const std::uint8_t* data, std::size_t size) {
		switch (kind_) {
			case XzCheck::Crc32:
				crc32_ = crc32Of(crc32_, data, size);
				break;
			case XzCheck::Crc64:
				crc64_ = crc64Of(crc64_, data, size);
				break;
			case XzCheck::Sha256:
				sha256_->update(data, size);
				break;
			case XzCheck::None:
			default:
				break;
		}
	}

	std::vector<std::uint8_t> RunningCheck::value() {
		std::vector<std::uint8_t> bytes;
		switch (kind_) {
			case XzCheck::Crc32:
				appendLe(bytes, crc32_, 4);
				break;
			case XzCheck::Crc64:
				appendLe(bytes, crc64_, 8);
				break;
			case XzCheck::Sha256:
				bytes = sha256_->digest();
				break;
			case XzCheck::None:
			default:
				break;
		}

		return bytes;
	}

	// Stream header: magic, stream flags (a zero byte, the check ID), CRC32
	// of the flags.
	static std::vector<std::uint8_t> streamHeader(XzCheck check) {
		std::vector<std::uint8_t> bytes(kXzMagic, kXzMagic + sizeof(kXzMagic));
		bytes.push_back(0x00);
		bytes.push_back(static_cast<std::uint8_t>(check));
		appendCrc32(bytes, sizeof(kXzMagic));
		return bytes;
	}

	// Block header: its size in four-byte units less one, block flags (one
	// filter, neither size stored), the LZMA2 filter flags (ID, property
	// size, dictionary property), padding, CRC32.
	static std::vector<std::uint8_t> blockHeader(const LzmaPreset& preset) {
		const std::uint8_t dictionary = lzma2DictionaryByte(preset.dictionary);
		std::vector<std::uint8_t> bytes = { 0x00, 0x00, kLzma2FilterId, 0x01, dictionary };
		padToFour(bytes);
		bytes[0] = static_cast<std::uint8_t>((bytes.size() + 4) / 4 - 1);
		appendCrc32(bytes, 0);
		return bytes;
	}

	// Index: indicator, record count, per block its unpadded and
	// uncompressed sizes, padding, CRC32.
	static std::vector<std::uint8_t> indexRecords(bool has_block, std::uint64_t unpadded,
	                                              std::uint64_t unpacked) {
		std::vector<std::uint8_t> bytes = { 0x00 };
		appendVli(bytes, has_block ? 1 : 0);
		if (has_block) {
			appendVli(bytes, unpadded);
			appendVli(bytes, unpacked);
		}

		padToFour(bytes);
		appendCrc32(bytes, 0);
		return bytes;
	}

	// Stream footer: CRC32 of the next two fields, the index size in
	// four-byte units less one, the stream flags again, magic.
	static std::vector<std::uint8_t> streamFooter(std::size_t index_size, XzCheck check) {
		std::vector<std::uint8_t> fields;
		appendLe(fields, index_size / 4 - 1, 4);
		fields.push_back(0x00);
		fields.push_back(static_cast<std::uint8_t>(check));

		std::vector<std::uint8_t> bytes;
		appendLe(bytes, crc32Of(0, fields.data(), fields.size()), 4);
		bytes.insert(bytes.end(), fields.begin(), fields.end());
		bytes.insert(bytes.end(), kFooterMagic, kFooterMagic + sizeof(kFooterMagic));
		return bytes;
	}

	static bool writeAll(ByteSink& out, const std::vector<std::uint8_t>& bytes) {
		return bytes.empty() || out.write(bytes.data(), bytes.size());
	}

	struct XzEncoder::State {
		State(ByteSink& out, const LzmaPreset& chosen, XzCheck kind)
			: packed(out), preset(chosen), check(kind), running(kind) {}

		CountingSink packed;
		LzmaPreset preset;
		XzCheck check;
		RunningCheck running;
		std::unique_ptr<Lzma2Encoder> block;
		std::uint64_t unpacked = 0;
		std::uint64_t unpadded = 0;
		bool header_written = false;
	};

	XzEncoder::XzEncoder(ByteSink& out, const LzmaPreset& preset, XzCheck check)
		: out_(out), state_(std::make_unique<State>(out, preset, check)) {}

	XzEncoder::~XzEncoder() = default;

	bool XzEncoder::startBlock() {
		State& state = *state_;
		const std::vector<std::uint8_t> header = blockHeader(state.preset);
		if (!state.running.ok()) return false;
		if (!writeAll(out_, streamHeader(state.check)) || !writeAll(out_, header)) return false;

		state.header_written = true;
		state.unpadded = header.size();
		state.block = std::make_unique<Lzma2Encoder>(state.packed, state.preset);
		return true;
	}

	bool XzEncoder::write(const std::uint8_t* data, std::size_t size) {
		State& state = *state_;
		if (size == 0) return true;
		if (!state.block && !startBlock()) return false;

		state.running.update(data, size);
		state.unpacked += size;
		return state.block->write(data, size);
	}

	// Block padding brings the compressed data to a multiple of four bytes;
	// the unpadded size the index records leaves it out.
	bool XzEncoder::finishBlock() {
		State& state = *state_;
		if (!state.block->finish()) return false;

		const std::vector<std::uint8_t> check = state.running.value();
		const auto padding = static_cast<std::size_t>((4 - state.packed.count() % 4) % 4);
		std::vector<std::uint8_t> tail(padding, 0x00);
		tail.insert(tail.end(), check.begin(), check.end());
		state.unpadded += state.packed.count() + check.size();
		return writeAll(out_, tail);
	}

	bool XzEncoder::writeIndexAndFooter() {
		State& state = *state_;
		const bool has_block = state.block != nullptr;
		const std::vector<std::uint8_t> index =
			indexRecords(has_block, state.unpadded, state.unpacked);
		return writeAll(out_, index) && writeAll(out_, streamFooter(index.size(), state.check));
	}

	bool XzEncoder::finish() {
		State& state = *state_;
		if (state.block && !finishBlock()) return false;
		if (!state.header_written && !writeAll(out_, streamHeader(state.check))) return false;
		return writeIndexAndFooter();
	}

}  // namespace wbsh

/**
 * @file xz.cpp
 * @brief LZMA / LZMA2 / .xz decoding. The LZMA part follows the
 *        structure of Igor Pavlov's public-domain LzmaSpec reference
 *        decoder; the container follows the .xz file format 1.0.4.
 *        Everything streams: input is read as needed and output leaves
 *        through the dictionary window as it is produced.
 */

#include "xz.h"

#include <algorithm>
#include <cstring>

namespace wbsh {

	namespace xz_detail {

		typedef std::uint16_t Probability;

		struct RangeDecoder {
			InputBuffer* in = nullptr;
			std::uint64_t limit = ~0ULL;
			std::uint64_t used = 0;
			std::uint32_t range = 0xFFFFFFFFu;
			std::uint32_t code = 0;
			bool corrupted = false;

			std::uint8_t nextByte() {
				const int byte = used < limit ? in->get() : -1;
				if (byte < 0) {
					corrupted = true;
					return 0;
				}

				++used;
				return static_cast<std::uint8_t>(byte);
			}

			bool start() {
				range = 0xFFFFFFFFu;
				code = 0;
				const bool zero_first = nextByte() == 0;
				for (int k = 0; k < 4; ++k) code = (code << 8) | nextByte();
				return zero_first && code != range && !corrupted;
			}

			void normalize() {
				if (range >= (1u << 24)) return;
				range <<= 8;
				code = (code << 8) | nextByte();
			}

			unsigned bit(Probability& probability) {
				const std::uint32_t bound = (range >> 11) * probability;
				unsigned value = 0;
				if (code < bound) {
					const unsigned gain = (2048u - probability) >> 5;
					probability = static_cast<Probability>(probability + gain);
					range = bound;
				} else {
					probability = static_cast<Probability>(probability - (probability >> 5));
					code -= bound;
					range -= bound;
					value = 1;
				}

				normalize();
				return value;
			}

			std::uint32_t directBits(int count) {
				std::uint32_t result = 0;
				for (; count > 0; --count) {
					range >>= 1;
					code -= range;
					const std::uint32_t mask = 0u - (code >> 31);
					code += range & mask;
					if (code == range) corrupted = true;
					normalize();
					result = (result << 1) + (mask + 1);
				}

				return result;
			}
		};

		struct LengthDecoder {
			Probability choice = 1024;
			Probability choice2 = 1024;
			Probability low[16][8];
			Probability mid[16][8];
			Probability high[256];
		};

		struct LzmaProperties {
			unsigned lc = 3;
			unsigned lp = 0;
			unsigned pb = 2;
			std::uint32_t dictionary = 1u << 23;
		};

		struct LzmaState {
			LzmaProperties props;
			std::vector<Probability> literals;
			Probability is_match[192];
			Probability is_rep[12];
			Probability is_rep_g0[12];
			Probability is_rep_g1[12];
			Probability is_rep_g2[12];
			Probability is_rep0_long[192];
			Probability pos_slot[4][64];
			Probability pos_special[115];
			Probability align[16];
			LengthDecoder length;
			LengthDecoder rep_length;
			unsigned state = 0;
			std::uint32_t reps[4] = { 0, 0, 0, 0 };
		};

		struct Window {
			LzWindow* out = nullptr;
			std::uint32_t dictionary_size = 0;

			// LZMA distances are zero-based: 0 is the byte just written.
			std::uint8_t back(std::uint32_t distance) const {
				return out->back(static_cast<std::size_t>(distance) + 1);
			}
		};

		enum class DecodeEnd {
			Limit,
			Marker,
			Error,
		};

		struct BlockFilters {
			bool delta = false;
			unsigned delta_distance = 1;
			bool x86 = false;
			bool delta_listed_first = false;
			std::uint32_t dictionary = 0;
			std::uint64_t unpacked = 0;
			bool unpacked_known = false;
		};

	}  // namespace xz_detail

	using xz_detail::BlockFilters;
	using xz_detail::DecodeEnd;
	using xz_detail::LengthDecoder;
	using xz_detail::LzmaProperties;
	using xz_detail::LzmaState;
	using xz_detail::Probability;
	using xz_detail::RangeDecoder;
	using xz_detail::Window;

	static const Probability kProbabilityStart = 1024;
	static const unsigned kMatchMinLength = 2;
	static const std::uint32_t kEndMarker = 0xFFFFFFFFu;
	static const std::uint8_t kXzMagic[6] = { 0xFD, '7', 'z', 'X', 'Z', 0x00 };
	static const std::size_t kStreamHeaderSize = 12;
	static const std::size_t kStreamFooterSize = 12;
	static const std::size_t kMinWindow = 4096;

	static void fillProbabilities(Probability* first, std::size_t count) {
		std::fill(first, first + count, kProbabilityStart);
	}

	static void resetLength(LengthDecoder& decoder) {
		decoder.choice = kProbabilityStart;
		decoder.choice2 = kProbabilityStart;
		fillProbabilities(&decoder.low[0][0], 16 * 8);
		fillProbabilities(&decoder.mid[0][0], 16 * 8);
		fillProbabilities(decoder.high, 256);
	}

	static void resetLzmaState(LzmaState& lzma) {
		lzma.literals.assign(static_cast<std::size_t>(0x300) << (lzma.props.lc + lzma.props.lp),
			kProbabilityStart);
		fillProbabilities(lzma.is_match, 192);
		fillProbabilities(lzma.is_rep, 12);
		fillProbabilities(lzma.is_rep_g0, 12);
		fillProbabilities(lzma.is_rep_g1, 12);
		fillProbabilities(lzma.is_rep_g2, 12);
		fillProbabilities(lzma.is_rep0_long, 192);
		fillProbabilities(&lzma.pos_slot[0][0], 4 * 64);
		fillProbabilities(lzma.pos_special, 115);
		fillProbabilities(lzma.align, 16);
		resetLength(lzma.length);
		resetLength(lzma.rep_length);
		lzma.state = 0;
		std::fill(lzma.reps, lzma.reps + 4, 0u);
	}

	static unsigned bitTree(RangeDecoder& rc, Probability* probabilities, int bits) {
		unsigned node = 1;
		for (int k = 0; k < bits; ++k) node = (node << 1) + rc.bit(probabilities[node]);
		return node - (1u << bits);
	}

	static unsigned reverseBitTree(RangeDecoder& rc, Probability* probabilities, int bits) {
		unsigned node = 1;
		unsigned symbol = 0;
		for (int k = 0; k < bits; ++k) {
			const unsigned bit = rc.bit(probabilities[node]);
			node = (node << 1) + bit;
			symbol |= bit << k;
		}

		return symbol;
	}

	static unsigned decodeLength(RangeDecoder& rc, LengthDecoder& decoder, unsigned pos_state) {
		if (rc.bit(decoder.choice) == 0) return bitTree(rc, decoder.low[pos_state], 3);
		if (rc.bit(decoder.choice2) == 0) return 8 + bitTree(rc, decoder.mid[pos_state], 3);
		return 16 + bitTree(rc, decoder.high, 8);
	}

	static std::uint32_t decodeDistance(RangeDecoder& rc, LzmaState& lzma, unsigned length) {
		const unsigned length_state = (std::min)(length, 3u);
		const unsigned slot = bitTree(rc, lzma.pos_slot[length_state], 6);
		if (slot < 4) return slot;

		const int direct = static_cast<int>(slot >> 1) - 1;
		std::uint32_t distance = (2u | (slot & 1u)) << direct;
		if (slot < 14) {
			return distance + reverseBitTree(rc, lzma.pos_special + distance - slot, direct);
		}

		distance += rc.directBits(direct - 4) << 4;
		return distance + reverseBitTree(rc, lzma.align, 4);
	}

	static void decodeLiteral(RangeDecoder& rc, LzmaState& lzma, Window& window) {
		const std::uint64_t total = window.out->total();
		const unsigned previous = window.out->history() > 0 ? window.back(0) : 0;
		const unsigned lp_mask = (1u << lzma.props.lp) - 1;
		const std::size_t context = ((static_cast<unsigned>(total) & lp_mask) << lzma.props.lc)
			+ (previous >> (8 - lzma.props.lc));
		Probability* probabilities = lzma.literals.data() + 0x300 * context;

		unsigned symbol = 1;
		if (lzma.state >= 7) {
			unsigned match_byte = window.back(lzma.reps[0]);
			while (symbol < 0x100) {
				const unsigned match_bit = (match_byte >> 7) & 1;
				match_byte <<= 1;
				const unsigned bit = rc.bit(probabilities[((1 + match_bit) << 8) + symbol]);
				symbol = (symbol << 1) | bit;
				if (match_bit != bit) break;
			}
		}

		while (symbol < 0x100) symbol = (symbol << 1) | rc.bit(probabilities[symbol]);
		window.out->put(static_cast<std::uint8_t>(symbol - 0x100));
		lzma.state = lzma.state < 4 ? 0 : (lzma.state < 10 ? lzma.state - 3 : lzma.state - 6);
	}

	static bool copyMatch(Window& window, std::uint32_t distance, unsigned length) {
		if (distance >= window.dictionary_size) return false;
		return window.out->copy(static_cast<std::size_t>(distance) + 1, length);
	}

	// Returns 0 for a short rep (one byte already written), otherwise the
	// match length; the distance is left in reps[0].
	static unsigned decodeRep(RangeDecoder& rc, LzmaState& lzma, Window& window,
	                          unsigned pos_state) {
		if (rc.bit(lzma.is_rep_g0[lzma.state]) == 0) {
			if (rc.bit(lzma.is_rep0_long[(lzma.state << 4) + pos_state]) == 0) {
				lzma.state = lzma.state < 7 ? 9 : 11;
				window.out->put(window.back(lzma.reps[0]));
				return 0;
			}
		} else {
			std::uint32_t distance = 0;
			if (rc.bit(lzma.is_rep_g1[lzma.state]) == 0) {
				distance = lzma.reps[1];
			} else {
				const bool third = rc.bit(lzma.is_rep_g2[lzma.state]) == 0;
				distance = third ? lzma.reps[2] : lzma.reps[3];
				if (!third) lzma.reps[3] = lzma.reps[2];
				lzma.reps[2] = lzma.reps[1];
			}

			lzma.reps[1] = lzma.reps[0];
			lzma.reps[0] = distance;
		}

		const unsigned length = decodeLength(rc, lzma.rep_length, pos_state);
		lzma.state = lzma.state < 7 ? 8 : 11;
		return length + kMatchMinLength;
	}

	static DecodeEnd decodeMatch(RangeDecoder& rc, LzmaState& lzma, unsigned pos_state,
	                             unsigned& length) {
		lzma.reps[3] = lzma.reps[2];
		lzma.reps[2] = lzma.reps[1];
		lzma.reps[1] = lzma.reps[0];
		length = decodeLength(rc, lzma.length, pos_state);
		lzma.state = lzma.state < 7 ? 7 : 10;
		lzma.reps[0] = decodeDistance(rc, lzma, length);
		length += kMatchMinLength;
		return lzma.reps[0] == kEndMarker ? DecodeEnd::Marker : DecodeEnd::Limit;
	}

	static DecodeEnd decodeSymbols(RangeDecoder& rc, LzmaState& lzma, Window& window,
	                               std::uint64_t limit) {
		const unsigned pb_mask = (1u << lzma.props.pb) - 1;
		LzWindow& out = *window.out;
		while (out.total() < limit) {
			const unsigned pos_state = static_cast<unsigned>(out.total()) & pb_mask;
			if (rc.corrupted || !out.ok()) return DecodeEnd::Error;
			if (rc.bit(lzma.is_match[(lzma.state << 4) + pos_state]) == 0) {
				decodeLiteral(rc, lzma, window);
				continue;
			}

			unsigned length = 0;
			if (rc.bit(lzma.is_rep[lzma.state]) != 0) {
				if (out.history() == 0) return DecodeEnd::Error;
				length = decodeRep(rc, lzma, window, pos_state);
				if (length == 0) continue;
			} else if (decodeMatch(rc, lzma, pos_state, length) == DecodeEnd::Marker) {
				return DecodeEnd::Marker;
			}

			if (out.total() + length > limit) return DecodeEnd::Error;
			if (!copyMatch(window, lzma.reps[0], length)) return DecodeEnd::Error;
		}

		return rc.corrupted ? DecodeEnd::Error : DecodeEnd::Limit;
	}

	static bool decodePropertiesByte(std::uint8_t byte, LzmaProperties& props) {
		if (byte >= 9 * 5 * 5) return false;
		props.lc = byte % 9;
		byte = static_cast<std::uint8_t>(byte / 9);
		props.lp = byte % 5;
		props.pb = byte / 5u;
		return true;
	}

	static std::uint32_t readLe32(const std::uint8_t* bytes) {
		return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8)
			| (static_cast<std::uint32_t>(bytes[2]) << 16)
			| (static_cast<std::uint32_t>(bytes[3]) << 24);
	}

	static std::size_t windowSizeFor(std::uint32_t dictionary, std::uint64_t unpacked, bool unpacked_known) {
		std::uint64_t size = (std::max<std::uint64_t>)(dictionary, kMinWindow);
		if (unpacked_known) size = (std::min)(size, (std::max<std::uint64_t>)(unpacked, kMinWindow));
		return static_cast<std::size_t>(size);
	}

	static bool corrupt(std::string& error, const LzWindow& window) {
		if (window.ok()) error = "data is corrupt";
		return false;
	}

	bool unlzmaStream(InputBuffer& in, ByteSink& out, std::string& error) {
		std::uint8_t header[13];
		LzmaState lzma;
		if (!in.readExact(header, sizeof(header)) || !decodePropertiesByte(header[0], lzma.props)) {
			error = "not in lzma format";
			return false;
		}

		lzma.props.dictionary = (std::max)(readLe32(header + 1), 4096u);
		std::uint64_t unpacked = 0;
		for (int k = 7; k >= 0; --k) unpacked = (unpacked << 8) | header[5 + k];
		const bool known = unpacked != ~0ULL;

		LzWindow window_out(out, windowSizeFor(lzma.props.dictionary, unpacked, known));
		Window window;
		window.out = &window_out;
		window.dictionary_size = lzma.props.dictionary;
		resetLzmaState(lzma);

		RangeDecoder rc;
		rc.in = &in;
		if (!rc.start()) return corrupt(error, window_out);

		const DecodeEnd end = decodeSymbols(rc, lzma, window, known ? unpacked : ~0ULL);
		const bool finished = end == DecodeEnd::Marker || (end == DecodeEnd::Limit && known);
		if (finished && window_out.flush()) return true;
		return corrupt(error, window_out);
	}

	static bool readVli(const std::uint8_t* data, std::size_t size, std::size_t& at,
	                    std::uint64_t& value) {
		value = 0;
		for (int shift = 0; shift < 63; shift += 7) {
			if (at >= size) return false;
			const std::uint8_t byte = data[at++];
			value |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
			if ((byte & 0x80) == 0) return true;
		}

		return false;
	}

	static bool readStreamVli(InputBuffer& in, std::uint64_t& value) {
		value = 0;
		for (int shift = 0; shift < 63; shift += 7) {
			const int byte = in.get();
			if (byte < 0) return false;
			value |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
			if ((byte & 0x80) == 0) return true;
		}

		return false;
	}

	static std::uint32_t lzma2DictionarySize(std::uint8_t bits) {
		if (bits >= 40) return 0xFFFFFFFFu;
		return (2u | (bits & 1u)) << (bits / 2 + 11);
	}

	static bool parseFilter(std::uint64_t id, const std::uint8_t* props, std::uint64_t props_size,
	                        BlockFilters& filters) {
		if (id == 0x21 && props_size == 1 && props[0] <= 40) {
			filters.dictionary = lzma2DictionarySize(props[0]);
			return true;
		}

		if (id == 0x03 && props_size == 1) {
			filters.delta = true;
			filters.delta_listed_first = !filters.x86;
			filters.delta_distance = props[0] + 1u;
			return true;
		}

		if (id == 0x04 && props_size == 0) {
			filters.x86 = true;
			return true;
		}

		return false;
	}

	static bool parseFilters(const std::uint8_t* header, std::size_t header_size, std::size_t at, int count,
	                         BlockFilters& filters, std::string& error) {
		for (int k = 0; k < count; ++k) {
			std::uint64_t id = 0;
			std::uint64_t props_size = 0;
			const bool parsed = readVli(header, header_size, at, id)
				&& readVli(header, header_size, at, props_size)
				&& at + props_size <= header_size - 4
				&& parseFilter(id, header + at, props_size, filters);
			if (!parsed) {
				error = "unsupported filter chain";
				return false;
			}

			at += static_cast<std::size_t>(props_size);
		}

		return filters.dictionary != 0;
	}

	static bool parseBlockHeader(const std::uint8_t* header, std::size_t header_size,
	                             BlockFilters& filters, std::string& error) {
		if (crc32Of(0, header, header_size - 4) != readLe32(header + header_size - 4)) {
			error = "block header checksum mismatch";
			return false;
		}

		const std::uint8_t flags = header[1];
		std::size_t at = 2;
		std::uint64_t ignored = 0;
		if ((flags & 0x40) != 0 && !readVli(header, header_size, at, ignored)) return false;
		if ((flags & 0x80) != 0) {
			if (!readVli(header, header_size, at, filters.unpacked)) return false;
			filters.unpacked_known = true;
		}

		return parseFilters(header, header_size, at, (flags & 3) + 1, filters, error);
	}

	static bool isX86PrefixByte(std::uint8_t byte) {
		return byte == 0x00 || byte == 0xFF;
	}

	static std::uint32_t x86Destination(std::uint32_t source, std::uint32_t position,
	                                    std::uint32_t mask) {
		static const unsigned kMaskToBits[8] = { 0, 1, 2, 2, 3, 3, 3, 3 };
		for (;;) {
			const std::uint32_t destination = source - (position + 5);
			if (mask == 0) return destination;

			const unsigned shift = kMaskToBits[mask] * 8;
			if (!isX86PrefixByte(static_cast<std::uint8_t>(destination >> (24 - shift)))) {
				return destination;
			}

			source = destination ^ ((1u << (32 - shift)) - 1);
		}
	}

	static bool x86MaskRejects(std::uint32_t mask, const std::uint8_t* call) {
		static const bool kAllowed[8] = { true, true, true, false, true, false, false, false };
		static const unsigned kMaskToBits[8] = { 0, 1, 2, 2, 3, 3, 3, 3 };
		if (mask == 0) return false;
		return !kAllowed[mask] || isX86PrefixByte(call[4 - kMaskToBits[mask]]);
	}

	namespace xz_detail {

		// Undoes the block's filters on the decoder output as it streams by,
		// counts it and keeps the running check value.
		class BlockSink : public ByteSink {
		public:
			BlockSink(ByteSink& out, const BlockFilters& filters, unsigned check)
				: out_(out), filters_(filters), check_(check) {}

			bool write(const std::uint8_t* data, std::size_t size) override;
			bool finish();
			std::uint64_t size() const { return size_; }

			// The header lists filters in encoding order, so the one listed
			// last is undone first.
			bool x86UndoneFirst() const { return filters_.delta && filters_.x86 && filters_.delta_listed_first; }
			bool matches(const std::uint8_t* stored) const;

		private:
			void undoDelta(std::uint8_t* data, std::size_t size);
			std::size_t undoX86(std::uint8_t* data, std::size_t size, bool final_chunk);
			bool forward(const std::uint8_t* data, std::size_t size);

			ByteSink& out_;
			BlockFilters filters_;
			unsigned check_;
			std::uint64_t size_ = 0;
			std::uint32_t crc32_ = 0;
			std::uint64_t crc64_ = 0;
			std::uint8_t delta_history_[256] = {};
			std::uint8_t delta_pos_ = 0;
			std::vector<std::uint8_t> carry_;
			std::uint64_t x86_position_ = 0;
			std::uint64_t x86_previous_ = ~0ULL;
			std::uint32_t x86_mask_ = 0;
		};

	}  // namespace xz_detail

	using xz_detail::BlockSink;

	void BlockSink::undoDelta(std::uint8_t* data, std::size_t size) {
		for (std::size_t k = 0; k < size; ++k) {
			const auto earlier_pos = static_cast<std::uint8_t>(delta_pos_ - filters_.delta_distance);
			const std::uint8_t earlier = delta_history_[earlier_pos];
			data[k] = static_cast<std::uint8_t>(data[k] + earlier);
			delta_history_[delta_pos_++] = data[k];
		}
	}

	// The BCJ x86 filter turned relative CALL / JMP targets into absolute
	// ones; this mirrors the reference decoder (xz-embedded's bcj_x86),
	// whose previous-position bookkeeping starts at "minus one". Returns how
	// many bytes are final; the rest wait for the bytes that follow them.
	std::size_t BlockSink::undoX86(std::uint8_t* data, std::size_t size, bool final_chunk) {
		std::size_t k = 0;
		for (; k + 4 < size; ++k) {
			if ((data[k] & 0xFE) != 0xE8) continue;

			const std::uint64_t at = x86_position_ + k;
			const std::uint64_t gap = at - x86_previous_;
			x86_previous_ = at;
			x86_mask_ = gap > 3 ? 0 : (x86_mask_ << (gap - 1)) & 7;
			if (x86MaskRejects(x86_mask_, data + k) || !isX86PrefixByte(data[k + 4])) {
				x86_mask_ = (x86_mask_ << 1) | 1;
				continue;
			}

			const auto position = static_cast<std::uint32_t>(at);
			std::uint32_t destination = x86Destination(readLe32(data + k + 1), position, x86_mask_);
			destination &= 0x01FFFFFF;
			destination |= 0u - (destination & 0x01000000);
			for (int byte = 0; byte < 4; ++byte) {
				data[k + 1 + byte] = static_cast<std::uint8_t>(destination >> (8 * byte));
			}

			k += 4;
		}

		const std::size_t done = final_chunk ? size : (std::min)(k, size);
		x86_position_ += done;
		return done;
	}

	bool BlockSink::forward(const std::uint8_t* data, std::size_t size) {
		if (size == 0) return true;

		size_ += size;
		if (check_ == 1) crc32_ = crc32Of(crc32_, data, size);
		if (check_ == 4) crc64_ = crc64Of(crc64_, data, size);
		return out_.write(data, size);
	}

	bool BlockSink::write(const std::uint8_t* data, std::size_t size) {
		if (!filters_.delta && !filters_.x86) return forward(data, size);

		std::vector<std::uint8_t> chunk(data, data + size);
		if (filters_.delta && !x86UndoneFirst()) undoDelta(chunk.data(), chunk.size());
		if (!filters_.x86) return forward(chunk.data(), chunk.size());

		carry_.insert(carry_.end(), chunk.begin(), chunk.end());
		const std::size_t done = undoX86(carry_.data(), carry_.size(), false);
		if (x86UndoneFirst()) undoDelta(carry_.data(), done);
		const bool ok = forward(carry_.data(), done);
		carry_.erase(carry_.begin(), carry_.begin() + static_cast<std::ptrdiff_t>(done));
		return ok;
	}

	bool BlockSink::finish() {
		if (carry_.empty()) return true;

		const std::size_t done = undoX86(carry_.data(), carry_.size(), true);
		if (x86UndoneFirst()) undoDelta(carry_.data(), done);
		const bool ok = forward(carry_.data(), done);
		carry_.clear();
		return ok;
	}

	bool BlockSink::matches(const std::uint8_t* stored) const {
		if (check_ == 1) return crc32_ == readLe32(stored);
		if (check_ != 4) return true;

		std::uint64_t expected = 0;
		for (int k = 7; k >= 0; --k) expected = (expected << 8) | stored[k];
		return crc64_ == expected;
	}

	static bool skipBytes(InputBuffer& in, std::uint64_t count) {
		for (; count > 0; --count) {
			if (in.get() < 0) return false;
		}

		return true;
	}

	static bool lzma2Chunk(InputBuffer& in, LzmaState& lzma, Window& window, std::uint8_t control) {
		std::uint8_t sizes[4];
		if (!in.readExact(sizes, sizeof(sizes))) return false;

		const std::size_t unpacked = ((static_cast<std::size_t>(control & 0x1F) << 16)
			| (static_cast<std::size_t>(sizes[0]) << 8) | sizes[1]) + 1;
		const std::size_t packed = ((static_cast<std::size_t>(sizes[2]) << 8) | sizes[3]) + 1;

		const unsigned reset = (control >> 5) & 3;
		if (reset >= 2) {
			const int props = in.get();
			if (props < 0 || !decodePropertiesByte(static_cast<std::uint8_t>(props), lzma.props)) return false;
			if (lzma.props.lc + lzma.props.lp > 4) return false;
		}

		if (reset >= 1) resetLzmaState(lzma);

		RangeDecoder rc;
		rc.in = &in;
		rc.limit = packed;
		if (!rc.start()) return false;

		const DecodeEnd end = decodeSymbols(rc, lzma, window, window.out->total() + unpacked);
		return end == DecodeEnd::Limit && skipBytes(in, packed - rc.used);
	}

	static bool copyStoredChunk(InputBuffer& in, LzWindow& out) {
		std::uint8_t sizes[2];
		if (!in.readExact(sizes, sizeof(sizes))) return false;

		const std::size_t length = ((static_cast<std::size_t>(sizes[0]) << 8) | sizes[1]) + 1;
		for (std::size_t k = 0; k < length; ++k) {
			const int byte = in.get();
			if (byte < 0) return false;
			out.put(static_cast<std::uint8_t>(byte));
		}

		return out.ok();
	}

	static bool decodeLzma2(InputBuffer& in, std::uint32_t dictionary, LzWindow& out) {
		LzmaState lzma;
		Window window;
		window.out = &out;
		window.dictionary_size = dictionary;
		bool need_properties = true;
		for (;;) {
			const int next = in.get();
			if (next < 0) return false;

			const std::uint8_t control = static_cast<std::uint8_t>(next);
			if (control == 0x00) return out.flush();
			if (control == 0x01 || control >= 0xE0) out.forgetHistory();

			if (control < 0x80) {
				if (control > 0x02 || !copyStoredChunk(in, out)) return false;
				continue;
			}

			if (need_properties && control < 0xC0) return false;
			need_properties = false;
			if (!lzma2Chunk(in, lzma, window, control)) return false;
		}
	}

	static std::size_t checkSize(unsigned check) {
		static const std::size_t kSizes[16] = {
			0, 4, 4, 4, 8, 8, 8, 16, 16, 16, 32, 32, 32, 64, 64, 64,
		};
		return kSizes[check & 15];
	}

	static bool readBlockHeader(InputBuffer& in, int first, BlockFilters& filters, std::string& error) {
		const std::size_t header_size = (static_cast<std::size_t>(first) + 1) * 4;
		std::vector<std::uint8_t> header(header_size);
		header[0] = static_cast<std::uint8_t>(first);
		if (!in.readExact(header.data() + 1, header_size - 1)) return false;
		return parseBlockHeader(header.data(), header_size, filters, error);
	}

	static bool skipBlockPadding(InputBuffer& in, std::uint64_t data_start) {
		while ((in.consumed() - data_start) % 4 != 0) {
			if (in.get() != 0) return false;
		}

		return true;
	}

	static bool decodeBlock(InputBuffer& in, int first, unsigned check, ByteSink& out, std::string& error) {
		BlockFilters filters;
		if (!readBlockHeader(in, first, filters, error)) return false;

		BlockSink block(out, filters, check);
		LzWindow window(block, windowSizeFor(filters.dictionary, filters.unpacked, filters.unpacked_known));
		const std::uint64_t data_start = in.consumed();
		if (!decodeLzma2(in, filters.dictionary, window)) {
			if (window.ok()) error = "data is corrupt";
			return false;
		}

		if (!block.finish()) return false;

		std::uint8_t stored[64];
		const std::size_t check_bytes = checkSize(check);
		if (!skipBlockPadding(in, data_start) || !in.readExact(stored, check_bytes)) return false;
		if (block.matches(stored)) return true;

		error = "checksum mismatch";
		return false;
	}

	static bool skipIndexAndFooter(InputBuffer& in) {
		const std::uint64_t index_start = in.consumed() - 1;
		std::uint64_t records = 0;
		if (!readStreamVli(in, records)) return false;
		for (std::uint64_t k = 0; k < records * 2; ++k) {
			std::uint64_t ignored = 0;
			if (!readStreamVli(in, ignored)) return false;
		}

		while ((in.consumed() - index_start) % 4 != 0) {
			if (in.get() < 0) return false;
		}

		std::uint8_t tail[4 + kStreamFooterSize];
		return in.readExact(tail, sizeof(tail)) && tail[4 + 10] == 'Y' && tail[4 + 11] == 'Z';
	}

	static bool readStreamHeader(InputBuffer& in, unsigned& check, std::string& error) {
		std::uint8_t header[kStreamHeaderSize];
		if (!in.readExact(header, sizeof(header)) || std::memcmp(header, kXzMagic, 6) != 0) {
			error = "not in xz format";
			return false;
		}

		if (crc32Of(0, header + 6, 2) != readLe32(header + 8)) {
			error = "stream header checksum mismatch";
			return false;
		}

		check = header[7] & 0x0F;
		return true;
	}

	static bool decodeXzStream(InputBuffer& in, ByteSink& out, std::string& error) {
		unsigned check = 0;
		if (!readStreamHeader(in, check, error)) return false;

		for (;;) {
			const int first = in.get();
			if (first < 0) break;
			if (first == 0x00) {
				if (skipIndexAndFooter(in)) return true;
				break;
			}

			if (!decodeBlock(in, first, check, out, error)) return false;
		}

		error = "truncated or corrupt index";
		return false;
	}

	static void skipStreamPadding(InputBuffer& in) {
		const std::uint8_t* next = nullptr;
		while (in.peek(next, 4) == 4 && readLe32(next) == 0) skipBytes(in, 4);
	}

	bool unxzStream(InputBuffer& in, ByteSink& out, std::string& error) {
		do {
			if (!decodeXzStream(in, out, error)) return false;
			skipStreamPadding(in);
		} while (!in.atEnd());

		return true;
	}

	bool unxzData(const std::uint8_t* in, std::size_t size, std::vector<std::uint8_t>& out,
	              std::string& error) {
		MemorySource source(in, size);
		InputBuffer input(source);
		VectorSink sink(out);
		return unxzStream(input, sink, error);
	}

	bool unlzmaData(const std::uint8_t* in, std::size_t size, std::vector<std::uint8_t>& out,
	                std::string& error) {
		MemorySource source(in, size);
		InputBuffer input(source);
		VectorSink sink(out);
		return unlzmaStream(input, sink, error);
	}

}  // namespace wbsh

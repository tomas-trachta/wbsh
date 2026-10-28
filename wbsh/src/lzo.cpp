/**
 * @file lzo.cpp
 * @brief LZO1X. The compressor is liblzo's lzo1x_1 scheme: one hash probe
 *        of the next four bytes per position, literal runs that skip ahead
 *        faster the longer they grow, and the input taken in 48 KiB pieces
 *        so every offset fits the format. Opcodes follow lzo1x_d.ch.
 */

#include "lzo.h"

#include <algorithm>
#include <cstring>

namespace wbsh {

	namespace lzo_detail {

		struct Encoder {
			const std::uint8_t* data = nullptr;
			std::vector<std::uint8_t>* out = nullptr;
			std::vector<std::uint16_t> table;
			int hash_bits = 14;
		};

		struct Decoder {
			const std::uint8_t* in = nullptr;
			std::size_t in_size = 0;
			std::size_t ip = 0;
			std::uint8_t* out = nullptr;
			std::size_t out_size = 0;
			std::size_t op = 0;
		};

		struct Instruction {
			std::size_t distance = 0;
			std::size_t length = 0;
			std::size_t trailing = 0;
			bool end = false;
		};

	}  // namespace lzo_detail

	using lzo_detail::Decoder;
	using lzo_detail::Encoder;
	using lzo_detail::Instruction;

	static const std::size_t kM2MaxLength      = 8;
	static const std::size_t kM3MaxLength      = 33;
	static const std::size_t kM4MaxLength      = 9;
	static const std::size_t kM2MaxOffset      = 0x0800;
	static const std::size_t kM3MaxOffset      = 0x4000;
	static const std::size_t kM4MaxOffset      = 0xBFFF;
	static const std::size_t kM2Opcode         = 64;
	static const std::size_t kM3Opcode         = 32;
	static const std::size_t kM4Opcode         = 16;
	static const std::size_t kM3LengthMask     = 31;
	static const std::size_t kM4LengthMask     = 7;
	static const std::size_t kRunLengthMask    = 15;
	static const std::size_t kChunkSize        = kM4MaxOffset + 1;
	static const std::size_t kTailMargin       = 20;
	static const std::size_t kMinMatch         = 4;
	static const std::size_t kMinRun           = 3;
	static const std::size_t kMaxShortRun      = 18;
	static const std::size_t kMaxTrailing      = 3;
	static const std::size_t kFirstRunBias     = 17;
	static const std::size_t kMaxFirstRun      = 238;
	static const std::size_t kAfterRun         = 4;
	static const std::size_t kSkipShift        = 5;
	static const std::uint32_t kHashMultiplier = 0x1824429D;

	static std::uint32_t loadLe32(const std::uint8_t* bytes) {
		return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8)
			| (static_cast<std::uint32_t>(bytes[2]) << 16)
			| (static_cast<std::uint32_t>(bytes[3]) << 24);
	}

	static void putByte(Encoder& encoder, std::size_t value) {
		encoder.out->push_back(static_cast<std::uint8_t>(value & 0xFF));
	}

	// A length too big for its opcode field: a zero field, then 255 for
	// every zero byte, then the non-zero rest.
	static void putLengthTail(Encoder& encoder, std::size_t rest) {
		for (; rest > 255; rest -= 255) putByte(encoder, 0);
		putByte(encoder, rest);
	}

	// Up to three literals ride in the low bits of the previous match's
	// second-to-last byte.
	static void putLiteralCount(Encoder& encoder, std::size_t count) {
		std::vector<std::uint8_t>& out = *encoder.out;
		if (count <= kMaxTrailing) {
			out[out.size() - 2] = static_cast<std::uint8_t>(out[out.size() - 2] | count);
		} else if (count <= kMaxShortRun) {
			putByte(encoder, count - kMinRun);
		} else {
			putByte(encoder, 0);
			putLengthTail(encoder, count - kMaxShortRun);
		}
	}

	static void copyInput(Encoder& encoder, std::size_t from, std::size_t count) {
		encoder.out->insert(encoder.out->end(), encoder.data + from, encoder.data + from + count);
	}

	static void putLiterals(Encoder& encoder, std::size_t from, std::size_t count) {
		if (count == 0) return;

		putLiteralCount(encoder, count);
		copyInput(encoder, from, count);
	}

	// A block without a single match opens with 17 + count, which needs no
	// length field; liblzo uses that form for nothing else.
	static void putFinalLiterals(Encoder& encoder, std::size_t from, std::size_t count) {
		if (count == 0) return;

		if (!encoder.out->empty() || count > kMaxFirstRun) {
			putLiterals(encoder, from, count);
			return;
		}

		putByte(encoder, kFirstRunBias + count);
		copyInput(encoder, from, count);
	}

	// M3 and M4: opcode with the length, then the offset shifted past the
	// two trailing-literal bits, little-endian.
	static void putLongMatch(Encoder& encoder, std::size_t opcode, std::size_t length,
	                         std::size_t max_length, std::size_t offset) {
		if (length <= max_length) {
			putByte(encoder, opcode | (length - 2));
		} else {
			putByte(encoder, opcode);
			putLengthTail(encoder, length - max_length);
		}

		putByte(encoder, offset << 2);
		putByte(encoder, offset >> 6);
	}

	// M2: 3 bits of length and 11 bits of offset in two bytes.
	static void putShortMatch(Encoder& encoder, std::size_t length, std::size_t offset) {
		putByte(encoder, ((length - 1) << 5) | ((offset & 7) << 2));
		putByte(encoder, offset >> 3);
	}

	static void putMatch(Encoder& encoder, std::size_t length, std::size_t distance) {
		if (length <= kM2MaxLength && distance <= kM2MaxOffset) {
			putShortMatch(encoder, length, distance - 1);
		} else if (distance <= kM3MaxOffset) {
			putLongMatch(encoder, kM3Opcode, length, kM3MaxLength, distance - 1);
		} else {
			const std::size_t offset = distance - kM3MaxOffset;
			putLongMatch(encoder, kM4Opcode | ((offset >> 11) & 8), length, kM4MaxLength, offset);
		}
	}

	static void putEndMarker(Encoder& encoder) {
		putByte(encoder, kM4Opcode | 1);
		putByte(encoder, 0);
		putByte(encoder, 0);
	}

	static std::size_t hashAt(const Encoder& encoder, std::size_t at) {
		const std::uint32_t mixed = loadLe32(encoder.data + at) * kHashMultiplier;
		return static_cast<std::size_t>(mixed >> (32 - encoder.hash_bits));
	}

	static std::uint64_t loadLe64(const std::uint8_t* bytes) {
		const std::uint64_t high = loadLe32(bytes + 4);
		return static_cast<std::uint64_t>(loadLe32(bytes)) | (high << 32);
	}

	static std::uint64_t groupDifference(const Encoder& encoder, std::size_t at,
	                                     std::size_t candidate) {
		return loadLe64(encoder.data + at) ^ loadLe64(encoder.data + candidate);
	}

	static std::size_t equalLowBytes(std::uint64_t difference) {
		std::size_t count = 0;
		for (; (difference & 0xFF) == 0; difference >>= 8) ++count;
		return count;
	}

	// Compared eight bytes at a time, as liblzo's 64-bit build does: the
	// first group to reach the limit ends the match, up to seven bytes past
	// it, which the tail margin keeps inside the input. Doing the same keeps
	// the output byte-identical to lzop's.
	static std::size_t matchLength(const Encoder& encoder, std::size_t at, std::size_t candidate,
	                               std::size_t limit) {
		std::size_t length = kMinMatch;
		std::uint64_t difference = groupDifference(encoder, at + length, candidate + length);
		while (difference == 0) {
			length += 8;
			difference = groupDifference(encoder, at + length, candidate + length);
			if (at + length >= limit) return length;
		}

		return length + equalLowBytes(difference);
	}

	static std::size_t skipAfterMiss(std::size_t at, std::size_t run_start) {
		return at + 1 + ((at - run_start) >> kSkipShift);
	}

	// One piece of at most kChunkSize bytes with a fresh table, so every
	// offset stays below kChunkSize. @p pending literals from earlier pieces
	// still wait for a match; returns how many wait after this one.
	static std::size_t compressChunk(Encoder& encoder, std::size_t base, std::size_t length,
	                                 std::size_t pending) {
		std::fill(encoder.table.begin(), encoder.table.end(), static_cast<std::uint16_t>(0));

		const std::size_t limit = base + length - kTailMargin;
		std::size_t literal_start = base - pending;
		std::size_t at = base + (pending < kMinMatch ? kMinMatch - pending : 0);
		at = skipAfterMiss(at, base);

		while (at < limit) {
			const std::size_t slot = hashAt(encoder, at);
			const std::size_t candidate = base + encoder.table[slot];
			encoder.table[slot] = static_cast<std::uint16_t>(at - base);
			if (loadLe32(encoder.data + candidate) != loadLe32(encoder.data + at)) {
				at = skipAfterMiss(at, (std::max)(literal_start, base));
				continue;
			}

			putLiterals(encoder, literal_start, at - literal_start);

			const std::size_t match = matchLength(encoder, at, candidate, limit);
			putMatch(encoder, match, at - candidate);
			at += match;
			literal_start = at;
		}

		return base + length - literal_start;
	}

	void lzo1xCompress(const std::uint8_t* data, std::size_t size, Lzo1xDictionary dictionary,
	                   std::vector<std::uint8_t>& out) {
		out.clear();
		out.reserve(size + size / 16 + 64 + 3);

		Encoder encoder;
		encoder.data = data;
		encoder.out = &out;
		encoder.hash_bits = static_cast<int>(dictionary);
		encoder.table.assign(static_cast<std::size_t>(1) << encoder.hash_bits, 0);

		std::size_t done = 0;
		std::size_t pending = 0;
		while (size - done > kTailMargin) {
			const std::size_t length = (std::min)(size - done, kChunkSize);
			pending = compressChunk(encoder, done, length, pending);
			done += length;
		}

		pending += size - done;
		putFinalLiterals(encoder, size - pending, pending);
		putEndMarker(encoder);
	}

	static bool takeByte(Decoder& decoder, std::size_t& value) {
		if (decoder.ip >= decoder.in_size) return false;

		value = decoder.in[decoder.ip++];
		return true;
	}

	static bool takeLengthTail(Decoder& decoder, std::size_t field_max, std::size_t& length) {
		std::size_t value = 0;
		length = field_max;
		for (;;) {
			if (!takeByte(decoder, value)) return false;
			if (value != 0) break;

			length += 255;
			if (length > decoder.out_size) return false;
		}

		length += value;
		return true;
	}

	static bool takeLength(Decoder& decoder, std::size_t field, std::size_t field_max,
	                       std::size_t& length) {
		if (field != 0) {
			length = field + 2;
			return true;
		}

		if (!takeLengthTail(decoder, field_max, length)) return false;
		length += 2;
		return true;
	}

	static bool takeOffsetPair(Decoder& decoder, std::size_t& offset, std::size_t& trailing) {
		std::size_t low = 0;
		std::size_t high = 0;
		if (!takeByte(decoder, low) || !takeByte(decoder, high)) return false;

		offset = (low >> 2) + (high << 6);
		trailing = low & 3;
		return true;
	}

	static bool copyLiterals(Decoder& decoder, std::size_t count) {
		const bool fits_input = count <= decoder.in_size - decoder.ip;
		const bool fits_output = count <= decoder.out_size - decoder.op;
		if (!fits_input || !fits_output) return false;

		std::memcpy(decoder.out + decoder.op, decoder.in + decoder.ip, count);
		decoder.ip += count;
		decoder.op += count;
		return true;
	}

	// Byte by byte: a match may overlap the bytes it is producing.
	static bool copyMatch(Decoder& decoder, std::size_t distance, std::size_t length) {
		const bool inside_history = distance != 0 && distance <= decoder.op;
		const bool fits_output = length <= decoder.out_size - decoder.op;
		if (!inside_history || !fits_output) return false;

		std::uint8_t* target = decoder.out + decoder.op;
		const std::uint8_t* source = target - distance;
		for (std::size_t k = 0; k < length; ++k) target[k] = source[k];
		decoder.op += length;
		return true;
	}

	static bool copyLiteralRun(Decoder& decoder, std::size_t opcode) {
		std::size_t count = opcode;
		if (opcode == 0 && !takeLengthTail(decoder, kRunLengthMask, count)) return false;
		return copyLiterals(decoder, count + kMinRun);
	}

	// M1, opcode below 16 after literals: a 3-byte copy from beyond the M2
	// range right after a run of four or more, otherwise a 2-byte copy.
	static bool readM1(Decoder& decoder, std::size_t opcode, std::size_t state,
	                   Instruction& match) {
		std::size_t high = 0;
		if (!takeByte(decoder, high)) return false;

		const bool after_run = state == kAfterRun;
		match.distance = 1 + (opcode >> 2) + (high << 2) + (after_run ? kM2MaxOffset : 0);
		match.length = after_run ? 3 : 2;
		match.trailing = opcode & 3;
		return true;
	}

	static bool readM2(Decoder& decoder, std::size_t opcode, Instruction& match) {
		std::size_t high = 0;
		if (!takeByte(decoder, high)) return false;

		match.distance = 1 + ((opcode >> 2) & 7) + (high << 3);
		match.length = (opcode >> 5) + 1;
		match.trailing = opcode & 3;
		return true;
	}

	static bool readM3(Decoder& decoder, std::size_t opcode, Instruction& match) {
		std::size_t offset = 0;
		if (!takeLength(decoder, opcode & kM3LengthMask, kM3LengthMask, match.length)) return false;
		if (!takeOffsetPair(decoder, offset, match.trailing)) return false;

		match.distance = offset + 1;
		return true;
	}

	// M4 with a zero distance is the end-of-stream marker.
	static bool readM4(Decoder& decoder, std::size_t opcode, Instruction& match) {
		std::size_t offset = 0;
		if (!takeLength(decoder, opcode & kM4LengthMask, kM4LengthMask, match.length)) return false;
		if (!takeOffsetPair(decoder, offset, match.trailing)) return false;

		match.distance = ((opcode & 8) << 11) + offset;
		match.end = match.distance == 0;
		match.distance += kM3MaxOffset;
		return true;
	}

	static bool readMatch(Decoder& decoder, std::size_t opcode, std::size_t state,
	                      Instruction& match) {
		if (opcode >= kM2Opcode) return readM2(decoder, opcode, match);
		if (opcode >= kM3Opcode) return readM3(decoder, opcode, match);
		if (opcode >= kM4Opcode) return readM4(decoder, opcode, match);
		return readM1(decoder, opcode, state, match);
	}

	// A first byte above 17 is a literal run of byte - 17 with no length
	// field. The state is how many literals came last: 0, 1-3, or
	// kAfterRun for a longer run.
	static bool openStream(Decoder& decoder, std::size_t& state) {
		state = 0;
		if (decoder.in_size == 0 || decoder.in[0] <= kFirstRunBias) return true;

		const std::size_t count = decoder.in[decoder.ip++] - kFirstRunBias;
		state = (std::min)(count, kAfterRun);
		return copyLiterals(decoder, count);
	}

	static bool decodeInstructions(Decoder& decoder) {
		std::size_t state = 0;
		if (!openStream(decoder, state)) return false;

		for (;;) {
			std::size_t opcode = 0;
			if (!takeByte(decoder, opcode)) return false;

			if (opcode < kM4Opcode && state == 0) {
				if (!copyLiteralRun(decoder, opcode)) return false;
				state = kAfterRun;
				continue;
			}

			Instruction match;
			if (!readMatch(decoder, opcode, state, match)) return false;
			if (match.end) return true;
			if (!copyMatch(decoder, match.distance, match.length)) return false;
			if (!copyLiterals(decoder, match.trailing)) return false;
			state = match.trailing;
		}
	}

	bool lzo1xDecompress(const std::uint8_t* data, std::size_t size, std::uint8_t* out,
	                     std::size_t out_size) {
		Decoder decoder;
		decoder.in = data;
		decoder.in_size = size;
		decoder.out = out;
		decoder.out_size = out_size;
		return decodeInstructions(decoder) && decoder.ip == size && decoder.op == out_size;
	}

}  // namespace wbsh

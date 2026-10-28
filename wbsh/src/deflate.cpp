/**
 * @file deflate.cpp
 * @brief DEFLATE encoder. Block layout and code tables follow RFC 1951
 *        section 3.2; bits go out least significant first, Huffman codes
 *        most significant first.
 */

#include "deflate.h"

#include "bytestream.h"

#include <algorithm>

#include "huffman.h"

namespace wbsh {

	namespace deflate_detail {

		struct Token {
			std::uint16_t length = 0;
			std::uint16_t distance = 0;
			std::uint8_t literal = 0;
		};

		struct BitWriter {
			std::vector<std::uint8_t>* out = nullptr;
			std::uint64_t buffer = 0;
			int count = 0;

			void put(std::uint32_t bits, int width) {
				buffer |= static_cast<std::uint64_t>(bits) << count;
				count += width;
				while (count >= 8) {
					out->push_back(static_cast<std::uint8_t>(buffer & 0xFF));
					buffer >>= 8;
					count -= 8;
				}
			}

			void alignToByte() {
				if (count > 0) put(0, 8 - count);
			}
		};

		struct LevelTuning {
			int max_chain;
			int nice_length;
			bool lazy;
		};

		struct BlockCodes {
			std::vector<std::uint8_t> literal_lengths;
			std::vector<std::uint8_t> distance_lengths;
			std::vector<std::uint32_t> literal_codes;
			std::vector<std::uint32_t> distance_codes;
		};

		struct DynamicHeader {
			std::vector<std::uint8_t> symbols;
			std::vector<std::uint8_t> extras;
			std::vector<std::uint8_t> lengths;
			std::vector<std::uint32_t> codes;
			std::size_t literal_count = 0;
			std::size_t distance_count = 0;
			std::size_t code_length_count = 0;
		};

		struct MatchFinder {
			const std::uint8_t* data = nullptr;
			std::size_t size = 0;
			std::vector<int> head;
			std::vector<int> previous;
			LevelTuning tuning = { 0, 0, false };
		};

	}  // namespace deflate_detail

	using deflate_detail::BitWriter;
	using deflate_detail::BlockCodes;
	using deflate_detail::DynamicHeader;
	using deflate_detail::LevelTuning;
	using deflate_detail::MatchFinder;
	using deflate_detail::Token;

	static const int kWindowSize        = 32768;
	static const int kWindowMask        = kWindowSize - 1;
	static const int kHashBits          = 15;
	static const int kMinMatch          = 3;
	static const int kMaxMatch          = 258;
	static const std::size_t kBlockTokens  = 65536;
	static const std::size_t kStoredMax    = 65535;
	static const int kLiteralSymbols    = 286;
	static const int kDistanceSymbols   = 30;
	static const int kEndOfBlock        = 256;
	static const int kMaxCodeLength     = 15;
	static const int kMaxCodeLengthCode = 7;

	static const std::uint16_t kLengthBase[29] = {
		3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
		35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
	};

	static const std::uint8_t kLengthExtra[29] = {
		0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
		3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
	};

	static const std::uint16_t kDistanceBase[30] = {
		1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
		257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
	};

	static const std::uint8_t kDistanceExtra[30] = {
		0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
		7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
	};

	static const std::uint8_t kCodeLengthOrder[19] = {
		16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15,
	};

	static LevelTuning tuningFor(int level) {
		static const LevelTuning kTunings[10] = {
			{ 0, 0, false },     { 4, 8, false },     { 8, 16, false },   { 16, 32, false },
			{ 16, 32, true },    { 32, 64, true },    { 64, 128, true },  { 128, 128, true },
			{ 512, 258, true },  { 2048, 258, true },
		};
		return kTunings[(std::min)((std::max)(level, 0), 9)];
	}

	static std::uint32_t reverseBits(std::uint32_t code, int width) {
		std::uint32_t reversed = 0;
		for (int k = 0; k < width; ++k) {
			reversed = (reversed << 1) | (code & 1);
			code >>= 1;
		}

		return reversed;
	}

	static void putCode(BitWriter& writer, std::uint32_t code, int width) {
		writer.put(reverseBits(code, width), width);
	}

	static int lengthSymbol(int length) {
		int index = 28;
		while (kLengthBase[index] > length) --index;
		return index;
	}

	static int distanceSymbol(int distance) {
		int index = 29;
		while (kDistanceBase[index] > distance) --index;
		return index;
	}

	static std::uint32_t hashAt(const std::uint8_t* bytes) {
		const std::uint32_t value = (static_cast<std::uint32_t>(bytes[0]) << 16)
			| (static_cast<std::uint32_t>(bytes[1]) << 8) | bytes[2];
		return (value * 2654435761u) >> (32 - kHashBits);
	}

	static void insertPosition(MatchFinder& finder, std::size_t position) {
		if (position + kMinMatch > finder.size) return;
		const std::uint32_t hash = hashAt(finder.data + position);
		finder.previous[position & kWindowMask] = finder.head[hash];
		finder.head[hash] = static_cast<int>(position);
	}

	static int matchLength(const MatchFinder& finder, std::size_t candidate, std::size_t position) {
		const std::size_t limit = (std::min)(static_cast<std::size_t>(kMaxMatch),
			finder.size - position);
		std::size_t length = 0;
		const std::uint8_t* earlier = finder.data + candidate;
		const std::uint8_t* here = finder.data + position;
		while (length < limit && earlier[length] == here[length]) {
			++length;
		}

		return static_cast<int>(length);
	}

	static int longestMatch(const MatchFinder& finder, std::size_t position, int& distance) {
		if (position + kMinMatch > finder.size) return 0;

		int best = 0;
		int candidate = finder.head[hashAt(finder.data + position)];
		for (int chain = 0; chain < finder.tuning.max_chain && candidate >= 0; ++chain) {
			const std::size_t from = static_cast<std::size_t>(candidate);
			if (from >= position || position - from > static_cast<std::size_t>(kWindowSize)) break;

			const int length = matchLength(finder, from, position);
			if (length > best) {
				best = length;
				distance = static_cast<int>(position - from);
				if (length >= finder.tuning.nice_length) break;
			}

			const int next = finder.previous[from & kWindowMask];
			if (next >= candidate) break;
			candidate = next;
		}

		return best >= kMinMatch ? best : 0;
	}

	static Token literalToken(std::uint8_t byte) {
		Token token;
		token.literal = byte;
		return token;
	}

	static Token matchToken(int length, int distance) {
		Token token;
		token.length = static_cast<std::uint16_t>(length);
		token.distance = static_cast<std::uint16_t>(distance);
		return token;
	}

	static void insertRange(MatchFinder& finder, std::size_t from, std::size_t to) {
		for (std::size_t k = from; k < to; ++k) insertPosition(finder, k);
	}

	static std::size_t tokenizeStep(MatchFinder& finder, std::size_t position,
	                                std::vector<Token>& tokens) {
		int distance = 0;
		const int length = longestMatch(finder, position, distance);
		insertPosition(finder, position);
		if (length == 0) {
			tokens.push_back(literalToken(finder.data[position]));
			return position + 1;
		}

		if (finder.tuning.lazy && length < finder.tuning.nice_length) {
			int next_distance = 0;
			const int next_length = longestMatch(finder, position + 1, next_distance);
			if (next_length > length) {
				tokens.push_back(literalToken(finder.data[position]));
				return position + 1;
			}
		}

		tokens.push_back(matchToken(length, distance));
		insertRange(finder, position + 1, position + static_cast<std::size_t>(length));
		return position + static_cast<std::size_t>(length);
	}

	static void countFrequencies(const std::vector<Token>& tokens,
	                             std::vector<std::uint32_t>& literals,
	                             std::vector<std::uint32_t>& distances) {
		literals.assign(kLiteralSymbols, 0);
		distances.assign(kDistanceSymbols, 0);
		for (const Token& token : tokens) {
			if (token.length == 0) {
				++literals[token.literal];
				continue;
			}

			++literals[257 + lengthSymbol(token.length)];
			++distances[distanceSymbol(token.distance)];
		}

		literals[kEndOfBlock] = 1;
	}

	static BlockCodes fixedCodes() {
		BlockCodes codes;
		codes.literal_lengths.assign(288, 8);
		for (int k = 144; k < 256; ++k) codes.literal_lengths[k] = 9;
		for (int k = 256; k < 280; ++k) codes.literal_lengths[k] = 7;
		codes.distance_lengths.assign(kDistanceSymbols, 5);
		codes.literal_codes = canonicalCodes(codes.literal_lengths);
		codes.distance_codes = canonicalCodes(codes.distance_lengths);
		return codes;
	}

	// Like zlib, give at least two distance codes a length so the code is
	// complete: some decoders reject the one-code form the RFC allows.
	static std::vector<std::uint32_t> withTwoDistances(std::vector<std::uint32_t> distances) {
		std::size_t used = 0;
		for (const std::uint32_t count : distances) {
			if (count != 0) ++used;
		}

		for (std::size_t k = 0; k < distances.size() && used < 2; ++k) {
			if (distances[k] != 0) continue;
			distances[k] = 1;
			++used;
		}

		return distances;
	}

	static BlockCodes dynamicCodes(const std::vector<std::uint32_t>& literals,
	                               const std::vector<std::uint32_t>& distances) {
		BlockCodes codes;
		codes.literal_lengths = limitedCodeLengths(literals, kMaxCodeLength);
		codes.distance_lengths = limitedCodeLengths(withTwoDistances(distances), kMaxCodeLength);

		codes.literal_codes = canonicalCodes(codes.literal_lengths);
		codes.distance_codes = canonicalCodes(codes.distance_lengths);
		return codes;
	}

	static std::size_t usedCount(const std::vector<std::uint8_t>& lengths, std::size_t minimum) {
		std::size_t used = lengths.size();
		while (used > minimum && lengths[used - 1] == 0) --used;
		return used;
	}

	// Code-length symbols 16 (repeat previous 3-6), 17 (3-10 zeros) and
	// 18 (11-138 zeros), each followed by its extra-bits value.
	static void runLengthEncode(const std::vector<std::uint8_t>& lengths,
	                            std::vector<std::uint8_t>& symbols,
	                            std::vector<std::uint8_t>& extras) {
		for (std::size_t k = 0; k < lengths.size();) {
			const std::uint8_t length = lengths[k];
			std::size_t run = 1;
			while (k + run < lengths.size() && lengths[k + run] == length) ++run;

			if (length == 0 && run >= 3) {
				const std::size_t take = (std::min)(run, static_cast<std::size_t>(138));
				symbols.push_back(take >= 11 ? 18 : 17);
				extras.push_back(static_cast<std::uint8_t>(take >= 11 ? take - 11 : take - 3));
				k += take;
				continue;
			}

			symbols.push_back(length);
			extras.push_back(0);
			++k;
			for (std::size_t left = run - 1; length != 0 && left >= 3;) {
				const std::size_t take = (std::min)(left, static_cast<std::size_t>(6));
				symbols.push_back(16);
				extras.push_back(static_cast<std::uint8_t>(take - 3));
				k += take;
				left -= take;
			}
		}
	}

	static int extraBitsFor(std::uint8_t symbol) {
		if (symbol == 16) return 2;
		if (symbol == 17) return 3;
		if (symbol == 18) return 7;
		return 0;
	}

	static std::size_t tokenBits(const std::vector<Token>& tokens, const BlockCodes& codes) {
		std::size_t bits = codes.literal_lengths[kEndOfBlock];
		for (const Token& token : tokens) {
			if (token.length == 0) {
				bits += codes.literal_lengths[token.literal];
				continue;
			}

			const int length_symbol = lengthSymbol(token.length);
			const int distance_symbol = distanceSymbol(token.distance);
			bits += codes.literal_lengths[257 + length_symbol] + kLengthExtra[length_symbol];
			bits += codes.distance_lengths[distance_symbol] + kDistanceExtra[distance_symbol];
		}

		return bits;
	}

	static DynamicHeader buildDynamicHeader(const BlockCodes& codes) {
		DynamicHeader header;
		header.literal_count = usedCount(codes.literal_lengths, 257);
		header.distance_count = usedCount(codes.distance_lengths, 1);

		std::vector<std::uint8_t> all(codes.literal_lengths.begin(),
			codes.literal_lengths.begin() + static_cast<std::ptrdiff_t>(header.literal_count));
		all.insert(all.end(), codes.distance_lengths.begin(),
			codes.distance_lengths.begin() + static_cast<std::ptrdiff_t>(header.distance_count));
		runLengthEncode(all, header.symbols, header.extras);

		std::vector<std::uint32_t> frequencies(19, 0);
		for (const std::uint8_t symbol : header.symbols) ++frequencies[symbol];
		header.lengths = limitedCodeLengths(frequencies, kMaxCodeLengthCode);
		header.codes = canonicalCodes(header.lengths);

		header.code_length_count = 19;
		while (header.code_length_count > 4
		       && header.lengths[kCodeLengthOrder[header.code_length_count - 1]] == 0) {
			--header.code_length_count;
		}

		return header;
	}

	static std::size_t dynamicHeaderBits(const DynamicHeader& header) {
		std::size_t bits = 5 + 5 + 4 + 3 * header.code_length_count;
		for (const std::uint8_t symbol : header.symbols) {
			bits += header.lengths[symbol] + static_cast<std::size_t>(extraBitsFor(symbol));
		}

		return bits;
	}

	static void writeDynamicHeader(BitWriter& writer, const DynamicHeader& header) {
		writer.put(static_cast<std::uint32_t>(header.literal_count - 257), 5);
		writer.put(static_cast<std::uint32_t>(header.distance_count - 1), 5);
		writer.put(static_cast<std::uint32_t>(header.code_length_count - 4), 4);
		for (std::size_t k = 0; k < header.code_length_count; ++k) {
			writer.put(header.lengths[kCodeLengthOrder[k]], 3);
		}

		for (std::size_t k = 0; k < header.symbols.size(); ++k) {
			const std::uint8_t symbol = header.symbols[k];
			putCode(writer, header.codes[symbol], header.lengths[symbol]);
			const int extra = extraBitsFor(symbol);
			if (extra != 0) writer.put(header.extras[k], extra);
		}
	}

	static void writeTokens(BitWriter& writer, const std::vector<Token>& tokens,
	                        const BlockCodes& codes) {
		for (const Token& token : tokens) {
			if (token.length == 0) {
				const std::uint8_t literal = token.literal;
				putCode(writer, codes.literal_codes[literal], codes.literal_lengths[literal]);
				continue;
			}

			const int length_symbol = lengthSymbol(token.length);
			const int literal_symbol = 257 + length_symbol;
			putCode(writer, codes.literal_codes[literal_symbol],
				codes.literal_lengths[literal_symbol]);
			writer.put(token.length - kLengthBase[length_symbol], kLengthExtra[length_symbol]);

			const int distance_symbol = distanceSymbol(token.distance);
			putCode(writer, codes.distance_codes[distance_symbol],
				codes.distance_lengths[distance_symbol]);
			writer.put(token.distance - kDistanceBase[distance_symbol],
				kDistanceExtra[distance_symbol]);
		}

		putCode(writer, codes.literal_codes[kEndOfBlock], codes.literal_lengths[kEndOfBlock]);
	}

	static void writeStoredBlocks(BitWriter& writer, const std::uint8_t* data, std::size_t size,
	                              bool final_block) {
		std::size_t offset = 0;
		do {
			const std::size_t take = (std::min)(kStoredMax, size - offset);
			const bool last = final_block && offset + take == size;
			writer.put(last ? 1 : 0, 1);
			writer.put(0, 2);
			writer.alignToByte();
			writer.put(static_cast<std::uint32_t>(take), 16);
			writer.put(static_cast<std::uint32_t>(~take & 0xFFFF), 16);
			writer.out->insert(writer.out->end(), data + offset, data + offset + take);
			offset += take;
		} while (offset < size);
	}

	static void writeBlock(BitWriter& writer, const std::vector<Token>& tokens,
	                       const std::uint8_t* raw, std::size_t raw_size, bool final_block) {
		std::vector<std::uint32_t> literals;
		std::vector<std::uint32_t> distances;
		countFrequencies(tokens, literals, distances);

		const BlockCodes dynamic = dynamicCodes(literals, distances);
		const DynamicHeader header = buildDynamicHeader(dynamic);
		const BlockCodes fixed = fixedCodes();
		const std::size_t dynamic_bits = dynamicHeaderBits(header) + tokenBits(tokens, dynamic);
		const std::size_t fixed_bits = tokenBits(tokens, fixed);
		const std::size_t stored_bits = (raw_size + 5 * (raw_size / kStoredMax + 1)) * 8;

		if (stored_bits <= dynamic_bits && stored_bits <= fixed_bits) {
			writeStoredBlocks(writer, raw, raw_size, final_block);
			return;
		}

		writer.put(final_block ? 1 : 0, 1);
		if (fixed_bits <= dynamic_bits) {
			writer.put(1, 2);
			writeTokens(writer, tokens, fixed);
			return;
		}

		writer.put(2, 2);
		writeDynamicHeader(writer, header);
		writeTokens(writer, tokens, dynamic);
	}

	// Bytes kept unprocessed until more input arrives: the longest match plus
	// the lazy look at the next position.
	static const std::size_t kLookahead = kMaxMatch + kMinMatch + 1;
	static const std::size_t kSlideAfter = 1024 * 1024;

	// Highly repetitive input yields few tokens per byte; capping a block's
	// input too keeps the encoder's buffer small.
	static const std::size_t kMaxBlockBytes = 1024 * 1024;

	struct DeflateEncoder::State {
		std::vector<std::uint8_t> data;
		std::vector<std::uint8_t> bytes;
		BitWriter writer;
		MatchFinder finder;
		std::vector<Token> tokens;
		std::size_t position = 0;
		std::size_t block_start = 0;
		std::uint64_t total = 0;
		int level = 6;
	};

	DeflateEncoder::DeflateEncoder(ByteSink& out, int level) : out_(out), state_(std::make_unique<State>()) {
		State& state = *state_;
		state.level = level;
		state.writer.out = &state.bytes;
		state.finder.head.assign(static_cast<std::size_t>(1) << kHashBits, -1);
		state.finder.previous.assign(kWindowSize, -1);
		state.finder.tuning = tuningFor(level);
	}

	DeflateEncoder::~DeflateEncoder() = default;

	bool DeflateEncoder::drain() {
		std::vector<std::uint8_t>& bytes = state_->bytes;
		const bool ok = bytes.empty() || out_.write(bytes.data(), bytes.size());
		bytes.clear();
		return ok;
	}

	static int shiftedPosition(int position, int shift) {
		return position >= shift ? position - shift : -1;
	}

	// Drops input more than a window behind, in whole windows so that the
	// previous-position table, indexed modulo the window, stays valid.
	static void slideWindow(DeflateEncoder::State& state) {
		if (state.block_start < kSlideAfter + 2 * static_cast<std::size_t>(kWindowSize)) return;

		const std::size_t keep_from = state.block_start - kWindowSize;
		const std::size_t shift = keep_from - keep_from % kWindowSize;
		state.data.erase(state.data.begin(), state.data.begin() + static_cast<std::ptrdiff_t>(shift));
		state.position -= shift;
		state.block_start -= shift;

		const int amount = static_cast<int>(shift);
		for (int& entry : state.finder.head) entry = shiftedPosition(entry, amount);
		for (int& entry : state.finder.previous) entry = shiftedPosition(entry, amount);
	}

	bool DeflateEncoder::emitBlock(bool final_block) {
		State& state = *state_;
		writeBlock(state.writer, state.tokens, state.data.data() + state.block_start,
			state.position - state.block_start, final_block);
		state.tokens.clear();
		state.block_start = state.position;
		if (!drain()) return false;

		slideWindow(state);
		return true;
	}

	bool DeflateEncoder::emitStored(bool final_block) {
		State& state = *state_;
		const std::size_t size = state.data.size();
		if (!final_block && size < kStoredMax) return true;

		const std::size_t take = final_block ? size : size - size % kStoredMax;
		writeStoredBlocks(state.writer, state.data.data(), take, final_block);
		state.data.erase(state.data.begin(), state.data.begin() + static_cast<std::ptrdiff_t>(take));
		return drain();
	}

	static std::size_t tokenizeLimit(const DeflateEncoder::State& state, bool final_block) {
		const std::size_t size = state.data.size();
		if (final_block) return size;
		return size > kLookahead ? size - kLookahead : 0;
	}

	bool DeflateEncoder::tokenize(bool final_block) {
		State& state = *state_;
		state.finder.data = state.data.data();
		state.finder.size = state.data.size();

		while (state.position < tokenizeLimit(state, final_block)) {
			state.position = tokenizeStep(state.finder, state.position, state.tokens);
			const bool full = state.tokens.size() >= kBlockTokens
				|| state.position - state.block_start >= kMaxBlockBytes;
			if (!full || state.position >= state.data.size()) continue;
			if (!emitBlock(false)) return false;

			state.finder.data = state.data.data();
			state.finder.size = state.data.size();
		}

		return true;
	}

	bool DeflateEncoder::write(const std::uint8_t* data, std::size_t size) {
		State& state = *state_;
		state.data.insert(state.data.end(), data, data + size);
		state.total += size;
		if (state.level <= 0) return emitStored(false);
		return tokenize(false);
	}

	bool DeflateEncoder::finish() {
		State& state = *state_;
		if (state.level <= 0 || state.total == 0) {
			if (!emitStored(true)) return false;
		} else if (!tokenize(true) || !emitBlock(true)) {
			return false;
		}

		state.writer.alignToByte();
		return drain();
	}

	void deflateRaw(const std::uint8_t* data, std::size_t size, int level,
	                std::vector<std::uint8_t>& out) {
		VectorSink sink(out);
		DeflateEncoder encoder(sink, level);
		encoder.write(data, size);
		encoder.finish();
	}

}  // namespace wbsh

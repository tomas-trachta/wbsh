/**
 * @file lzma_encoder.cpp
 * @brief LZMA symbol coding and its two framings. The range coder and the
 *        adaptive models mirror the decoder in xz.cpp; LZMA2 chunking
 *        follows the .xz file format 1.0.4. The match finder and the
 *        parsers that pick the symbols live beside this file.
 */

#include "lzma_encoder.h"

#include <algorithm>
#include <vector>

#include "lzma_encoder_internal.h"

namespace wbsh {

	namespace lzma_encoder_detail {

		struct RangeEncoder {
			std::vector<std::uint8_t> bytes;
			std::uint64_t low = 0;
			std::uint32_t range = 0xFFFFFFFFu;
			std::uint8_t cache = 0;
			std::uint64_t cache_size = 1;
		};

	}  // namespace lzma_encoder_detail

	using lzma_encoder_detail::Choice;
	using lzma_encoder_detail::Core;
	using lzma_encoder_detail::LengthModel;
	using lzma_encoder_detail::Model;
	using lzma_encoder_detail::Probability;
	using lzma_encoder_detail::RangeEncoder;
	using lzma_encoder_detail::kAlignSize;
	using lzma_encoder_detail::kDistanceSlots;
	using lzma_encoder_detail::kDistanceSpecials;
	using lzma_encoder_detail::kDistanceStates;
	using lzma_encoder_detail::kLiteral;
	using lzma_encoder_detail::kLiteralContextBits;
	using lzma_encoder_detail::kLiteralStates;
	using lzma_encoder_detail::kMatchMaxLength;
	using lzma_encoder_detail::kMatchMinLength;
	using lzma_encoder_detail::kOptimumSize;
	using lzma_encoder_detail::kPositionMask;
	using lzma_encoder_detail::kPositionStates;
	using lzma_encoder_detail::kReps;
	using lzma_encoder_detail::kStates;

	static const Probability kProbabilityStart = 1024;
	static const std::uint32_t kEndMarker = 0xFFFFFFFFu;
	static const std::uint32_t kTopValue = 1u << 24;
	static const unsigned kDistanceModelEnd = 14;
	static const std::uint8_t kPropertiesByte = (2 * 5 + 0) * 9 + 3;
	static const std::size_t kLookahead = kOptimumSize + kMatchMaxLength + 2;
	static const std::size_t kFeedSlice = 1 << 20;
	static const std::size_t kChunkPackedMax = 1 << 16;
	static const std::size_t kChunkUnpackedMax = 1 << 21;
	static const std::size_t kSymbolMaxBytes = 64;
	static const std::size_t kStoredChunkMax = 1 << 16;

	static void shiftLow(RangeEncoder& rc) {
		const bool carried = (rc.low >> 32) != 0;
		if (carried || static_cast<std::uint32_t>(rc.low) < 0xFF000000u) {
			const auto carry = static_cast<std::uint8_t>(rc.low >> 32);
			rc.bytes.push_back(static_cast<std::uint8_t>(rc.cache + carry));
			const auto run = static_cast<std::size_t>(rc.cache_size - 1);
			rc.bytes.insert(rc.bytes.end(), run, static_cast<std::uint8_t>(0xFF + carry));
			rc.cache_size = 0;
			rc.cache = static_cast<std::uint8_t>(rc.low >> 24);
		}

		++rc.cache_size;
		rc.low = (rc.low & 0x00FFFFFFu) << 8;
	}

	static void normalize(RangeEncoder& rc) {
		if (rc.range >= kTopValue) return;
		rc.range <<= 8;
		shiftLow(rc);
	}

	static void encodeBit(RangeEncoder& rc, Probability& probability, unsigned bit) {
		const std::uint32_t bound = (rc.range >> 11) * probability;
		if (bit == 0) {
			rc.range = bound;
			probability = static_cast<Probability>(probability + ((2048u - probability) >> 5));
		} else {
			rc.low += bound;
			rc.range -= bound;
			probability = static_cast<Probability>(probability - (probability >> 5));
		}

		normalize(rc);
	}

	static void encodeDirectBits(RangeEncoder& rc, std::uint32_t value, int count) {
		for (int k = count - 1; k >= 0; --k) {
			rc.range >>= 1;
			if (((value >> k) & 1) != 0) rc.low += rc.range;
			normalize(rc);
		}
	}

	static void flushRange(RangeEncoder& rc) {
		for (int k = 0; k < 5; ++k) shiftLow(rc);
	}

	static void resetRange(RangeEncoder& rc) {
		rc.bytes.clear();
		rc.low = 0;
		rc.range = 0xFFFFFFFFu;
		rc.cache = 0;
		rc.cache_size = 1;
	}

	// What flushRange would leave in bytes: the cached run plus four more.
	static std::size_t pendingBytes(const RangeEncoder& rc) {
		return rc.bytes.size() + static_cast<std::size_t>(rc.cache_size) + 4;
	}

	static void encodeBitTree(RangeEncoder& rc, Probability* probabilities, int bits,
	                          std::uint32_t value) {
		std::uint32_t node = 1;
		for (int k = bits - 1; k >= 0; --k) {
			const unsigned bit = (value >> k) & 1;
			encodeBit(rc, probabilities[node], bit);
			node = (node << 1) | bit;
		}
	}

	static void encodeReverseBitTree(RangeEncoder& rc, Probability* probabilities, int bits,
	                                 std::uint32_t value) {
		std::uint32_t node = 1;
		for (int k = 0; k < bits; ++k) {
			const unsigned bit = (value >> k) & 1;
			encodeBit(rc, probabilities[node], bit);
			node = (node << 1) | bit;
		}
	}

	static void fillProbabilities(Probability* first, std::size_t count) {
		std::fill(first, first + count, kProbabilityStart);
	}

	static void resetLengthModel(LengthModel& model) {
		model.choice = kProbabilityStart;
		model.choice2 = kProbabilityStart;
		fillProbabilities(&model.low[0][0], kPositionStates * 8);
		fillProbabilities(&model.mid[0][0], kPositionStates * 8);
		fillProbabilities(model.high, 256);
	}

	static void resetModel(Model& model) {
		const std::size_t literal_coders = static_cast<std::size_t>(1) << kLiteralContextBits;
		model.literals.assign(0x300 * literal_coders, kProbabilityStart);
		fillProbabilities(&model.is_match[0][0], kStates * kPositionStates);
		fillProbabilities(model.is_rep, kStates);
		fillProbabilities(model.is_rep_g0, kStates);
		fillProbabilities(model.is_rep_g1, kStates);
		fillProbabilities(model.is_rep_g2, kStates);
		fillProbabilities(&model.is_rep0_long[0][0], kStates * kPositionStates);
		fillProbabilities(&model.distance_slot[0][0], kDistanceStates * kDistanceSlots);
		fillProbabilities(model.distance_special, kDistanceSpecials);
		fillProbabilities(model.align, kAlignSize);
		resetLengthModel(model.length);
		resetLengthModel(model.rep_length);
		model.state = 0;
		std::fill(model.reps, model.reps + kReps, 0u);
		lzma_encoder_detail::resetPrices(model);
	}

	// The decoder starts every reset from fresh models and an empty path.
	static void resetCoder(Core& core) {
		resetModel(core.model);
		lzma_encoder_detail::dropPendingChoices(core);
	}

	static void encodeMatchedLiteral(RangeEncoder& rc, Probability* probabilities, unsigned byte,
	                                 unsigned match_byte) {
		unsigned node = 1;
		bool matching = true;
		for (int k = 7; k >= 0; --k) {
			const unsigned bit = (byte >> k) & 1;
			const unsigned match_bit = (match_byte >> k) & 1;
			const unsigned index = matching ? ((1 + match_bit) << 8) + node : node;
			encodeBit(rc, probabilities[index], bit);
			matching = matching && match_bit == bit;
			node = (node << 1) | bit;
		}
	}

	static void encodeLiteral(RangeEncoder& rc, Core& core, unsigned pos_state) {
		Model& model = core.model;
		const std::uint8_t* here = core.finder.window.data() + core.cursor;
		const unsigned previous = core.position > 0 ? here[-1] : 0;
		const unsigned context = previous >> (8 - kLiteralContextBits);
		Probability* probabilities = model.literals.data() + 0x300 * context;

		encodeBit(rc, model.is_match[model.state][pos_state], 0);
		if (model.state < kLiteralStates) {
			encodeBitTree(rc, probabilities, 8, here[0]);
		} else {
			const std::uint8_t match_byte = *(here - 1 - model.reps[0]);
			encodeMatchedLiteral(rc, probabilities, here[0], match_byte);
		}

		model.state = lzma_encoder_detail::literalNextState(model.state);
	}

	static void encodeLength(RangeEncoder& rc, LengthModel& model, std::uint32_t length,
	                         unsigned pos_state) {
		const std::uint32_t value = length - kMatchMinLength;
		if (value < 8) {
			encodeBit(rc, model.choice, 0);
			encodeBitTree(rc, model.low[pos_state], 3, value);
		} else if (value < 16) {
			encodeBit(rc, model.choice, 1);
			encodeBit(rc, model.choice2, 0);
			encodeBitTree(rc, model.mid[pos_state], 3, value - 8);
		} else {
			encodeBit(rc, model.choice, 1);
			encodeBit(rc, model.choice2, 1);
			encodeBitTree(rc, model.high, 8, value - 16);
		}

		--model.uses_left[pos_state];
		if (model.uses_left[pos_state] != 0) return;
		lzma_encoder_detail::updateLengthPrices(model, pos_state);
	}

	static void encodeDistance(RangeEncoder& rc, Model& model, std::uint32_t distance,
	                           std::uint32_t length) {
		const unsigned length_state = (std::min)(length - kMatchMinLength, 3u);
		const unsigned slot = lzma_encoder_detail::distanceSlot(distance);
		encodeBitTree(rc, model.distance_slot[length_state], 6, slot);
		if (slot < 4) return;

		const int footer_bits = static_cast<int>(slot >> 1) - 1;
		const std::uint32_t base = (2u | (slot & 1u)) << footer_bits;
		const std::uint32_t reduced = distance - base;
		if (slot < kDistanceModelEnd) {
			encodeReverseBitTree(rc, model.distance_special + base - slot, footer_bits, reduced);
			return;
		}

		encodeDirectBits(rc, reduced >> 4, footer_bits - 4);
		encodeReverseBitTree(rc, model.align, 4, reduced & (kAlignSize - 1));
		++model.distance_prices.aligned_since_fill;
	}

	static void encodeMatch(RangeEncoder& rc, Model& model, std::uint32_t distance,
	                        std::uint32_t length, unsigned pos_state) {
		encodeBit(rc, model.is_match[model.state][pos_state], 1);
		encodeBit(rc, model.is_rep[model.state], 0);
		encodeLength(rc, model.length, length, pos_state);
		encodeDistance(rc, model, distance, length);
		++model.distance_prices.matches_since_fill;

		std::copy_backward(model.reps, model.reps + kReps - 1, model.reps + kReps);
		model.reps[0] = distance;
		model.state = lzma_encoder_detail::matchNextState(model.state);
	}

	static void encodeRepIndex(RangeEncoder& rc, Model& model, unsigned index, std::uint32_t length,
	                           unsigned pos_state) {
		if (index == 0) {
			encodeBit(rc, model.is_rep_g0[model.state], 0);
			encodeBit(rc, model.is_rep0_long[model.state][pos_state], length == 1 ? 0 : 1);
			return;
		}

		encodeBit(rc, model.is_rep_g0[model.state], 1);
		if (index == 1) {
			encodeBit(rc, model.is_rep_g1[model.state], 0);
		} else {
			encodeBit(rc, model.is_rep_g1[model.state], 1);
			encodeBit(rc, model.is_rep_g2[model.state], index - 2);
		}

		const std::uint32_t distance = model.reps[index];
		std::copy_backward(model.reps, model.reps + index, model.reps + index + 1);
		model.reps[0] = distance;
	}

	static void encodeRep(RangeEncoder& rc, Model& model, unsigned index, std::uint32_t length,
	                      unsigned pos_state) {
		encodeBit(rc, model.is_match[model.state][pos_state], 1);
		encodeBit(rc, model.is_rep[model.state], 1);
		encodeRepIndex(rc, model, index, length, pos_state);
		if (length == 1) {
			model.state = lzma_encoder_detail::shortRepNextState(model.state);
			return;
		}

		encodeLength(rc, model.rep_length, length, pos_state);
		model.state = lzma_encoder_detail::repNextState(model.state);
	}

	static void encodeChoice(RangeEncoder& rc, Core& core, const Choice& choice) {
		const unsigned pos_state = static_cast<unsigned>(core.position) & kPositionMask;
		if (choice.back == kLiteral) {
			encodeLiteral(rc, core, pos_state);
		} else if (choice.back < kReps) {
			encodeRep(rc, core.model, choice.back, choice.length, pos_state);
		} else {
			encodeMatch(rc, core.model, choice.back - kReps, choice.length, pos_state);
		}

		core.cursor += choice.length;
		core.position += choice.length;
	}

	static void encodeNextSymbol(RangeEncoder& rc, Core& core) {
		encodeChoice(rc, core, lzma_encoder_detail::chooseSymbol(core));
	}

	static std::size_t encodeLimit(const Core& core, bool final_input) {
		const std::size_t size = core.finder.window.size();
		if (final_input) return size;
		return size > kLookahead ? size - kLookahead : 0;
	}

	static void setUpCore(Core& core, const LzmaPreset& preset) {
		lzma_encoder_detail::setUpFinder(core.finder, preset);
		core.optimal = preset.optimal;
		if (core.optimal) core.opts.resize(kOptimumSize);
		resetCoder(core);
	}

	static void appendInput(Core& core, const std::uint8_t* data, std::size_t size) {
		core.finder.window.insert(core.finder.window.end(), data, data + size);
	}

	static std::uint32_t lzma2DictionarySize(std::uint8_t bits) {
		return (2u | (bits & 1u)) << (bits / 2 + 11);
	}

	std::uint8_t lzma2DictionaryByte(std::uint32_t dictionary) {
		std::uint8_t bits = 0;
		while (bits < 40 && lzma2DictionarySize(bits) < dictionary) ++bits;
		return bits;
	}

	// xz's presets: levels 0-3 use the fast parser, the rest the optimal
	// one with a growing nice length; the tree depth follows xz's auto rule.
	LzmaPreset lzmaPreset(int level, bool extreme) {
		static const std::uint8_t kDictionaryBits[10] = { 18, 20, 21, 22, 22, 23, 23, 24, 25, 26 };
		static const std::uint16_t kFastDepths[4] = { 4, 8, 24, 48 };
		static const std::uint16_t kNiceLengths[10] = {
			128, 128, 273, 273, 16, 32, 64, 64, 64, 64,
		};
		level = (std::min)((std::max)(level, 0), 9);

		LzmaPreset preset;
		preset.dictionary = 1u << kDictionaryBits[level];
		preset.optimal = level >= 4 || extreme;
		preset.nice_length = kNiceLengths[level];
		preset.depth = level < 4 ? kFastDepths[level] : 16 + preset.nice_length / 2;
		if (!extreme) return preset;

		const bool middling = level == 3 || level == 5;
		preset.nice_length = middling ? 192 : kMatchMaxLength;
		preset.depth = middling ? 16 + preset.nice_length / 2 : 512;
		return preset;
	}

	struct Lzma2Encoder::State {
		Core core;
		RangeEncoder rc;
		std::size_t chunk_start = 0;
		bool chunk_open = false;
		bool need_dictionary_reset = true;
		bool need_properties = true;
		bool need_state_reset = false;
	};

	Lzma2Encoder::Lzma2Encoder(ByteSink& out, const LzmaPreset& preset)
		: out_(out), state_(std::make_unique<State>()) {
		setUpCore(state_->core, preset);
	}

	Lzma2Encoder::~Lzma2Encoder() = default;

	static void openChunk(Lzma2Encoder::State& state) {
		state.chunk_start = state.core.cursor;
		state.chunk_open = true;
		resetRange(state.rc);
	}

	static bool chunkFull(const Lzma2Encoder::State& state) {
		return state.core.cursor - state.chunk_start >= kChunkUnpackedMax - kMatchMaxLength
			|| pendingBytes(state.rc) >= kChunkPackedMax - kSymbolMaxBytes;
	}

	// LZMA chunk control: 0x80 plus the reset it carries in bits 5-6 (none,
	// state, state and new properties, everything including the dictionary)
	// plus the top bits of the unpacked size.
	static std::uint8_t lzmaChunkControl(const Lzma2Encoder::State& state) {
		if (state.need_dictionary_reset) return 0xE0;
		if (state.need_properties) return 0xC0;
		if (state.need_state_reset) return 0xA0;
		return 0x80;
	}

	bool Lzma2Encoder::writeLzmaChunk() {
		State& state = *state_;
		const std::vector<std::uint8_t>& packed = state.rc.bytes;
		const std::size_t unpacked_less_one = state.core.cursor - state.chunk_start - 1;
		const std::size_t packed_less_one = packed.size() - 1;

		const std::uint8_t header[6] = {
			static_cast<std::uint8_t>(lzmaChunkControl(state) | (unpacked_less_one >> 16)),
			static_cast<std::uint8_t>(unpacked_less_one >> 8),
			static_cast<std::uint8_t>(unpacked_less_one),
			static_cast<std::uint8_t>(packed_less_one >> 8),
			static_cast<std::uint8_t>(packed_less_one),
			kPropertiesByte,
		};
		const std::size_t header_size = state.need_properties ? 6 : 5;

		state.need_dictionary_reset = false;
		state.need_properties = false;
		state.need_state_reset = false;
		return out_.write(header, header_size) && out_.write(packed.data(), packed.size());
	}

	// Stored chunk control: 1 resets the dictionary, 2 does not. The decoder
	// never sees the symbols behind it, so the next LZMA chunk resets the
	// state and both sides start from fresh models.
	bool Lzma2Encoder::writeStoredChunks() {
		State& state = *state_;
		const std::uint8_t* data = state.core.finder.window.data() + state.chunk_start;
		const std::size_t size = state.core.cursor - state.chunk_start;
		for (std::size_t offset = 0; offset < size; offset += kStoredChunkMax) {
			const std::size_t take_less_one = (std::min)(kStoredChunkMax, size - offset) - 1;
			const std::uint8_t header[3] = {
				static_cast<std::uint8_t>(state.need_dictionary_reset ? 0x01 : 0x02),
				static_cast<std::uint8_t>(take_less_one >> 8),
				static_cast<std::uint8_t>(take_less_one),
			};
			state.need_dictionary_reset = false;
			if (!out_.write(header, sizeof(header))) return false;
			if (!out_.write(data + offset, take_less_one + 1)) return false;
		}

		resetCoder(state.core);
		state.need_state_reset = true;
		return true;
	}

	bool Lzma2Encoder::closeChunk() {
		State& state = *state_;
		flushRange(state.rc);
		state.chunk_open = false;

		const bool shrank = state.rc.bytes.size() < state.core.cursor - state.chunk_start;
		return shrank ? writeLzmaChunk() : writeStoredChunks();
	}

	bool Lzma2Encoder::encodeAvailable(bool final_input) {
		State& state = *state_;
		const std::size_t end = encodeLimit(state.core, final_input);
		while (state.core.cursor < end) {
			if (!state.chunk_open) openChunk(state);
			encodeNextSymbol(state.rc, state.core);
			if (chunkFull(state) && !closeChunk()) return false;
		}

		return true;
	}

	bool Lzma2Encoder::write(const std::uint8_t* data, std::size_t size) {
		State& state = *state_;
		for (std::size_t offset = 0; offset < size; offset += kFeedSlice) {
			appendInput(state.core, data + offset, (std::min)(kFeedSlice, size - offset));
			if (!encodeAvailable(false)) return false;

			const std::size_t keep_from = state.chunk_open ? state.chunk_start : state.core.cursor;
			const std::size_t shift = lzma_encoder_detail::slideWindow(state.core, keep_from);
			if (state.chunk_open) state.chunk_start -= shift;
		}

		return true;
	}

	bool Lzma2Encoder::finish() {
		if (!encodeAvailable(true)) return false;
		if (state_->chunk_open && !closeChunk()) return false;

		const std::uint8_t end_of_stream = 0x00;
		return out_.write(&end_of_stream, 1);
	}

	struct LzmaAloneEncoder::State {
		Core core;
		RangeEncoder rc;
	};

	// .lzma header: properties byte, dictionary size, then the uncompressed
	// size, all ones for "unknown, ends with the marker".
	static void appendAloneHeader(std::vector<std::uint8_t>& bytes, std::uint32_t dictionary) {
		bytes.push_back(kPropertiesByte);
		for (int k = 0; k < 4; ++k) {
			bytes.push_back(static_cast<std::uint8_t>(dictionary >> (8 * k)));
		}

		bytes.insert(bytes.end(), 8, 0xFF);
	}

	LzmaAloneEncoder::LzmaAloneEncoder(ByteSink& out, const LzmaPreset& preset)
		: out_(out), state_(std::make_unique<State>()) {
		setUpCore(state_->core, preset);
		appendAloneHeader(state_->rc.bytes, preset.dictionary);
	}

	LzmaAloneEncoder::~LzmaAloneEncoder() = default;

	bool LzmaAloneEncoder::drain() {
		std::vector<std::uint8_t>& bytes = state_->rc.bytes;
		const bool ok = bytes.empty() || out_.write(bytes.data(), bytes.size());
		bytes.clear();
		return ok;
	}

	bool LzmaAloneEncoder::write(const std::uint8_t* data, std::size_t size) {
		State& state = *state_;
		for (std::size_t offset = 0; offset < size; offset += kFeedSlice) {
			appendInput(state.core, data + offset, (std::min)(kFeedSlice, size - offset));
			const std::size_t end = encodeLimit(state.core, false);
			while (state.core.cursor < end) encodeNextSymbol(state.rc, state.core);
			if (!drain()) return false;

			lzma_encoder_detail::slideWindow(state.core, state.core.cursor);
		}

		return true;
	}

	bool LzmaAloneEncoder::finish() {
		State& state = *state_;
		const std::size_t end = encodeLimit(state.core, true);
		while (state.core.cursor < end) encodeNextSymbol(state.rc, state.core);

		const unsigned pos_state = static_cast<unsigned>(state.core.position) & kPositionMask;
		encodeMatch(state.rc, state.core.model, kEndMarker, kMatchMinLength, pos_state);
		flushRange(state.rc);
		return drain();
	}

}  // namespace wbsh

/**
 * @file lzma_prices.cpp
 * @brief What each LZMA symbol would cost under the current models, in
 *        sixteenths of a bit, for the optimal parser. Length and distance
 *        prices are cached in tables refreshed as the models drift.
 */

#include "lzma_encoder_internal.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace wbsh::lzma_encoder_detail {

	static const unsigned kProbabilityBits = 11;
	static const unsigned kPriceTableShift = 4;
	static const std::uint32_t kPriceShift = 4;
	static const std::uint32_t kDistanceRefreshMatches = 128;
	static const unsigned kDistanceModelEnd = 14;
	static const int kAlignBits = 4;

	// Price of a bit coded with probability p (out of 2048) is -log2(p / 2048)
	// in sixteenths, tabulated per 16 steps of p.
	static std::array<std::uint32_t, 128> buildBitPrices() {
		std::array<std::uint32_t, 128> prices{};
		for (std::size_t k = 0; k < prices.size(); ++k) {
			const double probability = (static_cast<double>(k << kPriceTableShift) + 8.0) / 2048.0;
			const double bits = -std::log2(probability);
			prices[k] = static_cast<std::uint32_t>(std::lround(bits * (1 << kPriceShift)));
		}

		return prices;
	}

	static const std::array<std::uint32_t, 128> kBitPrices = buildBitPrices();

	std::uint32_t bitPrice(Probability probability, unsigned bit) {
		const unsigned mask = bit == 0 ? 0u : (1u << kProbabilityBits) - 1;
		return kBitPrices[(probability ^ mask) >> kPriceTableShift];
	}

	static std::uint32_t bitTreePrice(const Probability* probabilities, int bits,
	                                  std::uint32_t value) {
		std::uint32_t price = 0;
		std::uint32_t node = 1;
		for (int k = bits - 1; k >= 0; --k) {
			const unsigned bit = (value >> k) & 1;
			price += bitPrice(probabilities[node], bit);
			node = (node << 1) | bit;
		}

		return price;
	}

	static std::uint32_t reverseBitTreePrice(const Probability* probabilities, int bits,
	                                         std::uint32_t value) {
		std::uint32_t price = 0;
		std::uint32_t node = 1;
		for (int k = 0; k < bits; ++k) {
			const unsigned bit = (value >> k) & 1;
			price += bitPrice(probabilities[node], bit);
			node = (node << 1) | bit;
		}

		return price;
	}

	static std::uint32_t matchedLiteralPrice(const Probability* probabilities, unsigned match_byte,
	                                         unsigned byte) {
		std::uint32_t price = 0;
		unsigned node = 1;
		bool matching = true;
		for (int k = 7; k >= 0; --k) {
			const unsigned bit = (byte >> k) & 1;
			const unsigned match_bit = (match_byte >> k) & 1;
			const unsigned index = matching ? ((1 + match_bit) << 8) + node : node;
			price += bitPrice(probabilities[index], bit);
			matching = matching && match_bit == bit;
			node = (node << 1) | bit;
		}

		return price;
	}

	std::uint32_t literalPrice(const Model& model, unsigned previous, unsigned match_byte,
	                           unsigned byte, bool matched) {
		const unsigned context = previous >> (8 - kLiteralContextBits);
		const Probability* probabilities = model.literals.data() + 0x300 * context;
		if (!matched) return bitTreePrice(probabilities, 8, byte);
		return matchedLiteralPrice(probabilities, match_byte, byte);
	}

	std::uint32_t shortRepPrice(const Model& model, unsigned state, unsigned pos_state) {
		const std::uint32_t price = bitPrice(model.is_rep_g0[state], 0);
		return price + bitPrice(model.is_rep0_long[state][pos_state], 0);
	}

	std::uint32_t pureRepPrice(const Model& model, unsigned index, unsigned state,
	                           unsigned pos_state) {
		if (index == 0) {
			const std::uint32_t price = bitPrice(model.is_rep_g0[state], 0);
			return price + bitPrice(model.is_rep0_long[state][pos_state], 1);
		}

		const std::uint32_t price = bitPrice(model.is_rep_g0[state], 1);
		if (index == 1) return price + bitPrice(model.is_rep_g1[state], 0);

		const std::uint32_t g2_price = bitPrice(model.is_rep_g2[state], index - 2);
		return price + bitPrice(model.is_rep_g1[state], 1) + g2_price;
	}

	std::uint32_t matchPrice(const Model& model, std::uint32_t distance, std::uint32_t length,
	                         unsigned pos_state) {
		const DistancePrices& prices = model.distance_prices;
		const std::uint32_t distance_state = (std::min)(length - kMatchMinLength, 3u);
		const std::uint32_t length_price = model.length.prices[pos_state][length - kMatchMinLength];
		if (distance < kFullDistances) return length_price + prices.full[distance_state][distance];

		const std::uint32_t align_price = prices.align[distance & (kAlignSize - 1)];
		return length_price + prices.slot[distance_state][distanceSlot(distance)] + align_price;
	}

	// Lengths are coded as choice bits then a 3-bit low or mid tree per
	// position state, or the shared 8-bit high tree.
	void updateLengthPrices(LengthModel& model, unsigned pos_state) {
		const std::uint32_t low_base = bitPrice(model.choice, 0);
		const std::uint32_t mid_base = bitPrice(model.choice, 1) + bitPrice(model.choice2, 0);
		const std::uint32_t high_base = bitPrice(model.choice, 1) + bitPrice(model.choice2, 1);
		std::uint32_t* prices = model.prices[pos_state];
		for (std::uint32_t value = 0; value < kLengthSymbols; ++value) {
			if (value < 8) {
				prices[value] = low_base + bitTreePrice(model.low[pos_state], 3, value);
			} else if (value < 16) {
				prices[value] = mid_base + bitTreePrice(model.mid[pos_state], 3, value - 8);
			} else {
				prices[value] = high_base + bitTreePrice(model.high, 8, value - 16);
			}
		}

		model.uses_left[pos_state] = kLengthSymbols;
	}

	// Slots from 14 up carry direct bits above the four align bits; those
	// cost a bit apiece and go into the slot price.
	static void fillSlotPrices(Model& model) {
		DistancePrices& prices = model.distance_prices;
		for (unsigned state = 0; state < kDistanceStates; ++state) {
			for (unsigned slot = 0; slot < kDistanceSlots; ++slot) {
				prices.slot[state][slot] = bitTreePrice(model.distance_slot[state], 6, slot);
				if (slot < kDistanceModelEnd) continue;
				prices.slot[state][slot] += (((slot >> 1) - 1) - kAlignBits) << kPriceShift;
			}

			for (unsigned distance = 0; distance < 4; ++distance) {
				prices.full[state][distance] = prices.slot[state][distance];
			}
		}
	}

	static void fillFullDistancePrices(Model& model) {
		DistancePrices& prices = model.distance_prices;
		for (std::uint32_t distance = 4; distance < kFullDistances; ++distance) {
			const unsigned slot = distanceSlot(distance);
			const int footer_bits = static_cast<int>(slot >> 1) - 1;
			const std::uint32_t base = (2u | (slot & 1u)) << footer_bits;
			const Probability* special = model.distance_special + base - slot;
			const std::uint32_t price = reverseBitTreePrice(special, footer_bits, distance - base);
			for (unsigned state = 0; state < kDistanceStates; ++state) {
				prices.full[state][distance] = price + prices.slot[state][slot];
			}
		}
	}

	static void fillAlignPrices(Model& model) {
		DistancePrices& prices = model.distance_prices;
		for (std::uint32_t value = 0; value < kAlignSize; ++value) {
			prices.align[value] = reverseBitTreePrice(model.align, kAlignBits, value);
		}

		prices.aligned_since_fill = 0;
	}

	void refreshDistancePrices(Model& model) {
		DistancePrices& prices = model.distance_prices;
		if (prices.matches_since_fill >= kDistanceRefreshMatches) {
			fillSlotPrices(model);
			fillFullDistancePrices(model);
			prices.matches_since_fill = 0;
		}

		if (prices.aligned_since_fill >= kAlignSize) fillAlignPrices(model);
	}

	void resetPrices(Model& model) {
		for (unsigned pos_state = 0; pos_state < kPositionStates; ++pos_state) {
			updateLengthPrices(model.length, pos_state);
			updateLengthPrices(model.rep_length, pos_state);
		}

		model.distance_prices.matches_since_fill = kDistanceRefreshMatches;
		model.distance_prices.aligned_since_fill = kAlignSize;
		refreshDistancePrices(model);
	}

}  // namespace wbsh::lzma_encoder_detail

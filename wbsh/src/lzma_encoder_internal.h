#pragma once

/**
 * @file lzma_encoder_internal.h
 * @brief State shared by the LZMA encoder's parts: the adaptive models with
 *        the prices derived from them, the binary-tree match finder, and the
 *        parsers that pick the next symbol.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "lzma_encoder.h"

namespace wbsh::lzma_encoder_detail {

	typedef std::uint16_t Probability;

	inline constexpr unsigned kStates = 12;
	inline constexpr unsigned kLiteralStates = 7;
	inline constexpr unsigned kPositionStates = 4;
	inline constexpr unsigned kPositionMask = kPositionStates - 1;
	inline constexpr unsigned kLiteralContextBits = 3;
	inline constexpr unsigned kReps = 4;
	inline constexpr unsigned kDistanceStates = 4;
	inline constexpr unsigned kDistanceSlots = 64;
	inline constexpr unsigned kFullDistances = 128;
	inline constexpr unsigned kDistanceSpecials = 115;
	inline constexpr unsigned kAlignSize = 16;
	inline constexpr std::uint32_t kMatchMinLength = 2;
	inline constexpr std::uint32_t kMatchMaxLength = 273;
	inline constexpr std::uint32_t kLengthSymbols = kMatchMaxLength - kMatchMinLength + 1;
	inline constexpr std::uint32_t kOptimumSize = 1 << 12;

	/// Choice::back for a literal; rep indexes sit below kReps.
	inline constexpr std::uint32_t kLiteral = 0xFFFFFFFFu;

	struct LengthModel {
		Probability choice;
		Probability choice2;
		Probability low[kPositionStates][8];
		Probability mid[kPositionStates][8];
		Probability high[256];
		std::uint32_t prices[kPositionStates][kLengthSymbols];
		std::uint32_t uses_left[kPositionStates];
	};

	struct DistancePrices {
		std::uint32_t slot[kDistanceStates][kDistanceSlots];
		std::uint32_t full[kDistanceStates][kFullDistances];
		std::uint32_t align[kAlignSize];
		std::uint32_t matches_since_fill;
		std::uint32_t aligned_since_fill;
	};

	struct Model {
		std::vector<Probability> literals;
		Probability is_match[kStates][kPositionStates];
		Probability is_rep[kStates];
		Probability is_rep_g0[kStates];
		Probability is_rep_g1[kStates];
		Probability is_rep_g2[kStates];
		Probability is_rep0_long[kStates][kPositionStates];
		Probability distance_slot[kDistanceStates][kDistanceSlots];
		Probability distance_special[kDistanceSpecials];
		Probability align[kAlignSize];
		LengthModel length;
		LengthModel rep_length;
		DistancePrices distance_prices;
		unsigned state = 0;
		std::uint32_t reps[kReps] = { 0, 0, 0, 0 };
	};

	struct Match {
		std::uint32_t length;
		std::uint32_t distance;
	};

	/// Matches in increasing length; distances are zero-based.
	struct Matches {
		std::array<Match, kMatchMaxLength> list;
		std::size_t count = 0;
		std::uint32_t longest = 0;
	};

	/// Positions in the tables are window indexes plus cyclic_size, so 0
	/// reads as "no earlier position" and anything stale lies at least a
	/// dictionary back. The tree keeps two children per dictionary slot.
	struct MatchFinder {
		std::vector<std::uint8_t> window;
		std::vector<std::uint32_t> head2;
		std::vector<std::uint32_t> head3;
		std::vector<std::uint32_t> head4;
		std::vector<std::uint32_t> tree;
		std::uint32_t cyclic_size = 0;
		int hash4_shift = 0;
		std::uint32_t depth = 0;
		std::uint32_t nice_length = 0;
		std::size_t inserted = 0;
	};

	/// One symbol: back is kLiteral, a rep index below kReps (rep 0 with
	/// length 1 being the short rep), or kReps plus a distance.
	struct Choice {
		std::uint32_t back;
		std::uint32_t length;
	};

	/// A node of the optimal parse: the cheapest known way to reach this
	/// many bytes past the cursor, and the coder state it leaves behind.
	struct Optimal {
		unsigned state;
		bool prev1_is_literal;
		bool prev2;
		std::uint32_t pos_prev2;
		std::uint32_t back_prev2;
		std::uint32_t price;
		std::uint32_t pos_prev;
		std::uint32_t back_prev;
		std::uint32_t backs[kReps];
	};

	struct Core {
		MatchFinder finder;
		Model model;
		std::array<Matches, 2> matches;
		unsigned current = 0;
		bool have_ahead = false;
		bool optimal = false;
		std::vector<Optimal> opts;
		std::uint32_t opts_current = 0;
		std::uint32_t opts_end = 0;
		std::size_t cursor = 0;
		std::uint64_t position = 0;
	};

	inline unsigned literalNextState(unsigned state) {
		return state < 4 ? 0 : (state < 10 ? state - 3 : state - 6);
	}

	inline unsigned matchNextState(unsigned state) {
		return state < kLiteralStates ? 7 : 10;
	}

	inline unsigned repNextState(unsigned state) {
		return state < kLiteralStates ? 8 : 11;
	}

	inline unsigned shortRepNextState(unsigned state) {
		return state < kLiteralStates ? 9 : 11;
	}

	inline unsigned distanceSlot(std::uint32_t distance) {
		if (distance < 4) return distance;

		unsigned top = 31;
		while ((distance >> top) == 0) --top;
		return (top << 1) | ((distance >> (top - 1)) & 1);
	}

	inline std::uint32_t matchLength(const std::uint8_t* earlier, const std::uint8_t* here,
	                                 std::uint32_t from, std::uint32_t limit) {
		std::uint32_t length = from;
		while (length < limit && earlier[length] == here[length]) ++length;
		return length;
	}

	inline std::size_t availableFrom(const MatchFinder& finder, std::size_t index) {
		return finder.window.size() - index;
	}

	// lzma_matchfinder.cpp
	void setUpFinder(MatchFinder& finder, const LzmaPreset& preset);
	void findMatches(MatchFinder& finder, Matches& found);
	void skipPositions(MatchFinder& finder, std::size_t target);
	std::size_t slideWindow(Core& core, std::size_t keep_from);

	// lzma_prices.cpp
	std::uint32_t bitPrice(Probability probability, unsigned bit);
	std::uint32_t literalPrice(const Model& model, unsigned previous, unsigned match_byte,
	                           unsigned byte, bool matched);
	std::uint32_t shortRepPrice(const Model& model, unsigned state, unsigned pos_state);
	std::uint32_t pureRepPrice(const Model& model, unsigned index, unsigned state,
	                           unsigned pos_state);
	std::uint32_t matchPrice(const Model& model, std::uint32_t distance, std::uint32_t length,
	                         unsigned pos_state);
	void updateLengthPrices(LengthModel& model, unsigned pos_state);
	void resetPrices(Model& model);
	void refreshDistancePrices(Model& model);

	// lzma_parser.cpp
	Choice chooseSymbol(Core& core);
	void dropPendingChoices(Core& core);

}  // namespace wbsh::lzma_encoder_detail

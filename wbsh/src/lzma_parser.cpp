/**
 * @file lzma_parser.cpp
 * @brief Picks the LZMA symbols, following xz's two encoder modes. The fast
 *        mode takes the longest match unless a rep or the match one byte
 *        further on is clearly better. The normal mode prices every way of
 *        coding the bytes ahead (literals, short reps, reps, matches, and
 *        a match or rep followed by a literal and rep0) and keeps the
 *        cheapest path, which it then hands out a symbol at a time.
 */

#include "lzma_encoder_internal.h"

#include <algorithm>

namespace wbsh::lzma_encoder_detail {

	static const Choice kLiteralChoice = { kLiteral, 1 };
	static const std::uint32_t kInfinitePrice = 1u << 30;

	struct RepLengths {
		std::uint32_t lengths[kReps];
		unsigned best;
	};

	/// The position being priced in the normal mode, cur bytes past the cursor.
	struct Step {
		std::uint32_t cur;
		std::uint32_t len_end;
		const std::uint8_t* here;
		std::uint64_t position;
		unsigned state;
		unsigned pos_state;
		std::uint32_t reps[kReps];
		std::uint32_t avail_full;
		std::uint32_t avail;
		std::uint32_t price;
		std::uint32_t literal_price;
		std::uint32_t match_base;
		std::uint32_t rep_base;
		std::uint32_t start_len;
		unsigned byte;
		unsigned match_byte;
		bool next_is_literal;
	};

	/// A rep or match about to be followed by a literal and a rep0.
	struct FirstSymbol {
		std::uint32_t price;
		std::uint32_t length;
		std::uint32_t back;
		unsigned state;
		const std::uint8_t* earlier;
	};

	static std::uint32_t availableAtCursor(const Core& core) {
		const std::size_t available = availableFrom(core.finder, core.cursor);
		const std::size_t most = kMatchMaxLength;
		return static_cast<std::uint32_t>((std::min)(available, most));
	}

	static Matches& currentMatches(Core& core) {
		Matches& found = core.matches[core.current];
		if (!core.have_ahead) findMatches(core.finder, found);
		core.have_ahead = false;
		return found;
	}

	static RepLengths repLengths(const Core& core, std::uint32_t available) {
		RepLengths reps = { { 0, 0, 0, 0 }, 0 };
		const std::uint8_t* here = core.finder.window.data() + core.cursor;
		for (unsigned k = 0; k < kReps; ++k) {
			const std::uint32_t distance = core.model.reps[k];
			if (distance >= core.position) continue;

			const std::uint8_t* earlier = here - distance - 1;
			if (earlier[0] != here[0] || earlier[1] != here[1]) continue;

			reps.lengths[k] = matchLength(earlier, here, 2, available);
			if (reps.lengths[k] > reps.lengths[reps.best]) reps.best = k;
		}

		return reps;
	}

	static Choice longestMatchChoice(const Matches& found) {
		return Choice{ kReps + found.list[found.count - 1].distance, found.longest };
	}

	// xz's change_pair: a match one byte shorter is worth taking when its
	// distance is over a hundred times smaller.
	static bool muchFarther(std::uint32_t near_distance, std::uint32_t far_distance) {
		return (far_distance >> 7) > near_distance;
	}

	static Match mainMatch(const Matches& found) {
		Match main = { 0, 0 };
		if (found.count == 0) return main;

		std::size_t index = found.count - 1;
		main = found.list[index];
		while (index > 0 && main.length == found.list[index - 1].length + 1
		       && muchFarther(found.list[index - 1].distance, main.distance)) {
			--index;
			main = found.list[index];
		}

		if (main.length == 2 && main.distance >= 0x80) main.length = 1;
		return main;
	}

	static bool repBeatsMain(std::uint32_t rep_length, const Match& main) {
		if (rep_length < 2) return false;
		return rep_length + 1 >= main.length
			|| (rep_length + 2 >= main.length && main.distance > (1u << 9))
			|| (rep_length + 3 >= main.length && main.distance > (1u << 15));
	}

	static bool nextBeatsMain(const Matches& next, const Match& main) {
		if (next.longest < 2) return false;

		const std::uint32_t next_distance = next.list[next.count - 1].distance;
		const bool closer_if_shorter = next.longest + 1 >= main.length && main.length >= 3
			&& muchFarther(next_distance, main.distance);
		return (next.longest >= main.length && next_distance < main.distance)
			|| (next.longest == main.length + 1 && !muchFarther(main.distance, next_distance))
			|| next.longest > main.length + 1
			|| closer_if_shorter;
	}

	static bool repStartsNext(const Core& core, const Match& main) {
		const std::uint8_t* next = core.finder.window.data() + core.cursor + 1;
		const std::uint32_t limit = (std::max)(2u, main.length - 1);
		for (unsigned k = 0; k < kReps; ++k) {
			const std::uint32_t distance = core.model.reps[k];
			if (distance > core.position) continue;
			if (matchLength(next - distance - 1, next, 0, limit) == limit) return true;
		}

		return false;
	}

	static bool literalAfterLookahead(Core& core, const Match& main) {
		const unsigned ahead = 1 - core.current;
		findMatches(core.finder, core.matches[ahead]);
		if (!nextBeatsMain(core.matches[ahead], main) && !repStartsNext(core, main)) return false;

		core.current = ahead;
		core.have_ahead = true;
		return true;
	}

	static Choice decideFast(Core& core) {
		const Matches& found = currentMatches(core);
		const std::uint32_t available = availableAtCursor(core);
		if (available < 2) return kLiteralChoice;

		const RepLengths reps = repLengths(core, available);
		const Choice rep = { reps.best, reps.lengths[reps.best] };
		if (rep.length >= core.finder.nice_length) return rep;
		if (found.longest >= core.finder.nice_length) return longestMatchChoice(found);

		const Match main = mainMatch(found);
		if (repBeatsMain(rep.length, main)) return rep;
		if (main.length < 2 || available <= 2) return kLiteralChoice;
		if (literalAfterLookahead(core, main)) return kLiteralChoice;
		return Choice{ kReps + main.distance, main.length };
	}

	static std::uint32_t repPrice(const Model& model, unsigned index, std::uint32_t length,
	                              unsigned state, unsigned pos_state) {
		return pureRepPrice(model, index, state, pos_state)
			+ model.rep_length.prices[pos_state][length - kMatchMinLength];
	}

	static unsigned stateAfter(unsigned state, std::uint32_t back) {
		return back < kReps ? repNextState(state) : matchNextState(state);
	}

	static void applyBack(const std::uint32_t* before, std::uint32_t back, std::uint32_t* after) {
		if (back >= kReps) {
			after[0] = back - kReps;
			std::copy(before, before + kReps - 1, after + 1);
			return;
		}

		after[0] = before[back];
		std::copy(before, before + back, after + 1);
		std::copy(before + back + 1, before + kReps, after + back + 1);
	}

	// Recovers the coder state and reps at @p cur from the path that
	// reached it, as xz's helper2 does.
	static void settleNode(Core& core, std::uint32_t cur) {
		Optimal& node = core.opts[cur];
		std::uint32_t pos_prev = node.pos_prev;
		unsigned state = core.opts[pos_prev].state;
		if (node.prev1_is_literal) {
			--pos_prev;
			const Optimal& before = node.prev2 ? core.opts[node.pos_prev2] : core.opts[pos_prev];
			state = node.prev2 ? stateAfter(before.state, node.back_prev2) : before.state;
			state = literalNextState(state);
		}

		if (pos_prev == cur - 1) {
			node.state = node.back_prev == 0 ? shortRepNextState(state) : literalNextState(state);
			std::copy(core.opts[pos_prev].backs, core.opts[pos_prev].backs + kReps, node.backs);
			return;
		}

		std::uint32_t back = node.back_prev;
		if (node.prev1_is_literal && node.prev2) {
			pos_prev = node.pos_prev2;
			back = node.back_prev2;
			node.state = repNextState(state);
		} else {
			node.state = stateAfter(state, back);
		}

		applyBack(core.opts[pos_prev].backs, back, node.backs);
	}

	static Step prepareStep(Core& core, std::uint32_t cur, std::uint32_t len_end) {
		if (cur > 0) settleNode(core, cur);

		const Optimal& node = core.opts[cur];
		Step step;
		step.cur = cur;
		step.len_end = len_end;
		step.here = core.finder.window.data() + core.cursor + cur;
		step.position = core.position + cur;
		step.state = node.state;
		step.pos_state = static_cast<unsigned>(step.position) & kPositionMask;
		std::copy(node.backs, node.backs + kReps, step.reps);

		const std::size_t available = availableFrom(core.finder, core.cursor + cur);
		const std::size_t room = kOptimumSize - 1 - cur;
		step.avail_full = static_cast<std::uint32_t>((std::min)(available, room));
		step.avail = (std::min)(step.avail_full, core.finder.nice_length);
		step.price = node.price;
		step.start_len = 2;
		step.byte = step.here[0];
		step.match_byte = *(step.here - 1 - step.reps[0]);
		step.next_is_literal = false;
		return step;
	}

	static void extendTo(Core& core, Step& step, std::uint32_t offset) {
		while (step.len_end < offset) core.opts[++step.len_end].price = kInfinitePrice;
	}

	static bool offer(Optimal& node, std::uint32_t price, std::uint32_t pos_prev,
	                  std::uint32_t back) {
		if (price >= node.price) return false;

		node.price = price;
		node.pos_prev = pos_prev;
		node.back_prev = back;
		node.prev1_is_literal = false;
		return true;
	}

	static void offerLiteral(Core& core, Step& step) {
		const Model& model = core.model;
		const bool matched = step.state >= kLiteralStates;
		step.literal_price = step.price + bitPrice(model.is_match[step.state][step.pos_state], 0)
			+ literalPrice(model, step.here[-1], step.match_byte, step.byte, matched);
		Optimal& next = core.opts[step.cur + 1];
		step.next_is_literal = offer(next, step.literal_price, step.cur, kLiteral);

		step.match_base = step.price + bitPrice(model.is_match[step.state][step.pos_state], 1);
		step.rep_base = step.match_base + bitPrice(model.is_rep[step.state], 1);
	}

	static void offerShortRep(Core& core, Step& step) {
		Optimal& next = core.opts[step.cur + 1];
		const bool rep0_reaches_next = next.pos_prev < step.cur && next.back_prev == 0;
		if (step.match_byte != step.byte || rep0_reaches_next) return;

		const std::uint32_t price = step.rep_base
			+ shortRepPrice(core.model, step.state, step.pos_state);
		if (price > next.price) return;

		next.price = price;
		next.pos_prev = step.cur;
		next.back_prev = 0;
		next.prev1_is_literal = false;
		step.next_is_literal = true;
	}

	static std::uint32_t rep0AfterLiteralPrice(const Model& model, std::uint32_t price,
	                                           unsigned state, std::uint64_t position,
	                                           std::uint32_t length) {
		const unsigned pos_state = static_cast<unsigned>(position) & kPositionMask;
		return price + bitPrice(model.is_match[state][pos_state], 1)
			+ bitPrice(model.is_rep[state], 1) + repPrice(model, 0, length, state, pos_state);
	}

	static void offerLiteralThenRep0(Core& core, Step& step) {
		if (step.next_is_literal || step.match_byte == step.byte) return;

		const std::uint8_t* earlier = step.here - step.reps[0] - 1;
		const std::uint32_t limit = (std::min)(step.avail_full, core.finder.nice_length + 1);
		const std::uint32_t length = matchLength(earlier, step.here, 1, limit) - 1;
		if (length < 2) return;

		const unsigned state = literalNextState(step.state);
		const std::uint32_t price =
			rep0AfterLiteralPrice(core.model, step.literal_price, state, step.position + 1, length);
		const std::uint32_t offset = step.cur + 1 + length;
		extendTo(core, step, offset);

		Optimal& node = core.opts[offset];
		if (!offer(node, price, step.cur + 1, 0)) return;
		node.prev1_is_literal = true;
		node.prev2 = false;
	}

	// A rep or match, the literal that broke it off, then rep0 again.
	static void offerThenLiteralThenRep0(Core& core, Step& step, const FirstSymbol& first) {
		std::uint32_t second = first.length + 1;
		const std::uint32_t limit = (std::min)(step.avail_full, second + core.finder.nice_length);
		if (second < limit) second = matchLength(first.earlier, step.here, second, limit);
		second -= first.length + 1;
		if (second < 2) return;

		const Model& model = core.model;
		const std::uint64_t literal_at = step.position + first.length;
		const unsigned pos_state = static_cast<unsigned>(literal_at) & kPositionMask;
		const std::uint8_t* literal = step.here + first.length;
		const std::uint32_t literal_price = first.price
			+ bitPrice(model.is_match[first.state][pos_state], 0)
			+ literalPrice(model, literal[-1], first.earlier[first.length], literal[0], true);
		const unsigned state = literalNextState(first.state);
		const std::uint32_t price =
			rep0AfterLiteralPrice(model, literal_price, state, literal_at + 1, second);

		const std::uint32_t offset = step.cur + first.length + 1 + second;
		extendTo(core, step, offset);
		Optimal& node = core.opts[offset];
		if (!offer(node, price, step.cur + first.length + 1, 0)) return;
		node.prev1_is_literal = true;
		node.prev2 = true;
		node.pos_prev2 = step.cur;
		node.back_prev2 = first.back;
	}

	static void offerRep(Core& core, Step& step, unsigned index, const std::uint8_t* earlier) {
		const std::uint32_t length = matchLength(earlier, step.here, 2, step.avail);
		extendTo(core, step, step.cur + length);

		const Model& model = core.model;
		const std::uint32_t* length_prices = model.rep_length.prices[step.pos_state];
		const std::uint32_t base = step.rep_base
			+ pureRepPrice(model, index, step.state, step.pos_state);
		for (std::uint32_t take = length; take >= kMatchMinLength; --take) {
			const std::uint32_t price = base + length_prices[take - kMatchMinLength];
			offer(core.opts[step.cur + take], price, step.cur, index);
		}

		if (index == 0) step.start_len = length + 1;

		const std::uint32_t price = base + length_prices[length - kMatchMinLength];
		const FirstSymbol first = { price, length, index, repNextState(step.state), earlier };
		offerThenLiteralThenRep0(core, step, first);
	}

	static void offerReps(Core& core, Step& step) {
		for (unsigned index = 0; index < kReps; ++index) {
			const std::uint8_t* earlier = step.here - step.reps[index] - 1;
			if (earlier[0] != step.here[0] || earlier[1] != step.here[1]) continue;
			offerRep(core, step, index, earlier);
		}
	}

	static void clampMatches(Matches& matches, std::uint32_t limit) {
		if (matches.longest <= limit) return;

		std::size_t count = 0;
		while (limit > matches.list[count].length) ++count;
		matches.list[count].length = limit;
		matches.count = count + 1;
		matches.longest = limit;
	}

	static void offerMatches(Core& core, Step& step, Matches& matches) {
		clampMatches(matches, step.avail);
		if (matches.longest < step.start_len) return;

		const Model& model = core.model;
		const std::uint32_t base = step.match_base + bitPrice(model.is_rep[step.state], 0);
		extendTo(core, step, step.cur + matches.longest);

		std::size_t index = 0;
		while (step.start_len > matches.list[index].length) ++index;
		for (std::uint32_t length = step.start_len;; ++length) {
			const Match& match = matches.list[index];
			const std::uint32_t back = kReps + match.distance;
			const std::uint32_t price = base
				+ matchPrice(model, match.distance, length, step.pos_state);
			offer(core.opts[step.cur + length], price, step.cur, back);
			if (length != match.length) continue;

			const std::uint8_t* earlier = step.here - match.distance - 1;
			const FirstSymbol first = { price, length, back, matchNextState(step.state), earlier };
			offerThenLiteralThenRep0(core, step, first);
			if (++index == matches.count) break;
		}
	}

	static std::uint32_t evaluatePosition(Core& core, std::uint32_t cur, std::uint32_t len_end,
	                                      Matches& matches) {
		Step step = prepareStep(core, cur, len_end);
		offerLiteral(core, step);
		offerShortRep(core, step);
		if (step.avail_full < 2) return step.len_end;

		offerLiteralThenRep0(core, step);
		offerReps(core, step);
		offerMatches(core, step, matches);
		return step.len_end;
	}

	static std::uint32_t startOptimum(Core& core, Matches& found, const RepLengths& reps) {
		Optimal& origin = core.opts[0];
		origin.state = core.model.state;
		origin.price = 0;
		origin.prev1_is_literal = false;
		std::copy(core.model.reps, core.model.reps + kReps, origin.backs);

		const std::uint32_t len_end = (std::max)({ found.longest, reps.lengths[reps.best], 1u });
		for (std::uint32_t k = 1; k <= len_end; ++k) core.opts[k].price = kInfinitePrice;
		return evaluatePosition(core, 0, len_end, found);
	}

	// Extends the parse a byte at a time while some path still reaches
	// further; a match of nice length ends it early, its matches kept for
	// the next parse. Returns how many bytes the parse covers.
	static std::uint32_t optimize(Core& core, Matches& found, const RepLengths& reps) {
		std::uint32_t len_end = startOptimum(core, found, reps);
		std::uint32_t cur = 1;
		for (; cur < len_end; ++cur) {
			Matches& next = core.matches[core.current];
			findMatches(core.finder, next);
			if (next.longest >= core.finder.nice_length) {
				core.have_ahead = true;
				break;
			}

			len_end = evaluatePosition(core, cur, len_end, next);
		}

		return cur;
	}

	static void unfoldLiteral(std::vector<Optimal>& opts, std::uint32_t cur,
	                          std::uint32_t literal_at) {
		Optimal& literal = opts[literal_at];
		literal.back_prev = kLiteral;
		literal.prev1_is_literal = false;
		literal.pos_prev = literal_at - 1;
		if (!opts[cur].prev2) return;

		Optimal& first = opts[literal_at - 1];
		first.prev1_is_literal = false;
		first.pos_prev = opts[cur].pos_prev2;
		first.back_prev = opts[cur].back_prev2;
	}

	// Turns the backward links of the cheapest path into forward ones:
	// afterwards each node's pos_prev is where its symbol ends.
	static void reversePath(Core& core, std::uint32_t cur) {
		std::vector<Optimal>& opts = core.opts;
		core.opts_end = cur;
		core.opts_current = 0;

		std::uint32_t pos_mem = opts[cur].pos_prev;
		std::uint32_t back_mem = opts[cur].back_prev;
		do {
			if (opts[cur].prev1_is_literal) unfoldLiteral(opts, cur, pos_mem);

			const std::uint32_t pos_prev = pos_mem;
			const std::uint32_t back_cur = back_mem;
			back_mem = opts[pos_prev].back_prev;
			pos_mem = opts[pos_prev].pos_prev;
			opts[pos_prev].back_prev = back_cur;
			opts[pos_prev].pos_prev = cur;
			cur = pos_prev;
		} while (cur != 0);
	}

	static bool hasPending(const Core& core) {
		return core.opts_current != core.opts_end;
	}

	static Choice nextPending(Core& core) {
		const Optimal& node = core.opts[core.opts_current];
		const Choice choice = { node.back_prev, node.pos_prev - core.opts_current };
		core.opts_current = node.pos_prev;
		return choice;
	}

	static bool nothingToGain(const Core& core, const Matches& found, const RepLengths& reps) {
		const std::uint8_t* here = core.finder.window.data() + core.cursor;
		const bool rep0_byte = here[0] == *(here - 1 - core.model.reps[0]);
		return found.longest < 2 && !rep0_byte && reps.lengths[reps.best] < 2;
	}

	static Choice decideOptimal(Core& core) {
		refreshDistancePrices(core.model);
		Matches& found = currentMatches(core);
		const std::uint32_t available = availableAtCursor(core);
		if (available < 2) return kLiteralChoice;

		const RepLengths reps = repLengths(core, available);
		const Choice rep = { reps.best, reps.lengths[reps.best] };
		if (rep.length >= core.finder.nice_length) return rep;
		if (found.longest >= core.finder.nice_length) return longestMatchChoice(found);
		if (nothingToGain(core, found, reps)) return kLiteralChoice;

		reversePath(core, optimize(core, found, reps));
		return nextPending(core);
	}

	// After the models were reset under it the finder may already be past
	// the cursor; those bytes go out as literals.
	static bool finderAhead(const Core& core) {
		return core.finder.inserted > core.cursor + (core.have_ahead ? 1 : 0);
	}

	static Choice decide(Core& core) {
		if (hasPending(core)) return nextPending(core);
		if (core.position == 0 || finderAhead(core)) return kLiteralChoice;
		return core.optimal ? decideOptimal(core) : decideFast(core);
	}

	Choice chooseSymbol(Core& core) {
		const Choice choice = decide(core);
		const std::size_t ahead = core.have_ahead ? 1 : 0;
		skipPositions(core.finder, core.cursor + choice.length + ahead);
		return choice;
	}

	void dropPendingChoices(Core& core) {
		core.opts_current = 0;
		core.opts_end = 0;
		core.have_ahead = false;
	}

}  // namespace wbsh::lzma_encoder_detail

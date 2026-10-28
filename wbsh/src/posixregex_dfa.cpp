/**
 * @file posixregex_dfa.cpp
 * @brief Lazily built DFA answering "is there a match at or after this
 *        position", for patterns whose only assertions are ^ and $ at the
 *        ends of the subject. States are sets of Pike VM threads, built the
 *        first time a character leads to them and cached on the program.
 */

#include "posixregex_internal.h"

#include <algorithm>
#include <cstring>

namespace wbsh::posixregex_detail {

	static const std::size_t kMaxStates = 2048;
	static const int kUnknown = -1;

	struct ClosureContext {
		bool at_start = false;
		bool at_end = false;
	};

	enum class DfaOutcome {
		Match,
		NoMatch,
		GaveUp,
	};

	static bool dfaAnchorHolds(Anchor anchor, ClosureContext context) {
		switch (anchor) {
		case Anchor::LineStart:
		case Anchor::TextStart: return context.at_start;
		case Anchor::LineEnd:
		case Anchor::TextEnd:   return context.at_end;
		default:                return false;
		}
	}

	static void pushNext(const Inst& inst, int pc, ClosureContext context,
	                     std::vector<int>& stack) {
		switch (inst.op) {
		case Op::Jump:
			stack.push_back(inst.x);
			break;
		case Op::Split:
			stack.push_back(inst.y);
			stack.push_back(inst.x);
			break;
		case Op::Assert:
			if (dfaAnchorHolds(inst.anchor, context)) stack.push_back(pc + 1);
			break;
		default:
			stack.push_back(pc + 1);
			break;
		}
	}

	static bool isThreadOp(Op op) {
		return op == Op::Char || op == Op::CharFold || op == Op::Any || op == Op::Set
			|| op == Op::Match;
	}

	// The consuming and Match instructions reachable from @p entry without
	// consuming, in ascending order.
	static std::vector<int> closure(const Program& program, const std::vector<int>& entry,
	                                ClosureContext context) {
		std::vector<bool> seen(program.code.size(), false);
		std::vector<int> stack(entry.rbegin(), entry.rend());
		std::vector<int> threads;
		while (!stack.empty()) {
			const int pc = stack.back();
			stack.pop_back();
			if (seen[static_cast<std::size_t>(pc)]) continue;
			seen[static_cast<std::size_t>(pc)] = true;

			const Inst& inst = program.code[static_cast<std::size_t>(pc)];
			if (isThreadOp(inst.op)) threads.push_back(pc);
			else                     pushNext(inst, pc, context, stack);
		}

		std::sort(threads.begin(), threads.end());
		return threads;
	}

	static bool hasMatch(const Program& program, const std::vector<int>& threads) {
		for (const int pc : threads) {
			if (program.code[static_cast<std::size_t>(pc)].op == Op::Match) return true;
		}

		return false;
	}

	static int findOrAddState(const Program& program, DfaCache& cache, std::vector<int> entry,
	                          bool initial) {
		const auto key = std::make_pair(initial, entry);
		const auto found = cache.index.find(key);
		if (found != cache.index.end()) return found->second;

		DfaState state;
		state.entry = std::move(entry);
		state.initial = initial;
		ClosureContext context;
		context.at_start = initial;
		state.threads = closure(program, state.entry, context);
		state.accepting = hasMatch(program, state.threads);
		state.next.fill(kUnknown);
		cache.states.push_back(std::move(state));
		const int id = static_cast<int>(cache.states.size()) - 1;
		cache.index.emplace(key, id);
		return id;
	}

	static int computeNext(const Program& program, DfaCache& cache, int from, char32_t code) {
		std::vector<int> entry(1, 0);
		for (const int pc : cache.states[static_cast<std::size_t>(from)].threads) {
			const Inst& inst = program.code[static_cast<std::size_t>(pc)];
			if (consumes(program, inst, code)) entry.push_back(pc + 1);
		}

		std::sort(entry.begin(), entry.end());
		entry.erase(std::unique(entry.begin(), entry.end()), entry.end());
		return findOrAddState(program, cache, std::move(entry), false);
	}

	static int transition(const Program& program, DfaCache& cache, int from, char32_t code) {
		DfaState& state = cache.states[static_cast<std::size_t>(from)];
		if (code < 0x80 && state.next[code] != kUnknown) return state.next[code];
		if (code >= 0x80) {
			const auto found = state.wide.find(code);
			if (found != state.wide.end()) return found->second;
		}

		const int to = computeNext(program, cache, from, code);
		DfaState& updated = cache.states[static_cast<std::size_t>(from)];
		if (code < 0x80) updated.next[code] = to;
		else             updated.wide.emplace(code, to);
		return to;
	}

	static bool acceptsAtEnd(const Program& program, DfaState& state) {
		if (state.accepting) return true;
		if (state.accepting_at_end != kUnknown) return state.accepting_at_end != 0;

		ClosureContext context;
		context.at_start = state.initial;
		context.at_end = true;
		const bool accepts = hasMatch(program, closure(program, state.entry, context));
		state.accepting_at_end = accepts ? 1 : 0;
		return accepts;
	}

	static void resetIfFull(DfaCache& cache) {
		if (cache.states.size() < kMaxStates) return;
		cache.states.clear();
		cache.index.clear();
		cache.initial = kUnknown;
		cache.idle = kUnknown;
	}

	bool dfaUsable(const Program& program) {
		if (program.has_backrefs || program.multiline) return false;

		for (const Inst& inst : program.code) {
			if (inst.op != Op::Assert) continue;
			if (inst.anchor > Anchor::TextEnd) return false;
		}

		return true;
	}

	// Only the restart thread is alive: nothing can match before the next
	// position whose byte may begin a match.
	static bool skipIdle(const Program& program, const std::string& text, std::size_t& pos) {
		if (program.start_anchored) return false;

		pos = nextCandidate(program, text, pos);
		return pos != std::string::npos;
	}

	static int restartState(const Program& program, DfaCache& cache, bool initial) {
		int& id = initial ? cache.initial : cache.idle;
		if (id == kUnknown) id = findOrAddState(program, cache, std::vector<int>(1, 0), initial);
		return id;
	}

	static DfaOutcome runDfa(const Program& program, DfaCache& cache, const std::string& text,
	                         std::size_t from) {
		int state = restartState(program, cache, from == 0);
		const int idle = restartState(program, cache, false);
		for (std::size_t pos = from; pos < text.size();) {
			if (cache.states[static_cast<std::size_t>(state)].accepting) return DfaOutcome::Match;
			if (state == idle && !skipIdle(program, text, pos)) return DfaOutcome::NoMatch;

			const Utf8Char ch = decodeChar(text, pos);
			state = transition(program, cache, state, ch.code);
			pos += ch.length;
			if (cache.states.size() >= kMaxStates) return DfaOutcome::GaveUp;
		}

		const bool accepts = acceptsAtEnd(program, cache.states[static_cast<std::size_t>(state)]);
		return accepts ? DfaOutcome::Match : DfaOutcome::NoMatch;
	}

	// Answers "does any match start at or after @p from". A subject that
	// needs more states than the cache holds is left to the Pike VM.
	bool dfaTest(const Program& program, const std::string& text, std::size_t from) {
		DfaCache& cache = *program.dfa;
		resetIfFull(cache);
		const DfaOutcome outcome = runDfa(program, cache, text, from);
		if (outcome != DfaOutcome::GaveUp) return outcome == DfaOutcome::Match;

		resetIfFull(cache);
		return pikeSearch(program, Subject{ text, false }, from, false, nullptr);
	}

}  // namespace wbsh::posixregex_detail

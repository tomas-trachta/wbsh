/**
 * @file posixregex_pike.cpp
 * @brief Leftmost-longest Pike VM: every thread advances in lock step, one
 *        character at a time, so the subject is read once and no matcher
 *        state lives on the call stack.
 */

#include "posixregex_internal.h"

#include <cstring>

#include "utf8.h"

namespace wbsh::posixregex_detail {

	struct ThreadList {
		std::vector<int> sparse;
		std::vector<int> dense;
		std::vector<std::size_t> caps;
		int size = 0;
	};

	struct Pending {
		int pc = 0;
		int slot = -1;
		std::size_t value = 0;
	};

	struct PikeScratch {
		ThreadList lists[2];
		std::vector<Pending> stack;
		std::vector<std::size_t> work;
		std::vector<std::size_t> fresh;
		std::vector<std::size_t> best;
	};

	struct Pike {
		const Program& program;
		const Subject& subject;
		std::size_t ncap;
		PikeScratch& scratch;
		ThreadList* current = nullptr;
		ThreadList* next = nullptr;
		bool matched = false;
	};

	static thread_local PikeScratch t_scratch;

	static void resetList(ThreadList& list, std::size_t code_size, std::size_t ncap) {
		if (list.sparse.size() < code_size) {
			list.sparse.resize(code_size);
			list.dense.resize(code_size);
		}

		if (list.caps.size() < code_size * ncap) list.caps.resize(code_size * ncap);
		list.size = 0;
	}

	static bool listHas(const ThreadList& list, int pc) {
		const int index = list.sparse[static_cast<std::size_t>(pc)];
		return index < list.size && list.dense[static_cast<std::size_t>(index)] == pc;
	}

	static int listInsert(ThreadList& list, int pc) {
		const int index = list.size++;
		list.sparse[static_cast<std::size_t>(pc)] = index;
		list.dense[static_cast<std::size_t>(index)] = pc;
		return index;
	}

	static void pushVisit(Pike& vm, int pc) {
		Pending pending;
		pending.pc = pc;
		vm.scratch.stack.push_back(pending);
	}

	static void pushSave(Pike& vm, const Inst& inst, std::size_t pos) {
		const auto slot = static_cast<std::size_t>(inst.x);
		if (slot >= vm.ncap) return;

		Pending restore;
		restore.slot = inst.x;
		restore.value = vm.scratch.work[slot];
		vm.scratch.stack.push_back(restore);
		vm.scratch.work[slot] = pos;
	}

	static void storeThread(Pike& vm, ThreadList& list, int index) {
		if (vm.ncap == 0) return;

		std::size_t* caps = &list.caps[static_cast<std::size_t>(index) * vm.ncap];
		std::memcpy(caps, vm.scratch.work.data(), vm.ncap * sizeof(std::size_t));
	}

	static void expand(Pike& vm, ThreadList& list, int pc, std::size_t pos) {
		if (listHas(list, pc)) return;

		const int index = listInsert(list, pc);
		const Inst& inst = vm.program.code[static_cast<std::size_t>(pc)];
		switch (inst.op) {
		case Op::Jump:
			pushVisit(vm, inst.x);
			break;
		case Op::Split:
			pushVisit(vm, inst.y);
			pushVisit(vm, inst.x);
			break;
		case Op::Save:
			pushSave(vm, inst, pos);
			pushVisit(vm, pc + 1);
			break;
		case Op::Assert:
			if (anchorHolds(inst.anchor, vm.subject, pos)) pushVisit(vm, pc + 1);
			break;
		case Op::LoopMark:
		case Op::LoopCheck:
			pushVisit(vm, pc + 1);
			break;
		default:
			storeThread(vm, list, index);
			break;
		}
	}

	// Adds the thread at @p pc and everything it reaches without consuming,
	// in priority order, each with the captures of the path that got there.
	static void addThread(Pike& vm, ThreadList& list, int pc, std::size_t pos,
	                      const std::size_t* caps) {
		PikeScratch& scratch = vm.scratch;
		scratch.work.assign(caps, caps + vm.ncap);
		scratch.stack.clear();
		pushVisit(vm, pc);
		while (!scratch.stack.empty()) {
			const Pending pending = scratch.stack.back();
			scratch.stack.pop_back();
			const auto slot = static_cast<std::size_t>(pending.slot);
			if (pending.slot >= 0) scratch.work[slot] = pending.value;
			else                   expand(vm, list, pending.pc, pos);
		}
	}

	static void recordMatch(Pike& vm, const std::size_t* caps) {
		std::vector<std::size_t>& best = vm.scratch.best;
		const bool better = caps != nullptr && (!vm.matched || caps[0] < best[0]
			|| (caps[0] == best[0] && caps[1] > best[1]));
		if (better) best.assign(caps, caps + vm.ncap);
		vm.matched = true;
	}

	// Returns true once the answer is known: a boolean search stops at its
	// first match.
	static bool step(Pike& vm, std::size_t pos, Utf8Char ch, bool at_end) {
		const ThreadList& list = *vm.current;
		for (int i = 0; i < list.size; ++i) {
			const int pc = list.dense[static_cast<std::size_t>(i)];
			const Inst& inst = vm.program.code[static_cast<std::size_t>(pc)];
			const std::size_t* caps = vm.ncap == 0 ? nullptr
				: &list.caps[static_cast<std::size_t>(i) * vm.ncap];
			if (vm.matched && caps != nullptr && caps[0] > vm.scratch.best[0]) continue;

			if (inst.op == Op::Match) {
				recordMatch(vm, caps);
				if (vm.ncap == 0) return true;
				continue;
			}

			if (at_end || !consumes(vm.program, inst, ch.code)) continue;
			addThread(vm, *vm.next, pc + 1, pos + ch.length, caps);
		}

		return false;
	}

	static bool mayBeginWith(const Program& program, const std::string& text, std::size_t pos) {
		if (program.can_match_empty) return true;
		return pos < text.size() && program.first_bytes[static_cast<unsigned char>(text[pos])];
	}

	static bool mayStartAt(const Pike& vm, std::size_t pos, std::size_t from, bool anchored) {
		if (anchored) return pos == from;
		if (vm.program.start_anchored && pos != 0) return false;
		return mayBeginWith(vm.program, vm.subject.text, pos);
	}

	static void prepare(Pike& vm) {
		const std::size_t code_size = vm.program.code.size();
		resetList(vm.scratch.lists[0], code_size, vm.ncap);
		resetList(vm.scratch.lists[1], code_size, vm.ncap);
		vm.current = &vm.scratch.lists[0];
		vm.next = &vm.scratch.lists[1];
		vm.scratch.fresh.assign(vm.ncap, std::string::npos);
	}

	static void addStartThread(Pike& vm, std::size_t pos) {
		addThread(vm, *vm.current, 0, pos, vm.scratch.fresh.data());
	}

	static bool seekStart(Pike& vm, std::size_t& pos, std::size_t from, bool anchored) {
		if (vm.current->size > 0) return true;

		const bool pinned = anchored || (vm.program.start_anchored && pos > 0);
		if (pinned) return pos == from && mayStartAt(vm, pos, from, anchored);

		pos = nextCandidate(vm.program, vm.subject.text, pos);
		return pos != std::string::npos;
	}

	bool pikeSearch(const Program& program, const Subject& subject, std::size_t from,
	                bool anchored, std::vector<std::size_t>* spans) {
		Pike vm{ program, subject, spans == nullptr ? 0 : spans->size(), t_scratch };
		prepare(vm);
		const std::string& text = subject.text;
		for (std::size_t pos = from;;) {
			if (!vm.matched) {
				if (!seekStart(vm, pos, from, anchored)) break;
				if (mayStartAt(vm, pos, from, anchored)) addStartThread(vm, pos);
			}

			if (vm.current->size == 0) break;

			const bool at_end = pos >= text.size();
			const Utf8Char ch = at_end ? Utf8Char{ 0, 0 } : decodeChar(text, pos);
			vm.next->size = 0;
			if (step(vm, pos, ch, at_end) || at_end) break;

			std::swap(vm.current, vm.next);
			pos += ch.length;
		}

		if (vm.matched && spans != nullptr) *spans = vm.scratch.best;
		return vm.matched;
	}

	static std::size_t findByte(const std::string& text, std::size_t pos, int byte) {
		if (pos >= text.size()) return std::string::npos;

		const void* found = std::memchr(text.data() + pos, byte, text.size() - pos);
		if (found == nullptr) return std::string::npos;
		return static_cast<std::size_t>(static_cast<const char*>(found) - text.data());
	}

	std::size_t nextCandidate(const Program& program, const std::string& text, std::size_t pos) {
		if (program.can_match_empty) return pos;

		if (program.single_first_byte >= 0) return findByte(text, pos, program.single_first_byte);

		while (pos < text.size()) {
			const auto byte = static_cast<unsigned char>(text[pos]);
			if (program.first_bytes[byte]) return pos;
			pos += decodeChar(text, pos).length;
		}

		return std::string::npos;
	}

}  // namespace wbsh::posixregex_detail

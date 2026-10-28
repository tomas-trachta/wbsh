/**
 * @file posixregex_backtrack.cpp
 * @brief Matcher for patterns with back-references: a backtracker that
 *        keeps its choice points on a heap-allocated stack and explores
 *        every path from a start position to find the longest match.
 */

#include "posixregex_internal.h"

#include "utf8.h"

namespace wbsh::posixregex_detail {

	enum class FrameKind : unsigned char {
		Visit,
		RestoreCap,
		RestoreMark,
	};

	struct Frame {
		FrameKind kind = FrameKind::Visit;
		int index = 0;
		std::size_t value = 0;
	};

	struct BacktrackScratch {
		std::vector<Frame> stack;
		std::vector<std::size_t> caps;
		std::vector<std::size_t> marks;
		std::vector<std::size_t> best;
	};

	struct Backtracker {
		const Program& program;
		const Subject& subject;
		BacktrackScratch& scratch;
		bool found = false;
	};

	static thread_local BacktrackScratch t_scratch;

	static void push(Backtracker& b, FrameKind kind, int index, std::size_t value) {
		Frame frame;
		frame.kind = kind;
		frame.index = index;
		frame.value = value;
		b.scratch.stack.push_back(frame);
	}

	static void setTracked(Backtracker& b, FrameKind kind, std::vector<std::size_t>& slots,
	                       int index, std::size_t value) {
		const auto slot = static_cast<std::size_t>(index);
		push(b, kind, index, slots[slot]);
		slots[slot] = value;
	}

	static bool sameCode(char32_t left, char32_t right, bool fold) {
		return fold ? foldCase(left) == foldCase(right) : left == right;
	}

	static bool sameCharacters(const std::string& text, std::size_t a, std::size_t b,
	                           std::size_t length, bool fold) {
		for (std::size_t done = 0; done < length;) {
			const Utf8Char left = decodeChar(text, a + done);
			const Utf8Char right = decodeChar(text, b + done);
			if (!sameCode(left.code, right.code, fold) || left.length != right.length) return false;
			done += left.length;
		}

		return true;
	}

	static bool matchBackRef(Backtracker& b, int group, bool fold, std::size_t& pos) {
		const std::size_t start = b.scratch.caps[static_cast<std::size_t>(group) * 2];
		const std::size_t end = b.scratch.caps[static_cast<std::size_t>(group) * 2 + 1];
		if (start == std::string::npos || end == std::string::npos) return false;

		const std::string& text = b.subject.text;
		const std::size_t length = end - start;
		if (pos + length > text.size()) return false;
		if (!sameCharacters(text, start, pos, length, fold)) return false;

		pos += length;
		return true;
	}

	static void recordLongest(Backtracker& b, std::size_t pos) {
		std::vector<std::size_t>& best = b.scratch.best;
		if (b.found && pos <= best[1]) return;

		best = b.scratch.caps;
		best[1] = pos;
		b.found = true;
	}

	static bool consumeAt(const Backtracker& b, const Inst& inst, std::size_t& pos) {
		const std::string& text = b.subject.text;
		if (pos >= text.size()) return false;

		const Utf8Char ch = decodeChar(text, pos);
		if (!consumes(b.program, inst, ch.code)) return false;

		pos += ch.length;
		return true;
	}

	// Advances one thread until it fails or matches. Returns true when the
	// match reaches the end of the subject, since nothing can beat it.
	static bool runThread(Backtracker& b, int pc, std::size_t pos) {
		BacktrackScratch& s = b.scratch;
		for (;;) {
			const Inst& inst = b.program.code[static_cast<std::size_t>(pc)];
			switch (inst.op) {
			case Op::Split:
				push(b, FrameKind::Visit, inst.y, pos);
				pc = inst.x;
				continue;
			case Op::Jump:
				pc = inst.x;
				continue;
			case Op::Save:
				setTracked(b, FrameKind::RestoreCap, s.caps, inst.x, pos);
				break;
			case Op::Assert:
				if (!anchorHolds(inst.anchor, b.subject, pos)) return false;
				break;
			case Op::BackRef:
				if (!matchBackRef(b, inst.x, inst.y != 0, pos)) return false;
				break;
			case Op::LoopMark:
				setTracked(b, FrameKind::RestoreMark, s.marks, inst.x, pos);
				break;
			case Op::LoopCheck:
				if (s.marks[static_cast<std::size_t>(inst.x)] == pos) return false;
				break;
			case Op::Match:
				recordLongest(b, pos);
				return pos == b.subject.text.size();
			default:
				if (!consumeAt(b, inst, pos)) return false;
				break;
			}

			++pc;
		}
	}

	static bool runFrom(Backtracker& b, std::size_t start) {
		BacktrackScratch& s = b.scratch;
		s.caps.assign(static_cast<std::size_t>(b.program.groups) * 2, std::string::npos);
		s.marks.assign(static_cast<std::size_t>(b.program.loops), std::string::npos);
		s.stack.clear();
		push(b, FrameKind::Visit, 0, start);
		while (!s.stack.empty()) {
			const Frame frame = s.stack.back();
			s.stack.pop_back();
			const auto slot = static_cast<std::size_t>(frame.index);
			switch (frame.kind) {
			case FrameKind::RestoreCap:  s.caps[slot] = frame.value;  break;
			case FrameKind::RestoreMark: s.marks[slot] = frame.value; break;
			default:
				if (runThread(b, frame.index, frame.value)) return true;
				break;
			}
		}

		return b.found;
	}

	static std::size_t nextStart(const Backtracker& b, std::size_t pos) {
		const std::string& text = b.subject.text;
		if (pos >= text.size()) return std::string::npos;
		return nextCandidate(b.program, text, pos + decodeChar(text, pos).length);
	}

	bool backtrackSearch(const Program& program, const Subject& subject, std::size_t from,
	                     bool anchored, std::vector<std::size_t>* spans) {
		Backtracker b{ program, subject, t_scratch };
		const bool pinned = anchored || program.start_anchored;
		std::size_t pos = pinned ? from : nextCandidate(program, subject.text, from);
		while (pos != std::string::npos) {
			const bool may_start = !program.start_anchored || pos == 0;
			if (may_start && runFrom(b, pos)) break;
			if (pinned) break;
			pos = nextStart(b, pos);
		}

		if (b.found && spans != nullptr) {
			const auto wanted = static_cast<std::ptrdiff_t>(spans->size());
			spans->assign(b.scratch.best.begin(), b.scratch.best.begin() + wanted);
		}

		return b.found;
	}

}  // namespace wbsh::posixregex_detail

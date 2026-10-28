/**
 * @file posixregex.cpp
 * @brief Regex compilation, program analysis and the public matching entry points.
 */

#include "posixregex.h"

#include <cctype>

#include "posixregex_internal.h"
#include "utf8.h"

namespace wbsh {

	namespace posixregex_detail {

		static const std::string kAlnumClass = "alnum";

		char32_t foldCase(char32_t code) {
			if (code < 0x80) return static_cast<char32_t>(std::tolower(static_cast<int>(code)));
			return lowerCode(code);
		}

		bool isWordCode(char32_t code) {
			if (code < 0x80) return code == U'_' || std::isalnum(static_cast<int>(code)) != 0;
			return isInCharClass(kAlnumClass, code);
		}

		static bool inRanges(const CharSet& set, char32_t code) {
			for (const auto& range : set.ranges) {
				if (code >= range.first && code <= range.second) return true;
			}

			return false;
		}

		static bool inClasses(const CharSet& set, char32_t code) {
			for (const std::string& name : set.classes) {
				if (isInCharClass(name, code)) return true;
			}

			return false;
		}

		static bool containsExactly(const CharSet& set, char32_t code) {
			return inRanges(set, code) || inClasses(set, code);
		}

		static bool contains(const CharSet& set, char32_t code) {
			if (containsExactly(set, code)) return true;
			if (!set.ignore_case) return false;

			return containsExactly(set, lowerCode(code)) || containsExactly(set, upperCode(code));
		}

		void finishCharSet(CharSet& set) {
			for (char32_t code = 0; code < 0x80; ++code) {
				set.ascii[code] = contains(set, code) != set.negated;
			}
		}

		bool charSetMatches(const CharSet& set, char32_t code) {
			if (code < 0x80) return set.ascii[code];
			return contains(set, code) != set.negated;
		}

		bool consumes(const Program& program, const Inst& inst, char32_t code) {
			const auto set = static_cast<std::size_t>(inst.x);
			switch (inst.op) {
			case Op::Char:     return code == inst.c;
			case Op::CharFold: return foldCase(code) == inst.c;
			case Op::Any:      return true;
			case Op::Set:      return charSetMatches(program.sets[set], code);
			default:           return false;
			}
		}

		static bool isContinuationByte(char c) {
			return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
		}

		static char32_t codeBefore(const std::string& text, std::size_t pos) {
			std::size_t start = pos - 1;
			while (start > 0 && pos - start < 4 && isContinuationByte(text[start])) --start;

			const Utf8Char decoded = decodeUtf8(text, start, pos);
			if (start + decoded.length == pos) return decoded.code;
			return decodeUtf8(text, pos - 1, pos).code;
		}

		static bool wordBefore(const std::string& text, std::size_t pos) {
			return pos > 0 && isWordCode(codeBefore(text, pos));
		}

		static bool wordAfter(const std::string& text, std::size_t pos) {
			return pos < text.size() && isWordCode(decodeChar(text, pos).code);
		}

		static bool wordAnchorHolds(Anchor anchor, const std::string& text, std::size_t pos) {
			const bool before = wordBefore(text, pos);
			const bool after = wordAfter(text, pos);
			switch (anchor) {
			case Anchor::WordBoundary:    return before != after;
			case Anchor::NotWordBoundary: return before == after;
			case Anchor::WordStart:       return !before && after;
			case Anchor::WordEnd:         return before && !after;
			case Anchor::NotAfterWord:    return !before;
			case Anchor::NotBeforeWord:   return !after;
			default:                      return false;
			}
		}

		bool anchorHolds(Anchor anchor, const Subject& subject, std::size_t pos) {
			const std::string& text = subject.text;
			switch (anchor) {
			case Anchor::LineStart:
				return pos == 0 || (subject.multiline && text[pos - 1] == '\n');
			case Anchor::LineEnd:
				return pos == text.size() || (subject.multiline && text[pos] == '\n');
			case Anchor::TextStart: return pos == 0;
			case Anchor::TextEnd:   return pos == text.size();
			default:                return wordAnchorHolds(anchor, text, pos);
			}
		}

	}  // namespace posixregex_detail

	using posixregex_detail::Anchor;
	using posixregex_detail::Fragment;
	using posixregex_detail::Inst;
	using posixregex_detail::Op;
	using posixregex_detail::Program;
	using posixregex_detail::Subject;

	static Inst programInst(Op op, int x = 0) {
		Inst inst;
		inst.op = op;
		inst.x = x;
		return inst;
	}

	static Inst anchorInst(Anchor anchor) {
		Inst inst = programInst(Op::Assert);
		inst.anchor = anchor;
		return inst;
	}

	static void wrapBody(const RegexOptions& options, Fragment& body) {
		if (options.wrap == RegexWrap::None) return;

		const bool word = options.wrap == RegexWrap::Word;
		const Inst before = anchorInst(word ? Anchor::NotAfterWord : Anchor::TextStart);
		body.code.insert(body.code.begin(), before);
		body.code.push_back(anchorInst(word ? Anchor::NotBeforeWord : Anchor::TextEnd));
	}

	static void unite(Fragment& all, const Fragment& next, bool first) {
		if (first) {
			all = next;
			return;
		}

		Fragment out;
		const int left = static_cast<int>(all.code.size());
		const int right = static_cast<int>(next.code.size());
		out.code.push_back(programInst(Op::Split, 1));
		out.code.back().y = left + 2;
		out.code.insert(out.code.end(), all.code.begin(), all.code.end());
		out.code.push_back(programInst(Op::Jump, right + 1));
		out.code.insert(out.code.end(), next.code.begin(), next.code.end());
		all.code.swap(out.code);
	}

	static void resolveTargets(Program& program) {
		for (std::size_t pc = 0; pc < program.code.size(); ++pc) {
			Inst& inst = program.code[pc];
			if (inst.op != Op::Split && inst.op != Op::Jump) continue;

			inst.x += static_cast<int>(pc);
			if (inst.op == Op::Split) inst.y += static_cast<int>(pc);
		}
	}

	static void markFirstCode(Program& program, char32_t code) {
		std::string bytes;
		appendUtf8(bytes, code);
		program.first_bytes[static_cast<unsigned char>(bytes[0])] = true;
	}

	static void markHighBytes(Program& program) {
		for (std::size_t byte = 0x80; byte < 0x100; ++byte) program.first_bytes[byte] = true;
	}

	// A case-insensitive letter can also begin with a non-ASCII character
	// that folds to it, such as the Kelvin sign to k.
	static void markFirstChar(Program& program, const Inst& inst) {
		markFirstCode(program, inst.c);
		if (inst.op != Op::CharFold) return;

		const auto upper = static_cast<char32_t>(std::toupper(static_cast<int>(inst.c & 0x7F)));
		if (inst.c < 0x80) markFirstCode(program, upper);
		markHighBytes(program);
	}

	static void markFirstSet(Program& program, const Inst& inst) {
		const posixregex_detail::CharSet& set = program.sets[static_cast<std::size_t>(inst.x)];
		for (std::size_t c = 0; c < 0x80; ++c) {
			if (set.ascii[c]) program.first_bytes[c] = true;
		}

		markHighBytes(program);
	}

	static void markFirst(Program& program, const Inst& inst) {
		switch (inst.op) {
		case Op::Char:
		case Op::CharFold: markFirstChar(program, inst); break;
		case Op::Set:      markFirstSet(program, inst);  break;
		case Op::Any:
			program.first_bytes.fill(true);
			break;
		default:
			program.can_match_empty = true;
			break;
		}
	}

	static void pushSuccessors(const Inst& inst, int pc, std::vector<int>& stack) {
		if (inst.op == Op::Jump) {
			stack.push_back(inst.x);
			return;
		}

		if (inst.op == Op::Split) stack.push_back(inst.y);
		stack.push_back(inst.op == Op::Split ? inst.x : pc + 1);
	}

	static bool isEpsilon(Op op) {
		return op == Op::Split || op == Op::Jump || op == Op::Save || op == Op::Assert
			|| op == Op::LoopMark || op == Op::LoopCheck;
	}

	static bool anchorsStart(const Program& program, const Inst& inst) {
		if (inst.op != Op::Assert) return false;
		if (inst.anchor == Anchor::TextStart) return true;
		return inst.anchor == Anchor::LineStart && !program.multiline;
	}

	// Visits every instruction reachable from the entry without consuming a
	// character; @p stop_at_start_anchor ends a path at ^ or \`.
	template <typename Visit>
	static void walkEntry(const Program& program, bool stop_at_start_anchor, Visit visit) {
		std::vector<bool> seen(program.code.size(), false);
		std::vector<int> stack(1, 0);
		while (!stack.empty()) {
			const int pc = stack.back();
			stack.pop_back();
			if (seen[static_cast<std::size_t>(pc)]) continue;
			seen[static_cast<std::size_t>(pc)] = true;

			const Inst& inst = program.code[static_cast<std::size_t>(pc)];
			if (stop_at_start_anchor && anchorsStart(program, inst)) continue;
			if (isEpsilon(inst.op)) pushSuccessors(inst, pc, stack);
			else                    visit(inst);
		}
	}

	static void analyzeEntry(Program& program) {
		bool anchored = true;
		walkEntry(program, true, [&anchored](const Inst&) { anchored = false; });
		program.start_anchored = anchored;
		walkEntry(program, false, [&program](const Inst& inst) { markFirst(program, inst); });
	}

	static void findSingleFirstByte(Program& program) {
		int count = 0;
		for (std::size_t byte = 0; byte < 0x100; ++byte) {
			if (!program.first_bytes[byte]) continue;
			++count;
			program.single_first_byte = static_cast<int>(byte);
		}

		if (count != 1 || program.single_first_byte >= 0x80) program.single_first_byte = -1;
	}

	static void finishProgram(Program& program, Fragment& body) {
		program.code.push_back(programInst(Op::Save, 0));
		program.code.insert(program.code.end(), body.code.begin(), body.code.end());
		program.code.push_back(programInst(Op::Save, 1));
		program.code.push_back(programInst(Op::Match));
		resolveTargets(program);
		analyzeEntry(program);
		findSingleFirstByte(program);
		if (!posixregex_detail::dfaUsable(program)) return;

		program.dfa = std::make_unique<posixregex_detail::DfaCache>();
	}

	static void addNeverMatching(Program& program, Fragment& body) {
		posixregex_detail::CharSet empty;
		posixregex_detail::finishCharSet(empty);
		program.sets.push_back(empty);
		body.code.push_back(programInst(Op::Set, static_cast<int>(program.sets.size()) - 1));
	}

	bool PosixRegex::compile(const std::string& pattern, const RegexOptions& options) {
		return compile(std::vector<std::string>(1, pattern), options);
	}

	static std::shared_ptr<Program> buildProgram(const std::vector<std::string>& patterns,
	                                             const RegexOptions& options, bool relax_backrefs,
	                                             std::string& error) {
		auto program = std::make_shared<Program>();
		program->multiline = options.multiline;
		Fragment all;
		for (std::size_t i = 0; i < patterns.size(); ++i) {
			Fragment piece;
			const int group_base = program->groups - 1;
			if (!posixregex_detail::parsePattern(patterns[i], options, group_base, relax_backrefs,
				    *program, piece, error)) {
				return nullptr;
			}
			unite(all, piece, i == 0);
		}

		wrapBody(options, all);
		if (patterns.empty()) addNeverMatching(*program, all);
		finishProgram(*program, all);
		return program;
	}

	bool PosixRegex::compile(const std::vector<std::string>& patterns,
	                         const RegexOptions& options) {
		std::shared_ptr<Program> program = buildProgram(patterns, options, false, error_);
		if (program == nullptr) return false;

		if (program->has_backrefs) program->relaxed = buildProgram(patterns, options, true, error_);
		program_ = program;
		error_.clear();
		return true;
	}

	std::size_t PosixRegex::groups() const {
		return program_ == nullptr ? 0 : static_cast<std::size_t>(program_->groups);
	}

	// A back-reference matches some string, so the pattern with each one
	// replaced by .* must match first; that linear check rejects most
	// subjects and finds where the backtracker has to start.
	static bool runBacktracker(const Program& program, const Subject& view, std::size_t from,
	                           bool anchored, std::vector<std::size_t>* spans) {
		std::vector<std::size_t> hint(2, std::string::npos);
		const Program& relaxed = *program.relaxed;
		if (!posixregex_detail::pikeSearch(relaxed, view, from, anchored, &hint)) return false;

		const std::size_t start = anchored ? from : hint[0];
		return posixregex_detail::backtrackSearch(program, view, start, anchored, spans);
	}

	static bool runSearch(const Program& program, const std::string& subject, std::size_t from,
	                      bool anchored, std::vector<std::size_t>* spans) {
		if (from > subject.size()) return false;

		const bool use_dfa = program.dfa != nullptr && !anchored;
		if (use_dfa && !posixregex_detail::dfaTest(program, subject, from)) return false;
		if (use_dfa && spans == nullptr) return true;

		const Subject view{ subject, program.multiline };
		if (program.has_backrefs) return runBacktracker(program, view, from, anchored, spans);
		return posixregex_detail::pikeSearch(program, view, from, anchored, spans);
	}

	bool PosixRegex::search(const std::string& subject, std::size_t from, RegexMatch& match) const {
		if (program_ == nullptr) return false;

		match.spans.assign(static_cast<std::size_t>(program_->groups) * 2, std::string::npos);
		return runSearch(*program_, subject, from, false, &match.spans);
	}

	bool PosixRegex::test(const std::string& subject) const {
		return program_ != nullptr && runSearch(*program_, subject, 0, false, nullptr);
	}

	bool PosixRegex::matchesWhole(const std::string& subject) const {
		if (program_ == nullptr) return false;

		std::vector<std::size_t> spans(2, std::string::npos);
		return runSearch(*program_, subject, 0, true, &spans) && spans[1] == subject.size();
	}

	bool RegexMatch::matched(std::size_t group) const {
		return group < groups() && start(group) != std::string::npos;
	}

	std::size_t RegexMatch::length(std::size_t group) const {
		return matched(group) ? end(group) - start(group) : 0;
	}

	std::string RegexMatch::str(const std::string& subject, std::size_t group) const {
		if (!matched(group)) return std::string();
		return subject.substr(start(group), length(group));
	}

}  // namespace wbsh

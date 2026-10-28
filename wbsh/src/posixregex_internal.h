#pragma once

/**
 * @file posixregex_internal.h
 * @brief The compiled form shared by the regex parser and its matchers.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "posixregex.h"
#include "utf8.h"

namespace wbsh::posixregex_detail {

	enum class Op : std::uint8_t {
		Char,
		CharFold,
		Any,
		Set,
		Split,
		Jump,
		Save,
		Assert,
		BackRef,
		LoopMark,
		LoopCheck,
		Match,
	};

	enum class Anchor : std::uint8_t {
		LineStart,
		LineEnd,
		TextStart,
		TextEnd,
		WordBoundary,
		NotWordBoundary,
		WordStart,
		WordEnd,
		NotAfterWord,
		NotBeforeWord,
	};

	/// Split prefers x over y. Jump and Split targets are relative offsets
	/// while a fragment is built and absolute in a Program. A BackRef with a
	/// non-zero y compares case-insensitively.
	struct Inst {
		Op op = Op::Match;
		Anchor anchor = Anchor::TextStart;
		int x = 0;
		int y = 0;
		char32_t c = 0;
	};

	struct CharSet {
		std::array<bool, 128> ascii{};
		std::vector<std::pair<char32_t, char32_t>> ranges;
		std::vector<std::string> classes;
		bool negated = false;
		bool ignore_case = false;
	};

	struct DfaState {
		std::vector<int> entry;
		std::vector<int> threads;
		std::array<int, 128> next{};
		std::unordered_map<char32_t, int> wide;
		bool initial = false;
		bool accepting = false;
		int accepting_at_end = -1;
	};

	struct DfaCache {
		std::vector<DfaState> states;
		std::map<std::pair<bool, std::vector<int>>, int> index;
		int initial = -1;
		int idle = -1;
	};

	struct Program {
		std::vector<Inst> code;
		std::vector<CharSet> sets;
		int groups = 1;
		int loops = 0;
		bool multiline = false;
		bool has_backrefs = false;
		bool start_anchored = false;
		bool can_match_empty = false;
		std::array<bool, 256> first_bytes{};
		int single_first_byte = -1;
		/// With back-references: the same pattern with each one read as .*
		std::shared_ptr<const Program> relaxed;
		/// Filled in while matching, so a Program serves one thread at a time;
		/// null when an assertion needs more context than the DFA tracks.
		std::unique_ptr<DfaCache> dfa;
	};

	struct Fragment {
		std::vector<Inst> code;
	};

	struct Subject {
		const std::string& text;
		bool multiline;
	};

	/// Parses one pattern into @p out; group numbers start after @p group_base.
	bool parsePattern(const std::string& pattern, const RegexOptions& options, int group_base,
	                  bool relax_backrefs, Program& program, Fragment& out, std::string& error);

	inline Utf8Char decodeChar(const std::string& text, std::size_t pos) {
		const auto byte = static_cast<unsigned char>(text[pos]);
		if (byte < 0x80) return { byte, 1 };
		return decodeUtf8(text, pos);
	}

	bool charSetMatches(const CharSet& set, char32_t code);
	void finishCharSet(CharSet& set);
	char32_t foldCase(char32_t code);
	bool isWordCode(char32_t code);

	bool anchorHolds(Anchor anchor, const Subject& subject, std::size_t pos);
	bool consumes(const Program& program, const Inst& inst, char32_t code);

	bool pikeSearch(const Program& program, const Subject& subject, std::size_t from,
	                bool anchored, std::vector<std::size_t>* spans);
	bool backtrackSearch(const Program& program, const Subject& subject, std::size_t from,
	                     bool anchored, std::vector<std::size_t>* spans);

	/// The next position at or after @p pos where a match could start.
	std::size_t nextCandidate(const Program& program, const std::string& text, std::size_t pos);

	bool dfaUsable(const Program& program);

	/// Whether any match starts at or after @p from; needs program.dfa.
	bool dfaTest(const Program& program, const std::string& text, std::size_t from);

}  // namespace wbsh::posixregex_detail

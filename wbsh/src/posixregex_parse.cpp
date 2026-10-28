/**
 * @file posixregex_parse.cpp
 * @brief POSIX BRE / ERE / awk pattern parser producing Pike VM fragments.
 */

#include "posixregex_internal.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "utf8.h"

namespace wbsh::posixregex_detail {

	static const int kDupMax = 32767;
	static const int kUnbounded = -1;
	static const std::size_t kMaxProgram = 4u << 20;
	static const int kMaxDepth = 1000;

	static const char* const kUnmatchedBracket = "Unmatched [, [^, [:, [., or [=";
	static const char* const kUnmatchedParen = "Unmatched ( or \\(";
	static const char* const kTooBig = "Regular expression too big";

	struct Parser {
		const std::string& pattern;
		const RegexOptions& options;
		Program& program;
		int group_base = 0;
		bool relax_backrefs = false;
		std::size_t at = 0;
		int opened = 0;
		std::vector<bool> closed;
		int depth = 0;
		std::string error;
	};

	struct Interval {
		int min = 0;
		int max = kUnbounded;
	};

	static bool fail(Parser& p, const char* message) {
		if (p.error.empty()) p.error = message;
		return false;
	}

	static bool basic(const Parser& p) {
		return p.options.syntax == RegexSyntax::Basic;
	}

	static bool awk(const Parser& p) {
		return p.options.syntax == RegexSyntax::Awk;
	}

	static bool atEnd(const Parser& p) {
		return p.at >= p.pattern.size();
	}

	static bool lookingAt(const Parser& p, const char* text) {
		return p.pattern.compare(p.at, std::strlen(text), text) == 0;
	}

	static bool atOperator(const Parser& p, char c) {
		if (basic(p)) return p.at + 1 < p.pattern.size() && p.pattern[p.at] == '\\'
			&& p.pattern[p.at + 1] == c;
		return !atEnd(p) && p.pattern[p.at] == c;
	}

	static void skipOperator(Parser& p) {
		p.at += basic(p) ? 2 : 1;
	}

	static bool atAlternation(const Parser& p) {
		return atOperator(p, '|');
	}

	static bool atGroupClose(const Parser& p) {
		return atOperator(p, ')') && (basic(p) || p.depth > 0);
	}

	static Inst makeInst(Op op, int x = 0, int y = 0) {
		Inst inst;
		inst.op = op;
		inst.x = x;
		inst.y = y;
		return inst;
	}

	static Inst assertInst(Anchor anchor) {
		Inst inst = makeInst(Op::Assert);
		inst.anchor = anchor;
		return inst;
	}

	static Fragment single(const Inst& inst) {
		Fragment out;
		out.code.push_back(inst);
		return out;
	}

	static Fragment literal(const Parser& p, char32_t code) {
		Inst inst = makeInst(p.options.ignore_case ? Op::CharFold : Op::Char);
		inst.c = p.options.ignore_case ? foldCase(code) : code;
		return single(inst);
	}

	static int length(const Fragment& fragment) {
		return static_cast<int>(fragment.code.size());
	}

	static void append(Fragment& out, const Fragment& tail) {
		out.code.insert(out.code.end(), tail.code.begin(), tail.code.end());
	}

	static Fragment alternate(const Fragment& left, const Fragment& right) {
		Fragment out = single(makeInst(Op::Split, 1, length(left) + 2));
		append(out, left);
		out.code.push_back(makeInst(Op::Jump, length(right) + 1));
		append(out, right);
		return out;
	}

	static Fragment optional(const Fragment& body) {
		Fragment out = single(makeInst(Op::Split, 1, length(body) + 1));
		append(out, body);
		return out;
	}

	// The mark / check pair stops an iteration that consumed nothing from
	// looping forever in the backtracker; the Pike VM treats both as no-ops.
	static Fragment star(Parser& p, const Fragment& body) {
		const int loop = p.program.loops++;
		Fragment out = single(makeInst(Op::Split, 1, length(body) + 4));
		out.code.push_back(makeInst(Op::LoopMark, loop));
		append(out, body);
		out.code.push_back(makeInst(Op::LoopCheck, loop));
		out.code.push_back(makeInst(Op::Jump, -(length(body) + 3)));
		return out;
	}

	// Each optional copy skips straight to the end, which matches the same
	// strings as nesting them and keeps construction linear.
	static void appendOptionalCopies(Fragment& out, const Fragment& atom, int copies) {
		const int stride = length(atom) + 1;
		for (int i = 0; i < copies && out.code.size() <= kMaxProgram; ++i) {
			out.code.push_back(makeInst(Op::Split, 1, (copies - i) * stride));
			append(out, atom);
		}
	}

	static bool repeat(Parser& p, Fragment& atom, Interval interval) {
		const bool unbounded = interval.max == kUnbounded;
		const int optional_copies = unbounded ? 1 : interval.max - interval.min;
		const auto copies = static_cast<std::size_t>(interval.min + optional_copies);
		if (copies * (atom.code.size() + 5) > kMaxProgram) return fail(p, kTooBig);

		Fragment out;
		for (int i = 0; i < interval.min; ++i) append(out, atom);

		if (unbounded) append(out, star(p, atom));
		else           appendOptionalCopies(out, atom, optional_copies);

		atom = out;
		return true;
	}

	static bool readCount(Parser& p, int& value) {
		const std::size_t begin = p.at;
		long long count = 0;
		while (!atEnd(p) && std::isdigit(static_cast<unsigned char>(p.pattern[p.at])) != 0) {
			count = (std::min)(count * 10 + (p.pattern[p.at] - '0'), 1LL << 40);
			++p.at;
		}

		if (p.at == begin) return false;
		if (count > kDupMax) return fail(p, kTooBig);
		value = static_cast<int>(count);
		return true;
	}

	static bool parseIntervalBody(Parser& p, Interval& interval) {
		const bool has_min = readCount(p, interval.min);
		if (!p.error.empty()) return false;
		interval.max = interval.min;
		if (!atEnd(p) && p.pattern[p.at] == ',') {
			++p.at;
			interval.max = kUnbounded;
			if (!readCount(p, interval.max) && !p.error.empty()) return false;
		} else if (!has_min) {
			return fail(p, "Invalid content of \\{\\}");
		}

		if (interval.max != kUnbounded && interval.max < interval.min) {
			return fail(p, "Invalid content of \\{\\}");
		}

		return true;
	}

	static bool parseInterval(Parser& p, Interval& interval) {
		skipOperator(p);
		if (!parseIntervalBody(p, interval)) return false;
		if (!atOperator(p, '}')) return fail(p, basic(p) ? "Unmatched \\{" : "Unmatched {");

		skipOperator(p);
		return true;
	}

	static bool isOneOf(const char* set, char c) {
		return c != '\0' && std::strchr(set, c) != nullptr;
	}

	static bool quantifierAhead(const Parser& p, char& kind) {
		if (atEnd(p)) return false;

		const char c = p.pattern[p.at];
		if (c == '*') {
			kind = c;
			return true;
		}

		if (!basic(p)) {
			kind = c;
			return isOneOf("+?{", c);
		}

		const bool escaped = c == '\\' && p.at + 1 < p.pattern.size();
		kind = escaped ? p.pattern[p.at + 1] : c;
		return escaped && isOneOf("+?{", kind);
	}

	static bool applyQuantifier(Parser& p, Fragment& atom, char kind) {
		Interval interval;
		switch (kind) {
		case '*':
			++p.at;
			atom = star(p, atom);
			return true;
		case '+':
			skipOperator(p);
			interval.min = 1;
			return repeat(p, atom, interval);
		case '?':
			skipOperator(p);
			atom = optional(atom);
			return true;
		case '{':
			return parseInterval(p, interval) && repeat(p, atom, interval);
		default:
			return true;
		}
	}

	static bool parseQuantifiers(Parser& p, Fragment& atom) {
		char kind = 0;
		while (quantifierAhead(p, kind)) {
			if (!applyQuantifier(p, atom, kind)) return false;
		}

		return true;
	}

	static bool parseAlternation(Parser& p, Fragment& out);

	static bool parseGroup(Parser& p, Fragment& out) {
		if (++p.depth > kMaxDepth) return fail(p, kTooBig);

		skipOperator(p);
		const int local = ++p.opened;
		p.closed.resize(static_cast<std::size_t>(local) + 1, false);
		Fragment inner;
		if (!parseAlternation(p, inner)) return false;
		if (!atGroupClose(p)) return fail(p, kUnmatchedParen);

		skipOperator(p);
		--p.depth;
		p.closed[static_cast<std::size_t>(local)] = true;

		const int group = p.group_base + local;
		p.program.groups = (std::max)(p.program.groups, group + 1);
		out = single(makeInst(Op::Save, group * 2));
		append(out, inner);
		out.code.push_back(makeInst(Op::Save, group * 2 + 1));
		return true;
	}

	static void addSet(Parser& p, CharSet& set, Fragment& out) {
		finishCharSet(set);
		p.program.sets.push_back(set);
		out = single(makeInst(Op::Set, static_cast<int>(p.program.sets.size()) - 1));
	}

	static void addClassSet(Parser& p, const char* name, bool negated, Fragment& out) {
		CharSet set;
		set.classes.push_back(name);
		if (std::strcmp(name, "alnum") == 0) set.ranges.emplace_back(U'_', U'_');
		set.negated = negated;
		addSet(p, set, out);
	}

	static char32_t decodeAt(Parser& p) {
		const Utf8Char decoded = decodeUtf8(p.pattern, p.at);
		p.at += decoded.length;
		return decoded.code;
	}

	static bool octalEscape(Parser& p, char32_t& code) {
		std::size_t digits = 0;
		code = 0;
		while (digits < 3 && !atEnd(p) && p.pattern[p.at] >= '0' && p.pattern[p.at] <= '7') {
			code = code * 8 + static_cast<char32_t>(p.pattern[p.at] - '0');
			++p.at;
			++digits;
		}

		return digits > 0;
	}

	static bool controlEscape(const Parser& p, char c, char32_t& code) {
		switch (c) {
		case 'n': code = U'\n';                    return true;
		case 't': code = U'\t';                    return true;
		case 'r': code = U'\r';                    return awk(p);
		case 'f': code = U'\f';                    return awk(p);
		case 'v': code = U'\v';                    return awk(p);
		case 'a': code = U'\a';                    return awk(p);
		case 'b': code = U'\b';                    return awk(p);
		default:                                   return false;
		}
	}

	// A numeric escape names a byte; one past ASCII must match that raw
	// byte, which decodes as a stray byte rather than as a code point.
	static char32_t rawByteCode(char32_t byte) {
		return byte < 0x80 ? byte : decodeUtf8(std::string(1, static_cast<char>(byte)), 0).code;
	}

	static bool hexEscape(Parser& p, char32_t& code) {
		if (p.pattern[p.at] != 'x') return false;

		std::size_t digits = 0;
		code = 0;
		while (digits < 2 && p.at + 1 + digits < p.pattern.size()) {
			const char c = p.pattern[p.at + 1 + digits];
			if (std::isxdigit(static_cast<unsigned char>(c)) == 0) break;
			const int value = std::isdigit(static_cast<unsigned char>(c)) != 0 ? c - '0'
				: std::tolower(static_cast<unsigned char>(c)) - 'a' + 10;
			code = code * 16 + static_cast<char32_t>(value);
			++digits;
		}

		if (digits == 0) return false;
		p.at += 1 + digits;
		return true;
	}

	// The escaped character after a backslash inside or outside brackets in
	// awk, and outside brackets in BRE / ERE when it is not an operator.
	static char32_t escapedCode(Parser& p) {
		const char c = p.pattern[p.at];
		char32_t code = 0;
		if (awk(p) && (octalEscape(p, code) || hexEscape(p, code))) return rawByteCode(code & 0xFF);
		if (controlEscape(p, c, code)) {
			++p.at;
			return code;
		}

		return decodeAt(p);
	}

	static bool anchorEscape(const Parser& p, char c, Anchor& anchor) {
		switch (c) {
		case 'b':
			anchor = Anchor::WordBoundary;
			return !awk(p);
		case 'y':
			anchor = Anchor::WordBoundary;
			return awk(p);
		case 'B':  anchor = Anchor::NotWordBoundary;   return true;
		case '<':  anchor = Anchor::WordStart;         return true;
		case '>':  anchor = Anchor::WordEnd;           return true;
		case '`':  anchor = Anchor::TextStart;         return true;
		case '\'': anchor = Anchor::TextEnd;           return true;
		default:                                       return false;
		}
	}

	static bool classEscape(Parser& p, char c, Fragment& out) {
		switch (c) {
		case 'w': addClassSet(p, "alnum", false, out); return true;
		case 'W': addClassSet(p, "alnum", true, out);  return true;
		case 's': addClassSet(p, "space", false, out); return true;
		case 'S': addClassSet(p, "space", true, out);  return true;
		default:                                       return false;
		}
	}

	static bool backReference(Parser& p, char c, Fragment& out) {
		const int local = c - '0';
		const bool known = local <= p.opened && p.closed[static_cast<std::size_t>(local)];
		if (!known) return fail(p, "Invalid back reference");

		++p.at;
		if (p.relax_backrefs) {
			out = star(p, single(makeInst(Op::Any)));
			return true;
		}

		p.program.has_backrefs = true;
		out = single(makeInst(Op::BackRef, p.group_base + local, p.options.ignore_case ? 1 : 0));
		return true;
	}

	static bool parseEscape(Parser& p, Fragment& out) {
		if (p.at + 1 >= p.pattern.size()) return fail(p, "Trailing backslash");

		const char c = p.pattern[p.at + 1];
		if (basic(p) && c == '(') return parseGroup(p, out);
		if (basic(p) && c == '{') return fail(p, "Invalid preceding regular expression");

		++p.at;
		Anchor anchor = Anchor::TextStart;
		if (!awk(p) && c >= '1' && c <= '9') return backReference(p, c, out);
		if (anchorEscape(p, c, anchor)) {
			++p.at;
			out = single(assertInst(anchor));
			return true;
		}

		if (classEscape(p, c, out)) {
			++p.at;
			return true;
		}

		out = literal(p, escapedCode(p));
		return true;
	}

	static bool parseAtom(Parser& p, bool branch_start, Fragment& out);

	static bool parseBranch(Parser& p, Fragment& out) {
		bool branch_start = true;
		while (!atEnd(p) && !atAlternation(p) && !atGroupClose(p)) {
			const std::size_t begin = p.at;
			Fragment atom;
			if (!parseAtom(p, branch_start, atom)) return false;

			const bool leading_anchor = basic(p) && branch_start && p.pattern[begin] == '^';
			if (!leading_anchor && !parseQuantifiers(p, atom)) return false;
			if (out.code.size() + atom.code.size() > kMaxProgram) return fail(p, kTooBig);

			append(out, atom);
			branch_start = leading_anchor;
		}

		return true;
	}

	static bool parseAlternation(Parser& p, Fragment& out) {
		if (!parseBranch(p, out)) return false;

		while (atAlternation(p)) {
			skipOperator(p);
			Fragment right;
			if (!parseBranch(p, right)) return false;
			out = alternate(out, right);
		}

		return true;
	}

	static bool breDollarIsAnchor(const Parser& p) {
		const std::size_t next = p.at + 1;
		if (next >= p.pattern.size()) return true;
		return p.pattern[next] == '\\' && next + 1 < p.pattern.size()
			&& (p.pattern[next + 1] == ')' || p.pattern[next + 1] == '|');
	}

	static bool parseAnchorChar(Parser& p, char c, bool branch_start, Fragment& out) {
		const bool bre_anchor = c == '^' ? branch_start : breDollarIsAnchor(p);
		const bool anchor = !basic(p) || bre_anchor;
		++p.at;
		out = anchor ? single(assertInst(c == '^' ? Anchor::LineStart : Anchor::LineEnd))
			: literal(p, static_cast<char32_t>(c));
		return true;
	}

	static bool parseBracket(Parser& p, Fragment& out);

	static bool parseAtom(Parser& p, bool branch_start, Fragment& out) {
		const char c = p.pattern[p.at];
		if (c == '[') return parseBracket(p, out);
		if (c == '\\') return parseEscape(p, out);
		if (c == '^' || c == '$') return parseAnchorChar(p, c, branch_start, out);
		if (!basic(p) && c == '(') return parseGroup(p, out);

		if (c == '.') {
			++p.at;
			out = single(makeInst(Op::Any));
			return true;
		}

		const bool leading_quantifier = !basic(p) && branch_start && isOneOf("*+?", c);
		if (leading_quantifier) {
			++p.at;
			return true;
		}

		out = literal(p, decodeAt(p));
		return true;
	}

	static bool readBracketName(Parser& p, char kind, std::string& name) {
		const char close[] = { kind, ']', '\0' };
		const std::size_t end = p.pattern.find(close, p.at + 2);
		if (end == std::string::npos) return fail(p, kUnmatchedBracket);

		name = p.pattern.substr(p.at + 2, end - p.at - 2);
		p.at = end + 2;
		return true;
	}

	static bool bracketCode(Parser& p, char32_t& code) {
		const bool named = lookingAt(p, "[=") || lookingAt(p, "[.");
		if (named) {
			std::string name;
			if (!readBracketName(p, p.pattern[p.at + 1], name)) return false;
			const Utf8Char decoded = name.empty() ? Utf8Char{ 0, 0 } : decodeUtf8(name, 0);
			if (decoded.length == 0 || decoded.length != name.size()) {
				return fail(p, "Invalid collation character");
			}

			code = decoded.code;
			return true;
		}

		if (awk(p) && p.pattern[p.at] == '\\' && p.at + 1 < p.pattern.size()) {
			++p.at;
			code = escapedCode(p);
			return true;
		}

		code = decodeAt(p);
		return true;
	}

	static bool parseBracketClass(Parser& p, CharSet& set) {
		std::string name;
		if (!readBracketName(p, ':', name)) return false;
		if (!isKnownCharClass(name)) return fail(p, "Invalid character class name");

		set.classes.push_back(name);
		return true;
	}

	static bool parseBracketItem(Parser& p, CharSet& set) {
		if (lookingAt(p, "[:")) return parseBracketClass(p, set);

		char32_t low = 0;
		if (!bracketCode(p, low)) return false;

		const bool range = p.at + 1 < p.pattern.size() && p.pattern[p.at] == '-'
			&& p.pattern[p.at + 1] != ']';
		if (!range) {
			set.ranges.emplace_back(low, low);
			return true;
		}

		++p.at;
		char32_t high = 0;
		if (lookingAt(p, "[:") || !bracketCode(p, high) || high < low) {
			return fail(p, "Invalid range end");
		}

		set.ranges.emplace_back(low, high);
		return true;
	}

	static bool parseBracket(Parser& p, Fragment& out) {
		++p.at;
		CharSet set;
		set.ignore_case = p.options.ignore_case;
		if (!atEnd(p) && p.pattern[p.at] == '^') {
			set.negated = true;
			++p.at;
		}

		for (bool first = true;; first = false) {
			if (atEnd(p)) return fail(p, kUnmatchedBracket);
			if (p.pattern[p.at] == ']' && !first) break;
			if (!parseBracketItem(p, set)) return false;
		}

		++p.at;
		addSet(p, set, out);
		return true;
	}

	bool parsePattern(const std::string& pattern, const RegexOptions& options, int group_base,
	                  bool relax_backrefs, Program& program, Fragment& out, std::string& error) {
		Parser p{ pattern, options, program, group_base, relax_backrefs };
		p.closed.push_back(true);
		const bool parsed = parseAlternation(p, out);
		if (parsed && !atEnd(p)) fail(p, "Unmatched ) or \\)");

		error = p.error;
		return error.empty();
	}

}  // namespace wbsh::posixregex_detail

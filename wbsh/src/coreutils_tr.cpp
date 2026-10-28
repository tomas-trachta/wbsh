/**
 * @file coreutils_tr.cpp
 * @brief tr: GNU set syntax (escapes, ranges, [:class:], [=c=], [c*n]),
 *        translate / delete / squeeze, and chunked byte I/O.
 */

#include "coreutils_internal.h"

#include <cstdio>
#include <string>
#include <vector>

#include "executor.h"
#include "optscan.h"

namespace wbsh {

	namespace tr_detail {

		enum class Kind { Chars, Class, Equiv, Repeat };

		struct Element {
			Kind kind = Kind::Chars;
			std::string chars;
			std::string class_name;
			bool fill = false;
		};

		struct Set {
			std::vector<Element> elements;
		};

		struct Options {
			bool delete_mode = false;
			bool squeeze = false;
			bool complement = false;
			bool truncate_set1 = false;
			std::vector<std::string> sets;
		};

		struct Tables {
			bool remove[256] = {};
			bool squeeze[256] = {};
			unsigned char map[256] = {};
		};

		struct ClassDef {
			const char* name;
			bool (*member)(int c);
		};

		enum OptionId { kComplement, kDelete, kSqueeze, kTruncate };

		static const std::size_t kChunk = 64 * 1024;
		static const int kOctalDigits = 3;

	}  // namespace tr_detail

	using tr_detail::Element;
	using tr_detail::Kind;
	using tr_detail::Set;
	using tr_detail::Tables;
	using TrOptions = tr_detail::Options;

	static bool trFail(const std::string& message) {
		perr("tr", message);
		return false;
	}

	static bool isUpper(int c) { return c >= 'A' && c <= 'Z'; }
	static bool isLower(int c) { return c >= 'a' && c <= 'z'; }
	static bool isDigit(int c) { return c >= '0' && c <= '9'; }
	static bool isAlpha(int c) { return isUpper(c) || isLower(c); }
	static bool isAlnum(int c) { return isAlpha(c) || isDigit(c); }
	static bool isBlank(int c) { return c == ' ' || c == '\t'; }
	static bool isCntrl(int c) { return c < 0x20 || c == 0x7F; }
	static bool isGraph(int c) { return c > 0x20 && c < 0x7F; }
	static bool isPrint(int c) { return c >= 0x20 && c < 0x7F; }
	static bool isPunct(int c) { return isGraph(c) && !isAlnum(c); }
	static bool isSpace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

	static bool isXdigit(int c) {
		return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
	}

	static const tr_detail::ClassDef kTrClasses[] = {
		{ "alnum", isAlnum }, { "alpha", isAlpha }, { "blank", isBlank },
		{ "cntrl", isCntrl }, { "digit", isDigit }, { "graph", isGraph },
		{ "lower", isLower }, { "print", isPrint }, { "punct", isPunct },
		{ "space", isSpace }, { "upper", isUpper }, { "xdigit", isXdigit },
	};

	static const tr_detail::ClassDef* findTrClass(const std::string& name) {
		for (const auto& def : kTrClasses) {
			if (name == def.name) return &def;
		}

		return nullptr;
	}

	static std::string expandTrClass(const tr_detail::ClassDef& def) {
		std::string chars;
		for (int c = 0; c < 256; ++c) {
			if (def.member(c)) chars.push_back(static_cast<char>(c));
		}

		return chars;
	}

	static char trEscapeLetter(char letter) {
		switch (letter) {
		case 'a':
			return '\a';
		case 'b':
			return '\b';
		case 'f':
			return '\f';
		case 'n':
			return '\n';
		case 'r':
			return '\r';
		case 't':
			return '\t';
		case 'v':
			return '\v';
		default:
			return letter;
		}
	}

	static bool isOctalDigit(char c) {
		return c >= '0' && c <= '7';
	}

	// GNU stops an octal escape before a digit that would push it past 0377.
	static unsigned char readTrOctal(const std::string& text, std::size_t& i) {
		int value = 0;
		for (int digits = 0; digits < tr_detail::kOctalDigits && i < text.size(); ++digits) {
			if (!isOctalDigit(text[i])) break;

			const int next = value * 8 + (text[i] - '0');
			if (next > 0xFF) break;

			value = next;
			++i;
		}

		return static_cast<unsigned char>(value);
	}

	static unsigned char readTrChar(const std::string& text, std::size_t& i) {
		if (text[i] != '\\' || i + 1 >= text.size()) return static_cast<unsigned char>(text[i++]);

		++i;
		if (isOctalDigit(text[i])) return readTrOctal(text, i);
		return static_cast<unsigned char>(trEscapeLetter(text[i++]));
	}

	static void appendTrChars(Set& set, const std::string& chars) {
		if (set.elements.empty() || set.elements.back().kind != Kind::Chars) {
			set.elements.push_back(Element{});
		}

		set.elements.back().chars += chars;
	}

	static bool parseTrClass(const std::string& text, std::size_t& i, Set& set, bool& matched) {
		const std::size_t close = text.find(":]", i + 2);
		matched = text.compare(i, 2, "[:") == 0 && close != std::string::npos;
		if (!matched) return true;

		const std::string name = text.substr(i + 2, close - i - 2);
		const tr_detail::ClassDef* def = findTrClass(name);
		if (def == nullptr) return trFail("invalid character class '" + name + "'");

		Element element;
		element.kind = Kind::Class;
		element.class_name = name;
		element.chars = expandTrClass(*def);
		set.elements.push_back(element);
		i = close + 2;
		return true;
	}

	static bool parseTrEquiv(const std::string& text, std::size_t& i, Set& set) {
		if (text.compare(i, 2, "[=") != 0 || i + 2 >= text.size()) return false;

		std::size_t at = i + 2;
		const unsigned char c = readTrChar(text, at);
		if (text.compare(at, 2, "=]") != 0) return false;

		Element element;
		element.kind = Kind::Equiv;
		element.chars.assign(1, static_cast<char>(c));
		set.elements.push_back(element);
		i = at + 2;
		return true;
	}

	static bool parseTrRepeatCount(const std::string& digits, std::size_t& count) {
		count = 0;
		const int base = (!digits.empty() && digits[0] == '0') ? 8 : 10;
		for (char d : digits) {
			const int value = d - '0';
			if (value < 0 || value >= base) return false;
			count = count * base + static_cast<std::size_t>(value);
		}

		return true;
	}

	static bool parseTrRepeat(const std::string& text, std::size_t& i, Set& set, bool& matched) {
		matched = false;
		if (i + 1 >= text.size()) return true;

		std::size_t at = i + 1;
		const unsigned char c = readTrChar(text, at);
		const std::size_t close = text.find(']', at);
		if (at >= text.size() || text[at] != '*' || close == std::string::npos) return true;

		matched = true;
		const std::string digits = text.substr(at + 1, close - at - 1);
		std::size_t count = 0;
		if (!parseTrRepeatCount(digits, count)) {
			return trFail("invalid repeat count '" + digits + "' in [c*n] construct");
		}

		Element element;
		element.kind = Kind::Repeat;
		element.fill = count == 0;
		element.chars.assign(count, static_cast<char>(c));
		element.class_name.assign(1, static_cast<char>(c));
		set.elements.push_back(element);
		i = close + 1;
		return true;
	}

	static bool parseTrBracket(const std::string& text, std::size_t& i, Set& set, bool& matched) {
		if (!parseTrClass(text, i, set, matched)) return false;
		if (matched) return true;

		matched = parseTrEquiv(text, i, set);
		if (matched) return true;

		return parseTrRepeat(text, i, set, matched);
	}

	static bool parseTrRange(const std::string& text, std::size_t& i, unsigned char low, Set& set) {
		++i;
		const unsigned char high = readTrChar(text, i);
		if (high < low) {
			const std::string range = std::string(1, static_cast<char>(low)) + "-"
				+ std::string(1, static_cast<char>(high));
			return trFail("range-endpoints of '" + range
				+ "' are in reverse collating sequence order");
		}

		std::string chars;
		for (int c = low; c <= high; ++c) chars.push_back(static_cast<char>(c));
		appendTrChars(set, chars);
		return true;
	}

	static bool parseTrSet(const std::string& text, Set& set) {
		std::size_t i = 0;
		while (i < text.size()) {
			bool matched = false;
			if (text[i] == '[' && !parseTrBracket(text, i, set, matched)) return false;
			if (matched) continue;

			const unsigned char c = readTrChar(text, i);
			const bool is_range = i + 1 < text.size() && text[i] == '-';
			if (is_range && !parseTrRange(text, i, c, set)) return false;
			if (!is_range) appendTrChars(set, std::string(1, static_cast<char>(c)));
		}

		return true;
	}

	static std::string flattenTrSet(const Set& set) {
		std::string chars;
		for (const Element& element : set.elements) chars += element.chars;
		return chars;
	}

	static std::string complementTrChars(const std::string& chars) {
		bool present[256] = {};
		for (unsigned char c : chars) present[c] = true;

		std::string out;
		for (int c = 0; c < 256; ++c) {
			if (!present[c]) out.push_back(static_cast<char>(c));
		}

		return out;
	}

	static bool hasTrFill(const Set& set) {
		for (const Element& element : set.elements) {
			if (element.fill) return true;
		}

		return false;
	}

	static bool isCaseClass(const Element& element) {
		return element.kind == Kind::Class
			&& (element.class_name == "upper" || element.class_name == "lower");
	}

	static bool checkTrSet2Constructs(const Set& set2) {
		int fills = 0;
		for (const Element& element : set2.elements) {
			if (element.kind == Kind::Class && !isCaseClass(element)) {
				return trFail("when translating, the only character classes that may appear"
					" in string2 are 'upper' and 'lower'");
			}

			if (element.kind == Kind::Equiv) {
				return trFail("[=c=] expressions may not appear in string2 when translating");
			}

			if (element.fill) ++fills;
		}

		if (fills > 1) return trFail("only one [c*] repeat construct may appear in string2");
		return true;
	}

	static std::string expandTrSet2(Set& set2, std::size_t set1_length) {
		const std::size_t fixed = flattenTrSet(set2).size();
		for (Element& element : set2.elements) {
			if (!element.fill) continue;

			const std::size_t count = set1_length > fixed ? set1_length - fixed : 0;
			element.chars.assign(count, element.class_name[0]);
		}

		return flattenTrSet(set2);
	}

	static bool caseClassStartsAt(const Set& set, std::size_t index) {
		std::size_t at = 0;
		for (const Element& element : set.elements) {
			if (at == index && isCaseClass(element)) return true;
			at += element.chars.size();
		}

		return false;
	}

	// [:upper:] / [:lower:] in string2 only make sense opposite a case class
	// at the same position of string1.
	static bool checkTrCaseAlignment(const Set& set1, const Set& set2, bool complement) {
		std::size_t at = 0;
		for (const Element& element : set2.elements) {
			if (isCaseClass(element) && (complement || !caseClassStartsAt(set1, at))) {
				return trFail("misaligned [:upper:] and/or [:lower:] construct");
			}

			at += element.chars.size();
		}

		return true;
	}

	static bool buildTrSet2(const TrOptions& options, const Set& set1, std::string& chars1,
			Set& set2, std::string& chars2) {
		if (!checkTrSet2Constructs(set2)) return false;

		chars2 = expandTrSet2(set2, chars1.size());
		if (!checkTrCaseAlignment(set1, set2, options.complement)) return false;

		if (chars2.empty() && !options.truncate_set1) {
			return trFail("when not truncating set1, string2 must be non-empty");
		}

		if (options.truncate_set1 && chars1.size() > chars2.size()) chars1.resize(chars2.size());
		if (chars2.size() < chars1.size()) {
			chars2.append(chars1.size() - chars2.size(), chars2.back());
		}
		return true;
	}

	static void fillTrMembership(bool (&table)[256], const std::string& chars) {
		for (unsigned char c : chars) table[c] = true;
	}

	static void fillTrMap(Tables& tables, const std::string& from, const std::string& to) {
		for (int c = 0; c < 256; ++c) tables.map[c] = static_cast<unsigned char>(c);
		for (std::size_t k = 0; k < from.size() && k < to.size(); ++k) {
			tables.map[static_cast<unsigned char>(from[k])] = static_cast<unsigned char>(to[k]);
		}
	}

	static bool expandTrSet1(const TrOptions& options, Set& set1, std::string& chars1) {
		if (!parseTrSet(options.sets[0], set1)) return false;
		if (hasTrFill(set1)) return trFail("the [c*] repeat construct may not appear in string1");

		chars1 = flattenTrSet(set1);
		if (options.complement) chars1 = complementTrChars(chars1);
		return true;
	}

	static bool buildTrTables(const TrOptions& options, Tables& tables) {
		Set set1;
		std::string chars1;
		if (!expandTrSet1(options, set1, chars1)) return false;

		Set set2;
		std::string chars2;
		if (options.sets.size() > 1 && !parseTrSet(options.sets[1], set2)) return false;

		const bool translating = !options.delete_mode && options.sets.size() > 1;
		if (translating && !buildTrSet2(options, set1, chars1, set2, chars2)) return false;
		if (!translating) chars2 = flattenTrSet(set2);

		fillTrMap(tables, translating ? chars1 : std::string(), chars2);
		if (options.delete_mode) fillTrMembership(tables.remove, chars1);
		if (options.squeeze) {
			fillTrMembership(tables.squeeze, options.sets.size() > 1 ? chars2 : chars1);
		}
		return true;
	}

	static const std::vector<OptDef> kTrOptionDefs = {
		{ tr_detail::kComplement, 'c', "complement",      OptArg::None },
		{ tr_detail::kComplement, 'C', nullptr,           OptArg::None },
		{ tr_detail::kDelete,     'd', "delete",          OptArg::None },
		{ tr_detail::kSqueeze,    's', "squeeze-repeats", OptArg::None },
		{ tr_detail::kTruncate,   't', "truncate-set1",   OptArg::None },
	};

	static void applyTrHit(const OptHit& hit, TrOptions& options) {
		switch (hit.id) {
		case tr_detail::kComplement:
			options.complement = true;
			break;
		case tr_detail::kDelete:
			options.delete_mode = true;
			break;
		case tr_detail::kSqueeze:
			options.squeeze = true;
			break;
		case tr_detail::kTruncate:
			options.truncate_set1 = true;
			break;
		default:
			break;
		}
	}

	static bool checkTrOperandCount(const TrOptions& options) {
		const std::size_t count = options.sets.size();
		const bool translating = !options.delete_mode && !options.squeeze;
		const bool two_sets = translating || (options.delete_mode && options.squeeze);
		if (count == 0) return trFail("missing operand");
		if (count < (two_sets ? 2u : 1u)) {
			return trFail("missing operand after '" + options.sets.back() + "'");
		}

		const std::size_t allowed = (options.delete_mode && !options.squeeze) ? 1 : 2;
		if (count > allowed) return trFail("extra operand '" + options.sets[allowed] + "'");
		return true;
	}

	static bool parseTrArgs(const std::vector<std::string>& args, TrOptions& options) {
		OptScanResult scan;
		if (!scanOptions("tr", args, kTrOptionDefs, scan)) return false;

		for (const OptHit& hit : scan.hits) applyTrHit(hit, options);
		options.sets = scan.operands;
		return checkTrOperandCount(options);
	}

	// From a pipe or the console a chunk ends at a newline, so a line-fed
	// producer sees each line translated as soon as it arrives; a file can
	// never stall, so it is read in whole blocks.
	static std::size_t readTrChunk(FILE* in, bool whole_blocks, char* buffer,
			std::size_t capacity) {
		if (whole_blocks) return std::fread(buffer, 1, capacity, in);

		std::size_t got = 0;
		int c = EOF;
		_lock_file(in);
		while (got < capacity && (c = _fgetc_nolock(in)) != EOF) {
			buffer[got++] = static_cast<char>(c);
			if (c == '\n') break;
		}

		_unlock_file(in);
		return got;
	}

	static void translateTrChunk(const Tables& tables, const char* data, std::size_t size,
			int& last, std::string& out) {
		out.clear();
		for (std::size_t k = 0; k < size; ++k) {
			const unsigned char byte = static_cast<unsigned char>(data[k]);
			if (tables.remove[byte]) continue;

			const unsigned char mapped = tables.map[byte];
			if (tables.squeeze[mapped] && last == mapped) continue;

			out.push_back(static_cast<char>(mapped));
			last = mapped;
		}
	}

	static void trStream(const Tables& tables) {
		std::vector<char> buffer(tr_detail::kChunk);
		std::string out;
		int last = -1;
		const bool whole_blocks = isDiskStream(stdin);
		for (;;) {
			const std::size_t got = readTrChunk(stdin, whole_blocks, buffer.data(), buffer.size());
			if (got == 0) return;

			translateTrChunk(tables, buffer.data(), got, last, out);
			std::fwrite(out.data(), 1, out.size(), stdout);
			if (std::ferror(stdout) != 0) return;
		}
	}

	static int builtin_tr(Executor&, const std::vector<std::string>& args) {
		TrOptions options;
		if (!parseTrArgs(args, options)) return 1;

		Tables tables;
		if (!buildTrTables(options, tables)) return 1;

		trStream(tables);
		std::fflush(stdout);
		return 0;
	}

	void registerTrBuiltin(Executor& exec) {
		exec.registerBuiltin("tr", builtin_tr);
	}

}  // namespace wbsh

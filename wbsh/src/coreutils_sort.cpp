/**
 * @file coreutils_sort.cpp
 * @brief sort: GNU key fields (-k / -t), the ordering families
 *        (-n -g -h -M -V -R, -d -i -f -b -r per key), -s / -u / -c / -C /
 *        -m / -o / -z, with GNU's last-resort whole-line comparison.
 */

#include "coreutils_internal.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "executor.h"
#include "optscan.h"

namespace wbsh {

	namespace sort_detail {

		struct Key {
			std::size_t sword = 0;
			std::size_t schar = 0;
			std::size_t eword = SIZE_MAX;
			std::size_t echar = 0;
			bool skip_start_blanks = false;
			bool skip_end_blanks = false;
			bool dictionary = false;
			bool printable = false;
			bool fold = false;
			bool reverse = false;
			bool numeric = false;
			bool general = false;
			bool human = false;
			bool month = false;
			bool version = false;
			bool random = false;
		};

		struct Options {
			Key global;
			std::vector<Key> keys;
			bool stable = false;
			bool unique = false;
			bool check = false;
			bool check_quiet = false;
			char terminator = '\n';
			int tab = -1;
			bool have_output = false;
			std::string output;
			std::vector<std::string> files;
		};

		struct Span {
			const char* begin;
			const char* end;
		};

		enum OptionId {
			kBlanks, kDictionary, kFold, kGeneral, kHuman, kPrintable, kMonth, kNumeric,
			kRandom, kReverse, kVersion, kSortWord, kCheck, kCheckQuiet, kMerge, kKey, kOutput,
			kStable, kTab, kUnique, kZero, kIgnoredValue, kIgnored
		};

		static const char* const kMonths[] = {
			"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
		};

		static const char kHumanUnits[] = "KMGTPEZYRQ";

		static std::uint64_t random_salt = 0;

	}  // namespace sort_detail

	using sort_detail::Key;
	using sort_detail::Span;
	using SortOptions = sort_detail::Options;

	static bool isSortBlank(char c) {
		return c == ' ' || c == '\t';
	}

	static bool isSortDigit(char c) {
		return c >= '0' && c <= '9';
	}

	static bool isSortAlpha(char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
	}

	static bool sortFail(const std::string& message) {
		perr("sort", message);
		return false;
	}

	static const std::vector<OptDef> kSortOptionDefs = {
		{ sort_detail::kBlanks,       'b', "ignore-leading-blanks", OptArg::None     },
		{ sort_detail::kDictionary,   'd', "dictionary-order",      OptArg::None     },
		{ sort_detail::kFold,         'f', "ignore-case",           OptArg::None     },
		{ sort_detail::kGeneral,      'g', "general-numeric-sort",  OptArg::None     },
		{ sort_detail::kHuman,        'h', "human-numeric-sort",    OptArg::None     },
		{ sort_detail::kPrintable,    'i', "ignore-nonprinting",    OptArg::None     },
		{ sort_detail::kMonth,        'M', "month-sort",            OptArg::None     },
		{ sort_detail::kNumeric,      'n', "numeric-sort",          OptArg::None     },
		{ sort_detail::kRandom,       'R', "random-sort",           OptArg::None     },
		{ sort_detail::kReverse,      'r', "reverse",               OptArg::None     },
		{ sort_detail::kVersion,      'V', "version-sort",          OptArg::None     },
		{ sort_detail::kSortWord,     0,   "sort",                  OptArg::Required },
		{ sort_detail::kCheck,        'c', nullptr,                 OptArg::None     },
		{ sort_detail::kCheck,        0,   "check",                 OptArg::Optional },
		{ sort_detail::kCheckQuiet,   'C', nullptr,                 OptArg::None     },
		{ sort_detail::kMerge,        'm', "merge",                 OptArg::None     },
		{ sort_detail::kKey,          'k', "key",                   OptArg::Required },
		{ sort_detail::kOutput,       'o', "output",                OptArg::Required },
		{ sort_detail::kStable,       's', "stable",                OptArg::None     },
		{ sort_detail::kTab,          't', "field-separator",       OptArg::Required },
		{ sort_detail::kUnique,       'u', "unique",                OptArg::None     },
		{ sort_detail::kZero,         'z', "zero-terminated",       OptArg::None     },
		{ sort_detail::kIgnoredValue, 'S', "buffer-size",           OptArg::Required },
		{ sort_detail::kIgnoredValue, 'T', "temporary-directory",   OptArg::Required },
		{ sort_detail::kIgnoredValue, 0,   "parallel",              OptArg::Required },
		{ sort_detail::kIgnoredValue, 0,   "batch-size",            OptArg::Required },
		{ sort_detail::kIgnoredValue, 0,   "random-source",         OptArg::Required },
		{ sort_detail::kIgnoredValue, 0,   "compress-program",      OptArg::Required },
	};

	static bool* sortFlagFor(char letter, Key& key) {
		switch (letter) {
		case 'd':
			return &key.dictionary;
		case 'f':
			return &key.fold;
		case 'i':
			return &key.printable;
		case 'r':
			return &key.reverse;
		case 'g':
			return &key.general;
		case 'h':
			return &key.human;
		case 'M':
			return &key.month;
		case 'n':
			return &key.numeric;
		case 'R':
			return &key.random;
		case 'V':
			return &key.version;
		default:
			return nullptr;
		}
	}

	// One ordering letter, as accepted both globally and after a -k position;
	// `at_end` says whether a `b` belongs to the start or the end position.
	static bool applySortLetter(char letter, Key& key, bool at_end) {
		if (letter == 'b') {
			(at_end ? key.skip_end_blanks : key.skip_start_blanks) = true;
			return true;
		}

		bool* flag = sortFlagFor(letter, key);
		if (flag == nullptr) return false;

		*flag = true;
		return true;
	}

	static std::string sortKeyLetters(const Key& key) {
		const bool flags[] = {
			key.skip_start_blanks || key.skip_end_blanks, key.dictionary, key.fold, key.general,
			key.printable && !key.dictionary, key.month, key.human, key.numeric, key.random,
			key.reverse, key.version
		};
		static const char kLetters[] = "bdfgiMhnRrV";

		std::string letters;
		for (std::size_t k = 0; k < sizeof(flags); ++k) {
			if (flags[k]) letters.push_back(kLetters[k]);
		}

		return letters;
	}

	static bool checkSortCompatibility(const Key& key) {
		const int orders = key.numeric + key.general + key.human + key.month + key.version
			+ key.random + (key.dictionary || key.printable);
		if (orders <= 1 && !(key.random && key.fold)) return true;

		return sortFail("options '-" + sortKeyLetters(key) + "' are incompatible");
	}

	static bool hasSpecialOrder(const Key& key) {
		return key.numeric || key.general || key.human || key.month || key.version || key.random;
	}

	static bool hasOrderingOptionsBesidesReverse(const Key& key) {
		return key.skip_start_blanks || key.skip_end_blanks || key.dictionary || key.printable
			|| key.fold || hasSpecialOrder(key);
	}

	static bool hasOrderingOptions(const Key& key) {
		return hasOrderingOptionsBesidesReverse(key) || key.reverse;
	}

	static bool readSortCount(const std::string& spec, std::size_t& i, std::size_t& out) {
		const std::size_t start = i;
		out = 0;
		while (i < spec.size() && isSortDigit(spec[i])) {
			const std::size_t digit = static_cast<std::size_t>(spec[i++] - '0');
			out = out > (SIZE_MAX - digit) / 10 ? SIZE_MAX : out * 10 + digit;
		}

		return i > start;
	}

	static const char* const kBadAfterDot = "invalid number after '.'";

	static bool badKeyCount(const std::string& spec, std::size_t at, const char* reason) {
		return sortFail(std::string(reason) + ": invalid count at start of '"
			+ spec.substr(at) + "'");
	}

	static bool badKeySpec(const std::string& spec, const char* reason) {
		return sortFail(std::string(reason) + ": invalid field specification '" + spec + "'");
	}

	static bool readKeyLetters(const std::string& spec, std::size_t& i, Key& key, bool at_end) {
		while (i < spec.size() && applySortLetter(spec[i], key, at_end)) ++i;
		return true;
	}

	static bool parseKeyStart(const std::string& spec, std::size_t& i, Key& key) {
		if (!readSortCount(spec, i, key.sword)) {
			return badKeyCount(spec, i, "invalid number at field start");
		}

		if (key.sword-- == 0) return badKeySpec(spec, "field number is zero");
		if (i < spec.size() && spec[i] == '.') {
			++i;
			if (!readSortCount(spec, i, key.schar)) return badKeyCount(spec, i, kBadAfterDot);
			if (key.schar-- == 0) return badKeySpec(spec, "character offset is zero");
		}

		return readKeyLetters(spec, i, key, false);
	}

	static bool parseKeyEnd(const std::string& spec, std::size_t& i, Key& key) {
		++i;
		if (!readSortCount(spec, i, key.eword)) {
			return badKeyCount(spec, i, "invalid number after ','");
		}
		if (key.eword-- == 0) return badKeySpec(spec, "field number is zero");

		if (i < spec.size() && spec[i] == '.') {
			++i;
			if (!readSortCount(spec, i, key.echar)) return badKeyCount(spec, i, kBadAfterDot);
		}

		return readKeyLetters(spec, i, key, true);
	}

	static bool parseKeySpec(const std::string& spec, Key& key) {
		std::size_t i = 0;
		if (!parseKeyStart(spec, i, key)) return false;

		if (i < spec.size() && spec[i] == ',' && !parseKeyEnd(spec, i, key)) return false;
		if (i < spec.size()) return badKeySpec(spec, "stray character in field spec");
		return true;
	}

	static bool parseSortTab(const std::string& value, int& tab) {
		if (value.empty()) return sortFail("empty tab");

		const bool is_nul = value == "\\0";
		if (value.size() > 1 && !is_nul) return sortFail("multi-character tab '" + value + "'");

		const int wanted = is_nul ? 0 : static_cast<unsigned char>(value[0]);
		if (tab >= 0 && tab != wanted) return sortFail("incompatible tabs");

		tab = wanted;
		return true;
	}

	static bool applySortWord(const std::string& word, Key& global) {
		static const char* const kWords[] = {
			"general-numeric", "human-numeric", "month", "numeric", "random", "version"
		};
		static const char kLetters[] = "ghMnRV";

		for (std::size_t k = 0; k < sizeof(kWords) / sizeof(kWords[0]); ++k) {
			if (word == kWords[k]) return applySortLetter(kLetters[k], global, false);
		}

		return sortFail("invalid argument '" + word + "' for '--sort'");
	}

	static bool applySortCheck(const OptHit& hit, SortOptions& options) {
		if (!hit.has_value || hit.value == "diagnose-first") {
			options.check = true;
			return true;
		}

		if (hit.value == "quiet" || hit.value == "silent") {
			options.check = true;
			options.check_quiet = true;
			return true;
		}

		return sortFail("invalid argument '" + hit.value + "' for '--check'");
	}

	static bool applySortKeyHit(const OptHit& hit, SortOptions& options) {
		Key key;
		if (!parseKeySpec(hit.value, key)) return false;

		options.keys.push_back(key);
		return true;
	}

	static bool applySortGlobalLetter(const OptHit& hit, SortOptions& options) {
		static const char kLetters[] = "bdfghiMnRrV";

		applySortLetter(kLetters[hit.id - sort_detail::kBlanks], options.global, false);
		if (options.global.skip_start_blanks) options.global.skip_end_blanks = true;
		return true;
	}

	static bool applySortHit(const OptHit& hit, SortOptions& options) {
		switch (hit.id) {
		case sort_detail::kSortWord:
			return applySortWord(hit.value, options.global);
		case sort_detail::kCheck:
			return applySortCheck(hit, options);
		case sort_detail::kCheckQuiet:
			options.check = true;
			options.check_quiet = true;
			return true;
		case sort_detail::kKey:
			return applySortKeyHit(hit, options);
		case sort_detail::kOutput:
			options.have_output = true;
			options.output = hit.value;
			return true;
		case sort_detail::kStable:
			options.stable = true;
			return true;
		case sort_detail::kTab:
			return parseSortTab(hit.value, options.tab);
		case sort_detail::kUnique:
			options.unique = true;
			return true;
		case sort_detail::kZero:
			options.terminator = '\0';
			return true;
		case sort_detail::kMerge:
		case sort_detail::kIgnoredValue:
			return true;
		default:
			return applySortGlobalLetter(hit, options);
		}
	}

	// GNU: a key without ordering options of its own inherits the global ones,
	// and global options with no -k make the whole line one such key.
	static void inheritGlobalOptions(SortOptions& options) {
		for (Key& key : options.keys) {
			if (hasOrderingOptions(key)) continue;

			const Key position = key;
			key = options.global;
			key.sword = position.sword;
			key.schar = position.schar;
			key.eword = position.eword;
			key.echar = position.echar;
		}

		if (options.keys.empty() && hasOrderingOptionsBesidesReverse(options.global)) {
			options.keys.push_back(options.global);
		}
	}

	static bool checkAllSortKeys(const SortOptions& options) {
		if (!checkSortCompatibility(options.global)) return false;

		for (const Key& key : options.keys) {
			if (!checkSortCompatibility(key)) return false;
		}

		return true;
	}

	static bool parseSortArgs(const std::vector<std::string>& args, SortOptions& options) {
		OptScanResult scan;
		if (!scanOptions("sort", args, kSortOptionDefs, scan)) return false;

		for (const OptHit& hit : scan.hits) {
			if (!applySortHit(hit, options)) return false;
		}

		if (!checkAllSortKeys(options)) return false;

		inheritGlobalOptions(options);
		options.files = scan.operands;
		if (options.check && options.files.size() > 1) {
			return sortFail("extra operand '" + options.files[1] + "' not allowed with -c");
		}

		if (options.files.empty()) options.files.push_back("-");
		return true;
	}

	static const char* skipSortBlanks(const char* p, const char* lim) {
		while (p < lim && isSortBlank(*p)) ++p;
		return p;
	}

	static const char* skipSortField(const char* p, const char* lim, int tab) {
		if (tab >= 0) {
			while (p < lim && static_cast<unsigned char>(*p) != tab) ++p;
			return p;
		}

		p = skipSortBlanks(p, lim);
		while (p < lim && !isSortBlank(*p)) ++p;
		return p;
	}

	static const char* keyBegin(const std::string& line, const Key& key, int tab) {
		const char* p = line.data();
		const char* lim = p + line.size();
		for (std::size_t word = key.sword; p < lim && word > 0; --word) {
			p = skipSortField(p, lim, tab);
			if (tab >= 0 && p < lim) ++p;
		}

		if (key.skip_start_blanks) p = skipSortBlanks(p, lim);
		return p + (std::min)(key.schar, static_cast<std::size_t>(lim - p));
	}

	static const char* keyLimit(const std::string& line, const Key& key, int tab) {
		const char* p = line.data();
		const char* lim = p + line.size();
		if (key.eword == SIZE_MAX) return lim;

		std::size_t word = key.echar == 0 ? key.eword + 1 : key.eword;
		while (p < lim && word > 0) {
			--word;
			p = skipSortField(p, lim, tab);
			if (tab >= 0 && p < lim && (word > 0 || key.echar != 0)) ++p;
		}

		if (key.echar == 0) return p;
		if (key.skip_end_blanks) p = skipSortBlanks(p, lim);
		return p + (std::min)(key.echar, static_cast<std::size_t>(lim - p));
	}

	static Span keySpan(const std::string& line, const Key& key, int tab) {
		const char* begin = keyBegin(line, key, tab);
		const char* end = keyLimit(line, key, tab);
		return Span{ begin, (std::max)(begin, end) };
	}

	static int compareBytes(Span a, Span b) {
		const std::size_t la = static_cast<std::size_t>(a.end - a.begin);
		const std::size_t lb = static_cast<std::size_t>(b.end - b.begin);
		const int diff = std::memcmp(a.begin, b.begin, (std::min)(la, lb));
		if (diff != 0) return diff < 0 ? -1 : 1;
		return la < lb ? -1 : la > lb ? 1 : 0;
	}

	static bool sortIgnores(const Key& key, unsigned char c) {
		const char ch = static_cast<char>(c);
		if (key.dictionary) return !(isSortBlank(ch) || isSortAlpha(ch) || isSortDigit(ch));
		return key.printable && !(c >= 0x20 && c < 0x7F);
	}

	static unsigned char sortTranslate(const Key& key, unsigned char c) {
		if (key.fold && c >= 'a' && c <= 'z') return static_cast<unsigned char>(c - 'a' + 'A');
		return c;
	}

	static const char* skipSortIgnored(Span span, const Key& key) {
		const char* p = span.begin;
		while (p < span.end && sortIgnores(key, static_cast<unsigned char>(*p))) ++p;
		return p;
	}

	static int compareFiltered(Span a, Span b, const Key& key) {
		for (;;) {
			a.begin = skipSortIgnored(a, key);
			b.begin = skipSortIgnored(b, key);
			if (a.begin >= a.end || b.begin >= b.end) break;

			const int ca = sortTranslate(key, static_cast<unsigned char>(*a.begin++));
			const int cb = sortTranslate(key, static_cast<unsigned char>(*b.begin++));
			if (ca != cb) return ca < cb ? -1 : 1;
		}

		return (a.begin < a.end) - (b.begin < b.end);
	}

	struct SortNumber {
		bool negative = false;
		std::string whole;
		std::string fraction;
		const char* end = nullptr;
	};

	static SortNumber parseSortNumber(Span span) {
		SortNumber number;
		const char* p = skipSortBlanks(span.begin, span.end);
		if (p < span.end && *p == '-') {
			number.negative = true;
			++p;
		}

		while (p < span.end && *p == '0') ++p;
		while (p < span.end && isSortDigit(*p)) number.whole.push_back(*p++);
		if (p < span.end && *p == '.') {
			++p;
			while (p < span.end && isSortDigit(*p)) number.fraction.push_back(*p++);
		}

		std::string& fraction = number.fraction;
		while (!fraction.empty() && fraction.back() == '0') fraction.pop_back();
		if (number.whole.empty() && number.fraction.empty()) number.negative = false;
		number.end = p;
		return number;
	}

	static int compareMagnitude(const SortNumber& a, const SortNumber& b) {
		if (a.whole.size() != b.whole.size()) return a.whole.size() < b.whole.size() ? -1 : 1;

		const int whole = a.whole.compare(b.whole);
		if (whole != 0) return whole < 0 ? -1 : 1;

		const int fraction = a.fraction.compare(b.fraction);
		return fraction < 0 ? -1 : fraction > 0 ? 1 : 0;
	}

	static int compareSortNumbers(const SortNumber& a, const SortNumber& b) {
		if (a.negative != b.negative) return a.negative ? -1 : 1;

		const int magnitude = compareMagnitude(a, b);
		return a.negative ? -magnitude : magnitude;
	}

	static int humanUnitOrder(const SortNumber& number, Span span) {
		if (number.whole.empty() && number.fraction.empty()) return 0;
		if (number.end >= span.end) return 0;

		const char unit = *number.end == 'k' ? 'K' : *number.end;
		const char* found = std::strchr(sort_detail::kHumanUnits, unit);
		if (unit == '\0' || found == nullptr) return 0;

		const int order = static_cast<int>(found - sort_detail::kHumanUnits) + 1;
		return number.negative ? -order : order;
	}

	static int compareHuman(Span a, Span b) {
		const SortNumber na = parseSortNumber(a);
		const SortNumber nb = parseSortNumber(b);
		const int ua = humanUnitOrder(na, a);
		const int ub = humanUnitOrder(nb, b);
		if (ua != ub) return ua < ub ? -1 : 1;
		return compareSortNumbers(na, nb);
	}

	static int compareGeneral(Span a, Span b) {
		const std::string sa(a.begin, a.end);
		const std::string sb(b.begin, b.end);
		char* ea = nullptr;
		char* eb = nullptr;
		const long double va = std::strtold(sa.c_str(), &ea);
		const long double vb = std::strtold(sb.c_str(), &eb);

		const bool a_bad = ea == sa.c_str();
		const bool b_bad = eb == sb.c_str();
		if (a_bad || b_bad) return a_bad && b_bad ? 0 : a_bad ? -1 : 1;

		const bool a_nan = va != va;
		const bool b_nan = vb != vb;
		if (a_nan || b_nan) return a_nan && b_nan ? 0 : a_nan ? -1 : 1;
		return va < vb ? -1 : va > vb ? 1 : 0;
	}

	static int sortMonth(Span span) {
		const char* p = skipSortBlanks(span.begin, span.end);
		if (span.end - p < 3) return 0;

		for (int month = 0; month < 12; ++month) {
			bool same = true;
			for (int k = 0; k < 3 && same; ++k) {
				const char c = p[k] >= 'a' && p[k] <= 'z' ? static_cast<char>(p[k] - 'a' + 'A')
					: p[k];
				same = c == sort_detail::kMonths[month][k];
			}

			if (same) return month + 1;
		}

		return 0;
	}

	static int versionOrder(const char* s, std::size_t pos, std::size_t len) {
		if (pos == len) return -1;

		const unsigned char c = static_cast<unsigned char>(s[pos]);
		if (isSortDigit(static_cast<char>(c))) return 0;
		if (isSortAlpha(static_cast<char>(c))) return c;
		if (c == '~') return -2;
		return c + 256;
	}

	static bool versionDigitAt(const char* s, std::size_t pos, std::size_t len) {
		return pos < len && isSortDigit(s[pos]);
	}

	static int versionCompareText(const char* a, std::size_t& ia, std::size_t la,
			const char* b, std::size_t& ib, std::size_t lb) {
		while ((ia < la && !isSortDigit(a[ia])) || (ib < lb && !isSortDigit(b[ib]))) {
			const int oa = versionOrder(a, ia, la);
			const int ob = versionOrder(b, ib, lb);
			if (oa != ob) return oa - ob;
			++ia;
			++ib;
		}

		return 0;
	}

	static int versionCompareDigits(const char* a, std::size_t& ia, std::size_t la,
			const char* b, std::size_t& ib, std::size_t lb) {
		while (ia < la && a[ia] == '0') ++ia;
		while (ib < lb && b[ib] == '0') ++ib;

		int first_diff = 0;
		while (versionDigitAt(a, ia, la) && versionDigitAt(b, ib, lb)) {
			if (first_diff == 0) first_diff = a[ia] - b[ib];
			++ia;
			++ib;
		}

		if (versionDigitAt(a, ia, la)) return 1;
		if (versionDigitAt(b, ib, lb)) return -1;
		return first_diff;
	}

	// Debian's verrevcmp, as used by gnulib's filevercmp.
	static int versionRevCompare(const char* a, std::size_t la, const char* b, std::size_t lb) {
		std::size_t ia = 0;
		std::size_t ib = 0;
		while (ia < la || ib < lb) {
			const int text = versionCompareText(a, ia, la, b, ib, lb);
			if (text != 0) return text;

			const int digits = versionCompareDigits(a, ia, la, b, ib, lb);
			if (digits != 0) return digits;
		}

		return 0;
	}

	static bool isVersionSuffixChar(char c) {
		return isSortAlpha(c) || isSortDigit(c) || c == '~';
	}

	// Length of `s` without its longest suffix matching (\.[A-Za-z~][A-Za-z0-9~]*)*$.
	static std::size_t versionPrefixLength(const char* s, std::size_t n) {
		std::size_t prefix = 0;
		std::size_t i = 0;
		while (i < n) {
			++i;
			prefix = i;
			while (i + 1 < n && s[i] == '.' && (isSortAlpha(s[i + 1]) || s[i + 1] == '~')) {
				for (i += 2; i < n && isVersionSuffixChar(s[i]); ++i) continue;
			}
		}

		return prefix;
	}

	static int versionDotRank(const char* s, std::size_t n) {
		if (n == 1) return 0;
		if (n == 2 && s[1] == '.') return 1;
		return 2;
	}

	static int compareVersion(Span a, Span b) {
		const std::size_t la = static_cast<std::size_t>(a.end - a.begin);
		const std::size_t lb = static_cast<std::size_t>(b.end - b.begin);
		if (la == 0 || lb == 0) return (lb == 0) - (la == 0);

		const bool a_dot = a.begin[0] == '.';
		const bool b_dot = b.begin[0] == '.';
		if (a_dot != b_dot) return a_dot ? -1 : 1;
		if (a_dot) {
			const int ra = versionDotRank(a.begin, la);
			const int rb = versionDotRank(b.begin, lb);
			if ((ra < 2 || rb < 2) && ra != rb) return ra < rb ? -1 : 1;
		}

		const std::size_t pa = versionPrefixLength(a.begin, la);
		const std::size_t pb = versionPrefixLength(b.begin, lb);
		const int result = versionRevCompare(a.begin, pa, b.begin, pb);
		if (result != 0 || (pa == la && pb == lb)) return result;
		return versionRevCompare(a.begin, la, b.begin, lb);
	}

	static std::uint64_t randomKeyHash(Span span) {
		std::uint64_t hash = 1469598103934665603ULL ^ sort_detail::random_salt;
		for (const char* p = span.begin; p < span.end; ++p) {
			hash ^= static_cast<unsigned char>(*p);
			hash *= 1099511628211ULL;
		}

		return hash;
	}

	static int compareRandom(Span a, Span b) {
		const std::uint64_t ha = randomKeyHash(a);
		const std::uint64_t hb = randomKeyHash(b);
		if (ha != hb) return ha < hb ? -1 : 1;
		return compareBytes(a, b);
	}

	static int compareKeySpans(Span a, Span b, const Key& key) {
		if (key.numeric) return compareSortNumbers(parseSortNumber(a), parseSortNumber(b));
		if (key.general) return compareGeneral(a, b);
		if (key.human) return compareHuman(a, b);
		if (key.month) return sortMonth(a) - sortMonth(b);
		if (key.version) return compareVersion(a, b);
		if (key.random) return compareRandom(a, b);
		if (key.dictionary || key.printable || key.fold) return compareFiltered(a, b, key);
		return compareBytes(a, b);
	}

	static int compareKeys(const std::string& a, const std::string& b, const SortOptions& options) {
		for (const Key& key : options.keys) {
			const Span sa = keySpan(a, key, options.tab);
			const Span sb = keySpan(b, key, options.tab);
			const int diff = compareKeySpans(sa, sb, key);
			if (diff != 0) return key.reverse ? -diff : diff;
		}

		return 0;
	}

	static int compareLines(const std::string& a, const std::string& b,
			const SortOptions& options) {
		if (!options.keys.empty()) {
			const int diff = compareKeys(a, b, options);
			if (diff != 0 || options.unique || options.stable) return diff;
		}

		const Span sa{ a.data(), a.data() + a.size() };
		const Span sb{ b.data(), b.data() + b.size() };
		const int diff = compareBytes(sa, sb);
		return options.global.reverse ? -diff : diff;
	}

	static void readSortRecords(FILE* stream, char terminator, std::vector<std::string>& lines) {
		std::string record;
		int c = EOF;
		_lock_file(stream);
		while ((c = _fgetc_nolock(stream)) != EOF) {
			if (c != static_cast<unsigned char>(terminator)) {
				record.push_back(static_cast<char>(c));
				continue;
			}

			lines.push_back(std::move(record));
			record.clear();
		}

		_unlock_file(stream);
		if (!record.empty()) lines.push_back(std::move(record));
	}

	static bool readSortInput(Executor& exec, const std::string& path, char terminator,
			std::vector<std::string>& lines) {
		FILE* stream = path == "-" ? stdin : fopenNative(exec, path, "rb");
		if (stream == nullptr) {
			return sortFail("cannot read: " + path + ": " + std::strerror(errno));
		}

		readSortRecords(stream, terminator, lines);
		if (stream != stdin) std::fclose(stream);
		return true;
	}

	static int checkSorted(const std::vector<std::string>& lines, const SortOptions& options) {
		for (std::size_t k = 1; k < lines.size(); ++k) {
			const int diff = compareLines(lines[k - 1], lines[k], options);
			if (diff < 0 || (diff == 0 && !options.unique)) continue;

			if (!options.check_quiet) {
				std::fprintf(stderr, "wbsh: sort: %s:%zu: disorder: ",
					options.files[0].c_str(), k + 1);
				std::fwrite(lines[k].data(), 1, lines[k].size(), stderr);
				std::fputc('\n', stderr);
			}

			return 1;
		}

		return 0;
	}

	static void sortAndUnique(std::vector<std::string>& lines, const SortOptions& options) {
		auto before = [&](const std::string& a, const std::string& b) {
			return compareLines(a, b, options) < 0;
		};
		std::stable_sort(lines.begin(), lines.end(), before);
		if (!options.unique) return;

		auto same = [&](const std::string& a, const std::string& b) {
			return compareLines(a, b, options) == 0;
		};
		lines.erase(std::unique(lines.begin(), lines.end(), same), lines.end());
	}

	static bool writeSortOutput(Executor& exec, const std::vector<std::string>& lines,
			const SortOptions& options) {
		FILE* out = options.have_output ? fopenNative(exec, options.output, "wb") : stdout;
		if (out == nullptr) {
			return sortFail("open failed: " + options.output + ": " + std::strerror(errno));
		}

		for (const std::string& line : lines) {
			std::fwrite(line.data(), 1, line.size(), out);
			std::fputc(options.terminator, out);
			if (std::ferror(out) != 0) break;
		}

		if (out != stdout) std::fclose(out);
		std::fflush(stdout);
		return true;
	}

	static void seedRandomSort(const SortOptions& options) {
		for (const Key& key : options.keys) {
			if (!key.random) continue;

			std::random_device device;
			sort_detail::random_salt = (static_cast<std::uint64_t>(device()) << 32) ^ device();
			return;
		}
	}

	static int builtin_sort(Executor& exec, const std::vector<std::string>& args) {
		SortOptions options;
		if (!parseSortArgs(args, options)) return 2;

		std::vector<std::string> lines;
		for (const auto& file : options.files) {
			if (!readSortInput(exec, file, options.terminator, lines)) return 2;
		}

		if (options.check) return checkSorted(lines, options);

		seedRandomSort(options);
		sortAndUnique(lines, options);
		return writeSortOutput(exec, lines, options) ? 0 : 2;
	}

	void registerSortBuiltin(Executor& exec) {
		exec.registerBuiltin("sort", builtin_sort);
	}

}  // namespace wbsh

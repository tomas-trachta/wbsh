/**
 * @file coreutils_text.cpp
 * @brief Text-processing coreutils: sort, uniq, tr, cut, tee, paste,
 *        tac, rev, nl, grep, find, sed, fold, column, expand,
 *        unexpand, comm, xargs (awk lives in awk.cpp).
 */

#include "coreutils_internal.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <regex>
#include <string>
#include <vector>

#include "awk.h"
#include "executor.h"
#include "numparse.h"
#include "optscan.h"
#include "regexutil.h"

namespace wbsh {

	namespace fs = std::filesystem;

	static const std::size_t kReadChunk = 4096;
	static const int kDefaultTabstop = 8;
	static const int kFoldDefaultWidth = 80;
	static const char* const kColumnDefaultSeparators = " \t";
	static const char* const kColumnDefaultOutputSeparator = "  ";
	static const char* const kStdinPath = "-";

	static bool isOptionArg(const std::string& arg) {
		return !arg.empty() && arg[0] == '-' && arg != kStdinPath;
	}

	static bool isShortCluster(const std::string& arg) {
		return arg.size() > 1 && arg[0] == '-' && arg[1] != '-';
	}

	static bool isNumericOption(const std::string& arg) {
		return isOptionArg(arg) && std::isdigit(static_cast<unsigned char>(arg[1])) != 0;
	}

	static bool hasAttachedValue(const std::string& arg, const char* option) {
		return arg.size() > 2 && arg.compare(0, 2, option) == 0;
	}

	static std::vector<std::string> nonOptionArgs(const std::vector<std::string>& args) {
		std::vector<std::string> files;
		for (const auto& arg : args) {
			if (!arg.empty() && arg[0] != '-') files.push_back(arg);
		}

		return files;
	}

	static void defaultToStdin(std::vector<std::string>& files) {
		if (files.empty()) files.push_back(kStdinPath);
	}

	static bool readsStdin(const std::vector<std::string>& files) {
		return files.empty() || files[0] == kStdinPath;
	}

	static bool loadLines(Executor& exec, const char* tool, const std::string& file,
			std::vector<std::string>& lines) {
		if (readAllLines(exec, file, lines)) return true;

		perr(tool, file);
		return false;
	}

	static bool loadLinesWithErrno(Executor& exec, const char* tool, const std::string& file,
			std::vector<std::string>& lines) {
		if (readAllLines(exec, file, lines)) return true;

		perr(tool, file + ": " + std::strerror(errno));
		return false;
	}

	// Calls `on_record` for every `terminator`-terminated record and once
	// more for a trailing unterminated one; an empty trailing buffer is not
	// a record. A false return from `on_record` (output gone) stops the scan.
	template <typename Fn>
	static bool forEachRecord(FILE* stream, char terminator, Fn on_record) {
		std::string record;
		bool keep_going = true;
		int c;
		_lock_file(stream);
		while (keep_going && (c = _fgetc_nolock(stream)) != EOF) {
			if (c != static_cast<unsigned char>(terminator)) {
				record.push_back(static_cast<char>(c));
				continue;
			}

			keep_going = on_record(record);
			record.clear();
		}

		_unlock_file(stream);
		if (keep_going && !record.empty()) keep_going = on_record(record);
		return keep_going;
	}

	template <typename Fn>
	static void forEachLine(FILE* stream, Fn on_line) {
		forEachRecord(stream, '\n', [&](const std::string& line) {
			on_line(line);
			return true;
		});
	}

	static bool outputOk(FILE* out) {
		return std::ferror(out) == 0;
	}

	static void writeRecord(FILE* out, const std::string& record, char terminator) {
		std::fwrite(record.data(), 1, record.size(), out);
		std::fputc(terminator, out);
	}

	static bool parseCountValue(const std::string& text, std::size_t& out) {
		unsigned long long value = 0;
		std::size_t consumed = 0;
		if (text.empty() || text[0] == '-' || text[0] == ' ') return false;
		if (!parseULL(text, value, &consumed) || consumed != text.size()) return false;

		out = static_cast<std::size_t>(value);
		return true;
	}

	static FILE* openInputStream(Executor& exec, const char* tool, const std::string& path) {
		if (path == kStdinPath) return stdin;

		FILE* stream = fopenNative(exec, path, "rb");
		if (stream == nullptr) perr(tool, path + ": " + std::strerror(errno));
		return stream;
	}

	static void closeInputStream(FILE* stream) {
		if (stream != nullptr && stream != stdin) std::fclose(stream);
	}

	static bool rejectExtraOperand(const char* tool, const std::vector<std::string>& operands,
			std::size_t allowed) {
		if (operands.size() <= allowed) return false;

		perr(tool, "extra operand '" + operands[allowed] + "'");
		return true;
	}

	// Runs `on_stream` over stdin when no file (or "-") is named, else
	// over each file in turn; the first file that fails to open ends the run.
	template <typename Fn>
	static int runOnInputs(Executor& exec, const char* tool,
			const std::vector<std::string>& files, Fn on_stream) {
		if (readsStdin(files)) {
			on_stream(stdin);
			return 0;
		}

		for (const auto& path : files) {
			FILE* stream = fopenNative(exec, path, "rb");
			if (stream == nullptr) {
				perr(tool, path, std::error_code(errno, std::system_category()));
				return 1;
			}

			on_stream(stream);
			std::fclose(stream);
		}

		return 0;
	}

	static std::string asciiLower(std::string text) {
		for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return text;
	}

	static bool equalFold(const std::string& a, const std::string& b) {
		if (a.size() != b.size()) return false;
		for (std::size_t i = 0; i < a.size(); ++i) {
			if (std::tolower(static_cast<unsigned char>(a[i]))
					!= std::tolower(static_cast<unsigned char>(b[i]))) return false;
		}

		return true;
	}

	static bool linesEqual(const std::string& a, const std::string& b, bool fold) {
		return fold ? equalFold(a, b) : a == b;
	}

	struct SortOptions {
		bool reverse = false;
		bool numeric = false;
		bool unique = false;
		bool fold = false;
		std::vector<std::string> files;
	};

	static bool applySortCluster(const std::string& cluster, SortOptions& options) {
		for (std::size_t k = 1; k < cluster.size(); ++k) {
			switch (cluster[k]) {
			case 'r': options.reverse = true; break;
			case 'n': options.numeric = true; break;
			case 'u': options.unique = true; break;
			case 'f': options.fold = true; break;
			default:
				std::fprintf(stderr, "wbsh: sort: unknown -%c\n", cluster[k]);
				return false;
			}
		}

		return true;
	}

	static int parseSortArgs(const std::vector<std::string>& args, SortOptions& options) {
		for (const auto& arg : args) {
			if (arg == "--") continue;

			if (arg == "-r" || arg == "--reverse") {
				options.reverse = true;
			} else if (arg == "-n" || arg == "--numeric-sort") {
				options.numeric = true;
			} else if (arg == "-u" || arg == "--unique") {
				options.unique = true;
			} else if (arg == "-f" || arg == "--ignore-case") {
				options.fold = true;
			} else if (isShortCluster(arg)) {
				if (!applySortCluster(arg, options)) return 2;
			} else {
				options.files.push_back(arg);
			}
		}

		defaultToStdin(options.files);
		return 0;
	}

	static void sortLinesNumeric(std::vector<std::string>& lines) {
		std::vector<std::pair<double, std::string>> keyed;
		keyed.reserve(lines.size());
		for (auto& line : lines) {
			double key = 0;
			parseDouble(line, key);
			keyed.emplace_back(key, std::move(line));
		}

		std::sort(keyed.begin(), keyed.end());
		for (std::size_t i = 0; i < keyed.size(); ++i) lines[i] = std::move(keyed[i].second);
	}

	static void sortLinesFolded(std::vector<std::string>& lines) {
		std::vector<std::pair<std::string, std::string>> keyed;
		keyed.reserve(lines.size());
		for (auto& line : lines) {
			std::string key = asciiLower(line);
			keyed.emplace_back(std::move(key), std::move(line));
		}

		std::sort(keyed.begin(), keyed.end());
		for (std::size_t i = 0; i < keyed.size(); ++i) lines[i] = std::move(keyed[i].second);
	}

	static void dropAdjacentDuplicates(std::vector<std::string>& lines, bool fold) {
		auto same = [fold](const std::string& a, const std::string& b) {
			return linesEqual(a, b, fold);
		};
		lines.erase(std::unique(lines.begin(), lines.end(), same), lines.end());
	}

	static void writeLines(const std::vector<std::string>& lines) {
		for (const auto& line : lines) {
			std::fwrite(line.data(), 1, line.size(), stdout);
			std::fputc('\n', stdout);
		}
	}

	static int builtin_sort(Executor& exec, const std::vector<std::string>& args) {
		SortOptions options;
		if (int status = parseSortArgs(args, options); status != 0) return status;

		std::vector<std::string> lines;
		for (const auto& file : options.files) {
			if (!loadLinesWithErrno(exec, "sort", file, lines)) return 2;
		}

		if (options.numeric)   sortLinesNumeric(lines);
		else if (options.fold) sortLinesFolded(lines);
		else                   std::sort(lines.begin(), lines.end());

		if (options.reverse) std::reverse(lines.begin(), lines.end());
		if (options.unique) dropAdjacentDuplicates(lines, options.fold);

		writeLines(lines);
		std::fflush(stdout);
		return 0;
	}

	enum class UniqDelimit { None, Prepend, Separate };

	struct UniqOptions {
		bool count = false;
		bool output_unique = true;
		bool output_first_repeated = true;
		bool output_later_repeated = false;
		bool fold = false;
		UniqDelimit delimit = UniqDelimit::None;
		std::size_t skip_fields = 0;
		std::size_t skip_chars = 0;
		std::size_t check_chars = SIZE_MAX;
		char terminator = '\n';
		std::string input = kStdinPath;
		std::string output = kStdinPath;
	};

	enum UniqOptionId {
		kUniqCount, kUniqRepeated, kUniqAllRepeated, kUniqSkipFields, kUniqIgnoreCase,
		kUniqSkipChars, kUniqUnique, kUniqZero, kUniqCheckChars
	};

	static const std::vector<OptDef> kUniqOptionDefs = {
		{ kUniqCount,       'c', "count",           OptArg::None     },
		{ kUniqRepeated,    'd', "repeated",        OptArg::None     },
		{ kUniqAllRepeated, 'D', nullptr,           OptArg::None     },
		{ kUniqAllRepeated, 0,   "all-repeated",    OptArg::Optional },
		{ kUniqSkipFields,  'f', "skip-fields",     OptArg::Required },
		{ kUniqIgnoreCase,  'i', "ignore-case",     OptArg::None     },
		{ kUniqSkipChars,   's', "skip-chars",      OptArg::Required },
		{ kUniqUnique,      'u', "unique",          OptArg::None     },
		{ kUniqZero,        'z', "zero-terminated", OptArg::None     },
		{ kUniqCheckChars,  'w', "check-chars",     OptArg::Required },
	};

	static bool applyUniqAllRepeated(const OptHit& hit, UniqOptions& options) {
		options.output_unique = false;
		options.output_later_repeated = true;
		if (!hit.has_value || hit.value == "none") {
			options.delimit = UniqDelimit::None;
		} else if (hit.value == "prepend") {
			options.delimit = UniqDelimit::Prepend;
		} else if (hit.value == "separate") {
			options.delimit = UniqDelimit::Separate;
		} else {
			perr("uniq", "invalid argument '" + hit.value + "' for '--all-repeated'");
			return false;
		}

		return true;
	}

	static bool applyUniqCount(const OptHit& hit, std::size_t& out, const char* what) {
		if (parseCountValue(hit.value, out)) return true;

		perr("uniq", hit.value + ": invalid number of " + what);
		return false;
	}

	static bool applyUniqHit(const OptHit& hit, UniqOptions& options) {
		switch (hit.id) {
		case kUniqCount:
			options.count = true;
			return true;
		case kUniqRepeated:
			options.output_unique = false;
			return true;
		case kUniqAllRepeated:
			return applyUniqAllRepeated(hit, options);
		case kUniqSkipFields:
			return applyUniqCount(hit, options.skip_fields, "fields to skip");
		case kUniqIgnoreCase:
			options.fold = true;
			return true;
		case kUniqSkipChars:
			return applyUniqCount(hit, options.skip_chars, "bytes to skip");
		case kUniqUnique:
			options.output_first_repeated = false;
			return true;
		case kUniqZero:
			options.terminator = '\0';
			return true;
		case kUniqCheckChars:
			return applyUniqCount(hit, options.check_chars, "bytes to compare");
		default:
			return false;
		}
	}

	static bool parseUniqArgs(const std::vector<std::string>& args, UniqOptions& options) {
		OptScanResult scan;
		if (!scanOptions("uniq", args, kUniqOptionDefs, scan)) return false;

		for (const OptHit& hit : scan.hits) {
			if (!applyUniqHit(hit, options)) return false;
		}

		if (options.count && options.output_later_repeated) {
			perr("uniq", "printing all duplicated lines and repeat counts is meaningless");
			return false;
		}

		if (rejectExtraOperand("uniq", scan.operands, 2)) return false;
		if (!scan.operands.empty()) options.input = scan.operands[0];
		if (scan.operands.size() > 1) options.output = scan.operands[1];
		return true;
	}

	static bool isBlankChar(char c) {
		return c == ' ' || c == '\t';
	}

	static std::size_t uniqKeyStart(const std::string& line, const UniqOptions& options) {
		std::size_t i = 0;
		for (std::size_t field = 0; field < options.skip_fields && i < line.size(); ++field) {
			while (i < line.size() && isBlankChar(line[i])) ++i;
			while (i < line.size() && !isBlankChar(line[i])) ++i;
		}

		return i + (std::min)(options.skip_chars, line.size() - i);
	}

	static bool equalFoldRange(const std::string& a, std::size_t a_at, const std::string& b,
			std::size_t b_at, std::size_t length) {
		for (std::size_t i = 0; i < length; ++i) {
			if (std::tolower(static_cast<unsigned char>(a[a_at + i]))
					!= std::tolower(static_cast<unsigned char>(b[b_at + i]))) return false;
		}

		return true;
	}

	static bool uniqKeysMatch(const std::string& a, std::size_t a_at, const std::string& b,
			std::size_t b_at, const UniqOptions& options) {
		const std::size_t a_length = (std::min)(a.size() - a_at, options.check_chars);
		const std::size_t b_length = (std::min)(b.size() - b_at, options.check_chars);
		if (a_length != b_length) return false;

		if (options.fold) return equalFoldRange(a, a_at, b, b_at, a_length);
		return a.compare(a_at, a_length, b, b_at, b_length) == 0;
	}

	struct UniqRun {
		FILE* out = nullptr;
		std::string previous;
		std::size_t previous_key = 0;
		bool have_previous = false;
		std::uintmax_t match_count = 0;
		bool first_delimiter = true;
	};

	static void uniqWriteLine(UniqRun& run, const UniqOptions& options, bool match) {
		const bool wanted = run.match_count == 0 ? options.output_unique
			: !match ? options.output_first_repeated
			: options.output_later_repeated;
		if (!wanted) return;

		if (options.count) {
			std::fprintf(run.out, "%7llu ", static_cast<unsigned long long>(run.match_count + 1));
		}

		writeRecord(run.out, run.previous, options.terminator);
	}

	// Mirrors GNU's group delimiting: a separator goes before the second line
	// of every repeated group, except the first group in `separate` mode.
	static void uniqDelimitGroup(UniqRun& run, const UniqOptions& options, bool match) {
		if (options.delimit == UniqDelimit::None) return;

		if (!match) {
			if (run.match_count != 0) run.first_delimiter = false;
			return;
		}

		if (run.match_count != 1) return;
		if (options.delimit == UniqDelimit::Prepend || !run.first_delimiter) {
			std::fputc(options.terminator, run.out);
		}
	}

	static void uniqStartRun(UniqRun& run, const std::string& line, std::size_t key) {
		run.previous = line;
		run.previous_key = key;
		run.have_previous = true;
	}

	// Outside --all-repeated the comparison line stays the first of its
	// group, as in GNU, which matters once -w / -s / -f / -i make
	// non-identical lines compare equal.
	static bool uniqFeedLine(UniqRun& run, const UniqOptions& options, const std::string& line) {
		const std::size_t key = uniqKeyStart(line, options);
		if (!run.have_previous) {
			uniqStartRun(run, line, key);
			return true;
		}

		const bool match = uniqKeysMatch(line, key, run.previous, run.previous_key, options);
		if (match) ++run.match_count;
		uniqDelimitGroup(run, options, match);

		if (!match || options.output_later_repeated) {
			uniqWriteLine(run, options, match);
			uniqStartRun(run, line, key);
			if (!match) run.match_count = 0;
		}

		return outputOk(run.out);
	}

	static void uniqRunStream(FILE* in, FILE* out, const UniqOptions& options) {
		UniqRun run;
		run.out = out;
		const bool finished = forEachRecord(in, options.terminator,
			[&](const std::string& line) { return uniqFeedLine(run, options, line); });

		if (finished && run.have_previous) uniqWriteLine(run, options, false);
	}

	static FILE* openUniqOutput(Executor& exec, const std::string& path) {
		if (path == kStdinPath) return stdout;

		FILE* stream = fopenNative(exec, path, "wb");
		if (stream == nullptr) perr("uniq", path + ": " + std::strerror(errno));
		return stream;
	}

	static int builtin_uniq(Executor& exec, const std::vector<std::string>& args) {
		UniqOptions options;
		if (!parseUniqArgs(args, options)) return 1;

		FILE* in = openInputStream(exec, "uniq", options.input);
		if (in == nullptr) return 1;

		FILE* out = openUniqOutput(exec, options.output);
		if (out == nullptr) {
			closeInputStream(in);
			return 1;
		}

		uniqRunStream(in, out, options);
		closeInputStream(in);
		if (out != stdout) std::fclose(out);
		std::fflush(stdout);
		return 0;
	}

	static char trEscapeChar(char escaped) {
		switch (escaped) {
		case 'n':  return '\n';
		case 't':  return '\t';
		case 'r':  return '\r';
		case '\\': return '\\';
		case 'a':  return '\a';
		case 'b':  return '\b';
		case '0':  return '\0';
		default:   return escaped;
		}
	}

	static void appendCharRange(std::string& out, char from, char to) {
		if (from <= to) for (char c = from; c <= to; ++c) out.push_back(c);
		else            for (char c = from; c >= to; --c) out.push_back(c);
	}

	static std::string trExpandSet(const std::string& set) {
		std::string out;
		for (std::size_t i = 0; i < set.size(); ++i) {
			if (set[i] == '\\' && i + 1 < set.size()) {
				out.push_back(trEscapeChar(set[++i]));
			} else if (i + 2 < set.size() && set[i + 1] == '-') {
				appendCharRange(out, set[i], set[i + 2]);
				i += 2;
			} else {
				out.push_back(set[i]);
			}
		}

		return out;
	}

	static std::vector<bool> buildTrMembership(const std::string& set1, bool complement) {
		std::vector<bool> in_set1(256, false);
		for (unsigned char c : set1) in_set1[c] = true;
		if (!complement) return in_set1;

		std::vector<bool> inverted(256, true);
		for (int k = 0; k < 256; ++k) if (in_set1[k]) inverted[k] = false;
		return inverted;
	}

	static std::vector<unsigned char> buildTrMap(const std::string& set1, const std::string& set2,
			const std::vector<bool>& in_set1, bool complement) {
		std::vector<unsigned char> map(256);
		for (int k = 0; k < 256; ++k) map[k] = static_cast<unsigned char>(k);
		if (set2.empty()) return map;

		if (complement) {
			const unsigned char replacement = static_cast<unsigned char>(set2.back());
			for (int k = 0; k < 256; ++k) if (in_set1[k]) map[k] = replacement;
			return map;
		}

		for (std::size_t k = 0; k < set1.size(); ++k) {
			const unsigned char src = static_cast<unsigned char>(set1[k]);
			const unsigned char dst = (k < set2.size())
				? static_cast<unsigned char>(set2[k])
				: static_cast<unsigned char>(set2.back());
			map[src] = dst;
		}

		return map;
	}

	struct TrOptions {
		bool delete_mode = false;
		bool squeeze = false;
		bool complement = false;
		bool truncate_set1 = false;
		std::vector<std::string> sets;
	};

	enum TrOptionId { kTrComplement, kTrDelete, kTrSqueeze, kTrTruncate };

	static const std::vector<OptDef> kTrOptionDefs = {
		{ kTrComplement, 'c', "complement",      OptArg::None },
		{ kTrComplement, 'C', nullptr,           OptArg::None },
		{ kTrDelete,     'd', "delete",          OptArg::None },
		{ kTrSqueeze,    's', "squeeze-repeats", OptArg::None },
		{ kTrTruncate,   't', "truncate-set1",   OptArg::None },
	};

	static void applyTrHit(const OptHit& hit, TrOptions& options) {
		switch (hit.id) {
		case kTrComplement:
			options.complement = true;
			break;
		case kTrDelete:
			options.delete_mode = true;
			break;
		case kTrSqueeze:
			options.squeeze = true;
			break;
		case kTrTruncate:
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
		const std::size_t wanted = two_sets ? 2 : 1;
		if (count == 0) {
			perr("tr", "missing operand");
			return false;
		}

		if (count < wanted) {
			perr("tr", "missing operand after '" + options.sets.back() + "'");
			return false;
		}

		const std::size_t allowed = (options.delete_mode && !options.squeeze) ? 1 : 2;
		return !rejectExtraOperand("tr", options.sets, allowed);
	}

	static bool parseTrArgs(const std::vector<std::string>& args, TrOptions& options) {
		OptScanResult scan;
		if (!scanOptions("tr", args, kTrOptionDefs, scan)) return false;

		for (const OptHit& hit : scan.hits) applyTrHit(hit, options);
		options.sets = scan.operands;
		return checkTrOperandCount(options);
	}

	static void trStream(const TrOptions& options, const std::vector<bool>& in_set1,
			const std::vector<unsigned char>& map) {
		int previous = -1;
		int c;
		while ((c = std::fgetc(stdin)) != EOF) {
			const unsigned char byte = static_cast<unsigned char>(c);
			if (options.delete_mode && in_set1[byte]) continue;

			const unsigned char out = map[byte];
			if (options.squeeze && static_cast<int>(out) == previous) continue;

			std::fputc(out, stdout);
			previous = out;
		}
	}

	static int builtin_tr(Executor&, const std::vector<std::string>& args) {
		TrOptions options;
		if (!parseTrArgs(args, options)) return 1;

		const std::string set2 =
			(options.sets.size() > 1) ? trExpandSet(options.sets[1]) : std::string();
		std::string set1 = trExpandSet(options.sets[0]);
		const bool translating = !options.delete_mode && !set2.empty();
		if (options.truncate_set1 && translating && set1.size() > set2.size()) {
			set1.resize(set2.size());
		}

		const std::vector<bool> in_set1 = buildTrMembership(set1, options.complement);
		const std::vector<unsigned char> map = buildTrMap(set1,
			options.delete_mode ? std::string() : set2, in_set1, options.complement);

		trStream(options, in_set1, map);
		std::fflush(stdout);
		return 0;
	}

	static const std::size_t kCutOpenEnd = SIZE_MAX;

	struct CutRange {
		std::size_t lo;
		std::size_t hi;
	};

	enum class CutMode { None, Bytes, Fields };

	struct CutOptions {
		CutMode mode = CutMode::None;
		std::string list;
		bool have_delim = false;
		char delim = '\t';
		bool have_output_delim = false;
		std::string output_delim;
		bool only_delimited = false;
		bool complement = false;
		char terminator = '\n';
		std::vector<std::string> files;
		std::vector<CutRange> ranges;
	};

	static bool cutFail(const std::string& message) {
		perr("cut", message);
		return false;
	}

	static bool parseCutBound(const std::string& text, std::size_t& out) {
		if (text.empty()) return false;
		for (char c : text) {
			if (std::isdigit(static_cast<unsigned char>(c)) == 0) return false;
		}

		return parseCountValue(text, out);
	}

	static std::string cutBadValueMessage(CutMode mode, const std::string& token) {
		if (mode == CutMode::Fields) return "invalid field value '" + token + "'";
		return "invalid byte/character position '" + token + "'";
	}

	static std::string cutNumberedFromOneMessage(CutMode mode) {
		if (mode == CutMode::Fields) return "fields are numbered from 1";
		return "byte/character positions are numbered from 1";
	}

	static bool parseCutBounds(const std::string& token, CutMode mode, CutRange& range) {
		const std::size_t dash = token.find('-');
		const std::string left = token.substr(0, dash);
		const std::string right = dash == std::string::npos ? left : token.substr(dash + 1);
		if (dash != std::string::npos && left.empty() && right.empty()) {
			return cutFail("invalid range with no endpoint: -");
		}

		range.lo = 1;
		range.hi = kCutOpenEnd;
		const bool left_ok = left.empty() || parseCutBound(left, range.lo);
		const bool right_ok = right.empty() || parseCutBound(right, range.hi);
		if (!left_ok || !right_ok) return cutFail(cutBadValueMessage(mode, token));
		return true;
	}

	static bool parseCutToken(const std::string& token, CutMode mode, CutRange& range) {
		if (token.empty()) return cutFail(cutNumberedFromOneMessage(mode));
		if (!parseCutBounds(token, mode, range)) return false;

		if (range.lo == 0 || range.hi == 0) return cutFail(cutNumberedFromOneMessage(mode));
		if (range.lo > range.hi) return cutFail("invalid decreasing range");
		return true;
	}

	static bool isCutListSeparator(char c) {
		return c == ',' || c == ' ' || c == '\t';
	}

	static bool parseCutList(const std::string& list, CutMode mode, std::vector<CutRange>& ranges) {
		std::size_t start = 0;
		while (start <= list.size()) {
			std::size_t end = start;
			while (end < list.size() && !isCutListSeparator(list[end])) ++end;

			CutRange range{};
			if (!parseCutToken(list.substr(start, end - start), mode, range)) return false;
			ranges.push_back(range);
			start = end + 1;
		}

		return true;
	}

	static void mergeCutRanges(std::vector<CutRange>& ranges) {
		std::sort(ranges.begin(), ranges.end(),
			[](const CutRange& a, const CutRange& b) { return a.lo < b.lo; });

		std::vector<CutRange> merged;
		for (const CutRange& range : ranges) {
			if (!merged.empty() && range.lo <= merged.back().hi) {
				merged.back().hi = (std::max)(merged.back().hi, range.hi);
				continue;
			}

			merged.push_back(range);
		}

		ranges = std::move(merged);
	}

	static std::vector<CutRange> complementCutRanges(const std::vector<CutRange>& ranges) {
		std::vector<CutRange> inverse;
		std::size_t next = 1;
		for (const CutRange& range : ranges) {
			if (range.lo > next) inverse.push_back({ next, range.lo - 1 });
			if (range.hi == kCutOpenEnd) return inverse;
			next = range.hi + 1;
		}

		inverse.push_back({ next, kCutOpenEnd });
		return inverse;
	}

	enum CutOptionId {
		kCutBytes, kCutDelimiter, kCutFields, kCutIgnored, kCutComplement,
		kCutOnlyDelimited, kCutOutputDelimiter, kCutZero
	};

	static const std::vector<OptDef> kCutOptionDefs = {
		{ kCutBytes,           'b', "bytes",            OptArg::Required },
		{ kCutBytes,           'c', "characters",       OptArg::Required },
		{ kCutDelimiter,       'd', "delimiter",        OptArg::Required },
		{ kCutFields,          'f', "fields",           OptArg::Required },
		{ kCutIgnored,         'n', nullptr,            OptArg::None     },
		{ kCutComplement,      0,   "complement",       OptArg::None     },
		{ kCutOnlyDelimited,   's', "only-delimited",   OptArg::None     },
		{ kCutOutputDelimiter, 0,   "output-delimiter", OptArg::Required },
		{ kCutZero,            'z', "zero-terminated",  OptArg::None     },
	};

	static bool setCutList(CutOptions& options, CutMode mode, const std::string& list) {
		if (options.mode != CutMode::None) return cutFail("only one type of list may be specified");

		options.mode = mode;
		options.list = list;
		return true;
	}

	static bool setCutDelimiter(CutOptions& options, const std::string& value) {
		if (value.size() > 1) return cutFail("the delimiter must be a single character");

		options.have_delim = true;
		options.delim = value.empty() ? '\0' : value[0];
		return true;
	}

	static bool applyCutHit(const OptHit& hit, CutOptions& options) {
		switch (hit.id) {
		case kCutBytes:
			return setCutList(options, CutMode::Bytes, hit.value);
		case kCutFields:
			return setCutList(options, CutMode::Fields, hit.value);
		case kCutDelimiter:
			return setCutDelimiter(options, hit.value);
		case kCutIgnored:
			return true;
		case kCutComplement:
			options.complement = true;
			return true;
		case kCutOnlyDelimited:
			options.only_delimited = true;
			return true;
		case kCutOutputDelimiter:
			options.have_output_delim = true;
			options.output_delim = hit.value;
			return true;
		case kCutZero:
			options.terminator = '\0';
			return true;
		default:
			return false;
		}
	}

	static bool checkCutModeOptions(const CutOptions& options) {
		if (options.mode == CutMode::None) {
			return cutFail("you must specify a list of bytes, characters, or fields");
		}

		if (options.mode == CutMode::Fields) return true;
		if (options.have_delim) {
			return cutFail("an input delimiter may be specified only when operating on fields");
		}

		if (options.only_delimited) {
			return cutFail("suppressing non-delimited lines makes sense"
				" only when operating on fields");
		}

		return true;
	}

	static bool buildCutRanges(CutOptions& options) {
		if (!parseCutList(options.list, options.mode, options.ranges)) return false;

		mergeCutRanges(options.ranges);
		if (options.complement) options.ranges = complementCutRanges(options.ranges);
		return true;
	}

	static bool parseCutArgs(const std::vector<std::string>& args, CutOptions& options) {
		OptScanResult scan;
		if (!scanOptions("cut", args, kCutOptionDefs, scan)) return false;

		for (const OptHit& hit : scan.hits) {
			if (!applyCutHit(hit, options)) return false;
		}

		if (!checkCutModeOptions(options)) return false;
		if (!buildCutRanges(options)) return false;

		if (!options.have_output_delim && options.mode == CutMode::Fields) {
			options.output_delim.assign(1, options.delim);
		}

		options.files = scan.operands;
		defaultToStdin(options.files);
		return true;
	}

	// Ranges are sorted and disjoint, so the cursor only ever moves forward
	// along a line.
	struct CutCursor {
		const std::vector<CutRange>& ranges;
		std::size_t at = 0;

		bool selects(std::size_t n) {
			while (at < ranges.size() && ranges[at].hi < n) ++at;
			return at < ranges.size() && ranges[at].lo <= n;
		}

		bool startsRange(std::size_t n) const {
			return at < ranges.size() && ranges[at].lo == n;
		}

		bool exhausted() const {
			return at >= ranges.size();
		}
	};

	static void cutEmitBytes(const std::string& line, const CutOptions& options, std::string& out) {
		CutCursor cursor{ options.ranges };
		bool printed = false;
		for (std::size_t k = 0; k < line.size() && !cursor.exhausted(); ++k) {
			if (!cursor.selects(k + 1)) continue;

			if (options.have_output_delim && printed && cursor.startsRange(k + 1)) {
				out += options.output_delim;
			}

			out.push_back(line[k]);
			printed = true;
		}
	}

	static void cutEmitFields(const std::string& line, const CutOptions& options,
			std::string& out) {
		CutCursor cursor{ options.ranges };
		bool printed = false;
		std::size_t field = 1;
		std::size_t start = 0;
		while (start <= line.size() && !cursor.exhausted()) {
			std::size_t end = line.find(options.delim, start);
			if (end == std::string::npos) end = line.size();

			if (cursor.selects(field)) {
				if (printed) out += options.output_delim;
				out.append(line, start, end - start);
				printed = true;
			}

			start = end + 1;
			++field;
		}
	}

	static bool cutEmitLine(const std::string& line, const CutOptions& options) {
		std::string out;
		if (options.mode == CutMode::Bytes) {
			cutEmitBytes(line, options, out);
		} else if (line.find(options.delim) == std::string::npos) {
			if (options.only_delimited) return true;
			out = line;
		} else {
			cutEmitFields(line, options, out);
		}

		writeRecord(stdout, out, options.terminator);
		return outputOk(stdout);
	}

	static int builtin_cut(Executor& exec, const std::vector<std::string>& args) {
		CutOptions options;
		if (!parseCutArgs(args, options)) return 1;

		int status = 0;
		for (const auto& file : options.files) {
			FILE* stream = openInputStream(exec, "cut", file);
			if (stream == nullptr) {
				status = 1;
				continue;
			}

			const bool finished = forEachRecord(stream, options.terminator,
				[&](const std::string& line) { return cutEmitLine(line, options); });
			closeInputStream(stream);
			if (!finished) break;
		}

		std::fflush(stdout);
		return status;
	}

	static std::vector<FILE*> teeOpenOutputs(Executor& exec, const std::vector<std::string>& files,
			bool append) {
		std::vector<FILE*> outs;
		for (const auto& file : files) {
			FILE* stream = fopenNative(exec, file, append ? "ab" : "wb");
			if (stream == nullptr) {
				perr("tee", file + ": " + std::strerror(errno));
				continue;
			}

			outs.push_back(stream);
		}

		return outs;
	}

	static int builtin_tee(Executor& exec, const std::vector<std::string>& args) {
		bool append = false;
		std::vector<std::string> files;
		for (const auto& arg : args) {
			if (arg == "-a" || arg == "--append") append = true;
			else if (arg.size() > 1 && isOptionArg(arg)) continue;
			else files.push_back(arg);
		}

		const std::vector<FILE*> outs = teeOpenOutputs(exec, files, append);

		char chunk[kReadChunk];
		while (true) {
			const std::size_t got = std::fread(chunk, 1, sizeof(chunk), stdin);
			if (got == 0) break;

			std::fwrite(chunk, 1, got, stdout);
			for (FILE* stream : outs) std::fwrite(chunk, 1, got, stream);
		}

		for (FILE* stream : outs) std::fclose(stream);
		std::fflush(stdout);
		return 0;
	}

	struct PasteOptions {
		std::string delims = "\t";
		bool serial = false;
		std::vector<std::string> files;
	};

	static PasteOptions parsePasteArgs(const std::vector<std::string>& args) {
		PasteOptions options;
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg == "-d" && i + 1 < args.size()) {
				options.delims = args[++i];
			} else if (hasAttachedValue(arg, "-d")) {
				options.delims = arg.substr(2);
			} else if (arg == "-s") {
				options.serial = true;
			} else if (arg == "--") {
				for (++i; i < args.size(); ++i) options.files.push_back(args[i]);
			} else if (!arg.empty() && arg[0] != '-') {
				options.files.push_back(arg);
			}
		}

		defaultToStdin(options.files);
		return options;
	}

	static char pasteDelimiter(const std::string& delims, std::size_t index) {
		return delims.empty() ? '\t' : delims[index % delims.size()];
	}

	static int pasteSerial(Executor& exec, const PasteOptions& options) {
		int status = 0;
		for (const auto& file : options.files) {
			std::vector<std::string> lines;
			if (!loadLines(exec, "paste", file, lines)) {
				status = 1;
				continue;
			}

			for (std::size_t k = 0; k < lines.size(); ++k) {
				if (k != 0) std::fputc(pasteDelimiter(options.delims, k), stdout);
				std::fputs(lines[k].c_str(), stdout);
			}

			std::fputc('\n', stdout);
		}

		std::fflush(stdout);
		return status;
	}

	static int pasteParallel(Executor& exec, const PasteOptions& options) {
		std::vector<std::vector<std::string>> columns;
		std::size_t longest = 0;
		for (const auto& file : options.files) {
			std::vector<std::string> lines;
			if (!loadLines(exec, "paste", file, lines)) return 1;

			longest = (std::max)(longest, lines.size());
			columns.push_back(std::move(lines));
		}

		for (std::size_t row = 0; row < longest; ++row) {
			for (std::size_t col = 0; col < columns.size(); ++col) {
				if (col != 0) std::fputc(pasteDelimiter(options.delims, col - 1), stdout);
				if (row < columns[col].size()) std::fputs(columns[col][row].c_str(), stdout);
			}

			std::fputc('\n', stdout);
		}

		std::fflush(stdout);
		return 0;
	}

	static int builtin_paste(Executor& exec, const std::vector<std::string>& args) {
		const PasteOptions options = parsePasteArgs(args);
		if (options.serial) return pasteSerial(exec, options);
		return pasteParallel(exec, options);
	}

	static int builtin_tac(Executor& exec, const std::vector<std::string>& args) {
		std::vector<std::string> files = nonOptionArgs(args);
		defaultToStdin(files);

		int status = 0;
		for (const auto& file : files) {
			std::vector<std::string> lines;
			if (!loadLines(exec, "tac", file, lines)) {
				status = 1;
				continue;
			}

			for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
				std::fputs(it->c_str(), stdout);
				std::fputc('\n', stdout);
			}
		}

		std::fflush(stdout);
		return status;
	}

	static int builtin_rev(Executor& exec, const std::vector<std::string>& args) {
		std::vector<std::string> files = nonOptionArgs(args);
		defaultToStdin(files);

		int status = 0;
		for (const auto& file : files) {
			std::vector<std::string> lines;
			if (!loadLines(exec, "rev", file, lines)) {
				status = 1;
				continue;
			}

			for (auto& line : lines) {
				std::reverse(line.begin(), line.end());
				std::fputs(line.c_str(), stdout);
				std::fputc('\n', stdout);
			}
		}

		std::fflush(stdout);
		return status;
	}

	static int builtin_nl(Executor& exec, const std::vector<std::string>& args) {
		bool number_blank = false;
		std::vector<std::string> files;
		for (const auto& arg : args) {
			if (arg == "-ba") number_blank = true;
			else if (arg == "-bt") number_blank = false;
			else if (!arg.empty() && arg[0] != '-') files.push_back(arg);
		}

		defaultToStdin(files);

		int status = 0;
		int number = 0;
		for (const auto& file : files) {
			std::vector<std::string> lines;
			if (!loadLines(exec, "nl", file, lines)) {
				status = 1;
				continue;
			}

			for (const auto& line : lines) {
				if (line.empty() && !number_blank) {
					std::fputc('\n', stdout);
					continue;
				}

				++number;
				std::fprintf(stdout, "%6d\t%s\n", number, line.c_str());
			}
		}

		std::fflush(stdout);
		return status;
	}

	namespace find_detail {
		struct FindOptions {
			std::vector<std::string> roots;
			std::string name_pat;
			char type_filter = 0;   // 0 = any, 'f', 'd', 'l'
			int max_depth = -1;
		};
	}  // namespace find_detail

	using find_detail::FindOptions;

	static int parseFindArgs(const std::vector<std::string>& args, FindOptions& options) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg == "-name" && i + 1 < args.size()) {
				options.name_pat = args[++i];
				continue;
			}

			if (arg == "-type" && i + 1 < args.size()) {
				if (!args[i + 1].empty()) options.type_filter = args[i + 1][0];
				++i;
				continue;
			}

			if (arg == "-maxdepth" && i + 1 < args.size()) {
				if (!parseInt(args[++i], options.max_depth)) {
					perr("find", "bad -maxdepth");
					return 1;
				}

				continue;
			}

			if (!arg.empty() && arg[0] == '-') {
				std::fprintf(stderr, "wbsh: find: option %s not implemented\n", arg.c_str());
				return 1;
			}

			options.roots.push_back(arg);
		}

		if (options.roots.empty()) options.roots.push_back(".");
		return 0;
	}

	static bool isRegexMetaChar(char c) {
		return c == '.' || c == '+' || c == '(' || c == ')' || c == '|'
			|| c == '^' || c == '$' || c == '{' || c == '}' || c == '\\';
	}

	static std::string globToRegex(const std::string& glob) {
		std::string pattern;
		for (char c : glob) {
			if (c == '*') {
				pattern += ".*";
			} else if (c == '?') {
				pattern += ".";
			} else if (isRegexMetaChar(c)) {
				pattern.push_back('\\');
				pattern.push_back(c);
			} else {
				pattern.push_back(c);
			}
		}

		return pattern;
	}

	static bool findNameMatches(const std::string& name_pat, const std::string& name) {
		if (name_pat.empty()) return true;

		std::regex re;
		if (!compileRegex(re, "^(?:" + globToRegex(name_pat) + ")$")) return false;
		return searchRegex(name, re);
	}

	static bool findTypeMatches(char type_filter, const fs::path& path) {
		if (type_filter == 0) return true;

		std::error_code ec;
		const auto status = fs::symlink_status(path, ec);
		if (ec) return false;
		if (type_filter == 'l') return fs::is_symlink(status);
		if (type_filter == 'd') return fs::is_directory(status);
		if (type_filter == 'f') return fs::is_regular_file(status);
		return true;
	}

	static void printFindMatch(Executor& exec, const fs::path& path) {
		std::printf("%s\n", exec.pathConv().toPosix(pathToUtf8(path)).c_str());
	}

	static void printFindRoot(Executor& exec, const FindOptions& options, const fs::path& native,
			const std::string& root_arg) {
		const std::string basename = pathToUtf8(native.filename());
		const std::string root_basename = basename.empty() ? root_arg : basename;
		if (!findTypeMatches(options.type_filter, native)) return;
		if (!findNameMatches(options.name_pat, root_basename)) return;

		printFindMatch(exec, native);
	}

	static void walkAndPrintFindMatches(Executor& exec, const FindOptions& options,
			const fs::path& native, const std::string& root_arg) {
		printFindRoot(exec, options, native, root_arg);

		std::error_code ec;
		if (!fs::is_directory(native, ec)) return;

		fs::recursive_directory_iterator it(native,
			fs::directory_options::skip_permission_denied, ec);
		if (ec) return;

		for (auto cur = it; cur != fs::recursive_directory_iterator(); cur.increment(ec)) {
			if (ec) break;
			if (options.max_depth >= 0 && cur.depth() >= options.max_depth) {
				cur.disable_recursion_pending();
			}

			const fs::path path = cur->path();
			if (!findTypeMatches(options.type_filter, path)) continue;
			if (!findNameMatches(options.name_pat, pathToUtf8(path.filename()))) continue;
			printFindMatch(exec, path);
		}
	}

	static int builtin_find(Executor& exec, const std::vector<std::string>& args) {
		FindOptions options;
		if (int status = parseFindArgs(args, options); status != 0) return status;

		int status = 0;
		for (const auto& root : options.roots) {
			std::error_code ec;
			const fs::path native = toNative(exec, root);
			if (!fs::exists(native, ec)) {
				std::fprintf(stderr, "wbsh: find: %s: no such file or directory\n", root.c_str());
				status = 1;
				continue;
			}

			walkAndPrintFindMatches(exec, options, native, root);
		}

		std::fflush(stdout);
		return status;
	}

	struct FoldOptions {
		int width = kFoldDefaultWidth;
		bool wrap_spaces = false;
		std::vector<std::string> files;
	};

	// -b and -c are both accepted: bytes and characters are the same
	// thing in the ASCII path.
	static bool parseFoldArgs(const std::vector<std::string>& args, FoldOptions& options) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg == "-b" || arg == "--bytes" || arg == "-c" || arg == "--characters") {
				continue;
			} else if (arg == "-s" || arg == "--spaces") {
				options.wrap_spaces = true;
			} else if (arg == "-w" && i + 1 < args.size()) {
				parseInt(args[++i], options.width);
			} else if (hasAttachedValue(arg, "-w")) {
				parseInt(arg.substr(2), options.width);
			} else if (isNumericOption(arg)) {
				parseInt(arg.substr(1), options.width);
			} else if (isOptionArg(arg)) {
				perr("fold", "unknown option: " + arg);
				return false;
			} else {
				options.files.push_back(arg);
			}
		}

		if (options.width <= 0) {
			perr("fold", "width must be > 0");
			return false;
		}

		return true;
	}

	static void foldEmitLine(const std::string& line, int width, bool wrap_spaces) {
		std::size_t pos = 0;
		while (pos < line.size()) {
			std::size_t take = (std::min<std::size_t>)(width, line.size() - pos);
			if (wrap_spaces && take < line.size() - pos) {
				const std::size_t space = line.rfind(' ', pos + take - 1);
				if (space != std::string::npos && space >= pos) take = space - pos + 1;
			}

			std::fwrite(line.data() + pos, 1, take, stdout);
			std::putchar('\n');
			pos += take;
		}

		if (line.empty()) std::putchar('\n');
	}

	static void foldRunOnStream(FILE* stream, const FoldOptions& options) {
		forEachLine(stream, [&](const std::string& line) {
			foldEmitLine(line, options.width, options.wrap_spaces);
		});
	}

	static int builtin_fold(Executor& exec, const std::vector<std::string>& args) {
		FoldOptions options;
		if (!parseFoldArgs(args, options)) return 1;

		return runOnInputs(exec, "fold", options.files,
			[&](FILE* stream) { foldRunOnStream(stream, options); });
	}

	struct ColumnOptions {
		bool table = false;
		std::string sep = kColumnDefaultSeparators;
		std::string out_sep = kColumnDefaultOutputSeparator;
		std::vector<std::string> files;
	};

	static bool parseColumnArgs(const std::vector<std::string>& args, ColumnOptions& options) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg == "-t" || arg == "--table") {
				options.table = true;
			} else if ((arg == "-s" || arg == "--separator") && i + 1 < args.size()) {
				options.sep = args[++i];
			} else if (hasAttachedValue(arg, "-s")) {
				options.sep = arg.substr(2);
			} else if ((arg == "-o" || arg == "--output-separator") && i + 1 < args.size()) {
				options.out_sep = args[++i];
			} else if (isOptionArg(arg)) {
				perr("column", "unknown option: " + arg);
				return false;
			} else {
				options.files.push_back(arg);
			}
		}

		return true;
	}

	// With the default separators a run of blanks counts as one
	// separator, so the empty fields it would produce are dropped.
	static std::vector<std::string> columnSplitLine(const std::string& line,
			const std::string& sep) {
		const bool collapse_runs = sep == kColumnDefaultSeparators;
		std::vector<std::string> fields;
		std::string current;
		for (char c : line) {
			if (sep.find(c) == std::string::npos) {
				current.push_back(c);
				continue;
			}

			fields.push_back(std::move(current));
			current.clear();
			if (collapse_runs) {
				while (!fields.empty() && fields.back().empty()) fields.pop_back();
			}
		}

		fields.push_back(std::move(current));
		return fields;
	}

	static void columnReadStream(FILE* stream, const std::string& sep,
			std::vector<std::vector<std::string>>& rows) {
		forEachLine(stream, [&](const std::string& line) {
			rows.push_back(columnSplitLine(line, sep));
		});
	}

	static void columnEmitPlain(const std::vector<std::vector<std::string>>& rows) {
		for (const auto& row : rows) {
			for (std::size_t i = 0; i < row.size(); ++i) {
				if (i != 0) std::fputs(" ", stdout);
				std::fputs(row[i].c_str(), stdout);
			}

			std::putchar('\n');
		}
	}

	static std::vector<std::size_t> columnWidths(
			const std::vector<std::vector<std::string>>& rows) {
		std::size_t cols = 0;
		for (const auto& row : rows) cols = (std::max)(cols, row.size());

		std::vector<std::size_t> widths(cols, 0);
		for (const auto& row : rows) {
			for (std::size_t i = 0; i < row.size(); ++i) {
				widths[i] = (std::max)(widths[i], row[i].size());
			}
		}

		return widths;
	}

	static void columnEmitTable(const std::vector<std::vector<std::string>>& rows,
			const std::string& out_sep) {
		const std::vector<std::size_t> widths = columnWidths(rows);
		for (const auto& row : rows) {
			for (std::size_t i = 0; i < row.size(); ++i) {
				if (i != 0) std::fputs(out_sep.c_str(), stdout);
				std::fputs(row[i].c_str(), stdout);
				if (i + 1 < row.size()) {
					for (std::size_t k = row[i].size(); k < widths[i]; ++k) std::putchar(' ');
				}
			}

			std::putchar('\n');
		}
	}

	static int builtin_column(Executor& exec, const std::vector<std::string>& args) {
		ColumnOptions options;
		if (!parseColumnArgs(args, options)) return 1;

		std::vector<std::vector<std::string>> rows;
		const int status = runOnInputs(exec, "column", options.files,
			[&](FILE* stream) { columnReadStream(stream, options.sep, rows); });
		if (status != 0) return status;

		if (options.table) columnEmitTable(rows, options.out_sep);
		else               columnEmitPlain(rows);
		return 0;
	}

	struct ExpandOptions {
		int tabstop = kDefaultTabstop;
		std::vector<std::string> files;
	};

	static bool parseExpandArgs(const std::vector<std::string>& args, ExpandOptions& options) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if ((arg == "-t" || arg == "--tabs") && i + 1 < args.size()) {
				parseInt(args[++i], options.tabstop);
			} else if (hasAttachedValue(arg, "-t")) {
				parseInt(arg.substr(2), options.tabstop);
			} else if (isNumericOption(arg)) {
				parseInt(arg.substr(1), options.tabstop);
			} else if (isOptionArg(arg)) {
				perr("expand", "unknown option: " + arg);
				return false;
			} else {
				options.files.push_back(arg);
			}
		}

		if (options.tabstop <= 0) options.tabstop = kDefaultTabstop;
		return true;
	}

	static void expandStream(FILE* stream, int tabstop) {
		int col = 0;
		int c;
		while ((c = std::fgetc(stream)) != EOF) {
			if (c == '\t') {
				const int spaces = tabstop - (col % tabstop);
				for (int i = 0; i < spaces; ++i) std::putchar(' ');
				col += spaces;
			} else if (c == '\n') {
				std::putchar('\n');
				col = 0;
			} else {
				std::putchar(c);
				++col;
			}
		}
	}

	static int builtin_expand(Executor& exec, const std::vector<std::string>& args) {
		ExpandOptions options;
		if (!parseExpandArgs(args, options)) return 1;

		return runOnInputs(exec, "expand", options.files,
			[&](FILE* stream) { expandStream(stream, options.tabstop); });
	}

	struct UnexpandOptions {
		int tabstop = kDefaultTabstop;
		bool all = false;
		std::vector<std::string> files;
	};

	static bool parseUnexpandArgs(const std::vector<std::string>& args, UnexpandOptions& options) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg == "-a" || arg == "--all") {
				options.all = true;
			} else if ((arg == "-t" || arg == "--tabs") && i + 1 < args.size()) {
				parseInt(args[++i], options.tabstop);
			} else if (hasAttachedValue(arg, "-t")) {
				parseInt(arg.substr(2), options.tabstop);
			} else if (isOptionArg(arg)) {
				perr("unexpand", "unknown option: " + arg);
				return false;
			} else {
				options.files.push_back(arg);
			}
		}

		if (options.tabstop <= 0) options.tabstop = kDefaultTabstop;
		return true;
	}

	static int nextTabStop(int col, int tabstop) {
		return ((col / tabstop) + 1) * tabstop;
	}

	static void emitSpacesAsTabs(int from_col, int to_col, int tabstop) {
		int col = from_col;
		int next_tab = nextTabStop(from_col, tabstop);
		while (next_tab <= to_col) {
			std::putchar('\t');
			col = next_tab;
			next_tab += tabstop;
		}

		while (col < to_col) {
			std::putchar(' ');
			++col;
		}
	}

	static void unexpandEmitLine(const std::string& line, int tabstop, bool all) {
		std::size_t i = 0;
		int col = 0;
		bool past_indent = false;
		while (i < line.size()) {
			if (!past_indent || all) {
				const int run_start = col;
				while (i < line.size() && line[i] == ' ') {
					++i;
					++col;
				}

				if (col > run_start) {
					emitSpacesAsTabs(run_start, col, tabstop);
					continue;
				}
			}

			const char ch = line[i++];
			if (ch != ' ' && ch != '\t') past_indent = true;
			std::putchar(ch);
			if (ch == '\t') col = nextTabStop(col, tabstop);
			else ++col;
		}
	}

	static void unexpandRunOnStream(FILE* stream, const UnexpandOptions& options) {
		std::string line;
		int c;
		while ((c = std::fgetc(stream)) != EOF) {
			if (c != '\n') {
				line.push_back(static_cast<char>(c));
				continue;
			}

			unexpandEmitLine(line, options.tabstop, options.all);
			std::putchar('\n');
			line.clear();
		}

		if (!line.empty()) unexpandEmitLine(line, options.tabstop, options.all);
	}

	static int builtin_unexpand(Executor& exec, const std::vector<std::string>& args) {
		UnexpandOptions options;
		if (!parseUnexpandArgs(args, options)) return 1;

		return runOnInputs(exec, "unexpand", options.files,
			[&](FILE* stream) { unexpandRunOnStream(stream, options); });
	}

	static const char* const kCommSuppressOptions[] = {
		"-1", "-2", "-3", "-12", "-21", "-13", "-31", "-23", "-32", "-123" };

	static bool isCommSuppressOption(const std::string& arg) {
		for (const char* accepted : kCommSuppressOptions) {
			if (arg == accepted) return true;
		}

		return false;
	}

	static void applyCommSuppress(const std::string& arg, bool (&suppress)[3]) {
		for (std::size_t k = 1; k < arg.size(); ++k) suppress[arg[k] - '1'] = true;
	}

	static std::vector<std::string> commLoadLines(Executor& exec, const std::string& path) {
		std::vector<std::string> lines;
		FILE* stream = (path == kStdinPath) ? stdin : fopenNative(exec, path, "rb");
		if (stream == nullptr) {
			perr("comm", path, std::error_code(errno, std::system_category()));
			return lines;
		}

		forEachLine(stream, [&](const std::string& line) { lines.push_back(line); });
		if (stream != stdin) std::fclose(stream);
		return lines;
	}

	static void commEmit(const bool (&suppress)[3], int col, const std::string& line) {
		if (suppress[col]) return;

		for (int k = 0; k < col; ++k) std::putchar('\t');
		std::fputs(line.c_str(), stdout);
		std::putchar('\n');
	}

	static void commMerge(const bool (&suppress)[3], const std::vector<std::string>& left,
			const std::vector<std::string>& right) {
		std::size_t i = 0;
		std::size_t j = 0;
		while (i < left.size() && j < right.size()) {
			if (left[i] == right[j]) {
				commEmit(suppress, 2, left[i]);
				++i;
				++j;
			} else if (left[i] < right[j]) {
				commEmit(suppress, 0, left[i]);
				++i;
			} else {
				commEmit(suppress, 1, right[j]);
				++j;
			}
		}

		while (i < left.size()) commEmit(suppress, 0, left[i++]);
		while (j < right.size()) commEmit(suppress, 1, right[j++]);
	}

	static int builtin_comm(Executor& exec, const std::vector<std::string>& args) {
		bool suppress[3] = { false, false, false };
		std::vector<std::string> files;
		for (const auto& arg : args) {
			if (isCommSuppressOption(arg)) {
				applyCommSuppress(arg, suppress);
				continue;
			}

			if (isOptionArg(arg)) {
				perr("comm", "unknown option: " + arg);
				return 1;
			}

			files.push_back(arg);
		}

		if (files.size() != 2) {
			perr("comm", "usage: comm [opts] FILE1 FILE2");
			return 1;
		}

		const std::vector<std::string> left = commLoadLines(exec, files[0]);
		const std::vector<std::string> right = commLoadLines(exec, files[1]);
		commMerge(suppress, left, right);
		return 0;
	}

	struct XargsOptions {
		int n_per = -1;   // -1 means "all in one batch"
		std::vector<std::string> cmd;
		std::string replace_str;
	};

	static bool parseXargsArgs(const std::vector<std::string>& args, XargsOptions& options) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg == "-n" && i + 1 < args.size()) {
				if (!parseInt(args[++i], options.n_per)) return false;
				continue;
			}

			if (hasAttachedValue(arg, "-n")) {
				if (!parseInt(arg.substr(2), options.n_per)) return false;
				continue;
			}

			if (arg == "-I" && i + 1 < args.size()) {
				options.replace_str = args[++i];
				continue;
			}

			if (hasAttachedValue(arg, "-I")) {
				options.replace_str = arg.substr(2);
				continue;
			}

			if (arg == "--") {
				for (++i; i < args.size(); ++i) options.cmd.push_back(args[i]);
				break;
			}

			options.cmd.push_back(arg);
		}

		return true;
	}

	static std::string xargsSubstitute(const std::string& word, const std::string& replace_str,
			const std::string& item) {
		std::string out;
		std::size_t pos = 0;
		while (true) {
			const std::size_t hit = word.find(replace_str, pos);
			if (hit == std::string::npos) {
				out += word.substr(pos);
				return out;
			}

			out += word.substr(pos, hit - pos);
			out += item;
			pos = hit + replace_str.size();
		}
	}

	static std::vector<std::string> xargsReadItems() {
		std::vector<std::string> items;
		std::string current;
		int c;
		while ((c = std::fgetc(stdin)) != EOF) {
			if (std::isspace(static_cast<unsigned char>(c)) == 0) {
				current.push_back(static_cast<char>(c));
				continue;
			}

			if (!current.empty()) {
				items.push_back(std::move(current));
				current.clear();
			}
		}

		if (!current.empty()) items.push_back(std::move(current));
		return items;
	}

	static std::string xargsQuoteArgv(const std::vector<std::string>& argv) {
		std::string line;
		for (std::size_t k = 0; k < argv.size(); ++k) {
			if (k != 0) line.push_back(' ');
			line.push_back('\'');
			for (char ch : argv[k]) {
				if (ch == '\'') line += "'\\''";
				else line.push_back(ch);
			}

			line.push_back('\'');
		}

		return line;
	}

	static int xargsInvokeBatch(Executor& exec, const std::vector<std::string>& cmd,
			std::vector<std::string> batch, bool skip_empty) {
		if (batch.empty() && skip_empty) return 0;

		std::vector<std::string> argv = cmd;
		for (auto& item : batch) argv.push_back(std::move(item));
		if (argv.empty()) return 0;

		if (exec.isFunction(argv[0]) || exec.isBuiltin(argv[0])) {
			const std::vector<std::string> call_args(argv.begin() + 1, argv.end());
			if (exec.isBuiltin(argv[0])) return exec.callBuiltin(argv[0], call_args);
			return exec.callFunction(argv[0], call_args);
		}

		return exec.executeText(xargsQuoteArgv(argv), "<xargs>");
	}

	static int xargsRunReplacing(Executor& exec, const XargsOptions& options,
			const std::vector<std::string>& items) {
		int status = 0;
		for (const auto& item : items) {
			std::vector<std::string> argv;
			for (const auto& word : options.cmd) {
				argv.push_back(xargsSubstitute(word, options.replace_str, item));
			}

			const int batch_status = xargsInvokeBatch(exec, argv, {}, false);
			if (batch_status != 0) status = batch_status;
		}

		return status;
	}

	static int xargsRunBatches(Executor& exec, const XargsOptions& options,
			const std::vector<std::string>& items) {
		const std::size_t per_batch = static_cast<std::size_t>(options.n_per);
		int status = 0;
		for (std::size_t k = 0; k < items.size(); k += per_batch) {
			std::vector<std::string> batch;
			for (std::size_t j = 0; j < per_batch && k + j < items.size(); ++j) {
				batch.push_back(items[k + j]);
			}

			const int batch_status = xargsInvokeBatch(exec, options.cmd, std::move(batch), false);
			if (batch_status != 0) status = batch_status;
		}

		return status;
	}

	static int builtin_xargs(Executor& exec, const std::vector<std::string>& args) {
		XargsOptions options;
		if (!parseXargsArgs(args, options)) return 1;
		if (options.cmd.empty()) options.cmd.push_back("echo");

		const std::vector<std::string> items = xargsReadItems();
		if (!options.replace_str.empty()) return xargsRunReplacing(exec, options, items);
		if (options.n_per > 0) return xargsRunBatches(exec, options, items);
		return xargsInvokeBatch(exec, options.cmd, items, true);
	}

	void registerTextBuiltins(Executor& exec) {
		exec.registerBuiltin("sort",     builtin_sort);
		exec.registerBuiltin("uniq",     builtin_uniq);
		exec.registerBuiltin("tr",       builtin_tr);
		exec.registerBuiltin("cut",      builtin_cut);
		exec.registerBuiltin("tee",      builtin_tee);
		exec.registerBuiltin("paste",    builtin_paste);
		exec.registerBuiltin("tac",      builtin_tac);
		exec.registerBuiltin("rev",      builtin_rev);
		exec.registerBuiltin("nl",       builtin_nl);
		exec.registerBuiltin("fold",     builtin_fold);
		exec.registerBuiltin("column",   builtin_column);
		exec.registerBuiltin("expand",   builtin_expand);
		exec.registerBuiltin("unexpand", builtin_unexpand);
		exec.registerBuiltin("comm",     builtin_comm);
		exec.registerBuiltin("find",     builtin_find);
		exec.registerBuiltin("xargs",    builtin_xargs);
		exec.registerBuiltin("awk",      builtin_awk);
		exec.registerBuiltin("gawk",     builtin_awk);
	}

}  // namespace wbsh

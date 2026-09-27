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

	// Calls `on_line` for every newline-terminated line and once more for
	// a trailing unterminated one; an empty trailing buffer is not a line.
	template <typename Fn>
	static void forEachLine(FILE* stream, Fn on_line) {
		std::string line;
		int c;
		while ((c = std::fgetc(stream)) != EOF) {
			if (c != '\n') {
				line.push_back(static_cast<char>(c));
				continue;
			}

			on_line(line);
			line.clear();
		}

		if (!line.empty()) on_line(line);
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

	struct UniqOptions {
		bool count = false;
		bool dups_only = false;
		bool uniques_only = false;
		bool fold = false;
		std::vector<std::string> files;
	};

	static void applyUniqCluster(const std::string& cluster, UniqOptions& options) {
		for (std::size_t k = 1; k < cluster.size(); ++k) {
			switch (cluster[k]) {
			case 'c': options.count = true; break;
			case 'd': options.dups_only = true; break;
			case 'u': options.uniques_only = true; break;
			case 'i': options.fold = true; break;
			}
		}
	}

	static UniqOptions parseUniqArgs(const std::vector<std::string>& args) {
		UniqOptions options;
		for (const auto& arg : args) {
			if (arg == "-c" || arg == "--count") {
				options.count = true;
			} else if (arg == "-d" || arg == "--repeated") {
				options.dups_only = true;
			} else if (arg == "-u" || arg == "--unique") {
				options.uniques_only = true;
			} else if (arg == "-i" || arg == "--ignore-case") {
				options.fold = true;
			} else if (isShortCluster(arg)) {
				applyUniqCluster(arg, options);
			} else {
				options.files.push_back(arg);
			}
		}

		defaultToStdin(options.files);
		return options;
	}

	static void uniqEmitRun(const UniqOptions& options, const std::string& line, int run_length) {
		if (options.dups_only && run_length < 2) return;
		if (options.uniques_only && run_length > 1) return;

		if (options.count) std::printf("%7d %s\n", run_length, line.c_str());
		else               std::printf("%s\n", line.c_str());
	}

	static int builtin_uniq(Executor& exec, const std::vector<std::string>& args) {
		const UniqOptions options = parseUniqArgs(args);

		std::vector<std::string> lines;
		for (const auto& file : options.files) {
			if (!loadLinesWithErrno(exec, "uniq", file, lines)) return 1;
		}

		std::size_t i = 0;
		while (i < lines.size()) {
			std::size_t j = i + 1;
			while (j < lines.size() && linesEqual(lines[j], lines[i], options.fold)) ++j;

			uniqEmitRun(options, lines[i], static_cast<int>(j - i));
			i = j;
		}

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
		std::vector<std::string> sets;
	};

	static void applyTrCluster(const std::string& cluster, TrOptions& options) {
		for (std::size_t k = 1; k < cluster.size(); ++k) {
			if (cluster[k] == 'd') options.delete_mode = true;
			else if (cluster[k] == 's') options.squeeze = true;
			else if (cluster[k] == 'c' || cluster[k] == 'C') options.complement = true;
		}
	}

	static TrOptions parseTrArgs(const std::vector<std::string>& args) {
		TrOptions options;
		for (const auto& arg : args) {
			if (arg == "-d") options.delete_mode = true;
			else if (arg == "-s") options.squeeze = true;
			else if (arg == "-c" || arg == "-C" || arg == "--complement") options.complement = true;
			else if (arg.size() > 1 && isOptionArg(arg)) applyTrCluster(arg, options);
			else options.sets.push_back(arg);
		}

		return options;
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
		const TrOptions options = parseTrArgs(args);
		const bool needs_two_sets = !options.delete_mode && !options.squeeze;
		if (options.sets.empty() || (needs_two_sets && options.sets.size() < 2)) {
			perr("tr", "usage: tr [-cds] SET1 [SET2]");
			return 2;
		}

		const std::string set1 = trExpandSet(options.sets[0]);
		const std::string set2 =
			(options.sets.size() > 1) ? trExpandSet(options.sets[1]) : std::string();
		const std::vector<bool> in_set1 = buildTrMembership(set1, options.complement);
		const std::vector<unsigned char> map = buildTrMap(set1,
			options.delete_mode ? std::string() : set2, in_set1, options.complement);

		trStream(options, in_set1, map);
		std::fflush(stdout);
		return 0;
	}

	// A range is [first, last], 1-indexed; last == -1 means "to end".
	static bool parseCutRange(const std::string& token, int& first, int& last) {
		const std::size_t dash = token.find('-');
		if (dash == std::string::npos) {
			if (!parseInt(token, first)) return false;
			last = first;
			return true;
		}

		if (dash == 0) {
			first = 1;
			return parseInt(token.substr(1), last);
		}

		if (dash + 1 == token.size()) {
			last = -1;
			return parseInt(token.substr(0, dash), first);
		}

		return parseInt(token.substr(0, dash), first) && parseInt(token.substr(dash + 1), last);
	}

	struct CutSpec {
		std::vector<std::pair<int, int>> ranges;

		bool parse(const std::string& spec) {
			std::size_t i = 0;
			while (i < spec.size()) {
				std::string token;
				while (i < spec.size() && spec[i] != ',') token.push_back(spec[i++]);
				if (i < spec.size() && spec[i] == ',') ++i;
				if (token.empty()) continue;

				int first = 0;
				int last = 0;
				if (!parseCutRange(token, first, last)) return false;
				ranges.emplace_back(first, last);
			}

			return !ranges.empty();
		}

		bool contains(int n) const {
			for (const auto& range : ranges) {
				if (n >= range.first && (range.second == -1 || n <= range.second)) return true;
			}

			return false;
		}
	};

	struct CutOptions {
		char delim = '\t';
		std::string field_spec;
		std::string char_spec;
		bool only_delim_lines = false;
		std::vector<std::string> files;
	};

	static CutOptions parseCutArgs(const std::vector<std::string>& args) {
		CutOptions options;
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg == "-d" && i + 1 < args.size()) {
				if (!args[i + 1].empty()) options.delim = args[i + 1][0];
				++i;
			} else if (hasAttachedValue(arg, "-d")) {
				options.delim = arg[2];
			} else if (arg == "-f" && i + 1 < args.size()) {
				options.field_spec = args[++i];
			} else if (hasAttachedValue(arg, "-f")) {
				options.field_spec = arg.substr(2);
			} else if (arg == "-c" && i + 1 < args.size()) {
				options.char_spec = args[++i];
			} else if (hasAttachedValue(arg, "-c")) {
				options.char_spec = arg.substr(2);
			} else if (arg == "-s") {
				options.only_delim_lines = true;
			} else if (arg == "--") {
				for (++i; i < args.size(); ++i) options.files.push_back(args[i]);
			} else if (!arg.empty() && arg[0] != '-') {
				options.files.push_back(arg);
			}
		}

		return options;
	}

	static std::vector<std::string> splitOnChar(const std::string& line, char delim) {
		std::vector<std::string> fields;
		std::string current;
		for (char c : line) {
			if (c != delim) {
				current.push_back(c);
				continue;
			}

			fields.push_back(std::move(current));
			current.clear();
		}

		fields.push_back(std::move(current));
		return fields;
	}

	static void cutEmitFieldLine(const std::string& line, const CutOptions& options,
			const CutSpec& spec) {
		if (line.find(options.delim) == std::string::npos) {
			if (!options.only_delim_lines) std::printf("%s\n", line.c_str());
			return;
		}

		const std::vector<std::string> fields = splitOnChar(line, options.delim);
		std::string out;
		bool first = true;
		for (std::size_t k = 0; k < fields.size(); ++k) {
			if (!spec.contains(static_cast<int>(k + 1))) continue;

			if (!first) out.push_back(options.delim);
			out += fields[k];
			first = false;
		}

		std::printf("%s\n", out.c_str());
	}

	static void cutEmitCharLine(const std::string& line, const CutSpec& spec) {
		std::string out;
		for (std::size_t k = 0; k < line.size(); ++k) {
			if (spec.contains(static_cast<int>(k + 1))) out.push_back(line[k]);
		}

		std::printf("%s\n", out.c_str());
	}

	static int builtin_cut(Executor& exec, const std::vector<std::string>& args) {
		CutOptions options = parseCutArgs(args);
		if (options.field_spec.empty() && options.char_spec.empty()) {
			perr("cut", "specify -f or -c");
			return 1;
		}

		CutSpec spec;
		if (!spec.parse(options.field_spec.empty() ? options.char_spec : options.field_spec)) {
			perr("cut", "bad field/char spec");
			return 1;
		}

		const bool by_field = !options.field_spec.empty();
		defaultToStdin(options.files);

		int status = 0;
		for (const auto& file : options.files) {
			std::vector<std::string> lines;
			if (!loadLinesWithErrno(exec, "cut", file, lines)) {
				status = 1;
				continue;
			}

			for (const auto& line : lines) {
				if (by_field) cutEmitFieldLine(line, options, spec);
				else          cutEmitCharLine(line, spec);
			}
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

	namespace grep_detail {
		struct GrepOptions {
			bool icase = false;
			bool invert = false;
			bool line_no = false;
			bool count_only = false;
			bool fixed = false;
			bool list_only = false;
			bool quiet = false;
			bool recursive = false;
			bool whole_line = false;
			std::string pattern;
			std::vector<std::string> files;
		};

		struct GrepRun {
			const GrepOptions* options = nullptr;
			const std::regex*  re      = nullptr;
			std::string        lower_pattern;
			bool               show_filename = false;
			bool               any_match = false;
		};

		// A null field is a flag that is accepted but changes nothing.
		struct GrepFlag {
			const char* name;
			bool GrepOptions::* field;
		};
	}  // namespace grep_detail

	using grep_detail::GrepOptions;
	using grep_detail::GrepRun;
	using grep_detail::GrepFlag;

	static bool applyGrepShortFlag(char flag, GrepOptions& options) {
		switch (flag) {
		case 'i': options.icase = true; return true;
		case 'v': options.invert = true; return true;
		case 'n': options.line_no = true; return true;
		case 'c': options.count_only = true; return true;
		case 'F': options.fixed = true; return true;
		case 'E': return true;                 // ERE — accept, no-op
		case 'l': options.list_only = true; return true;
		case 'q': options.quiet = true; return true;
		case 'r': case 'R': options.recursive = true; return true;
		case 'x': options.whole_line = true; return true;
		default:  return false;
		}
	}

	static const GrepFlag kGrepFlags[] = {
		{ "-i", &GrepOptions::icase },      { "--ignore-case",     &GrepOptions::icase },
		{ "-v", &GrepOptions::invert },     { "--invert-match",    &GrepOptions::invert },
		{ "-n", &GrepOptions::line_no },    { "--line-number",     &GrepOptions::line_no },
		{ "-c", &GrepOptions::count_only }, { "--count",           &GrepOptions::count_only },
		{ "-F", &GrepOptions::fixed },      { "--fixed-strings",   &GrepOptions::fixed },
		{ "-E", nullptr },                  { "--extended-regexp", nullptr },
		{ "-l", &GrepOptions::list_only },
		{ "-q", &GrepOptions::quiet },      { "--quiet",           &GrepOptions::quiet },
		{ "--silent", &GrepOptions::quiet },
		{ "-r", &GrepOptions::recursive },  { "-R",                &GrepOptions::recursive },
		{ "--recursive", &GrepOptions::recursive },
		{ "-x", &GrepOptions::whole_line }, { "--line-regexp",     &GrepOptions::whole_line },
	};

	static bool applyGrepLongFlag(const std::string& arg, GrepOptions& options) {
		for (const GrepFlag& flag : kGrepFlags) {
			if (arg != flag.name) continue;

			if (flag.field != nullptr) options.*flag.field = true;
			return true;
		}

		return false;
	}

	static void addGrepOperand(const std::string& arg, GrepOptions& options) {
		if (options.pattern.empty()) options.pattern = arg;
		else options.files.push_back(arg);
	}

	static bool applyGrepCluster(const std::string& cluster, GrepOptions& options) {
		for (std::size_t k = 1; k < cluster.size(); ++k) {
			if (applyGrepShortFlag(cluster[k], options)) continue;

			std::fprintf(stderr, "wbsh: grep: unknown option -%c\n", cluster[k]);
			return false;
		}

		return true;
	}

	static int parseGrepArgs(const std::vector<std::string>& args, GrepOptions& options) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg == "--") {
				for (++i; i < args.size(); ++i) addGrepOperand(args[i], options);
				break;
			}

			if (arg == "-e" && i + 1 < args.size()) {
				options.pattern = args[++i];
				continue;
			}

			if (applyGrepLongFlag(arg, options)) continue;
			if (isShortCluster(arg)) {
				if (!applyGrepCluster(arg, options)) return 2;
				continue;
			}

			addGrepOperand(arg, options);
		}

		return 0;
	}

	static void expandGrepDirectory(const fs::path& native, std::vector<std::string>& out) {
		std::error_code ec;
		fs::recursive_directory_iterator it(native,
			fs::directory_options::skip_permission_denied, ec);
		if (ec) return;

		for (auto cur = it; cur != fs::recursive_directory_iterator(); cur.increment(ec)) {
			if (ec) break;

			std::error_code file_ec;
			if (!cur->is_regular_file(file_ec)) continue;

			std::string path = pathToUtf8(cur->path());
			std::replace(path.begin(), path.end(), '\\', '/');
			out.push_back(std::move(path));
		}
	}

	static std::vector<std::string> expandGrepRecursive(Executor& exec,
			const std::vector<std::string>& files) {
		std::vector<std::string> out;
		for (const auto& file : files) {
			if (file == kStdinPath) {
				out.push_back(file);
				continue;
			}

			const fs::path native = toNative(exec, file);
			std::error_code ec;
			if (!fs::is_directory(native, ec)) {
				out.push_back(file);
				continue;
			}

			expandGrepDirectory(native, out);
		}

		return out;
	}

	static bool compileGrepRegex(const GrepOptions& options, std::regex& re) {
		if (options.fixed) return true;

		std::regex::flag_type flags = std::regex::ECMAScript;
		if (options.icase) flags |= std::regex::icase;
		if (compileRegex(re, options.pattern, flags)) return true;

		perr("grep", "bad pattern: " + options.pattern);
		return false;
	}

	static void prepareGrepRun(const GrepOptions& options, const std::regex& re, GrepRun& run) {
		run.options = &options;
		run.re = &re;
		run.show_filename = options.files.size() > 1;
		if (options.fixed && options.icase) run.lower_pattern = asciiLower(options.pattern);
	}

	static bool matchesFixedWholeLine(const GrepOptions& options, const std::string& line) {
		if (line.size() != options.pattern.size()) return false;
		if (!options.icase) return line == options.pattern;
		return equalFold(line, options.pattern);
	}

	static bool matchesGrepLine(const GrepRun& run, const std::string& line) {
		const GrepOptions& options = *run.options;
		if (!options.fixed) {
			if (options.whole_line) return std::regex_match(line, *run.re);
			return std::regex_search(line, *run.re);
		}

		if (options.whole_line) return matchesFixedWholeLine(options, line);
		if (!options.icase) return line.find(options.pattern) != std::string::npos;
		return asciiLower(line).find(run.lower_pattern) != std::string::npos;
	}

	static void printGrepMatch(const GrepRun& run, const std::string& file, std::size_t line_index,
			const std::string& line) {
		if (run.show_filename) std::printf("%s:", file.c_str());
		if (run.options->line_no) std::printf("%zu:", line_index + 1);
		std::printf("%s\n", line.c_str());
	}

	static int grepOneFile(Executor& exec, GrepRun& run, const std::string& file) {
		const GrepOptions& options = *run.options;
		std::vector<std::string> lines;
		if (!loadLinesWithErrno(exec, "grep", file, lines)) return 2;

		int file_count = 0;
		for (std::size_t k = 0; k < lines.size(); ++k) {
			bool matched = matchesGrepLine(run, lines[k]);
			if (options.invert) matched = !matched;
			if (!matched) continue;

			++file_count;
			run.any_match = true;
			if (options.quiet) return 0;
			if (options.list_only) {
				std::printf("%s\n", file.c_str());
				return 0;
			}

			if (options.count_only) continue;
			printGrepMatch(run, file, k, lines[k]);
		}

		if (options.count_only && !options.list_only && !options.quiet) {
			if (run.show_filename) std::printf("%s:", file.c_str());
			std::printf("%d\n", file_count);
		}

		return 0;
	}

	static int builtin_grep(Executor& exec, const std::vector<std::string>& args) {
		GrepOptions options;
		if (int status = parseGrepArgs(args, options); status != 0) return status;

		if (options.pattern.empty()) {
			perr("grep", "missing pattern");
			return 2;
		}

		if (options.files.empty()) options.files.push_back(options.recursive ? "." : kStdinPath);
		if (options.recursive) options.files = expandGrepRecursive(exec, options.files);

		std::regex re;
		if (!compileGrepRegex(options, re)) return 2;

		GrepRun run;
		prepareGrepRun(options, re, run);

		int status = 1;
		for (const auto& file : options.files) {
			if (grepOneFile(exec, run, file) == 2) status = 2;
		}

		std::fflush(stdout);
		if (options.quiet) return run.any_match ? 0 : 1;
		return run.any_match ? 0 : status;
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

	// sed subset: `s/PAT/REPL/[g]` commands only.
	struct SedSubst {
		std::regex re;
		std::string repl;
		bool global = false;
	};

	static bool isBreGroupChar(char c) {
		return c == '(' || c == ')' || c == '{' || c == '}' || c == '|';
	}

	// BRE patterns are translated to ERE and compiled under
	// std::regex::extended — MSVC's std::regex::basic engine has
	// greediness bugs with back-to-back `[^X]*` runs.
	static std::string translateBreToErePattern(const std::string& bre) {
		std::string out;
		out.reserve(bre.size());
		bool in_class = false;
		bool class_start = false;
		for (std::size_t i = 0; i < bre.size(); ++i) {
			const char c = bre[i];
			if (in_class) {
				out.push_back(c);
				if (c == ']' && !class_start) in_class = false;
				class_start = false;
				continue;
			}

			if (c == '[') {
				in_class = true;
				class_start = true;
				out.push_back(c);
				if (i + 1 < bre.size() && bre[i + 1] == '^') {
					out.push_back('^');
					++i;
				}

				continue;
			}

			if (c == '\\' && i + 1 < bre.size()) {
				const char escaped = bre[i + 1];
				if (!isBreGroupChar(escaped)) out.push_back(c);
				out.push_back(escaped);
				++i;
				continue;
			}

			if (isBreGroupChar(c) || c == '?' || c == '+') out.push_back('\\');
			out.push_back(c);
		}

		return out;
	}

	static bool sedScanPattern(const std::string& cmd, char delim, std::size_t& i,
			std::string& pattern, std::string& error) {
		while (i < cmd.size() && cmd[i] != delim) {
			if (cmd[i] == '\\' && i + 1 < cmd.size()) {
				pattern.push_back(cmd[i]);
				pattern.push_back(cmd[i + 1]);
				i += 2;
				continue;
			}

			pattern.push_back(cmd[i++]);
		}

		if (i >= cmd.size()) {
			error = "missing closing delimiter";
			return false;
		}

		++i;
		return true;
	}

	static void appendSedReplacementEscape(std::string& replacement, char escaped) {
		if (escaped == 'n') {
			replacement.push_back('\n');
		} else if (escaped == 't') {
			replacement.push_back('\t');
		} else if (escaped >= '0' && escaped <= '9') {
			replacement.push_back('$');
			replacement.push_back(escaped);
		} else {
			replacement.push_back(escaped);
		}
	}

	static void sedScanReplacement(const std::string& cmd, char delim, std::size_t& i,
			std::string& replacement) {
		while (i < cmd.size() && cmd[i] != delim) {
			if (cmd[i] == '\\' && i + 1 < cmd.size()) {
				appendSedReplacementEscape(replacement, cmd[i + 1]);
				i += 2;
				continue;
			}

			if (cmd[i] == '$') {
				replacement += "$$";
				++i;
				continue;
			}

			replacement.push_back(cmd[i++]);
		}

		if (i < cmd.size()) ++i;
	}

	static bool sedParseSubst(const std::string& cmd, bool extended, SedSubst& out,
			std::string& error) {
		if (cmd.size() < 4 || cmd[0] != 's') {
			error = "only s/PAT/REPL/[g] is supported";
			return false;
		}

		const char delim = cmd[1];
		std::size_t i = 2;
		std::string pattern;
		std::string replacement;
		if (!sedScanPattern(cmd, delim, i, pattern, error)) return false;
		sedScanReplacement(cmd, delim, i, replacement);

		const std::string flags = (i < cmd.size()) ? cmd.substr(i) : std::string();
		const std::string compiled_pattern =
			extended ? pattern : translateBreToErePattern(pattern);
		if (!compileRegex(out.re, compiled_pattern, std::regex::extended)) {
			error = "regex: invalid pattern: " + pattern;
			return false;
		}

		out.repl = replacement;
		out.global = flags.find('g') != std::string::npos;
		return true;
	}

	struct SedOptions {
		bool quiet = false;
		bool extended = false;
		std::string script;
		std::vector<std::string> files;
	};

	static void appendSedScript(std::string& script, const std::string& part) {
		if (!script.empty()) script.push_back(';');
		script += part;
	}

	static SedOptions parseSedArgs(const std::vector<std::string>& args) {
		SedOptions options;
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg == "-n" || arg == "--quiet" || arg == "--silent") {
				options.quiet = true;
				continue;
			}

			if (arg == "-E" || arg == "-r" || arg == "--regexp-extended") {
				options.extended = true;
				continue;
			}

			if (arg == "--") {
				for (++i; i < args.size(); ++i) options.files.push_back(args[i]);
				break;
			}

			if (arg == "-e" && i + 1 < args.size()) {
				appendSedScript(options.script, args[++i]);
				continue;
			}

			if (hasAttachedValue(arg, "-e")) {
				appendSedScript(options.script, arg.substr(2));
				continue;
			}

			if (options.script.empty() && (arg.empty() || arg[0] != '-')) {
				options.script = arg;
				continue;
			}

			options.files.push_back(arg);
		}

		return options;
	}

	static bool compileSedScript(const std::string& script, bool extended,
			std::vector<SedSubst>& out_cmds) {
		std::size_t start = 0;
		while (start <= script.size()) {
			std::size_t end = script.find(';', start);
			if (end == std::string::npos) end = script.size();

			const std::string part = script.substr(start, end - start);
			if (!part.empty()) {
				SedSubst subst;
				std::string error;
				if (!sedParseSubst(part, extended, subst, error)) {
					std::fprintf(stderr, "wbsh: sed: %s\n", error.c_str());
					return false;
				}

				out_cmds.push_back(std::move(subst));
			}

			if (end == script.size()) break;
			start = end + 1;
		}

		return true;
	}

	static std::string applySedCommandsToLine(const std::vector<SedSubst>& cmds,
			const std::string& line) {
		std::string out = line;
		for (const auto& cmd : cmds) {
			if (cmd.global) {
				out = std::regex_replace(out, cmd.re, cmd.repl);
			} else {
				out = std::regex_replace(out, cmd.re, cmd.repl,
					std::regex_constants::format_first_only);
			}
		}

		return out;
	}

	static void sedEmitLine(const std::vector<SedSubst>& cmds, const std::string& line, bool quiet,
			bool with_newline) {
		const std::string out = applySedCommandsToLine(cmds, line);
		if (quiet) return;

		std::fwrite(out.data(), 1, out.size(), stdout);
		if (with_newline) std::fputc('\n', stdout);
	}

	static void runSedOnStream(FILE* stream, const std::vector<SedSubst>& cmds, bool quiet) {
		std::string line;
		int c;
		while ((c = std::fgetc(stream)) != EOF) {
			if (c != '\n') {
				line.push_back(static_cast<char>(c));
				continue;
			}

			sedEmitLine(cmds, line, quiet, true);
			line.clear();
		}

		if (!line.empty()) sedEmitLine(cmds, line, quiet, false);
	}

	static int builtin_sed(Executor& exec, const std::vector<std::string>& args) {
		SedOptions options = parseSedArgs(args);
		if (options.script.empty()) {
			perr("sed", "no script provided");
			return 2;
		}

		std::vector<SedSubst> cmds;
		if (!compileSedScript(options.script, options.extended, cmds)) return 2;
		defaultToStdin(options.files);

		int status = 0;
		for (const auto& file : options.files) {
			FILE* stream = (file == kStdinPath) ? stdin : fopenNative(exec, file, "rb");
			if (stream == nullptr) {
				perr("sed", file + ": " + std::strerror(errno));
				status = 1;
				continue;
			}

			runSedOnStream(stream, cmds, options.quiet);
			if (stream != stdin) std::fclose(stream);
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
		exec.registerBuiltin("grep",     builtin_grep);
		exec.registerBuiltin("find",     builtin_find);
		exec.registerBuiltin("xargs",    builtin_xargs);
		exec.registerBuiltin("sed",      builtin_sed);
		exec.registerBuiltin("awk",      builtin_awk);
		exec.registerBuiltin("gawk",     builtin_awk);
	}

}  // namespace wbsh

/**
 * @file coreutils_grep.cpp
 * @brief grep, egrep, fgrep with the GNU / busybox option set.
 *
 * Patterns are POSIX basic (-G, the default) or extended (-E) through
 * posixregex.h, ECMAScript as written (-P), or fixed strings (-F). Input
 * is streamed line by line. A trailing carriage return is dropped from
 * each line before matching, so `grep 'x$'` works on CRLF files.
 */

#include "coreutils_internal.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <regex>
#include <string>
#include <system_error>
#include <vector>

#include "executor.h"
#include "fnmatch.h"
#include "numparse.h"
#include "pathconv.h"
#include "posixregex.h"
#include "regexutil.h"

namespace wbsh {

	namespace fs = std::filesystem;

	namespace grep_detail {

		enum class Syntax {
			Basic,
			Extended,
			Perl,
			Fixed,
		};

		enum class BinaryMode {
			Report,
			Text,
			Skip,
		};

		struct GrepOptions {
			Syntax syntax = Syntax::Basic;
			BinaryMode binary = BinaryMode::Report;
			bool ignore_case = false;
			bool invert = false;
			bool word = false;
			bool whole_line = false;
			bool count = false;
			bool list_matching = false;
			bool list_missing = false;
			bool only_matching = false;
			bool quiet = false;
			bool no_messages = false;
			bool line_numbers = false;
			bool byte_offset = false;
			bool recursive = false;
			bool dereference = false;
			bool null_data = false;
			bool color = false;
			int with_filename = -1;
			long max_count = -1;
			long before = 0;
			long after = 0;
			std::string label = "(standard input)";
			std::vector<std::string> patterns;
			bool have_pattern = false;
			std::vector<std::string> includes;
			std::vector<std::string> excludes;
			std::vector<std::string> exclude_dirs;
			std::vector<std::string> files;
		};

		struct Matcher {
			std::vector<std::regex> regexes;
			std::vector<std::string> fixed;
			bool fixed_mode = false;
			bool ignore_case = false;
			bool word = false;
			bool whole_line = false;
		};

		struct LineReader {
			std::FILE* stream = nullptr;
			char delimiter = '\n';
			std::vector<char> buffer = std::vector<char>(64 * 1024);
			std::size_t position = 0;
			std::size_t length = 0;
		};

		struct GrepLine {
			long number = 0;
			unsigned long long offset = 0;
			const std::string* text = nullptr;
		};

		struct FileState {
			std::string name;
			long matches = 0;
			unsigned long long offset = 0;
			long last_printed = 0;
			long after_left = 0;
			std::deque<std::pair<long, std::string>> before;
			std::deque<unsigned long long> before_offsets;
		};

		struct GrepRun {
			const GrepOptions* options = nullptr;
			Matcher matcher;
			bool show_names = false;
			bool any_selected = false;
			bool any_error = false;
			bool printed_group = false;
			std::vector<FileIdentity> ancestors;
		};

	}  // namespace grep_detail

	using grep_detail::BinaryMode;
	using grep_detail::FileState;
	using grep_detail::GrepLine;
	using grep_detail::GrepOptions;
	using grep_detail::GrepRun;
	using grep_detail::LineReader;
	using grep_detail::Matcher;
	using grep_detail::Syntax;

	static const int kGrepError = 2;
	static const char* const kColorMatch = "\x1b[01;31m\x1b[K";
	static const char* const kColorName  = "\x1b[35m\x1b[K";
	static const char* const kColorLine  = "\x1b[32m\x1b[K";
	static const char* const kColorSep   = "\x1b[36m\x1b[K";
	static const char* const kColorEnd   = "\x1b[m\x1b[K";

	static void splitPatternList(const std::string& text, std::vector<std::string>& patterns) {
		std::size_t start = 0;
		for (;;) {
			const std::size_t newline = text.find('\n', start);
			patterns.push_back(text.substr(start, newline == std::string::npos ? std::string::npos
				: newline - start));
			if (newline == std::string::npos) return;
			start = newline + 1;
		}
	}

	static bool readPatternFile(Executor& exec, const std::string& path, GrepOptions& options) {
		std::vector<std::string> lines;
		if (!readAllLines(exec, path, lines)) {
			perr("grep", path + ": " + std::strerror(errno));
			return false;
		}

		options.patterns.insert(options.patterns.end(), lines.begin(), lines.end());
		options.have_pattern = true;
		return true;
	}

	static bool parseCount(const std::string& text, long& value) {
		long long parsed = 0;
		std::size_t used = 0;
		if (!parseLL(text, parsed, 10, &used) || used != text.size() || parsed < 0) return false;
		value = static_cast<long>(parsed);
		return true;
	}

	static bool applyGrepLetter(char flag, GrepOptions& options) {
		switch (flag) {
		case 'E': options.syntax = Syntax::Extended;   return true;
		case 'F': options.syntax = Syntax::Fixed;      return true;
		case 'G': options.syntax = Syntax::Basic;      return true;
		case 'P': options.syntax = Syntax::Perl;       return true;
		case 'i':
		case 'y': options.ignore_case = true;          return true;
		case 'v': options.invert = true;               return true;
		case 'w': options.word = true;                 return true;
		case 'x': options.whole_line = true;           return true;
		case 'c': options.count = true;                return true;
		case 'l': options.list_matching = true;        return true;
		case 'L': options.list_missing = true;         return true;
		case 'o': options.only_matching = true;        return true;
		case 'q': options.quiet = true;                return true;
		case 's': options.no_messages = true;          return true;
		case 'n': options.line_numbers = true;         return true;
		case 'b': options.byte_offset = true;          return true;
		case 'H': options.with_filename = 1;           return true;
		case 'h': options.with_filename = 0;           return true;
		case 'r': options.recursive = true;            return true;
		case 'R':
			options.recursive = true;
			options.dereference = true;
			return true;
		case 'z': options.null_data = true;            return true;
		case 'a': options.binary = BinaryMode::Text;   return true;
		case 'I': options.binary = BinaryMode::Skip;   return true;
		case 'U':
		case 'T': return true;
		default:  return false;
		}
	}

	static bool takeGrepValue(const std::vector<std::string>& args, std::size_t& i,
	                          std::size_t& k, std::string& value) {
		const std::string& cluster = args[i];
		if (k + 1 < cluster.size()) {
			value = cluster.substr(k + 1);
			k = cluster.size();
			return true;
		}

		if (i + 1 >= args.size()) return false;
		value = args[++i];
		k = cluster.size();
		return true;
	}

	static bool applyValueLetter(Executor& exec, char letter, const std::string& value,
	                             GrepOptions& options) {
		switch (letter) {
		case 'e':
			splitPatternList(value, options.patterns);
			options.have_pattern = true;
			return true;
		case 'f': return readPatternFile(exec, value, options);
		case 'm': return parseCount(value, options.max_count);
		case 'A': return parseCount(value, options.after);
		case 'B': return parseCount(value, options.before);
		case 'C':
			if (!parseCount(value, options.after)) return false;
			options.before = options.after;
			return true;
		default:
			return false;
		}
	}

	static bool applyGrepCluster(Executor& exec, const std::vector<std::string>& args,
	                             std::size_t& i, GrepOptions& options) {
		const std::string cluster = args[i];
		if (std::isdigit(static_cast<unsigned char>(cluster[1])) != 0) {
			if (!parseCount(cluster.substr(1), options.after)) return false;
			options.before = options.after;
			return true;
		}

		for (std::size_t k = 1; k < cluster.size(); ++k) {
			const char letter = cluster[k];
			if (applyGrepLetter(letter, options)) continue;
			if (std::strchr("efmABC", letter) == nullptr) return false;

			std::string value;
			if (!takeGrepValue(args, i, k, value)) return false;
			if (!applyValueLetter(exec, letter, value, options)) return false;
		}

		return true;
	}

	static BinaryMode binaryModeNamed(const std::string& value) {
		if (value == "text") return BinaryMode::Text;
		if (value == "without-match") return BinaryMode::Skip;
		return BinaryMode::Report;
	}

	static bool applyLongValue(Executor& exec, const std::string& key, const std::string& value,
	                           GrepOptions& options) {
		if (key == "--regexp")        return applyValueLetter(exec, 'e', value, options);
		if (key == "--file")          return applyValueLetter(exec, 'f', value, options);
		if (key == "--max-count")     return applyValueLetter(exec, 'm', value, options);
		if (key == "--after-context") return applyValueLetter(exec, 'A', value, options);
		if (key == "--before-context") return applyValueLetter(exec, 'B', value, options);
		if (key == "--context")       return applyValueLetter(exec, 'C', value, options);
		if (key == "--include")       options.includes.push_back(value);
		else if (key == "--exclude")  options.excludes.push_back(value);
		else if (key == "--exclude-dir") options.exclude_dirs.push_back(value);
		else if (key == "--label")    options.label = value;
		else if (key == "--color" || key == "--colour") {
			options.color = value == "always" || (value == "auto" && isConsoleStream(stdout));
		} else if (key == "--binary-files") {
			options.binary = binaryModeNamed(value);
		} else {
			return false;
		}

		return true;
	}

	static bool applyLongFlag(const std::string& arg, GrepOptions& options) {
		static const struct { const char* name; char letter; } kLong[] = {
			{ "--extended-regexp", 'E' }, { "--fixed-strings", 'F' }, { "--basic-regexp", 'G' },
			{ "--perl-regexp", 'P' }, { "--ignore-case", 'i' }, { "--invert-match", 'v' },
			{ "--word-regexp", 'w' }, { "--line-regexp", 'x' }, { "--count", 'c' },
			{ "--files-with-matches", 'l' }, { "--files-without-match", 'L' },
			{ "--only-matching", 'o' }, { "--quiet", 'q' }, { "--silent", 'q' },
			{ "--no-messages", 's' }, { "--line-number", 'n' }, { "--byte-offset", 'b' },
			{ "--with-filename", 'H' }, { "--no-filename", 'h' }, { "--recursive", 'r' },
			{ "--dereference-recursive", 'R' }, { "--null-data", 'z' }, { "--text", 'a' },
		};
		for (const auto& entry : kLong) {
			if (arg == entry.name) return applyGrepLetter(entry.letter, options);
		}

		if (arg == "--color" || arg == "--colour") {
			options.color = isConsoleStream(stdout);
			return true;
		}

		return false;
	}

	static bool applyGrepArg(Executor& exec, const std::vector<std::string>& args, std::size_t& i,
	                         GrepOptions& options) {
		const std::string& arg = args[i];
		if (arg.compare(0, 2, "--") == 0) {
			const std::size_t eq = arg.find('=');
			if (eq != std::string::npos) {
				return applyLongValue(exec, arg.substr(0, eq), arg.substr(eq + 1), options);
			}
			if (applyLongFlag(arg, options)) return true;

			static const char* const kValued[] = {
				"--regexp", "--file", "--max-count", "--after-context", "--before-context",
				"--context", "--include", "--exclude", "--exclude-dir", "--label",
			};
			for (const char* name : kValued) {
				if (arg != name || i + 1 >= args.size()) continue;
				return applyLongValue(exec, arg, args[++i], options);
			}

			return false;
		}

		return applyGrepCluster(exec, args, i, options);
	}

	static bool parseGrepArgs(Executor& exec, const std::vector<std::string>& args,
	                          GrepOptions& options) {
		std::vector<std::string> operands;
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg == "--") {
				operands.insert(operands.end(), args.begin() + static_cast<std::ptrdiff_t>(i) + 1,
					args.end());
				break;
			}

			if (arg.size() < 2 || arg[0] != '-') {
				operands.push_back(arg);
				continue;
			}

			if (applyGrepArg(exec, args, i, options)) continue;
			std::fprintf(stderr, "grep: invalid option -- '%s'\n", arg.c_str());
			return false;
		}

		if (!options.have_pattern) {
			if (operands.empty()) {
				std::fprintf(stderr, "Usage: grep [OPTION]... PATTERNS [FILE]...\n");
				return false;
			}

			splitPatternList(operands.front(), options.patterns);
			operands.erase(operands.begin());
		}

		options.files = operands;
		return true;
	}

	static std::string lowerText(std::string text) {
		for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return text;
	}

	static bool buildMatcher(const GrepOptions& options, Matcher& matcher) {
		matcher.ignore_case = options.ignore_case;
		matcher.word = options.word;
		matcher.whole_line = options.whole_line;
		matcher.fixed_mode = options.syntax == Syntax::Fixed;
		for (const std::string& pattern : options.patterns) {
			if (matcher.fixed_mode) {
				matcher.fixed.push_back(options.ignore_case ? lowerText(pattern) : pattern);
				continue;
			}

			std::regex compiled;
			const bool ok = options.syntax == Syntax::Perl
				? compileRegex(compiled, pattern, options.ignore_case
					? std::regex::ECMAScript | std::regex::icase : std::regex::ECMAScript)
				: compilePosixRegex(compiled, pattern, options.syntax == Syntax::Extended,
					options.ignore_case);
			if (!ok) {
				perr("grep", "invalid regular expression: " + pattern);
				return false;
			}

			matcher.regexes.push_back(compiled);
		}

		return true;
	}

	static bool isWordChar(char c) {
		return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
	}

	static bool wordBounded(const std::string& line, std::size_t start, std::size_t length) {
		const bool left = start == 0 || !isWordChar(line[start - 1]);
		const bool right = start + length >= line.size() || !isWordChar(line[start + length]);
		return left && right;
	}

	static bool fixedCandidate(const std::string& haystack, const std::string& needle,
	                           std::size_t from, std::size_t& start) {
		start = haystack.find(needle, from);
		return start != std::string::npos;
	}

	static bool regexCandidate(const std::regex& re, const std::string& line, std::size_t from,
	                           std::size_t& start, std::size_t& length) {
		std::smatch match;
		const auto flags = from == 0 ? std::regex_constants::match_default
			: std::regex_constants::match_prev_avail;
		if (from > line.size()) return false;
		const auto first = line.begin() + static_cast<std::ptrdiff_t>(from);
		if (!searchRegexFlags(first, line.end(), re, match, flags)) return false;

		start = from + static_cast<std::size_t>(match.position(0));
		length = static_cast<std::size_t>(match.length(0));
		return true;
	}

	// One pattern's first acceptable match at or after `from`: -x wants the
	// whole line, -w wants word boundaries on both sides.
	static bool patternMatch(const Matcher& matcher, std::size_t index, const std::string& line,
	                         const std::string& folded, std::size_t from, std::size_t& start,
	                         std::size_t& length) {
		for (std::size_t at = from; at <= line.size();) {
			bool found = false;
			if (matcher.fixed_mode) {
				const std::string& needle = matcher.fixed[index];
				const std::string& haystack = matcher.ignore_case ? folded : line;
				found = fixedCandidate(haystack, needle, at, start);
				length = needle.size();
			} else {
				found = regexCandidate(matcher.regexes[index], line, at, start, length);
			}

			if (!found) return false;
			const bool whole_ok = !matcher.whole_line || (start == 0 && length == line.size());
			const bool word_ok = !matcher.word || wordBounded(line, start, length);
			if (whole_ok && word_ok) return true;
			if (matcher.whole_line) return false;
			at = start + 1;
		}

		return false;
	}

	static std::size_t patternCount(const Matcher& matcher) {
		return matcher.fixed_mode ? matcher.fixed.size() : matcher.regexes.size();
	}

	static bool firstMatch(const Matcher& matcher, const std::string& line,
	                       const std::string& folded,
	                       std::size_t from, std::size_t& start, std::size_t& length) {
		bool found = false;
		for (std::size_t k = 0; k < patternCount(matcher); ++k) {
			std::size_t candidate_start = 0;
			std::size_t candidate_length = 0;
			if (!patternMatch(matcher, k, line, folded, from, candidate_start, candidate_length)) {
				continue;
			}

			const bool better = !found || candidate_start < start
				|| (candidate_start == start && candidate_length > length);
			if (!better) continue;
			start = candidate_start;
			length = candidate_length;
			found = true;
		}

		return found;
	}

	static bool lineSelected(const GrepRun& run, const std::string& line) {
		const std::string folded = run.matcher.fixed_mode && run.matcher.ignore_case
			? lowerText(line) : std::string();
		std::size_t start = 0;
		std::size_t length = 0;
		const bool matched = firstMatch(run.matcher, line, folded, 0, start, length);
		return matched != run.options->invert;
	}

	static bool readLine(LineReader& reader, std::string& line, bool& terminated) {
		line.clear();
		for (;;) {
			if (reader.position == reader.length) {
				reader.position = 0;
				reader.length = std::fread(reader.buffer.data(), 1, reader.buffer.size(),
					reader.stream);
				if (reader.length == 0) {
					terminated = false;
					return !line.empty();
				}
			}

			const char* start = reader.buffer.data() + reader.position;
			const std::size_t available = reader.length - reader.position;
			const void* found = std::memchr(start, reader.delimiter, available);
			const std::size_t take = found == nullptr ? available
				: static_cast<std::size_t>(static_cast<const char*>(found) - start);
			line.append(start, take);
			reader.position += take;
			if (found == nullptr) continue;

			++reader.position;
			terminated = true;
			return true;
		}
	}

	static void printPrefix(const GrepRun& run, const FileState& file, long line_number,
	                        unsigned long long offset, char separator) {
		const GrepOptions& options = *run.options;
		const char* sep_on = options.color ? kColorSep : "";
		const char* sep_off = options.color ? kColorEnd : "";
		if (run.show_names) {
			std::printf("%s%s%s%s%c%s", options.color ? kColorName : "", file.name.c_str(),
				options.color ? kColorEnd : "", sep_on, separator, sep_off);
		}

		if (options.line_numbers) {
			std::printf("%s%ld%s%s%c%s", options.color ? kColorLine : "", line_number,
				options.color ? kColorEnd : "", sep_on, separator, sep_off);
		}

		if (options.byte_offset) std::printf("%llu%c", offset, separator);
	}

	static std::string colored(const GrepRun& run, const std::string& line) {
		if (!run.options->color || run.options->invert) return line;

		const std::string folded = run.matcher.fixed_mode && run.matcher.ignore_case
			? lowerText(line) : std::string();
		std::string out;
		std::size_t from = 0;
		std::size_t start = 0;
		std::size_t length = 0;
		while (from <= line.size() && firstMatch(run.matcher, line, folded, from, start, length)) {
			out.append(line, from, start - from);
			out += kColorMatch + line.substr(start, length) + kColorEnd;
			from = start + (length == 0 ? 1 : length);
			if (length == 0 && start < line.size()) out.push_back(line[start]);
		}

		if (from < line.size()) out.append(line, from, std::string::npos);
		return out;
	}

	static void printSeparatorIfNeeded(GrepRun& run, FileState& file, long line_number) {
		const GrepOptions& options = *run.options;
		const bool context = options.before > 0 || options.after > 0;
		if (!context) return;

		const bool gap = file.last_printed == 0 ? run.printed_group
			: line_number > file.last_printed + 1;
		if (gap) {
			std::printf("%s--%s\n", options.color ? kColorSep : "", options.color ? kColorEnd : "");
		}

		run.printed_group = true;
	}

	static void printLine(GrepRun& run, FileState& file, const GrepLine& line, bool selected) {
		const char terminator = run.options->null_data ? '\0' : '\n';
		printSeparatorIfNeeded(run, file, line.number);
		printPrefix(run, file, line.number, line.offset, selected ? ':' : '-');
		const std::string text = selected ? colored(run, *line.text) : *line.text;
		std::fwrite(text.data(), 1, text.size(), stdout);
		std::fputc(terminator, stdout);
		file.last_printed = line.number;
	}

	static GrepLine grepLine(long number, unsigned long long offset, const std::string& text) {
		GrepLine line;
		line.number = number;
		line.offset = offset;
		line.text = &text;
		return line;
	}

	static void printOnlyMatches(GrepRun& run, FileState& file, long line_number,
	                             unsigned long long offset, const std::string& line) {
		const std::string folded = run.matcher.fixed_mode && run.matcher.ignore_case
			? lowerText(line) : std::string();
		std::size_t from = 0;
		std::size_t start = 0;
		std::size_t length = 0;
		while (from <= line.size() && firstMatch(run.matcher, line, folded, from, start, length)) {
			if (length > 0) {
				printPrefix(run, file, line_number, offset + start, ':');
				const std::string piece = line.substr(start, length);
				const char* on = run.options->color ? kColorMatch : "";
				const char* off = run.options->color ? kColorEnd : "";
				std::printf("%s%s%s\n", on, piece.c_str(), off);
			}

			from = start + (length == 0 ? 1 : length);
		}
	}

	static void flushBefore(GrepRun& run, FileState& file) {
		while (!file.before.empty()) {
			const auto& held = file.before.front();
			printLine(run, file, grepLine(held.first, file.before_offsets.front(), held.second),
				false);
			file.before.pop_front();
			file.before_offsets.pop_front();
		}
	}

	static void rememberBefore(const GrepOptions& options, FileState& file, long line_number,
	                           unsigned long long offset, const std::string& line) {
		if (options.before <= 0) return;
		file.before.emplace_back(line_number, line);
		file.before_offsets.push_back(offset);
		if (static_cast<long>(file.before.size()) > options.before) {
			file.before.pop_front();
			file.before_offsets.pop_front();
		}
	}

	static bool printsLines(const GrepOptions& options) {
		return !options.quiet && !options.count && !options.list_matching && !options.list_missing;
	}

	static void handleSelected(GrepRun& run, FileState& file, long line_number,
	                           unsigned long long offset, const std::string& line) {
		const GrepOptions& options = *run.options;
		++file.matches;
		run.any_selected = true;
		if (!printsLines(options)) return;

		flushBefore(run, file);
		if (options.only_matching) {
			if (!options.invert) printOnlyMatches(run, file, line_number, offset, line);
		} else {
			printLine(run, file, grepLine(line_number, offset, line), true);
		}

		file.after_left = options.after;
	}

	static void handleUnselected(GrepRun& run, FileState& file, long line_number,
	                             unsigned long long offset, const std::string& line) {
		const GrepOptions& options = *run.options;
		if (!printsLines(options) || options.only_matching) return;
		if (file.after_left > 0) {
			--file.after_left;
			printLine(run, file, grepLine(line_number, offset, line), false);
			return;
		}

		rememberBefore(options, file, line_number, offset, line);
	}

	static bool looksBinary(std::FILE* stream) {
		std::vector<char> probe(32 * 1024);
		const std::size_t got = std::fread(probe.data(), 1, probe.size(), stream);
		_fseeki64(stream, 0, SEEK_SET);
		return std::memchr(probe.data(), '\0', got) != nullptr;
	}

	static bool reachedLimit(const GrepOptions& options, const FileState& file) {
		return options.max_count >= 0 && file.matches >= options.max_count;
	}

	static void scanStream(GrepRun& run, std::FILE* stream, FileState& file, bool binary) {
		const GrepOptions& options = *run.options;
		LineReader reader;
		reader.stream = stream;
		reader.delimiter = options.null_data ? '\0' : '\n';

		std::string line;
		bool terminated = true;
		long line_number = 0;
		while (!reachedLimit(options, file) && readLine(reader, line, terminated)) {
			++line_number;
			const unsigned long long offset = file.offset;
			file.offset += line.size() + (terminated ? 1 : 0);
			if (!line.empty() && line.back() == '\r') line.pop_back();

			const bool selected = lineSelected(run, line);
			if (selected && (options.quiet || options.list_matching || options.list_missing)) {
				++file.matches;
				run.any_selected = true;
				return;
			}

			if (selected && binary) {
				++file.matches;
				run.any_selected = true;
				if (!printsLines(options)) return;
				std::printf("Binary file %s matches\n", file.name.c_str());
				return;
			}

			if (selected) handleSelected(run, file, line_number, offset, line);
			else          handleUnselected(run, file, line_number, offset, line);
		}

		while (file.after_left > 0 && readLine(reader, line, terminated)) {
			++line_number;
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (lineSelected(run, line)) break;
			--file.after_left;
			printLine(run, file, grepLine(line_number, file.offset, line), false);
			file.offset += line.size() + 1;
		}
	}

	static void reportFile(const GrepRun& run, const FileState& file) {
		const GrepOptions& options = *run.options;
		if (options.quiet) return;
		if (options.list_matching && file.matches > 0) std::printf("%s\n", file.name.c_str());
		if (options.list_missing && file.matches == 0) std::printf("%s\n", file.name.c_str());
		if (!options.count || options.list_matching || options.list_missing) return;

		if (run.show_names) std::printf("%s:", file.name.c_str());
		std::printf("%ld\n", file.matches);
	}

	static void grepStream(GrepRun& run, std::FILE* stream, const std::string& name) {
		FileState file;
		file.name = name;
		const bool binary = run.options->binary != BinaryMode::Text && stream != stdin
			&& looksBinary(stream);
		if (binary && run.options->binary == BinaryMode::Skip) return;

		scanStream(run, stream, file, binary);
		reportFile(run, file);
	}

	static void grepPath(Executor& exec, GrepRun& run, const std::string& path,
	                     const std::string& shown) {
		if (path == "-") {
			grepStream(run, stdin, run.options->label);
			return;
		}

		std::FILE* stream = fopenNative(exec, path, "rb");
		if (stream == nullptr) {
			if (!run.options->no_messages) perr("grep", shown + ": " + std::strerror(errno));
			run.any_error = true;
			return;
		}

		grepStream(run, stream, shown);
		std::fclose(stream);
	}

	static bool globListMatches(const std::vector<std::string>& globs, const std::string& name) {
		for (const std::string& glob : globs) {
			if (fnmatchFull(glob, name)) return true;
		}

		return false;
	}

	static bool fileWanted(const GrepOptions& options, const std::string& name) {
		if (!options.includes.empty() && !globListMatches(options.includes, name)) return false;
		return !globListMatches(options.excludes, name);
	}

	static std::string childPath(const std::string& directory, const std::string& name) {
		if (directory.empty() || directory == ".") return name;
		if (directory.back() == '/') return directory + name;
		return directory + "/" + name;
	}

	static void grepTree(Executor& exec, GrepRun& run, const std::string& directory,
	                     const std::string& shown_base);

	static bool isAncestor(const GrepRun& run, const FileIdentity& identity) {
		return std::find(run.ancestors.begin(), run.ancestors.end(), identity) != run.ancestors.end();
	}

	static void warnDirectoryLoop(const GrepRun& run, const std::string& shown) {
		if (run.options->no_messages) return;
		std::fflush(stdout);
		std::fprintf(stderr, "grep: warning: %s: recursive directory loop\n", shown.c_str());
	}

	static void descendInto(Executor& exec, GrepRun& run, const std::string& path,
	                        const std::string& shown) {
		FileIdentity identity;
		const bool tracked = run.options->dereference
			&& queryFileIdentity(toNative(exec, path), identity);
		if (!tracked) {
			grepTree(exec, run, path, shown);
			return;
		}

		if (isAncestor(run, identity)) {
			warnDirectoryLoop(run, shown);
			return;
		}

		run.ancestors.push_back(identity);
		grepTree(exec, run, path, shown);
		run.ancestors.pop_back();
	}

	static void grepTreeEntry(Executor& exec, GrepRun& run, const fs::directory_entry& entry,
	                          const std::string& directory, const std::string& shown_base) {
		const std::string name = pathToUtf8(entry.path().filename());
		const std::string path = childPath(directory, name);
		const std::string shown = childPath(shown_base, name);

		std::error_code ec;
		if (!run.options->dereference && isLinkStatus(entry.symlink_status(ec))) return;

		if (entry.is_directory(ec)) {
			const bool excluded = globListMatches(run.options->exclude_dirs, name);
			if (!excluded) descendInto(exec, run, path, shown);
			return;
		}

		if (entry.is_regular_file(ec) && fileWanted(*run.options, name)) {
			grepPath(exec, run, path, shown);
		}
	}

	static void grepTree(Executor& exec, GrepRun& run, const std::string& directory,
	                     const std::string& shown_base) {
		std::error_code ec;
		std::vector<fs::directory_entry> entries;
		for (fs::directory_iterator it(toNative(exec, directory), ec), end; !ec && it != end;
		     it.increment(ec)) {
			entries.push_back(*it);
		}

		std::sort(entries.begin(), entries.end());
		for (const fs::directory_entry& entry : entries) {
			grepTreeEntry(exec, run, entry, directory, shown_base);
		}
	}

	static void grepOperand(Executor& exec, GrepRun& run, const std::string& operand) {
		std::error_code ec;
		const bool directory = operand != "-" && fs::is_directory(toNative(exec, operand), ec);
		if (!directory) {
			grepPath(exec, run, operand, operand);
			return;
		}

		if (run.options->recursive) {
			descendInto(exec, run, operand, operand);
			return;
		}

		if (run.options->no_messages) return;
		std::fprintf(stderr, "grep: %s: Is a directory\n", operand.c_str());
	}

	static int runGrep(Executor& exec, GrepOptions& options) {
		GrepRun run;
		run.options = &options;
		if (!buildMatcher(options, run.matcher)) return kGrepError;

		const bool implicit_tree = options.files.empty() && options.recursive;
		if (options.files.empty() && !options.recursive) options.files.push_back("-");
		const bool many = implicit_tree || options.files.size() > 1 || options.recursive;
		run.show_names = options.with_filename == 1 || (options.with_filename == -1 && many);

		if (implicit_tree) descendInto(exec, run, ".", "");
		for (const std::string& operand : options.files) grepOperand(exec, run, operand);

		std::fflush(stdout);
		if (options.quiet && run.any_selected) return 0;
		if (run.any_error) return kGrepError;
		return run.any_selected ? 0 : 1;
	}

	static int grepWithSyntax(Executor& exec, const std::vector<std::string>& args, Syntax syntax) {
		GrepOptions options;
		options.syntax = syntax;
		if (!parseGrepArgs(exec, args, options)) return kGrepError;
		return runGrep(exec, options);
	}

	static int builtin_grep(Executor& exec, const std::vector<std::string>& args) {
		return grepWithSyntax(exec, args, Syntax::Basic);
	}

	static int builtin_egrep(Executor& exec, const std::vector<std::string>& args) {
		return grepWithSyntax(exec, args, Syntax::Extended);
	}

	static int builtin_fgrep(Executor& exec, const std::vector<std::string>& args) {
		return grepWithSyntax(exec, args, Syntax::Fixed);
	}

	void registerGrepBuiltins(Executor& exec) {
		exec.registerBuiltin("grep",  builtin_grep);
		exec.registerBuiltin("egrep", builtin_egrep);
		exec.registerBuiltin("fgrep", builtin_fgrep);
	}

}  // namespace wbsh

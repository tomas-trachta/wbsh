/**
 * @file vi_substitute.cpp
 * @brief vi's :s (with & ~ \0-\9 \r \t \u \l \U \L \E in the replacement
 *        and the g i I e n & flags) and :g / :v.
 */

#include "vi_internal.h"

#include <string>
#include <vector>

#include "utf8.h"

namespace wbsh {

	namespace vi_detail {

		static const std::size_t kReportLines = 2;

		struct SubstituteFlags {
			bool global = false;
			bool ignore_case = false;
			bool match_case = false;
			bool quiet = false;
			bool count_only = false;
		};

		enum class CaseMode { None, Upper, Lower };

		struct Expansion {
			std::string out;
			CaseMode lasting = CaseMode::None;
			CaseMode next = CaseMode::None;
		};

		static char32_t casedCode(char32_t code, CaseMode mode) {
			if (mode == CaseMode::Upper) return upperCode(code);
			if (mode == CaseMode::Lower) return lowerCode(code);
			return code;
		}

		static void appendCased(Expansion& expansion, const std::string& text) {
			for (std::size_t at = 0; at < text.size();) {
				const Utf8Char decoded = decodeUtf8(text, at);
				const CaseMode mode = expansion.next != CaseMode::None
					? expansion.next
					: expansion.lasting;
				expansion.next = CaseMode::None;
				if (mode == CaseMode::None || (decoded.code >= 0xDC80 && decoded.code <= 0xDCFF)) {
					expansion.out.append(text, at, decoded.length);
				} else {
					appendUtf8(expansion.out, casedCode(decoded.code, mode));
				}

				at += decoded.length;
			}
		}

		static void appendGroup(Expansion& expansion, const std::string& subject,
		                        const RegexMatch& match, std::size_t group) {
			if (group < match.groups() && match.matched(group)) {
				appendCased(expansion, match.str(subject, group));
			}
		}

		static bool caseEscape(Expansion& expansion, char escaped) {
			switch (escaped) {
				case 'u':
					expansion.next = CaseMode::Upper;
					return true;
				case 'l':
					expansion.next = CaseMode::Lower;
					return true;
				case 'U':
					expansion.lasting = CaseMode::Upper;
					return true;
				case 'L':
					expansion.lasting = CaseMode::Lower;
					return true;
				case 'E':
				case 'e':
					expansion.lasting = CaseMode::None;
					return true;
				default:
					return false;
			}
		}

		static void expandEscape(Expansion& expansion, char escaped, const std::string& subject,
		                         const RegexMatch& match) {
			if (escaped >= '0' && escaped <= '9') {
				appendGroup(expansion, subject, match, static_cast<std::size_t>(escaped - '0'));
			} else if (escaped == 'r' || escaped == 'n') {
				expansion.out.push_back('\n');
			} else if (escaped == 't') {
				expansion.out.push_back('\t');
			} else if (!caseEscape(expansion, escaped)) {
				appendCased(expansion, std::string(1, escaped));
			}
		}

		static std::string expandReplacement(const std::string& replacement,
		                                     const std::string& subject, const RegexMatch& match) {
			Expansion expansion;
			for (std::size_t at = 0; at < replacement.size(); ++at) {
				const char c = replacement[at];
				const bool escape = c == '\\' && at + 1 < replacement.size();
				if (c == '&') appendGroup(expansion, subject, match, 0);
				else if (escape) expandEscape(expansion, replacement[++at], subject, match);
				else appendCased(expansion, std::string(1, c));
			}

			return expansion.out;
		}

		// An empty match where the previous match ended is skipped, so
		// `s/b*/-/g` turns "abc" into "-a-c-".
		static long substituteText(const std::string& text, const PosixRegex& regex,
		                           const std::string& replacement, bool global, std::string& out) {
			out.clear();
			std::size_t from = 0;
			std::size_t copied = 0;
			std::size_t previous_end = std::string::npos;
			long count = 0;
			RegexMatch match;
			while (from <= text.size() && regex.search(text, from, match)) {
				const std::size_t start = match.start(0);
				const std::size_t end = match.end(0);
				if (start == end && start == previous_end) {
					from = start < text.size() ? nextCharStart(text, start) : text.size() + 1;
					continue;
				}

				out.append(text, copied, start - copied);
				out += expandReplacement(replacement, text, match);
				copied = end;
				++count;
				if (!global) break;

				previous_end = end;
				from = end > start
					? end
					: (end < text.size() ? nextCharStart(text, end) : text.size() + 1);
			}

			out.append(text, copied, std::string::npos);
			return count;
		}

		static std::vector<std::string> splitAtNewlines(const std::string& text) {
			std::vector<std::string> pieces;
			std::size_t start = 0;
			std::size_t end = text.find('\n');
			for (; end != std::string::npos; end = text.find('\n', start)) {
				pieces.push_back(text.substr(start, end - start));
				start = end + 1;
			}

			pieces.push_back(text.substr(start));
			return pieces;
		}

		static std::string delimitedPart(const std::string& text, std::size_t& pos, char delimiter,
		                                 bool& terminated) {
			std::string part;
			terminated = false;
			while (pos < text.size()) {
				const char c = text[pos++];
				if (c == delimiter) {
					terminated = true;
					return part;
				}

				if (c == '\\' && pos < text.size()) {
					const char escaped = text[pos++];
					if (escaped != delimiter) part.push_back('\\');
					part.push_back(escaped);
					continue;
				}

				part.push_back(c);
			}

			return part;
		}

		// What vim takes as flags of a repeated :s; anything else delimits a pattern.
		static bool isRepeatChar(char c) {
			return c == '&' || c == 'c' || c == 'e' || c == 'g' || c == 'i' || c == 'I' || c == 'p'
				|| c == 'r' || (c >= '0' && c <= '9');
		}

		// An unescaped `~` stands for the previous replacement.
		static std::string withPreviousReplacement(const std::string& replacement,
		                                           const std::string& previous) {
			std::string out;
			for (std::size_t at = 0; at < replacement.size(); ++at) {
				if (replacement[at] == '~') {
					out += previous;
					continue;
				}

				if (replacement[at] == '\\' && at + 1 < replacement.size()) {
					out.push_back(replacement[at++]);
				}

				out.push_back(replacement[at]);
			}

			return out;
		}

		static void parseFlags(const std::string& text, std::size_t& pos, SubstituteFlags& flags) {
			if (pos < text.size() && text[pos] == '&') ++pos;

			for (; pos < text.size(); ++pos) {
				const char c = text[pos];
				if (c == 'g') flags.global = !flags.global;
				else if (c == 'i') flags.ignore_case = true;
				else if (c == 'I') flags.match_case = true;
				else if (c == 'e') flags.quiet = true;
				else if (c == 'n') flags.count_only = true;
				else if (c != 'c' && c != 'p' && c != '#' && c != 'l' && c != 'r') break;
			}
		}

		static bool parseCountArgument(Editor& ed, const std::string& text, std::size_t pos,
		                               ExRange& range) {
			while (pos < text.size() && text[pos] == ' ') ++pos;
			if (pos >= text.size()) return true;

			unsigned long long count = 0;
			for (; pos < text.size() && text[pos] >= '0' && text[pos] <= '9'; ++pos) {
				count = count * 10 + static_cast<unsigned long long>(text[pos] - '0');
			}

			if (pos < text.size() || count == 0) {
				setError(ed, "Trailing characters");
				return false;
			}

			range.first = range.last;
			range.last = range.first + static_cast<std::size_t>(count) - 1;
			if (range.last > ed.lines.size()) range.last = ed.lines.size();
			return true;
		}

		static bool parseSubstitution(Editor& ed, const std::string& args, std::size_t& pos,
		                              SubstituteFlags& flags) {
			const bool repeat = args.empty() || isRepeatChar(args[0]);
			if (repeat) {
				if (!ed.substitution.valid) {
					setError(ed, "No previous substitute regular expression");
					return false;
				}

				const bool keep = !args.empty() && args[0] == '&';
				if (keep) flags.global = ed.substitution.global;
				parseFlags(args, pos, flags);
				return true;
			}

			const char delimiter = args[pos++];
			bool terminated = false;
			std::string pattern = delimitedPart(args, pos, delimiter, terminated);
			std::string replacement = terminated
				? delimitedPart(args, pos, delimiter, terminated)
				: std::string();
			if (pattern.empty()) pattern = ed.last_search;
			replacement = withPreviousReplacement(replacement, ed.substitution.replacement);
			if (pattern.empty()) {
				setError(ed, "No previous regular expression");
				return false;
			}

			parseFlags(args, pos, flags);
			ed.substitution.pattern = pattern;
			ed.substitution.replacement = replacement;
			ed.substitution.global = flags.global;
			ed.substitution.valid = true;
			ed.last_search = pattern;
			return true;
		}

		static bool compileSubstitution(Editor& ed, const SubstituteFlags& flags,
		                                PosixRegex& regex) {
			RegexOptions options;
			options.syntax = RegexSyntax::Basic;
			options.ignore_case = (ed.options.ignorecase || flags.ignore_case) && !flags.match_case;
			if (regex.compile(ed.substitution.pattern, options)) return true;

			setError(ed, regex.error());
			return false;
		}

		struct SubstituteTotals {
			long substitutions = 0;
			std::size_t lines = 0;
			std::size_t last_row = 0;
		};

		static void substituteRange(Editor& ed, const ExRange& range, const PosixRegex& regex,
		                            const SubstituteFlags& flags, SubstituteTotals& totals) {
			std::size_t end = range.last;
			for (std::size_t line = range.first; line <= end; ++line) {
				std::string out;
				const std::string& text = ed.lines[line - 1];
				const std::string& replacement = ed.substitution.replacement;
				const long count = substituteText(text, regex, replacement, flags.global, out);
				if (count == 0) continue;

				totals.substitutions += count;
				++totals.lines;
				totals.last_row = line - 1;
				if (flags.count_only) continue;

				const std::vector<std::string> pieces = splitAtNewlines(out);
				replaceLines(ed, line - 1, 1, pieces);
				end += pieces.size() - 1;
				line += pieces.size() - 1;
				totals.last_row = line - 1;
			}
		}

		static std::string counted(std::size_t count, const std::string& noun) {
			const char* const suffix = noun.back() == 'h' ? "es" : "s";
			return std::to_string(count) + " " + noun + (count == 1 ? "" : suffix);
		}

		static void reportSubstitutions(Editor& ed, const SubstituteFlags& flags,
		                                const SubstituteTotals& totals) {
			const std::size_t count = static_cast<std::size_t>(totals.substitutions);
			const std::string where = counted(totals.lines, "line");
			if (flags.count_only) {
				setMessage(ed, counted(count, "match") + " on " + where);
				return;
			}

			if (totals.lines > kReportLines && !ed.in_global) {
				setMessage(ed, counted(count, "substitution") + " on " + where);
			}
		}

		bool substituteCommand(Editor& ed, const ExRange& given, const std::string& args) {
			ExRange range = given;
			if (range.count == 0) range.first = range.last = ed.cursor.row + 1;
			if (range.first == 0) range.first = 1;

			std::size_t pos = 0;
			SubstituteFlags flags;
			PosixRegex regex;
			if (!parseSubstitution(ed, args, pos, flags)) return false;
			if (!parseCountArgument(ed, args, pos, range)) return false;
			if (!compileSubstitution(ed, flags, regex)) return false;

			SubstituteTotals totals;
			substituteRange(ed, range, regex, flags, totals);
			if (totals.substitutions == 0) {
				if (!flags.quiet && !ed.in_global) {
					setError(ed, "Pattern not found: " + ed.substitution.pattern);
				}

				return flags.quiet || ed.in_global;
			}

			if (!flags.count_only) moveToFirstNonBlank(ed, totals.last_row);
			reportSubstitutions(ed, flags, totals);
			return true;
		}

		static bool parseGlobal(Editor& ed, const std::string& args, std::string& pattern,
		                        std::string& command) {
			if (args.empty()) {
				setError(ed, "Regular expression missing from :global");
				return false;
			}

			std::size_t pos = 1;
			bool terminated = false;
			pattern = delimitedPart(args, pos, args[0], terminated);
			command = pos < args.size() ? args.substr(pos) : std::string();
			if (command.empty()) command = "p";
			if (pattern.empty()) pattern = ed.last_search;
			if (pattern.empty()) {
				setError(ed, "No previous regular expression");
				return false;
			}

			ed.last_search = pattern;
			return true;
		}

		static void markGlobalLines(Editor& ed, const ExRange& range, const PosixRegex& regex,
		                            bool invert) {
			ed.global_flags.assign(ed.lines.size(), 0);
			for (std::size_t line = range.first; line <= range.last; ++line) {
				ed.global_flags[line - 1] = regex.test(ed.lines[line - 1]) != invert ? 1 : 0;
			}
		}

		static bool nextMarked(Editor& ed, std::size_t& hint, std::size_t& row) {
			const std::size_t total = ed.global_flags.size();
			for (std::size_t step = 0; step < total; ++step) {
				const std::size_t index = (hint + step) % total;
				if (ed.global_flags[index] == 0) continue;

				ed.global_flags[index] = 0;
				hint = index;
				row = index;
				return true;
			}

			return false;
		}

		static void runGlobal(Editor& ed, const std::string& command) {
			std::size_t hint = 0;
			std::size_t row = 0;
			while (!ed.quit && nextMarked(ed, hint, row)) {
				ed.cursor = { row, 0 };
				if (!executeExLine(ed, command)) break;
			}
		}

		bool globalCommand(Editor& ed, const ExRange& given, const std::string& args, bool invert) {
			if (ed.in_global) {
				setError(ed, "Cannot do :global recursive");
				return false;
			}

			ExRange range = given;
			if (range.count == 0) {
				range.first = 1;
				range.last = ed.lines.size();
			}

			std::string pattern;
			std::string command;
			PosixRegex regex;
			if (!parseGlobal(ed, args, pattern, command) || !compileSearch(ed, pattern, regex)) {
				return false;
			}

			markGlobalLines(ed, range, regex, invert);
			ed.in_global = true;
			runGlobal(ed, command);
			ed.in_global = false;
			ed.global_flags.clear();
			clampCursor(ed);
			return true;
		}

	}  // namespace vi_detail

}  // namespace wbsh

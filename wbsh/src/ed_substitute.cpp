/**
 * @file ed_substitute.cpp
 * @brief ed's s command and the global commands g, v, G and V.
 */

#include "ed_internal.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "textcells.h"

namespace wbsh {

	namespace ed_detail {

		static bool isRepeatFlag(char c) {
			return c == '\0' || c == 'g' || c == 'p' || c == 'l' || c == 'n' || c == 'r'
				|| (c >= '0' && c <= '9');
		}

		static void appendGroup(std::string& out, const std::string& text, const RegexMatch& match,
		                        std::size_t group) {
			if (group < match.groups() && match.matched(group)) out += match.str(text, group);
		}

		static std::string expandReplacement(const std::string& replacement,
		                                     const std::string& text, const RegexMatch& match) {
			std::string out;
			for (std::size_t at = 0; at < replacement.size(); ++at) {
				const char c = replacement[at];
				if (c == '&') {
					appendGroup(out, text, match, 0);
					continue;
				}

				if (c != '\\' || at + 1 >= replacement.size()) {
					out.push_back(c);
					continue;
				}

				const char escaped = replacement[++at];
				if (escaped >= '1' && escaped <= '9') {
					appendGroup(out, text, match, static_cast<std::size_t>(escaped - '0'));
				} else {
					out.push_back(escaped);
				}
			}

			return out;
		}

		static std::size_t stepPast(const std::string& text, std::size_t at) {
			return at + (std::max)(static_cast<std::size_t>(1), codePointLength(text, at));
		}

		// An empty match right where the previous match ended is skipped, so
		// `s/b*/-/g` turns "abc" into "-a-c-".
		static bool substituteText(const std::string& text, const Substitution& sub,
		                           std::string& out) {
			out.clear();
			std::size_t from = 0;
			std::size_t copied = 0;
			std::size_t previous_end = std::string::npos;
			long count = 0;
			bool changed = false;
			RegexMatch match;
			while (from <= text.size() && sub.regex.search(text, from, match)) {
				const std::size_t start = match.start(0);
				const std::size_t end = match.end(0);
				if (start == end && start == previous_end) {
					from = stepPast(text, start);
					continue;
				}

				++count;
				if (count >= sub.occurrence && (sub.global || count == sub.occurrence)) {
					out.append(text, copied, start - copied);
					out += expandReplacement(sub.replacement, text, match);
					copied = end;
					changed = true;
					if (!sub.global) break;
				}

				previous_end = end;
				from = end > start ? end : stepPast(text, end);
			}

			out.append(text, copied, std::string::npos);
			return changed;
		}

		static std::vector<std::string> splitAtNewlines(const std::string& text) {
			std::vector<std::string> pieces;
			std::size_t start = 0;
			for (;;) {
				const std::size_t end = text.find('\n', start);
				if (end == std::string::npos) {
					pieces.push_back(text.substr(start));
					return pieces;
				}

				pieces.push_back(text.substr(start, end - start));
				start = end + 1;
			}
		}

		// A replacement may go on over the next input line after a backslash.
		static bool continueReplacement(EdState& st, CommandText& cmd) {
			std::string more;
			if (!readInputLine(st, more)) return false;

			cmd.text += "\n" + more;
			return true;
		}

		static bool parseReplacement(EdState& st, CommandText& cmd, char delimiter,
		                             std::string& replacement) {
			replacement.clear();
			while (!cmd.atEnd()) {
				const char c = cmd.take();
				if (c == delimiter) return true;
				if (c == '\n') return false;
				if (c != '\\') {
					replacement.push_back(c);
					continue;
				}

				if (cmd.atEnd() && !continueReplacement(st, cmd)) return false;

				const char escaped = cmd.take();
				if (escaped == delimiter || escaped == '\n') replacement.push_back(escaped);
				else replacement += std::string("\\") + escaped;
			}

			return false;
		}

		static bool parseOccurrence(EdState& st, CommandText& cmd, Substitution& sub) {
			long value = 0;
			while (cmd.peek() >= '0' && cmd.peek() <= '9') {
				value = value * 10 + (cmd.take() - '0');
				if (value > 1000000000L) return fail(st, "Numerical result out of range");
			}

			if (value == 0) return fail(st, "Invalid count");
			sub.occurrence = value;
			return true;
		}

		static bool parseSubstituteFlags(EdState& st, CommandText& cmd, Substitution& sub) {
			for (;;) {
				const char c = cmd.peek();
				if (c == 'g') {
					cmd.take();
					sub.global = !sub.global;
				} else if (c >= '0' && c <= '9') {
					if (!parseOccurrence(st, cmd, sub)) return false;
				} else if (c == 'r') {
					cmd.take();
				} else {
					break;
				}
			}

			return parsePrintSuffix(cmd, sub.print_flags) && finishCommand(st, cmd);
		}

		static bool parseNewSubstitution(EdState& st, CommandText& cmd, Substitution& sub) {
			const char delimiter = cmd.take();
			if (delimiter == ' ' || delimiter == '\n') return fail(st, "Invalid pattern delimiter");

			std::string pattern;
			if (!parsePattern(cmd, delimiter, pattern)) {
				return fail(st, "Missing pattern delimiter");
			}

			if (!compilePattern(st, pattern)) return false;

			std::string replacement;
			const bool terminated = parseReplacement(st, cmd, delimiter, replacement);
			if (replacement == "%") {
				if (!st.has_substitution) return fail(st, "No previous substitution");
				replacement = st.substitution.replacement;
			}

			sub.regex = st.regex;
			sub.replacement = replacement;
			if (!terminated) {
				sub.print_flags = kPrintPlain;
				return finishCommand(st, cmd);
			}

			return parseSubstituteFlags(st, cmd, sub);
		}

		static bool parseRepeatedSubstitution(EdState& st, CommandText& cmd, Substitution& sub) {
			if (!st.has_substitution) return fail(st, "No previous substitution");

			sub = st.substitution;
			sub.print_flags = 0;
			return parseSubstituteFlags(st, cmd, sub);
		}

		static std::size_t applyToLine(EdState& st, std::size_t line, const Substitution& sub,
		                               bool& changed) {
			std::string out;
			changed = substituteText(st.lines[line - 1].text, sub, out);
			if (!changed) return 0;

			const std::vector<std::string> pieces = splitAtNewlines(out);
			replaceLine(st, line, pieces[0]);
			const std::vector<std::string> rest(pieces.begin() + 1, pieces.end());
			if (!rest.empty()) insertLines(st, line, rest);
			return rest.size();
		}

		static bool applySubstitution(EdState& st, const Range& range, const Substitution& sub) {
			std::size_t last_changed = 0;
			std::size_t end = range.second;
			for (std::size_t line = range.first; line <= end; ++line) {
				bool changed = false;
				const std::size_t added = applyToLine(st, line, sub, changed);
				if (!changed) continue;

				end += added;
				line += added;
				last_changed = line;
			}

			if (last_changed == 0) return st.in_global ? true : fail(st, "No match");

			st.current = last_changed;
			if (sub.print_flags != 0) printLine(st, st.current, sub.print_flags);
			return true;
		}

		bool substituteCommand(EdState& st, CommandText& cmd, const Range& given) {
			Range range = given;
			if (range.count == 0) range.first = range.second = st.current;
			const bool valid = range.first >= 1 && range.first <= range.second
				&& range.second <= lastLine(st);
			if (!valid) return fail(st, "Invalid address");

			Substitution sub;
			const bool repeat = isRepeatFlag(cmd.peek());
			const bool parsed = repeat ? parseRepeatedSubstitution(st, cmd, sub)
				: parseNewSubstitution(st, cmd, sub);
			if (!parsed) return false;

			st.substitution = sub;
			st.substitution.print_flags = 0;
			st.has_substitution = true;
			return applySubstitution(st, range, sub);
		}

		static bool endsInContinuation(const std::string& text) {
			std::size_t backslashes = 0;
			for (std::size_t at = text.size(); at > 0 && text[at - 1] == '\\'; --at) ++backslashes;
			return backslashes % 2 == 1;
		}

		static std::vector<std::string> readCommandList(EdState& st, CommandText& cmd) {
			std::vector<std::string> commands;
			std::string line = cmd.rest();
			cmd.pos = cmd.text.size();
			while (endsInContinuation(line)) {
				line.pop_back();
				commands.push_back(line);
				if (!readInputLine(st, line)) {
					line.clear();
					break;
				}
			}

			commands.push_back(line);
			if (commands.size() == 1 && commands[0].empty()) commands[0] = "p";
			return commands;
		}

		static void markMatchingLines(EdState& st, const Range& range, bool wanted) {
			for (EdLine& line : st.lines) line.global = false;
			for (std::size_t line = range.first; line <= range.second; ++line) {
				st.lines[line - 1].global = lineMatches(st, line) == wanted;
			}
		}

		static void clearGlobalMarks(EdState& st) {
			for (EdLine& line : st.lines) line.global = false;
		}

		// The next marked line, looked for from where the last one was so a
		// pass over the buffer stays linear for the usual command lists.
		static bool nextMarkedLine(EdState& st, std::size_t& hint, std::size_t& line) {
			const std::size_t total = st.lines.size();
			for (std::size_t step = 0; step < total; ++step) {
				const std::size_t index = (hint + step) % total;
				if (!st.lines[index].global) continue;

				st.lines[index].global = false;
				hint = index;
				line = index + 1;
				return true;
			}

			return false;
		}

		static bool runCommandList(EdState& st, const std::vector<std::string>& commands) {
			st.global_input = commands;
			st.global_next = 0;
			while (st.global_next < st.global_input.size() && !st.quit) {
				CommandText cmd;
				cmd.text = st.global_input[st.global_next++];
				if (!executeCommand(st, cmd)) return false;
			}

			return true;
		}

		static bool runGlobal(EdState& st, const std::vector<std::string>& commands) {
			std::size_t hint = 0;
			std::size_t line = 0;
			bool ok = true;
			while (ok && !st.quit && nextMarkedLine(st, hint, line)) {
				st.current = line;
				ok = runCommandList(st, commands);
			}

			return ok;
		}

		static bool readInteractiveCommand(EdState& st, std::string& previous,
		                                   std::string& command) {
			if (!readUserLine(st, command)) return false;
			if (command != "&") {
				previous = command;
				return true;
			}

			if (previous.empty()) return fail(st, "No previous command");
			command = previous;
			return true;
		}

		static bool runInteractiveGlobal(EdState& st) {
			std::size_t hint = 0;
			std::size_t line = 0;
			std::string previous;
			while (!st.quit && nextMarkedLine(st, hint, line)) {
				st.current = line;
				printLine(st, line, kPrintPlain);
				std::fflush(stdout);

				std::string command;
				if (!readInteractiveCommand(st, previous, command)) return st.input_ended;
				if (command.empty()) continue;
				if (!runCommandList(st, { command })) return false;
			}

			return true;
		}

		static bool prepareGlobal(EdState& st, CommandText& cmd, Range& range, char kind) {
			if (st.in_global) return fail(st, "Cannot nest global commands");
			if (range.count == 0) {
				range.first = 1;
				range.second = lastLine(st);
			}

			const bool valid = range.first >= 1 && range.first <= range.second
				&& range.second <= lastLine(st);
			if (!valid) return fail(st, "Invalid address");

			const char delimiter = cmd.take();
			if (delimiter == '\0' || delimiter == ' ' || delimiter == '\n') {
				return fail(st, "Invalid pattern delimiter");
			}

			std::string pattern;
			parsePattern(cmd, delimiter, pattern);
			if (!compilePattern(st, pattern)) return false;

			markMatchingLines(st, range, kind == 'g' || kind == 'G');
			return true;
		}

		bool globalCommand(EdState& st, CommandText& cmd, const Range& given, char kind) {
			Range range = given;
			if (!prepareGlobal(st, cmd, range, kind)) return false;

			const bool interactive = kind == 'G' || kind == 'V';
			if (interactive && !finishCommand(st, cmd)) return false;

			const std::vector<std::string> commands = interactive ? std::vector<std::string>()
				: readCommandList(st, cmd);
			st.in_global = true;
			const bool ok = interactive ? runInteractiveGlobal(st) : runGlobal(st, commands);
			st.in_global = false;
			st.global_input.clear();
			st.global_next = 0;
			clearGlobalMarks(st);
			return ok;
		}

	}  // namespace ed_detail

}  // namespace wbsh

/**
 * @file histexpand.cpp
 * @brief bash's history expansion: `!` event designators, word designators,
 *        modifiers and `^old^new^`, skipping the places `!` means something else.
 */

#include "history.h"

#include <cctype>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "numparse.h"

namespace wbsh {

	namespace histexpand_detail {

		static const char* const kNoExpandAfterBang = " \t\n\r=(";
		static const char* const kWordDelimiters = ";&|()<>";
		static const char* const kModifiers = "htrepqxsg&aG";
		static const std::size_t kNoWord = static_cast<std::size_t>(-1);

		struct Expansion {
			const std::string& line;
			const std::vector<std::string>& history;
			HistorySubstitution& last;
			std::string out;
			std::string search;
			std::string error;
			bool print_only = false;
			bool expanded = false;
		};

		static bool isDigit(char letter) {
			return std::isdigit(static_cast<unsigned char>(letter)) != 0;
		}

		static bool isBlank(char letter) {
			return letter == ' ' || letter == '\t' || letter == '\n' || letter == '\r';
		}

		static bool isOneOf(char letter, const char* set) {
			return letter != '\0' && std::strchr(set, letter) != nullptr;
		}

		static char at(const std::string& text, std::size_t i) {
			return i < text.size() ? text[i] : '\0';
		}

		static bool closesLater(const std::string& line, std::size_t from, char closer) {
			return line.find(closer, from) != std::string::npos;
		}

		// The places bash leaves a `!` alone: before a blank, `=` or `(`
		// (an extglob), before the closing quote of a double-quoted string,
		// and in `[!...]`, `${!name}` and `$!`.
		static bool bangIsLiteral(const std::string& line, std::size_t i, bool in_double) {
			const char next = at(line, i + 1);
			if (next == '\0' || isOneOf(next, kNoExpandAfterBang)) return true;
			if (in_double && next == '"') return true;

			const char before = i > 0 ? line[i - 1] : '\0';
			if (before == '$') return true;
			if (before == '[' && closesLater(line, i + 1, ']')) return true;

			return before == '{' && i > 1 && line[i - 2] == '$' && closesLater(line, i + 1, '}');
		}

		static std::size_t skipQuotedWord(const std::string& text, std::size_t i) {
			const char quote = text[i];
			for (++i; i < text.size(); ++i) {
				if (text[i] == quote) return i + 1;
				if (quote != '\'' && text[i] == '\\') ++i;
			}

			return text.size();
		}

		static std::size_t wordEnd(const std::string& text, std::size_t i) {
			if (isOneOf(text[i], kWordDelimiters)) {
				while (i < text.size() && isOneOf(text[i], kWordDelimiters)) ++i;
				return i;
			}

			while (i < text.size() && !isBlank(text[i]) && !isOneOf(text[i], kWordDelimiters)) {
				const char letter = text[i];
				if (letter == '\'' || letter == '"' || letter == '`') {
					i = skipQuotedWord(text, i);
				} else {
					i += letter == '\\' ? 2 : 1;
				}
			}

			return i < text.size() ? i : text.size();
		}

		// The words of an event as bash's history_tokenize splits them:
		// quotes keep a word together and the shell's operators are words
		// of their own.
		static std::vector<std::string> wordsOf(const std::string& text) {
			std::vector<std::string> words;
			std::size_t i = 0;
			while (i < text.size()) {
				if (isBlank(text[i])) {
					++i;
					continue;
				}

				const std::size_t end = wordEnd(text, i);
				words.push_back(text.substr(i, end - i));
				i = end;
			}

			return words;
		}

		static bool fail(Expansion& expansion, const std::string& spec, const char* reason) {
			expansion.error = spec + ": " + reason;
			return false;
		}

		static bool findByNumber(const Expansion& expansion, long long number, std::string& event) {
			const auto count = static_cast<long long>(expansion.history.size());
			const long long index = number < 0 ? count + number : number - 1;
			if (index < 0 || index >= count) return false;

			event = expansion.history[static_cast<std::size_t>(index)];
			return true;
		}

		static bool findNewest(const Expansion& expansion, const std::string& text, bool anywhere,
		                       std::string& event) {
			for (auto it = expansion.history.rbegin(); it != expansion.history.rend(); ++it) {
				const bool found = anywhere ? it->find(text) != std::string::npos
				                            : it->compare(0, text.size(), text) == 0;
				if (!found) continue;

				event = *it;
				return true;
			}

			return false;
		}

		static bool previousEvent(const Expansion& expansion, std::string& event) {
			if (expansion.history.empty()) return false;

			event = expansion.history.back();
			return true;
		}

		static bool numberedEvent(Expansion& expansion, std::size_t& j, std::string& event) {
			const std::string& line = expansion.line;
			const std::size_t start = j;
			if (line[j] == '-') ++j;
			while (j < line.size() && isDigit(line[j])) ++j;

			long long number = 0;
			if (!parseLL(line.substr(start, j - start), number)) return false;

			return findByNumber(expansion, number, event);
		}

		static bool searchedEvent(Expansion& expansion, std::size_t& j, std::string& event) {
			const std::string& line = expansion.line;
			const std::size_t start = ++j;
			while (j < line.size() && line[j] != '?' && line[j] != '\n') ++j;

			expansion.search = line.substr(start, j - start);
			if (at(line, j) == '?') ++j;

			if (expansion.search.empty()) return false;

			return findNewest(expansion, expansion.search, true, event);
		}

		static bool endsEventName(char letter) {
			return isBlank(letter) || letter == ':' || letter == '"' || letter == '\''
			    || isOneOf(letter, kWordDelimiters);
		}

		static bool prefixedEvent(Expansion& expansion, std::size_t& j, std::string& event) {
			const std::string& line = expansion.line;
			const std::size_t start = j;
			while (j < line.size() && !endsEventName(line[j])) ++j;

			return j > start && findNewest(expansion, line.substr(start, j - start), false, event);
		}

		static bool startsWordDesignator(const std::string& line, std::size_t j) {
			const char letter = at(line, j);
			if (isOneOf(letter, "^$*%")) return true;

			return letter == '-' && !isDigit(at(line, j + 1));
		}

		// `j` starts just past the `!` and ends past the event.
		static bool eventAt(Expansion& expansion, std::size_t& j, std::string& event) {
			const std::string& line = expansion.line;
			const char letter = line[j];
			if (letter == '!') {
				++j;
				return previousEvent(expansion, event);
			}

			if (letter == '#') {
				++j;
				event = expansion.out;
				return true;
			}

			if (isDigit(letter) || (letter == '-' && isDigit(at(line, j + 1)))) {
				return numberedEvent(expansion, j, event);
			}

			if (letter == '?') return searchedEvent(expansion, j, event);
			const bool designator_only = letter == ':' || startsWordDesignator(line, j);
			if (designator_only) return previousEvent(expansion, event);

			return prefixedEvent(expansion, j, event);
		}

		static std::size_t wordIndex(const std::string& line, std::size_t& j, std::size_t last) {
			const char letter = at(line, j);
			if (letter == '^') {
				++j;
				return 1;
			}

			if (letter == '$') {
				++j;
				return last;
			}

			const std::size_t start = j;
			while (j < line.size() && isDigit(line[j])) ++j;
			if (j == start) return kNoWord;

			long long number = 0;
			if (!parseLL(line.substr(start, j - start), number)) return kNoWord;

			return static_cast<std::size_t>(number);
		}

		struct WordRange {
			std::size_t first = 0;
			std::size_t last = 0;
			bool empty = false;
		};

		static bool rangeEnd(const std::string& line, std::size_t& j, std::size_t last_word,
		                     WordRange& range) {
			if (at(line, j) == '*') {
				++j;
				range.last = last_word;
				return true;
			}

			if (at(line, j) != '-') {
				range.last = range.first;
				return true;
			}

			++j;
			const std::size_t end = wordIndex(line, j, last_word);
			range.last = end != kNoWord ? end : last_word - 1;
			if (end == kNoWord && last_word == 0) range.empty = true;
			return true;
		}

		static bool wordRange(const std::string& line, std::size_t& j, std::size_t last_word,
		                      WordRange& range) {
			if (at(line, j) == '*') {
				++j;
				range.first = 1;
				range.last = last_word;
				range.empty = last_word == 0;
				return true;
			}

			if (at(line, j) == '-') {
				range.first = 0;
			} else {
				range.first = wordIndex(line, j, last_word);
				if (range.first == kNoWord) return false;
			}

			return rangeEnd(line, j, last_word, range);
		}

		static std::string joinWords(const std::vector<std::string>& words,
		                             const WordRange& range) {
			std::string text;
			for (std::size_t k = range.first; k <= range.last; ++k) {
				if (k > range.first) text.push_back(' ');
				text += words[k];
			}

			return text;
		}

		static bool searchedWord(Expansion& expansion, const std::vector<std::string>& words,
		                         std::string& text) {
			if (expansion.search.empty()) return false;

			for (const std::string& word : words) {
				if (word.find(expansion.search) == std::string::npos) continue;

				text = word;
				return true;
			}

			return false;
		}

		static bool selectWords(Expansion& expansion, std::size_t& j, const std::string& event,
		                        std::string& text) {
			const std::vector<std::string> words = wordsOf(event);
			if (at(expansion.line, j) == '%') {
				++j;
				return searchedWord(expansion, words, text);
			}

			WordRange range;
			const std::size_t last_word = words.empty() ? 0 : words.size() - 1;
			if (!wordRange(expansion.line, j, last_word, range)) return false;
			if (range.empty) {
				text.clear();
				return true;
			}

			const bool in_range = !words.empty() && range.first <= range.last
				&& range.last < words.size();
			if (!in_range) return false;

			text = joinWords(words, range);
			return true;
		}

		// The colon may be left out before a designator that starts with
		// ^ $ * % or -.
		static bool hasWordDesignator(const std::string& line, std::size_t j) {
			if (startsWordDesignator(line, j)) return true;
			if (at(line, j) != ':') return false;

			const char letter = at(line, j + 1);
			return isDigit(letter) || isOneOf(letter, "^$*%-");
		}

		static bool readDelimited(const std::string& line, std::size_t& j, char delimiter,
		                          std::string& text) {
			while (j < line.size() && line[j] != delimiter && line[j] != '\n') {
				if (line[j] == '\\' && at(line, j + 1) == delimiter) ++j;
				text.push_back(line[j]);
				++j;
			}

			if (at(line, j) != delimiter) return false;

			++j;
			return true;
		}

		static std::string withAmpersands(const std::string& replacement,
		                                  const std::string& old_text) {
			std::string text;
			for (std::size_t k = 0; k < replacement.size(); ++k) {
				if (replacement[k] == '\\' && at(replacement, k + 1) == '&') {
					text.push_back('&');
					++k;
				} else if (replacement[k] == '&') {
					text += old_text;
				} else {
					text.push_back(replacement[k]);
				}
			}

			return text;
		}

		static bool substitute(std::string& text, const HistorySubstitution& substitution,
		                       bool global) {
			const std::string replacement =
				withAmpersands(substitution.new_text, substitution.old_text);
			std::size_t from = 0;
			bool replaced = false;
			for (;;) {
				const std::size_t found = text.find(substitution.old_text, from);
				if (found == std::string::npos) break;

				text.replace(found, substitution.old_text.size(), replacement);
				from = found + replacement.size();
				replaced = true;
				if (!global) break;
			}

			return replaced;
		}

		// `s/old/new/`: any character after the s delimits, the last
		// delimiter may be left off at the end of the line, and an empty
		// old text means the previous one.
		static bool readSubstitution(Expansion& expansion, std::size_t& j) {
			const std::string& line = expansion.line;
			const char delimiter = at(line, j);
			if (delimiter == '\0') return false;

			++j;
			HistorySubstitution substitution;
			if (!readDelimited(line, j, delimiter, substitution.old_text)) return false;
			const bool closed = readDelimited(line, j, delimiter, substitution.new_text);
			if (!closed && j < line.size()) return false;

			if (substitution.old_text.empty()) {
				substitution.old_text = expansion.last.old_text.empty()
					? expansion.search : expansion.last.old_text;
			}

			expansion.last = substitution;
			return !substitution.old_text.empty();
		}

		static std::size_t suffixDot(const std::string& text) {
			const std::size_t slash = text.find_last_of('/');
			const std::size_t dot = text.find_last_of('.');
			if (dot != std::string::npos && slash != std::string::npos && dot < slash) {
				return std::string::npos;
			}

			return dot;
		}

		static std::string singleQuoted(const std::string& text) {
			std::string quoted = "'";
			for (const char letter : text) {
				if (letter == '\'') {
					quoted += "'\\''";
				} else {
					quoted.push_back(letter);
				}
			}

			return quoted + "'";
		}

		static std::string quotedWords(const std::string& text) {
			std::string quoted;
			for (const std::string& word : wordsOf(text)) {
				if (!quoted.empty()) quoted.push_back(' ');
				quoted += singleQuoted(word);
			}

			return quoted;
		}

		static void applyPathModifier(char modifier, std::string& text) {
			const std::size_t slash = text.find_last_of('/');
			const std::size_t dot = suffixDot(text);
			switch (modifier) {
			case 'h':
				if (slash != std::string::npos) text.resize(slash == 0 ? 1 : slash);
				break;
			case 't':
				if (slash != std::string::npos) text = text.substr(slash + 1);
				break;
			case 'r':
				if (dot != std::string::npos) text.resize(dot);
				break;
			case 'e':
				text = dot != std::string::npos ? text.substr(dot) : std::string();
				break;
			default:
				break;
			}
		}

		static bool applySubstitution(Expansion& expansion, std::size_t& j, bool global,
		                              std::string& text) {
			const char kind = expansion.line[j++];
			if (kind == 's' && !readSubstitution(expansion, j)) {
				return fail(expansion, ":s", "bad substitution");
			}

			if (expansion.last.old_text.empty()) {
				return fail(expansion, ":" + std::string(1, kind), "no previous substitution");
			}

			if (!substitute(text, expansion.last, global)) {
				const HistorySubstitution& last = expansion.last;
				const std::string spec = ":s/" + last.old_text + "/" + last.new_text + "/";
				return fail(expansion, spec, "substitution failed");
			}

			return true;
		}

		// `j` sits on the modifier letter after its colon.
		static bool applyModifier(Expansion& expansion, std::size_t& j, std::string& text) {
			const std::string& line = expansion.line;
			char modifier = line[j];
			const bool global = modifier == 'g' || modifier == 'a' || modifier == 'G';
			if (global) modifier = line[++j];

			switch (modifier) {
			case 's':
			case '&':
				return applySubstitution(expansion, j, global, text);
			case 'p':
				expansion.print_only = true;
				break;
			case 'q':
				text = singleQuoted(text);
				break;
			case 'x':
				text = quotedWords(text);
				break;
			case 'h':
			case 't':
			case 'r':
			case 'e':
				applyPathModifier(modifier, text);
				break;
			default:
				const std::string spec = ":" + std::string(1, modifier);
				return fail(expansion, spec, "unrecognized history modifier");
			}

			++j;
			return true;
		}

		static bool applyModifiers(Expansion& expansion, std::size_t& j, std::string& text) {
			const std::string& line = expansion.line;
			while (at(line, j) == ':' && isOneOf(at(line, j + 1), kModifiers)) {
				++j;
				if (!applyModifier(expansion, j, text)) return false;
			}

			return true;
		}

		static bool expandBang(Expansion& expansion, std::size_t i, std::size_t& j) {
			const std::string& line = expansion.line;
			j = i + 1;
			std::string event;
			if (!eventAt(expansion, j, event)) {
				const std::size_t end = j > i + 1 ? j : i + 2;
				return fail(expansion, line.substr(i, end - i), "event not found");
			}

			std::string text = event;
			if (hasWordDesignator(line, j)) {
				if (line[j] == ':') ++j;
				if (!selectWords(expansion, j, event, text)) {
					return fail(expansion, line.substr(i, j - i), "bad word specifier");
				}
			}

			if (!applyModifiers(expansion, j, text)) return false;

			expansion.out += text;
			expansion.expanded = true;
			return true;
		}

		struct QuoteState {
			bool single = false;
			bool ansi = false;
			bool dquote = false;
		};

		// Copies the character at `i` (two for an escape) when it is not a
		// `!` to expand; false when the `!` is up to expandBang.
		static bool copyQuoted(Expansion& expansion, std::size_t& i, QuoteState& quote) {
			const std::string& line = expansion.line;
			const char letter = line[i];
			const bool escapes = !quote.single || quote.ansi;
			const bool escape = letter == '\\' && i + 1 < line.size() && escapes;
			if (escape) {
				expansion.out += line.substr(i, 2);
				++i;
				return true;
			}

			if (quote.single) {
				quote.single = letter != '\'';
			} else if (letter == '\'' && !quote.dquote) {
				quote.single = true;
				quote.ansi = i > 0 && line[i - 1] == '$';
			} else if (letter == '"') {
				quote.dquote = !quote.dquote;
			} else if (letter == '!' && !bangIsLiteral(line, i, quote.dquote)) {
				return false;
			}

			expansion.out.push_back(letter);
			return true;
		}

		static bool expandBangs(Expansion& expansion, char starting_quote) {
			QuoteState quote;
			quote.single = starting_quote == '\'';
			quote.dquote = starting_quote == '"';

			const std::string& line = expansion.line;
			for (std::size_t i = 0; i < line.size(); ++i) {
				if (copyQuoted(expansion, i, quote)) continue;

				std::size_t j = i;
				if (!expandBang(expansion, i, j)) return false;

				i = j - 1;
			}

			return true;
		}

		// `^old^new^rest` is `!!:s^old^new^` with `rest` after it.
		static bool expandQuickSubstitution(Expansion& expansion) {
			std::string event;
			if (!previousEvent(expansion, event)) return fail(expansion, "^", "event not found");

			std::size_t j = 0;
			if (!readSubstitution(expansion, j)) {
				return fail(expansion, expansion.line, "bad substitution");
			}
			if (!substitute(event, expansion.last, false)) {
				return fail(expansion, ":s" + expansion.line, "substitution failed");
			}

			expansion.out = event + expansion.line.substr(j);
			expansion.expanded = true;
			return true;
		}

		static HistoryExpansion resultOf(const Expansion& expansion, bool ok) {
			HistoryExpansion result;
			if (!ok) {
				result.kind = HistoryExpansion::Kind::kFailed;
				result.text = expansion.error;
			} else if (!expansion.expanded) {
				result.kind = HistoryExpansion::Kind::kUnchanged;
				result.text = expansion.line;
			} else {
				result.kind = expansion.print_only ? HistoryExpansion::Kind::kPrintOnly
				                                   : HistoryExpansion::Kind::kExpanded;
				result.text = expansion.out;
			}

			return result;
		}

	}  // namespace histexpand_detail

	HistoryExpansion expandHistoryLine(const std::string& line,
	                                   const std::vector<std::string>& history,
	                                   char starting_quote, HistorySubstitution& last) {
		histexpand_detail::Expansion expansion{ line, history, last };
		const bool quick = starting_quote == '\0' && !line.empty() && line[0] == '^';
		const bool ok = quick ? histexpand_detail::expandQuickSubstitution(expansion)
		                      : histexpand_detail::expandBangs(expansion, starting_quote);
		return histexpand_detail::resultOf(expansion, ok);
	}

}  // namespace wbsh

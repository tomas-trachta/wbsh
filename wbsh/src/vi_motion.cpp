/**
 * @file vi_motion.cpp
 * @brief vi's cursor motions, as targets an operator can also use:
 *        characters, lines, words, finds, brackets, sentences,
 *        paragraphs, marks and searches.
 */

#include "vi_internal.h"

#include <cstring>
#include <string>

namespace wbsh {

	namespace vi_detail {

		static const char* const kBrackets = "()[]{}";
		static const std::size_t kPercent = 100;
		static const char* const kWrappedToTop = "search hit BOTTOM, continuing at TOP";
		static const char* const kWrappedToBottom = "search hit TOP, continuing at BOTTOM";

		static Motion motionTo(Position target, bool inclusive) {
			Motion motion;
			motion.target = target;
			motion.inclusive = inclusive;
			return motion;
		}

		static Motion lineMotion(const Editor& ed, std::size_t row, bool jump) {
			Motion motion;
			motion.target.row = row;
			motion.target.col = firstNonBlank(ed.lines[row]);
			motion.linewise = true;
			motion.jump = jump;
			return motion;
		}

		static const std::string& currentText(const Editor& ed) {
			return ed.lines[ed.cursor.row];
		}

		static std::size_t lastRow(const Editor& ed) {
			return ed.lines.size() - 1;
		}

		static bool stepForward(const Editor& ed, Position& p) {
			const std::string& line = ed.lines[p.row];
			const std::size_t next = nextCharStart(line, p.col);
			if (next < line.size()) {
				p.col = next;
				return true;
			}

			if (p.row >= lastRow(ed)) return false;
			++p.row;
			p.col = 0;
			return true;
		}

		static bool stepBackward(const Editor& ed, Position& p) {
			if (p.col > 0) {
				p.col = previousCharStart(ed.lines[p.row], p.col);
				return true;
			}

			if (p.row == 0) return false;
			--p.row;
			p.col = lastCharStart(ed.lines[p.row]);
			return true;
		}

		static bool stepToward(const Editor& ed, Position& p, bool forward) {
			return forward ? stepForward(ed, p) : stepBackward(ed, p);
		}

		static bool motionLeft(const Editor& ed, long count, Motion& motion) {
			Position p = ed.cursor;
			const std::string& line = ed.lines[p.row];
			for (long k = 0; k < count && p.col > 0; ++k) p.col = previousCharStart(line, p.col);
			if (p.col == ed.cursor.col) return false;

			motion = motionTo(p, false);
			return true;
		}

		static bool motionRight(const Editor& ed, const MotionRequest& request, Motion& motion) {
			const std::string& line = ed.lines[ed.cursor.row];
			Position p = ed.cursor;
			for (long k = 0; k < request.count && p.col < line.size(); ++k) {
				const std::size_t next = nextCharStart(line, p.col);
				if (next >= line.size() && !request.for_operator) break;
				p.col = next;
			}

			if (p.col == ed.cursor.col) return false;
			motion = motionTo(p, false);
			return true;
		}

		static bool verticalMotion(const Editor& ed, long count, bool down, Motion& motion) {
			const std::size_t row = ed.cursor.row;
			const std::size_t steps = static_cast<std::size_t>(count);
			if ((down && row >= lastRow(ed)) || (!down && row == 0)) return false;

			std::size_t target = down ? row + steps : (steps > row ? 0 : row - steps);
			if (target > lastRow(ed)) target = lastRow(ed);

			motion = Motion();
			motion.target.row = target;
			motion.target.col = ed.want_end ? ed.lines[target].size()
				: byteAtColumn(ed, target, ed.want_column);
			motion.linewise = true;
			motion.keep_column = true;
			return true;
		}

		static bool lineOffsetMotion(const Editor& ed, long count, bool down, Motion& motion) {
			const std::size_t row = ed.cursor.row;
			const std::size_t steps = static_cast<std::size_t>(count);
			if (down && row + steps > lastRow(ed)) return false;
			if (!down && steps > row) return false;

			motion = lineMotion(ed, down ? row + steps : row - steps, false);
			return true;
		}

		static std::size_t clampRow(const Editor& ed, long row) {
			if (row < 0) return 0;
			const std::size_t wanted = static_cast<std::size_t>(row);
			return wanted > lastRow(ed) ? lastRow(ed) : wanted;
		}

		static std::size_t countedRow(const Editor& ed, const MotionRequest& request) {
			return request.has_count ? clampRow(ed, request.count - 1) : lastRow(ed);
		}

		static bool screenMotion(const Editor& ed, Key key, long count, Motion& motion) {
			const std::size_t first = ed.top;
			const std::size_t last = lastVisibleLine(ed);
			std::size_t row = first + (last - first) / 2;
			if (key == 'H') row = first + static_cast<std::size_t>(count - 1);
			if (key == 'L') row = static_cast<std::size_t>(count - 1) > last - first ? first
				: last - static_cast<std::size_t>(count - 1);
			if (row > last) row = last;

			motion = lineMotion(ed, row, true);
			return true;
		}

		static bool endOfLineMotion(const Editor& ed, long count, Motion& motion) {
			const std::size_t row = clampRow(ed, static_cast<long>(ed.cursor.row) + count - 1);
			motion = motionTo({ row, lastCharStart(ed.lines[row]) }, true);
			motion.to_end = true;
			return true;
		}

		static bool columnMotion(const Editor& ed, long count, Motion& motion) {
			const std::size_t column = static_cast<std::size_t>(count - 1);
			motion = motionTo({ ed.cursor.row, byteAtColumn(ed, ed.cursor.row, column) }, false);
			return true;
		}

		static bool lineMotions(Editor& ed, const MotionRequest& request, Motion& motion) {
			const long count = request.count;
			switch (request.key) {
				case 'j':
				case kKeyDown:
				case ctrlKey('N'):
					return verticalMotion(ed, count, true, motion);
				case 'k':
				case kKeyUp:
				case ctrlKey('P'):
					return verticalMotion(ed, count, false, motion);
				case '+':
				case kKeyEnter:
					return lineOffsetMotion(ed, count, true, motion);
				case '-':
					return lineOffsetMotion(ed, count, false, motion);
				case '_':
					return lineOffsetMotion(ed, count - 1, true, motion);
				case 'G':
					motion = lineMotion(ed, countedRow(ed, request), true);
					return true;
				case 'H':
				case 'M':
				case 'L':
					return screenMotion(ed, request.key, count, motion);
				default:
					return false;
			}
		}

		static bool charMotions(Editor& ed, const MotionRequest& request, Motion& motion) {
			switch (request.key) {
				case 'h':
				case kKeyLeft:
				case kKeyBackspace:
					return motionLeft(ed, request.count, motion);
				case 'l':
				case ' ':
				case kKeyRight:
					return motionRight(ed, request, motion);
				case '0':
				case kKeyHome:
					motion = motionTo({ ed.cursor.row, 0 }, false);
					return true;
				case '^':
					motion = motionTo({ ed.cursor.row, firstNonBlank(currentText(ed)) }, false);
					return true;
				case '$':
				case kKeyEnd:
					return endOfLineMotion(ed, request.count, motion);
				case '|':
					return columnMotion(ed, request.count, motion);
				default:
					return lineMotions(ed, request, motion);
			}
		}

		static Position nextWordStart(const Editor& ed, Position p, bool big, bool& at_end) {
			const std::string* line = &ed.lines[p.row];
			const int kind = charClass(*line, p.col, big);
			while (kind != 0 && p.col < line->size() && charClass(*line, p.col, big) == kind) {
				p.col = nextCharStart(*line, p.col);
			}

			for (;;) {
				if (p.col >= line->size()) {
					if (p.row >= lastRow(ed)) {
						at_end = true;
						return p;
					}

					++p.row;
					p.col = 0;
					line = &ed.lines[p.row];
					if (line->empty()) return p;
				} else if (isBlankChar(*line, p.col)) {
					p.col = nextCharStart(*line, p.col);
				} else {
					return p;
				}
			}
		}

		static bool onlyBlanksBefore(const std::string& line, std::size_t col) {
			for (std::size_t at = 0; at < col; ++at) {
				if (line[at] != ' ' && line[at] != '\t') return false;
			}

			return true;
		}

		// An operator's w stops at the end of the line its last word is on
		// instead of taking the next line's indent with it.
		static bool wordForward(const Editor& ed, const MotionRequest& request, bool big,
		                        Motion& motion) {
			Position p = ed.cursor;
			bool at_end = false;
			for (long k = 0; k < request.count && !at_end; ++k) {
				p = nextWordStart(ed, p, big, at_end);
			}

			const bool moved = p.row != ed.cursor.row || p.col != ed.cursor.col;
			if (!moved) return false;

			const bool crossed = p.row > ed.cursor.row && onlyBlanksBefore(ed.lines[p.row], p.col);
			const bool from_empty_line = p.row == ed.cursor.row + 1 && currentText(ed).empty();
			if (request.for_operator && crossed && from_empty_line) p.col = 0;
			else if (request.for_operator && crossed) p = { p.row - 1, ed.lines[p.row - 1].size() };
			motion = motionTo(p, false);
			return true;
		}

		static bool nextWordEnd(const Editor& ed, Position& p, bool big) {
			if (!stepForward(ed, p)) return false;

			while (charClass(ed.lines[p.row], p.col, big) == 0) {
				if (!stepForward(ed, p)) return false;
			}

			const std::string& line = ed.lines[p.row];
			const int kind = charClass(line, p.col, big);
			for (;;) {
				const std::size_t next = nextCharStart(line, p.col);
				if (next >= line.size() || charClass(line, next, big) != kind) return true;
				p.col = next;
			}
		}

		static bool wordEnd(const Editor& ed, long count, bool big, Motion& motion) {
			Position p = ed.cursor;
			for (long k = 0; k < count; ++k) {
				if (!nextWordEnd(ed, p, big) && k == 0) return false;
			}

			motion = motionTo(p, true);
			return true;
		}

		static bool endsWordAt(const Editor& ed, Position p, bool big) {
			const std::string& line = ed.lines[p.row];
			const std::size_t next = nextCharStart(line, p.col);
			return next >= line.size() || charClass(line, next, big) != charClass(line, p.col, big);
		}

		// cw on a word changes to its end, not over the blanks after it.
		bool changeWordMotion(const Editor& ed, long count, bool big, Motion& motion) {
			Position p = ed.cursor;
			for (long k = 0; k < count; ++k) {
				if (k == 0 && endsWordAt(ed, p, big)) continue;
				if (!nextWordEnd(ed, p, big)) break;
			}

			motion = motionTo(p, true);
			return true;
		}

		static bool previousWordStart(const Editor& ed, Position& p, bool big) {
			if (!stepBackward(ed, p)) return false;

			while (charClass(ed.lines[p.row], p.col, big) == 0) {
				if (ed.lines[p.row].empty()) return true;
				if (!stepBackward(ed, p)) return true;
			}

			const std::string& line = ed.lines[p.row];
			const int kind = charClass(line, p.col, big);
			while (p.col > 0) {
				const std::size_t previous = previousCharStart(line, p.col);
				if (charClass(line, previous, big) != kind) break;
				p.col = previous;
			}

			return true;
		}

		static bool wordBackward(const Editor& ed, long count, bool big, Motion& motion) {
			Position p = ed.cursor;
			for (long k = 0; k < count; ++k) {
				if (!previousWordStart(ed, p, big) && k == 0) return false;
			}

			motion = motionTo(p, false);
			return true;
		}

		static bool wordMotions(Editor& ed, const MotionRequest& request, Motion& motion) {
			switch (request.key) {
				case 'w':
				case 'W':
					return wordForward(ed, request, request.key == 'W', motion);
				case 'e':
				case 'E':
					return wordEnd(ed, request.count, request.key == 'E', motion);
				case 'b':
				case 'B':
					return wordBackward(ed, request.count, request.key == 'B', motion);
				default:
					return charMotions(ed, request, motion);
			}
		}

		struct FindSpec {
			std::string target;
			long count = 1;
			bool skip_adjacent = false;
		};

		static bool startsWith(const std::string& line, std::size_t at, const std::string& target) {
			return line.compare(at, target.size(), target) == 0;
		}

		// A repeated t / T passes over a match right next to the cursor,
		// where it would otherwise stay put.
		static bool findForward(const std::string& line, std::size_t col, const FindSpec& spec,
		                        std::size_t& found) {
			std::size_t pos = col;
			for (long k = 0; k < spec.count; ++k) {
				std::size_t from = nextCharStart(line, pos);
				if (k == 0 && spec.skip_adjacent && startsWith(line, from, spec.target)) {
					from = nextCharStart(line, from);
				}

				if (from >= line.size()) return false;

				const std::size_t hit = line.find(spec.target, from);
				if (hit == std::string::npos) return false;
				pos = hit;
			}

			found = pos;
			return true;
		}

		static bool findBackward(const std::string& line, std::size_t col, const FindSpec& spec,
		                         std::size_t& found) {
			std::size_t pos = col;
			for (long k = 0; k < spec.count; ++k) {
				if (pos == 0) return false;

				std::size_t from = previousCharStart(line, pos);
				if (k == 0 && spec.skip_adjacent && startsWith(line, from, spec.target)) {
					if (from == 0) return false;
					from = previousCharStart(line, from);
				}

				const std::size_t hit = line.rfind(spec.target, from);
				if (hit == std::string::npos) return false;
				pos = hit;
			}

			found = pos;
			return true;
		}

		static bool findInLine(const Editor& ed, char kind, long count, bool repeat,
		                       Motion& motion) {
			const std::string& line = ed.lines[ed.cursor.row];
			const bool forward = kind == 'f' || kind == 't';
			const bool till = kind == 't' || kind == 'T';
			FindSpec spec;
			spec.target = keyText(ed.find_char);
			spec.count = count;
			spec.skip_adjacent = repeat && till;

			std::size_t found = 0;
			const bool hit = forward ? findForward(line, ed.cursor.col, spec, found)
				: findBackward(line, ed.cursor.col, spec, found);
			if (!hit) return false;

			if (till) found = forward ? previousCharStart(line, found) : nextCharStart(line, found);
			motion = motionTo({ ed.cursor.row, found }, forward);
			return true;
		}

		static char reversedFind(char kind) {
			switch (kind) {
				case 'f':
					return 'F';
				case 'F':
					return 'f';
				case 't':
					return 'T';
				default:
					return 't';
			}
		}

		static bool findMotion(Editor& ed, const MotionRequest& request, Motion& motion) {
			const Key key = request.key;
			if (key == ';' || key == ',') {
				if (ed.find_kind == 0) return false;
				const char kind = key == ';' ? ed.find_kind : reversedFind(ed.find_kind);
				return findInLine(ed, kind, request.count, true, motion);
			}

			Key target = 0;
			if (!nextKey(ed, target) || target == kKeyEscape) return false;

			ed.find_char = target;
			ed.find_kind = static_cast<char>(key);
			return findInLine(ed, ed.find_kind, request.count, false, motion);
		}

		static bool matchBracket(const Editor& ed, Position p, Motion& motion) {
			const char open = ed.lines[p.row][p.col];
			const char* const at = std::strchr(kBrackets, open);
			const std::size_t index = static_cast<std::size_t>(at - kBrackets);
			const bool forward = index % 2 == 0;
			const char close = kBrackets[forward ? index + 1 : index - 1];
			long depth = 0;
			do {
				const std::string& line = ed.lines[p.row];
				const char c = p.col < line.size() ? line[p.col] : '\0';
				if (c == open) ++depth;
				else if (c == close) --depth;
				if (depth == 0) {
					motion = motionTo(p, true);
					motion.jump = true;
					return true;
				}
			} while (forward ? stepForward(ed, p) : stepBackward(ed, p));

			return false;
		}

		static bool percentMotion(const Editor& ed, const MotionRequest& request, Motion& motion) {
			if (request.has_count) {
				if (request.count > static_cast<long>(kPercent)) return false;
				const std::size_t share = static_cast<std::size_t>(request.count) * ed.lines.size();
				const std::size_t row = (share + kPercent - 1) / kPercent;
				motion = lineMotion(ed, row > 0 ? row - 1 : 0, true);
				return true;
			}

			const std::string& line = ed.lines[ed.cursor.row];
			std::size_t col = ed.cursor.col;
			while (col < line.size() && std::strchr(kBrackets, line[col]) == nullptr) ++col;
			if (col >= line.size()) return false;

			return matchBracket(ed, { ed.cursor.row, col }, motion);
		}

		static bool isSentenceEnd(char c) {
			return c == '.' || c == '!' || c == '?';
		}

		static bool isClosing(char c) {
			return c == ')' || c == ']' || c == '"' || c == '\'';
		}

		// Back over blanks and line breaks from @p p; true when a gap was
		// crossed into a sentence end, an empty line or the buffer's start.
		static bool followsSentenceBreak(const Editor& ed, Position p) {
			bool gap = false;
			for (;;) {
				if (p.col == 0) {
					if (p.row == 0) return true;
					--p.row;
					if (ed.lines[p.row].empty()) return true;
					p.col = ed.lines[p.row].size();
					gap = true;
				}

				p.col = previousCharStart(ed.lines[p.row], p.col);
				if (!isBlankChar(ed.lines[p.row], p.col)) break;
				gap = true;
			}

			const std::string& line = ed.lines[p.row];
			while (p.col > 0 && isClosing(line[p.col])) p.col = previousCharStart(line, p.col);
			return gap && isSentenceEnd(line[p.col]);
		}

		static bool isSentenceStart(const Editor& ed, Position p) {
			const std::string& line = ed.lines[p.row];
			if (line.empty()) return p.row == 0 || !ed.lines[p.row - 1].empty();
			if (isBlankChar(line, p.col)) return false;
			return followsSentenceBreak(ed, p);
		}

		static bool sentenceMotion(const Editor& ed, long count, bool forward, Motion& motion) {
			Position p = ed.cursor;
			for (long k = 0; k < count; ++k) {
				bool moved = stepToward(ed, p, forward);
				while (moved && !isSentenceStart(ed, p)) moved = stepToward(ed, p, forward);
				if (!moved && forward) p.col = ed.lines[p.row].size();
			}

			motion = motionTo(p, false);
			motion.jump = true;
			return true;
		}

		static bool paragraphMotion(const Editor& ed, long count, bool forward, Motion& motion) {
			std::size_t row = ed.cursor.row;
			const std::size_t limit = forward ? lastRow(ed) : 0;
			for (long k = 0; k < count; ++k) {
				while (row != limit && ed.lines[row].empty()) row = forward ? row + 1 : row - 1;
				while (row != limit && !ed.lines[row].empty()) row = forward ? row + 1 : row - 1;
			}

			Position target = { row, 0 };
			if (forward && !ed.lines[row].empty()) target.col = ed.lines[row].size();
			motion = motionTo(target, false);
			motion.jump = true;
			return true;
		}

		static bool markMotion(Editor& ed, bool linewise, Motion& motion) {
			Key name = 0;
			if (!nextKey(ed, name) || name == kKeyEscape) return false;

			Position at;
			if (!markPosition(ed, name, at)) {
				setError(ed, "Mark not set");
				return false;
			}

			if (at.row > lastRow(ed)) at.row = lastRow(ed);
			motion = linewise ? lineMotion(ed, at.row, true) : motionTo(at, false);
			motion.jump = true;
			return true;
		}

		bool compileSearch(Editor& ed, const std::string& pattern, PosixRegex& regex) {
			RegexOptions options;
			options.syntax = RegexSyntax::Basic;
			options.ignore_case = ed.options.ignorecase;
			if (regex.compile(pattern, options)) return true;

			setError(ed, regex.error());
			return false;
		}

		static bool matchAfter(const PosixRegex& regex, const std::string& line, std::size_t from,
		                       std::size_t& found) {
			RegexMatch match;
			if (from > line.size() || !regex.search(line, from, match)) return false;

			found = match.start(0);
			return true;
		}

		static bool matchBefore(const PosixRegex& regex, const std::string& line, std::size_t limit,
		                        std::size_t& found) {
			bool any = false;
			std::size_t from = 0;
			std::size_t hit = 0;
			while (matchAfter(regex, line, from, hit) && hit < limit) {
				found = hit;
				any = true;
				from = hit < line.size() ? nextCharStart(line, hit) : line.size() + 1;
			}

			return any;
		}

		struct SearchResult {
			Position found;
			bool wrapped = false;
		};

		// Where a forward search from @p col goes on; past the end on an empty line.
		static std::size_t startAfter(const std::string& line, std::size_t col) {
			return line.empty() ? 1 : nextCharStart(line, col);
		}

		static bool searchForward(const Editor& ed, const PosixRegex& regex, Position from,
		                          SearchResult& result) {
			const std::size_t total = ed.lines.size();
			for (std::size_t k = 0; k <= total; ++k) {
				const std::size_t row = (from.row + k) % total;
				if (from.row + k == total) {
					if (!ed.options.wrapscan) break;
					result.wrapped = true;
				}

				const std::string& line = ed.lines[row];
				const std::size_t start = k == 0 ? startAfter(line, from.col) : 0;
				std::size_t col = 0;
				if (!matchAfter(regex, line, start, col)) continue;
				if (k == total && col > from.col) break;

				result.found = { row, col };
				return true;
			}

			return false;
		}

		static bool searchBackward(const Editor& ed, const PosixRegex& regex, Position from,
		                           SearchResult& result) {
			const std::size_t total = ed.lines.size();
			for (std::size_t k = 0; k <= total; ++k) {
				if (k == from.row + 1) {
					if (!ed.options.wrapscan) break;
					result.wrapped = true;
				}

				const std::size_t row = (from.row + total - k % total) % total;
				const std::string& line = ed.lines[row];
				const std::size_t limit = k == 0 ? from.col : line.size() + 1;
				std::size_t col = 0;
				if (!matchBefore(regex, line, limit, col)) continue;
				if (k == total && col < from.col) break;

				result.found = { row, col };
				return true;
			}

			return false;
		}

		bool findPattern(Editor& ed, const std::string& pattern, bool backward, Position from,
		                 Position& found) {
			PosixRegex regex;
			if (!compileSearch(ed, pattern, regex)) return false;

			SearchResult result;
			const bool hit = backward ? searchBackward(ed, regex, from, result)
				: searchForward(ed, regex, from, result);
			if (!hit) {
				setError(ed, "Pattern not found: " + pattern);
				return false;
			}

			if (result.wrapped) {
				setMessage(ed, backward ? kWrappedToBottom : kWrappedToTop);
			}

			found = result.found;
			return true;
		}

		static bool searchMotion(Editor& ed, const std::string& pattern, bool backward, long count,
		                         Motion& motion) {
			Position p = ed.cursor;
			for (long k = 0; k < count; ++k) {
				if (!findPattern(ed, pattern, backward, p, p)) return false;
			}

			motion = motionTo(p, false);
			motion.jump = true;
			return true;
		}

		static std::string patternBody(const std::string& text, char delimiter) {
			std::string body;
			for (std::size_t at = 0; at < text.size(); ++at) {
				if (text[at] == delimiter) break;
				if (text[at] == '\\' && at + 1 < text.size() && text[at + 1] == delimiter) ++at;
				else if (text[at] == '\\' && at + 1 < text.size()) body.push_back(text[at++]);
				body.push_back(text[at]);
			}

			return body;
		}

		static bool promptSearch(Editor& ed, const MotionRequest& request, Motion& motion) {
			const bool backward = request.key == '?';
			std::string text;
			if (!readCommandLine(ed, request.key, text)) return false;

			std::string pattern = patternBody(text, static_cast<char>(request.key));
			if (pattern.empty()) pattern = ed.last_search;
			if (pattern.empty()) {
				setError(ed, "No previous regular expression");
				return false;
			}

			ed.last_search = pattern;
			ed.search_backward = backward;
			return searchMotion(ed, pattern, backward, request.count, motion);
		}

		static bool repeatSearch(Editor& ed, const MotionRequest& request, Motion& motion) {
			if (ed.last_search.empty()) {
				setError(ed, "No previous regular expression");
				return false;
			}

			const bool backward = request.key == 'N' ? !ed.search_backward : ed.search_backward;
			return searchMotion(ed, ed.last_search, backward, request.count, motion);
		}

		static std::string escapedForRegex(const std::string& word) {
			std::string out;
			for (char c : word) {
				if (std::strchr("\\.*[]^$/~", c) != nullptr) out.push_back('\\');
				out.push_back(c);
			}

			return out;
		}

		static bool isWordChar(const std::string& line, std::size_t pos) {
			return charClass(line, pos, false) == 2;
		}

		static bool wordUnderCursor(const Editor& ed, std::string& word) {
			const std::string& line = ed.lines[ed.cursor.row];
			std::size_t start = ed.cursor.col;
			while (start < line.size() && !isWordChar(line, start)) {
				start = nextCharStart(line, start);
			}

			if (start >= line.size()) return false;

			while (start > 0 && isWordChar(line, previousCharStart(line, start))) {
				start = previousCharStart(line, start);
			}

			std::size_t end = start;
			while (end < line.size() && isWordChar(line, end)) end = nextCharStart(line, end);
			word = line.substr(start, end - start);
			return true;
		}

		static bool starSearch(Editor& ed, const MotionRequest& request, Motion& motion) {
			std::string word;
			if (!wordUnderCursor(ed, word)) {
				setError(ed, "No string under cursor");
				return false;
			}

			ed.last_search = "\\<" + escapedForRegex(word) + "\\>";
			ed.search_backward = request.key == '#';
			return searchMotion(ed, ed.last_search, ed.search_backward, request.count, motion);
		}

		static bool prefixedMotion(Editor& ed, const MotionRequest& request, Motion& motion) {
			Key second = 0;
			if (!nextKey(ed, second) || second != 'g') return false;

			const std::size_t row = request.has_count ? clampRow(ed, request.count - 1) : 0;
			motion = lineMotion(ed, row, true);
			return true;
		}

		static bool otherMotions(Editor& ed, const MotionRequest& request, Motion& motion) {
			switch (request.key) {
				case '%':
					return percentMotion(ed, request, motion);
				case '(':
				case ')':
					return sentenceMotion(ed, request.count, request.key == ')', motion);
				case '{':
				case '}':
					return paragraphMotion(ed, request.count, request.key == '}', motion);
				case '\'':
				case '`':
					return markMotion(ed, request.key == '\'', motion);
				case '/':
				case '?':
					return promptSearch(ed, request, motion);
				case 'n':
				case 'N':
					return repeatSearch(ed, request, motion);
				case '*':
				case '#':
					return starSearch(ed, request, motion);
				case 'g':
					return prefixedMotion(ed, request, motion);
				default:
					return wordMotions(ed, request, motion);
			}
		}

		bool isMotionKey(Key key) {
			static const char* const kMotionKeys = "hjklwWbBeE0^$|GHMLfFtT;,%(){}+-_'`/?nN*# g";
			const bool special = key == kKeyEnter || key == kKeyBackspace || key == ctrlKey('N')
				|| key == ctrlKey('P') || (key >= kKeyUp && key <= kKeyEnd);
			if (special) return true;
			return key < 0x80 && key != 0
				&& std::strchr(kMotionKeys, static_cast<int>(key)) != nullptr;
		}

		bool resolveMotion(Editor& ed, const MotionRequest& request, Motion& motion) {
			const Key key = request.key;
			if (key == 'f' || key == 'F' || key == 't' || key == 'T' || key == ';' || key == ',') {
				return findMotion(ed, request, motion);
			}

			return otherMotions(ed, request, motion);
		}

		void applyMotion(Editor& ed, const Motion& motion) {
			if (motion.jump) rememberContext(ed);

			ed.cursor = motion.target;
			clampCursor(ed);
			if (!motion.keep_column) updateWantColumn(ed);
			if (motion.to_end) ed.want_end = true;
		}

	}  // namespace vi_detail

}  // namespace wbsh

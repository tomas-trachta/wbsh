/**
 * @file vi_normal.cpp
 * @brief vi's normal mode: counts and registers, the d c y < > operators
 *        over motions, the single-key edits, puts, undo, `.` and scrolling.
 */

#include "vi_internal.h"

#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include "utf8.h"

namespace wbsh {

	namespace vi_detail {

		static const long kMaxCount = 1000000;
		static const std::size_t kReportLines = 2;

		struct Command {
			long count = 1;
			bool has_count = false;
			char reg = 0;
			Key key = 0;
		};

		struct Region {
			Position start;
			Position end;
			bool linewise = false;
		};

		static bool isDigitKey(Key key) {
			return key >= '0' && key <= '9';
		}

		static bool isRegisterName(Key key) {
			if (key >= 0x80) return false;
			return std::isalnum(static_cast<int>(key)) != 0 || key == '"' || key == '-'
				|| key == '_';
		}

		static bool readCount(Editor& ed, Key& key, long& count, bool& has_count) {
			while (isDigitKey(key) && (key != '0' || has_count)) {
				const long digit = static_cast<long>(key - '0');
				count = has_count ? count * 10 + digit : digit;
				if (count > kMaxCount) count = kMaxCount;
				has_count = true;
				if (!nextKey(ed, key)) return false;
			}

			return true;
		}

		static bool readCommandPrefix(Editor& ed, Command& cmd) {
			Key key = 0;
			if (!nextKey(ed, key)) return false;

			ed.message.clear();
			ed.message_error = false;
			for (;;) {
				if (!readCount(ed, key, cmd.count, cmd.has_count)) return false;
				if (key != '"') break;

				Key name = 0;
				if (!nextKey(ed, name) || !isRegisterName(name)) return false;
				cmd.reg = static_cast<char>(name);
				if (!nextKey(ed, key)) return false;
			}

			if (!cmd.has_count) cmd.count = 1;
			cmd.key = key;
			return true;
		}

		static bool before(Position a, Position b) {
			return a.row < b.row || (a.row == b.row && a.col < b.col);
		}

		static std::string linesText(const Editor& ed, std::size_t first, std::size_t last) {
			std::string text;
			for (std::size_t row = first; row <= last; ++row) text += ed.lines[row] + "\n";
			return text;
		}

		static Register regionRegister(const Editor& ed, const Region& region) {
			Register value;
			value.linewise = region.linewise;
			value.text = region.linewise ? linesText(ed, region.start.row, region.end.row)
				: textBetween(ed, region.start, region.end);
			return value;
		}

		// An exclusive motion that ends at the start of a later line stops
		// at the end of the line before; from the start of a line it takes
		// whole lines.
		static void applyExclusiveRule(const Editor& ed, Region& region) {
			const bool at_line_start = region.end.col == 0 && region.end.row > region.start.row;
			if (!at_line_start) return;

			const std::string& first = ed.lines[region.start.row];
			bool only_blanks = true;
			for (std::size_t at = 0; at < region.start.col && only_blanks; ++at) {
				only_blanks = first[at] == ' ' || first[at] == '\t';
			}

			--region.end.row;
			region.end.col = ed.lines[region.end.row].size();
			region.linewise = only_blanks;
		}

		static Region regionFor(const Editor& ed, const Motion& motion) {
			Region region;
			region.start = ed.cursor;
			region.end = motion.target;
			if (before(region.end, region.start)) std::swap(region.start, region.end);

			region.linewise = motion.linewise;
			if (region.linewise) return region;

			if (motion.inclusive) {
				region.end.col = nextCharStart(ed.lines[region.end.row], region.end.col);
				return region;
			}

			applyExclusiveRule(ed, region);
			return region;
		}

		static void reportLines(Editor& ed, std::size_t count, const char* what) {
			if (count > kReportLines) setMessage(ed, std::to_string(count) + " " + what);
		}

		static void deleteRegion(Editor& ed, const Region& region, char reg) {
			storeRegister(ed, reg, regionRegister(ed, region), true);
			if (!region.linewise) {
				deleteBetween(ed, region.start, region.end);
				ed.cursor = region.start;
				updateWantColumn(ed);
				return;
			}

			const std::size_t count = region.end.row - region.start.row + 1;
			deleteLineRange(ed, region.start.row, count);
			moveToFirstNonBlank(ed, region.start.row);
			reportLines(ed, count, "fewer lines");
		}

		static void yankRegion(Editor& ed, const Region& region, char reg) {
			storeRegister(ed, reg, regionRegister(ed, region), false);
			if (region.linewise) {
				ed.cursor.row = region.start.row;
				reportLines(ed, region.end.row - region.start.row + 1, "lines yanked");
			} else {
				ed.cursor = region.start;
			}

			clampCursor(ed);
			updateWantColumn(ed);
		}

		static void changeRegion(Editor& ed, const Region& region, char reg) {
			storeRegister(ed, reg, regionRegister(ed, region), true);
			if (!region.linewise) {
				deleteBetween(ed, region.start, region.end);
				ed.cursor = region.start;
				runInsertMode(ed, InsertKind::Change, 1);
				return;
			}

			const std::string& first = ed.lines[region.start.row];
			const std::string indent = ed.options.autoindent ? leadingIndent(first) : std::string();
			replaceLines(ed, region.start.row, region.end.row - region.start.row + 1, { indent });
			ed.cursor = { region.start.row, indent.size() };
			runInsertMode(ed, InsertKind::Change, 1);
		}

		static void shiftRow(Editor& ed, std::size_t row, bool right) {
			const std::string& line = ed.lines[row];
			if (line.empty()) return;

			const std::size_t width = indentWidth(ed, line);
			const std::size_t step = ed.options.shiftwidth;
			const std::size_t wanted = right ? width + step : (width > step ? width - step : 0);
			const std::string body = line.substr(leadingIndent(line).size());
			setLine(ed, row, indentOfWidth(ed, wanted) + body);
		}

		void shiftRows(Editor& ed, std::size_t first, std::size_t last, bool right,
		               std::size_t times) {
			for (std::size_t row = first; row <= last; ++row) {
				for (std::size_t k = 0; k < times; ++k) shiftRow(ed, row, right);
			}

			moveToFirstNonBlank(ed, first);
		}

		static void shiftRegion(Editor& ed, const Region& region, bool right) {
			shiftRows(ed, region.start.row, region.end.row, right, 1);

			moveToFirstNonBlank(ed, region.start.row);
			const std::size_t count = region.end.row - region.start.row + 1;
			reportLines(ed, count, right ? "lines >ed 1 time" : "lines <ed 1 time");
		}

		static void applyOperator(Editor& ed, Key op, Region region, char reg) {
			switch (op) {
				case 'd':
					deleteRegion(ed, region, reg);
					break;
				case 'y':
					yankRegion(ed, region, reg);
					break;
				case 'c':
					changeRegion(ed, region, reg);
					break;
				case '<':
				case '>':
					region.linewise = true;
					shiftRegion(ed, region, op == '>');
					break;
				default:
					break;
			}
		}

		static bool doubledRegion(const Editor& ed, long count, Region& region) {
			const std::size_t last = ed.lines.size() - 1;
			const std::size_t wanted = ed.cursor.row + static_cast<std::size_t>(count - 1);
			region.start = { ed.cursor.row, 0 };
			region.end = { wanted > last ? last : wanted, 0 };
			region.linewise = true;
			return true;
		}

		static bool blankUnderCursor(const Editor& ed) {
			const std::string& line = ed.lines[ed.cursor.row];
			return ed.cursor.col >= line.size() || isBlankChar(line, ed.cursor.col);
		}

		static bool motionRegion(Editor& ed, Key op, const MotionRequest& request, Region& region) {
			Motion motion;
			const bool change_word = op == 'c' && (request.key == 'w' || request.key == 'W')
				&& !blankUnderCursor(ed);
			const bool big = request.key == 'W';
			const bool found = change_word ? changeWordMotion(ed, request.count, big, motion)
				: resolveMotion(ed, request, motion);
			if (!found) return false;

			if (motion.jump) rememberContext(ed);
			region = regionFor(ed, motion);
			return true;
		}

		static bool runOperator(Editor& ed, const Command& cmd) {
			MotionRequest request;
			request.key = 0;
			request.count = 1;
			if (!nextKey(ed, request.key)) return false;
			if (!readCount(ed, request.key, request.count, request.has_count)) return false;

			request.count = request.has_count ? request.count * cmd.count : cmd.count;
			request.has_count = request.has_count || cmd.has_count;
			request.for_operator = true;

			Region region;
			const bool doubled = request.key == cmd.key;
			if (!doubled && !isMotionKey(request.key)) return false;
			if (doubled) doubledRegion(ed, request.count, region);
			else if (!motionRegion(ed, cmd.key, request, region)) return false;

			applyOperator(ed, cmd.key, region, cmd.reg);
			return true;
		}

		static bool runOperatorWith(Editor& ed, const Command& cmd, Key op, Key motion_key) {
			Command expanded = cmd;
			expanded.key = op;
			ed.injected.push_front(motion_key);
			return runOperator(ed, expanded);
		}

		static bool deleteChars(Editor& ed, const Command& cmd, bool backward) {
			const std::string& line = ed.lines[ed.cursor.row];
			if (line.empty() || (backward && ed.cursor.col == 0)) return false;

			Region region;
			region.start = ed.cursor;
			region.end = ed.cursor;
			for (long k = 0; k < cmd.count; ++k) {
				if (backward) region.start.col = previousCharStart(line, region.start.col);
				else region.end.col = nextCharStart(line, region.end.col);
			}

			deleteRegion(ed, region, cmd.reg);
			return true;
		}

		static bool replaceChars(Editor& ed, const Command& cmd) {
			Key with = 0;
			if (!nextKey(ed, with) || with == kKeyEscape) return false;

			const std::string& line = ed.lines[ed.cursor.row];
			std::size_t end = ed.cursor.col;
			for (long k = 0; k < cmd.count; ++k) {
				if (end >= line.size()) return false;
				end = nextCharStart(line, end);
			}

			const Position start = ed.cursor;
			if (with == kKeyEnter) {
				deleteBetween(ed, start, { start.row, end });
				insertText(ed, start, "\n");
				moveToFirstNonBlank(ed, start.row + 1);
				return true;
			}

			std::string text;
			for (long k = 0; k < cmd.count; ++k) text += keyText(with);
			deleteBetween(ed, start, { start.row, end });
			const Position after = insertText(ed, start, text);
			ed.cursor = { start.row, previousCharStart(ed.lines[start.row], after.col) };
			updateWantColumn(ed);
			return true;
		}

		static std::string toggledCase(const std::string& text) {
			const Utf8Char decoded = decodeUtf8(text, 0);
			const char32_t upper = upperCode(decoded.code);
			const char32_t changed = upper != decoded.code ? upper : lowerCode(decoded.code);
			if (changed == decoded.code) return text;

			std::string out;
			appendUtf8(out, changed);
			return out + text.substr(decoded.length);
		}

		static bool toggleCase(Editor& ed, const Command& cmd) {
			std::string line = ed.lines[ed.cursor.row];
			if (line.empty()) return false;

			std::size_t col = ed.cursor.col;
			std::string result = line.substr(0, col);
			for (long k = 0; k < cmd.count && col < line.size(); ++k) {
				const std::size_t next = nextCharStart(line, col);
				result += toggledCase(line.substr(col, next - col));
				col = next;
			}

			const std::size_t cursor = result.size();
			setLine(ed, ed.cursor.row, result + line.substr(col));
			ed.cursor.col = cursor;
			clampCursor(ed);
			updateWantColumn(ed);
			return true;
		}

		static bool endsInBlank(const std::string& text) {
			return !text.empty() && (text.back() == ' ' || text.back() == '\t');
		}

		static std::string joinedPiece(const std::string& line, bool add_space) {
			std::size_t start = 0;
			while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) ++start;
			const std::string body = line.substr(start);
			if (!add_space || body.empty() || body[0] == ')') return body;
			return " " + body;
		}

		static bool joinLines(Editor& ed, const Command& cmd, bool add_spaces) {
			const std::size_t wanted = static_cast<std::size_t>(cmd.count < 2 ? 2 : cmd.count);
			const std::size_t row = ed.cursor.row;
			if (row + 1 >= ed.lines.size()) return false;

			const std::size_t available = ed.lines.size() - row;
			joinRows(ed, row, wanted > available ? available : wanted, add_spaces);
			return true;
		}

		void joinRows(Editor& ed, std::size_t row, std::size_t count, bool add_spaces) {
			std::string joined = ed.lines[row];
			std::size_t join_point = joined.size();
			for (std::size_t k = 1; k < count; ++k) {
				const bool spaced = add_spaces && !joined.empty() && !endsInBlank(joined);
				join_point = joined.size();
				joined += joinedPiece(ed.lines[row + k], spaced);
			}

			replaceLines(ed, row, count, { joined });
			ed.cursor = { row, join_point };
			clampCursor(ed);
			updateWantColumn(ed);
		}

		std::vector<std::string> registerLines(const std::string& text) {
			std::vector<std::string> lines;
			std::size_t start = 0;
			while (start < text.size()) {
				std::size_t end = text.find('\n', start);
				if (end == std::string::npos) end = text.size();
				lines.push_back(text.substr(start, end - start));
				start = end + 1;
			}

			return lines;
		}

		static void putLines(Editor& ed, const Register& value, long count, bool after) {
			const std::vector<std::string> one = registerLines(value.text);
			std::vector<std::string> lines;
			for (long k = 0; k < count; ++k) lines.insert(lines.end(), one.begin(), one.end());

			const std::size_t at = after ? ed.cursor.row + 1 : ed.cursor.row;
			insertLineTexts(ed, at, lines);
			moveToFirstNonBlank(ed, at);
		}

		static void putChars(Editor& ed, const Register& value, long count, bool after) {
			std::string text;
			for (long k = 0; k < count; ++k) text += value.text;

			Position at = ed.cursor;
			const std::string& line = ed.lines[at.row];
			if (after && !line.empty()) at.col = nextCharStart(line, at.col);

			const Position end = insertText(ed, at, text);
			const bool multiline = text.find('\n') != std::string::npos;
			const Position last_char = { end.row, previousCharStart(ed.lines[end.row], end.col) };
			ed.cursor = multiline ? at : last_char;
			updateWantColumn(ed);
		}

		static bool putRegister(Editor& ed, const Command& cmd, bool after) {
			Register value;
			if (!fetchRegister(ed, cmd.reg, value)) {
				setError(ed, std::string("Nothing in register ") + (cmd.reg == 0 ? '"' : cmd.reg));
				return false;
			}

			if (value.linewise) putLines(ed, value, cmd.count, after);
			else putChars(ed, value, cmd.count, after);
			return true;
		}

		static std::vector<Key> withoutCount(const std::vector<Key>& keys) {
			std::vector<Key> out;
			std::size_t at = 0;
			if (keys.size() >= 2 && keys[0] == '"') {
				out.assign(keys.begin(), keys.begin() + 2);
				at = 2;
			}

			while (at < keys.size() && isDigitKey(keys[at]) && keys[at] != '0') {
				while (at < keys.size() && isDigitKey(keys[at])) ++at;
				if (at + 1 < keys.size() && keys[at] == '"') {
					out.push_back(keys[at]);
					out.push_back(keys[at + 1]);
					at += 2;
				}
			}

			out.insert(out.end(), keys.begin() + static_cast<std::ptrdiff_t>(at), keys.end());
			return out;
		}

		static bool repeatLastChange(Editor& ed, const Command& cmd) {
			if (ed.dot_keys.empty()) return false;

			std::vector<Key> keys = cmd.has_count ? withoutCount(ed.dot_keys) : ed.dot_keys;
			if (cmd.has_count) {
				const std::string digits = std::to_string(cmd.count);
				keys.insert(keys.begin(), digits.begin(), digits.end());
			}

			ed.pending.insert(ed.pending.begin(), keys.begin(), keys.end());
			return false;
		}

		static bool repeatHistory(Editor& ed, long count, bool redo) {
			for (long k = 0; k < count; ++k) {
				if (!(redo ? redoChange(ed) : undoChange(ed))) break;
			}

			return false;
		}

		static bool setMarkCommand(Editor& ed) {
			Key name = 0;
			if (!nextKey(ed, name)) return false;
			if (!setMark(ed, name, ed.cursor)) setError(ed, "Invalid mark name");
			return false;
		}

		static bool zCommand(Editor& ed) {
			Key second = 0;
			if (!nextKey(ed, second)) return false;
			if (second == 'Z') runExCommand(ed, "x");
			else if (second == 'Q') runExCommand(ed, "q!");
			return false;
		}

		static bool colonCommand(Editor& ed) {
			std::string text;
			if (readCommandLine(ed, ':', text)) runExCommand(ed, text);
			return false;
		}

		static std::size_t halfScreen(const Editor& ed) {
			const std::size_t half = textRows(ed) / 2;
			return half > 0 ? half : 1;
		}

		static std::size_t movedRow(std::size_t row, std::size_t steps, bool down,
		                            std::size_t last) {
			if (down) return row + steps > last ? last : row + steps;
			return steps > row ? 0 : row - steps;
		}

		static bool scrollLines(Editor& ed, long count, bool down) {
			const std::size_t last = ed.lines.size() - 1;
			const std::size_t steps = static_cast<std::size_t>(count);
			if (down && ed.top >= last) return false;
			if (!down && ed.top == 0) return false;

			ed.top = movedRow(ed.top, steps, down, last);
			ed.top_skip = 0;
			if (ed.cursor.row < ed.top) ed.cursor.row = ed.top;
			const std::size_t bottom = lastVisibleLine(ed);
			if (ed.cursor.row > bottom) ed.cursor.row = bottom;
			ed.cursor.col = byteAtColumn(ed, ed.cursor.row, ed.want_column);
			clampCursor(ed);
			return false;
		}

		static bool scrollHalf(Editor& ed, const Command& cmd, bool down) {
			const std::size_t last = ed.lines.size() - 1;
			const std::size_t steps = cmd.has_count ? static_cast<std::size_t>(cmd.count)
				: halfScreen(ed);
			if ((down && ed.cursor.row >= last) || (!down && ed.cursor.row == 0)) return false;

			ed.cursor.row = movedRow(ed.cursor.row, steps, down, last);
			ed.top = movedRow(ed.top, steps, down, last);
			ed.top_skip = 0;
			moveToFirstNonBlank(ed, ed.cursor.row);
			return false;
		}

		static bool scrollPages(Editor& ed, long count, bool down) {
			const std::size_t last = ed.lines.size() - 1;
			for (long k = 0; k < count; ++k) {
				if ((down && ed.top >= last) || (!down && ed.top == 0)) break;

				const std::size_t bottom = lastVisibleLine(ed);
				const std::size_t rows = textRows(ed);
				std::size_t top = 0;
				if (down) top = bottom > ed.top + 1 ? bottom - 1 : ed.top + 1;
				else top = ed.top + 2 > rows ? ed.top + 2 - rows : 0;
				ed.top = top > last ? last : top;
				ed.top_skip = 0;
			}

			if (ed.cursor.row < ed.top) moveToFirstNonBlank(ed, ed.top);
			if (ed.cursor.row > lastVisibleLine(ed)) moveToFirstNonBlank(ed, lastVisibleLine(ed));
			return false;
		}

		static bool scrollCommands(Editor& ed, const Command& cmd) {
			switch (cmd.key) {
				case ctrlKey('F'):
				case kKeyPageDown:
					return scrollPages(ed, cmd.count, true);
				case ctrlKey('B'):
				case kKeyPageUp:
					return scrollPages(ed, cmd.count, false);
				case ctrlKey('D'):
					return scrollHalf(ed, cmd, true);
				case ctrlKey('U'):
					return scrollHalf(ed, cmd, false);
				case ctrlKey('E'):
					return scrollLines(ed, cmd.count, true);
				case ctrlKey('Y'):
					return scrollLines(ed, cmd.count, false);
				case ctrlKey('G'):
					showFileInfo(ed);
					return false;
				default:
					return false;
			}
		}

		static bool motionCommand(Editor& ed, const Command& cmd) {
			MotionRequest request;
			if (!nextKey(ed, request.key)) return false;

			request.count = cmd.count;
			request.has_count = cmd.has_count;
			Motion motion;
			if (resolveMotion(ed, request, motion)) applyMotion(ed, motion);
			return false;
		}

		static bool gCommand(Editor& ed, const Command& cmd) {
			Key second = 0;
			if (!nextKey(ed, second)) return false;

			switch (second) {
				case 'g':
					ed.injected.push_front('g');
					ed.injected.push_front('g');
					return motionCommand(ed, cmd);
				case 'J':
					return joinLines(ed, cmd, false);
				case 'I':
					ed.cursor.col = 0;
					runInsertMode(ed, InsertKind::Insert, cmd.count);
					return true;
				default:
					return false;
			}
		}

		static bool insertCommands(Editor& ed, const Command& cmd) {
			switch (cmd.key) {
				case 'i':
				case kKeyInsert:
					runInsertMode(ed, InsertKind::Insert, cmd.count);
					return true;
				case 'a':
					runInsertMode(ed, InsertKind::Append, cmd.count);
					return true;
				case 'I':
					runInsertMode(ed, InsertKind::InsertLine, cmd.count);
					return true;
				case 'A':
					runInsertMode(ed, InsertKind::AppendLine, cmd.count);
					return true;
				case 'o':
					runInsertMode(ed, InsertKind::OpenBelow, cmd.count);
					return true;
				case 'O':
					runInsertMode(ed, InsertKind::OpenAbove, cmd.count);
					return true;
				case 'R':
					runInsertMode(ed, InsertKind::Replace, cmd.count);
					return true;
				default:
					return scrollCommands(ed, cmd);
			}
		}

		static bool editCommands(Editor& ed, const Command& cmd) {
			switch (cmd.key) {
				case 'x':
				case kKeyDelete:
					return deleteChars(ed, cmd, false);
				case 'X':
					return deleteChars(ed, cmd, true);
				case 'D':
					return runOperatorWith(ed, cmd, 'd', '$');
				case 'C':
					return runOperatorWith(ed, cmd, 'c', '$');
				case 'Y':
					return runOperatorWith(ed, cmd, 'y', 'y');
				case 's':
					return runOperatorWith(ed, cmd, 'c', 'l');
				case 'S':
					return runOperatorWith(ed, cmd, 'c', 'c');
				case 'r':
					return replaceChars(ed, cmd);
				case '~':
					return toggleCase(ed, cmd);
				case 'J':
					return joinLines(ed, cmd, true);
				case 'p':
				case 'P':
					return putRegister(ed, cmd, cmd.key == 'p');
				default:
					return insertCommands(ed, cmd);
			}
		}

		static bool otherCommands(Editor& ed, const Command& cmd) {
			switch (cmd.key) {
				case 'd':
				case 'c':
				case 'y':
				case '<':
				case '>':
					return runOperator(ed, cmd);
				case 'u':
					return repeatHistory(ed, cmd.count, false);
				case ctrlKey('R'):
					return repeatHistory(ed, cmd.count, true);
				case '.':
					return repeatLastChange(ed, cmd);
				case 'm':
					return setMarkCommand(ed);
				case 'Z':
					return zCommand(ed);
				case ':':
					return colonCommand(ed);
				case '&':
					runExCommand(ed, "s");
					return true;
				case 'g':
					return gCommand(ed, cmd);
				default:
					return editCommands(ed, cmd);
			}
		}

		// Returns whether `.` may repeat the command.
		static bool executeCommand(Editor& ed, const Command& cmd) {
			const bool motion = isMotionKey(cmd.key) && cmd.key != 'g';
			if (!motion) return otherCommands(ed, cmd);

			ed.injected.push_front(cmd.key);
			return motionCommand(ed, cmd);
		}

		static void startRecording(Editor& ed) {
			ed.recording.clear();
			ed.record_on = true;
			ed.quit_warning_active = ed.quit_warned;
			ed.quit_warned = false;
		}

		void runNormalCommand(Editor& ed) {
			startRecording(ed);
			Command cmd;
			if (!readCommandPrefix(ed, cmd)) {
				ed.record_on = false;
				return;
			}

			beginUndoGroup(ed);
			ed.text_changed = false;
			const bool repeatable = executeCommand(ed, cmd);
			endUndoGroup(ed);

			ed.record_on = false;
			if (ed.text_changed && repeatable) ed.dot_keys = ed.recording;
			ed.mode = Mode::Normal;
			clampCursor(ed);
			scrollToCursor(ed);
		}

	}  // namespace vi_detail

}  // namespace wbsh

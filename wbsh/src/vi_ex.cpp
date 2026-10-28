/**
 * @file vi_ex.cpp
 * @brief vi's command line: reading it, ex addresses and ranges, the
 *        command table, and the line commands (d y j m t > < pu ma = p).
 */

#include "vi_internal.h"

#include <cctype>
#include <string>
#include <vector>

namespace wbsh {

	namespace vi_detail {

		using ExHandler = bool (*)(Editor&, const ExCommand&);

		struct ExEntry {
			const char* name;
			std::size_t minimum;
			ExHandler handler;
		};

		static void popCharacter(std::string& text) {
			while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) {
				text.pop_back();
			}

			if (!text.empty()) text.pop_back();
		}

		static void eraseWord(std::string& text) {
			while (!text.empty() && text.back() == ' ') text.pop_back();
			while (!text.empty() && text.back() != ' ') text.pop_back();
		}

		static void recallHistory(const Editor& ed, std::size_t& index, bool older,
		                          std::string& text) {
			if (older && index > 0) --index;
			else if (!older && index < ed.history.size()) ++index;
			text = index < ed.history.size() ? ed.history[index] : std::string();
		}

		// False once the key ends the line; @p accepted says how.
		static bool editCommandLine(Editor& ed, Key key, std::string& text, std::size_t& index,
		                            bool& accepted) {
			Key literal = 0;
			switch (key) {
				case kKeyEnter:
					accepted = true;
					return false;
				case kKeyEscape:
				case ctrlKey('C'):
					return false;
				case kKeyBackspace:
					if (text.empty()) return false;
					popCharacter(text);
					return true;
				case ctrlKey('U'):
					text.clear();
					return true;
				case ctrlKey('W'):
					eraseWord(text);
					return true;
				case ctrlKey('V'):
					if (nextKey(ed, literal)) text += keyText(literal);
					return true;
				case kKeyUp:
				case kKeyDown:
					recallHistory(ed, index, key == kKeyUp, text);
					return true;
				default:
					if (key >= 0x20 || key == kKeyTab) text += keyText(key);
					return true;
			}
		}

		bool readCommandLine(Editor& ed, Key leader, std::string& text) {
			text.clear();
			std::size_t index = ed.history.size();
			bool accepted = false;
			bool editing = true;
			while (editing) {
				ed.command_active = true;
				ed.command_line = keyText(leader) + text;

				Key key = 0;
				if (!nextKey(ed, key)) break;
				editing = editCommandLine(ed, key, text, index, accepted);
			}

			ed.command_active = false;
			ed.command_line.clear();
			if (accepted && leader == ':' && !text.empty()) ed.history.push_back(text);
			return accepted;
		}

		static std::size_t currentLine(const Editor& ed) {
			return ed.cursor.row + 1;
		}

		static void skipBlanks(const std::string& text, std::size_t& pos) {
			while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) ++pos;
		}

		static bool isDigit(char c) {
			return c >= '0' && c <= '9';
		}

		static long long takeNumber(const std::string& text, std::size_t& pos) {
			long long value = 0;
			while (pos < text.size() && isDigit(text[pos])) {
				value = value * 10 + (text[pos++] - '0');
				if (value > 1000000000LL) value = 1000000000LL;
			}

			return value;
		}

		static std::string delimitedPattern(const std::string& text, std::size_t& pos,
		                                    char delimiter) {
			std::string pattern;
			while (pos < text.size() && text[pos] != delimiter) {
				if (text[pos] == '\\' && pos + 1 < text.size()) {
					if (text[pos + 1] != delimiter) pattern.push_back('\\');
					++pos;
				}

				pattern.push_back(text[pos++]);
			}

			if (pos < text.size()) ++pos;
			return pattern;
		}

		static bool searchLine(Editor& ed, const std::string& pattern, bool backward,
		                       long long& value) {
			PosixRegex regex;
			if (!compileSearch(ed, pattern, regex)) return false;

			const std::size_t total = ed.lines.size();
			std::size_t row = ed.cursor.row;
			for (std::size_t step = 0; step < total; ++step) {
				row = backward ? (row + total - 1) % total : (row + 1) % total;
				if (!regex.test(ed.lines[row])) continue;

				value = static_cast<long long>(row + 1);
				return true;
			}

			setError(ed, "Pattern not found: " + pattern);
			return false;
		}

		static bool searchAddress(Editor& ed, const std::string& text, std::size_t& pos,
		                          long long& value) {
			const char delimiter = text[pos++];
			std::string pattern = delimitedPattern(text, pos, delimiter);
			if (pattern.empty()) pattern = ed.last_search;
			if (pattern.empty()) {
				setError(ed, "No previous regular expression");
				return false;
			}

			ed.last_search = pattern;
			return searchLine(ed, pattern, delimiter == '?', value);
		}

		static bool markAddress(Editor& ed, const std::string& text, std::size_t& pos,
		                        long long& value) {
			Position at;
			const Key name = pos + 1 < text.size() ? static_cast<Key>(text[pos + 1]) : 0;
			pos += 2;
			if (!markPosition(ed, name, at)) {
				setError(ed, "Mark not set");
				return false;
			}

			value = static_cast<long long>(at.row + 1);
			return true;
		}

		static bool baseAddress(Editor& ed, const std::string& text, std::size_t& pos,
		                        long long& value, bool& have) {
			const char c = pos < text.size() ? text[pos] : '\0';
			have = true;
			if (c == '.') {
				++pos;
				value = static_cast<long long>(currentLine(ed));
			} else if (c == '$') {
				++pos;
				value = static_cast<long long>(ed.lines.size());
			} else if (isDigit(c)) {
				value = takeNumber(text, pos);
			} else if (c == '\'') {
				return markAddress(ed, text, pos, value);
			} else if (c == '/' || c == '?') {
				return searchAddress(ed, text, pos, value);
			} else if (c == '+' || c == '-') {
				value = static_cast<long long>(currentLine(ed));
			} else {
				have = false;
			}

			return true;
		}

		static void applyOffsets(const std::string& text, std::size_t& pos, long long& value) {
			for (;;) {
				skipBlanks(text, pos);
				if (pos >= text.size() || (text[pos] != '+' && text[pos] != '-')) return;

				const char sign = text[pos++];
				const long long amount = pos < text.size() && isDigit(text[pos])
					? takeNumber(text, pos) : 1;
				value += sign == '+' ? amount : -amount;
			}
		}

		bool parseExAddress(Editor& ed, const std::string& text, std::size_t& pos,
		                    std::size_t& line, bool& have) {
			skipBlanks(text, pos);
			long long value = 0;
			if (!baseAddress(ed, text, pos, value, have)) return false;
			if (!have) return true;

			applyOffsets(text, pos, value);
			if (value < 0 || value > static_cast<long long>(ed.lines.size())) {
				setError(ed, "Invalid range");
				return false;
			}

			line = static_cast<std::size_t>(value);
			return true;
		}

		static void storeRange(const std::vector<std::size_t>& list, ExRange& range) {
			range.count = list.size() < 2 ? list.size() : 2;
			if (list.empty()) return;

			range.last = list.back();
			range.first = list.size() >= 2 ? list[list.size() - 2] : list.back();
		}

		static bool parseRange(Editor& ed, const std::string& text, std::size_t& pos,
		                       ExRange& range) {
			std::vector<std::size_t> list;
			skipBlanks(text, pos);
			if (pos < text.size() && text[pos] == '%') {
				++pos;
				list.push_back(1);
				list.push_back(ed.lines.size());
			}

			std::size_t line = 0;
			bool have = false;
			if (list.empty() && !parseExAddress(ed, text, pos, line, have)) return false;
			if (have) list.push_back(line);

			while (pos < text.size() && (text[pos] == ',' || text[pos] == ';')) {
				const char separator = text[pos++];
				if (list.empty()) list.push_back(separator == ',' ? 1 : currentLine(ed));
				if (separator == ';' && list.back() > 0) ed.cursor.row = list.back() - 1;
				if (!parseExAddress(ed, text, pos, line, have)) return false;
				list.push_back(have ? line : list.back());
			}

			storeRange(list, range);
			return true;
		}

		static bool lineRange(Editor& ed, const ExCommand& cmd, ExRange& range) {
			range = cmd.range;
			if (range.count == 0) range.first = range.last = currentLine(ed);
			if (range.first == 0 && range.count > 0) range.first = 1;

			const bool valid = range.first >= 1 && range.first <= range.last
				&& range.last <= ed.lines.size();
			if (!valid) setError(ed, "Invalid range");
			return valid;
		}

		// The `{count}` after a command: that many lines from the range's last.
		static bool applyCount(Editor& ed, const std::string& args, std::size_t& pos,
		                       ExRange& range) {
			skipBlanks(args, pos);
			if (pos >= args.size() || !isDigit(args[pos])) return true;

			const long long count = takeNumber(args, pos);
			if (count <= 0) {
				setError(ed, "Positive count required");
				return false;
			}

			range.first = range.last;
			range.last = range.first + static_cast<std::size_t>(count) - 1;
			if (range.last > ed.lines.size()) range.last = ed.lines.size();
			return true;
		}

		static char registerArgument(const std::string& args, std::size_t& pos) {
			skipBlanks(args, pos);
			if (pos >= args.size() || isDigit(args[pos])) return 0;

			const char name = args[pos];
			if (std::isalpha(static_cast<unsigned char>(name)) == 0 && name != '"' && name != '_') {
				return 0;
			}

			++pos;
			return name;
		}

		static std::string rangeText(const Editor& ed, const ExRange& range) {
			std::string text;
			for (std::size_t line = range.first; line <= range.last; ++line) {
				text += ed.lines[line - 1] + "\n";
			}

			return text;
		}

		static bool yankOrDelete(Editor& ed, const ExCommand& cmd, bool remove) {
			ExRange range;
			std::size_t pos = 0;
			if (!lineRange(ed, cmd, range)) return false;

			const char name = registerArgument(cmd.args, pos);
			if (!applyCount(ed, cmd.args, pos, range)) return false;

			Register value;
			value.linewise = true;
			value.text = rangeText(ed, range);
			storeRegister(ed, name, value, remove);
			if (!remove) return true;

			deleteLineRange(ed, range.first - 1, range.last - range.first + 1);
			moveToFirstNonBlank(ed, range.first - 1);
			return true;
		}

		static bool exDelete(Editor& ed, const ExCommand& cmd) {
			return yankOrDelete(ed, cmd, true);
		}

		static bool exYank(Editor& ed, const ExCommand& cmd) {
			return yankOrDelete(ed, cmd, false);
		}

		static bool exJoin(Editor& ed, const ExCommand& cmd) {
			ExCommand adjusted = cmd;
			if (cmd.range.count < 2) {
				adjusted.range.first = cmd.range.count == 0 ? currentLine(ed) : cmd.range.last;
				adjusted.range.last = adjusted.range.first + 1;
				adjusted.range.count = 2;
			}

			ExRange range;
			std::size_t pos = 0;
			if (!applyCount(ed, cmd.args, pos, adjusted.range) || !lineRange(ed, adjusted, range)) {
				return false;
			}

			if (range.first == range.last) return true;

			joinRows(ed, range.first - 1, range.last - range.first + 1, !cmd.bang);
			return true;
		}

		static bool destinationLine(Editor& ed, const std::string& args, std::size_t& destination) {
			std::size_t pos = 0;
			bool have = false;
			if (!parseExAddress(ed, args, pos, destination, have)) return false;
			if (have) return true;

			setError(ed, "Invalid address");
			return false;
		}

		static std::vector<std::string> rangeLines(const Editor& ed, const ExRange& range) {
			const auto begin = ed.lines.begin() + static_cast<std::ptrdiff_t>(range.first - 1);
			const std::size_t count = range.last - range.first + 1;
			return std::vector<std::string>(begin, begin + static_cast<std::ptrdiff_t>(count));
		}

		static bool exMove(Editor& ed, const ExCommand& cmd) {
			ExRange range;
			std::size_t destination = 0;
			if (!lineRange(ed, cmd, range) || !destinationLine(ed, cmd.args, destination)) {
				return false;
			}

			if (destination >= range.first && destination < range.last) {
				setError(ed, "Move lines into themselves");
				return false;
			}

			const std::vector<std::string> moved = rangeLines(ed, range);
			const std::size_t count = moved.size();
			const std::size_t target = destination >= range.last
				? destination - count
				: destination;
			deleteLineRange(ed, range.first - 1, count);
			insertLineTexts(ed, target, moved);
			moveToFirstNonBlank(ed, target + count - 1);
			return true;
		}

		static bool exCopy(Editor& ed, const ExCommand& cmd) {
			ExRange range;
			std::size_t destination = 0;
			if (!lineRange(ed, cmd, range) || !destinationLine(ed, cmd.args, destination)) {
				return false;
			}

			const std::vector<std::string> copied = rangeLines(ed, range);
			insertLineTexts(ed, destination, copied);
			moveToFirstNonBlank(ed, destination + copied.size() - 1);
			return true;
		}

		static bool exShift(Editor& ed, const ExCommand& cmd) {
			ExRange range;
			std::size_t pos = 0;
			if (!lineRange(ed, cmd, range) || !applyCount(ed, cmd.args, pos, range)) return false;

			shiftRows(ed, range.first - 1, range.last - 1, cmd.name[0] == '>', cmd.name.size());
			return true;
		}

		static bool exPut(Editor& ed, const ExCommand& cmd) {
			const std::size_t line = cmd.range.count == 0 ? currentLine(ed) : cmd.range.last;
			std::size_t pos = 0;
			const char name = registerArgument(cmd.args, pos);
			Register value;
			if (!fetchRegister(ed, name, value)) {
				setError(ed, "Nothing in register");
				return false;
			}

			const std::size_t at = cmd.bang ? (line > 0 ? line - 1 : 0) : line;
			std::vector<std::string> lines = registerLines(value.text);
			if (lines.empty()) lines.push_back(std::string());
			insertLineTexts(ed, at, lines);
			moveToFirstNonBlank(ed, at + lines.size() - 1);
			return true;
		}

		static bool exMark(Editor& ed, const ExCommand& cmd) {
			const std::size_t line = cmd.range.count == 0 ? currentLine(ed) : cmd.range.last;
			const std::string name = cmd.name == "k" || cmd.name.size() > 2 || cmd.name == "ma"
				? cmd.args
				: cmd.name.substr(1);
			const bool valid = name.size() == 1 && line > 0;
			if (!valid || !setMark(ed, static_cast<Key>(name[0]), { line - 1, 0 })) {
				setError(ed, "Invalid mark name");
				return false;
			}

			return true;
		}

		static bool exPrint(Editor& ed, const ExCommand& cmd) {
			ExRange range;
			if (!lineRange(ed, cmd, range)) return false;

			const bool numbered = cmd.name[0] == 'n' || cmd.name[0] == '#';
			for (std::size_t line = range.first; line <= range.last; ++line) {
				const std::string number = numbered ? std::to_string(line) + " " : std::string();
				appendMessageLine(ed, number + ed.lines[line - 1]);
			}

			moveToFirstNonBlank(ed, range.last - 1);
			return true;
		}

		static bool exLineNumber(Editor& ed, const ExCommand& cmd) {
			const std::size_t line = cmd.range.count == 0 ? ed.lines.size() : cmd.range.last;
			setMessage(ed, std::to_string(line));
			return true;
		}

		static bool exUndo(Editor& ed, const ExCommand& cmd) {
			static_cast<void>(cmd);
			return undoChange(ed);
		}

		static bool exRedo(Editor& ed, const ExCommand& cmd) {
			static_cast<void>(cmd);
			return redoChange(ed);
		}

		static bool exVersion(Editor& ed, const ExCommand& cmd) {
			static_cast<void>(cmd);
			setMessage(ed, "wbsh vi (busybox-style vi with ex commands)");
			return true;
		}

		static bool exSubstitute(Editor& ed, const ExCommand& cmd) {
			return substituteCommand(ed, cmd.range, cmd.args);
		}

		static bool exGlobal(Editor& ed, const ExCommand& cmd) {
			return globalCommand(ed, cmd.range, cmd.args, cmd.bang || cmd.name[0] == 'v');
		}

		static bool exGoto(Editor& ed, const ExCommand& cmd) {
			if (cmd.range.count == 0) return true;

			const std::size_t line = cmd.range.last == 0 ? 1 : cmd.range.last;
			rememberContext(ed);
			moveToFirstNonBlank(ed, line - 1);
			return true;
		}

		static const ExEntry kExCommands[] = {
			{ "args", 2, exArgs },
			{ "copy", 2, exCopy },
			{ "delete", 1, exDelete },
			{ "edit", 1, exEdit },
			{ "file", 1, exFile },
			{ "global", 1, exGlobal },
			{ "join", 1, exJoin },
			{ "k", 1, exMark },
			{ "mark", 2, exMark },
			{ "move", 1, exMove },
			{ "next", 1, exNext },
			{ "Next", 1, exPrevious },
			{ "number", 2, exPrint },
			{ "print", 1, exPrint },
			{ "put", 2, exPut },
			{ "previous", 4, exPrevious },
			{ "quit", 1, exQuit },
			{ "qall", 2, exQuitAll },
			{ "read", 1, exRead },
			{ "redo", 3, exRedo },
			{ "rewind", 3, exRewind },
			{ "substitute", 1, exSubstitute },
			{ "set", 2, exSet },
			{ "t", 1, exCopy },
			{ "undo", 1, exUndo },
			{ "vglobal", 1, exGlobal },
			{ "version", 2, exVersion },
			{ "write", 1, exWrite },
			{ "wq", 2, exWriteQuit },
			{ "wnext", 2, exWriteNext },
			{ "xit", 1, exExit },
			{ "exit", 3, exExit },
			{ "yank", 1, exYank },
		};

		static ExHandler symbolHandler(const std::string& name) {
			switch (name[0]) {
				case '!':
					return exBang;
				case '<':
				case '>':
					return exShift;
				case '&':
				case '~':
					return exSubstitute;
				case '=':
					return exLineNumber;
				case '#':
					return exPrint;
				default:
					return nullptr;
			}
		}

		static ExHandler findHandler(const std::string& name) {
			if (name.empty()) return exGoto;
			if (std::isalpha(static_cast<unsigned char>(name[0])) == 0) return symbolHandler(name);

			for (const ExEntry& entry : kExCommands) {
				const std::string full = entry.name;
				const bool matches = name.size() >= entry.minimum && name.size() <= full.size()
					&& full.compare(0, name.size(), name) == 0;
				if (matches) return entry.handler;
			}

			const bool mark_shorthand = name.size() == 2 && name[0] == 'k';
			return mark_shorthand ? exMark : nullptr;
		}

		static std::string commandName(const std::string& text, std::size_t& pos) {
			const std::size_t start = pos;
			if (pos >= text.size()) return std::string();
			if (std::isalpha(static_cast<unsigned char>(text[pos])) == 0) {
				const char symbol = text[pos++];
				const bool repeats = symbol == '<' || symbol == '>' || symbol == '&';
				while (repeats && pos < text.size() && text[pos] == symbol) ++pos;

				return text.substr(start, pos - start);
			}

			while (pos < text.size() && std::isalpha(static_cast<unsigned char>(text[pos])) != 0) {
				++pos;
			}

			return text.substr(start, pos - start);
		}

		static std::string trimmed(const std::string& text) {
			std::size_t start = 0;
			std::size_t end = text.size();
			while (start < end && (text[start] == ' ' || text[start] == '\t')) ++start;
			while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
			return text.substr(start, end - start);
		}

		// `:s` and `:g` take their delimiter right after the name, so a
		// name only runs over letters when the command is not one of them.
		static void splitSubstituteName(ExCommand& cmd, std::size_t& pos) {
			const bool substitute = cmd.name.size() > 1 && cmd.name[0] == 's'
				&& findHandler(cmd.name) == nullptr;
			if (!substitute) return;

			pos -= cmd.name.size() - 1;
			cmd.name = "s";
		}

		static bool parseExCommand(Editor& ed, const std::string& text, ExCommand& cmd) {
			std::size_t pos = 0;
			while (pos < text.size() && text[pos] == ':') ++pos;
			skipBlanks(text, pos);
			if (!parseRange(ed, text, pos, cmd.range)) return false;

			skipBlanks(text, pos);
			cmd.name = commandName(text, pos);
			splitSubstituteName(cmd, pos);
			const bool takes_bang = !cmd.name.empty() && cmd.name[0] != '!';
			if (takes_bang && pos < text.size() && text[pos] == '!') {
				cmd.bang = true;
				++pos;
			}

			cmd.args = cmd.name == "!" ? text.substr(pos) : trimmed(text.substr(pos));
			return true;
		}

		bool executeExLine(Editor& ed, const std::string& text) {
			ExCommand cmd;
			if (!parseExCommand(ed, text, cmd)) return false;

			const ExHandler handler = findHandler(cmd.name);
			if (handler == nullptr) {
				setError(ed, "Not an editor command: " + trimmed(text));
				return false;
			}

			return handler(ed, cmd);
		}

		void runExCommand(Editor& ed, const std::string& text) {
			executeExLine(ed, text);
			clampCursor(ed);
			if (!ed.term.headless && ed.message.find('\n') != std::string::npos) {
				showLongMessage(ed);
			}
		}

	}  // namespace vi_detail

}  // namespace wbsh

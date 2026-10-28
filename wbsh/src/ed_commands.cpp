/**
 * @file ed_commands.cpp
 * @brief ed's command dispatch and every command but s and the globals.
 */

#include "ed_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace wbsh {

	namespace ed_detail {

		static const char* const kUndoable = "acdijmrstgvGV";
		static const std::size_t kDefaultWindow = 22;

		static bool noAddress(EdState& st, const Range& range) {
			if (range.count > 0) return fail(st, "Unexpected address");
			return true;
		}

		static void defaultTo(Range& range, std::size_t first, std::size_t second) {
			if (range.count > 0) return;

			range.first = first;
			range.second = second;
		}

		static bool validLines(EdState& st, const Range& range) {
			const bool valid = range.first >= 1 && range.first <= range.second
				&& range.second <= lastLine(st);
			if (!valid) return fail(st, "Invalid address");
			return true;
		}

		static bool validAddress(EdState& st, std::size_t address, bool allow_zero) {
			if ((address == 0 && !allow_zero) || address > lastLine(st)) {
				return fail(st, "Invalid address");
			}

			return true;
		}

		bool parsePrintSuffix(CommandText& cmd, int& flags) {
			for (;;) {
				const char c = cmd.peek();
				if (c == 'p') flags |= kPrintPlain;
				else if (c == 'l') flags |= kPrintList;
				else if (c == 'n') flags |= kPrintNumber;
				else return true;
				cmd.take();
			}
		}

		bool finishCommand(EdState& st, CommandText& cmd) {
			skipBlanks(cmd);
			if (!cmd.atEnd()) return fail(st, "Invalid command suffix");
			return true;
		}

		static bool endCommand(EdState& st, CommandText& cmd, int& print) {
			return parsePrintSuffix(cmd, print) && finishCommand(st, cmd);
		}

		static bool parseFilename(EdState& st, CommandText& cmd, std::string& name) {
			const char c = cmd.peek();
			if (!cmd.atEnd() && c != ' ' && c != '\t' && c != '!') {
				return fail(st, "Invalid command suffix");
			}

			skipBlanks(cmd);
			name = cmd.rest();
			cmd.pos = cmd.text.size();
			return true;
		}

		static std::vector<std::string> readTextLines(EdState& st) {
			std::vector<std::string> texts;
			std::string line;
			while (readInputLine(st, line) && line != ".") texts.push_back(line);
			return texts;
		}

		static bool warnModified(EdState& st) {
			st.warned = true;
			return fail(st, "Warning: buffer modified");
		}

		static bool appendCommand(EdState& st, CommandText& cmd, Range& range, int& print) {
			defaultTo(range, st.current, st.current);
			if (!validAddress(st, range.second, true) || !endCommand(st, cmd, print)) return false;

			const std::vector<std::string> texts = readTextLines(st);
			if (!texts.empty()) insertLines(st, range.second, texts);
			st.current = range.second + texts.size();
			return true;
		}

		static bool insertCommand(EdState& st, CommandText& cmd, Range& range, int& print) {
			defaultTo(range, st.current, st.current);
			if (!validAddress(st, range.second, true) || !endCommand(st, cmd, print)) return false;

			const std::size_t after = range.second == 0 ? 0 : range.second - 1;
			const std::vector<std::string> texts = readTextLines(st);
			if (!texts.empty()) insertLines(st, after, texts);
			st.current = after + texts.size();
			if (texts.empty()) {
				st.current = (range.second == 0 && lastLine(st) > 0) ? 1 : range.second;
			}

			return true;
		}

		static std::size_t lineAfterDeletion(const EdState& st, std::size_t first) {
			return first <= lastLine(st) ? first : lastLine(st);
		}

		static bool changeCommand(EdState& st, CommandText& cmd, Range& range, int& print) {
			defaultTo(range, st.current, st.current);
			if (!validLines(st, range) || !endCommand(st, cmd, print)) return false;

			const std::vector<std::string> texts = readTextLines(st);
			deleteLines(st, range.first, range.second);
			if (!texts.empty()) insertLines(st, range.first - 1, texts);
			st.current = texts.empty() ? lineAfterDeletion(st, range.first)
				: range.first - 1 + texts.size();
			return true;
		}

		static bool deleteCommand(EdState& st, CommandText& cmd, Range& range, int& print) {
			defaultTo(range, st.current, st.current);
			if (!validLines(st, range) || !endCommand(st, cmd, print)) return false;

			deleteLines(st, range.first, range.second);
			st.current = lineAfterDeletion(st, range.first);
			return true;
		}

		static bool joinCommand(EdState& st, CommandText& cmd, Range& range, int& print) {
			defaultTo(range, st.current, st.current + 1);
			if (!validLines(st, range) || !endCommand(st, cmd, print)) return false;

			if (range.first < range.second) {
				std::string joined;
				for (std::size_t line = range.first; line <= range.second; ++line) {
					joined += st.lines[line - 1].text;
				}

				replaceLine(st, range.first, joined);
				deleteLines(st, range.first + 1, range.second);
			}

			st.current = range.first;
			return true;
		}

		static bool markCommand(EdState& st, CommandText& cmd, Range& range, int& print) {
			defaultTo(range, st.current, st.current);
			if (!validAddress(st, range.second, false)) return false;

			const char mark = cmd.take();
			if (mark < 'a' || mark > 'z') return fail(st, "Invalid mark character");
			if (!endCommand(st, cmd, print)) return false;

			setMark(st, mark, range.second);
			return true;
		}

		static bool printCommand(EdState& st, CommandText& cmd, Range& range, int base) {
			defaultTo(range, st.current, st.current);
			int flags = base;
			if (!validLines(st, range) || !endCommand(st, cmd, flags)) return false;

			for (std::size_t line = range.first; line <= range.second; ++line) {
				printLine(st, line, flags);
			}

			st.current = range.second;
			return true;
		}

		static bool parseDestination(EdState& st, CommandText& cmd, std::size_t& destination) {
			bool have = false;
			if (!parseAddress(st, cmd, destination, have)) return false;
			if (!have) destination = st.current;
			return true;
		}

		static bool moveCommand(EdState& st, CommandText& cmd, Range& range, int& print) {
			defaultTo(range, st.current, st.current);
			std::size_t destination = 0;
			if (!validLines(st, range) || !parseDestination(st, cmd, destination)) return false;
			if (destination >= range.first && destination < range.second) {
				return fail(st, "Invalid destination");
			}

			if (!endCommand(st, cmd, print)) return false;

			const std::size_t count = range.second - range.first + 1;
			const std::size_t target = destination >= range.second
				? destination - count
				: destination;
			moveLines(st, range.first, range.second, destination);
			st.current = target + count;
			return true;
		}

		static bool transferCommand(EdState& st, CommandText& cmd, Range& range, int& print) {
			defaultTo(range, st.current, st.current);
			std::size_t destination = 0;
			if (!validLines(st, range) || !parseDestination(st, cmd, destination)) return false;
			if (!endCommand(st, cmd, print)) return false;

			std::vector<std::string> texts;
			for (std::size_t line = range.first; line <= range.second; ++line) {
				texts.push_back(st.lines[line - 1].text);
			}

			insertLines(st, destination, texts);
			st.current = destination + texts.size();
			return true;
		}

		static bool resolveFilename(EdState& st, const std::string& given, std::string& name) {
			name = given.empty() ? st.filename : given;
			if (name.empty()) return fail(st, "No current filename");
			return true;
		}

		static void rememberFilename(EdState& st, const std::string& name) {
			if (st.filename.empty() && !name.empty() && name[0] != '!') st.filename = name;
		}

		static bool readCommand(EdState& st, CommandText& cmd, Range& range) {
			defaultTo(range, lastLine(st), lastLine(st));
			std::string given;
			std::string name;
			if (!validAddress(st, range.second, true) || !parseFilename(st, cmd, given)) {
				return false;
			}

			if (!resolveFilename(st, given, name)) return false;

			std::string text;
			if (!readSource(st, name, text)) return false;

			bool crlf = false;
			bool complete = true;
			const std::vector<std::string> texts = splitText(text, crlf, complete);
			if (!texts.empty()) insertLines(st, range.second, texts);
			rememberFilename(st, name);
			printByteCount(st, text.size());
			st.current = range.second + texts.size();
			return true;
		}

		static bool writeRange(EdState& st, Range& range) {
			if (range.count == 0) {
				range.first = 1;
				range.second = lastLine(st);
				return true;
			}

			return validLines(st, range);
		}

		static bool writeCommand(EdState& st, CommandText& cmd, Range& range, bool append) {
			const bool quit_after = !append && cmd.peek() == 'q';
			if (quit_after) cmd.take();

			std::string given;
			std::string name;
			if (!writeRange(st, range) || !parseFilename(st, cmd, given)) return false;
			if (!resolveFilename(st, given, name)) return false;

			const bool whole = range.first <= 1 && range.second == lastLine(st);
			if (name[0] == '!') {
				std::string command;
				if (!expandShellCommand(st, name.substr(1), command)) return false;
				if (!writeToCommand(st, command, range)) return false;
			} else {
				if (!writeLines(st, name, range, append)) return false;
				rememberFilename(st, name);
				if (whole) st.modified = false;
			}

			if (quit_after) st.quit = true;
			return true;
		}

		bool editFile(EdState& st, const std::string& name) {
			std::string text;
			if (!readSource(st, name, text)) return false;

			bool complete = true;
			const std::vector<std::string> texts = splitText(text, st.crlf, complete);
			st.lines.clear();
			insertLines(st, 0, texts);
			st.marks.fill(0);
			st.has_undo = false;
			st.modified = false;
			st.current = lastLine(st);
			if (name[0] != '!') st.filename = name;
			printByteCount(st, text.size());
			return true;
		}

		static bool editCommand(EdState& st, CommandText& cmd, const Range& range, bool forced) {
			std::string given;
			std::string name;
			if (!noAddress(st, range) || !parseFilename(st, cmd, given)) return false;
			if (!forced && st.modified && !st.warning_active) return warnModified(st);
			if (!resolveFilename(st, given, name)) return false;

			return editFile(st, name);
		}

		static bool filenameCommand(EdState& st, CommandText& cmd, const Range& range) {
			std::string given;
			if (!noAddress(st, range) || !parseFilename(st, cmd, given)) return false;
			if (!given.empty()) st.filename = given;
			if (st.filename.empty()) return fail(st, "No current filename");

			std::printf("%s\n", st.filename.c_str());
			return true;
		}

		static bool helpCommand(EdState& st, CommandText& cmd, const Range& range, bool toggle) {
			if (!noAddress(st, range) || !finishCommand(st, cmd)) return false;

			if (toggle) st.verbose = !st.verbose;
			const bool show = !toggle || st.verbose;
			if (show && !st.error.empty()) std::printf("%s\n", st.error.c_str());
			return true;
		}

		static bool promptCommand(EdState& st, CommandText& cmd, const Range& range) {
			if (!noAddress(st, range) || !finishCommand(st, cmd)) return false;

			st.prompt_on = !st.prompt_on;
			return true;
		}

		static bool quitCommand(EdState& st, CommandText& cmd, const Range& range, bool forced) {
			if (!noAddress(st, range) || !finishCommand(st, cmd)) return false;
			if (!forced && st.modified && !st.warning_active) return warnModified(st);

			st.quit = true;
			return true;
		}

		static bool undoCommand(EdState& st, CommandText& cmd, const Range& range, int& print) {
			if (!noAddress(st, range) || !endCommand(st, cmd, print)) return false;
			if (!st.has_undo) return fail(st, "Nothing to undo");

			const Snapshot redo = takeSnapshot(st);
			restoreSnapshot(st, st.undo);
			st.undo = redo;
			return true;
		}

		static std::size_t takeCount(CommandText& cmd) {
			std::size_t count = 0;
			while (cmd.peek() >= '0' && cmd.peek() <= '9') {
				count = count * 10 + static_cast<std::size_t>(cmd.take() - '0');
			}

			return count;
		}

		static bool scrollCommand(EdState& st, CommandText& cmd, Range& range) {
			static std::size_t window = kDefaultWindow;
			defaultTo(range, st.current + 1, st.current + 1);
			if (!validAddress(st, range.second, false)) return false;

			const std::size_t count = takeCount(cmd);
			if (count > 0) window = count;

			int flags = kPrintPlain;
			if (!endCommand(st, cmd, flags)) return false;

			const std::size_t end = (std::min)(lastLine(st), range.second + window - 1);
			for (std::size_t line = range.second; line <= end; ++line) printLine(st, line, flags);
			st.current = end;
			return true;
		}

		static bool numberCommand(EdState& st, CommandText& cmd, Range& range, int& print) {
			defaultTo(range, lastLine(st), lastLine(st));
			if (!validAddress(st, range.second, true) || !endCommand(st, cmd, print)) return false;

			std::printf("%zu\n", range.second);
			return true;
		}

		static bool shellCommand(EdState& st, CommandText& cmd, const Range& range) {
			if (!noAddress(st, range)) return false;

			std::string command;
			if (!expandShellCommand(st, cmd.rest(), command)) return false;

			cmd.pos = cmd.text.size();
			runShellCommand(st, command);
			if (!st.silent) std::printf("!\n");
			return true;
		}

		static bool nullCommand(EdState& st, Range& range) {
			const std::size_t line = range.count == 0 ? st.current + 1 : range.second;
			if (!validAddress(st, line, false)) return false;

			printLine(st, line, kPrintPlain);
			st.current = line;
			return true;
		}

		static bool dispatchEditing(EdState& st, CommandText& cmd, Range& range, char name,
		                            int& print) {
			switch (name) {
				case 'a':
					return appendCommand(st, cmd, range, print);
				case 'i':
					return insertCommand(st, cmd, range, print);
				case 'c':
					return changeCommand(st, cmd, range, print);
				case 'd':
					return deleteCommand(st, cmd, range, print);
				case 'j':
					return joinCommand(st, cmd, range, print);
				case 'k':
					return markCommand(st, cmd, range, print);
				case 'm':
					return moveCommand(st, cmd, range, print);
				case 't':
					return transferCommand(st, cmd, range, print);
				case 'u':
					return undoCommand(st, cmd, range, print);
				case 's':
					return substituteCommand(st, cmd, range);
				case 'g':
				case 'v':
				case 'G':
				case 'V':
					return globalCommand(st, cmd, range, name);
				default:
					return fail(st, "Unknown command");
			}
		}

		static bool dispatchPrinting(EdState& st, CommandText& cmd, Range& range, char name,
		                             int& print) {
			switch (name) {
				case 'p':
					return printCommand(st, cmd, range, kPrintPlain);
				case 'l':
					return printCommand(st, cmd, range, kPrintList);
				case 'n':
					return printCommand(st, cmd, range, kPrintNumber);
				case 'z':
					return scrollCommand(st, cmd, range);
				case '=':
					return numberCommand(st, cmd, range, print);
				case '\0':
					return nullCommand(st, range);
				default:
					return dispatchEditing(st, cmd, range, name, print);
			}
		}

		static bool dispatchCommand(EdState& st, CommandText& cmd, Range& range, char name,
		                            int& print) {
			switch (name) {
				case 'r':
					return readCommand(st, cmd, range);
				case 'w':
					return writeCommand(st, cmd, range, false);
				case 'W':
					return writeCommand(st, cmd, range, true);
				case 'e':
					return editCommand(st, cmd, range, false);
				case 'E':
					return editCommand(st, cmd, range, true);
				case 'f':
					return filenameCommand(st, cmd, range);
				case 'h':
					return helpCommand(st, cmd, range, false);
				case 'H':
					return helpCommand(st, cmd, range, true);
				case 'P':
					return promptCommand(st, cmd, range);
				case 'q':
					return quitCommand(st, cmd, range, false);
				case 'Q':
					return quitCommand(st, cmd, range, true);
				case '!':
					return shellCommand(st, cmd, range);
				case '#':
					cmd.pos = cmd.text.size();
					return true;
				default:
					return dispatchPrinting(st, cmd, range, name, print);
			}
		}

		static bool isUndoable(const EdState& st, char name) {
			return !st.in_global && name != '\0' && std::strchr(kUndoable, name) != nullptr;
		}

		bool executeCommand(EdState& st, CommandText& cmd) {
			Range range;
			if (!parseRange(st, cmd, range)) return false;

			skipBlanks(cmd);
			const char name = cmd.take();
			const bool undoable = isUndoable(st, name);
			Snapshot before;
			if (undoable) {
				before = takeSnapshot(st);
				st.changed = false;
			}

			int print = 0;
			const bool ok = dispatchCommand(st, cmd, range, name, print);
			if (undoable && st.changed) {
				st.undo = before;
				st.has_undo = true;
			}

			if (ok && print != 0 && st.current > 0) printLine(st, st.current, print);
			return ok;
		}

	}  // namespace ed_detail

}  // namespace wbsh

/**
 * @file vi_buffer.cpp
 * @brief vi's text changes: the one line-splicing primitive with undo
 *        recording and mark tracking, region helpers, undo / redo,
 *        registers and marks.
 */

#include "vi_internal.h"

#include <string>
#include <utility>
#include <vector>

namespace wbsh {

	namespace vi_detail {

		static const std::size_t kNumberedRegisters = 9;

		// A replacement of exactly one line inside what the previous change
		// inserted folds into that change, so typing does not grow a record
		// per key.
		static bool mergeIntoLast(UndoGroup& group, std::size_t first, std::size_t count,
		                          const std::vector<std::string>& inserted) {
			if (group.changes.empty() || count != 1 || inserted.size() != 1) return false;

			UndoChange& last = group.changes.back();
			const bool inside = first >= last.first && first < last.first + last.inserted.size();
			if (!inside) return false;

			last.inserted[first - last.first] = inserted[0];
			return true;
		}

		static void recordChange(Editor& ed, std::size_t first, std::size_t count,
		                         const std::vector<std::string>& inserted) {
			if (mergeIntoLast(ed.group, first, count, inserted)) return;

			UndoChange change;
			change.first = first;
			const auto begin = ed.lines.begin() + static_cast<std::ptrdiff_t>(first);
			change.removed.assign(begin, begin + static_cast<std::ptrdiff_t>(count));
			change.inserted = inserted;
			ed.group.changes.push_back(std::move(change));
		}

		static void adjustRow(std::size_t& row, bool& valid, std::size_t first, std::size_t count,
		                      std::size_t inserted) {
			if (!valid || row < first) return;
			if (row >= first + count) {
				row = row - count + inserted;
				return;
			}

			if (row - first >= inserted) valid = false;
		}

		static void adjustMarks(Editor& ed, std::size_t first, std::size_t count,
		                        std::size_t inserted) {
			for (std::size_t k = 0; k < ed.marks.size(); ++k) {
				adjustRow(ed.marks[k].row, ed.mark_set[k], first, count, inserted);
			}

			adjustRow(ed.context.row, ed.context_set, first, count, inserted);
		}

		static void adjustGlobalFlags(Editor& ed, std::size_t first, std::size_t count,
		                              std::size_t inserted) {
			if (ed.global_flags.empty()) return;

			const std::size_t kept = count < inserted ? count : inserted;
			const auto at = ed.global_flags.begin() + static_cast<std::ptrdiff_t>(first + kept);
			ed.global_flags.erase(at, at + static_cast<std::ptrdiff_t>(count - kept));

			const auto insert_at = ed.global_flags.begin()
				+ static_cast<std::ptrdiff_t>(first + kept);
			ed.global_flags.insert(insert_at, inserted - kept, 0);
		}

		static void spliceLines(Editor& ed, std::size_t first, std::size_t count,
		                        const std::vector<std::string>& inserted) {
			const auto begin = ed.lines.begin() + static_cast<std::ptrdiff_t>(first);
			ed.lines.erase(begin, begin + static_cast<std::ptrdiff_t>(count));

			const auto at = ed.lines.begin() + static_cast<std::ptrdiff_t>(first);
			ed.lines.insert(at, inserted.begin(), inserted.end());
		}

		// Removing every line leaves the one empty line an empty buffer has.
		void replaceLines(Editor& ed, std::size_t first, std::size_t count,
		                  const std::vector<std::string>& inserted) {
			const bool empties = count == ed.lines.size() && inserted.empty();
			const std::vector<std::string> lines = empties
				? std::vector<std::string>{ std::string() }
				: inserted;
			if (!ed.replaying_undo) recordChange(ed, first, count, lines);

			adjustMarks(ed, first, count, lines.size());
			adjustGlobalFlags(ed, first, count, lines.size());
			spliceLines(ed, first, count, lines);
			ed.no_lines = empties;
			ed.text_changed = true;
		}

		void setLine(Editor& ed, std::size_t row, const std::string& text) {
			replaceLines(ed, row, 1, { text });
		}

		void insertLineTexts(Editor& ed, std::size_t at, const std::vector<std::string>& texts) {
			if (!texts.empty()) replaceLines(ed, at, 0, texts);
		}

		void deleteLineRange(Editor& ed, std::size_t first, std::size_t count) {
			replaceLines(ed, first, count, {});
		}

		void replaceBuffer(Editor& ed, std::vector<std::string> lines) {
			ed.no_lines = lines.empty();
			if (lines.empty()) lines.push_back(std::string());

			ed.lines = std::move(lines);
			ed.mark_set.fill(false);
			ed.context_set = false;
			ed.global_flags.clear();
			clearHistory(ed);
		}

		std::string textBetween(const Editor& ed, Position start, Position end) {
			if (start.row == end.row) {
				return ed.lines[start.row].substr(start.col, end.col - start.col);
			}

			std::string out = ed.lines[start.row].substr(start.col) + "\n";
			for (std::size_t row = start.row + 1; row < end.row; ++row) out += ed.lines[row] + "\n";
			return out + ed.lines[end.row].substr(0, end.col);
		}

		void deleteBetween(Editor& ed, Position start, Position end) {
			const std::string head = ed.lines[start.row].substr(0, start.col);
			const std::string tail = ed.lines[end.row].substr(end.col);
			replaceLines(ed, start.row, end.row - start.row + 1, { head + tail });
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

		Position insertText(Editor& ed, Position at, const std::string& text) {
			const std::string& line = ed.lines[at.row];
			const std::string head = line.substr(0, at.col);
			const std::string tail = line.substr(at.col);
			std::vector<std::string> pieces = splitAtNewlines(text);
			const std::size_t last_length = pieces.back().size();

			pieces.front() = head + pieces.front();
			pieces.back() += tail;
			const std::size_t added = pieces.size() - 1;
			replaceLines(ed, at.row, 1, pieces);

			Position end;
			end.row = at.row + added;
			end.col = added == 0 ? at.col + text.size() : last_length;
			return end;
		}

		void beginUndoGroup(Editor& ed) {
			ed.group = UndoGroup();
			ed.group.cursor_before = ed.cursor;
			ed.group.previous_state = ed.state;
			ed.group.no_lines_before = ed.no_lines;
			ed.group_open = true;
		}

		void endUndoGroup(Editor& ed) {
			ed.group_open = false;
			if (ed.group.changes.empty()) return;

			ed.group.state = ++ed.next_state;
			ed.state = ed.group.state;
			ed.undo.push_back(std::move(ed.group));
			ed.redo.clear();
			ed.group = UndoGroup();
		}

		static void finishReplay(Editor& ed, const UndoGroup& group) {
			ed.cursor = group.cursor_before;
			clampCursor(ed);
			updateWantColumn(ed);
		}

		bool undoChange(Editor& ed) {
			if (ed.undo.empty()) {
				setMessage(ed, "Already at oldest change");
				return false;
			}

			UndoGroup group = std::move(ed.undo.back());
			ed.undo.pop_back();
			ed.replaying_undo = true;
			for (auto it = group.changes.rbegin(); it != group.changes.rend(); ++it) {
				replaceLines(ed, it->first, it->inserted.size(), it->removed);
			}

			ed.replaying_undo = false;
			ed.state = group.previous_state;
			ed.no_lines = group.no_lines_before;
			finishReplay(ed, group);
			ed.redo.push_back(std::move(group));
			return true;
		}

		bool redoChange(Editor& ed) {
			if (ed.redo.empty()) {
				setMessage(ed, "Already at newest change");
				return false;
			}

			UndoGroup group = std::move(ed.redo.back());
			ed.redo.pop_back();
			ed.replaying_undo = true;
			for (const UndoChange& change : group.changes) {
				replaceLines(ed, change.first, change.removed.size(), change.inserted);
			}

			ed.replaying_undo = false;
			ed.state = group.state;
			finishReplay(ed, group);
			ed.undo.push_back(std::move(group));
			return true;
		}

		void clearHistory(Editor& ed) {
			ed.undo.clear();
			ed.redo.clear();
			ed.state = ++ed.next_state;
			ed.saved_state = ed.state;
		}

		static Register& registerAt(Editor& ed, char name) {
			return ed.registers[static_cast<unsigned char>(name) & 0x7F];
		}

		static void appendRegister(Editor& ed, char name, const Register& value) {
			Register& target = registerAt(ed, static_cast<char>(name - 'A' + 'a'));
			const bool needs_break = !target.linewise && value.linewise && !target.text.empty();
			if (needs_break) target.text += "\n";
			target.text += value.text;
			target.linewise = target.linewise || value.linewise;
			registerAt(ed, '"') = target;
		}

		static void shiftNumberedRegisters(Editor& ed, const Register& value) {
			for (std::size_t k = kNumberedRegisters; k > 1; --k) {
				const char name = static_cast<char>('0' + k);
				registerAt(ed, name) = registerAt(ed, static_cast<char>(name - 1));
			}

			registerAt(ed, '1') = value;
		}

		void storeRegister(Editor& ed, char name, const Register& value, bool is_delete) {
			if (name == '_') return;
			if (name >= 'A' && name <= 'Z') {
				appendRegister(ed, name, value);
				return;
			}

			registerAt(ed, '"') = value;
			if (name != 0 && name != '"') {
				registerAt(ed, name) = value;
				return;
			}

			const bool big = value.linewise || value.text.find('\n') != std::string::npos;
			if (!is_delete) registerAt(ed, '0') = value;
			else if (big) shiftNumberedRegisters(ed, value);
			else registerAt(ed, '-') = value;
		}

		bool fetchRegister(const Editor& ed, char name, Register& value) {
			char key = name == 0 ? '"' : name;
			if (key >= 'A' && key <= 'Z') key = static_cast<char>(key - 'A' + 'a');

			value = ed.registers[static_cast<unsigned char>(key) & 0x7F];
			return !value.text.empty() || value.linewise;
		}

		bool setMark(Editor& ed, Key name, Position at) {
			if (name == '\'' || name == '`') {
				ed.context = at;
				ed.context_set = true;
				return true;
			}

			if (name < 'a' || name > 'z') return false;

			const std::size_t index = static_cast<std::size_t>(name - 'a');
			ed.marks[index] = at;
			ed.mark_set[index] = true;
			return true;
		}

		bool markPosition(const Editor& ed, Key name, Position& at) {
			if (name == '\'' || name == '`') {
				at = ed.context;
				return ed.context_set;
			}

			if (name < 'a' || name > 'z') return false;

			const std::size_t index = static_cast<std::size_t>(name - 'a');
			at = ed.marks[index];
			return ed.mark_set[index];
		}

		void rememberContext(Editor& ed) {
			ed.context = ed.cursor;
			ed.context_set = true;
		}

	}  // namespace vi_detail

}  // namespace wbsh

/**
 * @file lineedit_vi.cpp
 * @brief readline's vi editing mode: insert and command mode, counts,
 *        the d / c / y operators with their motions, and `.` to repeat.
 *
 * A line starts in insert mode, where keys go through the vi-insert
 * keymap; Esc goes to command mode and its keymap. A count typed before a
 * command repeats it or is its argument. An operator waits for the motion
 * after it and then works on the text between the cursor and where the
 * motion goes; pressed twice it works on the whole line. The keys of the
 * last change -- from the command that started it to the Esc that ended
 * its insert -- are kept, and `.` types them again.
 */

#include "lineedit.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif /* _WIN32 */

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>
#include <vector>

#include "history.h"
#include "keymap.h"
#include "pathconv.h"
#include "textcells.h"

namespace wbsh {

	namespace vi_detail {

		struct NamedCommand {
			const char* name;
			void (LineEditor::*run)();
		};

		static bool isOneOf(const std::string& name, std::initializer_list<const char*> names) {
			for (const char* candidate : names) {
				if (name == candidate) return true;
			}

			return false;
		}

		// Commands a count simply repeats.
		static bool repeatsWithCount(const std::string& name) {
			return isOneOf(name, { "backward-char", "forward-char", "vi-next-word", "vi-prev-word",
				"vi-end-word", "vi-forward-bigword", "vi-backward-bigword", "vi-end-bigword",
				"previous-history", "next-history", "vi-search-again" });
		}

		static bool isMotion(const std::string& name) {
			return isOneOf(name, { "backward-char", "forward-char", "vi-next-word", "vi-prev-word",
				"vi-end-word", "vi-forward-bigword", "vi-backward-bigword", "vi-end-bigword",
				"beginning-of-line", "end-of-line", "vi-first-print", "vi-column", "vi-match" });
		}

		static bool endsOnLastCharacter(const std::string& name) {
			return isOneOf(name, { "vi-end-word", "vi-end-bigword", "vi-match" });
		}

		static bool startsOperator(const std::string& name) {
			return isOneOf(name, { "vi-delete-to", "vi-change-to", "vi-yank-to" });
		}

		static bool isBlank(char letter) {
			return letter == ' ' || letter == '\t';
		}

		// vi's words: runs of letters, digits and `_` (every byte of a
		// non-ASCII character counts as a letter), or runs of the other
		// characters that are not blank. A big word is any run of non-blanks.
		static int wordClass(char letter, bool big) {
			if (isBlank(letter)) return 0;
			if (big) return 1;

			const auto byte = static_cast<unsigned char>(letter);
			return byte >= 0x80 || std::isalnum(byte) != 0 || letter == '_' ? 1 : 2;
		}

		static std::size_t nextWordStart(const std::string& text, std::size_t pos, bool big) {
			if (pos >= text.size()) return text.size();

			const int word = wordClass(text[pos], big);
			while (pos < text.size() && word != 0 && wordClass(text[pos], big) == word) ++pos;

			while (pos < text.size() && isBlank(text[pos])) ++pos;
			return pos;
		}

		static std::size_t previousWordStart(const std::string& text, std::size_t pos, bool big) {
			while (pos > 0 && isBlank(text[pos - 1])) --pos;
			if (pos == 0) return 0;

			const int word = wordClass(text[pos - 1], big);
			while (pos > 0 && wordClass(text[pos - 1], big) == word) --pos;
			return pos;
		}

		// The first byte of the last character of the word at or after
		// the one following `pos`.
		static std::size_t wordEnd(const std::string& text, std::size_t pos, bool big) {
			if (pos < text.size()) pos = nextClusterEnd(text, pos);
			while (pos < text.size() && isBlank(text[pos])) ++pos;
			if (pos >= text.size()) return text.size();

			const int word = wordClass(text[pos], big);
			std::size_t end = pos;
			while (end < text.size() && wordClass(text[end], big) == word) ++end;
			return previousClusterStart(text, end);
		}

		// The first byte of the last character in the word `pos` is in.
		static std::size_t lastOfRun(const std::string& text, std::size_t pos, bool big) {
			const int word = wordClass(text[pos], big);
			std::size_t end = pos;
			while (end < text.size() && wordClass(text[end], big) == word) ++end;
			return previousClusterStart(text, end);
		}

		static char oppositeSearch(char kind) {
			switch (kind) {
			case 'f': return 'F';
			case 'F': return 'f';
			case 't': return 'T';
			case 'T': return 't';
			default:  return kind;
			}
		}

		static char bracketPartner(char letter, bool& forward) {
			forward = letter == '(' || letter == '[' || letter == '{';
			switch (letter) {
			case '(': return ')';
			case ')': return '(';
			case '[': return ']';
			case ']': return '[';
			case '{': return '}';
			case '}': return '{';
			default:  return 0;
			}
		}

#ifdef _WIN32
		static std::string otherCase(const std::string& text) {
			std::wstring wide = utf8ToWide(text);
			if (wide.empty()) return text;

			const std::wstring original = wide;
			::CharUpperBuffW(wide.data(), static_cast<DWORD>(wide.size()));
			if (wide == original) ::CharLowerBuffW(wide.data(), static_cast<DWORD>(wide.size()));
			return wideToUtf8(wide);
		}
#else
		static std::string otherCase(const std::string& text) {
			std::string out = text;
			for (char& letter : out) {
				const auto byte = static_cast<unsigned char>(letter);
				const int changed = std::isupper(byte) ? std::tolower(byte) : std::toupper(byte);
				letter = static_cast<char>(changed);
			}

			return out;
		}
#endif /* _WIN32 */

	}  // namespace vi_detail

	LineEditor::EditCommand LineEditor::viCommandNamed(const std::string& name) const {
		static const vi_detail::NamedCommand kCommands[] = {
			{ "emacs-editing-mode",  &LineEditor::emacsEditingMode },
			{ "vi-append-eol",       &LineEditor::viAppendEol },
			{ "vi-append-mode",      &LineEditor::viAppendMode },
			{ "vi-arg-digit",        &LineEditor::viArgDigit },
			{ "vi-backward-bigword", &LineEditor::viBackwardBigword },
			{ "vi-change-case",      &LineEditor::viChangeCase },
			{ "vi-change-char",      &LineEditor::viChangeChar },
			{ "vi-change-to",        &LineEditor::viChangeTo },
			{ "vi-char-search",      &LineEditor::viCharSearch },
			{ "vi-column",           &LineEditor::viColumn },
			{ "vi-delete",           &LineEditor::viDelete },
			{ "vi-delete-to",        &LineEditor::viDeleteTo },
			{ "vi-editing-mode",     &LineEditor::viEditingMode },
			{ "vi-end-bigword",      &LineEditor::viEndBigword },
			{ "vi-end-word",         &LineEditor::viEndWord },
			{ "vi-eof-maybe",        &LineEditor::viEofMaybe },
			{ "vi-fetch-history",    &LineEditor::viFetchHistory },
			{ "vi-first-print",      &LineEditor::viFirstPrint },
			{ "vi-forward-bigword",  &LineEditor::viForwardBigword },
			{ "vi-goto-mark",        &LineEditor::viGotoMark },
			{ "vi-insert-beg",       &LineEditor::viInsertBeg },
			{ "vi-insertion-mode",   &LineEditor::viInsertionMode },
			{ "vi-match",            &LineEditor::viMatch },
			{ "vi-movement-mode",    &LineEditor::viMovementMode },
			{ "vi-next-word",        &LineEditor::viNextWord },
			{ "vi-prev-word",        &LineEditor::viPrevWord },
			{ "vi-put",              &LineEditor::viPut },
			{ "vi-redo",             &LineEditor::viRedo },
			{ "vi-replace",          &LineEditor::viReplace },
			{ "vi-rubout",           &LineEditor::viRubout },
			{ "vi-search",           &LineEditor::viSearch },
			{ "vi-search-again",     &LineEditor::viSearchAgain },
			{ "vi-set-mark",         &LineEditor::viSetMark },
			{ "vi-subst",            &LineEditor::viSubst },
			{ "vi-yank-arg",         &LineEditor::viYankArg },
			{ "vi-yank-to",          &LineEditor::viYankTo },
		};

		for (const vi_detail::NamedCommand& command : kCommands) {
			if (name == command.name) return command.run;
		}

		return nullptr;
	}

	Keymap& LineEditor::activeKeymap() const {
		if (!viEditing()) return shellKeymap();

		return vi_command_ ? viCommandKeymap() : viInsertKeymap();
	}

	void LineEditor::resetViState() {
		vi_command_ = false;
		vi_count_ = 0;
		vi_operator_.clear();
		vi_operator_count_ = 0;
		vi_keys_.clear();
		vi_change_ = false;
		vi_marks_.clear();
	}

	std::size_t LineEditor::takeCount() {
		const std::size_t count = vi_count_;
		vi_count_ = 0;
		return count;
	}

	// A count, then the command: repeated if it simply repeats, or as
	// its argument; an operator waiting takes the command as its motion.
	void LineEditor::runEditCommand(const std::string& name, EditCommand command) {
		const bool vi = viEditing() && vi_command_;
		if (vi && !vi_operator_.empty()) {
			viRunWithOperator(name, command);
			return;
		}

		const std::size_t times = vi && vi_detail::repeatsWithCount(name)
			? (std::max)(std::size_t(1), takeCount()) : 1;
		const std::size_t history_pos = history_pos_;
		for (std::size_t i = 0; i < times && !done_; ++i) (this->*command)();

		if (!viKeepsCount(name)) vi_count_ = 0;
		if (vi && history_pos_ != history_pos) cursor_ = 0;
	}

	// A count stays for the key a command waits for (`3rx`, `2f-`).
	bool LineEditor::viKeepsCount(const std::string& name) const {
		return name == "vi-arg-digit" || vi_detail::startsOperator(name)
		    || pending_key_ != PendingKey::kNone;
	}

	// In command mode the cursor sits on a character, never past the last.
	void LineEditor::viSettleCursor() {
		if (!viEditing() || !vi_command_ || buffer_.empty()) return;
		if (cursor_ >= buffer_.size()) cursor_ = previousClusterStart(buffer_, buffer_.size());
	}

	void LineEditor::viRunWithOperator(const std::string& name, EditCommand command) {
		const bool own = (name == "vi-delete-to" && vi_operator_ == "d")
			|| (name == "vi-change-to" && vi_operator_ == "c")
			|| (name == "vi-yank-to" && vi_operator_ == "y");
		if (own) {
			viApplyOperator(0, buffer_.size(), false);
			return;
		}

		const bool waits = name == "vi-arg-digit" || name == "vi-char-search"
			|| name == "vi-goto-mark";
		if (waits) {
			(this->*command)();
			return;
		}

		if (!vi_detail::isMotion(name)) {
			viCancelOperator();
			ding();
			return;
		}

		viOperateOverMotion(name, command);
	}

	void LineEditor::viOperateOverMotion(const std::string& name, EditCommand command) {
		const std::size_t times = (std::max)(std::size_t(1), vi_operator_count_)
			* (std::max)(std::size_t(1), takeCount());
		const bool change_word = vi_operator_ == "c" && cursor_ < buffer_.size()
			&& !vi_detail::isBlank(buffer_[cursor_])
			&& (name == "vi-next-word" || name == "vi-forward-bigword");
		if (change_word) {
			viChangeWords(name == "vi-forward-bigword", times);
			return;
		}

		const std::size_t from = cursor_;
		for (std::size_t i = 0; i < times; ++i) (this->*command)();
		viApplyOperator(from, cursor_, vi_detail::endsOnLastCharacter(name));
	}

	// `cw` changes to the end of the word, not up to the next one, as vi
	// has always done.
	void LineEditor::viChangeWords(bool big, std::size_t times) {
		const std::size_t from = cursor_;
		std::size_t end = vi_detail::lastOfRun(buffer_, cursor_, big);
		for (std::size_t i = 1; i < times; ++i) end = vi_detail::wordEnd(buffer_, end, big);

		viApplyOperator(from, end, true);
	}

	void LineEditor::viStartOperator(const std::string& op) {
		vi_operator_ = op;
		vi_operator_count_ = takeCount();
		if (op != "y") vi_change_ = true;
	}

	void LineEditor::viCancelOperator() {
		vi_operator_.clear();
		vi_operator_count_ = 0;
		vi_count_ = 0;
		vi_change_ = false;
	}

	void LineEditor::viApplyOperator(std::size_t from, std::size_t to, bool inclusive) {
		std::size_t start = (std::min)(from, to);
		std::size_t end = (std::max)(from, to);
		if (inclusive && end < buffer_.size()) end = nextClusterEnd(buffer_, end);

		const std::string op = vi_operator_;
		vi_operator_.clear();
		vi_operator_count_ = 0;
		if (op == "y") {
			copyToKillRing(buffer_.substr(start, end - start));
			cursor_ = start;
			return;
		}

		killRange(start, end, false);
		cursor_ = start;
		if (op == "c") vi_command_ = false;
	}

	void LineEditor::viEnterInsert() {
		vi_command_ = false;
		vi_change_ = true;
	}

	void LineEditor::viMovementMode() {
		if (!viEditing()) return;

		vi_command_ = true;
		overwrite_ = false;
		if (cursor_ > 0) cursor_ = previousClusterStart(buffer_, cursor_);
	}

	void LineEditor::viInsertionMode() {
		viEnterInsert();
	}

	void LineEditor::viAppendMode() {
		if (cursor_ < buffer_.size()) cursor_ = nextClusterEnd(buffer_, cursor_);
		viEnterInsert();
	}

	void LineEditor::viInsertBeg() {
		cursor_ = 0;
		viEnterInsert();
	}

	void LineEditor::viAppendEol() {
		cursor_ = buffer_.size();
		viEnterInsert();
	}

	void LineEditor::viReplace() {
		overwrite_ = true;
		viEnterInsert();
	}

	std::size_t LineEditor::clustersAhead(std::size_t from, std::size_t count) const {
		std::size_t end = from;
		for (std::size_t i = 0; i < count && end < buffer_.size(); ++i) {
			end = nextClusterEnd(buffer_, end);
		}

		return end;
	}

	void LineEditor::viDelete() {
		if (buffer_.empty()) {
			ding();
			return;
		}

		const std::size_t count = (std::max)(std::size_t(1), takeCount());
		killRange(cursor_, clustersAhead(cursor_, count), false);
		vi_change_ = true;
	}

	void LineEditor::viRubout() {
		std::size_t start = cursor_;
		const std::size_t count = (std::max)(std::size_t(1), takeCount());
		for (std::size_t i = 0; i < count && start > 0; ++i) {
			start = previousClusterStart(buffer_, start);
		}

		if (start == cursor_) {
			ding();
			return;
		}

		killRange(start, cursor_, true);
		vi_change_ = true;
	}

	// `s` changes characters, `S` the whole line.
	void LineEditor::viSubst() {
		const std::size_t count = (std::max)(std::size_t(1), takeCount());
		if (current_keys_ == "S") {
			killRange(0, buffer_.size(), false);
		} else {
			killRange(cursor_, clustersAhead(cursor_, count), false);
		}

		viEnterInsert();
	}

	void LineEditor::viChangeChar() {
		pending_key_ = PendingKey::kViChangeChar;
	}

	void LineEditor::viReplaceCharacters(const std::string& with) {
		const std::size_t count = (std::max)(std::size_t(1), takeCount());
		const std::size_t end = clustersAhead(cursor_, count);
		std::size_t available = 0;
		for (std::size_t at = cursor_; at < end; at = nextClusterEnd(buffer_, at)) ++available;
		if (available < count || with.empty()) {
			ding();
			return;
		}

		std::string replacement;
		for (std::size_t i = 0; i < count; ++i) replacement += with;
		buffer_.replace(cursor_, end - cursor_, replacement);
		cursor_ += replacement.size() - with.size();
		vi_change_ = true;
	}

	void LineEditor::viChangeCase() {
		const std::size_t count = (std::max)(std::size_t(1), takeCount());
		for (std::size_t i = 0; i < count && cursor_ < buffer_.size(); ++i) {
			const std::size_t end = nextClusterEnd(buffer_, cursor_);
			const std::string letter = buffer_.substr(cursor_, end - cursor_);
			const std::string changed = vi_detail::otherCase(letter);
			buffer_.replace(cursor_, end - cursor_, changed);
			cursor_ += changed.size();
		}

		vi_change_ = true;
	}

	// `p` puts the last kill after the cursor, `P` before it; the cursor
	// ends on its last character.
	void LineEditor::viPut() {
		if (kill_ring_.empty()) {
			ding();
			return;
		}

		const std::size_t count = (std::max)(std::size_t(1), takeCount());
		std::string text;
		for (std::size_t i = 0; i < count; ++i) text += kill_ring_[kill_ring_top_];
		const bool after = current_keys_ == "p" && cursor_ < buffer_.size();
		if (after) cursor_ = nextClusterEnd(buffer_, cursor_);

		insertChars(text);
		cursor_ = previousClusterStart(buffer_, cursor_);
		vi_change_ = true;
	}

	// `D`, `C` and `Y` work to the end of the line at once.
	void LineEditor::viOperatorKey(const std::string& op, char to_end) {
		viStartOperator(op);
		if (current_keys_ != std::string(1, to_end)) return;

		viApplyOperator(cursor_, buffer_.size(), false);
	}

	void LineEditor::viDeleteTo() {
		viOperatorKey("d", 'D');
	}

	void LineEditor::viChangeTo() {
		viOperatorKey("c", 'C');
	}

	void LineEditor::viYankTo() {
		viOperatorKey("y", 'Y');
	}

	void LineEditor::viNextWord() {
		cursor_ = vi_detail::nextWordStart(buffer_, cursor_, false);
	}

	void LineEditor::viForwardBigword() {
		cursor_ = vi_detail::nextWordStart(buffer_, cursor_, true);
	}

	void LineEditor::viPrevWord() {
		cursor_ = vi_detail::previousWordStart(buffer_, cursor_, false);
	}

	void LineEditor::viBackwardBigword() {
		cursor_ = vi_detail::previousWordStart(buffer_, cursor_, true);
	}

	void LineEditor::viEndWord() {
		cursor_ = vi_detail::wordEnd(buffer_, cursor_, false);
	}

	void LineEditor::viEndBigword() {
		cursor_ = vi_detail::wordEnd(buffer_, cursor_, true);
	}

	void LineEditor::viFirstPrint() {
		cursor_ = 0;
		while (cursor_ < buffer_.size() && vi_detail::isBlank(buffer_[cursor_])) ++cursor_;
	}

	void LineEditor::viColumn() {
		const std::size_t column = (std::max)(std::size_t(1), takeCount());
		cursor_ = clustersAhead(0, column - 1);
	}

	void LineEditor::viMatch() {
		std::size_t at = cursor_;
		bool forward = true;
		while (at < buffer_.size() && vi_detail::bracketPartner(buffer_[at], forward) == 0) ++at;
		if (at >= buffer_.size()) {
			ding();
			return;
		}

		const char open = buffer_[at];
		const char close = vi_detail::bracketPartner(open, forward);
		int depth = 0;
		for (std::size_t i = at; i < buffer_.size(); forward ? ++i : --i) {
			if (buffer_[i] == open) ++depth;
			if (buffer_[i] == close && --depth == 0) {
				cursor_ = i;
				return;
			}

			if (!forward && i == 0) break;
		}

		ding();
	}

	// `0` is the start of the line unless it continues a count.
	void LineEditor::viArgDigit() {
		const int digit = current_keys_.empty() ? 0 : current_keys_[0] - '0';
		if (digit == 0 && vi_count_ == 0) {
			const std::size_t from = cursor_;
			cursor_ = 0;
			if (!vi_operator_.empty()) viApplyOperator(from, 0, false);
			return;
		}

		vi_count_ = vi_count_ * 10 + static_cast<std::size_t>(digit);
	}

	// f F t T wait for a character; ; and , search for the last one again,
	// the same way and the other way.
	void LineEditor::viCharSearch() {
		const char key = current_keys_.empty() ? 0 : current_keys_[0];
		if (key != ';' && key != ',') {
			vi_search_kind_ = key;
			pending_key_ = PendingKey::kViCharSearch;
			return;
		}

		if (vi_search_char_.empty()) {
			ding();
			return;
		}

		const char kind = key == ';' ? vi_search_kind_ : vi_detail::oppositeSearch(vi_search_kind_);
		viFindCharacter(kind, vi_search_char_);
	}

	void LineEditor::viFindCharacter(char kind, const std::string& target) {
		const std::size_t from = cursor_;
		const std::size_t count = (std::max)(std::size_t(1), takeCount())
			* (std::max)(std::size_t(1), vi_operator_.empty() ? 1 : vi_operator_count_);
		bool found = true;
		for (std::size_t i = 0; i < count && found; ++i) found = viStepToCharacter(kind, target);

		if (!found) {
			cursor_ = from;
			if (!vi_operator_.empty()) viCancelOperator();
			ding();
			return;
		}

		if (!vi_operator_.empty()) viApplyOperator(from, cursor_, kind == 'f' || kind == 't');
	}

	bool LineEditor::viStepToCharacter(char kind, const std::string& target) {
		const bool forward = kind == 'f' || kind == 't';
		const bool before = kind == 't' || kind == 'T';
		std::size_t from = cursor_;
		if (before && forward && from < buffer_.size()) from = nextClusterEnd(buffer_, from);
		if (before && !forward && from > 0) from = previousClusterStart(buffer_, from);

		std::size_t found = std::string::npos;
		if (forward && from < buffer_.size()) {
			found = buffer_.find(target, nextClusterEnd(buffer_, from));
		} else if (!forward && from > 0) {
			found = buffer_.rfind(target, previousClusterStart(buffer_, from));
		}

		if (found == std::string::npos) return false;

		cursor_ = found;
		if (before) cursor_ = forward ? previousClusterStart(buffer_, found)
		                              : nextClusterEnd(buffer_, found);
		return true;
	}

	void LineEditor::viSetMark() {
		pending_key_ = PendingKey::kViSetMark;
	}

	void LineEditor::viGotoMark() {
		pending_key_ = PendingKey::kViGotoMark;
	}

	// What a vi command that waits for a key does with it.
	bool LineEditor::takeViPendingKey(PendingKey pending, const std::string& keys) {
		const char letter = keys.size() == 1 ? keys[0] : 0;
		switch (pending) {
		case PendingKey::kViCharSearch:
			vi_search_char_ = keys;
			viFindCharacter(vi_search_kind_, keys);
			return true;
		case PendingKey::kViChangeChar:
			viReplaceCharacters(keys);
			return true;
		case PendingKey::kViSetMark:
			if (letter != 0) vi_marks_[letter] = cursor_;
			return true;
		case PendingKey::kViGotoMark:
			viJumpToMark(letter);
			return true;
		default:
			return false;
		}
	}

	void LineEditor::viJumpToMark(char letter) {
		const auto mark = vi_marks_.find(letter);
		if (mark == vi_marks_.end() || mark->second > buffer_.size()) {
			if (!vi_operator_.empty()) viCancelOperator();
			ding();
			return;
		}

		const std::size_t from = cursor_;
		cursor_ = mark->second;
		if (!vi_operator_.empty()) viApplyOperator(from, cursor_, false);
	}

	// With a count, that history entry; without one, the oldest.
	void LineEditor::viFetchHistory() {
		const auto& history = exec_.history();
		const std::size_t number = takeCount();
		if (history.empty() || number > history.size()) {
			ding();
			return;
		}

		showHistoryEntry(number == 0 ? history.size() : history.size() - number + 1);
	}

	// `/` searches older entries for the text typed after it, `?` newer
	// ones; an empty text searches for the last one again.
	void LineEditor::viSearch() {
		const std::string lead = current_keys_;
		std::string text;
		if (!readMinibuffer(lead, text)) return;
		if (!text.empty()) vi_search_text_ = text;

		vi_search_older_ = lead != "?";
		viSearchHistory(vi_search_older_);
	}

	void LineEditor::viSearchAgain() {
		viSearchHistory(current_keys_ == "N" ? !vi_search_older_ : vi_search_older_);
	}

	void LineEditor::viSearchHistory(bool older) {
		const auto& history = exec_.history();
		std::size_t back = history_pos_;
		while (!vi_search_text_.empty()) {
			if (older && back >= history.size()) break;
			if (!older && back <= 1) break;

			back = older ? back + 1 : back - 1;
			if (history[history.size() - back].find(vi_search_text_) == std::string::npos) continue;

			showHistoryEntry(back);
			return;
		}

		ding();
	}

	void LineEditor::viYankArg() {
		if (cursor_ < buffer_.size()) cursor_ = nextClusterEnd(buffer_, cursor_);
		yankLastArg();
		viEnterInsert();
	}

	void LineEditor::viEofMaybe() {
		if (!buffer_.empty()) {
			ding();
			return;
		}

		eof_ = true;
		done_ = true;
	}

	// Keys of a change are kept from its first command to the Esc that
	// ends its insert, for `.`.
	void LineEditor::viRecordKeys(const std::string& keys) {
		if (viEditing() && !vi_replaying_) vi_keys_.push_back(keys);
	}

	void LineEditor::viAfterKeys() {
		if (!viEditing() || vi_replaying_ || !vi_command_) return;

		const bool unfinished = !vi_operator_.empty() || vi_count_ != 0
			|| pending_key_ != PendingKey::kNone || !pending_keys_.empty();
		if (unfinished) return;

		if (vi_change_) vi_last_change_ = vi_keys_;
		vi_keys_.clear();
		vi_change_ = false;
	}

	void LineEditor::viRedo() {
		if (vi_last_change_.empty()) {
			ding();
			return;
		}

		const std::vector<std::string> keys = vi_last_change_;
		const std::size_t times = (std::max)(std::size_t(1), takeCount());
		vi_replaying_ = true;
		for (std::size_t i = 0; i < times; ++i) {
			for (const std::string& key : keys) dispatchKeys(key);
		}

		vi_replaying_ = false;
		vi_change_ = false;
		vi_command_ = true;
	}

	// In insert mode an Alt key arrives as Esc with the key; when that is
	// bound to nothing it is Esc to command mode, then the key there.
	bool LineEditor::viSplitsMetaKey(const std::string& keys) {
		if (!viEditing() || vi_command_ || !pending_keys_.empty()) return false;
		if (keys.size() < 2 || keys[0] != '\x1b') return false;

		KeyBinding binding;
		if (viInsertKeymap().lookup(keys, binding) != KeyLookup::kUnbound) return false;

		dispatchKeys("\x1b");
		dispatchKeys(keys.substr(1));
		return true;
	}

	// A key that came alone runs its own binding in vi, even where longer
	// keys start with it: Esc leaves insert mode at once.
	KeyLookup LineEditor::viSettlePrefix(KeyLookup found, const Keymap& keymap,
	                                     const std::string& keys, KeyBinding& binding) const {
		if (found != KeyLookup::kPrefix || !viEditing() || pending_keys_ != keys) return found;
		if (keymap.exactBinding(keys, binding)) return KeyLookup::kBound;

		return keys == "\x1b" ? KeyLookup::kUnbound : found;
	}

	void LineEditor::emacsEditingMode() {
		shellKeymap().setVariable("editing-mode", "emacs");
		syncEditingOptions(env_);
		vi_command_ = false;
	}

	void LineEditor::viEditingMode() {
		shellKeymap().setVariable("editing-mode", "vi");
		syncEditingOptions(env_);
		vi_command_ = false;
	}

}  // namespace wbsh

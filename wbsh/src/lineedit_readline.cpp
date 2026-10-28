/**
 * @file lineedit_readline.cpp
 * @brief More of readline's commands: history prefix search, menu completion,
 *        the mark and region, character search, quoted insert, overwrite mode.
 */

#include "lineedit.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "keymap.h"
#include "textcells.h"

namespace wbsh {

	namespace readline_detail {

		static const std::size_t kNoEntry = static_cast<std::size_t>(-1);
		static const std::size_t kKillRingSize = 10;

		static bool isOneOf(const std::string& name, std::initializer_list<const char*> names) {
			for (const char* candidate : names) {
				if (name == candidate) return true;
			}

			return false;
		}

		static bool isHistorySearch(const std::string& name) {
			return isOneOf(name, { "history-search-backward", "history-search-forward",
				"history-substring-search-backward", "history-substring-search-forward" });
		}

		static bool isMenuComplete(const std::string& name) {
			return isOneOf(name, { "menu-complete", "menu-complete-backward" });
		}

		static bool isFilenameBoundary(char letter) {
			return letter == '/' || std::isspace(static_cast<unsigned char>(letter)) != 0;
		}

		// How much of `$name` or `${name` comes before the name; 0 when the
		// word is no variable reference.
		static std::size_t variableLead(const std::string& word) {
			if (word.compare(0, 2, "${") == 0) return 2;

			return !word.empty() && word[0] == '$' ? 1 : 0;
		}

		static std::size_t foundAt(const std::string& entry, const std::string& text,
		                           bool anchored) {
			if (anchored) return entry.compare(0, text.size(), text) == 0 ? 0 : std::string::npos;

			return entry.find(text);
		}

		struct NamedCommand {
			const char* name;
			void (LineEditor::*run)();
		};

	}  // namespace readline_detail

	LineEditor::EditCommand LineEditor::readlineCommandNamed(const std::string& name) const {
		static const readline_detail::NamedCommand kCommands[] = {
			{ "backward-kill-line",                      &LineEditor::backwardKillLine },
			{ "character-search",                        &LineEditor::characterSearch },
			{ "character-search-backward",               &LineEditor::characterSearchBackward },
			{ "complete-command",                        &LineEditor::completeCommand },
			{ "complete-filename",                       &LineEditor::completeFilename },
			{ "complete-variable",                       &LineEditor::completeVariable },
			{ "copy-backward-word",                      &LineEditor::copyBackwardWord },
			{ "copy-forward-word",                       &LineEditor::copyForwardWord },
			{ "copy-region-as-kill",                     &LineEditor::copyRegionAsKill },
			{ "delete-char-or-list",                     &LineEditor::deleteCharOrList },
			{ "exchange-point-and-mark",                 &LineEditor::exchangePointAndMark },
			{ "forward-backward-delete-char",            &LineEditor::forwardBackwardDeleteChar },
			{ "history-search-backward",                 &LineEditor::historySearchBackward },
			{ "history-search-forward",                  &LineEditor::historySearchForward },
			{ "history-substring-search-backward",
			  &LineEditor::historySubstringSearchBackward },
			{ "history-substring-search-forward",
			  &LineEditor::historySubstringSearchForward },
			{ "insert-completions",                      &LineEditor::insertCompletions },
			{ "kill-region",                             &LineEditor::killRegion },
			{ "menu-complete",                           &LineEditor::menuComplete },
			{ "menu-complete-backward",                  &LineEditor::menuCompleteBackward },
			{ "non-incremental-forward-search-history",  &LineEditor::forwardSearchHistory },
			{ "non-incremental-reverse-search-history",  &LineEditor::reverseSearchHistory },
			{ "operate-and-get-next",                    &LineEditor::operateAndGetNext },
			{ "overwrite-mode",                          &LineEditor::overwriteMode },
			{ "quoted-insert",                           &LineEditor::quotedInsert },
			{ "redraw-current-line",                     &LineEditor::redrawCurrentLine },
			{ "set-mark",                                &LineEditor::setMark },
			{ "tab-insert",                              &LineEditor::tabInsert },
			{ "unix-filename-rubout",                    &LineEditor::unixFilenameRubout },
		};

		for (const readline_detail::NamedCommand& command : kCommands) {
			if (name == command.name) return command.run;
		}

		return nullptr;
	}

	// What a key that a command waits for does: quoted-insert types it,
	// character-search moves to it.
	bool LineEditor::takePendingKey(const std::string& keys) {
		const PendingKey pending = pending_key_;
		if (pending == PendingKey::kNone) return false;

		pending_key_ = PendingKey::kNone;
		if (pending == PendingKey::kQuotedInsert) {
			typeText(keys);
			return true;
		}

		const UndoPoint before = { buffer_, cursor_ };
		if (!takeViPendingKey(pending, keys)) {
			searchCharacter(keys, pending == PendingKey::kSearchForward);
		}

		recordUndo(before, "pending-key");
		viSettleCursor();
		redrawWhenIdle();
		return true;
	}

	void LineEditor::searchCharacter(const std::string& target, bool forward) {
		std::size_t found = std::string::npos;
		if (forward && cursor_ < buffer_.size()) {
			found = buffer_.find(target, nextClusterEnd(buffer_, cursor_));
		} else if (!forward && cursor_ > 0) {
			found = buffer_.rfind(target, previousClusterStart(buffer_, cursor_));
		}

		if (found == std::string::npos) {
			ding();
			return;
		}

		cursor_ = found;
	}

	// The text before the cursor when the search began is searched for
	// again on each repeat; an anchored search leaves the cursor after it,
	// a substring search after the place it was found.
	void LineEditor::searchHistory(bool older, bool anchored) {
		if (!readline_detail::isHistorySearch(last_command_)) {
			history_search_text_ = buffer_.substr(0, cursor_);
		}

		const auto& history = exec_.history();
		std::size_t back = history_pos_;
		for (;;) {
			if (older && back >= history.size()) break;
			if (!older && back <= 1) break;

			back = older ? back + 1 : back - 1;
			const std::string& entry = history[history.size() - back];
			const std::size_t at = readline_detail::foundAt(entry, history_search_text_, anchored);
			if (at == std::string::npos || entry == buffer_) continue;

			showHistoryEntry(back);
			cursor_ = at + history_search_text_.size();
			return;
		}

		ding();
	}

	void LineEditor::historySearchBackward() {
		searchHistory(true, true);
	}

	void LineEditor::historySearchForward() {
		searchHistory(false, true);
	}

	void LineEditor::historySubstringSearchBackward() {
		searchHistory(true, false);
	}

	void LineEditor::historySubstringSearchForward() {
		searchHistory(false, false);
	}

	// Each press puts the next match in place of the word; after the last
	// one the word comes back as it was typed, with a bell.
	void LineEditor::stepMenu(bool forward) {
		const bool continuing = readline_detail::isMenuComplete(last_command_)
			&& !menu_matches_.empty();
		if (!continuing && !startMenu()) return;

		const std::size_t choices = menu_matches_.size() + 1;
		menu_index_ = forward ? (menu_index_ + 1) % choices
		                      : (menu_index_ + choices - 1) % choices;

		Tok tok = menu_tok_;
		tok.end = menu_end_;
		if (menu_index_ == menu_matches_.size()) {
			buffer_.replace(tok.insert_start, tok.end - tok.insert_start, menu_word_);
			cursor_ = tok.insert_start + menu_word_.size();
			ding();
		} else {
			insertMatch(tok, menu_matches_[menu_index_], false);
		}

		menu_end_ = cursor_;
	}

	// A single match is completed at once, as complete does.
	bool LineEditor::startMenu() {
		Tok tok = currentToken();
		menu_matches_ = currentMatches(tok);
		if (menu_matches_.empty()) {
			ding();
			return false;
		}

		if (menu_matches_.size() == 1) {
			insertMatch(tok, menu_matches_[0], true);
			menu_matches_.clear();
			return false;
		}

		menu_tok_ = tok;
		menu_end_ = tok.end;
		menu_word_ = buffer_.substr(tok.insert_start, tok.end - tok.insert_start);
		menu_index_ = menu_matches_.size();
		return true;
	}

	void LineEditor::menuComplete() {
		stepMenu(true);
	}

	void LineEditor::menuCompleteBackward() {
		stepMenu(false);
	}

	void LineEditor::insertCompletions() {
		Tok tok = currentToken();
		const std::vector<std::string> matches = currentMatches(tok);
		if (matches.empty()) {
			ding();
			return;
		}

		Tok next = tok;
		for (const std::string& match : matches) {
			insertMatch(next, match, true);
			if (buffer_[cursor_ - 1] != ' ') insertChars(" ");

			next.insert_start = next.end = cursor_;
			next.quote = 0;
		}
	}

	void LineEditor::completeWith(std::vector<std::string> matches) {
		Tok tok = currentToken();
		applyCompletion(tok, matches);
	}

	void LineEditor::completeFilename() {
		completion_nospace_ = false;
		completeWith(pathCompletions(currentToken().word));
	}

	void LineEditor::completeCommand() {
		filename_matches_ = false;
		completion_nospace_ = false;
		completeWith(commandCompletions(currentToken().word));
	}

	// `$na` or `${na`: the shell's variables, written the same way.
	void LineEditor::completeVariable() {
		filename_matches_ = false;
		completion_nospace_ = true;
		const std::string word = currentToken().word;
		const std::size_t lead = readline_detail::variableLead(word);
		if (lead == 0) {
			ding();
			return;
		}

		const std::string prefix = word.substr(lead);
		std::set<std::string> names;
		for (const auto& entry : env_.vars()) {
			if (entry.first.compare(0, prefix.size(), prefix) == 0) names.insert(entry.first);
		}

		std::vector<std::string> matches;
		for (const std::string& name : names) {
			matches.push_back(word.substr(0, lead) + name + (lead == 2 ? "}" : ""));
		}

		completeWith(matches);
	}

	void LineEditor::copyToKillRing(const std::string& text) {
		if (text.empty()) return;

		kill_ring_.push_back(text);
		const bool full = kill_ring_.size() > readline_detail::kKillRingSize;
		if (full) kill_ring_.erase(kill_ring_.begin());
		kill_ring_top_ = kill_ring_.size() - 1;
	}

	void LineEditor::setMark() {
		mark_ = cursor_;
	}

	void LineEditor::exchangePointAndMark() {
		const std::size_t mark = (std::min)(mark_, buffer_.size());
		mark_ = cursor_;
		cursor_ = mark;
	}

	void LineEditor::killRegion() {
		const std::size_t mark = (std::min)(mark_, buffer_.size());
		killRange((std::min)(mark, cursor_), (std::max)(mark, cursor_), false);
		mark_ = cursor_;
	}

	void LineEditor::copyRegionAsKill() {
		const std::size_t mark = (std::min)(mark_, buffer_.size());
		const std::size_t start = (std::min)(mark, cursor_);
		copyToKillRing(buffer_.substr(start, (std::max)(mark, cursor_) - start));
	}

	void LineEditor::copyBackwardWord() {
		const std::size_t start = wordStartBefore(cursor_);
		copyToKillRing(buffer_.substr(start, cursor_ - start));
	}

	void LineEditor::copyForwardWord() {
		copyToKillRing(buffer_.substr(cursor_, wordEndAfter(cursor_) - cursor_));
	}

	void LineEditor::backwardKillLine() {
		killRange(0, cursor_, true);
	}

	void LineEditor::unixFilenameRubout() {
		std::size_t start = cursor_;
		while (start > 0 && readline_detail::isFilenameBoundary(buffer_[start - 1])) --start;
		while (start > 0 && !readline_detail::isFilenameBoundary(buffer_[start - 1])) --start;
		killRange(start, cursor_, true);
	}

	void LineEditor::characterSearch() {
		pending_key_ = PendingKey::kSearchForward;
	}

	void LineEditor::characterSearchBackward() {
		pending_key_ = PendingKey::kSearchBackward;
	}

	void LineEditor::quotedInsert() {
		pending_key_ = PendingKey::kQuotedInsert;
	}

	void LineEditor::tabInsert() {
		insertChars("\t");
	}

	void LineEditor::overwriteMode() {
		overwrite_ = !overwrite_;
	}

	// Typed text replaces as many characters as it brings.
	void LineEditor::overwriteChars(const std::string& text) {
		std::size_t end = cursor_;
		for (std::size_t at = 0; at < text.size() && end < buffer_.size(); ) {
			at = nextClusterEnd(text, at);
			end = nextClusterEnd(buffer_, end);
		}

		buffer_.erase(cursor_, end - cursor_);
	}

	void LineEditor::deleteCharOrList() {
		if (buffer_.empty() || cursor_ < buffer_.size()) {
			deleteChar();
			return;
		}

		possibleCompletions();
	}

	void LineEditor::forwardBackwardDeleteChar() {
		if (cursor_ < buffer_.size()) {
			buffer_.erase(cursor_, nextClusterEnd(buffer_, cursor_) - cursor_);
			return;
		}

		backwardDeleteChar();
	}

	void LineEditor::redrawCurrentLine() {
	}

	// The history entry after the one run comes up at the next prompt, so
	// a run of old commands is done again with one key each.
	void LineEditor::operateAndGetNext() {
		const auto& history = exec_.history();
		operate_next_ = history_pos_ == 0
			? readline_detail::kNoEntry : history.size() - history_pos_ + 1;
		acceptLine();
	}

	void LineEditor::loadOperateNext() {
		const std::size_t next = operate_next_;
		operate_next_ = readline_detail::kNoEntry;

		const auto& history = exec_.history();
		if (next >= history.size()) return;

		showHistoryEntry(history.size() - next);
	}

}  // namespace wbsh

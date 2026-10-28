/**
 * @file lineedit_commands.cpp
 * @brief The line editor's key dispatch and the readline commands keys run.
 */

#include "lineedit.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif /* _WIN32 */

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "history.h"
#include "keymap.h"
#include "numparse.h"
#include "pathconv.h"
#include "termreq.h"
#include "textcells.h"

namespace wbsh {

	namespace commands_detail {

		static const std::size_t kKillRingSize = 10;
		static const std::size_t kInsertRunLimit = 20;
		static const int kMacroDepthLimit = 16;
		static const char* const kEofKey = "\x04";
		static const char* const kDefaultEditor = "notepad";

		enum CaseMode { kUpper, kLower, kCapitalize };

		struct NamedCommand {
			const char* name;
			void (LineEditor::*run)();
		};

		// readline's word characters: letters and digits, with every byte
		// of a non-ASCII character counted as a letter.
		static bool isWordByte(char letter) {
			const auto byte = static_cast<unsigned char>(letter);
			return byte >= 0x80 || std::isalnum(byte) != 0;
		}

		static bool isSpaceByte(char letter) {
			return std::isspace(static_cast<unsigned char>(letter)) != 0;
		}

		static bool isBlank(char letter) {
			return letter == ' ' || letter == '\t';
		}

		static bool isOneOf(const std::string& name, std::initializer_list<const char*> names) {
			for (const char* candidate : names) {
				if (name == candidate) return true;
			}

			return false;
		}

		static bool isKillCommand(const std::string& name) {
			return isOneOf(name, { "kill-line", "unix-line-discard", "unix-word-rubout",
				"backward-kill-word", "kill-word", "kill-whole-line" });
		}

		static bool movesThroughHistory(const std::string& name) {
			return isOneOf(name, { "previous-history", "next-history", "beginning-of-history",
				"end-of-history", "reverse-search-history", "forward-search-history" });
		}

		// Typed text inserts itself; a key with control bytes in it, or a
		// Meta key nobody bound, does not.
		static bool insertsItself(const std::string& keys) {
			for (const char letter : keys) {
				const auto byte = static_cast<unsigned char>(letter);
				if (byte < 0x20 || byte == 0x7f) return false;
			}

			return !keys.empty();
		}

		static std::string casedAscii(const std::string& text, bool upper) {
			std::string out = text;
			for (char& letter : out) {
				const auto byte = static_cast<unsigned char>(letter);
				letter = static_cast<char>(upper ? std::toupper(byte) : std::tolower(byte));
			}

			return out;
		}

#ifdef _WIN32
		// Unicode-aware, so `žluť` upcases to `ŽLUŤ`.
		static std::string cased(const std::string& text, bool upper) {
			std::wstring wide = utf8ToWide(text);
			if (wide.empty()) return casedAscii(text, upper);

			const auto length = static_cast<DWORD>(wide.size());
			if (upper) {
				::CharUpperBuffW(wide.data(), length);
			} else {
				::CharLowerBuffW(wide.data(), length);
			}

			return wideToUtf8(wide);
		}
#else
		static std::string cased(const std::string& text, bool upper) {
			return casedAscii(text, upper);
		}
#endif /* _WIN32 */

		static std::string capitalized(const std::string& text) {
			std::size_t first = 0;
			while (first < text.size() && !isWordByte(text[first])) ++first;
			if (first >= text.size()) return text;

			const std::size_t letter_end = first + codePointLength(text, first);
			return text.substr(0, first) + cased(text.substr(first, letter_end - first), true)
				+ cased(text.substr(letter_end), false);
		}

		static std::string temporaryEditFile() {
#ifdef _WIN32
			wchar_t dir[MAX_PATH + 1] = {};
			if (::GetTempPathW(MAX_PATH + 1, dir) == 0) return {};

			return wideToUtf8(dir) + "wbsh-edit-" + std::to_string(::GetCurrentProcessId()) + ".sh";
#else
			return "/tmp/wbsh-edit.sh";
#endif /* _WIN32 */
		}

		static std::string fileText(const std::string& path) {
			std::ifstream file(utf8ToPath(path), std::ios::binary);
			std::stringstream text;
			text << file.rdbuf();

			std::string content = text.str();
			while (!content.empty() && (content.back() == '\n' || content.back() == '\r')) {
				content.pop_back();
			}

			return content;
		}

		static std::string withCrLf(const std::string& text) {
			std::string out;
			for (const char letter : text) {
				if (letter == '\n') out.push_back('\r');
				out.push_back(letter);
			}

			return out;
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

	}  // namespace commands_detail

	LineEditor::EditCommand LineEditor::commandNamed(const std::string& name) const {
		static const commands_detail::NamedCommand kCommands[] = {
			{ "abort",                    &LineEditor::abortKeys },
			{ "accept-line",              &LineEditor::acceptLine },
			{ "backward-char",            &LineEditor::backwardChar },
			{ "backward-delete-char",     &LineEditor::backwardDeleteChar },
			{ "backward-kill-word",       &LineEditor::backwardKillWord },
			{ "backward-word",            &LineEditor::backwardWord },
			{ "beginning-of-history",     &LineEditor::beginningOfHistory },
			{ "beginning-of-line",        &LineEditor::beginningOfLine },
			{ "capitalize-word",          &LineEditor::capitalizeWord },
			{ "clear-screen",             &LineEditor::clearScreen },
			{ "complete",                 &LineEditor::complete },
			{ "delete-char",              &LineEditor::deleteChar },
			{ "delete-horizontal-space",  &LineEditor::deleteHorizontalSpace },
			{ "downcase-word",            &LineEditor::downcaseWord },
			{ "edit-and-execute-command", &LineEditor::editAndExecuteCommand },
			{ "end-of-history",           &LineEditor::endOfHistory },
			{ "end-of-line",              &LineEditor::endOfLine },
			{ "forward-char",             &LineEditor::forwardChar },
			{ "forward-search-history",   &LineEditor::forwardSearchHistory },
			{ "forward-word",             &LineEditor::forwardWord },
			{ "insert-comment",           &LineEditor::insertComment },
			{ "kill-line",                &LineEditor::killLine },
			{ "kill-whole-line",          &LineEditor::killWholeLine },
			{ "kill-word",                &LineEditor::killWord },
			{ "next-history",             &LineEditor::nextHistory },
			{ "paste-from-clipboard",     &LineEditor::pasteFromClipboard },
			{ "possible-completions",     &LineEditor::possibleCompletions },
			{ "previous-history",         &LineEditor::previousHistory },
			{ "re-read-init-file",        &LineEditor::rereadInitFile },
			{ "reverse-search-history",   &LineEditor::reverseSearchHistory },
			{ "revert-line",              &LineEditor::revertLine },
			{ "self-insert",              &LineEditor::selfInsert },
			{ "transpose-chars",          &LineEditor::transposeChars },
			{ "transpose-words",          &LineEditor::transposeWords },
			{ "undo",                     &LineEditor::undo },
			{ "unix-line-discard",        &LineEditor::unixLineDiscard },
			{ "unix-word-rubout",         &LineEditor::unixWordRubout },
			{ "upcase-word",              &LineEditor::upcaseWord },
			{ "yank",                     &LineEditor::yank },
			{ "yank-last-arg",            &LineEditor::yankLastArg },
			{ "yank-nth-arg",             &LineEditor::yankNthArg },
			{ "yank-pop",                 &LineEditor::yankPop },
		};

		for (const commands_detail::NamedCommand& command : kCommands) {
			if (name == command.name) return command.run;
		}

		return nullptr;
	}

	void LineEditor::dispatchKeys(const std::string& keys) {
		pending_keys_ += keys;

		KeyBinding binding;
		const KeyLookup found = shellKeymap().lookup(pending_keys_, binding);
		if (found == KeyLookup::kPrefix) return;

		current_keys_ = pending_keys_;
		pending_keys_.clear();
		if (found == KeyLookup::kBound) {
			runBinding(binding);
		} else if (commands_detail::insertsItself(current_keys_)) {
			runCommand("self-insert");
		} else {
			ding();
		}
	}

	void LineEditor::runBinding(const KeyBinding& binding) {
		switch (binding.kind) {
		case KeyBinding::Kind::kMacro:
			typeMacro(binding.value);
			break;
		case KeyBinding::Kind::kShellCommand:
			runShellBinding(binding.value);
			break;
		default:
			runCommand(binding.value);
			break;
		}
	}

	// Each command remembers the line it changed, for undo, and then the
	// line is shown again unless the command finished it.
	void LineEditor::runCommand(const std::string& name) {
		const EditCommand command = commandNamed(name);
		if (command == nullptr) {
			ding();
			return;
		}

		const UndoPoint before = { buffer_, cursor_ };
		(this->*command)();
		recordUndo(before, name);
		last_command_ = name;
		last_was_tab_ = name == "complete";
		if (!done_) redraw();
	}

	void LineEditor::typeMacro(const std::string& text) {
		if (macro_depth_ >= commands_detail::kMacroDepthLimit) return;

		++macro_depth_;
		std::size_t at = 0;
		while (at < text.size() && !done_) {
			const std::size_t length = codePointLength(text, at);
			dispatchKeys(text.substr(at, length));
			at += length;
		}

		--macro_depth_;
	}

	// As bash's `bind -x`: the command sees the line in READLINE_LINE and
	// the cursor in READLINE_POINT, and whatever it leaves there is the
	// line afterwards. Its output goes below the line, which then starts
	// again under it.
	void LineEditor::runShellBinding(const std::string& command) {
		Environment& env = exec_.env();
		env.set("READLINE_LINE", buffer_);
		env.set("READLINE_POINT", std::to_string(cursor_));

		moveToLineEnd();
		emit("\r\n");
		leaveRawMode();
		const int status = exec_.lastStatus();
		exec_.executeText(command, "bind -x");
		exec_.clearFlow();
		exec_.setLastStatus(status);
		enterRawMode();

		buffer_ = env.get("READLINE_LINE");
		long long point = 0;
		const bool numeric = parseLL(env.get("READLINE_POINT"), point);
		cursor_ = numeric && point >= 0 ? static_cast<std::size_t>(point) : buffer_.size();
		if (cursor_ > buffer_.size()) cursor_ = buffer_.size();

		env.unset("READLINE_LINE");
		env.unset("READLINE_POINT");
		last_command_.clear();
		showPrompt();
		redraw();
	}

	void LineEditor::recordUndo(const UndoPoint& before, const std::string& name) {
		if (name == "undo" || name == "revert-line") return;
		if (commands_detail::movesThroughHistory(name)) {
			undo_.clear();
			insert_run_ = 0;
			return;
		}

		if (buffer_ == before.buffer) return;

		const bool typing = name == "self-insert";
		const bool continues = typing && last_command_ == "self-insert" && !undo_.empty()
			&& insert_run_ < commands_detail::kInsertRunLimit;
		if (continues) {
			++insert_run_;
			return;
		}

		undo_.push_back(before);
		insert_run_ = typing ? 1 : 0;
	}

	void LineEditor::ding() {
		const std::string style = shellKeymap().variable("bell-style");
		if (style == "none" || style == "visible") return;

		emit("\a");
	}

	void LineEditor::insertChars(const std::string& text) {
		buffer_.insert(cursor_, text);
		cursor_ += text.size();
	}

	// Consecutive kills build one kill-ring entry, as in readline: those
	// going backward put their text in front of it.
	void LineEditor::killRange(std::size_t start, std::size_t end, bool prepend) {
		if (end <= start) return;

		const std::string text = buffer_.substr(start, end - start);
		buffer_.erase(start, end - start);
		cursor_ = start;

		if (commands_detail::isKillCommand(last_command_) && !kill_ring_.empty()) {
			std::string& entry = kill_ring_[kill_ring_top_];
			entry = prepend ? text + entry : entry + text;
			return;
		}

		kill_ring_.push_back(text);
		const bool full = kill_ring_.size() > commands_detail::kKillRingSize;
		if (full) kill_ring_.erase(kill_ring_.begin());
		kill_ring_top_ = kill_ring_.size() - 1;
	}

	void LineEditor::insertYanked(const std::string& text, std::size_t& start, std::size_t& end) {
		start = cursor_;
		insertChars(text);
		end = cursor_;
	}

	std::size_t LineEditor::wordStartBefore(std::size_t pos) const {
		while (pos > 0 && !commands_detail::isWordByte(buffer_[pos - 1])) --pos;
		while (pos > 0 && commands_detail::isWordByte(buffer_[pos - 1])) --pos;
		return pos;
	}

	std::size_t LineEditor::wordEndAfter(std::size_t pos) const {
		while (pos < buffer_.size() && !commands_detail::isWordByte(buffer_[pos])) ++pos;
		while (pos < buffer_.size() && commands_detail::isWordByte(buffer_[pos])) ++pos;
		return pos;
	}

	void LineEditor::changeWordCase(int mode) {
		const std::size_t end = wordEndAfter(cursor_);
		const std::string word = buffer_.substr(cursor_, end - cursor_);
		const std::string changed = mode == commands_detail::kCapitalize
			? commands_detail::capitalized(word)
			: commands_detail::cased(word, mode == commands_detail::kUpper);

		buffer_.replace(cursor_, end - cursor_, changed);
		cursor_ += changed.size();
	}

	// `back` counts entries from the newest: 0 is the line being typed.
	void LineEditor::showHistoryEntry(std::size_t back) {
		const auto& history = exec_.history();
		if (history_pos_ == 0) saved_partial_ = buffer_;

		history_pos_ = back;
		buffer_ = back == 0 ? saved_partial_ : history[history.size() - back];
		cursor_ = buffer_.size();
	}

	void LineEditor::abortKeys() {
		ding();
	}

	// The output starts below the whole line, not below the row the
	// cursor was on, and without the ghost-text prediction.
	void LineEditor::acceptLine() {
		moveToLineEnd();
		emit("\r\n");
		accepted_ = buffer_;
		done_ = true;
	}

	// The whole line stays on screen with ^C after it, as readline leaves
	// it; the ghost-text prediction behind the cursor is wiped first.
	void LineEditor::interrupt() {
		pending_keys_.clear();
		moveToLineEnd();
		emit("^C\r\n");

		interrupted_ = true;
		done_ = true;
	}

	void LineEditor::backwardChar() {
		if (cursor_ > 0) cursor_ = previousClusterStart(buffer_, cursor_);
	}

	void LineEditor::forwardChar() {
		if (cursor_ < buffer_.size()) {
			cursor_ = nextClusterEnd(buffer_, cursor_);
			return;
		}

		acceptInlineSuggestion();
	}

	void LineEditor::beginningOfLine() {
		cursor_ = 0;
	}

	void LineEditor::endOfLine() {
		cursor_ = buffer_.size();
	}

	void LineEditor::backwardWord() {
		cursor_ = wordStartBefore(cursor_);
	}

	void LineEditor::forwardWord() {
		cursor_ = wordEndAfter(cursor_);
	}

	void LineEditor::backwardDeleteChar() {
		if (cursor_ == 0) {
			ding();
			return;
		}

		const std::size_t start = previousClusterStart(buffer_, cursor_);
		buffer_.erase(start, cursor_ - start);
		cursor_ = start;
	}

	// Ctrl-D ends the input on an empty line; Delete never does.
	void LineEditor::deleteChar() {
		if (buffer_.empty() && current_keys_ == commands_detail::kEofKey) {
			eof_ = true;
			done_ = true;
			return;
		}

		if (cursor_ >= buffer_.size()) {
			ding();
			return;
		}

		buffer_.erase(cursor_, nextClusterEnd(buffer_, cursor_) - cursor_);
	}

	void LineEditor::killLine() {
		killRange(cursor_, buffer_.size(), false);
	}

	void LineEditor::unixLineDiscard() {
		killRange(0, cursor_, true);
	}

	void LineEditor::killWholeLine() {
		killRange(0, buffer_.size(), false);
	}

	void LineEditor::unixWordRubout() {
		std::size_t start = cursor_;
		while (start > 0 && commands_detail::isSpaceByte(buffer_[start - 1])) --start;
		while (start > 0 && !commands_detail::isSpaceByte(buffer_[start - 1])) --start;
		killRange(start, cursor_, true);
	}

	void LineEditor::backwardKillWord() {
		killRange(wordStartBefore(cursor_), cursor_, true);
	}

	void LineEditor::killWord() {
		killRange(cursor_, wordEndAfter(cursor_), false);
	}

	void LineEditor::yank() {
		if (kill_ring_.empty()) {
			ding();
			return;
		}

		insertYanked(kill_ring_[kill_ring_top_], yank_start_, yank_end_);
	}

	// Only right after a yank: the yanked text is swapped for the kill
	// before it in the ring.
	void LineEditor::yankPop() {
		const bool after_yank = last_command_ == "yank" || last_command_ == "yank-pop";
		if (!after_yank || kill_ring_.empty()) {
			ding();
			return;
		}

		buffer_.erase(yank_start_, yank_end_ - yank_start_);
		cursor_ = yank_start_;
		kill_ring_top_ = (kill_ring_top_ + kill_ring_.size() - 1) % kill_ring_.size();
		insertYanked(kill_ring_[kill_ring_top_], yank_start_, yank_end_);
	}

	// Repeated, it steps to older entries, replacing what it inserted.
	void LineEditor::yankLastArg() {
		if (last_command_ == "yank-last-arg") {
			buffer_.erase(last_arg_start_, last_arg_end_ - last_arg_start_);
			cursor_ = last_arg_start_;
		} else {
			last_arg_back_ = 0;
		}

		const auto& history = exec_.history();
		if (last_arg_back_ >= history.size()) {
			last_arg_start_ = last_arg_end_ = cursor_;
			ding();
			return;
		}

		++last_arg_back_;
		const std::string& entry = history[history.size() - last_arg_back_];
		const std::vector<std::string> words = historyWords(entry);
		const std::string word = words.empty() ? std::string() : words.back();
		insertYanked(word, last_arg_start_, last_arg_end_);
	}

	void LineEditor::yankNthArg() {
		const auto& history = exec_.history();
		const std::vector<std::string> words = history.empty()
			? std::vector<std::string>() : historyWords(history.back());
		if (words.size() < 2) {
			ding();
			return;
		}

		std::size_t start = 0;
		std::size_t end = 0;
		insertYanked(words[1], start, end);
	}

	void LineEditor::transposeChars() {
		if (cursor_ == 0 || buffer_.size() < 2) {
			ding();
			return;
		}

		if (cursor_ >= buffer_.size()) cursor_ = previousClusterStart(buffer_, cursor_);
		const std::size_t first = previousClusterStart(buffer_, cursor_);
		const std::size_t second_end = nextClusterEnd(buffer_, cursor_);
		if (first == cursor_ || second_end == cursor_) {
			ding();
			return;
		}

		const std::string left = buffer_.substr(first, cursor_ - first);
		const std::string right = buffer_.substr(cursor_, second_end - cursor_);
		buffer_.replace(first, second_end - first, right + left);
		cursor_ = second_end;
	}

	void LineEditor::transposeWords() {
		const std::size_t second_end = wordEndAfter(cursor_);
		const std::size_t second_start = wordStartBefore(second_end);
		const std::size_t first_start = wordStartBefore(second_start);
		const std::size_t first_end = wordEndAfter(first_start);
		if (first_start == second_start || first_end > second_start) {
			ding();
			return;
		}

		const std::string first = buffer_.substr(first_start, first_end - first_start);
		const std::string middle = buffer_.substr(first_end, second_start - first_end);
		const std::string second = buffer_.substr(second_start, second_end - second_start);
		buffer_.replace(first_start, second_end - first_start, second + middle + first);
		cursor_ = second_end;
	}

	void LineEditor::upcaseWord() {
		changeWordCase(commands_detail::kUpper);
	}

	void LineEditor::downcaseWord() {
		changeWordCase(commands_detail::kLower);
	}

	void LineEditor::capitalizeWord() {
		changeWordCase(commands_detail::kCapitalize);
	}

	void LineEditor::undo() {
		if (undo_.empty()) {
			ding();
			return;
		}

		buffer_ = undo_.back().buffer;
		cursor_ = undo_.back().cursor;
		undo_.pop_back();
		insert_run_ = 0;
	}

	void LineEditor::revertLine() {
		if (undo_.empty()) return;

		buffer_ = undo_.front().buffer;
		cursor_ = undo_.front().cursor;
		undo_.clear();
		insert_run_ = 0;
	}

	void LineEditor::deleteHorizontalSpace() {
		std::size_t start = cursor_;
		std::size_t end = cursor_;
		while (start > 0 && commands_detail::isBlank(buffer_[start - 1])) --start;
		while (end < buffer_.size() && commands_detail::isBlank(buffer_[end])) ++end;

		buffer_.erase(start, end - start);
		cursor_ = start;
	}

	void LineEditor::insertComment() {
		buffer_.insert(0, "#");
		acceptLine();
	}

	void LineEditor::selfInsert() {
		insertChars(current_keys_);
	}

	void LineEditor::previousHistory() {
		const auto& history = exec_.history();
		if (history_pos_ >= history.size()) {
			ding();
			return;
		}

		showHistoryEntry(history_pos_ + 1);
	}

	void LineEditor::nextHistory() {
		if (history_pos_ == 0) {
			ding();
			return;
		}

		showHistoryEntry(history_pos_ - 1);
	}

	void LineEditor::beginningOfHistory() {
		const auto& history = exec_.history();
		if (!history.empty()) showHistoryEntry(history.size());
	}

	void LineEditor::endOfHistory() {
		if (history_pos_ != 0) showHistoryEntry(0);
	}

	void LineEditor::reverseSearchHistory() {
		runReverseSearch();
	}

	void LineEditor::forwardSearchHistory() {
		runReverseSearch();
	}

	void LineEditor::complete() {
		Tok tok = currentToken();
		applyCompletion(tok, currentMatches(tok));
	}

	void LineEditor::possibleCompletions() {
		Tok tok = currentToken();
		const std::vector<std::string> matches = currentMatches(tok);
		if (matches.empty()) {
			ding();
			return;
		}

		printMatches(matches);
	}

	void LineEditor::clearScreen() {
		emit("\x1b[H\x1b[2J\x1b[3J");
		requestScrollbackClear();
		showPrompt();
	}

	void LineEditor::pasteFromClipboard() {
		insertClipboardText();
	}

	void LineEditor::rereadInitFile() {
		loadInputrc(exec_);
	}

	// As bash's C-x C-e: the line goes to $VISUAL or $EDITOR in a file,
	// and what the editor leaves there runs at once.
	void LineEditor::editAndExecuteCommand() {
		const std::string path = commands_detail::temporaryEditFile();
		std::ofstream(utf8ToPath(path), std::ios::binary) << buffer_ << '\n';

		std::string editor = env_.get("VISUAL");
		if (editor.empty()) editor = env_.get("EDITOR");
		if (editor.empty()) editor = commands_detail::kDefaultEditor;

		moveToLineEnd();
		emit("\r\n");
		leaveRawMode();
		const int status = exec_.lastStatus();
		const std::string posix_path = exec_.pathConv().toPosix(path);
		exec_.executeText(editor + " " + commands_detail::singleQuoted(posix_path), "C-x C-e");
		exec_.clearFlow();
		exec_.setLastStatus(status);
		enterRawMode();

		accepted_ = commands_detail::fileText(path);
		std::error_code ec;
		std::filesystem::remove(utf8ToPath(path), ec);

		buffer_ = accepted_;
		cursor_ = buffer_.size();
		emit(commands_detail::withCrLf(accepted_) + "\r\n");
		done_ = true;
	}

}  // namespace wbsh

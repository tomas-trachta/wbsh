/**
 * @file lineedit.cpp
 * @brief Raw-console line editor: keys, redraw, history, and Tab completion.
 */

#include "lineedit.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>

#  include <io.h>
#endif /* _WIN32 */

#include <algorithm>
#include <atomic>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "consolekeys.h"
#include "numparse.h"
#include "strscan.h"
#include "termreq.h"
#include "textcells.h"
#include "utils.h"

namespace wbsh {

	namespace fs = std::filesystem;

	static const std::size_t kDefaultConsoleWidth = 80;
	static const std::size_t kNoHistoryIndex = static_cast<std::size_t>(-1);
	static const char* const kDimStyle = "\x1b[90m";
	static const char* const kResetStyle = "\x1b[0m";
	static const char* const kEraseToEnd = "\r\x1b[J";

#ifdef _WIN32
	static bool stdinIsTty() {
		return _isatty(_fileno(stdin)) != 0;
	}

#endif /* _WIN32 */

	// A prompt marks the bytes that take no cells -- what PS1 writes as
	// \[ and \] -- with \001 and \002, as readline expects them. Escape
	// sequences and control characters outside the marks take none either.
	namespace prompt_detail {

		static const char kStartIgnore = '\x01';
		static const char kEndIgnore = '\x02';
		static const char kEscape = '\x1b';
		static const char kBell = '\x07';

		static std::size_t skipCsi(const std::string& text, std::size_t i) {
			i += 2;
			while (i < text.size() && !(text[i] >= '@' && text[i] <= '~')) ++i;
			return i < text.size() ? i + 1 : i;
		}

		static std::size_t skipOsc(const std::string& text, std::size_t i) {
			for (i += 2; i < text.size(); ++i) {
				if (text[i] == kBell) return i + 1;
				if (text[i] == kEscape && i + 1 < text.size() && text[i + 1] == '\\') return i + 2;
			}

			return i;
		}

		static std::size_t skipEscape(const std::string& text, std::size_t i) {
			const char kind = i + 1 < text.size() ? text[i + 1] : '\0';
			if (kind == '[') return skipCsi(text, i);
			if (kind == ']') return skipOsc(text, i);

			return std::min<std::size_t>(i + 2, text.size());
		}

		static std::size_t skipIgnored(const std::string& text, std::size_t i) {
			const std::size_t end = text.find(kEndIgnore, i);
			return end == std::string::npos ? text.size() : end + 1;
		}

		static bool takesNoCells(char letter) {
			const auto byte = static_cast<unsigned char>(letter);
			return (byte < 0x20 && letter != '\n') || byte == 0x7f;
		}

		static std::string visiblePart(const std::string& text) {
			std::string shown;
			std::size_t i = 0;
			while (i < text.size()) {
				if (text[i] == kStartIgnore) {
					i = skipIgnored(text, i);
				} else if (text[i] == kEscape) {
					i = skipEscape(text, i);
				} else {
					if (!takesNoCells(text[i])) shown.push_back(text[i]);
					++i;
				}
			}

			return shown;
		}

		static std::string withoutMarks(const std::string& text) {
			std::string shown;
			for (const char letter : text) {
				if (letter != kStartIgnore && letter != kEndIgnore) shown.push_back(letter);
			}

			return shown;
		}

		// Where the prompt's last line starts: past its last newline that
		// is not inside \[ \].
		static std::size_t lastLineStart(const std::string& text) {
			std::size_t start = 0;
			std::size_t i = 0;
			while (i < text.size()) {
				if (text[i] == kStartIgnore) {
					i = skipIgnored(text, i);
					continue;
				}

				if (text[i] == '\n') start = i + 1;
				++i;
			}

			return start;
		}

	}  // namespace prompt_detail

	static bool hasPrefix(const std::string& text, const std::string& prefix) {
		return text.compare(0, prefix.size(), prefix) == 0;
	}

	std::size_t findReverseSearchMatch(const std::vector<std::string>& history,
	                                   const std::string& query,
	                                   std::size_t start_index,
	                                   bool forward) {
		if (query.empty() || history.empty()) return history.size();

		const std::size_t start = std::min<std::size_t>(start_index, history.size() - 1);
		if (forward) {
			for (std::size_t i = start; i < history.size(); ++i) {
				if (history[i].find(query) != std::string::npos) return i;
			}

			return history.size();
		}

		// Backward (toward older entries). i is unsigned, so guard the
		// underflow by walking until i hits 0 and then breaking after the
		// final compare. The loop body runs for i == 0 too.
		for (std::size_t i = start;; --i) {
			if (history[i].find(query) != std::string::npos) return i;
			if (i == 0) break;
		}

		return history.size();
	}

	static bool extendsPrefix(const std::string& entry, const std::string& prefix) {
		return entry.size() > prefix.size() && hasPrefix(entry, prefix);
	}

	// Newest-first scan for the first history entry that begins with
	// `prefix` and is strictly longer than it, skipping entries whose exit
	// status was non-zero. Returns the matching entry's index (history.size()
	// when nothing matches) so callers can cache and incrementally extend
	// the match on subsequent keystrokes instead of rescanning.
	static std::size_t findInlinePredictionIndexed(const std::vector<std::string>& history,
	                                               const std::vector<int>& history_status,
	                                               const std::string& prefix) {
		if (prefix.empty()) return history.size();

		for (std::size_t i = history.size(); i > 0; --i) {
			const std::size_t index = i - 1;
			if (index < history_status.size() && history_status[index] != 0) continue;
			if (extendsPrefix(history[index], prefix)) return index;
		}

		return history.size();
	}

	std::string findInlinePrediction(const std::vector<std::string>& history,
	                                 const std::vector<int>& history_status,
	                                 const std::string& prefix) {
		const std::size_t index = findInlinePredictionIndexed(history, history_status, prefix);
		if (index >= history.size()) return {};

		return history[index].substr(prefix.size());
	}

	static void stripLineEnding(std::string& line) {
		while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
	}

	// Pasted lines stay lines of one command, which runs when Enter is
	// pressed; a line break at the very end would run it at once.
	static std::string pastedLines(const std::string& text) {
		std::string lines;
		lines.reserve(text.size());
		for (std::size_t i = 0; i < text.size(); ++i) {
			const bool crlf = text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n';
			if (crlf) continue;

			lines.push_back(text[i] == '\r' ? '\n' : text[i]);
		}

		stripLineEnding(lines);
		return lines;
	}

	static fs::path findGitEntry(fs::path& probe) {
		std::error_code ec;
		for (;;) {
			const fs::path candidate = probe / ".git";
			if (fs::exists(candidate, ec)) return candidate;

			const fs::path parent = probe.parent_path();
			if (parent == probe) return {};

			probe = parent;
		}
	}

	static fs::path resolveGitDirFile(const fs::path& gitfile, const fs::path& worktree) {
		std::ifstream file(gitfile);
		std::string line;
		if (!std::getline(file, line)) return {};

		stripLineEnding(line);
		const std::string prefix = "gitdir: ";
		if (!hasPrefix(line, prefix)) return {};

		fs::path target = utf8ToPath(line.substr(prefix.size()));
		if (!target.is_absolute()) target = (worktree / target).lexically_normal();
		return target;
	}

	fs::path findGitDir() {
		std::error_code ec;
		fs::path probe = fs::current_path(ec);
		if (ec) return {};

		const fs::path gitdir = findGitEntry(probe);
		if (gitdir.empty()) return {};
		if (fs::is_directory(gitdir, ec)) return gitdir;

		return resolveGitDirFile(gitdir, probe);
	}

	fs::path findGitCommonDir(const fs::path& git_dir) {
		std::ifstream file(git_dir / "commondir");
		std::string line;
		if (!std::getline(file, line)) return git_dir;

		stripLineEnding(line);
		if (line.empty()) return git_dir;

		fs::path common = utf8ToPath(line);
		if (!common.is_absolute()) common = (git_dir / common).lexically_normal();
		return common;
	}

	bool usesRefTable(const fs::path& common_dir) {
		std::error_code ec;
		return fs::is_directory(common_dir / "reftable", ec);
	}

	LineEditor::LineEditor(Environment& env, Executor& exec)
		: env_(env), exec_(exec) {}

	void LineEditor::setPromptRefresher(std::function<std::string()> refresher) {
		prompt_refresher_ = std::move(refresher);
	}

	LineStatus LineEditor::readLine(const std::string& prompt, std::string& out) {
		resetLine(prompt);

#ifdef _WIN32
		if (stdinIsTty()) return readLineRaw(prompt, out);
#endif /* _WIN32 */

		std::fputs(prompt_detail::withoutMarks(prompt).c_str(), stdout);
		std::fflush(stdout);
		return readLineCooked(out);
	}

	void LineEditor::resetLine(const std::string& prompt) {
		buffer_.clear();
		cursor_ = 0;
		history_pos_ = 0;
		saved_partial_.clear();
		last_was_tab_ = false;
		last_tab_word_.clear();
		last_cursor_row_ = 0;
		suggestion_.clear();
		suggestion_cache_idx_ = kNoHistoryIndex;
		revsearch_active_ = false;
		interrupted_ = false;
		done_ = false;
		eof_ = false;
		accepted_.clear();
		pending_keys_.clear();
		last_command_.clear();
		undo_.clear();
		insert_run_ = 0;
		mark_ = 0;
		overwrite_ = false;
		pending_key_ = PendingKey::kNone;
		menu_matches_.clear();
		resetViState();
		setPrompt(prompt);
		loadOperateNext();
	}

	// Only the prompt's last line is drawn again as the line changes; the
	// lines above it are written once.
	void LineEditor::setPrompt(const std::string& prompt) {
		const std::size_t last_line = prompt_detail::lastLineStart(prompt);
		prompt_full_ = prompt;
		prompt_lead_visible_ = prompt_detail::visiblePart(prompt.substr(0, last_line));
		prompt_lead_ = prompt_detail::withoutMarks(prompt.substr(0, last_line));
		prompt_raw_ = prompt_detail::withoutMarks(prompt.substr(last_line));
		prompt_visible_ = prompt_detail::visiblePart(prompt.substr(last_line));
	}

	LineStatus LineEditor::readLineCooked(std::string& out) {
		out.clear();

		int byte;
		while ((byte = std::fgetc(stdin)) != EOF) {
			if (byte == '\n') break;
			out.push_back(static_cast<char>(byte));
		}

		if (byte == EOF && out.empty()) return LineStatus::kEof;

		if (!out.empty() && out.back() == '\r') out.pop_back();
		return LineStatus::kLine;
	}

	LineStatus LineEditor::lineStatus() const {
		if (interrupted_) return LineStatus::kInterrupted;
		if (eof_ && accepted_.empty()) return LineStatus::kEof;

		return LineStatus::kLine;
	}

	std::string LineEditor::describeReplay(bool finished) const {
		if (!finished) {
			return "[" + buffer_.substr(0, cursor_) + "][" + buffer_.substr(cursor_) + "]";
		}

		switch (lineStatus()) {
		case LineStatus::kInterrupted: return "interrupt";
		case LineStatus::kEof:         return "eof";
		case LineStatus::kLine:        return "accept [" + accepted_ + "]";
		default:                       return {};
		}
	}

	std::vector<std::string> LineEditor::currentMatches(Tok& tok) {
		return completionsFor(tok.word, tok.first);
	}

#ifdef _WIN32

	static std::size_t consoleWidth() {
		const HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
		if (out == INVALID_HANDLE_VALUE) return kDefaultConsoleWidth;

		CONSOLE_SCREEN_BUFFER_INFO info{};
		if (!::GetConsoleScreenBufferInfo(out, &info)) return kDefaultConsoleWidth;

		const int width = info.srWindow.Right - info.srWindow.Left + 1;
		if (width <= 0) return kDefaultConsoleWidth;

		return static_cast<std::size_t>(width);
	}

	// A resize reports the buffer, whose height is the scrollback; LINES
	// counts the window's rows.
	static int consoleLines(SHORT reported, bool replaying) {
		if (replaying) return reported;

		CONSOLE_SCREEN_BUFFER_INFO info{};
		const HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
		if (!::GetConsoleScreenBufferInfo(out, &info)) return reported;

		return info.srWindow.Bottom - info.srWindow.Top + 1;
	}

	static std::string utf16ToUtf8(const WCHAR* units, int count) {
		const int length = ::WideCharToMultiByte(CP_UTF8, 0, units, count,
			nullptr, 0, nullptr, nullptr);
		if (length <= 0) return {};

		std::string utf8(static_cast<std::size_t>(length), '\0');
		::WideCharToMultiByte(CP_UTF8, 0, units, count, utf8.data(), length, nullptr, nullptr);
		return utf8;
	}

	static bool readInputRecord(HANDLE input, INPUT_RECORD& record) {
		if (input == INVALID_HANDLE_VALUE) return false;

		DWORD read = 0;
		return ::ReadConsoleInputW(input, &record, 1, &read) && read != 0;
	}

	static void appendCursorUp(std::string& out, std::size_t rows) {
		if (rows > 0) out += "\x1b[" + std::to_string(rows) + "A";
	}

	static void appendCursorRight(std::string& out, std::size_t columns) {
		if (columns > 0) out += "\x1b[" + std::to_string(columns) + "C";
	}

	void LineEditor::emit(const std::string& text) {
		if (headless_) {
			if (capture_output_) captured_output_ += text;
			return;
		}

		const HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
		DWORD wrote;
		::WriteFile(out, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
	}

	struct ScreenPos {
		std::size_t row;
		std::size_t col;
	};

	// Where writing text[0, end) from `pos` leaves the cursor on a screen
	// `width` cells wide. A cluster that does not fit on the row moves
	// whole to the next one, as the console wraps it, and `col == width`
	// is the pending wrap after a row's last cell.
	static ScreenPos advanceOver(const std::string& text, std::size_t end, ScreenPos pos,
	                             std::size_t width) {
		std::size_t at = 0;
		while (at < end && at < text.size()) {
			if (text[at] == '\n') {
				pos = { pos.row + 1, 0 };
				++at;
				continue;
			}

			const TextCluster cluster = clusterAt(text, at);
			if (pos.col + cluster.width > width) pos = { pos.row + 1, 0 };
			pos.col += cluster.width;
			at = cluster.end;
		}

		return pos;
	}

	static ScreenPos settled(ScreenPos pos, std::size_t width) {
		if (pos.col < width) return pos;

		return { pos.row + 1, 0 };
	}

	static std::string withCrLf(const std::string& text) {
		std::string out;
		out.reserve(text.size());
		for (const char letter : text) {
			if (letter == '\n') out.push_back('\r');
			out.push_back(letter);
		}

		return out;
	}

	void LineEditor::appendLineText(std::string& out) const {
		out += withCrLf(buffer_);
		if (suggestion_.empty()) return;

		out += kDimStyle;
		out += withCrLf(suggestion_);
		out += kResetStyle;
	}

	// A line that ends exactly at the right edge leaves the console's
	// cursor waiting on that edge; the CRLF puts it on the row below,
	// where the layout counts it.
	std::size_t LineEditor::screenColumns() const {
		return forced_columns_ != 0 ? forced_columns_ : consoleWidth();
	}

	static ScreenPos promptEnd(const std::string& visible, std::size_t width) {
		return advanceOver(visible, visible.size(), { 0, 0 }, width);
	}

	std::string LineEditor::describeCursorCell() const {
		const std::size_t width = screenColumns();
		const ScreenPos start = promptEnd(prompt_visible_, width);
		const ScreenPos cursor = settled(advanceOver(buffer_, cursor_, start, width), width);
		return "@" + std::to_string(cursor.row) + ":" + std::to_string(cursor.col);
	}

	void LineEditor::redraw() {
		const std::size_t width = screenColumns();
		refreshSuggestion();

		const ScreenPos start = promptEnd(prompt_visible_, width);
		const ScreenPos cursor = settled(advanceOver(buffer_, cursor_, start, width), width);
		const ScreenPos text_end = advanceOver(buffer_, buffer_.size(), start, width);
		const ScreenPos end = advanceOver(suggestion_, suggestion_.size(), text_end, width);

		std::string out;
		appendCursorUp(out, last_cursor_row_);
		out += kEraseToEnd;
		out += prompt_raw_;
		appendLineText(out);
		if (end.col >= width) out += "\r\n";

		out += "\r";
		appendCursorUp(out, settled(end, width).row - cursor.row);
		appendCursorRight(out, cursor.col);

		last_cursor_row_ = cursor.row;
		redraw_due_ = false;
		emit(out);
	}

	// Keys already waiting -- a paste, or keys typed while the last ones
	// were handled -- are all taken in before the line is drawn again.
	void LineEditor::redrawWhenIdle() {
		if (keysWaiting()) {
			redraw_due_ = true;
			return;
		}

		redraw();
	}

	// The whole prompt, the lines above its last one included, from where
	// the cursor is; the next redraw starts from its last line.
	void LineEditor::showPrompt() {
		emit(withCrLf(prompt_lead_) + prompt_raw_);
		last_cursor_row_ = promptEnd(prompt_visible_, screenColumns()).row;
	}

	void LineEditor::moveToLineEnd() {
		cursor_ = buffer_.size();
		redraw();
		emit("\x1b[J");
	}

	void LineEditor::refreshSuggestion() {
		suggestion_.clear();
		if (revsearch_active_ || buffer_.empty() || cursor_ != buffer_.size()) {
			suggestion_cache_idx_ = kNoHistoryIndex;
			return;
		}

		// Fast path: if the cached match still begins with the (possibly
		// grown or shrunk) buffer, reuse it instead of rescanning the
		// whole history on every keystroke.
		const auto& history = exec_.history();
		if (suggestion_cache_idx_ < history.size()) {
			const std::string& entry = history[suggestion_cache_idx_];
			if (extendsPrefix(entry, buffer_)) {
				suggestion_ = entry.substr(buffer_.size());
				return;
			}
		}

		suggestion_cache_idx_ =
			findInlinePredictionIndexed(history, exec_.historyStatus(), buffer_);
		if (suggestion_cache_idx_ < history.size()) {
			suggestion_ = history[suggestion_cache_idx_].substr(buffer_.size());
		}
	}

	bool LineEditor::acceptInlineSuggestion() {
		if (suggestion_.empty()) return false;

		insertChars(suggestion_);
		suggestion_.clear();
		return true;
	}

	static bool readClipboardText(std::wstring& text) {
		if (!::OpenClipboard(nullptr)) return false;

		const HANDLE clip = ::GetClipboardData(CF_UNICODETEXT);
		if (clip == nullptr) {
			::CloseClipboard();
			return false;
		}

		const auto* units = static_cast<const WCHAR*>(::GlobalLock(clip));
		if (units == nullptr) {
			::CloseClipboard();
			return false;
		}

		text = units;
		::GlobalUnlock(clip);
		::CloseClipboard();
		return true;
	}

	void LineEditor::insertClipboardText() {
		std::wstring text;
		if (!readClipboardText(text)) return;

		insertChars(pastedLines(utf16ToUtf8(text.data(), static_cast<int>(text.size()))));
	}

	// Inside \e[200~ ... \e[201~ every key is text: Enter is a line
	// break, and nothing runs until the paste has ended.
	std::string LineEditor::pastedKeyText(const KEY_EVENT_RECORD& key) {
		const WCHAR ch = key.uChar.UnicodeChar;
		if (key.wVirtualKeyCode == VK_RETURN || ch == L'\r' || ch == L'\n') return "\n";
		if (key.wVirtualKeyCode == VK_ESCAPE || ch == 0x1b) return "\x1b";
		if (ch >= 0x80) return wideCharacter(ch);
		if (ch == L'\t' || ch >= 0x20) return std::string(1, static_cast<char>(ch));

		return {};
	}

	static bool endsWith(const std::string& text, const std::string& tail) {
		return text.size() >= tail.size()
		    && text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
	}

	std::string LineEditor::readBracketedPaste() {
		static const std::string kPasteEnd = "\x1b[201~";

		std::string text;
		KEY_EVENT_RECORD key{};
		while (nextKeyDown(key)) {
			text += pastedKeyText(key);
			if (!endsWith(text, kPasteEnd)) continue;

			text.resize(text.size() - kPasteEnd.size());
			break;
		}

		return pastedLines(text);
	}

	std::string LineEditor::replayCharacter(wchar_t ch) {
		WCHAR units[2] = { ch, 0 };
		const bool high = ch >= 0xD800 && ch <= 0xDBFF;
		const WCHAR next = replay_keys_.empty() || replay_keys_.front().EventType != KEY_EVENT
			? 0 : replay_keys_.front().Event.KeyEvent.uChar.UnicodeChar;
		const bool low_follows = next >= 0xDC00 && next <= 0xDFFF;
		if (!high || !low_follows) return utf16ToUtf8(units, 1);

		units[1] = next;
		replay_keys_.pop_front();
		return utf16ToUtf8(units, 2);
	}

	std::string LineEditor::wideCharacter(wchar_t ch) {
		if (headless_) return replayCharacter(ch);

		return readConsoleCharacter(::GetStdHandle(STD_INPUT_HANDLE), ch);
	}

	// As readline, the search moves through the places a query occurs, not
	// just the entries: toward older text the last place at or before
	// `hit.offset` in `hit.entry`, then the last place in each older entry.
	static bool searchOlder(const std::vector<std::string>& history, const std::string& query,
	                        SearchHit& hit) {
		if (query.empty() || hit.entry >= history.size()) return false;

		std::size_t from = hit.offset;
		for (std::size_t i = hit.entry + 1; i > 0; --i) {
			const std::size_t found = history[i - 1].rfind(query, from);
			if (found != std::string::npos) {
				hit = { i - 1, found };
				return true;
			}

			from = std::string::npos;
		}

		return false;
	}

	static bool searchNewer(const std::vector<std::string>& history, const std::string& query,
	                        SearchHit& hit) {
		if (query.empty()) return false;

		std::size_t from = hit.offset;
		for (std::size_t i = hit.entry; i < history.size(); ++i) {
			const std::size_t found = history[i].find(query, from);
			if (found != std::string::npos) {
				hit = { i, found };
				return true;
			}

			from = 0;
		}

		return false;
	}

	static SearchHit newestPlace(const std::vector<std::string>& history) {
		return { history.empty() ? 0 : history.size() - 1, std::string::npos };
	}

	static bool searchFails(const std::vector<std::string>& history, const std::string& query,
	                        const SearchHit& hit) {
		if (query.empty()) return false;
		if (hit.entry >= history.size()) return true;

		return history[hit.entry].compare(hit.offset, query.size(), query) != 0;
	}

	// The cursor sits where the query starts in the match, as in readline.
	// While the query matches nothing the last match stays, and before
	// the first one the line the search started from.
	void LineEditor::revsearchShowMatch(const SearchState& search) {
		const auto& history = exec_.history();
		if (search.match >= history.size()) {
			buffer_ = revsearch_origin_.buffer;
			cursor_ = revsearch_origin_.cursor;
			return;
		}

		buffer_ = history[search.match];
		cursor_ = (std::min)(search.offset, buffer_.size());
	}

	void LineEditor::revsearchRefresh(const SearchState& search) {
		const SearchHit hit = { search.match, search.offset };
		const bool failing = searchFails(exec_.history(), search.query, hit);
		prompt_raw_ = std::string(failing ? "(failed reverse-i-search)`" : "(reverse-i-search)`")
			+ search.query + "': ";
		prompt_visible_ = prompt_raw_;

		revsearchShowMatch(search);
		redraw();
	}

	void LineEditor::revsearchMoveTo(SearchState& search, bool found, const SearchHit& hit) {
		if (found) {
			search.match = hit.entry;
			search.offset = hit.offset;
		} else {
			ding();
		}

		revsearchRefresh(search);
	}

	// Ctrl-R or Ctrl-S on an empty query searches for the last query again.
	void LineEditor::revsearchStepOlder(SearchState& search) {
		if (search.query.empty()) search.query = last_search_query_;

		const auto& history = exec_.history();
		SearchHit hit = newestPlace(history);
		if (search.match < history.size()) {
			const bool line_start = search.offset == 0;
			hit = line_start ? SearchHit{ search.match - 1, std::string::npos }
			                 : SearchHit{ search.match, search.offset - 1 };
		}

		const bool before_first = search.match == 0 && search.offset == 0;
		const bool found = !before_first && searchOlder(history, search.query, hit);
		revsearchMoveTo(search, found, hit);
	}

	void LineEditor::revsearchStepNewer(SearchState& search) {
		if (search.query.empty()) search.query = last_search_query_;

		const auto& history = exec_.history();
		SearchHit hit = { search.match, search.offset + 1 };
		const bool found = search.match < history.size() && searchNewer(history, search.query, hit);
		revsearchMoveTo(search, found, hit);
	}

	void LineEditor::revsearchEraseQueryChar(SearchState& search) {
		if (search.query.empty()) {
			ding();
			return;
		}

		search.query.erase(previousClusterStart(search.query, search.query.size()));
		const auto& history = exec_.history();
		SearchHit hit = newestPlace(history);
		if (searchOlder(history, search.query, hit)) {
			search.match = hit.entry;
			search.offset = hit.offset;
		} else {
			search.match = history.size();
		}

		revsearchRefresh(search);
	}

	// A longer query stays on the place it matched if it still matches there.
	void LineEditor::revsearchExtendQuery(SearchState& search, const std::string& text) {
		search.query += text;

		const auto& history = exec_.history();
		SearchHit hit = newestPlace(history);
		if (search.match < history.size()) hit = { search.match, search.offset };

		const bool found = searchOlder(history, search.query, hit);
		revsearchMoveTo(search, found, hit);
	}

	// Another thread asks the editor waiting for keys to rebuild its
	// prompt by putting a menu event, which no key produces, into the
	// console's input queue.
	namespace refresh_detail {

		static const UINT kRefreshCommand = 0x7762;
		static std::atomic<bool> g_reading_keys{ false };

		static bool asksForRefresh(const INPUT_RECORD& record) {
			return record.EventType == MENU_EVENT
			    && record.Event.MenuEvent.dwCommandId == kRefreshCommand;
		}

	}  // namespace refresh_detail

	namespace keys_detail {

		static const DWORD kCtrlBits = LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED;
		static const DWORD kAltBits = LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED;
		static const char* const kMetaPrefix = "\x1b";

		static bool ctrlHeld(const KEY_EVENT_RECORD& key) {
			return (key.dwControlKeyState & kCtrlBits) != 0;
		}

		static bool altHeld(const KEY_EVENT_RECORD& key) {
			return (key.dwControlKeyState & kAltBits) != 0;
		}

		static bool shiftHeld(const KEY_EVENT_RECORD& key) {
			return (key.dwControlKeyState & SHIFT_PRESSED) != 0;
		}

		// Windows reports AltGr as Ctrl+Alt, so Ctrl+Alt with a character
		// is that character from the layout's third level (a Czech @ or {),
		// not a chord.
		static bool typesLayoutCharacter(const KEY_EVENT_RECORD& key) {
			return ctrlHeld(key) && altHeld(key) && key.uChar.UnicodeChar >= 0x20;
		}

		// AltGr on a key the layout gives nothing for.
		static bool isEmptyAltGr(const KEY_EVENT_RECORD& key) {
			const DWORD state = key.dwControlKeyState;
			return (state & RIGHT_ALT_PRESSED) != 0 && (state & LEFT_CTRL_PRESSED) != 0
			    && key.uChar.UnicodeChar == 0;
		}

		static bool isInterruptKey(const KEY_EVENT_RECORD& key) {
			if (typesLayoutCharacter(key)) return false;

			const bool ctrl_c = ctrlHeld(key) && !altHeld(key) && key.wVirtualKeyCode == 'C';
			return key.uChar.UnicodeChar == 0x03 || ctrl_c;
		}

		static int modifierCode(const KEY_EVENT_RECORD& key) {
			return 1 + (altHeld(key) ? 2 : 0) + (ctrlHeld(key) ? 4 : 0);
		}

		static std::string cursorKey(char final_byte, int modifier) {
			if (modifier == 1) return std::string("\x1b[") + final_byte;

			return "\x1b[1;" + std::to_string(modifier) + final_byte;
		}

		static std::string tildeKey(int number, int modifier) {
			const std::string code = "\x1b[" + std::to_string(number);
			if (modifier == 1) return code + "~";

			return code + ";" + std::to_string(modifier) + "~";
		}

		// Ctrl+Backspace erases a word as Alt+Backspace does; Shift alone
		// does not change a cursor key.
		static std::string editingKey(const KEY_EVENT_RECORD& key) {
			const bool meta = altHeld(key);
			switch (key.wVirtualKeyCode) {
			case VK_BACK:   return meta || ctrlHeld(key) ? "\x1b\x7f" : "\x7f";
			case VK_TAB:    return shiftHeld(key) ? "\x1b[Z" : (meta ? "\x1b\t" : "\t");
			case VK_RETURN: return meta ? "\x1b\r" : "\r";
			case VK_ESCAPE: return kMetaPrefix;
			default:        return {};
			}
		}

		// The keys with no character of their own, spelled as xterm sends them.
		static std::string specialKey(const KEY_EVENT_RECORD& key) {
			const int modifier = modifierCode(key);
			switch (key.wVirtualKeyCode) {
			case VK_UP:     return cursorKey('A', modifier);
			case VK_DOWN:   return cursorKey('B', modifier);
			case VK_RIGHT:  return cursorKey('C', modifier);
			case VK_LEFT:   return cursorKey('D', modifier);
			case VK_HOME:   return cursorKey('H', modifier);
			case VK_END:    return cursorKey('F', modifier);
			case VK_INSERT: return tildeKey(2, modifier);
			case VK_DELETE: return tildeKey(3, modifier);
			case VK_PRIOR:  return tildeKey(5, modifier);
			case VK_NEXT:   return tildeKey(6, modifier);
			default:        return editingKey(key);
			}
		}

		// Ctrl with a key Windows gives no character for.
		static std::string controlCharacter(const KEY_EVENT_RECORD& key) {
			const WORD vk = key.wVirtualKeyCode;
			if (vk >= 'A' && vk <= 'Z') return std::string(1, static_cast<char>(vk - 'A' + 1));
			if (vk == VK_SPACE || vk == '2') return std::string(1, '\0');
			if (vk == VK_OEM_MINUS || vk == VK_OEM_2) return "\x1f";

			return {};
		}

	}  // namespace keys_detail

	static bool isPrintableAscii(WCHAR unit) {
		return unit >= 0x20 && unit < 0x7F;
	}

	// Esc and Ctrl-J end the search on the match, as readline's
	// isearch-terminators do; Ctrl-G and Ctrl-C go back to the line the
	// search started from.
	bool LineEditor::revsearchHandleKey(const KEY_EVENT_RECORD& key, SearchState& search) {
		const bool chord = !keys_detail::typesLayoutCharacter(key);
		const bool is_ctrl = keys_detail::ctrlHeld(key) && chord;
		const bool is_meta = keys_detail::altHeld(key) && chord;
		const WORD vk = key.wVirtualKeyCode;
		const WCHAR ch = key.uChar.UnicodeChar;

		if (vk == VK_RETURN && !is_ctrl && !is_meta) { search.submit = true; return false; }
		if (vk == VK_ESCAPE || (is_ctrl && vk == 'J')) return false;
		if (vk == VK_BACK && !is_ctrl && !is_meta) {
			revsearchEraseQueryChar(search);
			return true;
		}

		if (keys_detail::isInterruptKey(key)) interrupted_ = true;
		if (interrupted_ || (is_ctrl && vk == 'G')) { search.cancel = true; return false; }
		if (is_ctrl && vk == 'R') { revsearchStepOlder(search); return true; }
		if (is_ctrl && vk == 'S') { revsearchStepNewer(search); return true; }

		const bool typed = ch >= 0x80 || isPrintableAscii(ch);
		if (is_ctrl || is_meta || !typed) {
			search.redispatch = true;
			return false;
		}

		const std::string text = ch >= 0x80
			? wideCharacter(ch) : std::string(1, static_cast<char>(ch));
		revsearchExtendQuery(search, text);
		return true;
	}

	void LineEditor::runReverseSearch() {
		const std::string saved_prompt = prompt_raw_;
		const std::string saved_visible = prompt_visible_;
		revsearch_origin_ = { buffer_, cursor_ };
		revsearch_active_ = true;

		SearchState search;
		search.match = exec_.history().size();
		revsearchRefresh(search);

		KEY_EVENT_RECORD key{};
		bool looping = true;
		while (looping) {
			if (!nextKeyDown(key)) {
				search.cancel = true;
				break;
			}

			looping = revsearchHandleKey(key, search);
		}

		prompt_raw_ = saved_prompt;
		prompt_visible_ = saved_visible;
		endReverseSearch(search);
		if (search.redispatch) dispatchKeyDown(key);
	}

	// The line is drawn again from the row the search left the cursor on,
	// so a match that wrapped leaves no rows behind.
	void LineEditor::endReverseSearch(const SearchState& search) {
		if (!search.query.empty()) last_search_query_ = search.query;
		if (search.cancel) {
			buffer_ = revsearch_origin_.buffer;
			cursor_ = revsearch_origin_.cursor;
		}

		revsearch_active_ = false;
		redraw();
		if (interrupted_) interrupt();
		if (search.submit) acceptLine();
	}

	static DWORD enterRawInputMode(HANDLE input) {
		DWORD saved = 0;
		::GetConsoleMode(input, &saved);

		DWORD raw_mode = saved;
		raw_mode &= ~ENABLE_LINE_INPUT;
		raw_mode &= ~ENABLE_ECHO_INPUT;
		raw_mode &= ~ENABLE_PROCESSED_INPUT;
		raw_mode &= ~ENABLE_MOUSE_INPUT;
		raw_mode |= ENABLE_WINDOW_INPUT;
		::SetConsoleMode(input, raw_mode);
		return saved;
	}

	static bool bracketedPasteWanted() {
		DWORD mode = 0;
		const bool vt_output = ::GetConsoleMode(::GetStdHandle(STD_OUTPUT_HANDLE), &mode)
			&& (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
		return vt_output && shellKeymap().isOn("enable-bracketed-paste");
	}

	// A terminal in bracketed paste mode wraps what it pastes in
	// \e[200~ ... \e[201~, which runs bracketed-paste-begin.
	void LineEditor::enterRawMode() {
		if (headless_) return;

		cooked_input_mode_ = enterRawInputMode(::GetStdHandle(STD_INPUT_HANDLE));
		refresh_detail::g_reading_keys.store(true);
		if (bracketedPasteWanted()) emit("\x1b[?2004h");
	}

	void LineEditor::leaveRawMode() {
		if (headless_) return;

		if (bracketedPasteWanted()) emit("\x1b[?2004l");
		refresh_detail::g_reading_keys.store(false);
		::SetConsoleMode(::GetStdHandle(STD_INPUT_HANDLE), cooked_input_mode_);
	}

	DWORD LineEditor::pendingInputRecords() const {
		if (headless_) return 0;

		DWORD count = 0;
		if (!::GetNumberOfConsoleInputEvents(::GetStdHandle(STD_INPUT_HANDLE), &count)) return 0;

		return count;
	}

	bool LineEditor::keysWaiting() const {
		return pendingInputRecords() > 0;
	}

	static bool keyDownQueued(HANDLE input) {
		static const DWORD kPeekLimit = 64;

		INPUT_RECORD records[kPeekLimit];
		DWORD count = 0;
		if (!::PeekConsoleInputW(input, records, kPeekLimit, &count)) return false;

		for (DWORD i = 0; i < count; ++i) {
			const INPUT_RECORD& record = records[i];
			if (record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown) return true;
		}

		return false;
	}

	// A console that knows no bracketed paste types a paste as keys, all
	// queued at once. An Enter with more keys already behind it is then a
	// line break in the paste, not the user running the line -- unless
	// the keys were typed ahead while the last command ran.
	bool LineEditor::isPastedLineBreak(const KEY_EVENT_RECORD& key) const {
		if (headless_ || typeahead_records_ > 0) return false;

		const bool plain_enter = key.wVirtualKeyCode == VK_RETURN
			&& !keys_detail::ctrlHeld(key) && !keys_detail::altHeld(key);
		return plain_enter && keyDownQueued(::GetStdHandle(STD_INPUT_HANDLE));
	}

	void LineEditor::requestPromptRefresh() {
		if (!refresh_detail::g_reading_keys.load()) return;

		INPUT_RECORD record{};
		record.EventType = MENU_EVENT;
		record.Event.MenuEvent.dwCommandId = refresh_detail::kRefreshCommand;
		DWORD written = 0;
		::WriteConsoleInputW(::GetStdHandle(STD_INPUT_HANDLE), &record, 1, &written);
	}

	// Lines of a multi-line prompt above its last one, as they wrapped.
	std::size_t LineEditor::leadRows() const {
		return advanceOver(prompt_lead_visible_, prompt_lead_visible_.size(), { 0, 0 },
			screenColumns()).row;
	}

	// The whole prompt is erased and written again, only when it changed.
	void LineEditor::refreshPrompt() {
		if (!prompt_refresher_ || revsearch_active_ || headless_) return;

		const std::string prompt = prompt_refresher_();
		if (prompt == prompt_full_) return;

		std::string out;
		appendCursorUp(out, last_cursor_row_ + leadRows());
		out += kEraseToEnd;
		emit(out);

		setPrompt(prompt);
		showPrompt();
		redraw();
	}

	// Alt-code input (Alt + numpad digits) is delivered as a key-UP event
	// for VK_MENU carrying the resolved Unicode char.
	void LineEditor::handleAltKeyUp(const KEY_EVENT_RECORD& key) {
		if (key.wVirtualKeyCode != VK_MENU) return;
		if (key.uChar.UnicodeChar < 0x20) return;

		const WCHAR unit = key.uChar.UnicodeChar;
		const std::string utf8 = utf16ToUtf8(&unit, 1);
		if (!utf8.empty()) dispatchKeys(utf8);
	}

	// Every key becomes the bytes a terminal would send for it, so the
	// keymap sees Alt-b as ESC b and Up as ESC [ A, as readline does.
	void LineEditor::dispatchKeyDown(const KEY_EVENT_RECORD& key) {
		if (keys_detail::isInterruptKey(key)) {
			interrupt();
			return;
		}

		if (keys_detail::isEmptyAltGr(key)) return;
		if (isPastedLineBreak(key)) {
			typeText("\n");
			return;
		}

		const std::string special = keys_detail::specialKey(key);
		if (!special.empty()) {
			dispatchKeys(special);
			return;
		}

		const WCHAR ch = key.uChar.UnicodeChar;
		std::string text;
		if (ch >= 0x80) text = wideCharacter(ch);
		else if (ch != 0) text = std::string(1, static_cast<char>(ch));
		else if (keys_detail::ctrlHeld(key)) text = keys_detail::controlCharacter(key);
		if (text.empty()) return;

		const bool meta = keys_detail::altHeld(key) && !keys_detail::typesLayoutCharacter(key);
		dispatchKeys(meta ? keys_detail::kMetaPrefix + text : text);
	}

	// vi's `/` and `?`: the text typed after the lead, shown in the
	// prompt's place. False when Esc, Ctrl-C or Ctrl-G gives it up, or
	// Backspace on an empty text.
	bool LineEditor::readMinibuffer(const std::string& lead, std::string& text) {
		const std::string saved_raw = prompt_raw_;
		const std::string saved_visible = prompt_visible_;
		const UndoPoint saved = { buffer_, cursor_ };
		revsearch_active_ = true;
		prompt_raw_ = prompt_visible_ = lead;
		buffer_.clear();
		cursor_ = 0;
		redraw();

		const bool entered = collectMinibuffer();
		text = buffer_;
		prompt_raw_ = saved_raw;
		prompt_visible_ = saved_visible;
		buffer_ = saved.buffer;
		cursor_ = saved.cursor;
		revsearch_active_ = false;
		return entered;
	}

	bool LineEditor::collectMinibuffer() {
		KEY_EVENT_RECORD key{};
		while (nextKeyDown(key)) {
			const WORD vk = key.wVirtualKeyCode;
			const WCHAR ch = key.uChar.UnicodeChar;
			if (vk == VK_RETURN) return true;

			const bool abort_key = vk == VK_ESCAPE || keys_detail::isInterruptKey(key)
				|| (keys_detail::ctrlHeld(key) && vk == 'G');
			if (abort_key || (vk == VK_BACK && buffer_.empty())) return false;

			if (vk == VK_BACK) {
				buffer_.erase(previousClusterStart(buffer_, buffer_.size()));
			} else if (ch >= 0x80 || isPrintableAscii(ch)) {
				buffer_ += ch >= 0x80 ? wideCharacter(ch) : std::string(1, static_cast<char>(ch));
			}

			cursor_ = buffer_.size();
			redraw();
		}

		return false;
	}

	// readline's answers: y, Y or Space say yes; n, N, Rubout, Esc, Ctrl-G
	// or Ctrl-C say no; any other key rings the bell.
	bool LineEditor::askYesNo() {
		KEY_EVENT_RECORD key{};
		while (nextKeyDown(key)) {
			const WCHAR ch = key.uChar.UnicodeChar;
			const WORD vk = key.wVirtualKeyCode;
			if (ch == L'y' || ch == L'Y' || ch == L' ') return true;

			const bool abort_key = keys_detail::isInterruptKey(key) || vk == VK_ESCAPE
				|| (keys_detail::ctrlHeld(key) && vk == 'G');
			if (ch == L'n' || ch == L'N' || vk == VK_BACK || abort_key) return false;

			ding();
		}

		return false;
	}

	// Key-ups only matter for Alt-code input; a resize lays the line out
	// again, and other events are passed over.
	bool LineEditor::nextKeyDown(KEY_EVENT_RECORD& key) {
		INPUT_RECORD record;
		while (nextInputRecord(record)) {
			if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) resizeScreen(record);
			if (refresh_detail::asksForRefresh(record)) refreshPrompt();
			if (record.EventType != KEY_EVENT) continue;

			key = record.Event.KeyEvent;
			if (key.bKeyDown) return true;

			handleAltKeyUp(key);
		}

		return false;
	}

	// While replaying, the events come from the replay queue instead of
	// the console.
	bool LineEditor::nextInputRecord(INPUT_RECORD& record) {
		if (headless_) return takeReplayRecord(record);
		if (redraw_due_ && !keysWaiting()) redraw();

		if (!readInputRecord(::GetStdHandle(STD_INPUT_HANDLE), record)) return false;
		if (typeahead_records_ > 0) --typeahead_records_;
		return true;
	}

	bool LineEditor::takeReplayRecord(INPUT_RECORD& record) {
		if (replay_keys_.empty()) return false;

		record = replay_keys_.front();
		replay_keys_.pop_front();
		return true;
	}

	// As readline after SIGWINCH: the console has rewrapped the line to
	// the new width, so the cursor's row is counted again at that width
	// before the line is drawn over. COLUMNS and LINES follow at once.
	void LineEditor::resizeScreen(const INPUT_RECORD& record) {
		const COORD size = record.Event.WindowBufferSizeEvent.dwSize;
		if (headless_ && size.X > 0) forced_columns_ = static_cast<std::size_t>(size.X);

		const std::size_t width = screenColumns();
		const ScreenPos start = promptEnd(prompt_visible_, width);
		last_cursor_row_ = settled(advanceOver(buffer_, cursor_, start, width), width).row;

		env_.set("COLUMNS", std::to_string(width));
		if (size.Y > 0) env_.set("LINES", std::to_string(consoleLines(size.Y, headless_)));
		redraw();
	}

	bool LineEditor::editUntilDone() {
		while (!done_) {
			KEY_EVENT_RECORD key;
			if (!nextKeyDown(key)) {
				eof_ = true;
				return false;
			}

			dispatchKeyDown(key);
		}

		return true;
	}

	LineStatus LineEditor::readLineRaw(const std::string& prompt, std::string& out) {
		const HANDLE input  = ::GetStdHandle(STD_INPUT_HANDLE);
		const HANDLE output = ::GetStdHandle(STD_OUTPUT_HANDLE);
		if (input == INVALID_HANDLE_VALUE || output == INVALID_HANDLE_VALUE) {
			return readLineCooked(out);
		}

		enterRawMode();
		typeahead_records_ = pendingInputRecords();
		showPrompt();
		if (!buffer_.empty()) redraw();
		editUntilDone();
		leaveRawMode();

		out = accepted_;
		return lineStatus();
	}

	namespace replay_detail {

		struct NamedKey {
			const char* name;
			WORD vk;
			WCHAR ch;
		};

		static const NamedKey kNamedKeys[] = {
			{ "LEFT",  VK_LEFT,   0 },
			{ "RIGHT", VK_RIGHT,  0 },
			{ "UP",    VK_UP,     0 },
			{ "DOWN",  VK_DOWN,   0 },
			{ "HOME",  VK_HOME,   0 },
			{ "END",   VK_END,    0 },
			{ "BS",    VK_BACK,   0x08 },
			{ "DEL",   VK_DELETE, 0 },
			{ "TAB",   VK_TAB,    0x09 },
			{ "ENTER", VK_RETURN, 0x0D },
			{ "ESC",   VK_ESCAPE, 0x1B },
		};

		static KEY_EVENT_RECORD pressOf(WORD vk, WCHAR ch, DWORD modifiers) {
			KEY_EVENT_RECORD key{};
			key.bKeyDown = TRUE;
			key.wRepeatCount = 1;
			key.wVirtualKeyCode = vk;
			key.uChar.UnicodeChar = ch;
			key.dwControlKeyState = modifiers;
			return key;
		}

		static bool namedKey(const std::string& name, DWORD modifiers, KEY_EVENT_RECORD& key) {
			for (const NamedKey& named : kNamedKeys) {
				if (name != named.name) continue;

				key = pressOf(named.vk, named.ch, modifiers);
				return true;
			}

			return false;
		}

		static WORD chordVk(char letter) {
			if (std::isalpha(static_cast<unsigned char>(letter)) != 0) {
				return static_cast<WORD>(std::toupper(static_cast<unsigned char>(letter)));
			}

			if (letter == '_' || letter == '-') return VK_OEM_MINUS;
			if (letter == '/') return VK_OEM_2;
			if (letter == '@') return '2';
			if (letter == ' ') return VK_SPACE;
			if (letter == ']') return VK_OEM_6;

			return 0;
		}

		// What the console puts in uChar for the chord: a control code
		// for Ctrl with a letter, `_` or `]`, nothing for Ctrl+/ or Ctrl+@.
		static WCHAR chordChar(char letter, bool ctrl) {
			if (!ctrl) return static_cast<WCHAR>(static_cast<unsigned char>(letter));
			const auto byte = static_cast<unsigned char>(letter);
			if (std::isalpha(byte) != 0) return static_cast<WCHAR>(std::toupper(byte) - 'A' + 1);
			if (letter == '_') return 0x1F;
			if (letter == ']') return 0x1D;
			if (letter == '/' || letter == '@' || letter == ' ') return 0;

			return static_cast<WCHAR>(letter);
		}

		static std::wstring widen(const std::string& text) {
			const int count = static_cast<int>(text.size());
			const int length = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), count, nullptr, 0);
			if (length <= 0) return {};

			std::wstring wide(static_cast<std::size_t>(length), L'\0');
			::MultiByteToWideChar(CP_UTF8, 0, text.data(), count, wide.data(), length);
			return wide;
		}

		static DWORD takeModifiers(std::string& name) {
			DWORD modifiers = 0;
			for (;;) {
				if (name.size() > 2 && name.compare(0, 2, "C-") == 0) {
					modifiers |= LEFT_CTRL_PRESSED;
				} else if (name.size() > 2 && name.compare(0, 2, "M-") == 0) {
					modifiers |= LEFT_ALT_PRESSED;
				} else {
					return modifiers;
				}

				name.erase(0, 2);
			}
		}

		// <AltGr-@>: the character as a Czech keyboard types it, with the
		// Ctrl and right Alt bits Windows reports for AltGr.
		static bool altGrKey(const std::string& name, KEY_EVENT_RECORD& key) {
			const std::string prefix = "AltGr-";
			if (name.size() <= prefix.size()) return false;
			if (name.compare(0, prefix.size(), prefix) != 0) return false;

			const std::wstring wide = widen(name.substr(prefix.size()));
			if (wide.size() != 1) return false;

			key = pressOf(0, wide[0], LEFT_CTRL_PRESSED | RIGHT_ALT_PRESSED);
			return true;
		}

		static bool chordKey(std::string name, KEY_EVENT_RECORD& key) {
			const DWORD modifiers = takeModifiers(name);
			if (modifiers == 0) return false;
			if (namedKey(name, modifiers, key)) return true;
			if (name.size() != 1) return false;

			const bool ctrl = (modifiers & LEFT_CTRL_PRESSED) != 0;
			key = pressOf(chordVk(name[0]), chordChar(name[0], ctrl), modifiers);
			return true;
		}

		static bool keyFor(const std::string& token, KEY_EVENT_RECORD& key) {
			if (token.size() < 3 || token.front() != '<' || token.back() != '>') return false;

			const std::string name = token.substr(1, token.size() - 2);
			return namedKey(name, 0, key) || altGrKey(name, key) || chordKey(name, key);
		}

		static INPUT_RECORD keyRecord(const KEY_EVENT_RECORD& key) {
			INPUT_RECORD record{};
			record.EventType = KEY_EVENT;
			record.Event.KeyEvent = key;
			return record;
		}

		// <RESIZE:COLUMNS>: the screen changes to that width.
		static bool resizeFor(const std::string& token, INPUT_RECORD& record) {
			const std::string prefix = "<RESIZE:";
			if (token.size() <= prefix.size() + 1 || token.compare(0, prefix.size(), prefix) != 0) {
				return false;
			}

			long long columns = 0;
			const std::size_t digits = token.size() - prefix.size() - 1;
			const std::string number = token.substr(prefix.size(), digits);
			if (!parseLL(number, columns) || columns <= 0 || columns > SHRT_MAX) return false;

			record = INPUT_RECORD{};
			record.EventType = WINDOW_BUFFER_SIZE_EVENT;
			record.Event.WindowBufferSizeEvent.dwSize = { static_cast<SHORT>(columns), 0 };
			return true;
		}

		static void queueText(const std::string& text, std::deque<INPUT_RECORD>& queue) {
			for (const wchar_t unit : widen(text)) queue.push_back(keyRecord(pressOf(0, unit, 0)));
		}

		static void queueToken(const std::string& token, std::deque<INPUT_RECORD>& queue) {
			KEY_EVENT_RECORD key{};
			INPUT_RECORD resize{};
			if (keyFor(token, key)) {
				queue.push_back(keyRecord(key));
			} else if (resizeFor(token, resize)) {
				queue.push_back(resize);
			} else {
				queueText(token, queue);
			}
		}

	}  // namespace replay_detail

	static std::string spelledOut(const std::string& output) {
		std::string text;
		for (const char letter : output) {
			if (letter == '\x1b') text += "\\e";
			else if (letter == '\r') text += "\\r";
			else if (letter == '\n') text += "\\n\n";
			else if (letter == '\a') text += "\\a";
			else text.push_back(letter);
		}

		return text;
	}

	std::string LineEditor::replayKeys(const std::vector<std::string>& keys,
	                                   const ReplayOptions& options) {
		resetLine(options.prompt);
		replay_keys_.clear();
		for (const std::string& token : keys) replay_detail::queueToken(token, replay_keys_);

		forced_columns_ = options.columns;
		capture_output_ = options.show_output;
		captured_output_.clear();
		headless_ = true;
		showPrompt();
		const bool finished = editUntilDone();
		headless_ = false;
		replay_keys_.clear();

		std::string report = describeReplay(finished);
		if (options.columns != 0 && !finished) report += " " + describeCursorCell();
		if (operate_next_ < exec_.history().size()) {
			report += " then [" + exec_.history()[operate_next_] + "]";
		}

		if (options.show_output) report += "\n" + spelledOut(captured_output_);

		forced_columns_ = 0;
		capture_output_ = false;
		return report;
	}

#else  /* !_WIN32 */

	LineStatus LineEditor::readLineRaw(const std::string& prompt, std::string& out) {
		std::fputs(prompt.c_str(), stdout);
		std::fflush(stdout);
		return readLineCooked(out);
	}

	void LineEditor::emit(const std::string&) {}
	void LineEditor::redraw() {}
	void LineEditor::redrawWhenIdle() {}
	bool LineEditor::readMinibuffer(const std::string&, std::string&) { return false; }
	bool LineEditor::collectMinibuffer() { return false; }
	bool LineEditor::askYesNo() { return true; }
	std::string LineEditor::readBracketedPaste() { return {}; }
	void LineEditor::moveToLineEnd() {}
	void LineEditor::showPrompt() {}
	void LineEditor::appendLineText(std::string&) const {}
	bool LineEditor::editUntilDone() { return false; }
	void LineEditor::leaveRawMode() {}
	void LineEditor::enterRawMode() {}
	void LineEditor::requestPromptRefresh() {}
	std::size_t LineEditor::leadRows() const { return 0; }
	void LineEditor::refreshPrompt() {}
	void LineEditor::insertClipboardText() {}
	void LineEditor::runReverseSearch() {}
	void LineEditor::revsearchRefresh(const SearchState&) {}
	void LineEditor::refreshSuggestion() {}
	bool LineEditor::acceptInlineSuggestion() { return false; }
	std::size_t LineEditor::screenColumns() const { return 0; }
	std::string LineEditor::describeCursorCell() const { return {}; }
	std::string LineEditor::replayKeys(const std::vector<std::string>&, const ReplayOptions&) {
		return {};
	}

#endif /* _WIN32 */

	// -c COLUMNS, -p PROMPT and -o come before the keys; `--` ends them.
	static std::size_t takeReplayOptions(const std::vector<std::string>& args,
	                                     LineEditor::ReplayOptions& options) {
		std::size_t i = 0;
		while (i < args.size()) {
			const std::string& arg = args[i];
			if (arg == "--") return i + 1;

			const bool valued = (arg == "-c" || arg == "-p") && i + 1 < args.size();
			if (!valued && arg != "-o") return i;

			long long columns = 0;
			if (arg == "-c" && parseLL(args[i + 1], columns) && columns > 0) {
				options.columns = static_cast<std::size_t>(columns);
			}

			if (arg == "-p") options.prompt = args[i + 1];
			if (arg == "-o") options.show_output = true;
			i += valued ? 2 : 1;
		}

		return i;
	}

	int lineEditBuiltin(Executor& exec, const std::vector<std::string>& args) {
		LineEditor::ReplayOptions options;
		const std::size_t first = takeReplayOptions(args, options);
		const std::vector<std::string> keys(args.begin() + static_cast<std::ptrdiff_t>(first),
		                                    args.end());

		LineEditor editor(exec.env(), exec);
		std::printf("%s\n", editor.replayKeys(keys, options).c_str());
		return 0;
	}

}  // namespace wbsh

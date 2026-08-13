#pragma once

/**
 * @file lineedit.h
 * @brief Interactive line editor with history, kill ring, and completion.
 *
 * Reads one line of input at a time. On a real TTY console it runs
 * the raw editor loop with cursor movement, backspace, persistent
 * history (up/down), Tab completion (filename, command, and
 * per-tool), kill-to-end / kill-to-start, clipboard paste, and
 * Unicode-aware redraw. On a non-TTY stdin (piped scripts, tests) it
 * falls back to a line-buffered `fgetc` loop so existing scripted
 * usage works unchanged.
 */

#include "environment.h"
#include "executor.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#include <cstddef>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace wbsh {

	/**
	 * @brief Walk up from the cwd to the enclosing repository's git dir.
	 *
	 * Resolves the `.git`-file indirection used by linked worktrees and
	 * submodules. Returns an empty path when not inside a repository.
	 * Used by the prompt's `\g` expansion and the git Tab completions.
	 */
	std::filesystem::path findGitDir();

	/**
	 * @brief Substring search over shell history, used by reverse-i-search.
	 *
	 * Scans `history` either backward from `start_index` toward 0 (when
	 * `forward` is false) or forward from `start_index` toward the end,
	 * inclusive at the starting position, and returns the index of the
	 * first entry that contains `query` as a substring.
	 *
	 * @param history     History entries, oldest first.
	 * @param query       Substring to look for. An empty query never matches.
	 * @param start_index Index to begin scanning from, inclusive. Values
	 *                    past the end are clamped to the last entry.
	 * @param forward     true to scan toward newer entries, false toward older.
	 *
	 * @return Index of the matching entry, or `history.size()` when no
	 *         entry matches (also when `history` is empty or `query` is empty).
	 */
	std::size_t findReverseSearchMatch(const std::vector<std::string>& history,
	                                   const std::string& query,
	                                   std::size_t start_index,
	                                   bool forward);

	/**
	 * @brief Compute the ghost-text inline prediction for a typed prefix.
	 *
	 * Scans @p history newest-first for the first entry that begins with
	 * @p prefix and is strictly longer than it; returns just the trailing
	 * portion that the editor should render in dim text past the cursor
	 * (PowerShell-style inline prediction). Entries whose last execution
	 * exited non-zero (as reported via @p history_status) are skipped, so
	 * typos and previously-failed commands never get suggested. Empty
	 * prefix never predicts.
	 *
	 * @param history        Shell history, oldest first (same vector
	 *                       layout as Executor::history()).
	 * @param history_status Parallel exit-status vector (0 = OK). Indices
	 *                       past the end are treated as 0; pass an empty
	 *                       vector to disable filtering entirely.
	 * @param prefix         The buffer typed so far. An empty string
	 *                       returns "".
	 *
	 * @return The suggested suffix to display after the cursor, or an
	 *         empty string when no entry matches or @p prefix is empty.
	 */
	std::string findInlinePrediction(const std::vector<std::string>& history,
	                                 const std::vector<int>& history_status,
	                                 const std::string& prefix);

	/**
	 * @brief Interactive line editor.
	 *
	 * Stateful object — one instance per REPL session. Keeps the
	 * current edit buffer, cursor position, history scroll position,
	 * and Tab-state across readLine() calls.
	 */
	class LineEditor {
	public:
		LineEditor(Environment& env, Executor& exec);

		/**
		 * @brief Read one line from the user.
		 *
		 * @param prompt  Prompt string to display (already expanded).
		 * @param[out] out On success, the typed line (without the
		 *                 terminating newline).
		 *
		 * @return true on Enter (the line is also appended to the
		 *         executor's history if non-empty and distinct from
		 *         the previous entry). false on EOF (Ctrl-D on an
		 *         empty line, or stdin closed).
		 */
		bool readLine(const std::string& prompt, std::string& out);

	private:
		bool readLineRaw(const std::string& prompt, std::string& out);
		bool readLineCooked(std::string& out);

#ifdef _WIN32
		// Helpers used inside readLineRaw — declared here only to keep the
		// .cpp's per-key dispatch under 60 lines. KEY_EVENT_RECORD comes
		// from <windows.h>, which the .cpp pulls in unconditionally.
		void handleAltKeyUp(const ::KEY_EVENT_RECORD& k);
		bool handleCtrlKey(const ::KEY_EVENT_RECORD& k,
		                   const std::string& prompt,
		                   std::string& out, bool& done, bool& eof);
		bool handleNavigationKey(const ::KEY_EVENT_RECORD& k,
		                         std::string& out, bool& done, bool& was_tab);
		void insertReceivedChar(wchar_t ch);
		// One keystroke inside the reverse-search modal. Returns true to
		// keep looping, false to exit. Sets `cancel` on Esc / Ctrl-G /
		// Ctrl-C (restore the pre-search buffer) and `submit` on Enter.
		bool revsearchHandleKey(const ::KEY_EVENT_RECORD& k,
		                        std::string& query, std::size_t& match_index,
		                        bool& cancel, bool& submit);
		void revsearchStepOlder(const std::string& query, std::size_t& match_index);
		void revsearchStepNewer(const std::string& query, std::size_t& match_index);
#endif

		void redraw();
		void emit(const std::string& s);
		void handleEnter(std::string& out, bool& done);
		void handleBackspace();
		void handleDelete();
		void handleTab();
		void handleHistoryUp();
		void handleHistoryDown();
		void handleKillToEnd();
		void handleKillToStart();
		void handleKillWordBack();
		void handleClearScreen();
		void insertChars(const std::string& s);

		// PowerShell-style inline prediction. refreshSuggestion() is
		// called at the top of redraw() and populates suggestion_ from
		// history when the cursor sits at end-of-buffer and we aren't in
		// the reverse-search modal. handleRightArrow() folds in
		// suggestion-accept at end-of-line: ordinary right-arrow moves
		// one char forward, but when the cursor is already at the end
		// and a suggestion is shown, it absorbs the suggestion instead.
		void refreshSuggestion();
		bool acceptInlineSuggestion();
		void handleRightArrow();

		// Ctrl-R: enter the reverse-incremental-search modal loop. The
		// search prompt replaces the normal prompt; printable chars extend
		// the query, Ctrl-R/Ctrl-S iterate matches, Enter accepts and
		// submits, Esc/Ctrl-G cancels, any other editing key accepts the
		// match and falls through to normal editing. Windows-only; a no-op
		// stub on other platforms.
		void runReverseSearch(std::string& out, bool& done, bool& eof);
		void revsearchRefresh(const std::string& query, std::size_t match_index);

		// Ctrl-V: pull CF_UNICODETEXT off the clipboard, strip newlines,
		// insert at cursor. Windows-only; a no-op stub on other platforms.
		void pasteFromClipboard();
		// Wide-char input >= 0x80: emit as UTF-8, fetching the low
		// surrogate from the console input queue when `ch` is a high
		// surrogate. Windows-only; a no-op stub on other platforms.
		void insertWideCharFromConsole(wchar_t ch);

		struct Tok {
			std::size_t start;   // byte offset in buffer_
			std::size_t end;
			bool first;          // true = command-position word
		};
		Tok currentToken() const;
		std::vector<std::string> completionsFor(const std::string& prefix,
		                                        bool command_pos);
		std::vector<std::string> commandCompletions(const std::string& prefix);
		// PATH-derived executable names (extension-stripped), memoized
		// across Tab presses and rebuilt only when PATH itself changes —
		// consecutive Tabs on the same prefix no longer re-list every
		// PATH directory from disk.
		const std::set<std::string>& pathCommandNames();
		std::vector<std::string> pathCompletions(const std::string& prefix);

		// Per-tool argument completion. Each looks at the token sequence up
		// to (but not including) the current word; if the head is the tool
		// name, it returns matches, otherwise empty (caller falls through to
		// path completion).
		std::vector<std::string> toolCompletions(const std::string& prefix,
		                                         const Tok& tok);
		std::vector<std::string> gitCompletions(const std::string& prefix,
		                                        const std::vector<std::string>& prev);
		std::vector<std::string> dockerCompletions(const std::string& prefix,
		                                           const std::vector<std::string>& prev);
		std::vector<std::string> npmCompletions(const std::string& prefix,
		                                        const std::vector<std::string>& prev);
		std::vector<std::string> cargoCompletions(const std::string& prefix,
		                                          const std::vector<std::string>& prev);
		std::vector<std::string> kubectlCompletions(const std::string& prefix,
		                                            const std::vector<std::string>& prev);
		std::vector<std::string> prevTokensBefore(const Tok& tok) const;
		std::vector<std::string> gitBranches();
		std::vector<std::string> gitRemotes();
		void applyCompletion(const Tok& tok,
		                     const std::vector<std::string>& matches);
		void printMatches(const std::vector<std::string>& matches);
		std::string longestCommonPrefix(const std::vector<std::string>& v);

		Environment& env_;
		Executor& exec_;

		std::size_t history_pos_ = 0;        // 0 = current edit, 1 = last entry, ...
		std::string saved_partial_;          // buffer at the moment user started scrolling

		std::string buffer_;
		std::size_t cursor_ = 0;
		std::string prompt_raw_;
		std::size_t prompt_visible_len_ = 0;

		// Row offset of the cursor below the prompt's first row, as of the
		// most recent redraw. When the buffer wraps, redraw() must walk
		// back up this many rows before re-emitting, otherwise it leaves
		// stale wrapped lines above and re-prints the prompt on each row.
		std::size_t last_cursor_row_ = 0;

		// Tab-tracking: two consecutive tabs at the same word with no
		// progress means "show me all matches".
		bool last_was_tab_ = false;
		std::string last_tab_word_;

		// Current ghost-text inline prediction (the tail past the
		// cursor), rendered in dim style by redraw(). Empty when no
		// prediction applies — including while the reverse-search
		// modal owns the prompt, gated by revsearch_active_.
		std::string suggestion_;
		bool revsearch_active_ = false;
		// History index behind the current suggestion_, or history.size()
		// (via SIZE_MAX, clamped in refreshSuggestion's bounds check) when
		// there is none. Lets refreshSuggestion() extend/shrink the same
		// match across consecutive keystrokes without rescanning history.
		std::size_t suggestion_cache_idx_ = static_cast<std::size_t>(-1);

		// Cache backing pathCommandNames(): the set of PATH-visible
		// executable basenames, plus the PATH string it was built from.
		std::set<std::string> path_command_cache_;
		std::string path_command_cache_path_;
	};

}  // namespace wbsh

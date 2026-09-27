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

	// Walks up from the cwd to the enclosing repository's git dir, following
	// the `.git`-file indirection of linked worktrees and submodules. Empty
	// when not inside a repository.
	std::filesystem::path findGitDir();

	// Substring search over shell history for reverse-i-search. Scans from
	// `start_index` (inclusive; clamped to the last entry) toward older
	// entries, or toward newer ones when `forward`. Returns the index of
	// the first entry containing `query`, or `history.size()` when nothing
	// matches -- also for an empty history or an empty query.
	std::size_t findReverseSearchMatch(const std::vector<std::string>& history,
	                                   const std::string& query,
	                                   std::size_t start_index,
	                                   bool forward);

	// Ghost-text inline prediction for a typed prefix: the tail of the
	// newest history entry that begins with `prefix` and is strictly longer
	// than it. Entries whose last run exited non-zero (per `history_status`,
	// parallel to `history`; indices past its end count as 0) are skipped so
	// typos and failed commands are never suggested. Empty for an empty
	// prefix or no match.
	std::string findInlinePrediction(const std::vector<std::string>& history,
	                                 const std::vector<int>& history_status,
	                                 const std::string& prefix);

	// One instance per REPL session: keeps the edit buffer, cursor, history
	// scroll position, and Tab state across readLine() calls.
	class LineEditor {
	public:
		LineEditor(Environment& env, Executor& exec);

		// Returns true on Enter with the typed line in `out` (without the
		// newline), false on EOF (Ctrl-D on an empty line, or stdin closed).
		bool readLine(const std::string& prompt, std::string& out);

	private:
		bool readLineRaw(const std::string& prompt, std::string& out);
		bool readLineCooked(std::string& out);

#ifdef _WIN32
		void handleAltKeyUp(const ::KEY_EVENT_RECORD& key);
		void dispatchKeyDown(const ::KEY_EVENT_RECORD& key,
		                     std::string& out, bool& done, bool& eof);
		bool handleCtrlKey(const ::KEY_EVENT_RECORD& key,
		                   std::string& out, bool& done, bool& eof);
		bool handleNavigationKey(const ::KEY_EVENT_RECORD& key,
		                         std::string& out, bool& done, bool& was_tab);
		void insertReceivedChar(wchar_t ch);
		void handleInterrupt();
		void handleEofOrDelete(bool& done, bool& eof);
		void moveCursorBack();
		void moveCursorForward();
		void moveCursorHome();
		void moveCursorEnd();

		// One keystroke inside the reverse-search modal. Returns true to
		// keep looping, false to exit. Sets `cancel` on Esc / Ctrl-G /
		// Ctrl-C (restore the pre-search buffer) and `submit` on Enter.
		bool revsearchHandleKey(const ::KEY_EVENT_RECORD& key,
		                        std::string& query, std::size_t& match_index,
		                        bool& cancel, bool& submit);
		void revsearchStepOlder(const std::string& query, std::size_t& match_index);
		void revsearchStepNewer(const std::string& query, std::size_t& match_index);
		void revsearchEraseQueryChar(std::string& query, std::size_t& match_index);
		void revsearchExtendQuery(std::string& query, char letter, std::size_t& match_index);
#endif

		void redraw();
		void emit(const std::string& text);
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
		void insertChars(const std::string& text);

		// PowerShell-style inline prediction: refreshSuggestion() fills
		// suggestion_ from history when the cursor sits at end-of-buffer
		// outside the reverse-search modal. Right-arrow at end-of-line
		// absorbs the suggestion instead of moving.
		void refreshSuggestion();
		bool acceptInlineSuggestion();
		void handleRightArrow();

		// Ctrl-R modal: the search prompt replaces the normal one; printable
		// chars extend the query, Ctrl-R/Ctrl-S iterate matches, Enter
		// accepts and submits, Esc/Ctrl-G cancels, any other editing key
		// accepts the match and returns to normal editing. Windows-only;
		// a no-op stub elsewhere.
		void runReverseSearch(std::string& out, bool& done, bool& eof);
		void revsearchRefresh(const std::string& query, std::size_t match_index);

		// Ctrl-V: CF_UNICODETEXT with newlines stripped, inserted at the
		// cursor. Windows-only; a no-op stub elsewhere.
		void pasteFromClipboard();
		// Wide-char input >= 0x80: emitted as UTF-8, fetching the low
		// surrogate from the console input queue when `ch` is a high
		// surrogate. Windows-only; a no-op stub elsewhere.
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
		// across Tab presses and rebuilt only when PATH itself changes --
		// consecutive Tabs on the same prefix never re-list every PATH
		// directory from disk.
		const std::set<std::string>& pathCommandNames();
		std::vector<std::string> pathCompletions(const std::string& prefix);

		// Per-tool argument completion. Each looks at the token sequence up
		// to (but not including) the current word; if the head is the tool
		// name, it returns matches, otherwise empty (caller falls through to
		// path completion).
		std::vector<std::string> toolCompletions(const std::string& prefix,
		                                         const Tok& tok);
		std::vector<std::string> specCompletions(const Executor::CompletionSpec& spec,
		                                         const std::string& prefix, const Tok& tok,
		                                         const std::vector<std::string>& prev);
		void appendFunctionCompletions(const std::string& function,
		                               const std::string& prefix, const Tok& tok,
		                               const std::vector<std::string>& prev,
		                               std::vector<std::string>& matches);
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
		std::string longestCommonPrefix(const std::vector<std::string>& words);

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
		// prediction applies -- including while the reverse-search
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

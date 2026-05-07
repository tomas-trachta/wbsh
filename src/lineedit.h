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

#include <string>
#include <vector>

namespace wbsh {

	/**
	 * @brief Interactive line editor.
	 *
	 * Stateful object — one instance per REPL session. Keeps the
	 * current edit buffer, cursor position, history scroll position,
	 * and Tab-state across readLine() calls.
	 */
	class LineEditor {
	public:
		/// Construct over a borrowed Environment and Executor.
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
		// Raw-TTY path (Windows console).
		bool readLineRaw(const std::string& prompt, std::string& out);
		// Cooked / piped fallback.
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
#endif

		// ---- Helpers used by the raw path ----
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

		// Ctrl-V: pull CF_UNICODETEXT off the clipboard, strip newlines,
		// insert at cursor. Windows-only; a no-op stub on other platforms.
		void pasteFromClipboard();
		// Wide-char input >= 0x80: emit as UTF-8, fetching the low
		// surrogate from the console input queue when `ch` is a high
		// surrogate. Windows-only; a no-op stub on other platforms.
		void insertWideCharFromConsole(wchar_t ch);

		// ---- Completion ----
		struct Tok {
			std::size_t start;   // byte offset in buffer_
			std::size_t end;
			bool first;          // true = command-position word
		};
		Tok currentToken() const;
		std::vector<std::string> completionsFor(const std::string& prefix,
		                                        bool command_pos);
		std::vector<std::string> commandCompletions(const std::string& prefix);
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

		// ---- State ----
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
	};

}  // namespace wbsh

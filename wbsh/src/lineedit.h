#pragma once

/**
 * @file lineedit.h
 * @brief Interactive line editor with readline's emacs keys, history, and completion.
 *
 * Reads one line of input at a time. On a real TTY console it runs the
 * raw editor loop: every key is turned into the bytes a terminal sends
 * for it and looked up in the shell's keymap (keymap.h), whose bindings
 * name readline commands -- cursor and word motion, the kill ring and
 * yank, undo, transposition and case changes, history and reverse
 * search, Tab completion, yank-last-arg, edit-and-execute -- or type a
 * macro, or run a shell command. On a non-TTY stdin (piped scripts,
 * tests) it falls back to a line-buffered `fgetc` loop so existing
 * scripted usage works unchanged.
 */

#include "environment.h"
#include "executor.h"
#include "keymap.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#include <cstddef>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace wbsh {

	// Walks up from the cwd to the enclosing repository's git dir, following
	// the `.git`-file indirection of linked worktrees and submodules. Empty
	// when not inside a repository.
	std::filesystem::path findGitDir();

	// The directory a linked worktree shares with its main repository
	// (refs, packed-refs, config), named by its git dir's `commondir`
	// file; `git_dir` itself when there is none.
	std::filesystem::path findGitCommonDir(const std::filesystem::path& git_dir);

	// Refs kept in a reftable instead of files: only git can list them.
	bool usesRefTable(const std::filesystem::path& common_dir);

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

	// `__lineedit KEY...`: replays keys through a fresh LineEditor. A KEY is
	// literal text or, in angle brackets, a named key -- LEFT RIGHT UP DOWN
	// HOME END BS DEL TAB ENTER ESC -- or a character, either with C- / M- /
	// C-M- in front (<C-LEFT>, <M-.>, <C-_>), or <AltGr-x> for a character
	// typed with AltGr; <RESIZE:N> makes the screen N columns wide. Before
	// the keys, `-p PROMPT` sets the prompt,
	// `-c COLUMNS` a screen width (the cursor's cell is then reported as
	// @row:col), and `-o` prints what the editor wrote. A line run with
	// operate-and-get-next is followed by `then [ENTRY]`, the history
	// entry the next prompt would start with.
	int lineEditBuiltin(Executor& exec, const std::vector<std::string>& args);

	// A place reverse-i-search found its query: the history entry and
	// the byte offset in it.
	struct SearchHit {
		std::size_t entry;
		std::size_t offset;
	};

	enum class LineStatus {
		kLine,          // Enter: the typed line is in `out`
		kEof,           // Ctrl-D on an empty line, or stdin closed
		kInterrupted,   // Ctrl-C: the line is abandoned
	};

	// One instance per REPL session: keeps the edit buffer, cursor, history
	// scroll position, kill ring and Tab state across readLine() calls.
	class LineEditor {
	public:
		LineEditor(Environment& env, Executor& exec);

		// The typed line goes to `out` without its newline.
		LineStatus readLine(const std::string& prompt, std::string& out);

		// How to build the prompt again when requestPromptRefresh() asks.
		void setPromptRefresher(std::function<std::string()> refresher);

		// Callable from any thread: an editor waiting for a key at a prompt
		// builds that prompt again and redraws it if it changed.
		static void requestPromptRefresh();

		struct ReplayOptions {
			std::string prompt;
			std::size_t columns = 0;    // a screen this wide instead of the console's
			bool show_output = false;   // also what the editor wrote, escapes spelled out
		};

		// Test hook behind `__lineedit`: types `keys` into an empty line
		// with the console left alone, and describes the line it ends with
		// -- with `columns`, also the screen cell the cursor is left in.
		std::string replayKeys(const std::vector<std::string>& keys, const ReplayOptions& options);

	private:
		struct UndoPoint {
			std::string buffer;
			std::size_t cursor;
		};

		using EditCommand = void (LineEditor::*)();

		struct SearchState {
			std::string query;
			std::size_t match = 0;       // history index shown; its size for the starting line
			std::size_t offset = 0;      // where in that entry the query was found
			bool cancel = false;         // back to the line the search started from
			bool submit = false;         // Enter: the shown line runs
			bool redispatch = false;     // another editing key runs on the shown line
		};

		void resetLine(const std::string& prompt);
		void setPrompt(const std::string& prompt);
		LineStatus readLineRaw(const std::string& prompt, std::string& out);
		LineStatus readLineCooked(std::string& out);
		LineStatus lineStatus() const;
		std::string describeReplay(bool finished) const;

#ifdef _WIN32
		void handleAltKeyUp(const ::KEY_EVENT_RECORD& key);
		void dispatchKeyDown(const ::KEY_EVENT_RECORD& key);
		std::string wideCharacter(wchar_t ch);
		bool nextKeyDown(::KEY_EVENT_RECORD& key);
		bool nextInputRecord(::INPUT_RECORD& record);
		bool takeReplayRecord(::INPUT_RECORD& record);
		void resizeScreen(const ::INPUT_RECORD& record);
		::DWORD pendingInputRecords() const;
		bool keysWaiting() const;
		bool isPastedLineBreak(const ::KEY_EVENT_RECORD& key) const;
		std::string pastedKeyText(const ::KEY_EVENT_RECORD& key);
		std::string replayCharacter(wchar_t ch);

		// One keystroke inside the reverse-search modal; false ends it.
		bool revsearchHandleKey(const ::KEY_EVENT_RECORD& key, SearchState& search);
		void revsearchStepOlder(SearchState& search);
		void revsearchStepNewer(SearchState& search);
		void revsearchEraseQueryChar(SearchState& search);
		void revsearchExtendQuery(SearchState& search, const std::string& text);
		void revsearchShowMatch(const SearchState& search);
		void revsearchMoveTo(SearchState& search, bool found, const SearchHit& hit);
		void endReverseSearch(const SearchState& search);
#endif

		// False when the input ran out before a key finished the line.
		bool editUntilDone();
		void leaveRawMode();
		void enterRawMode();

		// Keys arrive as the bytes a terminal sends; they collect until
		// they name a binding, or cannot start one any more.
		void dispatchKeys(const std::string& keys);
		void runBinding(const KeyBinding& binding);
		void runCommand(const std::string& name);
		EditCommand commandNamed(const std::string& name) const;
		EditCommand basicCommandNamed(const std::string& name) const;
		EditCommand readlineCommandNamed(const std::string& name) const;
		EditCommand viCommandNamed(const std::string& name) const;
		void lookUpKeys(const std::string& keys);
		void runEditCommand(const std::string& name, EditCommand command);
		void typeMacro(const std::string& text);
		void runShellBinding(const std::string& command);
		void recordUndo(const UndoPoint& before, const std::string& name);
		void ding();

		void redraw();
		void redrawWhenIdle();
		void typeText(const std::string& text);
		std::string readBracketedPaste();
		void emit(const std::string& text);
		std::size_t screenColumns() const;
		std::string describeCursorCell() const;
		void moveToLineEnd();
		void showPrompt();
		void refreshPrompt();
		std::size_t leadRows() const;
		void appendLineText(std::string& out) const;
		void insertChars(const std::string& text);
		void killRange(std::size_t start, std::size_t end, bool prepend);
		void insertYanked(const std::string& text, std::size_t& start, std::size_t& end);
		std::size_t wordStartBefore(std::size_t pos) const;
		std::size_t wordEndAfter(std::size_t pos) const;
		void changeWordCase(int mode);
		void showHistoryEntry(std::size_t back);
		void interrupt();

		// The commands keys are bound to, named as in readline.
		void abortKeys();
		void acceptLine();
		void backwardChar();
		void backwardDeleteChar();
		void backwardKillWord();
		void backwardWord();
		void beginningOfHistory();
		void beginningOfLine();
		void bracketedPasteBegin();
		void capitalizeWord();
		void clearScreen();
		void complete();
		void deleteChar();
		void deleteHorizontalSpace();
		void downcaseWord();
		void editAndExecuteCommand();
		void endOfHistory();
		void endOfLine();
		void forwardChar();
		void forwardSearchHistory();
		void forwardWord();
		void insertComment();
		void killLine();
		void killWholeLine();
		void killWord();
		void nextHistory();
		void pasteFromClipboard();
		void possibleCompletions();
		void previousHistory();
		void rereadInitFile();
		void reverseSearchHistory();
		void revertLine();
		void selfInsert();
		void transposeChars();
		void transposeWords();
		void undo();
		void unixLineDiscard();
		void unixWordRubout();
		void upcaseWord();
		void yank();
		void yankLastArg();
		void yankNthArg();
		void yankPop();

		// lineedit_readline.cpp: more of readline's commands.
		void backwardKillLine();
		void characterSearch();
		void characterSearchBackward();
		void completeCommand();
		void completeFilename();
		void completeVariable();
		void copyBackwardWord();
		void copyForwardWord();
		void copyRegionAsKill();
		void deleteCharOrList();
		void exchangePointAndMark();
		void forwardBackwardDeleteChar();
		void historySearchBackward();
		void historySearchForward();
		void historySubstringSearchBackward();
		void historySubstringSearchForward();
		void insertCompletions();
		void killRegion();
		void menuComplete();
		void menuCompleteBackward();
		void operateAndGetNext();
		void overwriteMode();
		void quotedInsert();
		void redrawCurrentLine();
		void setMark();
		void tabInsert();
		void unixFilenameRubout();

		enum class PendingKey {
			kNone, kQuotedInsert, kSearchForward, kSearchBackward,
			kViCharSearch, kViChangeChar, kViSetMark, kViGotoMark,
		};
		bool takePendingKey(const std::string& keys);
		void searchCharacter(const std::string& target, bool forward);
		void searchHistory(bool older, bool anchored);
		void stepMenu(bool forward);
		bool startMenu();
		void completeWith(std::vector<std::string> matches);
		void copyToKillRing(const std::string& text);
		void overwriteChars(const std::string& text);
		void loadOperateNext();

		// lineedit_vi.cpp: vi's insert and command modes.
		void emacsEditingMode();
		void viAppendEol();
		void viAppendMode();
		void viArgDigit();
		void viBackwardBigword();
		void viChangeCase();
		void viChangeChar();
		void viChangeTo();
		void viCharSearch();
		void viColumn();
		void viDelete();
		void viDeleteTo();
		void viEditingMode();
		void viEndBigword();
		void viEndWord();
		void viEofMaybe();
		void viFetchHistory();
		void viFirstPrint();
		void viForwardBigword();
		void viGotoMark();
		void viInsertBeg();
		void viInsertionMode();
		void viMatch();
		void viMovementMode();
		void viNextWord();
		void viPrevWord();
		void viPut();
		void viRedo();
		void viReplace();
		void viRubout();
		void viSearch();
		void viSearchAgain();
		void viSetMark();
		void viSubst();
		void viYankArg();
		void viYankTo();

		Keymap& activeKeymap() const;
		void resetViState();
		std::size_t takeCount();
		bool viKeepsCount(const std::string& name) const;
		void viSettleCursor();
		void viRunWithOperator(const std::string& name, EditCommand command);
		void viOperateOverMotion(const std::string& name, EditCommand command);
		void viChangeWords(bool big, std::size_t times);
		void viStartOperator(const std::string& op);
		void viCancelOperator();
		void viApplyOperator(std::size_t from, std::size_t to, bool inclusive);
		void viOperatorKey(const std::string& op, char to_end);
		void viEnterInsert();
		std::size_t clustersAhead(std::size_t from, std::size_t count) const;
		void viReplaceCharacters(const std::string& with);
		void viFindCharacter(char kind, const std::string& target);
		bool viStepToCharacter(char kind, const std::string& target);
		bool takeViPendingKey(PendingKey pending, const std::string& keys);
		void viJumpToMark(char letter);
		void viSearchHistory(bool older);
		void viRecordKeys(const std::string& keys);
		void viAfterKeys();
		bool viSplitsMetaKey(const std::string& keys);
		KeyLookup viSettlePrefix(KeyLookup found, const Keymap& keymap, const std::string& keys,
		                         KeyBinding& binding) const;
		bool readMinibuffer(const std::string& lead, std::string& text);
		bool collectMinibuffer();

		// PowerShell-style inline prediction: refreshSuggestion() fills
		// suggestion_ from history when the cursor sits at end-of-buffer
		// outside the reverse-search modal. forward-char at end-of-line
		// absorbs the suggestion instead of moving.
		void refreshSuggestion();
		bool acceptInlineSuggestion();

		// Ctrl-R modal: the search prompt replaces the normal one; typed
		// characters extend the query, Ctrl-R/Ctrl-S iterate matches, Enter
		// accepts and submits, Esc/Ctrl-J keep the match, Ctrl-G cancels,
		// any other editing key runs on the match. Windows-only; a no-op
		// stub elsewhere.
		void runReverseSearch();
		void revsearchRefresh(const SearchState& search);

		// Ctrl-V: CF_UNICODETEXT inserted at the cursor, its lines as
		// lines of the one command. Windows-only; a no-op stub elsewhere.
		void insertClipboardText();

		struct Tok {
			std::size_t start = 0;          // byte offset in buffer_ of the whole word
			std::size_t end = 0;            // the cursor
			bool first = true;              // true = command-position word
			std::size_t insert_start = 0;   // where a completion replaces the line
			char quote = 0;                 // the quote still open at the cursor
			std::string word;               // the text being completed, quotes removed
		};
		Tok currentToken() const;
		void insertMatch(const Tok& tok, const std::string& match, bool whole);
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
		std::vector<std::string> pluginCompletions(const PluginCommand& plugin,
		                                           const std::string& prefix,
		                                           const std::vector<std::string>& prev);
		std::vector<std::string> specCompletions(const CompletionSpec& spec,
		                                         const std::string& prefix, const Tok& tok,
		                                         const std::vector<std::string>& prev);
		const CompletionSpec* specFor(const std::string& command) const;
		std::vector<std::string> defaultSpecCompletions(const std::string& prefix, const Tok& tok,
		                                                const std::vector<std::string>& prev);
		std::vector<std::string> builtinToolCompletions(const std::string& head,
		                                                const std::string& prefix,
		                                                const std::vector<std::string>& prev);
		std::vector<std::string> commandWordCompletions(const std::string& prefix,
		                                                const Tok& tok);
		void publishCompletionContext(const Tok& tok, const std::vector<std::string>& prev);
		void markDirectories(std::vector<std::string>& matches) const;
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
		// `kind` is refs/heads, refs/tags or refs/remotes; names come back
		// without it.
		std::vector<std::string> gitRefs(const std::string& kind);
		void appendRefsFromGit(const std::string& kind, std::vector<std::string>& names);
		std::vector<std::string> gitRefTargets(bool tags_and_remotes, bool guesses);
		std::vector<std::string> gitRemotes();
		std::vector<std::string> currentMatches(Tok& tok);
		void applyCompletion(const Tok& tok,
		                     const std::vector<std::string>& matches);
		void printMatches(const std::vector<std::string>& matches);
		bool confirmLongList(std::size_t count);
		bool listsAmbiguous(bool extended) const;
		void listMatches(const std::vector<std::string>& matches);
		bool askYesNo();
		std::string longestCommonPrefix(const std::vector<std::string>& words);

		Environment& env_;
		Executor& exec_;
		bool headless_ = false;
		std::size_t forced_columns_ = 0;
		bool capture_output_ = false;
		std::string captured_output_;
		bool interrupted_ = false;
		bool done_ = false;
		bool eof_ = false;
		std::string accepted_;
#ifdef _WIN32
		std::deque<::INPUT_RECORD> replay_keys_;
		::DWORD cooked_input_mode_ = 0;
		::DWORD typeahead_records_ = 0;   // input queued before the prompt was shown
#endif

		std::size_t history_pos_ = 0;        // 0 = current edit, 1 = last entry, ...
		std::string saved_partial_;          // buffer at the moment user started scrolling

		std::string buffer_;
		std::size_t cursor_ = 0;
		std::string prompt_raw_;
		std::string prompt_lead_;      // lines of a multi-line prompt above its last one
		std::string prompt_visible_;   // the cells the last line takes, as text
		std::string prompt_lead_visible_;
		std::string prompt_full_;
		std::function<std::string()> prompt_refresher_;

		// Row offset of the cursor below the prompt's first row, as of the
		// most recent redraw. When the buffer wraps, redraw() must walk
		// back up this many rows before re-emitting, otherwise it leaves
		// stale wrapped lines above and re-prints the prompt on each row.
		std::size_t last_cursor_row_ = 0;
		bool redraw_due_ = false;

		// Keys typed so far toward a multi-key binding, the keys of the
		// binding running now, and the commands that ran before and now:
		// consecutive kills join, yank-pop and yank-last-arg follow on.
		std::string pending_keys_;
		std::string current_keys_;
		std::string last_command_;
		int macro_depth_ = 0;

		// The mark, overwrite mode, a key a command waits for, the text a
		// history search looks for, menu completion's matches, and the
		// entry operate-and-get-next brings up at the next prompt.
		std::size_t mark_ = 0;
		bool overwrite_ = false;
		PendingKey pending_key_ = PendingKey::kNone;
		std::string history_search_text_;
		std::vector<std::string> menu_matches_;
		std::size_t menu_index_ = 0;
		std::size_t menu_end_ = 0;
		std::string menu_word_;
		Tok menu_tok_;
		std::size_t operate_next_ = static_cast<std::size_t>(-1);

		// vi: command mode or insert mode, a count being typed, an
		// operator waiting for its motion, the last f/F/t/T and `/` search,
		// marks, and the keys of the change being made and the last one.
		bool vi_command_ = false;
		std::size_t vi_count_ = 0;
		std::string vi_operator_;
		std::size_t vi_operator_count_ = 0;
		char vi_search_kind_ = 0;
		std::string vi_search_char_;
		std::string vi_search_text_;
		bool vi_search_older_ = true;
		std::map<char, std::size_t> vi_marks_;
		std::vector<std::string> vi_keys_;
		std::vector<std::string> vi_last_change_;
		bool vi_change_ = false;
		bool vi_replaying_ = false;

		std::vector<std::string> kill_ring_;
		std::size_t kill_ring_top_ = 0;
		std::size_t yank_start_ = 0;
		std::size_t yank_end_ = 0;
		std::size_t last_arg_back_ = 0;
		std::size_t last_arg_start_ = 0;
		std::size_t last_arg_end_ = 0;

		// The line before each change, newest last; a run of typed
		// characters is one change.
		std::vector<UndoPoint> undo_;
		std::size_t insert_run_ = 0;

		// Tab-tracking: two consecutive tabs at the same word with no
		// progress means "show me all matches".
		bool last_was_tab_ = false;
		std::string last_tab_word_;

		// The last matches were file names: they are quoted for the shell
		// when inserted and listed by their last path component.
		bool filename_matches_ = false;

		// What the last completion spec asked for: no space after a whole
		// match (-o nospace), whether a spec answered at all (then its
		// empty answer stands), and its -F function's exit status.
		bool completion_nospace_ = false;
		bool spec_answered_ = false;
		int last_completion_status_ = 0;

		// Current ghost-text inline prediction (the tail past the
		// cursor), rendered in dim style by redraw(). Empty when no
		// prediction applies -- including while the reverse-search
		// modal owns the prompt, gated by revsearch_active_.
		std::string suggestion_;
		bool revsearch_active_ = false;
		UndoPoint revsearch_origin_;
		std::string last_search_query_;
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

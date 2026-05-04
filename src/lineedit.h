#pragma once

#include "environment.h"
#include "executor.h"

#include <string>
#include <vector>

namespace wbsh {

	// Interactive line editor. Reads a single line of input from a real TTY
	// console with editing (arrow keys, backspace, etc.), persistent history
	// (up/down), and Tab completion. Falls back to a line-buffered fgetc
	// loop when stdin isn't a TTY (so piped scripts and the existing tests
	// still work).
	class LineEditor {
	public:
		LineEditor(Environment& env, Executor& exec);

		// Read one line from the user. On Enter, fills `out` and returns
		// true (the line is also appended to the executor's history if
		// non-empty and distinct from the previous entry). On EOF (Ctrl-D
		// on an empty line, or stdin closed), returns false.
		bool readLine(const std::string& prompt, std::string& out);

	private:
		// Raw-TTY path (Windows console).
		bool readLineRaw(const std::string& prompt, std::string& out);
		// Cooked / piped fallback.
		bool readLineCooked(std::string& out);

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

		// Tab-tracking: two consecutive tabs at the same word with no
		// progress means "show me all matches".
		bool last_was_tab_ = false;
		std::string last_tab_word_;
	};

}  // namespace wbsh

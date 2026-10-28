#pragma once

/**
 * @file history.h
 * @brief How typed lines become history entries, and how the history file is kept.
 *
 * A command typed over several lines is one history entry, as bash's
 * cmdhist keeps it: each continuation line is joined to the entry with
 * "; ", a space, or -- inside quotes, here-documents and after comments
 * -- a literal newline, so that the entry runs the same command when
 * recalled.
 *
 * The history file is appended to as each command finishes, so several
 * shells share it without overwriting each other and a closed window
 * loses nothing; it is only ever rewritten whole through a temporary
 * file renamed over it.
 */

#include <cstddef>
#include <string>
#include <vector>

namespace wbsh {

	class Executor;

	/**
	 * Adds @p line, typed after the not yet complete command @p pending
	 * (its lines joined with newlines), to the history @p entry being
	 * built for that command.
	 */
	void appendHistoryLine(std::string& entry, const std::string& pending,
	                       const std::string& line);

	/** True when @p text ends with an unescaped backslash that continues it on the next line. */
	bool endsWithLineContinuation(const std::string& text);

	/** The last `s/old/new/`, which `:&`, `:g&` and an empty old text reuse. */
	struct HistorySubstitution {
		std::string old_text;
		std::string new_text;
	};

	struct HistoryExpansion {
		enum class Kind {
			kUnchanged,   // no `!` to expand; `text` is the line
			kExpanded,    // `text` is the line to run
			kPrintOnly,   // `:p` -- `text` is shown and saved, not run
			kFailed,      // `text` is the message, e.g. "!foo: event not found"
		};
		Kind kind = Kind::kUnchanged;
		std::string text;
	};

	/**
	 * bash's history expansion of one input line against @p history:
	 * events (`!!`, `!n`, `!-n`, `!str`, `!?str?`, `!#`), word designators
	 * (`:n`, `^`, `$`, `*`, `x-y`, `%`), modifiers (`h t r e p q x s & g`)
	 * and `^old^new^`. Nothing in single quotes, after a backslash, before
	 * a blank, `=`, `(` or a closing `"`, or in `[!...]`, `${!name}` and
	 * `$!` is expanded. @p starting_quote is the quote (`'` or `"`) a
	 * continuation line starts inside of, or `\0`.
	 */
	HistoryExpansion expandHistoryLine(const std::string& line,
	                                   const std::vector<std::string>& history,
	                                   char starting_quote, HistorySubstitution& last);

	/** The entries of a history file, oldest first, with each command's exit status. */
	struct HistoryList {
		std::vector<std::string> commands;
		std::vector<int>         statuses;
	};

	/** Reads the history file at the native @p path; false when it cannot be opened. */
	bool readHistoryFile(const std::string& path, HistoryList& list);

	/** Replaces the file with the entries of @p list from @p first on, all or nothing. */
	bool writeHistoryFile(const std::string& path, const HistoryList& list, std::size_t first);

	/**
	 * Appends the entries of @p list from @p first on in a single write, so
	 * the entries of shells sharing the file never interleave mid-line. A
	 * file in an older format is rewritten in the current one first.
	 */
	bool appendHistoryFile(const std::string& path, const HistoryList& list, std::size_t first);

	/** Drops all but the newest @p keep entries of the file at @p path. */
	bool trimHistoryFile(const std::string& path, std::size_t keep);

	/** HISTSIZE / HISTFILESIZE as a count; negative (no limit) when unset or not a count. */
	long long historyLimit(const std::string& value);

	/** The HISTCONTROL and HISTIGNORE settings an entry is checked against. */
	struct HistoryFilter {
		std::string control;
		std::string ignore;
	};

	/** Whether @p filter lets @p entry into a history whose newest entry is @p newest. */
	bool historyAccepts(const HistoryFilter& filter, const std::string& entry,
	                    const std::string& newest);

	/** True when HISTCONTROL asks for older copies of an entry to be erased. */
	bool historyErasesDuplicates(const HistoryFilter& filter);

	/**
	 * Makes the console's close, logoff and shutdown events write the
	 * command being run (see holdUnsavedHistory) to the history file before
	 * the process is ended.
	 */
	void installHistoryCloseHandler();

	/** The entry of the command about to run, for the close handler to write. */
	void holdUnsavedHistory(const std::string& path, const std::string& command);

	/**
	 * Appends @p exec's entries that are not in the file at @p path yet and
	 * forgets the held entry, under the lock the close handler takes.
	 */
	void commitHistory(Executor& exec, const std::string& path);

}  // namespace wbsh

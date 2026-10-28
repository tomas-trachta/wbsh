/**
 * @file keymap_vi.cpp
 * @brief readline's vi keymaps: insert mode, and the command (movement) mode.
 */

#include "keymap.h"

#include <string>
#include <utility>
#include <vector>

#include "environment.h"

namespace wbsh {

	namespace keymap_vi_detail {

		using Defaults = std::vector<std::pair<std::string, std::string>>;

		// Keys every mode shares: the cursor and editing keys a console
		// sends, and the end of a bracketed paste's start.
		static void addSpecialKeys(Defaults& keys) {
			keys.insert(keys.end(), {
				{ "\x1b[A", "previous-history" },  { "\x1b[B", "next-history" },
				{ "\x1b[C", "forward-char" },      { "\x1b[D", "backward-char" },
				{ "\x1b[H", "beginning-of-line" }, { "\x1b[F", "end-of-line" },
				{ "\x1b[3~", "delete-char" },      { "\x1b[200~", "bracketed-paste-begin" },
				{ "\x1b[1;5C", "vi-next-word" },   { "\x1b[1;5D", "vi-prev-word" },
			});
		}

		static Defaults insertDefaults() {
			Defaults keys = {
				{ "\x1b", "vi-movement-mode" },      { "\x04", "vi-eof-maybe" },
				{ "\x08", "backward-delete-char" },  { "\x7f", "backward-delete-char" },
				{ "\t", "complete" },                { "\n", "accept-line" },
				{ "\r", "accept-line" },             { "\x0c", "clear-screen" },
				{ "\x11", "quoted-insert" },         { "\x12", "reverse-search-history" },
				{ "\x13", "forward-search-history" }, { "\x14", "transpose-chars" },
				{ "\x15", "unix-line-discard" },     { "\x16", "paste-from-clipboard" },
				{ "\x17", "unix-word-rubout" },      { "\x19", "yank" },
				{ "\x1f", "undo" },
			};
			addSpecialKeys(keys);
			return keys;
		}

		static void addCommandMotions(Defaults& keys) {
			keys.insert(keys.end(), {
				{ " ", "forward-char" },        { "\x08", "backward-char" },
				{ "\x7f", "backward-char" },    { "h", "backward-char" },
				{ "l", "forward-char" },        { "w", "vi-next-word" },
				{ "b", "vi-prev-word" },        { "e", "vi-end-word" },
				{ "W", "vi-forward-bigword" },  { "B", "vi-backward-bigword" },
				{ "E", "vi-end-bigword" },      { "0", "vi-arg-digit" },
				{ "^", "vi-first-print" },      { "$", "end-of-line" },
				{ "|", "vi-column" },           { "%", "vi-match" },
				{ "f", "vi-char-search" },      { "F", "vi-char-search" },
				{ "t", "vi-char-search" },      { "T", "vi-char-search" },
				{ ";", "vi-char-search" },      { ",", "vi-char-search" },
				{ "`", "vi-goto-mark" },        { "'", "vi-goto-mark" },
				{ "m", "vi-set-mark" },
			});
			for (char digit = '1'; digit <= '9'; ++digit) {
				keys.emplace_back(std::string(1, digit), "vi-arg-digit");
			}
		}

		static void addCommandEdits(Defaults& keys) {
			keys.insert(keys.end(), {
				{ "i", "vi-insertion-mode" },   { "a", "vi-append-mode" },
				{ "I", "vi-insert-beg" },       { "A", "vi-append-eol" },
				{ "R", "vi-replace" },          { "r", "vi-change-char" },
				{ "x", "vi-delete" },           { "X", "vi-rubout" },
				{ "s", "vi-subst" },            { "S", "vi-subst" },
				{ "d", "vi-delete-to" },        { "D", "vi-delete-to" },
				{ "c", "vi-change-to" },        { "C", "vi-change-to" },
				{ "y", "vi-yank-to" },          { "Y", "vi-yank-to" },
				{ "p", "vi-put" },              { "P", "vi-put" },
				{ "~", "vi-change-case" },      { ".", "vi-redo" },
				{ "u", "undo" },                { "U", "revert-line" },
				{ "_", "vi-yank-arg" },         { "#", "insert-comment" },
				{ "v", "edit-and-execute-command" },
			});
		}

		static void addCommandLineKeys(Defaults& keys) {
			keys.insert(keys.end(), {
				{ "j", "next-history" },        { "k", "previous-history" },
				{ "+", "next-history" },        { "-", "previous-history" },
				{ "G", "vi-fetch-history" },    { "/", "vi-search" },
				{ "?", "vi-search" },           { "n", "vi-search-again" },
				{ "N", "vi-search-again" },     { "\t", "complete" },
				{ "\\", "complete" },           { "=", "possible-completions" },
				{ "*", "insert-completions" },  { "\n", "accept-line" },
				{ "\r", "accept-line" },        { "\x04", "vi-eof-maybe" },
				{ "\x05", "emacs-editing-mode" }, { "\x0c", "clear-screen" },
				{ "\x12", "reverse-search-history" }, { "\x13", "forward-search-history" },
				{ "\x15", "unix-line-discard" }, { "\x17", "unix-word-rubout" },
				{ "\x19", "yank" },             { "\x16", "paste-from-clipboard" },
			});
		}

		static Defaults commandDefaults() {
			Defaults keys;
			addCommandMotions(keys);
			addCommandEdits(keys);
			addCommandLineKeys(keys);
			addSpecialKeys(keys);
			return keys;
		}

	}  // namespace keymap_vi_detail

	const std::vector<std::pair<std::string, std::string>>& viKeymapDefaults(KeymapKind kind) {
		static const keymap_vi_detail::Defaults insert = keymap_vi_detail::insertDefaults();
		static const keymap_vi_detail::Defaults command = keymap_vi_detail::commandDefaults();
		return kind == KeymapKind::kViCommand ? command : insert;
	}

	Keymap& viInsertKeymap() {
		static Keymap keymap(KeymapKind::kViInsert);
		return keymap;
	}

	Keymap& viCommandKeymap() {
		static Keymap keymap(KeymapKind::kViCommand);
		return keymap;
	}

	bool viEditing() {
		return shellKeymap().variable("editing-mode") == "vi";
	}

	Keymap& editingKeymap() {
		return viEditing() ? viInsertKeymap() : shellKeymap();
	}

	// `set -o vi` / `set -o emacs` and readline's editing-mode are one
	// setting; whichever changes it, both show the same.
	void syncEditingOptions(Environment& env) {
		const bool vi = viEditing();
		env.options().vi = vi;
		env.options().emacs = !vi;
	}

}  // namespace wbsh

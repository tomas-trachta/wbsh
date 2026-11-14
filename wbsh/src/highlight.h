#pragma once

/**
 * @file highlight.h
 * @brief Syntax colours for the command line being edited.
 *
 * A light, error-tolerant scan of shell text -- half-typed quotes and
 * substitutions included -- that gives every byte a role: command word,
 * keyword, string, expansion, comment, operator, option or path. It
 * never runs the parser and never touches the disk itself; what a word
 * names is asked of the caller, which answers from its caches.
 */

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace wbsh {

	enum class HighlightRole : unsigned char {
		kPlain,
		kCommand,    // a builtin, function, alias or program that exists
		kUnknown,    // a command word that names nothing
		kKeyword,
		kString,
		kVariable,   // $name, ${...}, $(...), `...`, $((...))
		kComment,
		kOperator,
		kOption,
		kPath,       // an argument naming a file or directory that exists
	};

	constexpr std::size_t kHighlightRoleCount = 10;

	struct HighlightLookup {
		// kCommand or kUnknown for a command word, kPlain when it cannot
		// tell (yet) -- the word is then left uncoloured.
		std::function<HighlightRole(const std::string& word)> command;
		std::function<bool(const std::string& word)> existing_path;
	};

	// One role per byte of `text`.
	std::vector<HighlightRole> highlightRoles(const std::string& text, const HighlightLookup& lookup);

	// SGR parameters per role, such as "1;32"; an empty one leaves the
	// role's text in the terminal's default style.
	struct HighlightPalette {
		std::string sgr[kHighlightRoleCount];
	};

	HighlightPalette defaultHighlightPalette();

	// `role=SGR:role=SGR...`, as in WBSH_HIGHLIGHT; entries with an unknown
	// role or a value other than digits and `;` are passed over.
	void applyHighlightSpec(const std::string& spec, HighlightPalette& palette);

	// text[from, end) with an SGR sequence wherever the colour changes;
	// back in the default style at the end. The sequences take no cells.
	std::string paintHighlighted(const std::string& text, const std::vector<HighlightRole>& roles,
	                             std::size_t from, const HighlightPalette& palette);

}  // namespace wbsh

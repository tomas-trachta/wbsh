#pragma once

/**
 * @file man_pages.h
 * @brief The whatis line and synopsis of every command wbsh carries, the
 *        material `man`, `apropos` and `whatis` build their pages from.
 */

#include <cstddef>
#include <string>

namespace wbsh {

	enum class ManKind { Shell, Utility, Program };

	struct ManPage {
		const char* name;
		ManKind kind;
		const char* summary;
		/// One usage form per line.
		const char* synopsis;
		/// The command prints its own usage for `--help`.
		bool self_help;
	};

	/// nullptr when wbsh has no page for @p name.
	const ManPage* findManPage(const std::string& name);

	const ManPage* manPagesBegin();
	const ManPage* manPagesEnd();

}  // namespace wbsh

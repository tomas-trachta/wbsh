#pragma once

/**
 * @file collsyms.h
 * @brief The POSIX collating-symbol names a bracket expression may use
 *        in `[.name.]`, as bash's collsyms.h lists them.
 */

#include <string>

namespace wbsh {

	/// The character a `[.name.]` names: a single character stands for
	/// itself, a POSIX name (`space`, `hyphen`, ...) for its character.
	/// False for anything else, such as a multi-character element.
	bool collatingSymbolChar(const std::string& name, char32_t& out);

}  // namespace wbsh

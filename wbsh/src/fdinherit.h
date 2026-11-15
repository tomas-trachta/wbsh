#pragma once

/**
 * @file fdinherit.h
 * @brief The descriptors above 2 that the shell's children get, as a
 *        forked bash's children inherit every fd not marked close-on-exec.
 *
 * Windows' close-on-exec is a handle without HANDLE_FLAG_INHERIT. A
 * redirection the user made (`exec 3>file`, `{fd}<file`, `cmd 4>&1`) sets
 * it on its fd; the shell's own copies (saved fds, coproc ends) clear it.
 * A child is given the inheritable ones in the block the C runtime reads
 * back into its fd table at startup (STARTUPINFO's lpReserved2), the way
 * `_spawn` passes them, so a child wbsh has them under the same numbers.
 */

#include <vector>

namespace wbsh {

	/// @p fd (3 and up) goes to every child the shell starts from now on.
	void passFdToChildren(int fd);

	/// @p fd is the shell's own and no child gets it.
	void keepFdFromChildren(int fd);

	struct ChildFds {
		/// lpReserved2's contents; empty when no descriptor above 2 is passed.
		std::vector<unsigned char> block;
		/// The handles in @c block, for the child's inherit list.
		std::vector<void*> handles;
	};

	ChildFds collectChildFds();

}  // namespace wbsh

#pragma once

/**
 * @file pipesecurity.h
 * @brief Security attributes for the named pipes wbsh creates.
 *
 * The default DACL of a named pipe gives read access to Everyone and to
 * Anonymous, and wbsh's pipe names (`\\.\pipe\wbsh-<pid>-<n>`) are easy to
 * guess, so another user on the same machine could connect to a `<(cmd)`
 * pipe first and read its data. These attributes let only the current
 * user and SYSTEM open the pipe.
 */

#ifdef _WIN32

#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>

namespace wbsh {

	/// Built once and kept for the life of the process. Null when the
	/// descriptor cannot be built; the pipe then gets the default DACL.
	SECURITY_ATTRIBUTES* currentUserPipeSecurity();

}  // namespace wbsh

#endif  // _WIN32

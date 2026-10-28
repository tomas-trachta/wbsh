#pragma once

/**
 * @file procsubst.h
 * @brief The named pipe behind `<(cmd)` and `>(cmd)`.
 *
 * The command line gets the name of a pipe, `\\.\pipe\wbsh-<pid>-<n>`,
 * and the substituted process gets one end of an anonymous pipe as its
 * stdout (`<`) or stdin (`>`). A relay thread joins the two once a
 * client opens the name, so the process runs concurrently with the
 * command that reads (or writes) it and nothing lands on disk.
 */

#ifdef _WIN32

#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>

#include <string>

namespace wbsh {

	struct ProcSubstRelay {
		std::string path;                            ///< `\\.\pipe\...`, for the command line.
		HANDLE child_end = INVALID_HANDLE_VALUE;     ///< Inheritable; the caller closes it.
		HANDLE cancel = nullptr;
		HANDLE thread = nullptr;
	};

	/// Creates the pipes and starts the relay; @p direction is `<` or `>`.
	bool startProcSubstRelay(char direction, ProcSubstRelay& relay);

	/// The command that named the pipe has finished: a relay whose pipe
	/// nobody has used yet stops waiting, which ends the substituted
	/// process's stdin (`>`) or its next write (`<`). A relay already
	/// moving data finishes on its own.
	void releaseProcSubstRelay(ProcSubstRelay& relay);

}  // namespace wbsh

#endif /* _WIN32 */

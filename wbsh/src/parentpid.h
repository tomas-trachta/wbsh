#pragma once

/**
 * @file parentpid.h
 * @brief The id of the process that started this one, for `$PPID`.
 */

namespace wbsh {

	/// 0 when the system will not say.
	long long parentProcessId();

}  // namespace wbsh

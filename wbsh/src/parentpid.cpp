/**
 * @file parentpid.cpp
 * @brief The id of the process that started this one, for `$PPID`.
 */

#include "parentpid.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <unistd.h>
#endif /* _WIN32 */

namespace wbsh {

#ifdef _WIN32
	namespace parentpid_detail {

		struct BasicInformation {
			LONG      exit_status;
			void*     peb;
			ULONG_PTR affinity;
			LONG      base_priority;
			ULONG_PTR process_id;
			ULONG_PTR parent_process_id;
		};

		using QueryInformationFn = LONG (WINAPI*)(HANDLE, int, void*, ULONG, ULONG*);

		static const int kProcessBasicInformation = 0;

		static QueryInformationFn queryInformationProcess() {
			const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
			if (ntdll == nullptr) return nullptr;

			return reinterpret_cast<QueryInformationFn>(
				reinterpret_cast<void*>(::GetProcAddress(ntdll, "NtQueryInformationProcess")));
		}

	}  // namespace parentpid_detail

	// One query of this process instead of a snapshot of every process on
	// the machine, which cost several milliseconds of each start.
	long long parentProcessId() {
		const parentpid_detail::QueryInformationFn query =
			parentpid_detail::queryInformationProcess();
		if (query == nullptr) return 0;

		parentpid_detail::BasicInformation info{};
		const LONG status = query(::GetCurrentProcess(),
			parentpid_detail::kProcessBasicInformation, &info, sizeof(info), nullptr);
		if (status < 0) return 0;

		return static_cast<long long>(info.parent_process_id);
	}
#else
	long long parentProcessId() {
		return static_cast<long long>(::getppid());
	}
#endif /* _WIN32 */

}  // namespace wbsh

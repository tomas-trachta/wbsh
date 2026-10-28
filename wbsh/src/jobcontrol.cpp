/**
 * @file jobcontrol.cpp
 * @brief Stopping and resuming jobs, and catching Ctrl+Z, on Windows.
 */

#include "jobcontrol.h"

#ifdef _WIN32

#include <vector>

#include "interrupt.h"

namespace wbsh::jobcontrol_detail {

	static const DWORD kPollMilliseconds = 50;

	using ProcessCall = LONG (NTAPI*)(HANDLE);

	// NtSuspendProcess / NtResumeProcess stop and restart every thread of
	// a process at once; the documented API only works thread by thread.
	static ProcessCall ntdllCall(const char* name) {
		const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
		if (ntdll == nullptr) return nullptr;

		return reinterpret_cast<ProcessCall>(
			reinterpret_cast<void*>(::GetProcAddress(ntdll, name)));
	}

	static bool callOnProcess(bool suspend, HANDLE process) {
		static const ProcessCall suspend_call = ntdllCall("NtSuspendProcess");
		static const ProcessCall resume_call = ntdllCall("NtResumeProcess");
		const ProcessCall call = suspend ? suspend_call : resume_call;
		return call != nullptr && call(process) >= 0;
	}

	static bool callOnProcessId(bool suspend, unsigned long pid) {
		const HANDLE process = ::OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, pid);
		if (process == nullptr) return false;

		const bool done = callOnProcess(suspend, process);
		::CloseHandle(process);
		return done;
	}

	static std::vector<ULONG_PTR> groupProcessIds(HANDLE group) {
		std::vector<char> buffer(sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + 256 * sizeof(ULONG_PTR));
		auto* list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(buffer.data());
		const DWORD size = static_cast<DWORD>(buffer.size());
		if (!::QueryInformationJobObject(group, JobObjectBasicProcessIdList, list, size, nullptr)) {
			return {};
		}

		return std::vector<ULONG_PTR>(list->ProcessIdList,
			list->ProcessIdList + list->NumberOfProcessIdsInList);
	}

	// JOBOBJECT_FREEZE_INFORMATION (job information class 18): a frozen
	// job's processes run no thread at all, not even the one the console
	// starts in every attached process for Ctrl+C, so a stopped job is not
	// ended by a Ctrl+C meant for the foreground.
	struct JobFreezeInformation {
		ULONG   flags;
		BOOLEAN freeze;
		BOOLEAN swap;
		UCHAR   reserved[2];
		ULONG   high_edge_filter;
		ULONG   low_edge_filter;
	};

	static const int kJobObjectFreezeInformation = 18;
	static const ULONG kFreezeOperation = 1;

	using JobInformationCall = LONG (NTAPI*)(HANDLE, int, void*, ULONG);

	static bool freezeGroup(HANDLE group, bool freeze) {
		static const JobInformationCall call = reinterpret_cast<JobInformationCall>(
			reinterpret_cast<void*>(ntdllCall("NtSetInformationJobObject")));
		if (call == nullptr) return false;

		JobFreezeInformation information{};
		information.flags = kFreezeOperation;
		information.freeze = freeze ? TRUE : FALSE;
		return call(group, kJobObjectFreezeInformation, &information,
			static_cast<ULONG>(sizeof(information))) >= 0;
	}

	static void callOnGroup(bool suspend, HANDLE group) {
		if (group == nullptr) return;
		if (freezeGroup(group, suspend)) return;

		for (const ULONG_PTR pid : groupProcessIds(group)) {
			callOnProcessId(suspend, static_cast<unsigned long>(pid));
		}
	}

	static bool isCtrlZ(const INPUT_RECORD& record) {
		if (record.EventType != KEY_EVENT) return false;

		const KEY_EVENT_RECORD& key = record.Event.KeyEvent;
		const DWORD ctrl = LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED;
		const DWORD alt = LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED;
		if (key.uChar.UnicodeChar == 0x1A) return true;

		return key.wVirtualKeyCode == 'Z' && (key.dwControlKeyState & ctrl) != 0
			&& (key.dwControlKeyState & alt) == 0;
	}

	static bool containsCtrlZ(const std::vector<INPUT_RECORD>& records) {
		for (const INPUT_RECORD& record : records) {
			if (isCtrlZ(record) && record.Event.KeyEvent.bKeyDown) return true;
		}

		return false;
	}

	static std::vector<INPUT_RECORD> peekInput(HANDLE console) {
		DWORD count = 0;
		if (!::GetNumberOfConsoleInputEvents(console, &count) || count == 0) return {};

		std::vector<INPUT_RECORD> records(count);
		DWORD got = 0;
		if (!::PeekConsoleInputW(console, records.data(), count, &got)) return {};

		records.resize(got);
		return records;
	}

	// The events are taken off the buffer and all but the Ctrl+Z ones put
	// back, so whatever the job had not read yet is still there for it.
	static void dropCtrlZ(HANDLE console, std::vector<INPUT_RECORD>& records) {
		DWORD got = 0;
		::ReadConsoleInputW(console, records.data(), static_cast<DWORD>(records.size()), &got);
		records.resize(got);

		std::vector<INPUT_RECORD> kept;
		for (const INPUT_RECORD& record : records) {
			if (!isCtrlZ(record)) kept.push_back(record);
		}

		DWORD written = 0;
		if (!kept.empty()) {
			::WriteConsoleInputW(console, kept.data(), static_cast<DWORD>(kept.size()), &written);
		}
	}

	static HANDLE firstRunning(const std::vector<HANDLE>& processes) {
		for (const HANDLE process : processes) {
			if (::WaitForSingleObject(process, 0) != WAIT_OBJECT_0) return process;
		}

		return nullptr;
	}

	static bool timeIsUp(ULONGLONG start, unsigned long milliseconds) {
		return milliseconds != INFINITE && ::GetTickCount64() - start >= milliseconds;
	}

}  // namespace wbsh::jobcontrol_detail

namespace wbsh {

	HANDLE createProcessGroup() {
		return ::CreateJobObjectW(nullptr, nullptr);
	}

	void suspendProcessGroup(HANDLE group) {
		jobcontrol_detail::callOnGroup(true, group);
	}

	void resumeProcessGroup(HANDLE group) {
		jobcontrol_detail::callOnGroup(false, group);
	}

	bool suspendProcessById(unsigned long pid) {
		return jobcontrol_detail::callOnProcessId(true, pid);
	}

	bool resumeProcessById(unsigned long pid) {
		return jobcontrol_detail::callOnProcessId(false, pid);
	}

	HANDLE consoleInput() {
		const HANDLE input = ::GetStdHandle(STD_INPUT_HANDLE);
		DWORD mode = 0;
		if (input == nullptr || input == INVALID_HANDLE_VALUE) return nullptr;

		return ::GetConsoleMode(input, &mode) ? input : nullptr;
	}

	bool takeCtrlZ(HANDLE console) {
		if (console == nullptr) return false;

		std::vector<INPUT_RECORD> records = jobcontrol_detail::peekInput(console);
		if (!jobcontrol_detail::containsCtrlZ(records)) return false;

		jobcontrol_detail::dropCtrlZ(console, records);
		return true;
	}

	// With no processes this is an interruptible sleep (the builtin
	// `sleep`), which Ctrl+C ends as well.
	ForegroundEnd waitForeground(const std::vector<HANDLE>& processes, HANDLE console,
	                             unsigned long milliseconds) {
		const ULONGLONG start = ::GetTickCount64();
		for (;;) {
			const HANDLE running = jobcontrol_detail::firstRunning(processes);
			if (!processes.empty() && running == nullptr) return ForegroundEnd::Exited;
			if (takeCtrlZ(console)) return ForegroundEnd::Stopped;
			if (jobcontrol_detail::timeIsUp(start, milliseconds)) return ForegroundEnd::Exited;
			if (processes.empty() && ctrlCPending()) return ForegroundEnd::Interrupted;

			const DWORD poll = console != nullptr ? jobcontrol_detail::kPollMilliseconds : INFINITE;
			if (running == nullptr) ::Sleep(jobcontrol_detail::kPollMilliseconds);
			else ::WaitForSingleObject(running, poll);
		}
	}

}  // namespace wbsh

#endif /* _WIN32 */

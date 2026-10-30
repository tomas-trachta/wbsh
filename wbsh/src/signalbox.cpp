/**
 * @file signalbox.cpp
 * @brief The per-process signal mailbox: a shared word of bits and an event.
 */

#include "signalbox.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif /* _WIN32 */

#include <atomic>
#include <string>

#include "exitstatus.h"

namespace wbsh {

#ifdef _WIN32

	namespace signalbox_detail {

		struct Mailbox {
			HANDLE mapping = nullptr;
			HANDLE event = nullptr;
			volatile LONG* pending = nullptr;
		};

	}  // namespace signalbox_detail

	using signalbox_detail::Mailbox;

	static Mailbox g_mailbox;
	static std::atomic<unsigned long> g_caught{ 0 };

	static const DWORD kPollMs = 100;
	static const DWORD kGraceMs = 500;

	static std::wstring mappingName(unsigned long pid) {
		return L"Local\\wbsh-signals-" + std::to_wstring(pid);
	}

	static std::wstring eventName(unsigned long pid) {
		return mappingName(pid) + L"-event";
	}

	static int lowestUncaught() {
		const unsigned long uncaught = pendingShellSignals() & ~g_caught.load();
		for (int signum = 1; signum < 32; ++signum) {
			if ((uncaught & (1UL << signum)) != 0) return signum;
		}

		return 0;
	}

	// The shell takes a signal at its next safe point. One it neither traps
	// nor ignores that is still pending after the grace means it is busy in
	// a builtin and will not get there soon, so the process ends as that
	// signal would end it.
	static DWORD WINAPI watchMailbox(LPVOID) {
		for (;;) {
			if (::WaitForSingleObject(g_mailbox.event, INFINITE) != WAIT_OBJECT_0) return 0;

			const int signum = lowestUncaught();
			::Sleep(signum == 0 ? kPollMs : kGraceMs);
			if (signum == 0 || lowestUncaught() != signum) continue;

			::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(kSignalStatusBase + signum));
		}
	}

	void setCaughtShellSignals(unsigned long mask) {
		g_caught.store(mask);
	}

	void openSignalMailbox() {
		if (g_mailbox.pending != nullptr) return;

		const unsigned long pid = ::GetCurrentProcessId();
		g_mailbox.mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
			sizeof(LONG), mappingName(pid).c_str());
		if (g_mailbox.mapping == nullptr) return;

		g_mailbox.event = ::CreateEventW(nullptr, TRUE, FALSE, eventName(pid).c_str());
		void* view = ::MapViewOfFile(g_mailbox.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LONG));
		if (view == nullptr || g_mailbox.event == nullptr) return;

		g_mailbox.pending = static_cast<volatile LONG*>(view);
		::InterlockedExchange(g_mailbox.pending, 0);

		const HANDLE watcher = ::CreateThread(nullptr, 0, watchMailbox, nullptr, 0, nullptr);
		if (watcher != nullptr) ::CloseHandle(watcher);
	}

	static bool setPendingBit(HANDLE mapping, int signum) {
		void* view = ::MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(LONG));
		if (view == nullptr) return false;

		::InterlockedOr(static_cast<volatile LONG*>(view), static_cast<LONG>(1UL << signum));
		::UnmapViewOfFile(view);
		return true;
	}

	bool postShellSignal(unsigned long pid, int signum) {
		if (signum < 1 || signum > 31) return false;

		const HANDLE mapping = ::OpenFileMappingW(FILE_MAP_WRITE, FALSE, mappingName(pid).c_str());
		if (mapping == nullptr) return false;

		const bool posted = setPendingBit(mapping, signum);
		::CloseHandle(mapping);
		if (!posted) return false;

		const HANDLE event = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName(pid).c_str());
		if (event == nullptr) return true;

		::SetEvent(event);
		::CloseHandle(event);
		return true;
	}

	unsigned long pendingShellSignals() {
		if (g_mailbox.pending == nullptr) return 0;
		return static_cast<unsigned long>(::InterlockedCompareExchange(g_mailbox.pending, 0, 0));
	}

	// The event is reset only once no bit is left; a post that lands between
	// the reset and the check sets it again here.
	bool takeShellSignal(int signum) {
		if (g_mailbox.pending == nullptr || signum < 1 || signum > 31) return false;

		const LONG bit = static_cast<LONG>(1UL << signum);
		const LONG before = ::InterlockedAnd(g_mailbox.pending, ~bit);
		if ((before & ~bit) == 0) ::ResetEvent(g_mailbox.event);
		if (pendingShellSignals() != 0) ::SetEvent(g_mailbox.event);
		return (before & bit) != 0;
	}

	void* shellSignalEvent() {
		return g_mailbox.pending == nullptr ? nullptr : g_mailbox.event;
	}

#else

	void openSignalMailbox() {}

	void setCaughtShellSignals(unsigned long) {}

	bool postShellSignal(unsigned long, int) {
		return false;
	}

	unsigned long pendingShellSignals() {
		return 0;
	}

	bool takeShellSignal(int) {
		return false;
	}

	void* shellSignalEvent() {
		return nullptr;
	}

#endif /* _WIN32 */

}  // namespace wbsh

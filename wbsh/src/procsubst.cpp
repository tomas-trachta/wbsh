/**
 * @file procsubst.cpp
 * @brief Named-pipe relay for `<(cmd)` and `>(cmd)`.
 */

#include "procsubst.h"

#ifdef _WIN32

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "pathconv.h"

namespace wbsh::procsubst_detail {

	static const DWORD kChunkBytes = 64 * 1024;
	static const std::size_t kListeningInstances = 2;

	// One OVERLAPPED serves the connect and then each transfer in turn.
	// `in_flight` is set only while the kernel owns it: a call that fails
	// at once leaves it looking pending, and waiting for it would hang.
	struct Instance {
		HANDLE pipe = INVALID_HANDLE_VALUE;
		HANDLE event = nullptr;
		OVERLAPPED overlapped{};
		bool in_flight = false;
		std::vector<char> buffer;
	};

	// Opening the pipe's name only to look at it (GetFileAttributes, a
	// stat, an open for attributes) connects to an instance just like an
	// open to read or write. So every client gets the first transfer
	// offered at once, and the one that moves data first is the real
	// one; the others are dropped. Two instances always listen, so an
	// open that follows such a probe never finds the pipe busy.
	struct Relay {
		std::wstring name;
		bool outbound = true;
		HANDLE local_end = INVALID_HANDLE_VALUE;
		HANDLE cancel = nullptr;
		std::vector<std::unique_ptr<Instance>> listening;
		std::vector<std::unique_ptr<Instance>> clients;
		std::unique_ptr<Instance> chosen;
		std::string pending;
		DWORD first_moved = 0;
		bool delivered = false;
	};

	enum class IoResult { Moved, Failed, Cancelled };

	static std::wstring uniquePipeName() {
		static std::atomic<unsigned long> counter{ 0 };
		return L"\\\\.\\pipe\\wbsh-" + std::to_wstring(::GetCurrentProcessId()) + L"-"
			+ std::to_wstring(++counter);
	}

	static void startListening(Instance& instance) {
		if (::ConnectNamedPipe(instance.pipe, &instance.overlapped)) {
			::SetEvent(instance.event);
			return;
		}

		const DWORD error = ::GetLastError();
		instance.in_flight = error == ERROR_IO_PENDING;
		if (error == ERROR_PIPE_CONNECTED || error == ERROR_NO_DATA) ::SetEvent(instance.event);
	}

	static std::unique_ptr<Instance> openInstance(const Relay& relay, bool first) {
		const DWORD access = (relay.outbound ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND)
			| FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
		const DWORD mode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT
			| PIPE_REJECT_REMOTE_CLIENTS;

		auto instance = std::make_unique<Instance>();
		instance->pipe = ::CreateNamedPipeW(relay.name.c_str(), access, mode,
			PIPE_UNLIMITED_INSTANCES, 0, 0, 0, nullptr);
		if (instance->pipe == INVALID_HANDLE_VALUE) return nullptr;

		instance->event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		instance->overlapped.hEvent = instance->event;
		return instance;
	}

	static void closeInstance(Instance& instance) {
		if (instance.in_flight) {
			DWORD ignored = 0;
			::CancelIoEx(instance.pipe, &instance.overlapped);
			::GetOverlappedResult(instance.pipe, &instance.overlapped, &ignored, TRUE);
		}

		::CloseHandle(instance.pipe);
		::CloseHandle(instance.event);
	}

	static void closeAll(std::vector<std::unique_ptr<Instance>>& instances) {
		for (auto& instance : instances) closeInstance(*instance);
		instances.clear();
	}

	// Instances are created where the name must already exist, but only
	// the relay thread issues I/O on them, so all of it can be cancelled
	// and waited for there.
	static void addListener(Relay& relay, bool first) {
		std::unique_ptr<Instance> instance = openInstance(relay, first);
		if (instance != nullptr) relay.listening.push_back(std::move(instance));
	}

	static void listenOnAll(Relay& relay) {
		for (auto& instance : relay.listening) startListening(*instance);
	}

	static bool startTransfer(Relay& relay, Instance& instance) {
		instance.overlapped = OVERLAPPED{};
		instance.overlapped.hEvent = instance.event;
		instance.in_flight = false;
		::ResetEvent(instance.event);

		BOOL ok = FALSE;
		if (relay.outbound) {
			const DWORD size = static_cast<DWORD>(relay.pending.size());
			ok = ::WriteFile(instance.pipe, relay.pending.data(), size, nullptr,
				&instance.overlapped);
		} else {
			instance.buffer.resize(kChunkBytes);
			ok = ::ReadFile(instance.pipe, instance.buffer.data(), kChunkBytes, nullptr,
				&instance.overlapped);
		}

		instance.in_flight = ok || ::GetLastError() == ERROR_IO_PENDING;
		return instance.in_flight;
	}

	static bool finishTransfer(Instance& instance, DWORD& moved) {
		moved = 0;
		const BOOL ok = ::GetOverlappedResult(instance.pipe, &instance.overlapped, &moved, TRUE);
		instance.in_flight = false;
		return ok != 0;
	}

	// The release interrupts a blocked read or write on the anonymous pipe
	// with CancelSynchronousIo; that only counts while no data has moved.
	static bool interrupted() {
		return ::GetLastError() == ERROR_OPERATION_ABORTED;
	}

	static IoResult fillPending(Relay& relay) {
		std::string chunk(kChunkBytes, '\0');
		for (;;) {
			DWORD got = 0;
			if (::ReadFile(relay.local_end, chunk.data(), kChunkBytes, &got, nullptr) && got > 0) {
				relay.pending.assign(chunk.data(), got);
				return IoResult::Moved;
			}

			if (!interrupted()) return IoResult::Failed;
			if (!relay.delivered) return IoResult::Cancelled;
		}
	}

	static bool writeToProcess(Relay& relay, const char* data, DWORD size) {
		while (size > 0) {
			DWORD written = 0;
			if (!::WriteFile(relay.local_end, data, size, &written, nullptr)) {
				if (interrupted()) continue;
				return false;
			}

			data += written;
			size -= written;
		}

		return true;
	}

	static std::vector<HANDLE> pickWaits(const Relay& relay) {
		std::vector<HANDLE> waits;
		for (const auto& instance : relay.listening) waits.push_back(instance->event);
		for (const auto& instance : relay.clients) waits.push_back(instance->event);
		waits.push_back(relay.cancel);
		return waits;
	}

	// With no data to give, a client is answered with end-of-file.
	static void acceptClient(Relay& relay, std::size_t index, bool has_data) {
		std::unique_ptr<Instance> client = std::move(relay.listening[index]);
		relay.listening.erase(relay.listening.begin() + static_cast<long>(index));
		addListener(relay, false);
		if (relay.listening.size() == kListeningInstances) startListening(*relay.listening.back());

		if (!has_data || !startTransfer(relay, *client)) {
			closeInstance(*client);
			return;
		}

		relay.clients.push_back(std::move(client));
	}

	// A read of zero bytes moves nothing; the client may still write.
	static bool settleClient(Relay& relay, std::size_t index) {
		Instance& client = *relay.clients[index];
		DWORD moved = 0;
		const bool ok = finishTransfer(client, moved);
		if (ok && (relay.outbound || moved > 0)) {
			relay.first_moved = moved;
			relay.chosen = std::move(relay.clients[index]);
			relay.clients.erase(relay.clients.begin() + static_cast<long>(index));
			return true;
		}

		if (ok && startTransfer(relay, client)) return false;

		closeInstance(client);
		relay.clients.erase(relay.clients.begin() + static_cast<long>(index));
		return false;
	}

	static bool pickClient(Relay& relay, bool has_data) {
		for (;;) {
			const std::vector<HANDLE> waits = pickWaits(relay);
			const DWORD count = static_cast<DWORD>(waits.size());
			const DWORD which = ::WaitForMultipleObjects(count, waits.data(), FALSE, INFINITE);
			const std::size_t index = which - WAIT_OBJECT_0;
			if (index + 1 >= waits.size()) return false;

			if (index < relay.listening.size()) {
				acceptClient(relay, index, has_data);
				continue;
			}

			if (settleClient(relay, index - relay.listening.size())) return true;
		}
	}

	static IoResult transferWithChosen(Relay& relay, DWORD& moved) {
		if (!startTransfer(relay, *relay.chosen)) return IoResult::Failed;
		return finishTransfer(*relay.chosen, moved) ? IoResult::Moved : IoResult::Failed;
	}

	static void streamToReader(Relay& relay) {
		DWORD moved = relay.first_moved;
		for (;;) {
			relay.pending.erase(0, moved);
			if (relay.pending.empty() && fillPending(relay) != IoResult::Moved) return;
			if (transferWithChosen(relay, moved) != IoResult::Moved) return;
		}
	}

	static void streamToProcess(Relay& relay) {
		DWORD moved = relay.first_moved;
		for (;;) {
			if (!writeToProcess(relay, relay.chosen->buffer.data(), moved)) return;
			if (transferWithChosen(relay, moved) != IoResult::Moved) return;
		}
	}

	static void runRelay(Relay& relay) {
		listenOnAll(relay);
		bool has_data = true;
		if (relay.outbound) {
			const IoResult fill = fillPending(relay);
			if (fill == IoResult::Cancelled) return;
			has_data = fill == IoResult::Moved;
		}

		if (!pickClient(relay, has_data)) return;

		relay.delivered = true;
		closeAll(relay.clients);
		closeAll(relay.listening);
		if (relay.outbound) streamToReader(relay);
		else                streamToProcess(relay);
	}

	static void closeRelay(Relay& relay) {
		closeAll(relay.clients);
		closeAll(relay.listening);
		if (relay.chosen != nullptr) closeInstance(*relay.chosen);
		relay.chosen.reset();
		if (relay.local_end != INVALID_HANDLE_VALUE) ::CloseHandle(relay.local_end);
		if (relay.cancel != nullptr) ::CloseHandle(relay.cancel);
	}

	static DWORD WINAPI relayThread(void* parameter) {
		std::unique_ptr<Relay> relay(static_cast<Relay*>(parameter));
		runRelay(*relay);
		closeRelay(*relay);
		return 0;
	}

	static bool createProcessPipe(Relay& relay, HANDLE& child_end) {
		HANDLE read_end = INVALID_HANDLE_VALUE;
		HANDLE write_end = INVALID_HANDLE_VALUE;
		if (!::CreatePipe(&read_end, &write_end, nullptr, kChunkBytes)) return false;

		relay.local_end = relay.outbound ? read_end : write_end;
		child_end = relay.outbound ? write_end : read_end;
		return true;
	}

	static bool openListeners(Relay& relay) {
		addListener(relay, true);
		if (relay.listening.empty()) return false;

		for (std::size_t i = 1; i < kListeningInstances; ++i) addListener(relay, false);
		return true;
	}

	static void abandonRelay(Relay& relay, HANDLE child_end) {
		closeRelay(relay);
		if (child_end != INVALID_HANDLE_VALUE) ::CloseHandle(child_end);
	}

}  // namespace wbsh::procsubst_detail

namespace wbsh {

	using procsubst_detail::Relay;

	static bool prepareCancel(Relay& relay, ProcSubstRelay& out) {
		out.cancel = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (out.cancel == nullptr) return false;

		const HANDLE self = ::GetCurrentProcess();
		if (::DuplicateHandle(self, out.cancel, self, &relay.cancel, 0, FALSE,
		                      DUPLICATE_SAME_ACCESS)) {
			return true;
		}

		::CloseHandle(out.cancel);
		out.cancel = nullptr;
		return false;
	}

	// From CreateThread on, the relay belongs to its thread.
	static bool launchRelay(std::unique_ptr<Relay>& relay, ProcSubstRelay& out) {
		Relay* owned = relay.release();
		out.thread = ::CreateThread(nullptr, 0, procsubst_detail::relayThread, owned, 0, nullptr);
		if (out.thread != nullptr) return true;

		relay.reset(owned);
		::CloseHandle(out.cancel);
		out.cancel = nullptr;
		return false;
	}

	bool startProcSubstRelay(char direction, ProcSubstRelay& out) {
		auto relay = std::make_unique<Relay>();
		relay->outbound = direction == '<';
		relay->name = procsubst_detail::uniquePipeName();

		HANDLE child_end = INVALID_HANDLE_VALUE;
		const bool ready = procsubst_detail::createProcessPipe(*relay, child_end)
			&& procsubst_detail::openListeners(*relay)
			&& prepareCancel(*relay, out);
		const std::string path = wideToUtf8(relay->name);
		if (!ready || !launchRelay(relay, out)) {
			procsubst_detail::abandonRelay(*relay, child_end);
			return false;
		}

		out.path = path;
		out.child_end = child_end;
		return true;
	}

	void releaseProcSubstRelay(ProcSubstRelay& relay) {
		if (relay.cancel != nullptr) {
			::SetEvent(relay.cancel);
			::CloseHandle(relay.cancel);
			relay.cancel = nullptr;
		}

		if (relay.thread != nullptr) {
			::CancelSynchronousIo(relay.thread);
			::CloseHandle(relay.thread);
			relay.thread = nullptr;
		}
	}

}  // namespace wbsh

#endif /* _WIN32 */

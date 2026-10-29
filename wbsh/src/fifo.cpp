/**
 * @file fifo.cpp
 * @brief FIFO marker files and the named-pipe meeting of their two ends.
 */

#include "fifo.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "interrupt.h"
#include "pathconv.h"

namespace wbsh {

	namespace fifo_detail {

		enum class WriterState { Waiting, Arrived, Failed };

	}  // namespace fifo_detail

	using fifo_detail::WriterState;

	static const char kMarker[] = "!<wbsh-fifo>\n";
	static const DWORD kMarkerBytes = sizeof(kMarker) - 1;
	static const DWORD kPipeBufferBytes = 64 * 1024;
	static const unsigned long kPollMs = 10;
	static const DWORD kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

	static std::wstring longPath(const std::filesystem::path& native) {
		return longPathSafe(native).native();
	}

	static bool readMarker(const std::filesystem::path& native) {
		const HANDLE file = ::CreateFileW(longPath(native).c_str(), GENERIC_READ, kShareAll,
			nullptr, OPEN_EXISTING, 0, nullptr);
		if (file == INVALID_HANDLE_VALUE) return false;

		char content[sizeof(kMarker)] = {};
		DWORD got = 0;
		const BOOL read = ::ReadFile(file, content, sizeof(content), &got, nullptr);
		::CloseHandle(file);
		if (read == 0 || got != kMarkerBytes) return false;
		return std::memcmp(content, kMarker, kMarkerBytes) == 0;
	}

	// The system attribute and the size rule out every ordinary file
	// before anything is opened.
	bool isFifoFile(const std::filesystem::path& native) {
		if (isDevicePath(native)) return false;

		WIN32_FILE_ATTRIBUTE_DATA data{};
		if (::GetFileAttributesExW(longPath(native).c_str(), GetFileExInfoStandard, &data) == 0) {
			return false;
		}

		const DWORD unwanted = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
		if ((data.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) == 0) return false;
		if ((data.dwFileAttributes & unwanted) != 0) return false;
		if (data.nFileSizeHigh != 0 || data.nFileSizeLow != kMarkerBytes) return false;

		return readMarker(native);
	}

	static std::error_code lastError() {
		return std::error_code(static_cast<int>(::GetLastError()), std::system_category());
	}

	bool createFifoFile(const std::filesystem::path& native, std::error_code& ec) {
		const HANDLE file = ::CreateFileW(longPath(native).c_str(), GENERIC_WRITE, 0, nullptr,
			CREATE_NEW, FILE_ATTRIBUTE_SYSTEM, nullptr);
		if (file == INVALID_HANDLE_VALUE) {
			ec = ::GetLastError() == ERROR_FILE_EXISTS
				? std::make_error_code(std::errc::file_exists) : lastError();
			return false;
		}

		DWORD written = 0;
		const bool ok = ::WriteFile(file, kMarker, kMarkerBytes, &written, nullptr) != 0
			&& written == kMarkerBytes;
		if (!ok) ec = lastError();
		::CloseHandle(file);
		if (!ok) ::DeleteFileW(longPath(native).c_str());
		return ok;
	}

	static bool pipeNameFor(const std::filesystem::path& native, std::wstring& name) {
		const HANDLE file = ::CreateFileW(longPath(native).c_str(), FILE_READ_ATTRIBUTES, kShareAll,
			nullptr, OPEN_EXISTING, 0, nullptr);
		if (file == INVALID_HANDLE_VALUE) return false;

		BY_HANDLE_FILE_INFORMATION info{};
		const bool known = ::GetFileInformationByHandle(file, &info) != 0;
		::CloseHandle(file);
		if (!known) return false;

		wchar_t text[64];
		std::swprintf(text, sizeof(text) / sizeof(text[0]),
			L"\\\\.\\pipe\\wbsh-fifo-%08lx-%08lx%08lx", info.dwVolumeSerialNumber,
			info.nFileIndexHigh, info.nFileIndexLow);
		name = text;
		return true;
	}

	static HANDLE failWith(int error, HANDLE to_close = INVALID_HANDLE_VALUE) {
		if (to_close != INVALID_HANDLE_VALUE) ::CloseHandle(to_close);
		errno = error;
		return INVALID_HANDLE_VALUE;
	}

	static HANDLE createInstance(const std::wstring& name) {
		const DWORD mode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT
			| PIPE_REJECT_REMOTE_CLIENTS;
		return ::CreateNamedPipeW(name.c_str(), PIPE_ACCESS_INBOUND, mode,
			PIPE_UNLIMITED_INSTANCES, kPipeBufferBytes, kPipeBufferBytes, 0, nullptr);
	}

	// ConnectNamedPipe would block past Ctrl+C or a keeper's closing, so
	// waits peek instead: an instance still waiting is a "bad pipe", one
	// with its writer peeks fine, and one whose writer has been and gone
	// is broken while still holding what that writer wrote.
	static WriterState writerState(HANDLE pipe) {
		DWORD available = 0;
		if (::PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) != 0) {
			return WriterState::Arrived;
		}

		const DWORD error = ::GetLastError();
		if (error == ERROR_BROKEN_PIPE) return WriterState::Arrived;
		if (error == ERROR_BAD_PIPE || error == ERROR_PIPE_LISTENING) return WriterState::Waiting;
		return WriterState::Failed;
	}

	static bool awaitWriter(HANDLE pipe) {
		for (;;) {
			const WriterState state = writerState(pipe);
			if (state != WriterState::Waiting) return state == WriterState::Arrived;
			if (!sleepUnlessCtrlC(kPollMs)) return false;
		}
	}

	static HANDLE serveReaderEnd(const std::wstring& name) {
		const HANDLE pipe = createInstance(name);
		if (pipe == INVALID_HANDLE_VALUE) return failWith(EACCES);
		if (!awaitWriter(pipe)) return failWith(ctrlCPending() ? EINTR : EIO, pipe);
		return pipe;
	}

	// Until a reader serves the pipe it does not exist; while every
	// instance has its writer, it is busy.
	static HANDLE connectWriterEnd(const std::wstring& name, bool interruptible) {
		for (;;) {
			const HANDLE pipe = ::CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr,
				OPEN_EXISTING, 0, nullptr);
			if (pipe != INVALID_HANDLE_VALUE) return pipe;

			const DWORD error = ::GetLastError();
			if (error == ERROR_PIPE_BUSY) ::WaitNamedPipeW(name.c_str(), kPollMs);
			else if (error != ERROR_FILE_NOT_FOUND) return failWith(EACCES);

			if (!interruptible) ::Sleep(kPollMs);
			else if (!sleepUnlessCtrlC(kPollMs)) return failWith(EINTR);
		}
	}

	// A zero-byte write fails once the pipe's reader has closed it.
	static bool readerOpen(HANDLE sink) {
		DWORD written = 0;
		return ::WriteFile(sink, "", 0, &written, nullptr) != 0;
	}

	// Returns once a writer is there, the keeper's own descriptor has
	// closed, or the pipe failed.
	static WriterState awaitKeptWriter(HANDLE pipe, HANDLE sink) {
		for (;;) {
			const WriterState state = writerState(pipe);
			if (state != WriterState::Waiting || !readerOpen(sink)) return state;
			::Sleep(kPollMs);
		}
	}

	// True once the writer is done; false when the sink's reader has gone.
	static bool copyWriter(HANDLE pipe, HANDLE sink) {
		std::vector<char> buffer(kPipeBufferBytes);
		for (;;) {
			DWORD got = 0;
			const BOOL read = ::ReadFile(pipe, buffer.data(), kPipeBufferBytes, &got, nullptr);
			if (read == 0 || got == 0) return true;

			DWORD written = 0;
			if (::WriteFile(sink, buffer.data(), got, &written, nullptr) == 0) return false;
		}
	}

	// A writer can still reach the keeper in the moment between its
	// descriptor closing and the keeper noticing; what it writes goes on
	// to the FIFO's next reader, as if it had opened the FIFO then.
	static void passOn(const std::wstring& name, HANDLE pipe) {
		const HANDLE next = connectWriterEnd(name, false);
		if (next == INVALID_HANDLE_VALUE) return;

		copyWriter(pipe, next);
		::CloseHandle(next);
	}

	// One writer of a kept FIFO; false once the keeping is over.
	static bool keepOneWriter(const std::wstring& name, HANDLE pipe, HANDLE sink) {
		const WriterState state = awaitKeptWriter(pipe, sink);
		if (state == WriterState::Failed) return false;

		const bool keeping = readerOpen(sink);
		if (writerState(pipe) != WriterState::Arrived) return keeping;
		if (keeping) return copyWriter(pipe, sink);

		passOn(name, pipe);
		return false;
	}

	static void runKeeper(std::wstring name, HANDLE sink) {
		bool keeping = true;
		while (keeping) {
			const HANDLE pipe = createInstance(name);
			if (pipe == INVALID_HANDLE_VALUE) break;

			keeping = keepOneWriter(name, pipe, sink);
			::CloseHandle(pipe);
		}

		::CloseHandle(sink);
	}

	// Opened for reading and writing, a FIFO counts as its own writer, so
	// opening never waits and the reader never sees end of file: writers
	// come and go through a thread that copies each into a pipe of the
	// shell's own, whose read end is the descriptor.
	static HANDLE keepFifoOpen(const std::wstring& name) {
		HANDLE read_end = INVALID_HANDLE_VALUE;
		HANDLE write_end = INVALID_HANDLE_VALUE;
		if (::CreatePipe(&read_end, &write_end, nullptr, kPipeBufferBytes) == 0) {
			return failWith(EMFILE);
		}

		std::thread(runKeeper, name, write_end).detach();
		return read_end;
	}
	static bool isReadOnly(const std::filesystem::path& native) {
		const DWORD attributes = ::GetFileAttributesW(longPath(native).c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES) return false;
		return (attributes & FILE_ATTRIBUTE_READONLY) != 0;
	}

	int openFifo(const std::filesystem::path& native, int flags) {
		std::wstring name;
		if (!pipeNameFor(native, name)) {
			errno = ENOENT;
			return -1;
		}

		const int access = flags & (_O_RDONLY | _O_WRONLY | _O_RDWR);
		if (access != _O_RDONLY && isReadOnly(native)) {
			errno = EACCES;
			return -1;
		}

		HANDLE pipe = INVALID_HANDLE_VALUE;
		if (access == _O_WRONLY) pipe = connectWriterEnd(name, true);
		else if (access == _O_RDWR) pipe = keepFifoOpen(name);
		else pipe = serveReaderEnd(name);
		if (pipe == INVALID_HANDLE_VALUE) return -1;

		const int fd_access = access == _O_WRONLY ? _O_WRONLY : _O_RDONLY;
		const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(pipe), fd_access | _O_BINARY);
		if (fd < 0) ::CloseHandle(pipe);
		return fd;
	}

}  // namespace wbsh

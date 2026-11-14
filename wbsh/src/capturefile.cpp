/**
 * @file capturefile.cpp
 * @brief The temp files `$(...)` output is captured in, kept for reuse.
 */

#include "capturefile.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace wbsh {

	namespace capturefile_detail {

		struct ObjectBasicInformation {
			ULONG         attributes;
			ACCESS_MASK   granted_access;
			ULONG         handle_count;
			ULONG         pointer_count;
			ULONG         charges_and_sizes[8];
			LARGE_INTEGER creation_time;
		};

		using QueryObjectFn = LONG (WINAPI*)(HANDLE, int, void*, ULONG, ULONG*);

		static const int kObjectBasicInformation = 0;
		static const int kMaxNameAttempts = 64;

		static std::mutex         g_lock;
		static std::vector<HANDLE> g_idle;
		static std::atomic<unsigned long> g_serial{ 0 };

		static QueryObjectFn queryObject() {
			static const QueryObjectFn query = reinterpret_cast<QueryObjectFn>(
				reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"),
					"NtQueryObject")));
			return query;
		}

		// Only our own handle left means no process can write into it later.
		static bool heldOnlyHere(HANDLE file) {
			const QueryObjectFn query = queryObject();
			if (query == nullptr) return false;

			ObjectBasicInformation info{};
			const LONG status = query(file, kObjectBasicInformation, &info, sizeof(info), nullptr);
			return status >= 0 && info.handle_count == 1;
		}

		static std::wstring tempDirectory() {
			wchar_t dir[MAX_PATH + 1] = {};
			const DWORD length = ::GetTempPathW(MAX_PATH + 1, dir);
			if (length == 0 || length > MAX_PATH) return std::wstring();

			return std::wstring(dir, length);
		}

		static std::wstring captureName(const std::wstring& dir) {
			return dir + L"wbsh-capture-" + std::to_wstring(::GetCurrentProcessId()) + L"-"
				+ std::to_wstring(++g_serial) + L".tmp";
		}

		static HANDLE createNamed(const std::wstring& path) {
			const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
			const DWORD flags = FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE;
			return ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, share, nullptr,
				CREATE_NEW, flags, nullptr);
		}

		static HANDLE createCaptureFile() {
			const std::wstring dir = tempDirectory();
			if (dir.empty()) return nullptr;

			for (int attempt = 0; attempt < kMaxNameAttempts; ++attempt) {
				const HANDLE file = createNamed(captureName(dir));
				if (file != INVALID_HANDLE_VALUE) return file;
				if (::GetLastError() != ERROR_FILE_EXISTS) return nullptr;
			}

			return nullptr;
		}

		static bool truncate(HANDLE file) {
			LARGE_INTEGER start{};
			return ::SetFilePointerEx(file, start, nullptr, FILE_BEGIN) != 0
				&& ::SetEndOfFile(file) != 0;
		}

		static HANDLE takeIdle() {
			const std::lock_guard<std::mutex> guard(g_lock);
			if (g_idle.empty()) return nullptr;

			const HANDLE file = g_idle.back();
			g_idle.pop_back();
			return file;
		}

		static const std::size_t kMaxReadChunk = 1u << 30;

		// Reads at @p offset regardless of the shared file position; 0 at
		// the end or on failure.
		static std::size_t readAt(HANDLE file, std::string& out, std::size_t offset) {
			OVERLAPPED at{};
			at.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
			at.OffsetHigh = static_cast<DWORD>(static_cast<unsigned long long>(offset) >> 32);

			DWORD got = 0;
			const std::size_t left = out.size() - offset;
			const DWORD wanted = static_cast<DWORD>(left < kMaxReadChunk ? left : kMaxReadChunk);
			if (::ReadFile(file, out.data() + offset, wanted, &got, &at) == 0) return 0;

			return got;
		}

	}  // namespace capturefile_detail

	void* takeCaptureFile() {
		const HANDLE idle = capturefile_detail::takeIdle();
		if (idle != nullptr && capturefile_detail::truncate(idle)) return idle;
		if (idle != nullptr) ::CloseHandle(idle);

		return capturefile_detail::createCaptureFile();
	}

	// The descriptor has a handle of its own on the same open file, so
	// what is written through it moves the shared position.
	int openCaptureFd(void* file) {
		HANDLE copy = nullptr;
		const HANDLE self = ::GetCurrentProcess();
		if (::DuplicateHandle(self, file, self, &copy, 0, FALSE, DUPLICATE_SAME_ACCESS) == 0) {
			return -1;
		}

		const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(copy), _O_WRONLY | _O_BINARY);
		if (fd < 0) ::CloseHandle(copy);
		return fd;
	}

	std::string readCaptureFile(void* file) {
		LARGE_INTEGER size{};
		if (::GetFileSizeEx(file, &size) == 0 || size.QuadPart <= 0) return std::string();

		std::string out(static_cast<std::size_t>(size.QuadPart), '\0');
		std::size_t offset = 0;
		while (offset < out.size()) {
			const std::size_t got = capturefile_detail::readAt(file, out, offset);
			if (got == 0) break;

			offset += got;
		}

		out.resize(offset);
		return out;
	}

	void releaseCaptureFile(void* file) {
		if (!capturefile_detail::heldOnlyHere(file)) {
			::CloseHandle(file);
			return;
		}

		const std::lock_guard<std::mutex> guard(capturefile_detail::g_lock);
		capturefile_detail::g_idle.push_back(file);
	}

}  // namespace wbsh

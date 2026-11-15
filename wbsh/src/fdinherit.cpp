/**
 * @file fdinherit.cpp
 * @brief The descriptors above 2 that the shell's children get.
 */

#include "fdinherit.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <io.h>
#endif /* _WIN32 */

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace wbsh {

#ifdef _WIN32

	namespace fdinherit_detail {

		// The C runtime's own per-fd flag bits, which lpReserved2 carries
		// unchanged into the child's fd table.
		static const unsigned char kCrtOpen   = 0x01;
		static const unsigned char kCrtPipe   = 0x08;
		static const unsigned char kCrtDevice = 0x40;

		static const int kFirstPassedFd = 3;
		// lpReserved2's size is a WORD: a count, then a flag byte and a
		// handle per fd.
		static const int kLastPassedFd =
			static_cast<int>((0xFFFF - sizeof(int)) / (1 + sizeof(std::intptr_t))) - 1;

		static std::atomic<int> highest_passed_fd{ -1 };

		static HANDLE fdHandle(int fd) {
			return reinterpret_cast<HANDLE>(_get_osfhandle(fd));
		}

		static int inheritedFdCount() {
			STARTUPINFOW info{};
			::GetStartupInfoW(&info);
			if (info.lpReserved2 == nullptr || info.cbReserved2 < sizeof(int)) return 0;

			int count = 0;
			std::memcpy(&count, info.lpReserved2, sizeof(count));
			return count;
		}

		// Descriptors this process inherited go on to its children too.
		static int highestCandidateFd() {
			static const int inherited_top = inheritedFdCount() - 1;
			const int top = std::max(inherited_top, highest_passed_fd.load());
			return std::min(top, kLastPassedFd);
		}

		static void notePassedFd(int fd) {
			int seen = highest_passed_fd.load();
			while (fd > seen) {
				if (highest_passed_fd.compare_exchange_weak(seen, fd)) return;
			}
		}

		static bool isInheritable(HANDLE handle) {
			DWORD flags = 0;
			if (::GetHandleInformation(handle, &flags) == 0) return false;

			return (flags & HANDLE_FLAG_INHERIT) != 0;
		}

		static HANDLE passedHandle(int fd) {
			const HANDLE handle = fdHandle(fd);
			if (handle == INVALID_HANDLE_VALUE || handle == nullptr) return INVALID_HANDLE_VALUE;

			return isInheritable(handle) ? handle : INVALID_HANDLE_VALUE;
		}

		static unsigned char crtFlags(HANDLE handle) {
			switch (::GetFileType(handle)) {
				case FILE_TYPE_PIPE: return kCrtOpen | kCrtPipe;
				case FILE_TYPE_CHAR: return kCrtOpen | kCrtDevice;
				default:             return kCrtOpen;
			}
		}

		// Entries 0 to 2 stay empty: the child takes those from the
		// standard handles in its STARTUPINFO.
		static void writeBlock(const std::vector<HANDLE>& table, std::vector<unsigned char>& block) {
			const int count = static_cast<int>(table.size());
			block.resize(sizeof(int) + table.size() * (1 + sizeof(std::intptr_t)));
			std::memcpy(block.data(), &count, sizeof(count));

			unsigned char* flags = block.data() + sizeof(int);
			unsigned char* handles = flags + table.size();
			for (std::size_t fd = 0; fd < table.size(); ++fd) {
				const HANDLE handle = table[fd];
				flags[fd] = handle == INVALID_HANDLE_VALUE ? 0 : crtFlags(handle);

				const std::intptr_t value = reinterpret_cast<std::intptr_t>(handle);
				std::memcpy(handles + fd * sizeof(value), &value, sizeof(value));
			}
		}

	}  // namespace fdinherit_detail

	void passFdToChildren(int fd) {
		if (fd < fdinherit_detail::kFirstPassedFd) return;

		const HANDLE handle = fdinherit_detail::fdHandle(fd);
		if (handle == INVALID_HANDLE_VALUE || handle == nullptr) return;

		::SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
		fdinherit_detail::notePassedFd(fd);
	}

	void keepFdFromChildren(int fd) {
		const HANDLE handle = fdinherit_detail::fdHandle(fd);
		if (handle == INVALID_HANDLE_VALUE || handle == nullptr) return;

		::SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);
	}

	ChildFds collectChildFds() {
		std::vector<HANDLE> table(fdinherit_detail::kFirstPassedFd, INVALID_HANDLE_VALUE);
		ChildFds fds;
		const int top = fdinherit_detail::highestCandidateFd();
		for (int fd = fdinherit_detail::kFirstPassedFd; fd <= top; ++fd) {
			const HANDLE handle = fdinherit_detail::passedHandle(fd);
			table.push_back(handle);
			if (handle != INVALID_HANDLE_VALUE) fds.handles.push_back(handle);
		}

		if (fds.handles.empty()) return fds;

		while (table.back() == INVALID_HANDLE_VALUE) table.pop_back();
		fdinherit_detail::writeBlock(table, fds.block);
		return fds;
	}

#else

	void passFdToChildren(int) {
	}

	void keepFdFromChildren(int) {
	}

	ChildFds collectChildFds() {
		return ChildFds();
	}

#endif /* _WIN32 */

}  // namespace wbsh

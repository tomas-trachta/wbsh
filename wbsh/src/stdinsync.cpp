/**
 * @file stdinsync.cpp
 * @brief Keeping the CRT's `stdin` buffer in step with file descriptor 0.
 */

#include "stdinsync.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>

#  include <io.h>
#endif /* _WIN32 */

#include <cstdio>

namespace wbsh::stdinsync_detail {

	struct StreamBuffer {
		char** base = nullptr;
		char** ptr = nullptr;
		int* count = nullptr;
	};

	static bool stdinBuffer(StreamBuffer& buffer) {
		if (_get_stream_buffer_pointers(stdin, &buffer.base, &buffer.ptr, &buffer.count) != 0) {
			return false;
		}

		return buffer.base != nullptr && buffer.ptr != nullptr && buffer.count != nullptr;
	}

	static int bufferedBytes(const StreamBuffer& buffer) {
		return *buffer.count > 0 ? *buffer.count : 0;
	}

	// The next read refills the buffer from wherever fd 0 then is.
	static void dropBuffer(StreamBuffer& buffer) {
		*buffer.count = 0;
		*buffer.ptr = *buffer.base;
	}

	static bool stdinIsFile() {
		const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(0));
		return handle != INVALID_HANDLE_VALUE && GetFileType(handle) == FILE_TYPE_DISK;
	}

}  // namespace wbsh::stdinsync_detail

namespace wbsh {

	using stdinsync_detail::StreamBuffer;

	int stdinBufferedBytes() {
		StreamBuffer buffer;
		return stdinsync_detail::stdinBuffer(buffer) ? stdinsync_detail::bufferedBytes(buffer) : 0;
	}

	// fseek() cannot be used: a target inside the buffer only moves the
	// buffer pointer and leaves fd 0 where the read-ahead left it. A file
	// on fd 0 is binary, so the buffered bytes are the bytes to step back.
	void giveBackStdinReadAhead() {
		StreamBuffer buffer;
		if (!stdinsync_detail::stdinBuffer(buffer)) return;

		const int buffered = stdinsync_detail::bufferedBytes(buffer);
		if (buffered == 0 || !stdinsync_detail::stdinIsFile()) return;

		const long long physical = _lseeki64(0, 0, SEEK_CUR);
		if (physical < buffered) return;

		stdinsync_detail::dropBuffer(buffer);
		_lseeki64(0, physical - buffered, SEEK_SET);
	}

	void resetStdinBuffer() {
		giveBackStdinReadAhead();

		StreamBuffer buffer;
		if (stdinsync_detail::stdinBuffer(buffer)) stdinsync_detail::dropBuffer(buffer);
		std::clearerr(stdin);
	}

}  // namespace wbsh

#pragma once

/**
 * @file fifo.h
 * @brief FIFOs (mkfifo) as marker files joined through Windows named pipes.
 *
 * Windows keeps named pipes in \\.\pipe\, outside every file system, so a
 * FIFO is a small system file holding a marker. Opening it from wbsh (a
 * redirection or a bundled command) meets the other end on the pipe named
 * after the file's identity, \\.\pipe\wbsh-fifo-<volume>-<index>: a
 * reader serves the pipe and waits for a writer, a writer waits for a
 * reader, and the reader sees end of file once its writer closes. Each
 * connection joins one reader to one writer; a writer that comes while
 * another is connected waits for the next reader. Ctrl+C ends the wait.
 * Programs outside wbsh see the marker file.
 */

#include <filesystem>
#include <system_error>

namespace wbsh {

	bool isFifoFile(const std::filesystem::path& native);

	/// A new FIFO at @p native; fails with EEXIST when anything is there.
	bool createFifoFile(const std::filesystem::path& native, std::error_code& ec);

	/**
	 * @brief A descriptor on the FIFO at @p native, opened with the _open
	 *        access in @p flags: read-only waits for a writer, write-only
	 *        for a reader, read-write waits for neither.
	 * @return -1 with errno set: EINTR when Ctrl+C ended the wait.
	 */
	int openFifo(const std::filesystem::path& native, int flags);

}  // namespace wbsh

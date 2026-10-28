#pragma once

/**
 * @file stdinsync.h
 * @brief Keeping the CRT's `stdin` buffer in step with file descriptor 0.
 *
 * In-process commands (`head`, `cat`, `mapfile`, ...) read fd 0 through
 * the CRT's FILE buffer, which reads ahead. Whatever reads fd 0 directly
 * afterwards (`read`, an external command, a redirection that points
 * fd 0 elsewhere) must first get that read-ahead back.
 */

namespace wbsh {

	/// Bytes the CRT has read from fd 0 that no command consumed yet.
	int  stdinBufferedBytes();

	/// Before a child inherits fd 0: when fd 0 is a file, seek it back to
	/// the first unconsumed byte. A pipe keeps its read-ahead in the buffer.
	void giveBackStdinReadAhead();

	/// Before fd 0 is pointed elsewhere: give a file's read-ahead back and
	/// drop a pipe's, which belongs to the stream being replaced.
	void resetStdinBuffer();

}  // namespace wbsh

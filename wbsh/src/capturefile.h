#pragma once

/**
 * @file capturefile.h
 * @brief The temp files `$(...)` output is captured in, kept for reuse.
 *
 * Creating, reopening and deleting a named temp file for every command
 * substitution cost most of its time. A capture file here is opened
 * once with delete-on-close and emptied for the next substitution, as
 * long as no other process still holds it.
 */

#include <string>

namespace wbsh {

	/// An empty capture file, or null when none could be created.
	void* takeCaptureFile();

	/// A CRT descriptor writing into @p file; -1 on failure.
	int openCaptureFd(void* file);

	/// Everything written into @p file since it was taken.
	std::string readCaptureFile(void* file);

	/// Keeps @p file for the next capture, or closes it when a process the
	/// substitution started (a background job) can still write to it.
	void releaseCaptureFile(void* file);

}  // namespace wbsh

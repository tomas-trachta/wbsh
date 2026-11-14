#pragma once

/**
 * @file archivepath.h
 * @brief Keeping extracted archive members inside the extraction directory,
 *        shared by tar, cpio and unzip.
 */

#include <string>

namespace wbsh {

	/// Turns backslashes into slashes and drops a leading drive (`C:`) and the
	/// leading slashes of a rooted or UNC name; true when something was dropped.
	bool stripMemberRoot(std::string& name);

	/// A `..` component (or a run of dots and spaces Windows may trim to `..`),
	/// with either slash as separator.
	bool memberEscapes(const std::string& name);

	/// A link target that may point out of the extraction directory: rooted,
	/// drive-qualified, or with a `..` component.
	bool isEscapingLinkTarget(const std::string& target);

}  // namespace wbsh

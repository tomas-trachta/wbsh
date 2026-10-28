#pragma once

/**
 * @file filetimes.h
 * @brief A file's four timestamps with full precision, read and set.
 */

#include <filesystem>

#include "dateparse.h"

namespace wbsh {

	struct FileTimes {
		DateValue access;
		DateValue modify;
		DateValue change;
		DateValue birth;
	};

	/// The times of @p native; with @p follow a symbolic link is resolved.
	bool readFileTimes(const std::filesystem::path& native, bool follow, FileTimes& times);

	/// A std::filesystem time stamp as epoch seconds and nanoseconds.
	DateValue dateFromFileTime(const std::filesystem::file_time_type& time);

	/// Set the access and / or modification time of @p native; nullptr keeps one.
	bool writeFileTimes(const std::filesystem::path& native, const DateValue* access,
	                    const DateValue* modify);

}  // namespace wbsh

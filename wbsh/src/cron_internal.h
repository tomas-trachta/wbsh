#pragma once

/**
 * @file cron_internal.h
 * @brief What crontab and crond share: the crontab directory, its files, and time zones.
 *
 * A crontab directory holds one crontab per user, named after the user.
 * crond's own files sit beside them under names starting with a dot,
 * which never name a crontab: .crond.pid (also the one-crond-per-
 * directory lock), .crond.reboot and the default log .crond.log.
 */

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "cron_schedule.h"

namespace wbsh {

	class Executor;

	/// @p option (-c DIR) when given, else $WBSH_CRONTAB_DIR, else
	/// %APPDATA%\\wbsh\\crontabs (~/.wbsh/crontabs without APPDATA); absolute.
	std::filesystem::path crontabDirectory(Executor& exec, const std::string& option);

	/// Whether @p name can be a crontab in the directory (not one of crond's dot files).
	bool isCrontabName(const std::string& name);

	bool readWholeFile(const std::filesystem::path& path, std::string& text, std::error_code& ec);

	/// Replace @p path by renaming a finished temporary sibling over it,
	/// creating the directory first when it is missing.
	bool writeWholeFile(const std::filesystem::path& path, const std::string& text,
	                    std::error_code& ec);

	/// The zone Windows is set to.
	std::unique_ptr<CronZone> localCronZone();

	/// A Windows time zone by its key name ("UTC", "Central Europe Standard
	/// Time"); nullptr when Windows has no such zone.
	std::unique_ptr<CronZone> namedCronZone(const std::string& key);

	/// "YYYY-MM-DD HH:MM" (or with a T between date and time).
	bool parseCivilMinute(const std::string& text, CivilTime& civil);

	/// The crond builtin (cron_daemon.cpp).
	int runCrond(Executor& exec, const std::vector<std::string>& args);

}  // namespace wbsh

/**
 * @file cron_log.cpp
 * @brief crond's log: stamped lines, and job output appended under a header.
 *
 * Every message is one append of the whole line, opened and closed each
 * time, so the log can be moved away or truncated while crond runs.
 */

#include "crond_internal.h"

#ifdef _WIN32

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>

#include "pathconv.h"

namespace wbsh {

	namespace fs = std::filesystem;

	static std::string timeStamp() {
		const std::time_t now = std::time(nullptr);
		std::tm local{};
		localtime_s(&local, &now);

		char text[32] = {};
		std::strftime(text, sizeof(text), "%b %d %H:%M:%S", &local);
		return text;
	}

	static std::string stampedLine(const std::string& message) {
		return timeStamp() + " crond[" + std::to_string(::GetCurrentProcessId()) + "]: "
			+ message + "\n";
	}

	static bool appendToFile(const fs::path& path, const std::string& text) {
		std::FILE* file = openUtf8(pathToUtf8(path), "ab");
		if (file == nullptr) return false;

		const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
		return std::fclose(file) == 0 && written;
	}

	static void emit(const CrondLog& log, const fs::path& destination, const std::string& text) {
		if (!destination.empty() && appendToFile(destination, text)) return;
		if (!log.file.empty()) {
			appendToFile(log.file, text);
			return;
		}

		std::fputs(text.c_str(), stderr);
		std::fflush(stderr);
	}

	void crondLog(const CrondLog& log, int level, const std::string& message) {
		if (level < log.level) return;
		emit(log, fs::path(), stampedLine(message));
	}

	void appendCrondOutput(const CrondLog& log, const fs::path& destination,
	                       const std::string& header, const std::string& text) {
		std::string block = stampedLine(header) + text;
		if (!text.empty() && text.back() != '\n') block.push_back('\n');
		emit(log, destination, block);
	}

	bool crondLogWritable(const CrondLog& log) {
		return log.file.empty() || appendToFile(log.file, std::string());
	}

}  // namespace wbsh

#endif /* _WIN32 */

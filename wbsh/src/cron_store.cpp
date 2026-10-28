/**
 * @file cron_store.cpp
 * @brief The crontab directory: where it is, which of its names are crontabs, and
 *        reading and replacing them.
 */

#include "cron_internal.h"

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#include "coreutils_internal.h"
#include "executor.h"
#include "pathconv.h"

namespace wbsh {

	namespace fs = std::filesystem;

	static fs::path defaultCrontabDirectory(Executor& exec) {
		const std::string override_dir = exec.env().get("WBSH_CRONTAB_DIR");
		if (!override_dir.empty()) return toNative(exec, override_dir);

		const std::string roaming = exec.env().get("APPDATA");
		if (!roaming.empty()) return toNative(exec, roaming) / "wbsh" / "crontabs";

		return toNative(exec, exec.env().get("HOME")) / ".wbsh" / "crontabs";
	}

	fs::path crontabDirectory(Executor& exec, const std::string& option) {
		const fs::path chosen = option.empty() ? defaultCrontabDirectory(exec)
			: toNative(exec, option);

		std::error_code ec;
		const fs::path absolute = fs::absolute(chosen, ec);
		return ec ? chosen : absolute.lexically_normal();
	}

	bool isCrontabName(const std::string& name) {
		return !name.empty() && name[0] != '.';
	}

	static std::error_code lastErrno() {
		return std::error_code(errno, std::generic_category());
	}

	bool readWholeFile(const fs::path& path, std::string& text, std::error_code& ec) {
		std::FILE* file = openUtf8(pathToUtf8(path), "rb");
		if (file == nullptr) {
			ec = lastErrno();
			return false;
		}

		text.clear();
		char buffer[4096];
		std::size_t got = 0;
		while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, got);

		const bool failed = std::ferror(file) != 0;
		if (failed) ec = lastErrno();
		std::fclose(file);
		return !failed;
	}

	static bool writeAndClose(std::FILE* file, const std::string& text, std::error_code& ec) {
		const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
		if (!written) ec = lastErrno();

		const bool closed = std::fclose(file) == 0;
		if (written && !closed) ec = lastErrno();
		return written && closed;
	}

	bool writeWholeFile(const fs::path& path, const std::string& text, std::error_code& ec) {
		fs::create_directories(path.parent_path(), ec);
		if (ec) return false;

		fs::path temp;
		std::FILE* file = createSiblingTempFile(path, ".crontab.", temp);
		if (file == nullptr) {
			ec = lastErrno();
			return false;
		}

		std::error_code ignored;
		if (!writeAndClose(file, text, ec)) {
			fs::remove(temp, ignored);
			return false;
		}

		fs::rename(temp, path, ec);
		if (ec) fs::remove(temp, ignored);
		return !ec;
	}

}  // namespace wbsh

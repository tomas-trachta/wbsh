/**
 * @file startup.cpp
 * @brief Startup options and startup files of a new shell.
 */

#include "startup.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "environment.h"
#include "executor.h"
#include "pathconv.h"
#include "setup.h"

namespace wbsh {

	namespace fs = std::filesystem;

	static void applyShopts(Executor& exec, const char* flag,
			const std::vector<std::string>& names) {
		if (names.empty()) return;

		std::vector<std::string> args;
		args.reserve(names.size() + 1);
		args.emplace_back(flag);
		args.insert(args.end(), names.begin(), names.end());
		exec.callBuiltin("shopt", args);
	}

	void applyStartupOptions(Executor& exec, const ShellStartup& startup) {
		if (startup.login) exec.env().shopts().login_shell = true;
		if (!startup.set_words.empty()) exec.callBuiltin("set", startup.set_words);

		applyShopts(exec, "-s", startup.shopts_on);
		applyShopts(exec, "-u", startup.shopts_off);
	}

	static bool readTextFile(const fs::path& native, std::string& out_text) {
		std::error_code ec;
		if (!fs::is_regular_file(native, ec)) return false;

		std::ifstream file(native, std::ios::binary);
		if (!file) return false;

		std::stringstream contents;
		contents << file.rdbuf();
		out_text = contents.str();
		return true;
	}

	static void sourceGivenFile(Executor& exec, const std::string& path) {
		std::string text;
		if (!readTextFile(utf8ToPath(exec.pathConv().toWin32(path)), text)) return;

		exec.executeText(text, path);
	}

	void sourceLoginProfile(Executor& exec, const ShellStartup& startup) {
		if (!startup.login || !startup.read_profile) return;

		startLoginShell(exec);
	}

	void sourceRcFile(Executor& exec, const ShellStartup& startup) {
		if (!startup.read_rc || exec.flowPending()) return;

		if (startup.rcfile.empty()) sourceHomeFile(exec, ".wbshrc");
		else                        sourceGivenFile(exec, startup.rcfile);
	}

}  // namespace wbsh

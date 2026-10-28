/**
 * @file historyfile.cpp
 * @brief The history file: its format, appending, atomic rewrites, and the HIST* settings.
 */

#include "history.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif /* _WIN32 */

#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "executor.h"
#include "fnmatch.h"
#include "numparse.h"
#include "pathconv.h"

namespace wbsh {

	// On-disk format:
	//
	//   #!wbsh-history-v3
	//   <status>\t<command>
	//   #<seconds>
	//   <status>\t<command>
	//   ...
	//
	// A `#<seconds>` line gives the time of the entry after it, as in
	// bash's own history files; readers that do not know it skip it.
	// A command typed over several lines is one entry, so v3 writes a
	// newline in it as \n and a backslash as \\. v2 files (the same lines,
	// nothing escaped) and legacy files (no header, one command per line,
	// status 0) still load, which keeps old ~/.wbsh_history files working.
	namespace histfile_detail {

		static const char* const kV2Header = "#!wbsh-history-v2";
		static const char* const kV3Header = "#!wbsh-history-v3";

		enum class Format { kMissing, kLegacy, kV2, kV3 };

		static void stripLineEnding(std::string& line) {
			while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
		}

		static std::string escapeCommand(const std::string& command) {
			std::string escaped;
			escaped.reserve(command.size());
			for (const char letter : command) {
				if (letter == '\\') {
					escaped += "\\\\";
				} else if (letter == '\n') {
					escaped += "\\n";
				} else if (letter != '\r') {
					escaped.push_back(letter);
				}
			}

			return escaped;
		}

		static std::string unescapeCommand(const std::string& text) {
			std::string command;
			command.reserve(text.size());
			for (std::size_t i = 0; i < text.size(); ++i) {
				if (text[i] != '\\' || i + 1 >= text.size()) {
					command.push_back(text[i]);
					continue;
				}

				++i;
				command.push_back(text[i] == 'n' ? '\n' : text[i]);
			}

			return command;
		}

		static Format formatOf(const std::string& first_line) {
			if (first_line == kV3Header) return Format::kV3;
			if (first_line == kV2Header) return Format::kV2;

			return Format::kLegacy;
		}

		static bool parseLine(const std::string& line, Format format, int& status,
		                      std::string& command) {
			status = 0;
			command = line;
			if (format == Format::kLegacy) return true;

			const std::size_t tab = line.find('\t');
			if (tab == std::string::npos || !parseInt(line.substr(0, tab), status)) return false;

			command = line.substr(tab + 1);
			if (format == Format::kV3) command = unescapeCommand(command);
			return !command.empty();
		}

		static std::string formatEntries(const HistoryList& list, std::size_t first,
		                                 bool with_times) {
			std::string text;
			for (std::size_t i = first; i < list.commands.size(); ++i) {
				const int status = i < list.statuses.size() ? list.statuses[i] : 0;
				const long long time = i < list.times.size() ? list.times[i] : 0;
				if (with_times && time > 0) text += "#" + std::to_string(time) + '\n';
				text += std::to_string(status) + '\t' + escapeCommand(list.commands[i]) + '\n';
			}

			return text;
		}

		static bool timeLine(const std::string& line, long long& time) {
			if (line.size() < 2 || line[0] != '#') return false;
			if (std::isdigit(static_cast<unsigned char>(line[1])) == 0) return false;

			return parseLL(line.substr(1), time);
		}

		static Format formatOfFile(const std::string& path) {
			std::ifstream file(utf8ToPath(path), std::ios::binary);
			std::string line;
			if (!file || !std::getline(file, line)) return Format::kMissing;

			stripLineEnding(line);
			return formatOf(line);
		}

		static std::string temporarySibling(const std::string& path) {
#ifdef _WIN32
			return path + ".tmp" + std::to_string(::GetCurrentProcessId());
#else
			return path + ".tmp";
#endif /* _WIN32 */
		}

		static bool writeWhole(const std::string& path, const std::string& text) {
			std::ofstream file(utf8ToPath(path), std::ios::binary | std::ios::trunc);
			if (!file) return false;

			file << text;
			file.close();
			return !file.fail();
		}

		// The file is written beside its destination and renamed over it, so
		// a crash or a full disk leaves either the old file or the new one.
		static bool replaceFile(const std::string& path, const std::string& text) {
			const std::string temporary = temporarySibling(path);
			std::error_code ec;
			if (!writeWhole(temporary, text)) {
				std::filesystem::remove(utf8ToPath(temporary), ec);
				return false;
			}

			std::filesystem::rename(utf8ToPath(temporary), utf8ToPath(path), ec);
			if (!ec) return true;

			std::filesystem::remove(utf8ToPath(temporary), ec);
			return false;
		}

#ifdef _WIN32
		static bool appendText(const std::string& path, const std::string& text) {
			const HANDLE file = ::CreateFileW(utf8ToWide(path).c_str(), FILE_APPEND_DATA,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
				FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE) return false;

			DWORD written = 0;
			const BOOL ok = ::WriteFile(file, text.data(), static_cast<DWORD>(text.size()),
				&written, nullptr);
			::CloseHandle(file);
			return ok && written == text.size();
		}
#else
		static bool appendText(const std::string& path, const std::string& text) {
			std::ofstream file(utf8ToPath(path), std::ios::binary | std::ios::app);
			file << text;
			return static_cast<bool>(file);
		}
#endif /* _WIN32 */

		static bool upgradeToCurrentFormat(const std::string& path) {
			HistoryList existing;
			if (!readHistoryFile(path, existing)) return false;

			return writeHistoryFile(path, existing, 0, historyHasTimes(existing));
		}

		static std::vector<std::string> splitColons(const std::string& text) {
			std::vector<std::string> parts;
			std::size_t start = 0;
			while (start <= text.size()) {
				std::size_t colon = text.find(':', start);
				if (colon == std::string::npos) colon = text.size();

				parts.push_back(text.substr(start, colon - start));
				start = colon + 1;
			}

			return parts;
		}

		static bool hasControl(const HistoryFilter& filter, const char* word) {
			for (const std::string& part : splitColons(filter.control)) {
				if (part == word) return true;
			}

			return false;
		}

		static bool ignoredByPattern(const std::string& ignore, const std::string& entry,
		                             const std::string& newest) {
			if (ignore.empty()) return false;

			for (const std::string& pattern : splitColons(ignore)) {
				if (pattern == "&" && entry == newest) return true;
				if (!pattern.empty() && pattern != "&" && fnmatchFull(pattern, entry)) return true;
			}

			return false;
		}

		struct UnsavedEntry {
			std::mutex  lock;
			std::string path;
			std::string command;
		};

		static UnsavedEntry& unsavedEntry() {
			static UnsavedEntry entry;
			return entry;
		}

		static void writeUnsavedEntry() {
			UnsavedEntry& unsaved = unsavedEntry();
			const std::lock_guard<std::mutex> guard(unsaved.lock);
			if (unsaved.command.empty() || unsaved.path.empty()) return;

			HistoryList list;
			list.commands.push_back(unsaved.command);
			list.statuses.push_back(0);
			list.times.push_back(0);
			appendHistoryFile(unsaved.path, list, 0, false);
			unsaved.command.clear();
		}

#ifdef _WIN32
		// The console ends the process once this returns, so the entry of a
		// command still running is written now or never.
		static BOOL WINAPI closeHandler(DWORD event) {
			const bool ending = event == CTRL_CLOSE_EVENT || event == CTRL_LOGOFF_EVENT
				|| event == CTRL_SHUTDOWN_EVENT;
			if (ending) writeUnsavedEntry();

			return FALSE;
		}
#endif /* _WIN32 */

	}  // namespace histfile_detail

	bool readHistoryFile(const std::string& path, HistoryList& list) {
		std::ifstream file(utf8ToPath(path), std::ios::binary);
		if (!file) return false;

		std::string line;
		bool first = true;
		long long time = 0;
		histfile_detail::Format format = histfile_detail::Format::kLegacy;
		while (std::getline(file, line)) {
			histfile_detail::stripLineEnding(line);
			if (first) {
				first = false;
				format = histfile_detail::formatOf(line);
				if (format != histfile_detail::Format::kLegacy) continue;
			}

			int status = 0;
			std::string command;
			if (line.empty() || histfile_detail::timeLine(line, time)) continue;
			if (!histfile_detail::parseLine(line, format, status, command)) continue;

			list.commands.push_back(std::move(command));
			list.statuses.push_back(status);
			list.times.push_back(time);
			time = 0;
		}

		return true;
	}

	bool historyHasTimes(const HistoryList& list) {
		for (const long long time : list.times) {
			if (time > 0) return true;
		}

		return false;
	}

	bool writeHistoryFile(const std::string& path, const HistoryList& list, std::size_t first,
	                      bool with_times) {
		const std::string text = std::string(histfile_detail::kV3Header) + '\n'
			+ histfile_detail::formatEntries(list, first, with_times);
		return histfile_detail::replaceFile(path, text);
	}

	bool appendHistoryFile(const std::string& path, const HistoryList& list, std::size_t first,
	                       bool with_times) {
		if (first >= list.commands.size()) return true;

		std::string text = histfile_detail::formatEntries(list, first, with_times);
		switch (histfile_detail::formatOfFile(path)) {
		case histfile_detail::Format::kMissing:
			text = std::string(histfile_detail::kV3Header) + '\n' + text;
			break;
		case histfile_detail::Format::kV3:
			break;
		default:
			if (!histfile_detail::upgradeToCurrentFormat(path)) return false;
			break;
		}

		return histfile_detail::appendText(path, text);
	}

	bool trimHistoryFile(const std::string& path, std::size_t keep) {
		HistoryList list;
		if (!readHistoryFile(path, list)) return false;
		if (list.commands.size() <= keep) return true;

		return writeHistoryFile(path, list, list.commands.size() - keep, historyHasTimes(list));
	}

	long long historyLimit(const std::string& value) {
		long long limit = -1;
		if (!parseLL(value, limit)) return -1;

		return limit;
	}

	bool historyAccepts(const HistoryFilter& filter, const std::string& entry,
	                    const std::string& newest) {
		if (entry.empty()) return false;

		const bool ignore_both = histfile_detail::hasControl(filter, "ignoreboth");
		const bool ignore_space = ignore_both || histfile_detail::hasControl(filter, "ignorespace");
		const bool ignore_dups = ignore_both || histfile_detail::hasControl(filter, "ignoredups");
		if (ignore_space && entry[0] == ' ') return false;
		if (ignore_dups && entry == newest) return false;

		return !histfile_detail::ignoredByPattern(filter.ignore, entry, newest);
	}

	bool historyErasesDuplicates(const HistoryFilter& filter) {
		return histfile_detail::hasControl(filter, "erasedups");
	}

	void installHistoryCloseHandler() {
#ifdef _WIN32
		::SetConsoleCtrlHandler(histfile_detail::closeHandler, TRUE);
#endif /* _WIN32 */
	}

	void holdUnsavedHistory(const std::string& path, const std::string& command) {
		histfile_detail::UnsavedEntry& unsaved = histfile_detail::unsavedEntry();
		const std::lock_guard<std::mutex> guard(unsaved.lock);
		unsaved.path = path;
		unsaved.command = command;
	}

	void commitHistory(Executor& exec, const std::string& path) {
		histfile_detail::UnsavedEntry& unsaved = histfile_detail::unsavedEntry();
		const std::lock_guard<std::mutex> guard(unsaved.lock);
		unsaved.command.clear();
		if (!path.empty()) exec.appendNewHistory(path);
	}

}  // namespace wbsh

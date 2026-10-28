/**
 * @file vi_ex_files.cpp
 * @brief vi's file and session commands: :w :wq :x :q :e :r :! :n :prev
 *        :rew :args :f, with the "No write since last change" guards.
 */

#include "vi_internal.h"

#include <cstdio>
#include <string>
#include <vector>

#include "coreutils_internal.h"
#include "executor.h"
#include "pathconv.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif /* _WIN32 */

namespace wbsh {

	namespace vi_detail {

		static ExRange wholeBuffer(const Editor& ed, const ExRange& given) {
			if (given.count > 0) return given;

			ExRange range;
			range.first = 1;
			range.last = ed.lines.size();
			range.count = 2;
			return range;
		}

		static std::string singleQuoted(const std::string& text) {
			std::string quoted = "'";
			for (char c : text) {
				if (c == '\'') quoted += "'\\''";
				else quoted.push_back(c);
			}

			return quoted + "'";
		}

		static std::string rangeText(const Editor& ed, const ExRange& range) {
			std::string text;
			for (std::size_t line = range.first; line <= range.last && line > 0; ++line) {
				text += ed.lines[line - 1] + "\n";
			}

			return text;
		}

		static bool writeTempFile(Editor& ed, const std::string& text, std::string& path) {
			path = makeTempPath(*ed.exec);
			std::FILE* file = path.empty() ? nullptr : _wfopen(utf8ToFileWide(path).c_str(), L"wb");
			if (file == nullptr) {
				setError(ed, "Cannot create a temporary file");
				return false;
			}

			std::fwrite(text.data(), 1, text.size(), file);
			std::fclose(file);
			return true;
		}

		static void removeTempFile(const std::string& path) {
			::DeleteFileW(utf8ToFileWide(path).c_str());
		}

		static bool readTempFile(const std::string& path, std::string& text) {
			std::FILE* file = _wfopen(utf8ToFileWide(path).c_str(), L"rb");
			if (file == nullptr) return false;

			char chunk[65536];
			std::size_t got = 0;
			while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) text.append(chunk, got);
			std::fclose(file);
			return true;
		}

		// The output comes back through a temporary file of our own rather
		// than Executor::runRaw, whose capture file a pipeline stage may
		// fail to create.
		static bool captureOutput(Editor& ed, const std::string& command, std::string& text) {
			const std::string temp = makeTempPath(*ed.exec);
			if (temp.empty()) {
				setError(ed, "Cannot create a temporary file");
				return false;
			}

			std::fflush(stdout);
			ed.exec->executeText("(" + command + "\n) > " + singleQuoted(temp), "vi");
			const bool read = readTempFile(temp, text);
			removeTempFile(temp);
			if (!read) setError(ed, "Cannot read the command's output");
			return read;
		}

		static bool isEscapedSpecial(const std::string& raw, std::size_t at) {
			if (raw[at] != '\\' || at + 1 >= raw.size()) return false;
			return raw[at + 1] == '%' || raw[at + 1] == '!';
		}

		// `!!` is the previous command again and `%` the file name.
		static bool expandCommand(Editor& ed, const std::string& raw, std::string& command) {
			command.clear();
			for (std::size_t at = 0; at < raw.size(); ++at) {
				if (isEscapedSpecial(raw, at)) {
					command.push_back(raw[++at]);
				} else if (raw[at] == '!') {
					command += ed.last_shell_command;
				} else if (raw[at] == '%') {
					command += ed.filename;
				} else {
					command.push_back(raw[at]);
				}
			}

			ed.last_shell_command = command;
			return !command.empty();
		}

		static bool writeToCommand(Editor& ed, const ExCommand& cmd, const std::string& raw) {
			std::string command;
			std::string temp;
			if (!expandCommand(ed, raw, command)) return false;
			if (!writeTempFile(ed, rangeText(ed, wholeBuffer(ed, cmd.range)), temp)) return false;

			runShell(ed, "(" + command + "\n) < " + singleQuoted(temp));
			removeTempFile(temp);
			return true;
		}

		bool exWrite(Editor& ed, const ExCommand& cmd) {
			if (!cmd.args.empty() && cmd.args[0] == '!') {
				return writeToCommand(ed, cmd, cmd.args.substr(1));
			}

			std::string name = cmd.args;
			const bool append = name.compare(0, 2, ">>") == 0;
			if (append) {
				name = name.substr(2);
				while (!name.empty() && name[0] == ' ') name.erase(0, 1);
			}

			if (name.empty()) name = ed.filename;
			return writeFile(ed, name, wholeBuffer(ed, cmd.range), append, cmd.bang);
		}

		static std::size_t filesAfterCurrent(const Editor& ed) {
			return ed.files.size() > ed.file_index + 1 ? ed.files.size() - ed.file_index - 1 : 0;
		}

		static bool quitEditor(Editor& ed, bool force) {
			if (!force && isModified(ed)) {
				setError(ed, "No write since last change (add ! to override)");
				return false;
			}

			const std::size_t more = filesAfterCurrent(ed);
			if (!force && more > 0 && !ed.quit_warning_active) {
				const std::string files = more == 1 ? " more file" : " more files";
				setError(ed, std::to_string(more) + files + " to edit");
				ed.quit_warned = true;
				return false;
			}

			ed.quit = true;
			return true;
		}

		bool exWriteQuit(Editor& ed, const ExCommand& cmd) {
			return exWrite(ed, cmd) && quitEditor(ed, cmd.bang);
		}

		bool exExit(Editor& ed, const ExCommand& cmd) {
			const bool needs_write = isModified(ed) || !cmd.args.empty();
			if (needs_write && !exWrite(ed, cmd)) return false;
			return quitEditor(ed, cmd.bang);
		}

		bool exQuit(Editor& ed, const ExCommand& cmd) {
			return quitEditor(ed, cmd.bang);
		}

		bool exQuitAll(Editor& ed, const ExCommand& cmd) {
			if (!cmd.bang && isModified(ed)) {
				setError(ed, "No write since last change (add ! to override)");
				return false;
			}

			ed.quit = true;
			return true;
		}

		static bool refuseUnsaved(Editor& ed, bool force) {
			if (force || !isModified(ed)) return false;

			setError(ed, "No write since last change (add ! to override)");
			return true;
		}

		bool exEdit(Editor& ed, const ExCommand& cmd) {
			const std::string name = cmd.args.empty() ? ed.filename : cmd.args;
			if (name.empty()) {
				setError(ed, "No file name");
				return false;
			}

			if (refuseUnsaved(ed, cmd.bang)) return false;
			loadFile(ed, name);
			return true;
		}

		static bool commandOutput(Editor& ed, const std::string& raw, std::string& text) {
			std::string command;
			if (!expandCommand(ed, raw, command)) return false;

			suspendTerminal(ed);
			const bool captured = captureOutput(ed, command, text);
			resumeTerminal(ed);
			return captured;
		}

		bool exRead(Editor& ed, const ExCommand& cmd) {
			const std::size_t line = cmd.range.count == 0 ? ed.cursor.row + 1 : cmd.range.last;
			const bool from_command = !cmd.args.empty() && cmd.args[0] == '!';
			const std::string name = cmd.args.empty() ? ed.filename : cmd.args;
			std::string text;
			const bool read = from_command ? commandOutput(ed, cmd.args.substr(1), text)
				: readFileText(ed, name, text);
			if (!read) return false;

			bool crlf = false;
			const std::vector<std::string> lines = splitLines(text, crlf);
			if (lines.empty()) return true;

			insertLineTexts(ed, line, lines);
			moveToFirstNonBlank(ed, line);
			return true;
		}

		static bool filterRange(Editor& ed, const ExCommand& cmd) {
			const ExRange range = cmd.range;
			std::string command;
			std::string temp;
			std::string output;
			if (range.first == 0 || range.last > ed.lines.size() || range.first > range.last) {
				return false;
			}

			if (!expandCommand(ed, cmd.args, command)) return false;
			if (!writeTempFile(ed, rangeText(ed, range), temp)) return false;

			const std::string redirected = "(" + command + "\n) < " + singleQuoted(temp);
			const bool captured = captureOutput(ed, redirected, output);
			removeTempFile(temp);
			if (!captured) return false;

			bool crlf = false;
			const std::vector<std::string> lines = splitLines(output, crlf);
			replaceLines(ed, range.first - 1, range.last - range.first + 1, lines);
			moveToFirstNonBlank(ed, range.first - 1);
			return true;
		}

		bool exBang(Editor& ed, const ExCommand& cmd) {
			if (cmd.range.count > 0) return filterRange(ed, cmd);

			std::string command;
			if (!expandCommand(ed, cmd.args, command)) {
				setError(ed, "No previous command");
				return false;
			}

			runShell(ed, command);
			return true;
		}

		static bool switchToFile(Editor& ed, std::size_t index, bool force) {
			if (refuseUnsaved(ed, force)) return false;

			ed.file_index = index;
			loadFile(ed, ed.files[index]);
			return true;
		}

		static std::vector<std::string> splitWords(const std::string& text) {
			std::vector<std::string> words;
			std::size_t at = 0;
			while (at < text.size()) {
				while (at < text.size() && text[at] == ' ') ++at;
				const std::size_t start = at;
				while (at < text.size() && text[at] != ' ') ++at;
				if (at > start) words.push_back(text.substr(start, at - start));
			}

			return words;
		}

		bool exNext(Editor& ed, const ExCommand& cmd) {
			if (!cmd.args.empty()) {
				if (refuseUnsaved(ed, cmd.bang)) return false;
				ed.files = splitWords(cmd.args);
				return switchToFile(ed, 0, true);
			}

			if (filesAfterCurrent(ed) == 0) {
				setError(ed, "Cannot go beyond last file");
				return false;
			}

			return switchToFile(ed, ed.file_index + 1, cmd.bang);
		}

		bool exWriteNext(Editor& ed, const ExCommand& cmd) {
			ExCommand write = cmd;
			write.args.clear();
			if (!exWrite(ed, write)) return false;

			ExCommand next = cmd;
			next.bang = false;
			return exNext(ed, next);
		}

		bool exPrevious(Editor& ed, const ExCommand& cmd) {
			if (ed.file_index == 0 || ed.files.empty()) {
				setError(ed, "Cannot go before first file");
				return false;
			}

			return switchToFile(ed, ed.file_index - 1, cmd.bang);
		}

		bool exRewind(Editor& ed, const ExCommand& cmd) {
			if (ed.files.empty()) {
				setError(ed, "No file name");
				return false;
			}

			return switchToFile(ed, 0, cmd.bang);
		}

		bool exArgs(Editor& ed, const ExCommand& cmd) {
			static_cast<void>(cmd);
			std::string text;
			for (std::size_t k = 0; k < ed.files.size(); ++k) {
				if (!text.empty()) text += " ";
				text += k == ed.file_index ? "[" + ed.files[k] + "]" : ed.files[k];
			}

			setMessage(ed, text);
			return true;
		}

		bool exFile(Editor& ed, const ExCommand& cmd) {
			if (!cmd.args.empty()) ed.filename = cmd.args;

			showFileInfo(ed);
			return true;
		}

	}  // namespace vi_detail

}  // namespace wbsh

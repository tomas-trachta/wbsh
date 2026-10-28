/**
 * @file ed_buffer.cpp
 * @brief ed's buffer: line edits, marks, undo snapshots, files, shell
 *        commands and the p / l / n listings.
 */

#include "ed_internal.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif /* _WIN32 */

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "coreutils_internal.h"
#include "environment.h"
#include "executor.h"
#include "pathconv.h"

namespace wbsh {

	namespace ed_detail {

		static const std::size_t kListWidth = 72;

		std::size_t lastLine(const EdState& st) {
			return st.lines.size();
		}

		static void markChanged(EdState& st) {
			st.modified = true;
			st.changed = true;
		}

		void insertLines(EdState& st, std::size_t after, const std::vector<std::string>& texts) {
			std::vector<EdLine> fresh;
			fresh.reserve(texts.size());
			for (const std::string& text : texts) {
				EdLine line;
				line.text = text;
				line.id = st.next_id++;
				fresh.push_back(line);
			}

			const auto at = st.lines.begin() + static_cast<std::ptrdiff_t>(after);
			st.lines.insert(at, fresh.begin(), fresh.end());
			markChanged(st);
		}

		void deleteLines(EdState& st, std::size_t first, std::size_t last) {
			const auto begin = st.lines.begin() + static_cast<std::ptrdiff_t>(first - 1);
			const auto end = st.lines.begin() + static_cast<std::ptrdiff_t>(last);
			st.lines.erase(begin, end);
			markChanged(st);
		}

		void replaceLine(EdState& st, std::size_t line, const std::string& text) {
			st.lines[line - 1].text = text;
			markChanged(st);
		}

		// The moved lines keep their identity, so marks travel with them.
		void moveLines(EdState& st, std::size_t first, std::size_t last, std::size_t after) {
			const auto begin = st.lines.begin() + static_cast<std::ptrdiff_t>(first - 1);
			const auto end = st.lines.begin() + static_cast<std::ptrdiff_t>(last);
			const std::vector<EdLine> moved(begin, end);
			st.lines.erase(begin, end);

			const std::size_t count = last - first + 1;
			const std::size_t target = after >= last ? after - count : after;
			const auto at = st.lines.begin() + static_cast<std::ptrdiff_t>(target);
			st.lines.insert(at, moved.begin(), moved.end());
			markChanged(st);
		}

		Snapshot takeSnapshot(const EdState& st) {
			Snapshot snapshot;
			snapshot.lines = st.lines;
			snapshot.current = st.current;
			snapshot.modified = st.modified;
			return snapshot;
		}

		void restoreSnapshot(EdState& st, const Snapshot& snapshot) {
			st.lines = snapshot.lines;
			st.current = snapshot.current;
			st.modified = snapshot.modified;
		}

		static bool isMarkName(char mark) {
			return mark >= 'a' && mark <= 'z';
		}

		bool lineOfMark(const EdState& st, char mark, std::size_t& line) {
			if (!isMarkName(mark)) return false;

			const std::uint64_t id = st.marks[static_cast<std::size_t>(mark - 'a')];
			for (std::size_t k = 0; k < st.lines.size() && id != 0; ++k) {
				if (st.lines[k].id != id) continue;

				line = k + 1;
				return true;
			}

			return false;
		}

		bool setMark(EdState& st, char mark, std::size_t line) {
			if (!isMarkName(mark)) return false;

			st.marks[static_cast<std::size_t>(mark - 'a')] = st.lines[line - 1].id;
			return true;
		}

		static bool everyNewlineHasReturn(const std::string& text) {
			std::size_t newlines = 0;
			std::size_t at = text.find('\n');
			for (; at != std::string::npos; at = text.find('\n', at + 1)) {
				if (at == 0 || text[at - 1] != '\r') return false;
				++newlines;
			}

			return newlines > 0;
		}

		std::vector<std::string> splitText(const std::string& text, bool& crlf, bool& complete) {
			crlf = everyNewlineHasReturn(text);
			complete = text.empty() || text.back() == '\n';

			std::vector<std::string> lines;
			std::size_t start = 0;
			while (start < text.size()) {
				std::size_t end = text.find('\n', start);
				if (end == std::string::npos) end = text.size();

				std::size_t stop = end;
				if (crlf && end < text.size()) --stop;
				lines.push_back(text.substr(start, stop - start));
				start = end + 1;
			}

			return lines;
		}

		bool readFileText(EdState& st, const std::string& name, std::string& text) {
			std::FILE* file = fopenNative(*st.exec, name, "rb");
			if (file == nullptr) {
				perr("ed", name + ": " + std::strerror(errno));
				return fail(st, "Cannot open input file");
			}

			char chunk[65536];
			std::size_t got = 0;
			while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) text.append(chunk, got);

			const bool failed = std::ferror(file) != 0;
			std::fclose(file);
			if (failed) return fail(st, "Cannot read input file");
			return true;
		}

		static std::string singleQuoted(const std::string& text) {
			std::string quoted = "'";
			for (char c : text) {
				if (c == '\'') quoted += "'\\''";
				else quoted.push_back(c);
			}

			return quoted + "'";
		}

		static std::string makeTempPath(Executor& exec) {
			const std::wstring dir = shellTempDirectory(exec).wstring();
			wchar_t path[MAX_PATH];
			if (dir.empty()) return std::string();
			if (::GetTempFileNameW(dir.c_str(), L"ed", 0, path) == 0) return std::string();
			return wideToUtf8(path);
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
		static bool captureOutput(EdState& st, const std::string& command, std::string& text) {
			const std::string temp = makeTempPath(*st.exec);
			if (temp.empty()) return fail(st, "Cannot create temporary file");

			std::fflush(stdout);
			st.exec->executeText("(" + command + "\n) > " + singleQuoted(temp), "ed");
			const bool read = readTempFile(temp, text);
			::DeleteFileW(utf8ToFileWide(temp).c_str());
			if (!read) return fail(st, "Cannot read command output");
			return true;
		}

		bool readSource(EdState& st, const std::string& name, std::string& text) {
			if (name.empty() || name[0] != '!') return readFileText(st, name, text);

			std::string command;
			if (!expandShellCommand(st, name.substr(1), command)) return false;

			return captureOutput(st, command, text);
		}

		static std::string joinedLines(const EdState& st, const Range& range) {
			const char* const ending = st.crlf ? "\r\n" : "\n";
			std::string out;
			for (std::size_t line = range.first; line <= range.second && line > 0; ++line) {
				out += st.lines[line - 1].text;
				out += ending;
			}

			return out;
		}

		void printByteCount(const EdState& st, std::size_t bytes) {
			if (!st.silent) std::printf("%zu\n", bytes);
		}

		bool writeLines(EdState& st, const std::string& name, const Range& range, bool append) {
			const std::string text = joinedLines(st, range);
			std::FILE* file = fopenNative(*st.exec, name, append ? "ab" : "wb");
			if (file == nullptr) {
				perr("ed", name + ": " + std::strerror(errno));
				return fail(st, "Cannot open output file");
			}

			const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
			const bool closed = std::fclose(file) == 0;
			if (!written || !closed) return fail(st, "Cannot write file");

			printByteCount(st, text.size());
			return true;
		}

		// The lines reach the command's stdin through a temporary file.
		bool writeToCommand(EdState& st, const std::string& command, const Range& range) {
			const std::string temp = makeTempPath(*st.exec);
			const std::string text = joinedLines(st, range);
			std::FILE* file = temp.empty() ? nullptr : _wfopen(utf8ToFileWide(temp).c_str(), L"wb");
			if (file == nullptr) return fail(st, "Cannot create temporary file");

			std::fwrite(text.data(), 1, text.size(), file);
			std::fclose(file);

			std::fflush(stdout);
			st.exec->executeText("(" + command + "\n) < " + singleQuoted(temp), "ed");
			std::fflush(stdout);
			::DeleteFileW(utf8ToFileWide(temp).c_str());
			printByteCount(st, text.size());
			return true;
		}

		bool runShellCommand(EdState& st, const std::string& command) {
			std::fflush(stdout);
			st.exec->executeText(command + "\n", "ed");
			std::fflush(stdout);
			return true;
		}

		static bool appendFilename(EdState& st, std::string& command) {
			if (st.filename.empty()) return fail(st, "No current filename");

			command += st.filename;
			return true;
		}

		// `!` first stands for the previous command and an unescaped `%`
		// for the remembered filename; either one gets the result echoed.
		bool expandShellCommand(EdState& st, const std::string& raw, std::string& command) {
			bool expanded = false;
			std::size_t at = 0;
			command.clear();
			if (!raw.empty() && raw[0] == '!') {
				if (st.shell_command.empty()) return fail(st, "No previous command");

				command = st.shell_command;
				expanded = true;
				at = 1;
			}

			for (; at < raw.size(); ++at) {
				if (raw[at] == '\\' && at + 1 < raw.size() && raw[at + 1] == '%') {
					command.push_back('%');
					++at;
				} else if (raw[at] == '%') {
					if (!appendFilename(st, command)) return false;
					expanded = true;
				} else {
					command.push_back(raw[at]);
				}
			}

			st.shell_command = command;
			if (expanded) std::printf("%s\n", command.c_str());
			return true;
		}

		static void appendEscaped(std::string& out, unsigned char byte) {
			static const char kNames[] = "abtnvfr";
			if (byte >= 0x07 && byte <= 0x0D) {
				out.push_back('\\');
				out.push_back(kNames[byte - 0x07]);
				return;
			}

			char octal[8];
			std::snprintf(octal, sizeof(octal), "\\%03o", static_cast<unsigned int>(byte));
			out += octal;
		}

		static std::string escapedPiece(unsigned char byte) {
			std::string piece;
			if (byte == '\\') piece = "\\\\";
			else if (byte < 0x20 || byte >= 0x7F) appendEscaped(piece, byte);
			else piece.push_back(static_cast<char>(byte));
			return piece;
		}

		// The `l` form: escapes for what does not print, a `$` at the end,
		// and long lines folded with a trailing backslash.
		static std::string listForm(const std::string& text) {
			std::string out;
			std::size_t column = 0;
			for (char c : text) {
				const std::string piece = escapedPiece(static_cast<unsigned char>(c));
				if (column + piece.size() > kListWidth - 1) {
					out += "\\\n";
					column = 0;
				}

				out += piece;
				column += piece.size();
			}

			return out + "$";
		}

		void printLine(const EdState& st, std::size_t line, int flags) {
			const std::string& text = st.lines[line - 1].text;
			if ((flags & kPrintNumber) != 0) std::printf("%zu\t", line);
			if ((flags & kPrintList) != 0) {
				std::printf("%s\n", listForm(text).c_str());
				return;
			}

			std::fwrite(text.data(), 1, text.size(), stdout);
			std::fputc('\n', stdout);
		}

	}  // namespace ed_detail

}  // namespace wbsh

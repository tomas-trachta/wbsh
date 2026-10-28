/**
 * @file vi_file.cpp
 * @brief vi's files: reading them (CRLF files are remembered as dos and
 *        written back the same way), writing ranges, and the file
 *        status messages.
 */

#include "vi_internal.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "coreutils_internal.h"

namespace wbsh {

	namespace vi_detail {

		namespace fs = std::filesystem;

		static const std::size_t kPercent = 100;

		static std::string quotedName(const std::string& name) {
			return "\"" + name + "\"";
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

		std::vector<std::string> splitLines(const std::string& text, bool& crlf) {
			crlf = everyNewlineHasReturn(text);
			std::vector<std::string> lines;
			std::size_t start = 0;
			while (start < text.size()) {
				std::size_t end = text.find('\n', start);
				if (end == std::string::npos) end = text.size();

				const std::size_t stop = crlf && end < text.size() ? end - 1 : end;
				lines.push_back(text.substr(start, stop - start));
				start = end + 1;
			}

			return lines;
		}

		bool readFileText(Editor& ed, const std::string& name, std::string& text) {
			std::error_code ec;
			if (fs::is_directory(toNative(*ed.exec, name), ec)) {
				setError(ed, quotedName(name) + " is a directory");
				return false;
			}

			std::FILE* file = fopenNative(*ed.exec, name, "rb");
			if (file == nullptr) {
				setError(ed, quotedName(name) + " " + std::strerror(errno));
				return false;
			}

			char chunk[65536];
			std::size_t got = 0;
			while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) text.append(chunk, got);
			std::fclose(file);
			return true;
		}

		static std::string countsText(std::size_t lines, std::size_t bytes) {
			return std::to_string(lines) + "L, " + std::to_string(bytes) + "C";
		}

		static std::string loadedMessage(const Editor& ed, const std::string& text,
		                                 std::size_t lines) {
			std::string message = quotedName(ed.filename);
			if (ed.options.readonly) message += " [readonly]";
			if (ed.crlf) message += " [dos]";
			if (!text.empty() && text.back() != '\n') message += " [noeol]";
			return message + " " + countsText(lines, text.size());
		}

		static void resetView(Editor& ed) {
			ed.cursor = Position();
			ed.top = 0;
			ed.top_skip = 0;
			ed.want_column = 0;
			ed.want_end = false;
		}

		bool loadFile(Editor& ed, const std::string& name) {
			ed.filename = name;
			ed.crlf = false;
			resetView(ed);

			std::error_code ec;
			if (!fs::exists(toNative(*ed.exec, name), ec)) {
				replaceBuffer(ed, {});
				setMessage(ed, quotedName(name) + " [New file]");
				return true;
			}

			std::string text;
			if (!readFileText(ed, name, text)) {
				replaceBuffer(ed, {});
				return false;
			}

			std::vector<std::string> lines = splitLines(text, ed.crlf);
			const std::size_t count = lines.size();
			replaceBuffer(ed, std::move(lines));
			setMessage(ed, loadedMessage(ed, text, count));
			return true;
		}

		static std::string bufferText(const Editor& ed, const ExRange& range) {
			if (ed.no_lines) return std::string();

			const char* const ending = ed.crlf ? "\r\n" : "\n";
			std::string text;
			for (std::size_t line = range.first; line <= range.last && line > 0; ++line) {
				text += ed.lines[line - 1];
				text += ending;
			}

			return text;
		}

		static bool refuseWrite(Editor& ed, const std::string& name, bool append, bool force) {
			if (name.empty()) {
				setError(ed, "No file name");
				return true;
			}

			if (force) return false;

			if (ed.options.readonly && name == ed.filename) {
				setError(ed, "'readonly' option is set (add ! to override)");
				return true;
			}

			std::error_code ec;
			const bool other_file = name != ed.filename && !append;
			if (other_file && fs::exists(toNative(*ed.exec, name), ec)) {
				setError(ed, "File exists (add ! to override)");
				return true;
			}

			return false;
		}

		static bool storeText(Editor& ed, const std::string& name, const std::string& text,
		                      bool append) {
			std::FILE* file = fopenNative(*ed.exec, name, append ? "ab" : "wb");
			if (file == nullptr) {
				setError(ed, quotedName(name) + " " + std::strerror(errno));
				return false;
			}

			const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
			const bool closed = std::fclose(file) == 0;
			if (written && closed) return true;

			setError(ed, quotedName(name) + " write error");
			return false;
		}

		bool writeFile(Editor& ed, const std::string& name, const ExRange& range, bool append,
		               bool force) {
			if (refuseWrite(ed, name, append, force)) return false;

			std::error_code ec;
			const bool is_new = !fs::exists(toNative(*ed.exec, name), ec);
			const std::string text = bufferText(ed, range);
			if (!storeText(ed, name, text, append)) return false;

			const bool whole = range.first <= 1 && range.last >= ed.lines.size();
			if (ed.filename.empty()) ed.filename = name;
			if (whole && name == ed.filename) ed.saved_state = ed.state;

			const std::size_t lines = ed.no_lines ? 0 : range.last - range.first + 1;
			std::string message = quotedName(name) + (is_new ? " [New]" : "")
				+ (ed.crlf ? " [dos]" : "");
			message += " " + countsText(lines, text.size()) + (append ? " appended" : " written");
			setMessage(ed, message);
			return true;
		}

		void showFileInfo(Editor& ed) {
			std::string message = quotedName(ed.filename.empty() ? "[No Name]" : ed.filename);
			if (isModified(ed)) message += " [Modified]";
			if (ed.options.readonly) message += " [readonly]";
			if (ed.no_lines) {
				setMessage(ed, message + " --No lines in buffer--");
				return;
			}

			const std::size_t total = ed.lines.size();
			const std::size_t line = ed.cursor.row + 1;
			message += " line " + std::to_string(line) + " of " + std::to_string(total);
			message += " --" + std::to_string(line * kPercent / total) + "%--";
			message += " col "
				+ std::to_string(displayColumn(ed, ed.cursor.row, ed.cursor.col) + 1);
			setMessage(ed, message);
		}

	}  // namespace vi_detail

}  // namespace wbsh

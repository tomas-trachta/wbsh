/**
 * @file file_links.cpp
 * @brief Finding a printed path under the pointer, with its line and column.
 */

#include "file_links.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace wbshterm {

	namespace links_detail {

		static const char* const kWordStops = " \t\"'`<>|*?";
		static const char* const kLeadingJunk = "([{<'\"";
		static const char* const kTrailingJunk = ".,;:!?)]}>'\"";

		static const char* const kRunnableExtensions[] = {
			".exe", ".com", ".bat", ".cmd", ".ps1", ".psm1", ".vbs", ".vbe", ".js", ".jse",
			".wsf", ".wsh", ".msi", ".msc", ".scr", ".lnk", ".url", ".cpl", ".hta", ".jar",
			".reg", ".sh", ".py", ".pyw", ".application", ".appref-ms", nullptr,
		};

		static bool isWordCell(const std::string& cell) {
			if (cell.empty()) return true;

			return cell.size() > 1 || std::strchr(kWordStops, cell[0]) == nullptr;
		}

		static bool isBlank(const std::string& cell) {
			return cell.empty() || cell == " ";
		}

		static std::string wordAround(const std::vector<std::string>& cells, int column) {
			int first = column;
			int last = column;
			while (first > 0 && isWordCell(cells[static_cast<std::size_t>(first - 1)])) --first;
			while (last + 1 < static_cast<int>(cells.size())
				&& isWordCell(cells[static_cast<std::size_t>(last + 1)])) {
				++last;
			}

			std::string word;
			for (int at = first; at <= last; ++at) word += cells[static_cast<std::size_t>(at)];
			return word;
		}

		static bool allDigits(const std::string& text) {
			if (text.empty()) return false;

			for (const char letter : text) {
				if (std::isdigit(static_cast<unsigned char>(letter)) == 0) return false;
			}

			return true;
		}

		static bool closesOwnParenthesis(const std::string& word) {
			return word.back() == ')' && word.find('(') != std::string::npos;
		}

		static bool endsInJunk(const std::string& word) {
			if (word.empty() || closesOwnParenthesis(word)) return false;

			return std::strchr(kTrailingJunk, word.back()) != nullptr;
		}

		static void trimJunk(std::string& word) {
			while (!word.empty() && std::strchr(kLeadingJunk, word.front()) != nullptr) {
				word.erase(0, 1);
			}

			while (endsInJunk(word)) {
				word.pop_back();
			}
		}

		// MSVC's file(42) and file(42,7).
		static bool takeParenthesised(std::string& word, FileLink& link) {
			if (word.empty() || word.back() != ')') return false;

			const std::size_t open = word.rfind('(');
			if (open == std::string::npos || open == 0) return false;

			const std::string inside = word.substr(open + 1, word.size() - open - 2);
			const std::size_t comma = inside.find(',');
			const std::string line = inside.substr(0, comma);
			const std::string column = comma == std::string::npos ? "" : inside.substr(comma + 1);
			if (!allDigits(line) || (comma != std::string::npos && !allDigits(column))) {
				return false;
			}

			link.line = std::atoi(line.c_str());
			link.column = column.empty() ? 0 : std::atoi(column.c_str());
			word.erase(open);
			return true;
		}

		static bool takeTrailingNumber(std::string& word, int& out_number) {
			const std::size_t colon = word.rfind(':');
			if (colon == std::string::npos || colon == 0) return false;

			const std::string digits = word.substr(colon + 1);
			if (!allDigits(digits)) return false;

			out_number = std::atoi(digits.c_str());
			word.erase(colon);
			return true;
		}

		// grep's file:42, a compiler's file:42:7.
		static void takeColonNumbers(std::string& word, FileLink& link) {
			int last = 0;
			if (!takeTrailingNumber(word, last)) return;

			int before = 0;
			if (takeTrailingNumber(word, before)) {
				link.line = before;
				link.column = last;
				return;
			}

			link.line = last;
		}

		static bool looksLikePath(const std::string& word) {
			if (word.empty() || word.find("://") != std::string::npos) return false;

			return word.find_first_of("/\\.") != std::string::npos;
		}

		static bool isDriveLetter(char letter) {
			return std::isalpha(static_cast<unsigned char>(letter)) != 0;
		}

		static bool isAbsolute(const std::string& path) {
			const bool drive = path.size() >= 2 && isDriveLetter(path[0]) && path[1] == ':';
			return drive || path.compare(0, 2, "//") == 0 || path.compare(0, 2, "\\\\") == 0;
		}

		// /c/Users as an MSYS tool prints it is C:/Users.
		static bool msysDrivePath(const std::string& path) {
			return path.size() >= 3 && path[0] == '/' && isDriveLetter(path[1]) && path[2] == '/';
		}

		static void replaceAll(std::string& text, const std::string& from, const std::string& to) {
			std::size_t at = text.find(from);
			while (at != std::string::npos) {
				text.replace(at, from.size(), to);
				at = text.find(from, at + to.size());
			}
		}

		static std::string lowered(std::string text) {
			for (char& letter : text) {
				letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
			}

			return text;
		}

	}  // namespace links_detail

	bool findFileLink(const std::vector<std::string>& cells, int column, FileLink& out_link) {
		if (column < 0 || column >= static_cast<int>(cells.size())) return false;
		if (links_detail::isBlank(cells[static_cast<std::size_t>(column)])) return false;

		std::string word = links_detail::wordAround(cells, column);
		links_detail::trimJunk(word);

		FileLink link;
		if (!links_detail::takeParenthesised(word, link)) {
			links_detail::takeColonNumbers(word, link);
			links_detail::trimJunk(word);
		}

		if (!links_detail::looksLikePath(word)) return false;

		link.path = word;
		out_link = link;
		return true;
	}

	std::string resolveLinkPath(const std::string& path, const std::string& directory,
			const std::string& home) {
		std::string resolved = path;
		for (char& letter : resolved) {
			if (letter == '\\') letter = '/';
		}

		if (links_detail::msysDrivePath(resolved)) {
			const auto letter = static_cast<unsigned char>(resolved[1]);
			const char drive = static_cast<char>(std::toupper(letter));
			return std::string(1, drive) + ":" + resolved.substr(2);
		}

		if (resolved.compare(0, 2, "~/") == 0 && !home.empty()) return home + resolved.substr(1);
		if (links_detail::isAbsolute(resolved) || directory.empty()) return resolved;

		return directory + (directory.back() == '/' ? "" : "/") + resolved;
	}

	std::string editorCommandFor(const std::string& editor, const std::string& file,
			const FileLink& link) {
		std::string command = editor;
		links_detail::replaceAll(command, "{file}", file);
		links_detail::replaceAll(command, "{line}", std::to_string(link.line > 0 ? link.line : 1));
		links_detail::replaceAll(command, "{column}",
			std::to_string(link.column > 0 ? link.column : 1));
		return command;
	}

	bool linkRunsSomething(const std::string& path) {
		const std::string name = links_detail::lowered(path);
		for (int i = 0; links_detail::kRunnableExtensions[i] != nullptr; ++i) {
			const std::string extension = links_detail::kRunnableExtensions[i];
			if (name.size() > extension.size()
				&& name.compare(name.size() - extension.size(), extension.size(), extension) == 0) {
				return true;
			}
		}

		return false;
	}

} /* namespace wbshterm */

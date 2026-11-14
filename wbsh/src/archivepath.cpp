/**
 * @file archivepath.cpp
 * @brief Archive member names and link targets checked against leaving the
 *        extraction directory.
 */

#include "archivepath.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace wbsh {

	static bool isSeparator(char c) {
		return c == '/' || c == '\\';
	}

	static bool hasDrivePrefix(const std::string& name) {
		return name.size() >= 2 && name[1] == ':'
			&& std::isalpha(static_cast<unsigned char>(name[0])) != 0;
	}

	static bool isRooted(const std::string& name) {
		return !name.empty() && isSeparator(name[0]);
	}

	static bool stripDrive(std::string& name) {
		if (!hasDrivePrefix(name)) return false;

		name.erase(0, 2);
		return true;
	}

	static bool stripLeadingSlashes(std::string& name) {
		const std::size_t first = name.find_first_not_of('/');
		const std::size_t count = first == std::string::npos ? name.size() : first;
		name.erase(0, count);
		return count > 0;
	}

	bool stripMemberRoot(std::string& name) {
		std::replace(name.begin(), name.end(), '\\', '/');

		const bool drive = stripDrive(name);
		const bool slashes = stripLeadingSlashes(name);
		return drive || slashes;
	}

	// Win32 path parsing trims trailing dots and spaces from a component,
	// so `.. ` or `...` must not slip past a plain `..` comparison.
	static bool isParentLike(const std::string& component) {
		const bool only_dots_and_spaces = component.find_first_not_of(". ") == std::string::npos;
		return only_dots_and_spaces && std::count(component.begin(), component.end(), '.') >= 2;
	}

	static std::size_t componentEnd(const std::string& name, std::size_t start) {
		const auto separator = std::find_if(name.begin() + start, name.end(), isSeparator);
		return static_cast<std::size_t>(separator - name.begin());
	}

	bool memberEscapes(const std::string& name) {
		std::size_t start = 0;
		while (start <= name.size()) {
			const std::size_t end = componentEnd(name, start);
			if (isParentLike(name.substr(start, end - start))) return true;
			start = end + 1;
		}

		return false;
	}

	bool isEscapingLinkTarget(const std::string& target) {
		return isRooted(target) || hasDrivePrefix(target) || memberEscapes(target);
	}

}  // namespace wbsh

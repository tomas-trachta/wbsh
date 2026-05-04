#pragma once

#include <cstddef>
#include <string>

namespace wbsh {

	struct SourceLoc {
		std::size_t line = 1;
		std::size_t column = 1;
		std::size_t offset = 0;
	};

	inline std::string toString(const SourceLoc& l) {
		return std::to_string(l.line) + ":" + std::to_string(l.column);
	}

}  // namespace wbsh

/**
 * @file status.cpp
 * @brief Putting the bar's segments into one line.
 */

#include "status.h"

namespace wbshterm {

	bool operator==(const StatusSegment& left, const StatusSegment& right) {
		return left.label == right.label && left.value == right.value && left.tint == right.tint;
	}

	bool operator!=(const StatusSegment& left, const StatusSegment& right) {
		return !(left == right);
	}

	std::string segmentText(const StatusSegment& segment) {
		if (segment.label.empty()) return segment.value;

		return segment.label + " " + segment.value;
	}

	std::size_t segmentCells(const StatusSegment& segment) {
		return segmentText(segment).size();
	}

	std::string statusLine(const std::vector<StatusSegment>& segments) {
		std::string line;
		for (const StatusSegment& segment : segments) {
			if (!line.empty()) line += "  ";

			line += segmentText(segment);
		}

		return line;
	}

} /* namespace wbshterm */

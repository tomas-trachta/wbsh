#pragma once

/**
 * @file status.h
 * @brief One part of the bar along the bottom, and what the parts add up to.
 */

#include <cstddef>
#include <string>
#include <vector>

namespace wbshterm {

	/** Slots in the theme's ANSI palette a value may be tinted with. */
	static const int kTintPlain  = -1;
	static const int kTintRed    = 1;
	static const int kTintGreen  = 2;
	static const int kTintYellow = 3;

	/**
	 * @brief A reading as the bar shows it: a quiet label, a bright value.
	 *
	 * The tint colours the value alone, so a load that is high or a battery
	 * that is low says so before the number is read; kTintPlain is the
	 * bar's ordinary ink. A label may be empty, for a clock or a host.
	 */
	struct StatusSegment {
		std::string label;
		std::string value;
		int         tint = kTintPlain;
	};

	bool operator==(const StatusSegment& left, const StatusSegment& right);
	bool operator!=(const StatusSegment& left, const StatusSegment& right);

	/** The segment as one string, label first, for a check or a log. */
	std::string segmentText(const StatusSegment& segment);

	/** How many grid columns the segment takes when drawn. */
	std::size_t segmentCells(const StatusSegment& segment);

	/** Every segment's text, two spaces apart, the way tmux joins status-right. */
	std::string statusLine(const std::vector<StatusSegment>& segments);

} /* namespace wbshterm */

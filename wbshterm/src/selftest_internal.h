#pragma once

/**
 * @file selftest_internal.h
 * @brief What the self-test's translation units share: the report and grid helpers.
 */

#include "screen.h"

#include <string>

namespace wbshterm {

	namespace test {

		class Report {
		public:
			void check(const std::string& name, bool passed, const std::string& detail);
			bool passed() const { return failures_ == 0; }
			const std::string& text() const { return text_; }

		private:
			std::string text_;
			int         failures_ = 0;
		};

		/** Parses @p bytes into @p screen and returns the grid as text. */
		std::string feedToScreen(Screen& screen, const std::string& bytes);

		std::string firstLine(const std::string& text);

	} /* namespace test */

	/** The escape-sequence checks: modes, resets, scrolling and SGR. */
	void runVtChecks(test::Report& report);

	/** Text in and out of the window: UTF-8, copying the selection, the picker. */
	void runInputChecks(test::Report& report);

} /* namespace wbshterm */

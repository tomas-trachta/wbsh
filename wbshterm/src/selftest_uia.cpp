/**
 * @file selftest_uia.cpp
 * @brief Checks for the text screen readers are handed: units, boundaries and text.
 */

#include "selftest_internal.h"

#include "uia_text.h"

#include <string>
#include <vector>

namespace wbshterm {

	namespace uia_test {

		using test::Report;
		using test::feedToScreen;

		static std::string narrow(const std::wstring& text) {
			std::string out;
			for (const wchar_t unit : text) {
				out.push_back(unit < 0x80 ? static_cast<char>(unit) : '?');
			}

			return out;
		}

		static void checkLinesEndInCrLf(Report& report) {
			Screen screen(10, 3);
			feedToScreen(screen, "one two\r\nthree");

			const TextModel text(screen, screen.rows());
			const std::string all = narrow(text.text(0, text.end(), -1, nullptr));
			report.check("rows end in CR LF with their trailing blanks dropped",
				all == "one two\r\nthree\r\n", all);
			report.check("the document is every cell of every row",
				text.end() == 30, std::to_string(text.end()));

			const std::string limited = narrow(text.text(0, text.end(), 5, nullptr));
			report.check("a length limit cuts the text", limited == "one t", limited);
		}

		static void checkSoftWrapsJoinParagraphs(Report& report) {
			Screen screen(5, 4);
			feedToScreen(screen, "abcdefgh\r\nxy");

			const TextModel text(screen, screen.rows());
			const std::string wrapped = narrow(text.text(0, 10, -1, nullptr));
			report.check("a soft-wrapped row runs on without a line break",
				wrapped == "abcdefgh\r\n", wrapped);
			report.check("a line is a row, a paragraph the rows a wrap joins",
				text.nextBoundary(0, TextUnit::kLine) == 5
					&& text.nextBoundary(0, TextUnit::kParagraph) == 10
					&& text.unitStart(7, TextUnit::kParagraph) == 0,
				std::to_string(text.nextBoundary(0, TextUnit::kParagraph)));
		}

		static void checkWordsTakeTheirBlanks(Report& report) {
			Screen screen(20, 2);
			feedToScreen(screen, "ls  -la /tmp");

			const TextModel text(screen, screen.rows());
			report.check("a word runs to the start of the next one",
				text.nextBoundary(0, TextUnit::kWord) == 4
					&& text.nextBoundary(4, TextUnit::kWord) == 8
					&& text.unitStart(10, TextUnit::kWord) == 8,
				std::to_string(text.nextBoundary(0, TextUnit::kWord)));
			report.check("format falls back to word, as UI Automation asks",
				text.nextBoundary(0, TextUnit::kFormat) == 4, "");

			long long at = 0;
			const int moved = text.move(at, TextUnit::kWord, 2);
			report.check("moving by words crosses their starts",
				moved == 2 && at == 8, std::to_string(at));
		}

		static void checkWideCharactersAreOneStop(Report& report) {
			Screen screen(10, 2);
			feedToScreen(screen, "a\xE4\xB8\xAD" "b");

			const TextModel text(screen, screen.rows());
			report.check("the tail of a wide character is no stop",
				text.nextBoundary(1, TextUnit::kCharacter) == 3
					&& text.previousBoundary(3, TextUnit::kCharacter) == 1,
				std::to_string(text.nextBoundary(1, TextUnit::kCharacter)));

			const std::wstring row = text.text(0, 10, -1, nullptr);
			report.check("a wide character reads once",
				row.size() >= 3 && row[1] == static_cast<wchar_t>(0x4E2D) && row[2] == L'b',
				narrow(row));
		}

		static void checkMovesStopAtTheEnds(Report& report) {
			Screen screen(4, 3);
			feedToScreen(screen, "ab\r\ncd");

			const TextModel text(screen, screen.rows());
			long long at = 1;
			const int back = text.move(at, TextUnit::kLine, -5);
			report.check("moving back stops at the start and says how far it went",
				back == -1 && at == 0, std::to_string(back));

			const int forward = text.move(at, TextUnit::kDocument, 3);
			report.check("moving by the document goes to its end at once",
				forward == 1 && at == text.end(), std::to_string(forward));
			report.check("the last unit is found from the very end",
				text.unitStart(text.end(), TextUnit::kLine) == 8, "");
		}

		static void checkPositionsFollowText(Report& report) {
			Screen screen(6, 2);
			feedToScreen(screen, "ab\r\ncd");

			const TextModel text(screen, screen.rows());
			std::vector<long long> positions;
			const std::wstring all = text.text(0, text.end(), -1, &positions);
			const bool aligned = positions.size() == all.size() && positions.size() >= 5
				&& positions[0] == 0 && positions[1] == 1 && positions[4] == 6;
			report.check("every unit of text knows the cell it came from", aligned,
				std::to_string(positions.size()));
		}

	} /* namespace uia_test */

	void runAccessibilityChecks(test::Report& report) {
		uia_test::checkLinesEndInCrLf(report);
		uia_test::checkSoftWrapsJoinParagraphs(report);
		uia_test::checkWordsTakeTheirBlanks(report);
		uia_test::checkWideCharactersAreOneStop(report);
		uia_test::checkMovesStopAtTheEnds(report);
		uia_test::checkPositionsFollowText(report);
	}

} /* namespace wbshterm */

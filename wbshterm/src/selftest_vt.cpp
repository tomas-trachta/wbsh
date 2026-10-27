/**
 * @file selftest_vt.cpp
 * @brief Grid checks for escape sequences: what each one leaves behind.
 */

#include "selftest_internal.h"

#include "vtparse.h"

#include <string>

namespace wbshterm {

	namespace vt_test {

		using test::Report;
		using test::feedToScreen;
		using test::firstLine;

		class CollectingResponder : public VtResponder {
		public:
			void vtRespond(const std::string& bytes) override { replies += bytes; }

			std::string replies;
		};

		static const std::uint32_t kRed = kPaletteColor | 1u;

		static void checkSaveRestoreKeepsThePen(Report& report) {
			Screen screen(10, 2);
			feedToScreen(screen, "\x1b[31m\x1b" "7\x1b[0m\x1b" "8X");
			report.check("DECRC brings back the pen DECSC saved",
				screen.cell(0, 0).foreground == kRed, "");

			Screen csi(10, 2);
			feedToScreen(csi, "\x1b[1;31m\x1b[s\x1b[0m\x1b[uX");
			report.check("CSI u brings back the pen CSI s saved",
				csi.cell(0, 0).foreground == kRed
					&& (csi.cell(0, 0).attributes & kAttrBold) != 0, "");
		}

		static void checkSaveRestoreKeepsThePendingWrap(Report& report) {
			Screen screen(5, 3);
			const std::string grid = feedToScreen(screen, "abcde\x1b" "7\x1b[3;1H\x1b" "8X");
			report.check("DECRC brings back a pending wrap",
				grid == "abcde\nX\n\n", grid);
		}

		static void checkSaveRestoreKeepsOriginMode(Report& report) {
			Screen screen(10, 6);
			feedToScreen(screen, "\x1b[3;5r\x1b[?6h\x1b" "7\x1b[?6l\x1b" "8\x1b[1;1HX");
			report.check("DECRC brings back origin mode", screen.cell(2, 0).code == U'X',
				screen.toText());
		}

		static void checkOriginMode(Report& report) {
			Screen screen(10, 8);
			CollectingResponder responder;
			screen.setResponder(&responder);

			feedToScreen(screen, "\x1b[3;5r\x1b[?6hA\x1b[9;2HB\x1b[6n");
			report.check("origin mode counts rows from the top margin",
				screen.cell(2, 0).code == U'A', screen.toText());
			report.check("origin mode keeps the cursor inside the region",
				screen.cell(4, 1).code == U'B', screen.toText());
			report.check("CPR reports a row relative to the margin in origin mode",
				responder.replies == "\x1b[3;3R", responder.replies);
		}

		static void checkHardResetClearsModes(Report& report) {
			Screen screen(10, 10);
			feedToScreen(screen, "\x1b]0;stuck\x07\x1b[?2004h\x1b[?1h\x1b[?2026h\x1b[?25l"
				"\x1b[?1049h\x1b[2;5r\x1b[?6h\x1b[31mjunk\x1b" "c");

			report.check("RIS turns bracketed paste off", !screen.bracketedPaste(), "");
			report.check("RIS turns application cursor keys off",
				!screen.applicationCursorKeys(), "");
			report.check("RIS ends synchronized output", !screen.synchronizedOutput(), "");
			report.check("RIS leaves the alt screen", !screen.onAltScreen(), "");
			report.check("RIS forgets the title", screen.title().empty(), screen.title());
			report.check("RIS shows the cursor at home", screen.cursor().visible
				&& screen.cursor().row == 0 && screen.cursor().column == 0, "");

			const std::string grid = feedToScreen(screen, "\x1b[10;1HZ\n");
			report.check("RIS clears the scroll region and origin mode",
				screen.cell(8, 0).code == U'Z', grid);
			report.check("RIS resets the pen", screen.cell(8, 0).foreground == kDefaultColor, "");
		}

		static void checkSoftReset(Report& report) {
			Screen screen(10, 6);
			const std::string grid = feedToScreen(screen,
				"kept\x1b[31m\x1b[?25l\x1b[?1h\x1b[2;4r\x1b[?6h\x1b[!p\x1b[1;1HX");

			report.check("DECSTR keeps the text", firstLine(grid) == "Xept", grid);
			report.check("DECSTR resets the pen, modes and margins",
				screen.cell(0, 0).foreground == kDefaultColor && screen.cursor().visible
					&& !screen.applicationCursorKeys(), "");
		}

		static void checkUnknownIntermediatesAreIgnored(Report& report) {
			Screen screen(10, 10);
			feedToScreen(screen, "\x1b[5;5H\x1b[1;5;3;10;1$rX");
			report.check("DECCARA is not taken for DECSTBM", screen.cell(4, 4).code == U'X',
				screen.toText());

			Screen esc(10, 3);
			feedToScreen(esc, "ab\x1b" "7\x1b[3;3H\x1b#8X");
			report.check("ESC # 8 is not taken for DECRC", esc.cell(2, 2).code == U'X',
				esc.toText());
		}

		static void runSaveRestoreAndResetChecks(Report& report) {
			checkSaveRestoreKeepsThePen(report);
			checkSaveRestoreKeepsThePendingWrap(report);
			checkSaveRestoreKeepsOriginMode(report);
			checkOriginMode(report);
			checkHardResetClearsModes(report);
			checkSoftReset(report);
			checkUnknownIntermediatesAreIgnored(report);
		}

		static const char* const kFiveRows = "1\r\n2\r\n3\r\n4\r\n5";

		static void checkLineEditsOutsideTheMarginsDoNothing(Report& report) {
			Screen screen(4, 6);
			feedToScreen(screen, kFiveRows);
			const std::string before = screen.toText();

			const std::string inserted = feedToScreen(screen, "\x1b[1;3r\x1b[5H\x1b[L");
			report.check("IL below the scroll region does nothing", inserted == before, inserted);

			const std::string deleted = feedToScreen(screen, "\x1b[5H\x1b[2M");
			report.check("DL below the scroll region does nothing", deleted == before, deleted);
		}

		static void checkLineEditsStayInsideTheMargins(Report& report) {
			Screen screen(4, 6);
			const std::string inserted = feedToScreen(screen,
				std::string(kFiveRows) + "\x1b[2;4r\x1b[3;3H\x1b[LX");
			report.check("IL pushes rows down only to the bottom margin and homes the column",
				inserted == "1\n2\nX\n3\n5\n\n", inserted);

			Screen deleting(4, 6);
			const std::string deleted = feedToScreen(deleting,
				std::string(kFiveRows) + "\x1b[2;4r\x1b[2;3H\x1b[MX");
			report.check("DL pulls rows up only from the bottom margin and homes the column",
				deleted == "1\nX\n4\n\n5\n\n", deleted);
		}

		static void checkDeleteLineKeepsHistoryAlone(Report& report) {
			Screen screen(4, 3);
			feedToScreen(screen, "top\r\nmid\x1b[H\x1b[M");
			report.check("DL on the top row does not push it into scrollback",
				screen.scrollbackRows() == 0 && firstLine(screen.toText()) == "mid",
				screen.toText());

			Screen scrolling(4, 3);
			feedToScreen(scrolling, "top\r\nmid\x1b[S");
			report.check("SU still sends the top row to scrollback",
				scrolling.scrollbackRows() == 1 && scrolling.cellAt(0, 0).code == U't', "");
		}

		static void checkHugeScrollCountsAreClamped(Report& report) {
			Screen screen(4, 3);
			feedToScreen(screen, "a\r\nb\r\nc\x1b[999999S");
			report.check("a huge SU scrolls one screenful at most",
				screen.scrollbackRows() == 3 && screen.toText() == "\n\n\n", screen.toText());

			Screen down(4, 3);
			const std::string grid = feedToScreen(down, "a\r\nb\x1b[999999T\x1b[999999L");
			report.check("huge SD and IL counts clear the region and nothing more",
				grid == "\n\n\n" && down.scrollbackRows() == 0, grid);
		}

		static void runLineScrollingChecks(Report& report) {
			checkLineEditsOutsideTheMarginsDoNothing(report);
			checkLineEditsStayInsideTheMargins(report);
			checkDeleteLineKeepsHistoryAlone(report);
			checkHugeScrollCountsAreClamped(report);
		}

	} /* namespace vt_test */

	void runVtChecks(test::Report& report) {
		vt_test::runSaveRestoreAndResetChecks(report);
		vt_test::runLineScrollingChecks(report);
	}

} /* namespace wbshterm */

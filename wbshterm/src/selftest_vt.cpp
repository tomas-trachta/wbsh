/**
 * @file selftest_vt.cpp
 * @brief Grid checks for escape sequences: what each one leaves behind.
 */

#include "selftest_internal.h"

#include "vtparse.h"

#include <string>
#include <vector>

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

		static Cell penAfter(const std::string& sgr) {
			Screen screen(4, 1);
			feedToScreen(screen, sgr + "X");
			return screen.cell(0, 0);
		}

		static bool hasAttributes(const Cell& cell, std::uint16_t attributes) {
			return (cell.attributes & attributes) == attributes;
		}

		static void checkColonUnderlineStyles(Report& report) {
			const Cell curly = penAfter("\x1b[1;4:3m");
			report.check("SGR 4:3 is a curly underline, not italic",
				hasAttributes(curly, kAttrBold | kAttrUnderline | kAttrUnderlineCurly)
					&& (curly.attributes & kAttrItalic) == 0, "");

			report.check("SGR 4:2, 4:4 and 4:5 pick double, dotted and dashed",
				hasAttributes(penAfter("\x1b[4:2m"), kAttrUnderlineDouble)
					&& hasAttributes(penAfter("\x1b[4:4m"), kAttrUnderlineDotted)
					&& hasAttributes(penAfter("\x1b[4:5m"), kAttrUnderlineDashed), "");

			const Cell none = penAfter("\x1b[4:3m\x1b[4:0m");
			const Cell reset = penAfter("\x1b[21m\x1b[24m");
			report.check("SGR 4:0 and SGR 24 take every underline shape off",
				(none.attributes & (kAttrUnderline | kAttrUnderlineShapes)) == 0
					&& (reset.attributes & (kAttrUnderline | kAttrUnderlineShapes)) == 0, "");
			report.check("SGR 21 is a double underline",
				hasAttributes(penAfter("\x1b[21m"), kAttrUnderline | kAttrUnderlineDouble), "");
		}

		static void checkUnderlineColour(Report& report) {
			const Cell semicolons = penAfter("\x1b[1;31;58;2;255;0;0m");
			report.check("SGR 58;2 takes its own arguments, not the pen's",
				hasAttributes(semicolons, kAttrBold) && semicolons.foreground == kRed
					&& semicolons.underline == 0xFF0000u, "");

			report.check("SGR 58:2::r:g:b and 58:2:r:g:b both set the underline colour",
				penAfter("\x1b[58:2::10:20:30m").underline == 0x0A141Eu
					&& penAfter("\x1b[58:2:10:20:30m").underline == 0x0A141Eu, "");
			report.check("SGR 58;5 indexes the palette",
				penAfter("\x1b[58;5;1m").underline == kRed, "");
			report.check("SGR 59 gives the underline the text's colour again",
				penAfter("\x1b[58;5;1;59m").underline == kDefaultColor, "");
		}

		static void checkColonColours(Report& report) {
			const Cell truecolor = penAfter("\x1b[38:2::1:2:3;1m");
			report.check("SGR 38:2::r:g:b keeps its group and the codes after it",
				truecolor.foreground == 0x010203u && hasAttributes(truecolor, kAttrBold), "");

			const Cell indexed = penAfter("\x1b[48:5:1;3m");
			report.check("SGR 48:5:n is an indexed background",
				indexed.background == kRed && hasAttributes(indexed, kAttrItalic), "");

			const Cell skipped = penAfter("\x1b[1;38:9:9:9m");
			report.check("an unknown colour group is skipped whole",
				hasAttributes(skipped, kAttrBold) && skipped.foreground == kDefaultColor, "");
		}

		static void checkParameterCap(Report& report) {
			std::string sgr = "\x1b[";
			for (std::size_t index = 0; index < kMaxParams; ++index) sgr += "0;";
			const Cell cell = penAfter(sgr + "31m");
			report.check("parameters past the cap are dropped, not merged",
				cell.foreground == kDefaultColor, "");
		}

		static void checkUnderlineSurvivesDiskHistory(Report& report) {
			Cell styled = penAfter("\x1b[4:3;58;2;1;2;3m");
			styled.code = U'u';

			HistoryStore store;
			store.setColumns(4);
			const bool stored = store.append({ std::vector<Cell>(1, styled) });
			const Cell back = store.cellAt(0, 0);

			report.check("history on disk keeps the underline shape and colour",
				stored && back.underline == 0x010203u
					&& hasAttributes(back, kAttrUnderlineCurly), "");
		}

		static void runSgrChecks(Report& report) {
			checkColonUnderlineStyles(report);
			checkUnderlineColour(report);
			checkColonColours(report);
			checkParameterCap(report);
			checkUnderlineSurvivesDiskHistory(report);
		}

		static void checkCursorShapes(Report& report) {
			Screen screen(10, 2);
			feedToScreen(screen, "\x1b[5 q");
			const bool bar = screen.cursorShape() == CursorShape::kBlinkingBar;
			feedToScreen(screen, "\x1b[2 q");
			const bool block = screen.cursorShape() == CursorShape::kSteadyBlock;
			feedToScreen(screen, "\x1b[0 q");
			const bool reset = screen.cursorShape() == CursorShape::kDefault;
			feedToScreen(screen, "\x1b[4 q\x1b" "c");
			const bool cleared = screen.cursorShape() == CursorShape::kDefault;

			report.check("DECSCUSR picks the cursor shape", bar && block, "");
			report.check("DECSCUSR 0 and RIS hand the shape back to the config",
				reset && cleared, "");
		}

		static int cursorColumnAfter(Screen& screen, const std::string& bytes) {
			feedToScreen(screen, bytes);
			return screen.cursor().column;
		}

		static void checkTabStops(Report& report) {
			Screen screen(30, 2);
			report.check("tab stops start every eight columns",
				cursorColumnAfter(screen, "\t") == 8, "");
			report.check("HTS sets a stop where the cursor is",
				cursorColumnAfter(screen, "\r\x1b[4G\x1bH\r\t") == 3, "");
			report.check("TBC 0 clears the stop under the cursor",
				cursorColumnAfter(screen, "\x1b[g\r\t") == 8, "");
			report.check("CHT moves over several stops",
				cursorColumnAfter(screen, "\r\x1b[2I") == 16, "");
			report.check("CBT moves back to earlier stops",
				cursorColumnAfter(screen, "\x1b[20G\x1b[Z") == 16
					&& cursorColumnAfter(screen, "\x1b[2Z") == 0, "");
			report.check("TBC 3 clears every stop",
				cursorColumnAfter(screen, "\x1b[3g\r\t") == 29, "");

			Screen resized(10, 2);
			feedToScreen(resized, "\x1b[3g\x1b[3G\x1bH");
			resized.resize(30, 2);
			report.check("a resize keeps set stops and gives new columns the default",
				cursorColumnAfter(resized, "\r\t") == 2
					&& cursorColumnAfter(resized, "\t") == 16, "");
		}

		static void checkAutowrapMode(Report& report) {
			Screen screen(5, 2);
			CollectingResponder responder;
			screen.setResponder(&responder);

			const std::string grid = feedToScreen(screen, "\x1b[?7labcdefg\x1b[?7$p");
			report.check("without DECAWM the last column is overwritten",
				grid == "abcdg\n\n", grid);
			report.check("DECRQM reports DECAWM as reset", responder.replies == "\x1b[?7;2$y",
				responder.replies);

			const std::string wrapped = feedToScreen(screen, "\x1b[?7h\x1b[2;1Hxyzuvw");
			report.check("DECAWM on wraps again", wrapped == "xyzuv\nw\n", wrapped);
		}

		static void checkRepeatAndMotion(Report& report) {
			Screen screen(10, 4);
			report.check("REP repeats the last printed character",
				firstLine(feedToScreen(screen, "ab\x1b[3b")) == "abbbb", "");

			Screen flood(4, 2);
			const std::string flooded = feedToScreen(flood, "x\x1b[999999b");
			report.check("a huge REP count writes one screenful at most",
				flooded == "xxxx\nx\n" && flood.scrollbackRows() == 1, flooded);

			Screen motion(10, 4);
			feedToScreen(motion, "\x1b[5`A\r\x1b[2aB\x1b[2eC");
			report.check("HPA, HPR and VPR move the cursor",
				motion.cell(0, 4).code == U'A' && motion.cell(0, 2).code == U'B'
					&& motion.cell(2, 3).code == U'C', motion.toText());
		}

		static void runModeChecks(Report& report) {
			checkCursorShapes(report);
			checkTabStops(report);
			checkAutowrapMode(report);
			checkRepeatAndMotion(report);
		}

		static void checkHyperlinks(Report& report) {
			Screen screen(30, 2);
			feedToScreen(screen, "\x1b]8;id=1;https://example.test/a\x1b\\link\x1b]8;;\x1b\\ "
				"\x1b]8;;https://example.test/a\x07" "again");

			const std::string* first = screen.linkAt(0, 0);
			const std::string* plain = screen.linkAt(0, 4);
			const std::string* again = screen.linkAt(0, 5);
			report.check("OSC 8 marks the cells it covers with its target",
				first != nullptr && *first == "https://example.test/a", "");
			report.check("an empty OSC 8 ends the link", plain == nullptr, "");
			report.check("the same target is one link", again == first, "");

			report.check("only web and mail links may be opened",
				linkIsSafeToOpen("HTTPS://example.test") && linkIsSafeToOpen("mailto:a@b.c")
					&& !linkIsSafeToOpen("file:///C:/Windows/notepad.exe")
					&& !linkIsSafeToOpen("C:\\Windows\\notepad.exe"), "");
		}

		class CollectingClipboard : public ClipboardHandler {
		public:
			void clipboardWrite(const std::string& utf8) override {
				writes.push_back(utf8);
			}

			std::vector<std::string> writes;
		};

		static void checkClipboardWrites(Report& report) {
			Screen screen(10, 2);
			CollectingClipboard clipboard;
			screen.setClipboardHandler(&clipboard);

			feedToScreen(screen, "\x1b]52;c;aGVsbG8gd29ybGQ=\x07\x1b]52;c;?\x07");
			report.check("OSC 52 puts decoded text on the clipboard",
				clipboard.writes.size() == 1 && clipboard.writes[0] == "hello world", "");

			const std::string payload(8000, 'Q');
			feedToScreen(screen, "\x1b]52;c;" + payload + "\x1b\\");
			report.check("an OSC 52 payload past 4 KB arrives whole",
				clipboard.writes.size() == 2 && clipboard.writes[1].size() == 6000,
				std::to_string(clipboard.writes.size()));
		}

		static void checkFinalTermMarks(Report& report) {
			Screen screen(10, 4);
			feedToScreen(screen, "\x1b]133;A\x07\x1b]633;A\x07$ ls\r\n\x1b]133;C\x07out\r\n"
				"\x1b]133;D;3\x07");

			const std::vector<CommandBlock>& blocks = screen.commandBlocks();
			report.check("OSC 133 marks commands like OSC 633, once per prompt",
				blocks.size() == 1 && blocks[0].output_row == 1 && blocks[0].finished
					&& blocks[0].exit_status == 3, std::to_string(blocks.size()));
		}

		static void checkColorQueries(Report& report) {
			Palette palette;
			palette.foreground = 0x112233;
			palette.background = 0x445566;
			palette.ansi[1] = 0xAB0000;

			Screen screen(10, 2);
			CollectingResponder responder;
			screen.setResponder(&responder);
			screen.setPalette(&palette);

			feedToScreen(screen, "\x1b]10;?\x07");
			report.check("OSC 10 reports the text colour",
				responder.replies == "\x1b]10;rgb:1111/2222/3333\x1b\\", responder.replies);

			responder.replies.clear();
			feedToScreen(screen, "\x1b]10;?;?\x1b\\");
			report.check("OSC 10 ; ? ; ? reports text and background",
				responder.replies == "\x1b]10;rgb:1111/2222/3333\x1b\\"
					"\x1b]11;rgb:4444/5555/6666\x1b\\", responder.replies);

			responder.replies.clear();
			feedToScreen(screen, "\x1b]4;1;?;200;?\x07");
			report.check("OSC 4 reports palette and cube entries",
				responder.replies == "\x1b]4;1;rgb:abab/0000/0000\x1b\\"
					"\x1b]4;200;rgb:ffff/0000/d7d7\x1b\\", responder.replies);
		}

		static void runOscChecks(Report& report) {
			checkHyperlinks(report);
			checkClipboardWrites(report);
			checkFinalTermMarks(report);
			checkColorQueries(report);
		}

	} /* namespace vt_test */

	void runVtChecks(test::Report& report) {
		vt_test::runSaveRestoreAndResetChecks(report);
		vt_test::runLineScrollingChecks(report);
		vt_test::runSgrChecks(report);
		vt_test::runModeChecks(report);
		vt_test::runOscChecks(report);
	}

} /* namespace wbshterm */

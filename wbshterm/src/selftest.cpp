/**
 * @file selftest.cpp
 * @brief Golden grid checks plus a live round trip through the pty.
 */

#include "selftest.h"

#include "keymap.h"
#include "view.h"
#include "view.h"
#include "session.h"

#include <chrono>
#include <cstdio>
#include <thread>

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

		void Report::check(const std::string& name, bool passed, const std::string& detail) {
			text_ += passed ? "ok   " : "FAIL ";
			text_ += name;
			text_ += "\n";

			if (passed) return;

			++failures_;
			if (detail.empty()) return;

			text_ += "     ";
			text_ += detail;
			text_ += "\n";
		}

		static std::string feedToScreen(Screen& screen, const std::string& bytes) {
			VtParser parser(screen);
			parser.consume(bytes.data(), bytes.size());
			return screen.toText();
		}

		static std::string firstLine(const std::string& text) {
			const std::size_t end = text.find('\n');
			return end == std::string::npos ? text : text.substr(0, end);
		}

		static void checkPlainText(Report& report) {
			Screen screen(20, 3);
			const std::string grid = feedToScreen(screen, "hello");
			report.check("plain text lands in row 0", firstLine(grid) == "hello", firstLine(grid));
		}

		static void checkCursorMotion(Report& report) {
			Screen screen(20, 3);
			const std::string grid = feedToScreen(screen, "abc\x1b[2;5Hxy");
			report.check("CUP moves the cursor", firstLine(grid) == "abc"
				&& grid.find("\n    xy\n") != std::string::npos, grid);
		}

		static void checkEraseInLine(Report& report) {
			Screen screen(10, 2);
			const std::string grid = feedToScreen(screen, "abcdef\x1b[4G\x1b[K");
			report.check("EL clears from the cursor", firstLine(grid) == "abc", firstLine(grid));
		}

		static void checkWrapAndScroll(Report& report) {
			Screen screen(4, 2);
			const std::string grid = feedToScreen(screen, "aaaabbbbcccc");
			report.check("wrapping scrolls the grid", grid == "bbbb\ncccc\n", grid);
		}

		static void checkSgrColors(Report& report) {
			Screen screen(10, 1);
			feedToScreen(screen, "\x1b[31mR\x1b[1;32mG\x1b[0mP");

			const bool red_is_red = screen.cell(0, 0).foreground == 0xCD3131;
			const bool green_is_bold = (screen.cell(0, 1).attributes & kAttrBold) != 0
				&& screen.cell(0, 1).foreground == 0x0DBC79;
			const bool reset_clears = screen.cell(0, 2).foreground == kDefaultColor
				&& screen.cell(0, 2).attributes == kAttrNone;

			report.check("SGR sets indexed colors", red_is_red && green_is_bold, "");
			report.check("SGR 0 resets the pen", reset_clears, "");
		}

		static void checkTruecolorAndOsc(Report& report) {
			Screen screen(10, 1);
			feedToScreen(screen, "\x1b]0;my title\x07\x1b[38;2;10;20;30mX");

			report.check("OSC 0 sets the title", screen.title() == "my title", screen.title());
			report.check("SGR 38;2 sets rgb", screen.cell(0, 0).foreground == 0x0A141E, "");
		}

		static void checkUtf8(Report& report) {
			Screen screen(10, 1);
			feedToScreen(screen, "a\xC3\xA9z");

			const bool decoded = screen.cell(0, 0).code == U'a'
				&& screen.cell(0, 1).code == 0xE9
				&& screen.cell(0, 2).code == U'z';
			report.check("UTF-8 sequences decode to one cell", decoded, "");
		}

		static void checkSplitSequence(Report& report) {
			Screen screen(10, 2);
			VtParser parser(screen);

			const std::string first = "ab\x1b[2;";
			const std::string second = "3Hz";
			parser.consume(first.data(), first.size());
			parser.consume(second.data(), second.size());

			const std::string grid = screen.toText();
			report.check("a sequence split across reads still applies",
				grid == "ab\n  z\n", grid);
		}

		static void checkInsertDelete(Report& report) {
			Screen screen(10, 1);
			const std::string grid = feedToScreen(screen, "abcdef\x1b[3G\x1b[2P");
			report.check("DCH deletes characters", firstLine(grid) == "abef", firstLine(grid));
		}

		class RecordingResponder : public VtResponder {
		public:
			void vtRespond(const std::string& bytes) override { replies += bytes; }

			std::string replies;
		};

		static void checkQueryReplies(Report& report) {
			Screen screen(80, 24);
			RecordingResponder responder;
			screen.setResponder(&responder);

			feedToScreen(screen, "\x1b[c");
			report.check("DA1 is answered", responder.replies == "\x1b[?1;2c", responder.replies);

			responder.replies.clear();
			feedToScreen(screen, "\x1b[>c");
			report.check("DA2 is answered", responder.replies == "\x1b[>0;10;1c",
				responder.replies);

			responder.replies.clear();
			feedToScreen(screen, "\x1b[3;7H\x1b[6n");
			report.check("CPR reports the cursor position",
				responder.replies == "\x1b[3;7R", responder.replies);
		}

		static void checkScrollbackKeepsLines(Report& report) {
			Screen screen(10, 2);
			feedToScreen(screen, "one\r\ntwo\r\nthree\r\n");

			const bool kept = screen.scrollbackRows() == 2 && screen.totalRows() == 4;
			const bool oldest_first = screen.cellAt(0, 0).code == U'o'
				&& screen.cellAt(1, 0).code == U't'
				&& screen.cellAt(2, 0).code == U't';

			report.check("lines that scroll off are kept as scrollback", kept,
				std::to_string(screen.scrollbackRows()));
			report.check("scrollback reads oldest first", oldest_first, "");
		}

		static void checkAltScreenKeepsHistory(Report& report) {
			Screen screen(10, 2);
			feedToScreen(screen, "shell\r\noutput\r\n");
			const int before = screen.scrollbackRows();

			feedToScreen(screen, "\x1b[?1049hfull\r\nscreen\r\nmore\r\n");
			const bool on_alt = screen.onAltScreen();
			const bool no_new_history = screen.scrollbackRows() == before;

			feedToScreen(screen, "\x1b[?1049l");
			const bool restored = !screen.onAltScreen()
				&& screen.toText().find("output") != std::string::npos;

			report.check("the alt screen is a separate grid", on_alt, "");
			report.check("the alt screen records no scrollback", no_new_history, "");
			report.check("leaving the alt screen restores what was under it", restored,
				screen.toText());
		}

		static void checkViewScrolling(Report& report) {
			Screen screen(10, 2);
			feedToScreen(screen, "one\r\ntwo\r\nthree\r\nfour\r\n");

			TerminalView view;
			view.followOutput(screen);
			const bool starts_at_bottom = view.topRow(screen) == screen.totalRows() - screen.rows();

			view.scrollBy(2, screen);
			const bool scrolled = view.topRow(screen) == screen.totalRows() - screen.rows() - 2;

			view.scrollBy(100, screen);
			const bool clamped = view.topRow(screen) == 0;

			view.scrollToBottom();
			const bool returned = view.scrollOffset() == 0;

			report.check("the view starts at the bottom", starts_at_bottom, "");
			report.check("scrolling back moves the top row", scrolled, "");
			report.check("scrolling stops at the oldest line", clamped, "");
			report.check("the view can return to the bottom", returned, "");
		}

		static void checkScrolledViewHoldsStill(Report& report) {
			Screen screen(10, 2);
			VtParser parser(screen);
			const std::string first = "one\r\ntwo\r\nthree\r\n";
			parser.consume(first.data(), first.size());

			TerminalView view;
			view.followOutput(screen);
			view.scrollBy(1, screen);
			const int looking_at = view.topRow(screen);

			const std::string more = "four\r\nfive\r\n";
			parser.consume(more.data(), more.size());
			view.followOutput(screen);

			report.check("new output does not slide the text being read",
				view.topRow(screen) == looking_at, std::to_string(view.topRow(screen)));
		}

		static void checkSelectionText(Report& report) {
			Screen screen(20, 3);
			feedToScreen(screen, "hello world\r\nsecond line\r\n");

			TerminalView view;
			view.beginSelection({ 0, 0 });
			view.extendSelection({ 0, 4 });
			const std::string word = view.selectedText(screen);

			view.beginSelection({ 0, 6 });
			view.extendSelection({ 1, 5 });
			const std::string across = view.selectedText(screen);

			report.check("a selection yields its text", word == "hello", word);
			report.check("a selection spanning rows joins with a newline",
				across == "world\r\nsecond", across);
		}

		static void checkSelectionShape(Report& report) {
			Screen screen(20, 3);
			feedToScreen(screen, "alpha beta gamma\r\n");

			TerminalView view;
			view.selectWord({ 0, 8 }, screen);
			const std::string word = view.selectedText(screen);

			view.selectLine({ 0, 3 }, screen);
			const std::string line = view.selectedText(screen);

			view.beginSelection({ 0, 2 });
			view.extendSelection({ 0, 9 });
			const bool marks_cells = view.isSelected(0, 5) && !view.isSelected(0, 12)
				&& !view.isSelected(1, 5);

			view.clearSelection();

			report.check("double click selects a word", word == "beta", word);
			report.check("triple click selects the line, trimmed",
				line == "alpha beta gamma", line);
			report.check("selection covers the cells between its ends", marks_cells, "");
			report.check("a cleared selection yields nothing",
				view.selectedText(screen).empty(), "");
		}

		static KeyPress pressOf(unsigned int virtual_key, bool control, bool alt, bool shift) {
			KeyPress press;
			press.virtual_key = virtual_key;
			press.control     = control;
			press.alt         = alt;
			press.shift       = shift;
			return press;
		}

		static void checkCursorKeyEncoding(Report& report) {
			KeyModes modes;
			const std::string plain = encodeKeyPress(pressOf(VK_UP, false, false, false), modes);
			report.check("an arrow key sends CSI A", plain == "\x1b[A", plain);

			modes.application_cursor = true;
			const std::string application =
				encodeKeyPress(pressOf(VK_UP, false, false, false), modes);
			report.check("application cursor mode sends SS3 A", application == "\x1bOA",
				application);

			modes.application_cursor = false;
			const std::string control =
				encodeKeyPress(pressOf(VK_RIGHT, true, false, false), modes);
			report.check("Ctrl+Right carries the modifier parameter",
				control == "\x1b[1;5C", control);
		}

		static void checkModifierParameters(Report& report) {
			const bool shift_only = modifierParameter(pressOf(0, false, false, true)) == 2;
			const bool alt_only   = modifierParameter(pressOf(0, false, true, false)) == 3;
			const bool ctrl_shift = modifierParameter(pressOf(0, true, false, true)) == 6;
			const bool all_three  = modifierParameter(pressOf(0, true, true, true)) == 8;

			report.check("modifier parameters follow the xterm table",
				shift_only && alt_only && ctrl_shift && all_three, "");
		}

		static void checkFunctionAndEditingKeys(Report& report) {
			KeyModes modes;
			const std::string f1 = encodeKeyPress(pressOf(VK_F1, false, false, false), modes);
			const std::string f5 = encodeKeyPress(pressOf(VK_F5, false, false, false), modes);
			const std::string del =
				encodeKeyPress(pressOf(VK_DELETE, false, false, false), modes);
			const std::string shift_del =
				encodeKeyPress(pressOf(VK_DELETE, false, false, true), modes);

			report.check("F1 sends SS3 P", f1 == "\x1bOP", f1);
			report.check("F5 sends CSI 15~", f5 == "\x1b[15~", f5);
			report.check("Delete sends CSI 3~", del == "\x1b[3~", del);
			report.check("Shift+Delete carries the modifier", shift_del == "\x1b[3;2~",
				shift_del);
		}

		static void checkEraseKeys(Report& report) {
			KeyModes modes;
			const std::string plain =
				encodeKeyPress(pressOf(VK_BACK, false, false, false), modes);
			const std::string control =
				encodeKeyPress(pressOf(VK_BACK, true, false, false), modes);
			const std::string alt = encodeKeyPress(pressOf(VK_BACK, false, true, false), modes);

			report.check("Backspace sends DEL, which is what ConPTY reads as Backspace",
				plain == "\x7f", plain);
			report.check("Ctrl+Backspace sends BS", control == "\x08", control);
			report.check("Alt+Backspace is ESC-prefixed DEL", alt == "\x1b\x7f", alt);
		}

		static void checkTextKeysFallThrough(Report& report) {
			KeyModes modes;
			const bool letter_is_text =
				encodeKeyPress(pressOf('A', false, false, false), modes).empty();
			const bool ctrl_letter_is_text =
				encodeKeyPress(pressOf('C', true, false, false), modes).empty();
			const std::string ctrl_space =
				encodeKeyPress(pressOf(VK_SPACE, true, false, false), modes);

			report.check("plain and Ctrl-held letters are left to the character message",
				letter_is_text && ctrl_letter_is_text, "");
			report.check("Ctrl+Space sends NUL", ctrl_space == std::string(1, '\0'), "");
		}

		static void checkPasteEncoding(Report& report) {
			KeyModes modes;
			const std::string plain = encodePaste("one\r\ntwo", modes);
			report.check("paste turns newlines into CR", plain == "one\rtwo", plain);

			modes.bracketed_paste = true;
			const std::string bracketed = encodePaste("hi", modes);
			report.check("bracketed paste wraps the text",
				bracketed == "\x1b[200~hi\x1b[201~", bracketed);
		}

		static std::size_t countOccurrences(const std::string& haystack,
				const std::string& needle) {
			std::size_t count = 0;
			for (std::size_t at = haystack.find(needle); at != std::string::npos;
					at = haystack.find(needle, at + needle.size())) {
				++count;
			}

			return count;
		}

		static bool waitForText(Session& session, const std::string& needle, int timeout_ms) {
			const auto deadline = std::chrono::steady_clock::now()
				+ std::chrono::milliseconds(timeout_ms);

			while (std::chrono::steady_clock::now() < deadline) {
				session.drainOutput();
				if (session.screen().toText().find(needle) != std::string::npos) return true;
				std::this_thread::sleep_for(std::chrono::milliseconds(25));
			}

			return false;
		}

		static bool waitForCount(Session& session, const std::string& needle,
				std::size_t wanted, int timeout_ms) {
			const auto deadline = std::chrono::steady_clock::now()
				+ std::chrono::milliseconds(timeout_ms);

			while (std::chrono::steady_clock::now() < deadline) {
				session.drainOutput();
				if (countOccurrences(session.screen().toText(), needle) >= wanted) return true;
				std::this_thread::sleep_for(std::chrono::milliseconds(25));
			}

			return false;
		}

		static void runCommand(Session& session, const std::string& command) {
			const std::string line = command + "\r";
			session.writeInput(line.data(), line.size());
		}

		// A resize is only real once the shell has seen it: wbsh republishes
		// COLUMNS from the pseudoconsole, so ask it what it thinks.
		static void checkResizeReachesTheShell(Report& report, Session& session) {
			session.resize(132, 24);

			// wbsh republishes COLUMNS when it draws a prompt, so give it
			// one before asking; querying immediately reads the old width.
			runCommand(session, "");
			std::this_thread::sleep_for(std::chrono::milliseconds(400));
			session.drainOutput();

			runCommand(session, "echo width=$COLUMNS");

			const bool resized = waitForText(session, "width=132", 6000);
			report.check("a resize reaches the shell through ResizePseudoConsole", resized,
				session.screen().toText());
		}

		// cat would echo the literal text back; only the shell expands the
		// arithmetic, so the expanded form proves Ctrl-C got us a prompt.
		static void checkInterrupt(Report& report, Session& session) {
			runCommand(session, "cat");
			std::this_thread::sleep_for(std::chrono::milliseconds(600));
			session.drainOutput();

			const std::string interrupt(1, '\003');
			session.writeInput(interrupt.data(), interrupt.size());
			std::this_thread::sleep_for(std::chrono::milliseconds(400));
			session.drainOutput();

			runCommand(session, "echo interrupted-$((2+2))");

			const bool recovered = waitForText(session, "interrupted-4", 6000);
			report.check("Ctrl-C interrupts the running command", recovered,
				session.screen().toText());
		}

		// Typing a character and erasing it must leave the shell running the
		// shortened command -- the check that plain Backspace really erases.
		static void checkBackspaceErases(Report& report, Session& session) {
			KeyModes modes;
			const std::string typed = "echo erase-okZZ";
			session.writeInput(typed.data(), typed.size());
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
			session.drainOutput();

			const std::string erase =
				encodeKeyPress(pressOf(VK_BACK, false, false, false), modes);
			for (int press = 0; press < 2; ++press) {
				session.writeInput(erase.data(), erase.size());
				std::this_thread::sleep_for(std::chrono::milliseconds(120));
			}

			session.drainOutput();
			runCommand(session, "");

			const bool ran = waitForText(session, "erase-ok", 6000);
			const bool erased =
				session.screen().toText().find("erase-okZ") == std::string::npos;

			report.check("Backspace erases what was typed", ran && erased,
				session.screen().toText());
		}

		static void sendKey(Session& session, unsigned int virtual_key, int times) {
			const KeyModes modes;
			const std::string bytes =
				encodeKeyPress(pressOf(virtual_key, false, false, false), modes);

			for (int press = 0; press < times; ++press) {
				session.writeInput(bytes.data(), bytes.size());
				std::this_thread::sleep_for(std::chrono::milliseconds(120));
			}

			session.drainOutput();
		}

		// Delete needs the cursor moved off the end first, so this also
		// exercises the arrow keys mid-line.
		static void checkDeleteErases(Report& report, Session& session) {
			const std::string typed = "echo cut-okZZ";
			session.writeInput(typed.data(), typed.size());
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
			session.drainOutput();

			sendKey(session, VK_LEFT, 2);
			sendKey(session, VK_DELETE, 2);
			runCommand(session, "");

			const bool ran = waitForText(session, "cut-ok", 6000);
			const bool erased =
				session.screen().toText().find("cut-okZ") == std::string::npos;

			report.check("Delete erases mid-line", ran && erased, session.screen().toText());
		}

		// ConPTY never forwards the alt-screen sequences, so a pager draws
		// into the main grid; what matters is that quitting it leaves a
		// working prompt and the scrollback that was there before.
		static void checkPagerReturnsToShell(Report& report, Session& session) {
			const int history_before = session.screen().scrollbackRows();

			runCommand(session, "ls /c/Windows/System32/drivers/etc | less");
			std::this_thread::sleep_for(std::chrono::milliseconds(1200));
			session.drainOutput();

			const std::string quit = "q";
			session.writeInput(quit.data(), quit.size());
			std::this_thread::sleep_for(std::chrono::milliseconds(800));
			session.drainOutput();

			runCommand(session, "echo pager-done-marker");

			const bool prompt_back = waitForText(session, "pager-done-marker", 6000);
			const bool history_kept = session.screen().scrollbackRows() >= history_before;

			report.check("quitting a pager leaves a working prompt", prompt_back,
				session.screen().toText());
			report.check("a pager does not discard scrollback", history_kept, "");
		}

		// The encoder is only right if the shell's line editor agrees: Up
		// must recall the previous command through ConPTY's key translation.
		static void checkHistoryRecall(Report& report, Session& session) {
			const std::size_t before =
				countOccurrences(session.screen().toText(), "selftest-marker");

			KeyModes modes;
			modes.application_cursor = session.screen().applicationCursorKeys();

			const std::string up = encodeKeyPress(pressOf(VK_UP, false, false, false), modes);
			session.writeInput(up.data(), up.size());

			const bool recalled = waitForCount(session, "selftest-marker", before + 1, 5000);
			report.check("Up recalls the previous command", recalled, session.screen().toText());
			if (!recalled) return;

			const std::string enter = "\r";
			session.writeInput(enter.data(), enter.size());

			const bool ran = waitForCount(session, "selftest-marker", before + 2, 5000);
			report.check("the recalled command runs again", ran, session.screen().toText());
		}

		static void checkLiveSession(Report& report, const std::wstring& command_line) {
			Session session;
			std::string error;
			if (!session.start(command_line, 200, 12, error)) {
				report.check("the shell starts on a pseudoconsole", false, error);
				return;
			}

			report.check("the shell starts on a pseudoconsole", true, "");

			const bool prompt = waitForText(session, "$", 8000);
			report.check("the shell renders a prompt", prompt, session.screen().toText());

			const std::string command = "echo selftest-marker\r";
			session.writeInput(command.data(), command.size());

			const bool echoed = waitForText(session, "selftest-marker", 8000);
			report.check("a typed command runs and its output is parsed into the grid", echoed,
				session.screen().toText());

			checkHistoryRecall(report, session);
			checkBackspaceErases(report, session);
			checkDeleteErases(report, session);
			checkPagerReturnsToShell(report, session);
			checkInterrupt(report, session);
			checkResizeReachesTheShell(report, session);
			session.stop();
		}

		static bool writeReport(const std::wstring& path, const std::string& text) {
			FILE* file = nullptr;
			if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) return false;

			std::fwrite(text.data(), 1, text.size(), file);
			std::fclose(file);
			return true;
		}

	} /* namespace test */

	bool runSelfTest(const std::wstring& report_path, const std::wstring& shell_command_line) {
		test::Report report;

		test::checkPlainText(report);
		test::checkCursorMotion(report);
		test::checkEraseInLine(report);
		test::checkWrapAndScroll(report);
		test::checkSgrColors(report);
		test::checkTruecolorAndOsc(report);
		test::checkUtf8(report);
		test::checkSplitSequence(report);
		test::checkInsertDelete(report);
		test::checkQueryReplies(report);
		test::checkScrollbackKeepsLines(report);
		test::checkAltScreenKeepsHistory(report);
		test::checkViewScrolling(report);
		test::checkScrolledViewHoldsStill(report);
		test::checkSelectionText(report);
		test::checkSelectionShape(report);
		test::checkCursorKeyEncoding(report);
		test::checkModifierParameters(report);
		test::checkFunctionAndEditingKeys(report);
		test::checkEraseKeys(report);
		test::checkTextKeysFallThrough(report);
		test::checkPasteEncoding(report);
		test::checkLiveSession(report, shell_command_line);

		test::writeReport(report_path, report.text());
		return report.passed();
	}

} /* namespace wbshterm */

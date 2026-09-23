/**
 * @file selftest.cpp
 * @brief Golden grid checks plus a live round trip through the pty.
 */

#include "selftest.h"

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

		static void checkLiveSession(Report& report, const std::wstring& command_line) {
			Session session;
			std::string error;
			if (!session.start(command_line, 90, 12, error)) {
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
		test::checkLiveSession(report, shell_command_line);

		test::writeReport(report_path, report.text());
		return report.passed();
	}

} /* namespace wbshterm */

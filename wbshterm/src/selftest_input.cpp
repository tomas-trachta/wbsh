/**
 * @file selftest_input.cpp
 * @brief Checks for input to the window and text out of it: typing, the wheel, copying.
 */

#include "selftest_internal.h"

#include "mouse.h"
#include "picker.h"
#include "utf8.h"
#include "view.h"

#include <string>

namespace wbshterm {

	namespace test {

		static std::string encoded(char32_t code) {
			std::string bytes;
			appendUtf8(bytes, code);
			return bytes;
		}

		static void checkUtf8Encoding(Report& report) {
			report.check("ASCII encodes as itself", encoded(U'A') == "A", encoded(U'A'));
			report.check("a Czech letter encodes as two bytes", encoded(0x0159) == "\xc5\x99", "");
			report.check("a CJK character encodes as three bytes",
				encoded(0x4E2D) == "\xe4\xb8\xad", "");
			report.check("an astral emoji encodes as four bytes",
				encoded(0x1F600) == "\xf0\x9f\x98\x80", "");
			report.check("a lone surrogate encodes as U+FFFD",
				encoded(0xD83D) == "\xef\xbf\xbd", "");

			std::string text = "a\xc5\x99";
			popLastUtf8(text);
			report.check("deleting the last character drops all of its bytes", text == "a", text);

			report.check("UTF-8 widens to UTF-16",
				widenUtf8("\xc5\xbe\xf0\x9f\x98\x80") == L"ž\U0001F600", "");
		}

		static const wchar_t kGrinningHigh = static_cast<wchar_t>(0xD83D);
		static const wchar_t kGrinningLow  = static_cast<wchar_t>(0xDE00);

		static void checkSurrogatePairsJoin(Report& report) {
			SurrogateJoiner joiner;
			char32_t code = 0;
			const bool high_waits = !joiner.take(kGrinningHigh, code);
			const bool low_completes = joiner.take(kGrinningLow, code) && code == 0x1F600;

			std::string bytes;
			appendUtf8(bytes, code);
			report.check("a high surrogate waits for its low half", high_waits, "");
			report.check("an emoji typed as two WM_CHARs is sent as one UTF-8 character",
				low_completes && bytes == "\xf0\x9f\x98\x80", bytes);

			char32_t letter = 0;
			const bool bmp_passes = joiner.take(static_cast<wchar_t>(0x017E), letter)
				&& letter == 0x017E;
			report.check("a BMP character passes straight through", bmp_passes, "");
		}

		static void checkLoneSurrogatesAreDropped(Report& report) {
			SurrogateJoiner joiner;
			char32_t code = 0;
			const bool lone_low_dropped = !joiner.take(kGrinningLow, code);

			joiner.take(kGrinningHigh, code);
			const bool next_letter_kept = joiner.take(L'a', code) && code == U'a';
			const bool stale_high_gone = !joiner.take(kGrinningLow, code);

			report.check("a low surrogate with no high half is dropped", lone_low_dropped, "");
			report.check("a high surrogate left alone does not eat the next character",
				next_letter_kept && stale_high_gone, "");
		}

		static void checkWholeStringsNarrowAsOne(Report& report) {
			const std::wstring path = std::wstring(L"C:\\")
				+ kGrinningHigh + kGrinningLow + L".txt";
			const std::string narrowed = narrowUtf16(path);
			report.check("a dropped path with an emoji narrows to proper UTF-8",
				narrowed == "C:\\\xf0\x9f\x98\x80.txt", narrowed);
		}

		static std::string selectRows(const Screen& screen, int first_row, int last_row) {
			TerminalView view;
			view.beginSelection({ first_row, 0 });
			view.extendSelection({ last_row, screen.columns() - 1 });
			view.endSelection();
			return view.selectedText(screen);
		}

		static void checkCopiedTextIsUtf8(Report& report) {
			const std::string czech = "p\xc5\x99\xc3\xadli\xc5\xa1 "
				"\xc5\xbelu\xc5\xa5ou\xc4\x8dk\xc3\xbd";
			Screen screen(40, 3);
			feedToScreen(screen, czech);

			const std::string copied = selectRows(screen, 0, 0);
			report.check("copying Czech text keeps its letters", copied == czech, copied);
		}

		static void checkCopiedWideAndClusteredCells(Report& report) {
			Screen wide(10, 2);
			feedToScreen(wide, "\xe4\xb8\xadX");
			const std::string wide_text = selectRows(wide, 0, 0);
			report.check("a wide character copies once, without a trailing blank",
				wide_text == "\xe4\xb8\xadX", wide_text);

			Screen marked(10, 2);
			feedToScreen(marked, "e\xcc\x81x");
			const std::string marked_text = selectRows(marked, 0, 0);
			report.check("a combining mark copies with its base",
				marked_text == "e\xcc\x81x", marked_text);

			Screen emoji(10, 2);
			feedToScreen(emoji, "\xf0\x9f\x98\x80!");
			const std::string emoji_text = selectRows(emoji, 0, 0);
			report.check("an astral emoji copies as four bytes",
				emoji_text == "\xf0\x9f\x98\x80!", emoji_text);
		}

		static void checkCopiedWrappedLines(Report& report) {
			Screen wrapped(5, 3);
			feedToScreen(wrapped, "abcdefgh");
			const std::string joined = selectRows(wrapped, 0, 1);
			report.check("a soft-wrapped line copies as one line", joined == "abcdefgh", joined);

			Screen ended(5, 3);
			feedToScreen(ended, "ab\r\ncd");
			const std::string separate = selectRows(ended, 0, 1);
			report.check("a hard line break copies as CRLF", separate == "ab\r\ncd", separate);
		}

		static void checkPickerTakesNonAscii(Report& report) {
			Picker picker;
			picker.begin("pick");
			picker.addItem("p\xc5\x99\xc3\xadli\xc5\xa1.txt");
			picker.addItem("plain.txt");
			picker.finish();

			picker.typeCharacter(0x0159);
			const bool narrowed = picker.query() == "\xc5\x99" && picker.matches().size() == 1;

			picker.backspace();
			const bool restored = picker.query().empty() && picker.matches().size() == 2;

			report.check("the picker query takes a non-ASCII letter", narrowed, picker.query());
			report.check("backspace in the picker drops the whole letter", restored,
				picker.query());
		}

		static void checkWheelNotchesScrollWholeLines(Report& report) {
			WheelAccumulator wheel;
			const int notch = wheel.linesFor(120, 3);
			const int back = wheel.linesFor(-240, 3);
			const int page = wheel.linesFor(120, 40);
			const int off = wheel.linesFor(120, 0);

			report.check("one wheel notch scrolls the configured lines", notch == 3,
				std::to_string(notch));
			report.check("turning the wheel back scrolls the other way", back == -6,
				std::to_string(back));
			report.check("a page-per-notch setting scrolls a page", page == 40,
				std::to_string(page));
			report.check("a zero-lines setting does not scroll", off == 0, std::to_string(off));
		}

		static void checkTouchpadDeltasAccumulate(Report& report) {
			WheelAccumulator wheel;
			int total = 0;
			for (int event = 0; event < 10; ++event) total += wheel.linesFor(12, 3);

			report.check("small touchpad deltas add up to whole lines", total == 3,
				std::to_string(total));

			wheel.reset();
			const int forward = wheel.linesFor(30, 3);
			const int reversed = wheel.linesFor(-30, 3) + wheel.linesFor(-30, 3);
			report.check("a reversed swipe does not first pay back the old remainder",
				forward == 0 && reversed == -1, std::to_string(reversed));
		}

		static void checkMousePixelsBecomeDips(Report& report) {
			const float at_100 = pixelsToDips(300, 96);
			const float at_125 = pixelsToDips(300, 120);
			const float at_150 = pixelsToDips(300, 144);
			const float unknown = pixelsToDips(300, 0);

			report.check("at 100% scale a pixel is a DIP", at_100 == 300.0f,
				std::to_string(at_100));
			report.check("at 125% and 150% scale pixels shrink to DIPs",
				at_125 == 240.0f && at_150 == 200.0f,
				std::to_string(at_125) + " " + std::to_string(at_150));
			report.check("an unknown DPI counts as 100%", unknown == 300.0f,
				std::to_string(unknown));
		}

		static MouseReport mouseAt(MouseAction action, MouseButton button, int column, int row) {
			MouseReport report;
			report.action = action;
			report.button = button;
			report.column = column;
			report.row    = row;
			return report;
		}

		static MouseModes modesOf(MouseTracking tracking, MouseEncoding encoding) {
			MouseModes modes;
			modes.tracking = tracking;
			modes.encoding = encoding;
			return modes;
		}

		static void checkSgrMouseReports(Report& report) {
			const MouseModes sgr = modesOf(MouseTracking::kDrags, MouseEncoding::kSgr);
			MouseReport press = mouseAt(MouseAction::kPress, MouseButton::kRight, 4, 9);
			press.control = true;

			const std::string pressed = encodeMouseReport(sgr, press);
			const std::string released = encodeMouseReport(sgr,
				mouseAt(MouseAction::kRelease, MouseButton::kRight, 4, 9));
			const std::string dragged = encodeMouseReport(sgr,
				mouseAt(MouseAction::kMotion, MouseButton::kLeft, 300, 0));
			const std::string wheel = encodeMouseReport(sgr,
				mouseAt(MouseAction::kPress, MouseButton::kWheelDown, 0, 0));

			report.check("SGR reports name the button, its cell and Ctrl",
				pressed == "\x1b[<18;5;10M", pressed);
			report.check("an SGR release keeps its button and ends in m",
				released == "\x1b[<2;5;10m", released);
			report.check("SGR drags add 32 and reach any column",
				dragged == "\x1b[<32;301;1M", dragged);
			report.check("the wheel is buttons 64 and 65", wheel == "\x1b[<65;1;1M", wheel);
		}

		static void checkLegacyMouseReports(Report& report) {
			const MouseModes x10 = modesOf(MouseTracking::kClicks, MouseEncoding::kDefault);
			const std::string pressed = encodeMouseReport(x10,
				mouseAt(MouseAction::kPress, MouseButton::kMiddle, 0, 2));
			const std::string released = encodeMouseReport(x10,
				mouseAt(MouseAction::kRelease, MouseButton::kMiddle, 0, 2));
			const std::string far_off = encodeMouseReport(x10,
				mouseAt(MouseAction::kPress, MouseButton::kLeft, 223, 0));

			report.check("X10 bytes offset everything by 32", pressed == "\x1b[M!!#", pressed);
			report.check("an X10 release says only that a button came up",
				released == "\x1b[M#!#", released);
			report.check("a column X10 bytes cannot spell sends nothing", far_off.empty(), far_off);

			const MouseModes utf8 = modesOf(MouseTracking::kClicks, MouseEncoding::kUtf8);
			const std::string wide = encodeMouseReport(utf8,
				mouseAt(MouseAction::kPress, MouseButton::kLeft, 223, 0));
			report.check("UTF-8 encoding reaches past column 223",
				wide == "\x1b[M \xC4\x80!", wide);
		}

		static void checkTrackingModesFilterEvents(Report& report) {
			const MouseReport hover = mouseAt(MouseAction::kMotion, MouseButton::kNone, 1, 1);
			const MouseReport drag = mouseAt(MouseAction::kMotion, MouseButton::kLeft, 1, 1);
			const MouseReport release = mouseAt(MouseAction::kRelease, MouseButton::kLeft, 1, 1);
			const MouseReport wheel_up = mouseAt(MouseAction::kRelease, MouseButton::kWheelUp,
				1, 1);

			report.check("1000 reports clicks but no motion",
				mouseTrackingWants(MouseTracking::kClicks, release)
					&& !mouseTrackingWants(MouseTracking::kClicks, drag), "");
			report.check("1002 adds drags, 1003 adds plain motion",
				mouseTrackingWants(MouseTracking::kDrags, drag)
					&& !mouseTrackingWants(MouseTracking::kDrags, hover)
					&& mouseTrackingWants(MouseTracking::kMotion, hover), "");
			report.check("X10 mode reports presses only, and off reports nothing",
				!mouseTrackingWants(MouseTracking::kX10, release)
					&& !mouseTrackingWants(MouseTracking::kOff, release), "");
			report.check("the wheel never releases",
				!mouseTrackingWants(MouseTracking::kMotion, wheel_up), "");

			MouseReport modified = mouseAt(MouseAction::kPress, MouseButton::kLeft, 0, 0);
			modified.alt = true;
			const std::string plain = encodeMouseReport(
				modesOf(MouseTracking::kX10, MouseEncoding::kSgr), modified);
			report.check("X10 mode leaves the modifiers out", plain == "\x1b[<0;1;1M", plain);
		}

	} /* namespace test */

	void runInputChecks(test::Report& report) {
		test::checkUtf8Encoding(report);
		test::checkSurrogatePairsJoin(report);
		test::checkLoneSurrogatesAreDropped(report);
		test::checkWholeStringsNarrowAsOne(report);
		test::checkCopiedTextIsUtf8(report);
		test::checkCopiedWideAndClusteredCells(report);
		test::checkCopiedWrappedLines(report);
		test::checkPickerTakesNonAscii(report);
		test::checkWheelNotchesScrollWholeLines(report);
		test::checkTouchpadDeltasAccumulate(report);
		test::checkMousePixelsBecomeDips(report);
		test::checkSgrMouseReports(report);
		test::checkLegacyMouseReports(report);
		test::checkTrackingModesFilterEvents(report);
	}

} /* namespace wbshterm */

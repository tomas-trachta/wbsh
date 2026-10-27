/**
 * @file selftest_input.cpp
 * @brief Checks for text going in and out of the window: typing, copying, the picker.
 */

#include "selftest_internal.h"

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
	}

} /* namespace wbshterm */

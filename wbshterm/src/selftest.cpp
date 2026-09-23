/**
 * @file selftest.cpp
 * @brief Golden grid checks plus a live round trip through the pty.
 */

#include "selftest.h"

#include "charwidth.h"
#include "config.h"
#include "fetch.h"
#include "keymap.h"
#include "menu.h"
#include "picker.h"
#include "view.h"
#include "session.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
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

		static std::size_t countOccurrences(const std::string& haystack,
				const std::string& needle) {
			std::size_t count = 0;
			for (std::size_t at = haystack.find(needle); at != std::string::npos;
					at = haystack.find(needle, at + needle.size())) {
				++count;
			}

			return count;
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

			const bool red_is_red = screen.cell(0, 0).foreground == (kPaletteColor | 1u);
			const bool green_is_bold = (screen.cell(0, 1).attributes & kAttrBold) != 0
				&& screen.cell(0, 1).foreground == (kPaletteColor | 2u);
			const bool reset_clears = screen.cell(0, 2).foreground == kDefaultColor
				&& screen.cell(0, 2).attributes == kAttrNone;

			report.check("SGR keeps indexed colours as palette slots",
				red_is_red && green_is_bold, "");
			report.check("SGR 0 resets the pen", reset_clears, "");
		}

		static void checkTruecolorAndOsc(Report& report) {
			Screen screen(10, 1);
			feedToScreen(screen, "\x1b]0;my title\a\x1b[38;2;10;20;30mX");

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

		static void checkCharacterWidths(Report& report) {
			const bool narrow = characterWidth(U'a') == 1 && characterWidth(U'~') == 1;
			const bool wide = characterWidth(0x4E2D) == 2 && characterWidth(0x1F680) == 2
				&& characterWidth(0xFF21) == 2;
			const bool zero = characterWidth(0x0301) == 0 && characterWidth(0xFE0F) == 0;

			report.check("latin text is one cell wide", narrow, "");
			report.check("CJK and emoji are two cells wide", wide, "");
			report.check("combining marks take no cell", zero, "");
		}

		static void checkWideCharactersTakeTwoCells(Report& report) {
			Screen screen(10, 2);
			feedToScreen(screen, "a\xE4\xB8\xAD" "b");

			const bool lead = (screen.cell(0, 1).attributes & kAttrWide) != 0;
			const bool tail = (screen.cell(0, 2).attributes & kAttrWideTail) != 0;
			const bool after = screen.cell(0, 3).code == U'b';

			report.check("a wide character claims two cells", lead && tail, "");
			report.check("text after a wide character lands past it", after, "");
		}

		static void checkResizeKeepsContent(Report& report) {
			Screen screen(20, 4);
			feedToScreen(screen, "first\r\nsecond\r\nthird\r\n");

			screen.resize(20, 3);
			const std::string shrunk = screen.toText();

			screen.resize(30, 6);
			const std::string grown = screen.toText();

			report.check("shrinking keeps the text that fits",
				shrunk.find("third") != std::string::npos, shrunk);
			report.check("growing keeps the text already there",
				grown.find("second") != std::string::npos
				&& grown.find("third") != std::string::npos, grown);
			report.check("rows pushed off by a resize become scrollback",
				screen.scrollbackRows() > 0, std::to_string(screen.scrollbackRows()));
		}

		static void checkThemesAreAvailable(Report& report) {
			Palette palette;
			const bool found = findBuiltInTheme("dracula", palette);
			const bool coloured = palette.background == 0x282A36 && palette.ansi[1] == 0xFF5555;
			const bool insensitive = findBuiltInTheme("Nord", palette);
			const bool missing = !findBuiltInTheme("no-such-theme", palette);

			report.check("built-in themes load by name", found && coloured, "");
			report.check("theme names ignore case", insensitive, "");
			report.check("an unknown theme is reported, not guessed", missing, "");
			report.check("several themes ship", builtInThemeNames().size() >= 8,
				std::to_string(builtInThemeNames().size()));
		}

		static void checkConfigRoundTrip(Report& report, const std::wstring& directory) {
			const std::wstring path = directory + L"wbshterm-test.conf";
			if (!writeDefaultConfig(path)) {
				report.check("the default configuration file is written", false, "");
				return;
			}

			report.check("the default configuration file is written", true, "");

			Config config;
			std::string error;
			const bool loaded = loadConfig(path, config, error);
			report.check("the default file parses", loaded, error);

			FILE* file = nullptr;
			if (_wfopen_s(&file, path.c_str(), L"wb") == 0 && file != nullptr) {
				const char* text =
					"[font]\nfamily = Consolas\nsize = 14\n"
					"[cursor]\nstyle = bar\nblink = false\n"
					"[window]\npadding = 24\nopacity = 0.85\n"
					"[theme]\nname = nord\nforeground = #ABCDEF\n";
				std::fwrite(text, 1, std::strlen(text), file);
				std::fclose(file);
			}

			Config edited;
			loadConfig(path, edited, error);

			const bool font = edited.font.family == L"Consolas" && edited.font.size == 14.0f;
			const bool cursor = edited.cursor.style == CursorStyle::Bar && !edited.cursor.blink;
			const bool window = edited.window.padding == 24 && edited.window.opacity > 0.84f
				&& edited.window.opacity < 0.86f;
			const bool theme = edited.palette.background == 0x2E3440
				&& edited.palette.foreground == 0xABCDEF;

			report.check("the font is configurable", font, "");
			report.check("the cursor style and blink are configurable", cursor, "");
			report.check("padding and opacity are configurable", window, "");
			report.check("a theme applies and single colours override it", theme, "");

			_wremove(path.c_str());
		}

		// Every menu entry has to be reachable: walking the real menu is the
		// only way to know the ids the window will actually receive.
		static MenuChoice choiceFromMenu(HMENU menu, const std::wstring& label,
				const std::vector<std::string>& themes) {
			const int count = ::GetMenuItemCount(menu);
			for (int i = 0; i < count; ++i) {
				wchar_t text[128] = {};
				::GetMenuStringW(menu, static_cast<UINT>(i), text, 128, MF_BYPOSITION);

				HMENU child = ::GetSubMenu(menu, i);
				if (child != nullptr) {
					const MenuChoice found = choiceFromMenu(child, label, themes);
					if (found.action != MenuAction::None) return found;
					continue;
				}

				if (label == text) {
					return menuChoiceFor(static_cast<int>(::GetMenuItemID(menu,
						static_cast<UINT>(i))), themes);
				}
			}

			return MenuChoice();
		}

		// A colour written above the theme name must not be swallowed by it:
		static void checkThemeOverridesIgnoreOrder(Report& report,
				const std::wstring& directory) {
			const std::wstring path = directory + L"wbshterm-order.conf";

			FILE* file = nullptr;
			if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) {
				report.check("overrides work whatever order they are written in", false, "");
				return;
			}

			const char* text = "[theme]\nbackground = #7F0000\nansi1 = #010203\nname = nord\n";
			std::fwrite(text, 1, std::strlen(text), file);
			std::fclose(file);

			Config config;
			std::string error;
			loadConfig(path, config, error);

			const bool kept = config.palette.background == 0x7F0000
				&& config.palette.ansi[1] == 0x010203;
			const bool themed = config.palette.foreground == 0xD8DEE9;

			report.check("overrides work whatever order they are written in", kept, "");
			report.check("the named theme still fills in what was not overridden", themed, "");

			_wremove(path.c_str());
		}

		static bool writeTextFile(const std::wstring& path, const char* text) {
			FILE* file = nullptr;
			if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) return false;

			std::fwrite(text, 1, std::strlen(text), file);
			std::fclose(file);
			return true;
		}

		// A palette dropped in the themes folder is a theme: it gets a name from
		// its file, shows up in the menu, and can replace a built-in.
		struct ThemeFolderFixture {
			std::wstring config_path;
			std::wstring themes;
			std::wstring mine;
			std::wstring shadow;
		};

		static ThemeFolderFixture makeThemeFolder(const std::wstring& directory) {
			ThemeFolderFixture fixture;
			fixture.config_path = directory + L"wbshterm-folder.conf";
			fixture.themes      = themesDirectory(fixture.config_path);
			fixture.mine        = fixture.themes + L"\\midnight.conf";
			fixture.shadow      = fixture.themes + L"\\nord.conf";

			::CreateDirectoryW(fixture.themes.c_str(), nullptr);
			writeTextFile(fixture.mine,
				"background = #101014\nforeground = #C8D0E0\nansi2 = #7FD88F\n");
			writeTextFile(fixture.shadow, "[theme]\nbackground = #123456\n");
			return fixture;
		}

		static void removeThemeFolder(const ThemeFolderFixture& fixture) {
			_wremove((fixture.themes + L"\\example.conf.txt").c_str());
			_wremove(fixture.mine.c_str());
			_wremove(fixture.shadow.c_str());
			_wremove(fixture.config_path.c_str());
			::RemoveDirectoryW(fixture.themes.c_str());
		}

		// A palette dropped in the themes folder is a theme: it gets a name from
		// its file, and can replace a built-in of the same name.
		static void checkThemesFolder(Report& report, const ThemeFolderFixture& fixture) {
			const std::vector<std::string> names = availableThemeNames(fixture.themes);
			const bool listed =
				std::find(names.begin(), names.end(), "midnight") != names.end();
			const bool built_ins_kept =
				std::find(names.begin(), names.end(), "dracula") != names.end();
			const bool no_duplicate_nord =
				std::count(names.begin(), names.end(), "nord") == 1;

			Palette palette;
			const bool loaded = findTheme(fixture.themes, "midnight", palette)
				&& palette.background == 0x101014 && palette.ansi[2] == 0x7FD88F;

			Palette shadowed;
			const bool replaces = findTheme(fixture.themes, "nord", shadowed)
				&& shadowed.background == 0x123456;

			Palette untouched;
			const bool still_built_in = findTheme(fixture.themes, "dracula", untouched)
				&& untouched.background == 0x282A36;

			report.check("a file in the themes folder becomes a theme", listed && loaded, "");
			report.check("the built-in themes are still listed", built_ins_kept, "");
			report.check("a file named after a built-in replaces it, once",
				replaces && no_duplicate_nord, "");
			report.check("other built-ins are unaffected", still_built_in, "");
		}

		static void checkFolderThemeReachesConfigAndMenu(Report& report,
				const ThemeFolderFixture& fixture) {
			writeTextFile(fixture.config_path, "[theme]\nname = midnight\nansi1 = #FF0000\n");

			Config config;
			std::string error;
			loadConfig(fixture.config_path, config, error);

			report.check("the config can name a theme from the folder",
				config.palette.background == 0x101014 && config.palette.ansi[2] == 0x7FD88F,
				config.theme_name);
			report.check("colours in the config still override a folder theme",
				config.palette.ansi[1] == 0xFF0000, "");

			const std::vector<std::string> names = availableThemeNames(fixture.themes);
			HMENU menu = buildTerminalMenu(config, names, false, false);
			const MenuChoice chosen = choiceFromMenu(menu, L"midnight", names);
			::DestroyMenu(menu);

			report.check("the menu offers themes from the folder",
				chosen.action == MenuAction::SetTheme && chosen.text == "midnight", chosen.text);
		}

		// What is written out has to be readable back as the same theme, or
		// editing a shipped palette would quietly change it.
		static void checkShippedThemesAreWrittenOut(Report& report, const std::wstring& themes) {
			ensureThemesDirectory(themes);

			const std::wstring path = themes + L"\\dracula.conf";
			const bool written =
				::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;

			Palette built_in;
			findBuiltInTheme("dracula", built_in);

			Palette from_file;
			const bool loaded = findTheme(themes, "dracula", from_file);
			bool same = loaded && from_file.background == built_in.background
				&& from_file.foreground == built_in.foreground
				&& from_file.cursor == built_in.cursor
				&& from_file.selection == built_in.selection;
			for (int i = 0; i < 16 && same; ++i) same = from_file.ansi[i] == built_in.ansi[i];

			report.check("every built-in theme is written to the folder", written, "");
			report.check("a written theme reads back as the same colours", same, "");

			writeTextFile(path, "background = #010203\n");
			Palette edited;
			const bool honoured = findTheme(themes, "dracula", edited)
				&& edited.background == 0x010203;
			report.check("editing a shipped theme file changes that theme", honoured, "");

			_wremove(path.c_str());
			Palette restored;
			report.check("deleting the file brings the built-in back",
				findTheme(themes, "dracula", restored)
				&& restored.background == built_in.background, "");
		}

		// Editing a theme file must be noticed, not just the config's own.
		static void checkThemeEditsAreNoticed(Report& report, const std::wstring& config_path) {
			const std::wstring themes = themesDirectory(config_path);
			ensureThemesDirectory(themes);
			writeTextFile(config_path, "[theme]\nname = nord\n");

			const unsigned long long before = settingsStamp(config_path, "nord");

			std::this_thread::sleep_for(std::chrono::milliseconds(40));
			writeTextFile(themes + L"\\nord.conf", "background = #020304\n");

			const unsigned long long after = settingsStamp(config_path, "nord");
			const unsigned long long other = settingsStamp(config_path, "dracula");

			report.check("editing the active theme file is noticed", before != after, "");
			report.check("an untouched theme does not look edited",
				other == settingsStamp(config_path, "dracula"), "");
		}

		// The folder button has to create what it opens.
		static void checkThemesFolderIsCreated(Report& report, const std::wstring& themes) {
			_wremove((themes + L"\\example.conf.txt").c_str());
			::RemoveDirectoryW(themes.c_str());

			const bool created = ensureThemesDirectory(themes)
				&& ::GetFileAttributesW(themes.c_str()) != INVALID_FILE_ATTRIBUTES
				&& ::GetFileAttributesW((themes + L"\\example.conf.txt").c_str())
					!= INVALID_FILE_ATTRIBUTES;

			report.check("opening the themes folder creates it, with an example", created, "");
		}

		static void checkMenuOffersCustomisation(Report& report) {
			Config config;
			findBuiltInTheme(config.theme_name, config.palette);

			const std::vector<std::string> themes = builtInThemeNames();
			HMENU menu = buildTerminalMenu(config, themes, true, true);

			const MenuChoice theme = choiceFromMenu(menu, L"dracula", themes);
			const MenuChoice cursor = choiceFromMenu(menu, L"Underline", themes);
			const MenuChoice font = choiceFromMenu(menu, L"14 pt", themes);
			const MenuChoice opacity = choiceFromMenu(menu, L"90%", themes);
			const MenuChoice padding = choiceFromMenu(menu, L"24 px", themes);

			report.check("the menu offers every built-in theme",
				theme.action == MenuAction::SetTheme && theme.text == "dracula", theme.text);
			report.check("the menu offers cursor styles",
				cursor.action == MenuAction::SetCursorStyle
				&& cursor.style == CursorStyle::Underline, "");
			report.check("the menu offers font sizes",
				font.action == MenuAction::SetFontSize && font.number == 14, "");
			report.check("the menu offers opacity levels",
				opacity.action == MenuAction::SetOpacity && opacity.number == 90, "");
			// Source is UTF-8; without /utf-8 the compiler reads it as the
			// system code page and these labels arrive as mojibake. The text is
			// built from code points here so the check cannot be mangled too.
			const std::wstring ellipsis(1, static_cast<wchar_t>(0x2026));
			bool saw_config_label = false;
			bool saw_themes_label = false;

			for (int i = 0; i < ::GetMenuItemCount(menu); ++i) {
				wchar_t label[128] = {};
				::GetMenuStringW(menu, static_cast<UINT>(i), label, 128, MF_BYPOSITION);
				if (std::wstring(label) == L"Edit configuration file" + ellipsis) {
					saw_config_label = true;
				}

				if (std::wstring(label) == L"Open themes folder" + ellipsis) {
					saw_themes_label = true;
				}
			}

			report.check("menu labels keep their non-ASCII characters",
				saw_config_label && saw_themes_label, "");
			report.check("the menu can open the themes folder", saw_themes_label, "");

			report.check("the menu offers padding levels",
				padding.action == MenuAction::SetPadding && padding.number == 24, "");

			::DestroyMenu(menu);
		}

		static void checkMenuChoicesApply(Report& report) {
			Config config;
			findBuiltInTheme(config.theme_name, config.palette);

			MenuChoice theme;
			theme.action = MenuAction::SetTheme;
			theme.text   = "nord";
			const bool changed = applyMenuChoice(theme, std::wstring(), config);

			MenuChoice blink;
			blink.action = MenuAction::ToggleBlink;
			const bool was_blinking = config.cursor.blink;
			applyMenuChoice(blink, std::wstring(), config);

			report.check("choosing a theme repaints in its colours",
				changed && config.palette.background == 0x2E3440, "");
			report.check("choosing the same theme again changes nothing",
				!applyMenuChoice(theme, std::wstring(), config), "");
			report.check("blinking toggles", config.cursor.blink != was_blinking, "");
		}

		static void checkMenuChoicesPersist(Report& report, const std::wstring& directory) {
			const std::wstring path = directory + L"wbshterm-menu.conf";
			writeDefaultConfig(path);

			Config config;
			std::string error;
			loadConfig(path, config, error);

			MenuChoice theme;
			theme.action = MenuAction::SetTheme;
			theme.text   = "gruvbox-dark";
			applyMenuChoice(theme, std::wstring(), config);

			std::string section;
			std::string key;
			std::string value;
			settingForChoice(theme, config, section, key, value);
			updateConfigValue(path, section, key, value);

			MenuChoice padding;
			padding.action = MenuAction::SetPadding;
			padding.number = 24;
			applyMenuChoice(padding, std::wstring(), config);
			settingForChoice(padding, config, section, key, value);
			updateConfigValue(path, section, key, value);

			Config reloaded;
			loadConfig(path, reloaded, error);

			std::string text;
			FILE* file = nullptr;
			if (_wfopen_s(&file, path.c_str(), L"rb") == 0 && file != nullptr) {
				char buffer[4096];
				const std::size_t got = std::fread(buffer, 1, sizeof(buffer), file);
				text.assign(buffer, got);
				std::fclose(file);
			}

			report.check("a menu choice survives a reload",
				reloaded.theme_name == "gruvbox-dark" && reloaded.window.padding == 24,
				reloaded.theme_name);
			report.check("writing a setting keeps the rest of the file",
				text.find("# wbshterm configuration.") != std::string::npos
				&& text.find("fallback = ") != std::string::npos, "");
			report.check("a setting is written once, not appended twice",
				countOccurrences(text, "padding = ") == 1,
				std::to_string(countOccurrences(text, "padding = ")));

			_wremove(path.c_str());
		}

		static std::size_t countLines(const std::string& text) {
			return countOccurrences(text, "\n");
		}

		// The panel is laid out from a fixed set of rows here, so the shape is
		// checked rather than this machine's own numbers.
		static void checkFetchPanelLayout(Report& report) {
			FetchInfo info;
			info.user = "ada";
			info.host = "analytical";
			info.rows.push_back({ "OS", "Windows" });
			info.rows.push_back({ "Shell", "wbsh 1.0.10" });
			info.rows.push_back({ "Nothing", "" });

			const std::string panel = renderFetchPanel(info);

			report.check("the panel names the user and host",
				panel.find("ada") != std::string::npos
				&& panel.find("analytical") != std::string::npos, "");
			report.check("the panel lists the rows it was given",
				panel.find("OS") != std::string::npos
				&& panel.find("wbsh 1.0.10") != std::string::npos, "");
			report.check("a row with nothing to say is left out",
				panel.find("Nothing") == std::string::npos, "");
			report.check("the logo is drawn beside the rows",
				panel.find("\\u2588") != std::string::npos
				|| panel.find("\x1b[36;1m") != std::string::npos, "");
			report.check("the palette strip shows both halves",
				panel.find("\x1b[40m") != std::string::npos
				&& panel.find("\x1b[107m") != std::string::npos, "");
		}

		// However many rows there are, the panel keeps its shape: never fewer
		// lines than the logo, one line per row when there are more.
		static void checkFetchPanelGrowsWithRows(Report& report) {
			FetchInfo small;
			small.user = "a";
			small.host = "b";
			small.rows.push_back({ "One", "1" });

			FetchInfo large = small;
			for (int i = 0; i < 12; ++i) {
				large.rows.push_back({ "Row" + std::to_string(i), std::to_string(i) });
			}

			const std::size_t small_lines = countLines(renderFetchPanel(small));
			const std::size_t large_lines = countLines(renderFetchPanel(large));

			report.check("a short panel is still as tall as the logo", small_lines >= 8,
				std::to_string(small_lines));
			report.check("more rows make a taller panel", large_lines > small_lines,
				std::to_string(large_lines));
		}

		static void checkStartupFetchSetting(Report& report, const std::wstring& directory) {
			const std::wstring path = directory + L"wbshterm-startup.conf";
			writeTextFile(path, "[startup]\nfetch = false\n");

			Config off;
			std::string error;
			loadConfig(path, off, error);

			writeTextFile(path, "[startup]\nfetch = true\n");
			Config on;
			loadConfig(path, on, error);

			report.check("the startup panel can be switched off",
				!off.startup_fetch && on.startup_fetch, "");
			_wremove(path.c_str());
		}

		// The shell says where a prompt began, where its output began, and
		// how it ended; the grid turns that into blocks it can navigate.
		static void checkShellMarksMakeBlocks(Report& report) {
			Screen screen(40, 6);
			feedToScreen(screen,
				"\x1b]633;A\a$ echo one\r\n\x1b]633;C\aone\r\n\x1b]633;D;0\a"
				"\x1b]633;A\a$ false\r\n\x1b]633;C\a\x1b]633;D;1\a");

			const std::vector<CommandBlock>& blocks = screen.commandBlocks();
			if (blocks.size() != 2) {
				report.check("each marked command becomes a block", false,
					std::to_string(blocks.size()));
				return;
			}

			const bool rows_ordered = blocks[0].prompt_row < blocks[0].output_row
				&& blocks[0].output_row <= blocks[0].end_row
				&& blocks[1].prompt_row >= blocks[0].end_row;

			report.check("each marked command becomes a block", true, "");
			report.check("a block knows where its output began", rows_ordered, "");
			report.check("a block carries the exit status",
				blocks[0].exit_status == 0 && blocks[1].exit_status == 1
				&& blocks[0].finished && blocks[1].finished, "");
		}

		static void checkBlockNavigation(Report& report) {
			Screen screen(40, 4);
			for (int command = 0; command < 4; ++command) {
				feedToScreen(screen, "\x1b]633;A\a$ cmd\r\n\x1b]633;C\aout\r\n\x1b]633;D;0\a");
			}

			TerminalView view;
			view.followOutput(screen);

			const int first_back = view.neighbouringCommandRow(screen, true);
			report.check("there is a previous command to jump to", first_back >= 0,
				std::to_string(first_back));
			if (first_back < 0) return;

			view.scrollToRow(first_back, screen);
			const bool moved = view.topRow(screen) == first_back;

			const int further = view.neighbouringCommandRow(screen, true);
			const int forward = view.neighbouringCommandRow(screen, false);

			report.check("jumping puts that command at the top", moved,
				std::to_string(view.topRow(screen)));
			report.check("jumping again goes further back", further >= 0 && further < first_back,
				std::to_string(further));
			report.check("jumping forward comes back down", forward > first_back,
				std::to_string(forward));
		}

		static void checkBlockOutputSelection(Report& report) {
			Screen screen(40, 8);
			feedToScreen(screen,
				"\x1b]633;A\a$ echo two\r\n\x1b]633;C\aalpha\r\nbeta\r\n\x1b]633;D;0\a");

			TerminalView view;
			view.followOutput(screen);

			const std::vector<CommandBlock>& blocks = screen.commandBlocks();
			if (blocks.empty()) {
				report.check("the output of a command can be selected on its own", false, "");
				return;
			}

			view.selectBlockOutput(blocks.back(), screen);
			const std::string text = view.selectedText(screen);

			report.check("the output of a command can be selected on its own",
				text.find("alpha") != std::string::npos
				&& text.find("beta") != std::string::npos
				&& text.find("echo two") == std::string::npos, text);
		}

		static void checkWorkingDirectoryReport(Report& report) {
			Screen screen(40, 4);
			feedToScreen(screen, "\x1b]7;file:///C:/Users/me/My%20Code\a");

			report.check("OSC 7 reports the working directory",
				screen.workingDirectory() == "C:/Users/me/My Code", screen.workingDirectory());
		}

		static void checkFuzzyMatching(Report& report) {
			int loose = 0;
			int tight = 0;
			int missing = 0;

			const bool matches = fuzzyScore("scr", "src/screen.cpp", tight)
				&& fuzzyScore("scr", "some/other/character.txt", loose);
			const bool rejects = !fuzzyScore("zzz", "src/screen.cpp", missing);

			report.check("a query matches as a subsequence", matches, "");
			report.check("a query that is not there does not match", rejects, "");
			report.check("adjacent matches score above scattered ones", tight > loose,
				std::to_string(tight) + " vs " + std::to_string(loose));
		}

		static Picker pickerWith(const std::vector<std::string>& items) {
			Picker picker;
			picker.begin("fzf");
			for (const std::string& item : items) picker.addItem(item);
			picker.finish();
			return picker;
		}

		static void checkPickerFiltering(Report& report) {
			Picker picker = pickerWith({ "src/screen.cpp", "src/window.cpp", "README.md" });

			const bool starts_open = picker.active() && picker.matches().size() == 3;

			picker.typeCharacter(U'w');
			picker.typeCharacter(U'i');
			const bool narrowed = picker.matches().size() == 1
				&& picker.chosen() == "src/window.cpp";

			picker.backspace();
			picker.backspace();
			const bool restored = picker.matches().size() == 3;

			picker.typeCharacter(U'q');
			const bool empty = picker.matches().empty() && picker.chosen().empty();

			report.check("the overlay opens with every item", starts_open,
				std::to_string(picker.itemCount()));
			report.check("typing narrows the list", narrowed, "");
			report.check("deleting the query brings the list back", restored, "");
			report.check("a query matching nothing chooses nothing", empty, "");
		}

		static void checkPickerSelection(Report& report) {
			Picker picker = pickerWith({ "alpha", "beta", "gamma" });

			const std::string first = picker.chosen();
			picker.moveSelection(1);
			const std::string second = picker.chosen();

			picker.moveSelection(-5);
			const bool clamped_top = picker.selected() == 0;
			picker.moveSelection(99);
			const bool clamped_bottom = picker.selected() == 2;

			picker.cancel();
			const bool closed = !picker.active() && picker.chosen().empty();

			report.check("moving the selection changes the choice", first != second,
				first + " then " + second);
			report.check("the selection stops at both ends", clamped_top && clamped_bottom, "");
			report.check("cancelling closes the overlay", closed, "");
		}

		class PickRecorder : public PickHandler {
		public:
			void pickBegin(const std::string& prompt) override { began = prompt; }
			void pickItem(const std::string& text) override { items.push_back(text); }
			void pickEnd() override { ended = true; }
			void pickCancel() override { cancelled = true; }

			std::string began;
			std::vector<std::string> items;
			bool ended = false;
			bool cancelled = false;
		};

		// The request travels as OSC 1337, so the parts have to survive the
		// encoding: spaces, semicolons and anything else in a file name.
		static void checkPickRequestParsing(Report& report) {
			Screen screen(40, 6);
			PickRecorder recorder;
			screen.setPickHandler(&recorder);

			feedToScreen(screen,
				"\x1b]1337;pick;begin;fzf\a"
				"\x1b]1337;pick;item;src/screen.cpp\a"
				"\x1b]1337;pick;item;My%20Notes%3Bdraft.md\a"
				"\x1b]1337;pick;end\a");

			const bool named = recorder.began == "fzf";
			const bool both = recorder.items.size() == 2;
			const bool decoded = both && recorder.items[1] == "My Notes;draft.md";

			report.check("a pick request names its prompt", named, recorder.began);
			report.check("every item arrives", both, std::to_string(recorder.items.size()));
			report.check("an item survives encoding", decoded,
				both ? recorder.items[1] : std::string());
			report.check("the end of the list is announced", recorder.ended, "");
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

		// Stands in for the window: collects the list the shell sends and
		// answers with a choice, so the whole round trip runs headless.
		class AutoPicker : public PickHandler {
		public:
			explicit AutoPicker(Session& session) : session_(session) {}

			void pickBegin(const std::string&) override { picker_.begin("fzf"); }
			void pickItem(const std::string& text) override { picker_.addItem(text); }
			void pickCancel() override { picker_.cancel(); }

			void pickEnd() override {
				picker_.finish();
				saw_list = true;
				item_count = picker_.itemCount();

				for (char letter : wanted) picker_.typeCharacter(static_cast<char32_t>(letter));
				answered = picker_.chosen();

				const std::string reply = answered + "\r";
				session_.writeInput(reply.data(), reply.size());
				picker_.cancel();
			}

			std::string wanted;
			std::string answered;
			std::size_t item_count = 0;
			bool saw_list = false;

		private:
			Session& session_;
			Picker   picker_;
		};

		static void checkPickRoundTrip(Report& report, const std::wstring& command_line) {
			// Without this the shell draws its own picker and waits for keys
			// that never come; the child inherits what is set here.
			::SetEnvironmentVariableW(L"WBSHTERM_PICKER", L"1");

			Session session;
			std::string error;
			if (!session.start(command_line, 100, 16, error)) {
				report.check("the shell hands its picker to the terminal", false, error);
				return;
			}

			AutoPicker picker(session);
			picker.wanted = "tests";
			session.screen().setPickHandler(&picker);

			waitForText(session, "$", 8000);
			runCommand(session, "cd wbshterm");
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			session.drainOutput();

			runCommand(session, "fzf");

			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (std::chrono::steady_clock::now() < deadline) {
				session.drainOutput();
				if (session.screen().workingDirectory().find("tests") != std::string::npos) break;
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			}

			const std::string directory = session.screen().workingDirectory();

			report.check("the shell hands its picker to the terminal",
				picker.saw_list && picker.item_count > 0, std::to_string(picker.item_count));
			report.check("the terminal's choice reaches the shell",
				picker.answered == "tests", picker.answered);
			report.check("the shell acts on what was chosen",
				directory.find("wbshterm/tests") != std::string::npos
				|| directory.find("wbshterm\\tests") != std::string::npos, directory);

			session.stop();
		}

		// The banner promises Ctrl-D quits, so the window has to see the shell
		// go away when it is pressed on an empty line.
		static void checkCtrlDEndsTheSession(Report& report, const std::wstring& command_line) {
			Session session;
			std::string error;
			if (!session.start(command_line, 90, 12, error)) {
				report.check("Ctrl-D on an empty line ends the session", false, error);
				return;
			}

			waitForText(session, "$", 8000);

			const std::string eot(1, '\004');
			session.writeInput(eot.data(), eot.size());

			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
			while (session.childRunning() && std::chrono::steady_clock::now() < deadline) {
				session.drainOutput();
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			}

			const bool ended = !session.childRunning();
			report.check("Ctrl-D on an empty line ends the session", ended,
				session.screen().toText());
			session.stop();
		}

		// Marks are worth nothing unless the shell actually sends them.
		static void checkShellEmitsMarks(Report& report, Session& session) {
			runCommand(session, "echo mark-probe");
			waitForText(session, "mark-probe", 6000);

			const std::vector<CommandBlock>& blocks = session.screen().commandBlocks();
			bool finished_with_status = false;
			for (const CommandBlock& block : blocks) {
				if (block.finished && block.exit_status == 0) finished_with_status = true;
			}

			report.check("the shell marks its prompts", !blocks.empty(),
				std::to_string(blocks.size()));
			report.check("the shell reports how a command ended", finished_with_status, "");
			report.check("the shell reports its working directory",
				!session.screen().workingDirectory().empty(),
				session.screen().workingDirectory());
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
			checkShellEmitsMarks(report, session);
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

	static std::wstring reportDirectory(const std::wstring& report_path) {
		const std::size_t cut = report_path.find_last_of(L"/\\");
		return cut == std::wstring::npos ? std::wstring() : report_path.substr(0, cut + 1);
	}

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
		test::checkShellMarksMakeBlocks(report);
		test::checkBlockNavigation(report);
		test::checkBlockOutputSelection(report);
		test::checkWorkingDirectoryReport(report);
		test::checkFuzzyMatching(report);
		test::checkPickerFiltering(report);
		test::checkPickerSelection(report);
		test::checkPickRequestParsing(report);
		test::checkScrollbackKeepsLines(report);
		test::checkAltScreenKeepsHistory(report);
		test::checkViewScrolling(report);
		test::checkScrolledViewHoldsStill(report);
		test::checkSelectionText(report);
		test::checkSelectionShape(report);
		test::checkCharacterWidths(report);
		test::checkWideCharactersTakeTwoCells(report);
		test::checkResizeKeepsContent(report);
		test::checkThemesAreAvailable(report);
		test::checkConfigRoundTrip(report, reportDirectory(report_path));
		test::checkThemeOverridesIgnoreOrder(report, reportDirectory(report_path));
		test::checkMenuOffersCustomisation(report);
		const test::ThemeFolderFixture themes =
			test::makeThemeFolder(reportDirectory(report_path));
		test::checkThemesFolder(report, themes);
		test::checkFolderThemeReachesConfigAndMenu(report, themes);
		test::checkThemesFolderIsCreated(report, themes.themes);
		test::checkFetchPanelLayout(report);
		test::checkFetchPanelGrowsWithRows(report);
		test::checkStartupFetchSetting(report, reportDirectory(report_path));
		test::checkShippedThemesAreWrittenOut(report, themes.themes);
		test::checkThemeEditsAreNoticed(report, themes.config_path);
		test::removeThemeFolder(themes);
		test::checkMenuChoicesApply(report);
		test::checkMenuChoicesPersist(report, reportDirectory(report_path));
		test::checkCursorKeyEncoding(report);
		test::checkModifierParameters(report);
		test::checkFunctionAndEditingKeys(report);
		test::checkEraseKeys(report);
		test::checkTextKeysFallThrough(report);
		test::checkPasteEncoding(report);
		test::checkLiveSession(report, shell_command_line);
		test::checkPickRoundTrip(report, shell_command_line);
		test::checkCtrlDEndsTheSession(report, shell_command_line);

		test::writeReport(report_path, report.text());
		return report.passed();
	}

} /* namespace wbshterm */

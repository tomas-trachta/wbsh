/**
 * @file menu.cpp
 * @brief Menu construction, and the command-id to intent mapping.
 */

#include "menu.h"

#include "config.h"
#include "theme.h"

#include <string>
#include <vector>

namespace wbshterm {

	static const int kCommandCopy        = 100;
	static const int kCommandPaste       = 101;
	static const int kCommandBlink       = 102;
	static const int kCommandOpenConfig  = 103;
	static const int kCommandOpenThemes  = 104;
	static const int kCommandLastOutput  = 105;
	static const int kCommandStatusBar   = 106;

	static const int kCommandThemeBase   = 200;
	static const int kCommandCursorBase  = 300;
	static const int kCommandFontBase    = 400;
	static const int kCommandOpacityBase = 500;
	static const int kCommandPaddingBase = 600;
	static const int kCommandFamilyBase  = 1000;

	static const int kFontSizes[]    = { 9, 10, 11, 12, 13, 14, 16, 18, 20 };
	static const int kOpacityLevels[] = { 100, 97, 94, 90, 85, 80 };
	static const int kPaddingLevels[] = { 0, 4, 10, 16, 24 };

	static std::wstring widen(const std::string& text) {
		const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
			static_cast<int>(text.size()), nullptr, 0);
		std::wstring out(static_cast<std::size_t>(needed), L'\0');
		::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
			out.data(), needed);
		return out;
	}

	static std::string narrow(const std::wstring& text) {
		const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
			static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
		std::string out(static_cast<std::size_t>(needed), '\0');
		::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
			out.data(), needed, nullptr, nullptr);
		return out;
	}

	static bool sameFamily(const std::wstring& left, const std::wstring& right) {
		return ::CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
	}

	std::vector<std::wstring> menuFontFamilies(const Config& config,
			const std::vector<std::wstring>& installed) {
		std::vector<std::wstring> families = installed;

		for (const std::wstring& family : families) {
			if (sameFamily(family, config.font.family)) return families;
		}

		families.insert(families.begin(), config.font.family);
		return families;
	}

	static void appendItem(HMENU menu, int id, const std::wstring& label, bool checked) {
		::AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : 0), static_cast<UINT_PTR>(id),
			label.c_str());
	}

	static HMENU buildThemeMenu(const Config& config, const std::vector<std::string>& themes) {
		HMENU menu = ::CreatePopupMenu();

		for (std::size_t i = 0; i < themes.size(); ++i) {
			appendItem(menu, kCommandThemeBase + static_cast<int>(i), widen(themes[i]),
				themes[i] == config.theme_name);
		}

		return menu;
	}

	static HMENU buildCursorMenu(const Config& config) {
		HMENU menu = ::CreatePopupMenu();
		appendItem(menu, kCommandCursorBase + 0, L"Block",
			config.cursor.style == CursorStyle::Block);
		appendItem(menu, kCommandCursorBase + 1, L"Bar", config.cursor.style == CursorStyle::Bar);
		appendItem(menu, kCommandCursorBase + 2, L"Underline",
			config.cursor.style == CursorStyle::Underline);
		::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
		appendItem(menu, kCommandBlink, L"Blinking", config.cursor.blink);
		return menu;
	}

	// A long list of installed faces would run off the screen; Windows
	// breaks the column when told to, so it is broken every so often.
	static HMENU buildFamilyMenu(const Config& config, const std::vector<std::wstring>& fonts) {
		static const std::size_t kRowsPerColumn = 30;

		HMENU menu = ::CreatePopupMenu();
		for (std::size_t i = 0; i < fonts.size(); ++i) {
			const UINT column_break = (i > 0 && i % kRowsPerColumn == 0) ? MF_MENUBARBREAK : 0;
			const bool checked = sameFamily(fonts[i], config.font.family);
			::AppendMenuW(menu, MF_STRING | column_break | (checked ? MF_CHECKED : 0),
				static_cast<UINT_PTR>(kCommandFamilyBase + static_cast<int>(i)), fonts[i].c_str());
		}

		return menu;
	}

	static HMENU buildFontMenu(const Config& config) {
		HMENU menu = ::CreatePopupMenu();
		for (std::size_t i = 0; i < std::size(kFontSizes); ++i) {
			const int size = kFontSizes[i];
			appendItem(menu, kCommandFontBase + static_cast<int>(i),
				std::to_wstring(size) + L" pt",
				static_cast<int>(config.font.size + 0.5f) == size);
		}

		return menu;
	}

	static HMENU buildOpacityMenu(const Config& config) {
		HMENU menu = ::CreatePopupMenu();
		const int current = static_cast<int>(config.window.opacity * 100.0f + 0.5f);

		for (std::size_t i = 0; i < std::size(kOpacityLevels); ++i) {
			const int level = kOpacityLevels[i];
			appendItem(menu, kCommandOpacityBase + static_cast<int>(i),
				std::to_wstring(level) + L"%", current == level);
		}

		return menu;
	}

	static HMENU buildPaddingMenu(const Config& config) {
		HMENU menu = ::CreatePopupMenu();
		for (std::size_t i = 0; i < std::size(kPaddingLevels); ++i) {
			const int level = kPaddingLevels[i];
			appendItem(menu, kCommandPaddingBase + static_cast<int>(i),
				std::to_wstring(level) + L" px", config.window.padding == level);
		}

		return menu;
	}

	static void appendSubMenu(HMENU menu, HMENU child, const wchar_t* label) {
		::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(child), label);
	}

	HMENU buildTerminalMenu(const Config& config, const MenuLists& lists,
			bool has_selection, bool has_blocks) {
		HMENU menu = ::CreatePopupMenu();

		::AppendMenuW(menu, MF_STRING | (has_selection ? 0 : MF_GRAYED), kCommandCopy,
			L"Copy\tCtrl+Shift+C");
		::AppendMenuW(menu, MF_STRING, kCommandPaste, L"Paste\tCtrl+V");
		::AppendMenuW(menu, MF_STRING | (has_blocks ? 0 : MF_GRAYED), kCommandLastOutput,
			L"Copy last command output");
		::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

		appendSubMenu(menu, buildThemeMenu(config, lists.themes), L"Theme");
		appendSubMenu(menu, buildFamilyMenu(config, lists.fonts), L"Font");
		appendSubMenu(menu, buildFontMenu(config), L"Font size");
		appendSubMenu(menu, buildCursorMenu(config), L"Cursor");
		appendSubMenu(menu, buildOpacityMenu(config), L"Opacity");
		appendSubMenu(menu, buildPaddingMenu(config), L"Padding");
		appendItem(menu, kCommandStatusBar, L"Status bar", config.statusbar.enabled);

		::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
		::AppendMenuW(menu, MF_STRING, kCommandOpenConfig, L"Edit configuration file…");
		::AppendMenuW(menu, MF_STRING, kCommandOpenThemes, L"Open themes folder…");
		return menu;
	}

	static bool withinRange(int command_id, int base, std::size_t count) {
		return command_id >= base && command_id < base + static_cast<int>(count);
	}

	static bool listChoiceFor(int command_id, const MenuLists& lists, MenuChoice& out_choice) {
		if (withinRange(command_id, kCommandFamilyBase, lists.fonts.size())) {
			const std::size_t index = static_cast<std::size_t>(command_id - kCommandFamilyBase);
			out_choice.action = MenuAction::SetFontFamily;
			out_choice.family = lists.fonts[index];
			return true;
		}

		if (withinRange(command_id, kCommandThemeBase, lists.themes.size())) {
			const std::size_t index = static_cast<std::size_t>(command_id - kCommandThemeBase);
			out_choice.action = MenuAction::SetTheme;
			out_choice.text   = lists.themes[index];
			return true;
		}

		return false;
	}

	static bool plainChoiceFor(int command_id, MenuChoice& out_choice) {
		switch (command_id) {
		case kCommandCopy:       out_choice.action = MenuAction::Copy; return true;
		case kCommandPaste:      out_choice.action = MenuAction::Paste; return true;
		case kCommandBlink:      out_choice.action = MenuAction::ToggleBlink; return true;
		case kCommandOpenConfig: out_choice.action = MenuAction::OpenConfigFile; return true;
		case kCommandOpenThemes: out_choice.action = MenuAction::OpenThemesFolder; return true;
		case kCommandLastOutput: out_choice.action = MenuAction::CopyLastOutput; return true;
		case kCommandStatusBar:  out_choice.action = MenuAction::ToggleStatusBar; return true;
		default:                 return false;
		}
	}

	MenuChoice menuChoiceFor(int command_id, const MenuLists& lists) {
		MenuChoice choice;
		if (plainChoiceFor(command_id, choice)) return choice;
		if (listChoiceFor(command_id, lists, choice)) return choice;

		if (withinRange(command_id, kCommandCursorBase, 3)) {
			const int index = command_id - kCommandCursorBase;
			choice.action = MenuAction::SetCursorStyle;
			choice.style  = index == 1 ? CursorStyle::Bar
				: (index == 2 ? CursorStyle::Underline : CursorStyle::Block);
			return choice;
		}

		if (withinRange(command_id, kCommandFontBase, std::size(kFontSizes))) {
			choice.action = MenuAction::SetFontSize;
			choice.number = kFontSizes[command_id - kCommandFontBase];
			return choice;
		}

		if (withinRange(command_id, kCommandOpacityBase, std::size(kOpacityLevels))) {
			choice.action = MenuAction::SetOpacity;
			choice.number = kOpacityLevels[command_id - kCommandOpacityBase];
			choice.amount = static_cast<float>(choice.number) / 100.0f;
			return choice;
		}

		if (withinRange(command_id, kCommandPaddingBase, std::size(kPaddingLevels))) {
			choice.action = MenuAction::SetPadding;
			choice.number = kPaddingLevels[command_id - kCommandPaddingBase];
			return choice;
		}

		return choice;
	}

	bool applyMenuChoice(const MenuChoice& choice, const std::wstring& themes_directory,
			Config& config) {
		switch (choice.action) {
		case MenuAction::SetTheme:
			if (config.theme_name == choice.text) return false;
			config.theme_name = choice.text;
			findTheme(themes_directory, choice.text, config.palette);
			return true;
		case MenuAction::SetCursorStyle:
			if (config.cursor.style == choice.style) return false;
			config.cursor.style = choice.style;
			return true;
		case MenuAction::ToggleBlink:
			config.cursor.blink = !config.cursor.blink;
			return true;
		case MenuAction::ToggleStatusBar:
			config.statusbar.enabled = !config.statusbar.enabled;
			return true;
		case MenuAction::SetFontSize:
			config.font.size = static_cast<float>(choice.number);
			return true;
		case MenuAction::SetFontFamily:
			if (choice.family.empty()) return false;
			if (sameFamily(config.font.family, choice.family)) return false;
			config.font.family = choice.family;
			return true;
		case MenuAction::SetOpacity:
			config.window.opacity = choice.amount;
			return true;
		case MenuAction::SetPadding:
			config.window.padding = choice.number;
			return true;
		default:
			return false;
		}
	}

	static std::string cursorStyleName(CursorStyle style) {
		switch (style) {
		case CursorStyle::Bar:       return "bar";
		case CursorStyle::Underline: return "underline";
		case CursorStyle::Block:
		default:                     return "block";
		}
	}

	bool settingForChoice(const MenuChoice& choice, const Config& config,
			std::string& out_section, std::string& out_key, std::string& out_value) {
		switch (choice.action) {
		case MenuAction::SetTheme:
			out_section = "theme";
			out_key     = "name";
			out_value   = config.theme_name;
			return true;
		case MenuAction::SetCursorStyle:
			out_section = "cursor";
			out_key     = "style";
			out_value   = cursorStyleName(config.cursor.style);
			return true;
		case MenuAction::ToggleBlink:
			out_section = "cursor";
			out_key     = "blink";
			out_value   = config.cursor.blink ? "true" : "false";
			return true;
		case MenuAction::ToggleStatusBar:
			out_section = "statusbar";
			out_key     = "enabled";
			out_value   = config.statusbar.enabled ? "true" : "false";
			return true;
		case MenuAction::SetFontSize:
			out_section = "font";
			out_key     = "size";
			out_value   = std::to_string(choice.number);
			return true;
		case MenuAction::SetFontFamily:
			out_section = "font";
			out_key     = "family";
			out_value   = narrow(config.font.family);
			return true;
		case MenuAction::SetOpacity:
			out_section = "window";
			out_key     = "opacity";
			out_value   = std::to_string(choice.number / 100) + "."
				+ std::to_string((choice.number % 100) / 10) + std::to_string(choice.number % 10);
			return true;
		case MenuAction::SetPadding:
			out_section = "window";
			out_key     = "padding";
			out_value   = std::to_string(choice.number);
			return true;
		default:
			return false;
		}
	}

} /* namespace wbshterm */

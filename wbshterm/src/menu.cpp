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

	static const int kCommandThemeBase   = 200;
	static const int kCommandCursorBase  = 300;
	static const int kCommandFontBase    = 400;
	static const int kCommandOpacityBase = 500;
	static const int kCommandPaddingBase = 600;

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

	HMENU buildTerminalMenu(const Config& config, const std::vector<std::string>& themes,
			bool has_selection) {
		HMENU menu = ::CreatePopupMenu();

		::AppendMenuW(menu, MF_STRING | (has_selection ? 0 : MF_GRAYED), kCommandCopy,
			L"Copy\tCtrl+Shift+C");
		::AppendMenuW(menu, MF_STRING, kCommandPaste, L"Paste\tCtrl+V");
		::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

		appendSubMenu(menu, buildThemeMenu(config, themes), L"Theme");
		appendSubMenu(menu, buildFontMenu(config), L"Font size");
		appendSubMenu(menu, buildCursorMenu(config), L"Cursor");
		appendSubMenu(menu, buildOpacityMenu(config), L"Opacity");
		appendSubMenu(menu, buildPaddingMenu(config), L"Padding");

		::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
		::AppendMenuW(menu, MF_STRING, kCommandOpenConfig, L"Edit configuration file…");
		::AppendMenuW(menu, MF_STRING, kCommandOpenThemes, L"Open themes folder…");
		return menu;
	}

	static bool withinRange(int command_id, int base, std::size_t count) {
		return command_id >= base && command_id < base + static_cast<int>(count);
	}

	MenuChoice menuChoiceFor(int command_id, const std::vector<std::string>& themes) {
		MenuChoice choice;

		if (command_id == kCommandCopy)       { choice.action = MenuAction::Copy; return choice; }
		if (command_id == kCommandPaste)      { choice.action = MenuAction::Paste; return choice; }
		if (command_id == kCommandBlink) {
			choice.action = MenuAction::ToggleBlink;
			return choice;
		}

		if (command_id == kCommandOpenConfig) {
			choice.action = MenuAction::OpenConfigFile;
			return choice;
		}

		if (command_id == kCommandOpenThemes) {
			choice.action = MenuAction::OpenThemesFolder;
			return choice;
		}

		if (withinRange(command_id, kCommandThemeBase, themes.size())) {
			choice.action = MenuAction::SetTheme;
			choice.text   = themes[static_cast<std::size_t>(command_id - kCommandThemeBase)];
			return choice;
		}

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
		case MenuAction::SetFontSize:
			config.font.size = static_cast<float>(choice.number);
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
		case MenuAction::SetFontSize:
			out_section = "font";
			out_key     = "size";
			out_value   = std::to_string(choice.number);
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

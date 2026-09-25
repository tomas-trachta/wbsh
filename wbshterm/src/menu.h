#pragma once

/**
 * @file menu.h
 * @brief The right-click menu: what it offers and what a click meant.
 */

#include "config.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif /* WIN32_LEAN_AND_MEAN */
#include <windows.h>

#include <string>
#include <vector>

namespace wbshterm {

	enum class MenuAction {
		None,
		Copy,
		Paste,
		SetTheme,
		SetCursorStyle,
		ToggleBlink,
		SetFontSize,
		SetFontFamily,
		SetOpacity,
		SetPadding,
		OpenConfigFile,
		OpenThemesFolder,
		CopyLastOutput,
	};

	/**
	 * @brief What a chosen command means, with its value where it has one.
	 *
	 * Keeping this separate from the window is what makes the menu
	 * testable: the mapping from command id to intent is ordinary code.
	 */
	struct MenuChoice {
		MenuAction   action = MenuAction::None;
		int          number = 0;
		float        amount = 0.0f;
		std::string  text;
		std::wstring family;
		CursorStyle  style  = CursorStyle::Block;
	};

	/** What the menu can offer beyond its fixed entries. */
	struct MenuLists {
		std::vector<std::string>  themes;
		std::vector<std::wstring> fonts;
	};

	/**
	 * @brief The font families the menu lists: the installed ones plus the
	 *        configured one, so the tick always has somewhere to go.
	 */
	std::vector<std::wstring> menuFontFamilies(const Config& config,
		const std::vector<std::wstring>& installed);

	/**
	 * @brief Builds the popup, ticking whatever the config currently says.
	 *
	 * The caller owns the returned menu and must DestroyMenu it.
	 */
	HMENU buildTerminalMenu(const Config& config, const MenuLists& lists,
		bool has_selection, bool has_blocks);

	/** Translates a command id from TrackPopupMenu into an intent. */
	MenuChoice menuChoiceFor(int command_id, const MenuLists& lists);

	/** Applies a choice to @p config; false when nothing about it changed. */
	bool applyMenuChoice(const MenuChoice& choice, const std::wstring& themes_directory,
		Config& config);

	/** The config file section and key a choice writes to, for persisting. */
	bool settingForChoice(const MenuChoice& choice, const Config& config,
		std::string& out_section, std::string& out_key, std::string& out_value);

} /* namespace wbshterm */

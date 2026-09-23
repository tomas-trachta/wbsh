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
		SetOpacity,
		SetPadding,
		OpenConfigFile,
		OpenThemesFolder,
	};

	/**
	 * @brief What a chosen command means, with its value where it has one.
	 *
	 * Keeping this separate from the window is what makes the menu
	 * testable: the mapping from command id to intent is ordinary code.
	 */
	struct MenuChoice {
		MenuAction  action = MenuAction::None;
		int         number = 0;
		float       amount = 0.0f;
		std::string text;
		CursorStyle style  = CursorStyle::Block;
	};

	/**
	 * @brief Builds the popup, ticking whatever the config currently says.
	 *
	 * The caller owns the returned menu and must DestroyMenu it.
	 */
	HMENU buildTerminalMenu(const Config& config, const std::vector<std::string>& themes,
		bool has_selection);

	/** Translates a command id from TrackPopupMenu into an intent. */
	MenuChoice menuChoiceFor(int command_id, const std::vector<std::string>& themes);

	/** Applies a choice to @p config; false when nothing about it changed. */
	bool applyMenuChoice(const MenuChoice& choice, const std::wstring& themes_directory,
		Config& config);

	/** The config file section and key a choice writes to, for persisting. */
	bool settingForChoice(const MenuChoice& choice, const Config& config,
		std::string& out_section, std::string& out_key, std::string& out_value);

} /* namespace wbshterm */

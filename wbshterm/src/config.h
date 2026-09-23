#pragma once

/**
 * @file config.h
 * @brief The settings file: fonts, colours, cursor, window.
 */

#include "theme.h"

#include <string>
#include <vector>

namespace wbshterm {

	enum class CursorStyle {
		Block,
		Bar,
		Underline,
	};

	struct FontSettings {
		std::wstring family = L"Cascadia Mono";
		float        size   = 11.0f;
		float        line_height = 1.0f;

		/** Families tried for glyphs the main font lacks: emoji, CJK, icons. */
		std::vector<std::wstring> fallback = {
			L"Segoe UI Emoji", L"Microsoft YaHei UI", L"Segoe UI Symbol",
		};
	};

	struct WindowSettings {
		int   padding = 10;
		float opacity = 1.0f;
		int   columns = 100;
		int   rows    = 30;
	};

	struct CursorSettings {
		CursorStyle style = CursorStyle::Block;
		bool        blink = true;
	};

	struct Config {
		FontSettings   font;
		WindowSettings window;
		CursorSettings cursor;
		Palette        palette;
		std::string    theme_name      = "catppuccin-mocha";
		int            scrollback_lines = 10000;
		bool           startup_fetch    = true;
	};

	/** Sets one palette colour by its config key; false if the key is not one. */
	bool applyPaletteKey(const std::string& key, const std::string& value, Palette& palette);

	/** A theme file's text: the twenty colours, with the name in a comment. */
	std::string paletteToText(const std::string& name, const Palette& palette);

	/**
	 * @brief Creates the themes folder and fills in whatever is missing.
	 *
	 * Every built-in theme is written out as a file, so the folder shows
	 * what a theme looks like and any of them can be edited or copied. A
	 * file already there is left alone: these are the user's now.
	 */
	bool ensureThemesDirectory(const std::wstring& directory);

	/** The themes folder beside a config file, where a user's own palettes live. */
	std::wstring themesDirectory(const std::wstring& config_path);

	/**
	 * @brief Every theme that can be chosen: the built-ins plus @p directory.
	 *
	 * A file named nord.conf shadows the built-in of that name, so a shipped
	 * palette can be adjusted without losing its place in the menu.
	 */
	std::vector<std::string> availableThemeNames(const std::wstring& directory);

	/** Resolves a theme name against the folder first, then the built-ins. */
	bool findTheme(const std::wstring& directory, const std::string& name, Palette& out_palette);

	/** %APPDATA%\\wbshterm\\wbshterm.conf, or empty when APPDATA is unset. */
	std::wstring defaultConfigPath();

	/**
	 * @brief Reads @p path over the defaults already in @p config.
	 *
	 * Unknown keys and unreadable values are skipped rather than fatal:
	 * a typo in one line must not cost the user their terminal. Returns
	 * false only when the file cannot be read at all.
	 */
	bool loadConfig(const std::wstring& path, Config& config, std::string& out_error);

	/** Writes the documented default file, creating directories as needed. */
	bool writeDefaultConfig(const std::wstring& path);

	/**
	 * @brief Sets one key in the file, leaving every other line alone.
	 *
	 * Menu choices have to survive a restart, and the file is the user's:
	 * comments, ordering and unrelated settings all stay as written. A
	 * missing section or key is appended rather than reported.
	 */
	bool updateConfigValue(const std::wstring& path, const std::string& section,
		const std::string& key, const std::string& value);

	/** Last write time, for noticing edits without re-reading the file. */
	unsigned long long configStamp(const std::wstring& path);

	/**
	 * @brief One stamp covering the config and the theme file it names.
	 *
	 * A theme is its own file now, so watching only the config would leave
	 * an edit to a palette sitting there until something else changed.
	 */
	unsigned long long settingsStamp(const std::wstring& config_path,
		const std::string& theme_name);

} /* namespace wbshterm */

/**
 * @file config.cpp
 * @brief A small key = value parser, and the default file it writes.
 */

#include "config.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif /* WIN32_LEAN_AND_MEAN */
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace wbshterm {

	static const char* kDefaultFile =
		"# wbshterm configuration.\n"
		"#\n"
		"# Edited while the terminal is open, the file is picked up within a\n"
		"# second; nothing needs restarting. Lines starting with # are notes.\n"
		"\n"
		"[font]\n"
		"family = Cascadia Mono\n"
		"size = 11\n"
		"# Row height as a multiple of the font's own. 1.15 breathes a little.\n"
		"line_height = 1.0\n"
		"# Families used for glyphs this font lacks: emoji, CJK, icons.\n"
		"fallback = Segoe UI Emoji, Microsoft YaHei UI, Segoe UI Symbol\n"
		"\n"
		"[window]\n"
		"# Space between the text and the window edge, in pixels.\n"
		"padding = 10\n"
		"# 1.0 is opaque; 0.9 lets the desktop through.\n"
		"opacity = 1.0\n"
		"columns = 100\n"
		"rows = 30\n"
		"\n"
		"[cursor]\n"
		"# block, bar or underline\n"
		"style = block\n"
		"blink = true\n"
		"\n"
		"[startup]\n"
		"# The panel of system information shown when a session opens.\n"
		"fetch = true\n"
		"\n"
		"[scrollback]\n"
		"lines = 10000\n"
		"\n"
		"[theme]\n"
		"# catppuccin-mocha, tokyo-night, dracula, nord, gruvbox-dark,\n"
		"# one-dark, solarized-dark, solarized-light, vscode-dark\n"
		"name = catppuccin-mocha\n"
		"\n"
		"# Anything set below overrides the named theme.\n"
		"# background = #1E1E2E\n"
		"# foreground = #CDD6F4\n"
		"# cursor = #F5E0DC\n"
		"# selection = #45475A\n"
		"# ansi0 = #45475A\n"
		"# ansi1 = #F38BA8\n"
		"# ... through ansi15\n";

	static const char* kThemeExample =
		"# A theme of your own.\n"
		"#\n"
		"# Copy this file to <name>.conf in this folder and it becomes a theme\n"
		"# called <name>: choose it from the right-click menu, or write\n"
		"# name = <name> under [theme] in wbshterm.conf. A file named after a\n"
		"# built-in theme replaces it.\n"
		"#\n"
		"# Every key is optional; anything left out keeps its default.\n"
		"\n"
		"background = #11121B\n"
		"foreground = #C8D0E0\n"
		"cursor     = #F2CDCD\n"
		"selection  = #3A3F58\n"
		"\n"
		"# The eight normal colours, then the eight bright ones.\n"
		"ansi0 = #11121B\n"
		"ansi1 = #F07178\n"
		"ansi2 = #C3E88D\n"
		"ansi3 = #FFCB6B\n"
		"ansi4 = #82AAFF\n"
		"ansi5 = #C792EA\n"
		"ansi6 = #89DDFF\n"
		"ansi7 = #C8D0E0\n"
		"ansi8 = #3A3F58\n"
		"ansi9 = #FF8B92\n"
		"ansi10 = #DDFFA7\n"
		"ansi11 = #FFE585\n"
		"ansi12 = #9CC4FF\n"
		"ansi13 = #E1ACFF\n"
		"ansi14 = #A3F7FF\n"
		"ansi15 = #FFFFFF\n";

	struct Setting {
		std::string section;
		std::string key;
		std::string value;
	};

	static std::string trimmed(const std::string& text) {
		std::size_t first = text.find_first_not_of(" \t\r\n");
		if (first == std::string::npos) return std::string();

		std::size_t last = text.find_last_not_of(" \t\r\n");
		return text.substr(first, last - first + 1);
	}

	static bool parseColor(const std::string& text, std::uint32_t& out_color) {
		std::string digits = trimmed(text);
		if (!digits.empty() && digits[0] == '#') digits.erase(0, 1);
		else if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
			digits.erase(0, 2);
		}

		if (digits.size() != 6) return false;

		char* end = nullptr;
		const unsigned long value = std::strtoul(digits.c_str(), &end, 16);
		if (end == nullptr || *end != '\0') return false;

		out_color = static_cast<std::uint32_t>(value) & 0xFFFFFF;
		return true;
	}

	static bool parseBool(const std::string& text) {
		const std::string value = trimmed(text);
		return value == "true" || value == "yes" || value == "1" || value == "on";
	}

	static float parseFloat(const std::string& text, float fallback) {
		char* end = nullptr;
		const double value = std::strtod(text.c_str(), &end);
		return (end == text.c_str()) ? fallback : static_cast<float>(value);
	}

	static int parseInt(const std::string& text, int fallback) {
		char* end = nullptr;
		const long value = std::strtol(text.c_str(), &end, 10);
		return (end == text.c_str()) ? fallback : static_cast<int>(value);
	}

	static std::wstring widen(const std::string& text) {
		if (text.empty()) return std::wstring();

		const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
			static_cast<int>(text.size()), nullptr, 0);
		std::wstring out(static_cast<std::size_t>(needed), L'\0');
		::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
			out.data(), needed);
		return out;
	}

	static bool readFileText(const std::wstring& path, std::string& out_text) {
		FILE* file = nullptr;
		if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr) return false;

		char buffer[4096];
		for (;;) {
			const std::size_t got = std::fread(buffer, 1, sizeof(buffer), file);
			if (got == 0) break;
			out_text.append(buffer, got);
		}

		std::fclose(file);
		return true;
	}

	static std::vector<Setting> parseSettings(const std::string& text) {
		std::vector<Setting> settings;
		std::string section;
		std::size_t at = 0;

		while (at <= text.size()) {
			const std::size_t end = text.find('\n', at);
			const std::string line = trimmed(text.substr(at,
				(end == std::string::npos ? text.size() : end) - at));
			at = (end == std::string::npos) ? text.size() + 1 : end + 1;

			if (line.empty() || line[0] == '#' || line[0] == ';') continue;

			if (line.front() == '[' && line.back() == ']') {
				section = trimmed(line.substr(1, line.size() - 2));
				continue;
			}

			const std::size_t equals = line.find('=');
			if (equals == std::string::npos) continue;

			settings.push_back({ section, trimmed(line.substr(0, equals)),
				trimmed(line.substr(equals + 1)) });
		}

		return settings;
	}

	static void applyFontSetting(const Setting& setting, Config& config) {
		if (setting.key == "family") config.font.family = widen(setting.value);
		if (setting.key == "size") config.font.size = parseFloat(setting.value, config.font.size);
		if (setting.key == "line_height") {
			config.font.line_height = parseFloat(setting.value, config.font.line_height);
		}

		if (setting.key == "fallback") {
			config.font.fallback.clear();
			std::size_t at = 0;
			while (at < setting.value.size()) {
				const std::size_t comma = setting.value.find(',', at);
				const std::string one = trimmed(setting.value.substr(at,
					(comma == std::string::npos ? setting.value.size() : comma) - at));
				if (!one.empty()) config.font.fallback.push_back(widen(one));
				if (comma == std::string::npos) break;
				at = comma + 1;
			}
		}
	}

	static void applyWindowSetting(const Setting& setting, Config& config) {
		if (setting.key == "padding") {
			config.window.padding = parseInt(setting.value, config.window.padding);
		}

		if (setting.key == "opacity") {
			config.window.opacity = parseFloat(setting.value, config.window.opacity);
		}

		if (setting.key == "columns") {
			config.window.columns = parseInt(setting.value, config.window.columns);
		}

		if (setting.key == "rows") config.window.rows = parseInt(setting.value, config.window.rows);
	}

	static void applyCursorSetting(const Setting& setting, Config& config) {
		if (setting.key == "blink") config.cursor.blink = parseBool(setting.value);
		if (setting.key != "style") return;

		if (setting.value == "bar") config.cursor.style = CursorStyle::Bar;
		else if (setting.value == "underline") config.cursor.style = CursorStyle::Underline;
		else config.cursor.style = CursorStyle::Block;
	}

	bool applyPaletteKey(const std::string& key, const std::string& value, Palette& palette) {
		std::uint32_t color = 0;
		if (!parseColor(value, color)) return false;

		if (key == "background") { palette.background = color; return true; }
		if (key == "foreground") { palette.foreground = color; return true; }
		if (key == "cursor")     { palette.cursor = color; return true; }
		if (key == "selection")  { palette.selection = color; return true; }

		if (key.rfind("ansi", 0) != 0) return false;

		const int index = parseInt(key.substr(4), -1);
		if (index < 0 || index > 15) return false;

		palette.ansi[static_cast<std::size_t>(index)] = color;
		return true;
	}

	static void applyThemeSetting(const Setting& setting, Config& config) {
		if (setting.key == "name") return;
		applyPaletteKey(setting.key, setting.value, config.palette);
	}

	std::wstring themesDirectory(const std::wstring& config_path) {
		const std::size_t cut = config_path.find_last_of(L"/\\");
		if (cut == std::wstring::npos) return std::wstring();

		return config_path.substr(0, cut) + L"\\themes";
	}

	static std::string themeNameFromFile(const std::wstring& file_name) {
		const std::size_t dot = file_name.find_last_of(L'.');
		const std::wstring stem = dot == std::wstring::npos ? file_name : file_name.substr(0, dot);

		std::string name;
		for (wchar_t letter : stem) name.push_back(static_cast<char>(letter & 0x7F));
		return name;
	}

	static std::vector<std::wstring> themeFiles(const std::wstring& directory) {
		std::vector<std::wstring> files;
		if (directory.empty()) return files;

		WIN32_FIND_DATAW found{};
		const HANDLE search = ::FindFirstFileW((directory + L"\\*.conf").c_str(), &found);
		if (search == INVALID_HANDLE_VALUE) return files;

		do {
			if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
				files.push_back(found.cFileName);
			}
		} while (::FindNextFileW(search, &found));

		::FindClose(search);
		return files;
	}

	// A theme file is the same key = value text as the config's own [theme]
	// section, so one can be pasted into the other; any section header in it
	// is ignored rather than being a second place to get things wrong.
	static bool readPaletteFile(const std::wstring& path, Palette& out_palette) {
		std::string text;
		if (!readFileText(path, text)) return false;

		for (const Setting& setting : parseSettings(text)) {
			applyPaletteKey(setting.key, setting.value, out_palette);
		}

		return true;
	}

	std::vector<std::string> availableThemeNames(const std::wstring& directory) {
		std::vector<std::string> names = builtInThemeNames();

		for (const std::wstring& file : themeFiles(directory)) {
			const std::string name = themeNameFromFile(file);
			if (name.empty()) continue;
			if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
		}

		return names;
	}

	bool findTheme(const std::wstring& directory, const std::string& name, Palette& out_palette) {
		for (const std::wstring& file : themeFiles(directory)) {
			if (themeNameFromFile(file) != name) continue;

			Palette palette;
			if (!readPaletteFile(directory + L"\\" + file, palette)) break;

			out_palette = palette;
			return true;
		}

		return findBuiltInTheme(name, out_palette);
	}

	static void applySetting(const Setting& setting, Config& config) {
		if (setting.section == "font")   applyFontSetting(setting, config);
		if (setting.section == "window") applyWindowSetting(setting, config);
		if (setting.section == "cursor") applyCursorSetting(setting, config);
		if (setting.section == "theme")  applyThemeSetting(setting, config);
		if (setting.section == "startup" && setting.key == "fetch") {
			config.startup_fetch = parseBool(setting.value);
		}

		if (setting.section == "scrollback" && setting.key == "lines") {
			config.scrollback_lines = parseInt(setting.value, config.scrollback_lines);
		}
	}

	std::wstring defaultConfigPath() {
		wchar_t* roaming = nullptr;
		std::size_t length = 0;
		if (_wdupenv_s(&roaming, &length, L"APPDATA") != 0 || roaming == nullptr) {
			return std::wstring();
		}

		std::wstring path(roaming);
		std::free(roaming);
		return path + L"\\wbshterm\\wbshterm.conf";
	}

	bool loadConfig(const std::wstring& path, Config& config, std::string& out_error) {
		std::string text;
		if (!readFileText(path, text)) {
			out_error = "no configuration file";
			return false;
		}

		// The theme is the starting point every other colour is written over,
		// so it is applied before them whatever order the file lists them in.
		const std::vector<Setting> settings = parseSettings(text);
		for (const Setting& setting : settings) {
			if (setting.section == "theme" && setting.key == "name") {
				config.theme_name = setting.value;
			}
		}

		findTheme(themesDirectory(path), config.theme_name, config.palette);

		for (const Setting& setting : settings) {
			if (setting.section == "theme" && setting.key == "name") continue;
			applySetting(setting, config);
		}

		return true;
	}

	static void writeThemeExample(const std::wstring& directory);
	static bool writeFileText(const std::wstring& path, const std::string& text);
	static std::wstring widenAscii(const std::string& text);

	static std::string colorLine(const char* key, std::uint32_t color) {
		char text[64];
		std::snprintf(text, sizeof(text), "%-10s = #%06X\n", key, color & 0xFFFFFF);
		return text;
	}

	std::string paletteToText(const std::string& name, const Palette& palette) {
		std::string text = "# " + name + "\n"
			"#\n"
			"# Edit this file and the theme changes; delete it and the built-in\n"
			"# one comes back. Copy it to <name>.conf for a theme of your own.\n"
			"\n";

		text += colorLine("background", palette.background);
		text += colorLine("foreground", palette.foreground);
		text += colorLine("cursor", palette.cursor);
		text += colorLine("selection", palette.selection);
		text += "\n# The eight normal colours, then the eight bright ones.\n";

		for (int index = 0; index < 16; ++index) {
			const std::string key = "ansi" + std::to_string(index);
			text += colorLine(key.c_str(), palette.ansi[static_cast<std::size_t>(index)]);
			if (index == 7) text += "\n";
		}

		return text;
	}

	static std::wstring widenAscii(const std::string& text) {
		std::wstring out;
		for (char letter : text) out.push_back(static_cast<wchar_t>(letter));
		return out;
	}

	static void writeMissingThemeFiles(const std::wstring& directory) {
		for (const std::string& name : builtInThemeNames()) {
			const std::wstring path = directory + L"\\" + widenAscii(name) + L".conf";
			if (::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) continue;

			Palette palette;
			if (!findBuiltInTheme(name, palette)) continue;
			writeFileText(path, paletteToText(name, palette));
		}
	}

	bool ensureThemesDirectory(const std::wstring& directory) {
		if (directory.empty()) return false;

		::CreateDirectoryW(directory.c_str(), nullptr);
		if (::GetFileAttributesW(directory.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

		const std::wstring example = directory + L"\\example.conf.txt";
		if (::GetFileAttributesW(example.c_str()) == INVALID_FILE_ATTRIBUTES) {
			writeThemeExample(directory);
		}

		writeMissingThemeFiles(directory);
		return true;
	}

	static void writeThemeExample(const std::wstring& directory) {
		if (directory.empty()) return;
		::CreateDirectoryW(directory.c_str(), nullptr);

		const std::wstring path = directory + L"\\example.conf.txt";
		FILE* file = nullptr;
		if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) return;

		std::fwrite(kThemeExample, 1, std::strlen(kThemeExample), file);
		std::fclose(file);
	}

	bool writeDefaultConfig(const std::wstring& path) {
		const std::size_t cut = path.find_last_of(L'\\');
		if (cut != std::wstring::npos) ::CreateDirectoryW(path.substr(0, cut).c_str(), nullptr);

		FILE* file = nullptr;
		if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) return false;

		std::fwrite(kDefaultFile, 1, std::strlen(kDefaultFile), file);
		std::fclose(file);

		ensureThemesDirectory(themesDirectory(path));
		return true;
	}

	static bool writeFileText(const std::wstring& path, const std::string& text) {
		FILE* file = nullptr;
		if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) return false;

		std::fwrite(text.data(), 1, text.size(), file);
		std::fclose(file);
		return true;
	}

	static bool isSectionHeader(const std::string& line, const std::string& section) {
		const std::string trimmed_line = trimmed(line);
		if (trimmed_line.size() < 2 || trimmed_line.front() != '[') return false;
		return trimmed(trimmed_line.substr(1, trimmed_line.size() - 2)) == section;
	}

	static bool lineSetsKey(const std::string& line, const std::string& key) {
		const std::string trimmed_line = trimmed(line);
		if (trimmed_line.empty() || trimmed_line[0] == '#' || trimmed_line[0] == ';') return false;

		const std::size_t equals = trimmed_line.find('=');
		if (equals == std::string::npos) return false;
		return trimmed(trimmed_line.substr(0, equals)) == key;
	}

	static std::vector<std::string> splitLines(const std::string& text) {
		std::vector<std::string> lines;
		std::size_t at = 0;

		while (at <= text.size()) {
			const std::size_t end = text.find('\n', at);
			if (end == std::string::npos) {
				if (at < text.size()) lines.push_back(text.substr(at));
				break;
			}

			lines.push_back(text.substr(at, end - at));
			at = end + 1;
		}

		return lines;
	}

	static std::string joinLines(const std::vector<std::string>& lines) {
		std::string text;
		for (const std::string& line : lines) {
			text += line;
			text += "\n";
		}

		return text;
	}

	bool updateConfigValue(const std::wstring& path, const std::string& section,
			const std::string& key, const std::string& value) {
		std::string text;
		readFileText(path, text);

		std::vector<std::string> lines = splitLines(text);
		const std::string setting = key + " = " + value;

		std::size_t section_at = lines.size();
		std::size_t section_end = lines.size();

		for (std::size_t i = 0; i < lines.size(); ++i) {
			if (isSectionHeader(lines[i], section)) {
				section_at = i;
				continue;
			}

			const bool in_section = section_at < lines.size() && i > section_at;
			if (!in_section) continue;

			if (!trimmed(lines[i]).empty() && trimmed(lines[i]).front() == '[') {
				section_end = i;
				break;
			}

			if (lineSetsKey(lines[i], key)) {
				lines[i] = setting;
				return writeFileText(path, joinLines(lines));
			}
		}

		if (section_at == lines.size()) {
			lines.push_back("");
			lines.push_back("[" + section + "]");
			lines.push_back(setting);
			return writeFileText(path, joinLines(lines));
		}

		const std::size_t insert_at = std::min(section_end, lines.size());
		lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insert_at), setting);
		return writeFileText(path, joinLines(lines));
	}

	unsigned long long settingsStamp(const std::wstring& config_path,
			const std::string& theme_name) {
		const std::wstring themes = themesDirectory(config_path);
		const std::wstring theme_file = themes.empty()
			? std::wstring()
			: themes + L"\\" + widenAscii(theme_name) + L".conf";

		return configStamp(config_path) ^ (configStamp(theme_file) * 1000003ull);
	}

	unsigned long long configStamp(const std::wstring& path) {
		WIN32_FILE_ATTRIBUTE_DATA attributes{};
		if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) return 0;

		ULARGE_INTEGER stamp;
		stamp.LowPart  = attributes.ftLastWriteTime.dwLowDateTime;
		stamp.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
		return stamp.QuadPart;
	}

} /* namespace wbshterm */

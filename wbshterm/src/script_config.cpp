/**
 * @file script_config.cpp
 * @brief The bridge between C++ structs and the tables init.lua sees.
 */

#include "script_internal.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif /* WIN32_LEAN_AND_MEAN */
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

namespace wbshterm {

	namespace script_detail {

		std::wstring widenUtf8(const std::string& text) {
			if (text.empty()) return std::wstring();

			const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
				static_cast<int>(text.size()), nullptr, 0);
			if (needed <= 0) return std::wstring();

			std::wstring wide(static_cast<std::size_t>(needed), L'\0');
			::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
				wide.data(), needed);
			return wide;
		}

		std::string narrowUtf8(const std::wstring& text) {
			if (text.empty()) return std::string();

			const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
				static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
			if (needed <= 0) return std::string();

			std::string narrow(static_cast<std::size_t>(needed), '\0');
			::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
				narrow.data(), needed, nullptr, nullptr);
			return narrow;
		}

		std::string fieldString(lua_State* state, int index, const char* key,
				const std::string& fallback) {
			const int type = lua_getfield(state, index, key);
			std::string value = fallback;

			if (type == LUA_TSTRING || type == LUA_TNUMBER) {
				std::size_t length = 0;
				const char* text = lua_tolstring(state, -1, &length);
				value.assign(text, length);
			}

			lua_pop(state, 1);
			return value;
		}

		bool fieldBool(lua_State* state, int index, const char* key, bool fallback) {
			const int type = lua_getfield(state, index, key);
			const bool value = type == LUA_TBOOLEAN ? lua_toboolean(state, -1) != 0 : fallback;
			lua_pop(state, 1);
			return value;
		}

		double fieldNumber(lua_State* state, int index, const char* key, double fallback) {
			const int type = lua_getfield(state, index, key);
			const double value = type == LUA_TNUMBER ? lua_tonumber(state, -1) : fallback;
			lua_pop(state, 1);
			return value;
		}

		bool fieldIsTable(lua_State* state, int index, const char* key) {
			const int type = lua_getfield(state, index, key);
			lua_pop(state, 1);
			return type == LUA_TTABLE;
		}

		void setField(lua_State* state, const char* key, const std::string& value) {
			lua_pushlstring(state, value.data(), value.size());
			lua_setfield(state, -2, key);
		}

		void setField(lua_State* state, const char* key, bool value) {
			lua_pushboolean(state, value ? 1 : 0);
			lua_setfield(state, -2, key);
		}

		void setField(lua_State* state, const char* key, double value) {
			lua_pushnumber(state, value);
			lua_setfield(state, -2, key);
		}

		void setField(lua_State* state, const char* key, int value) {
			lua_pushinteger(state, value);
			lua_setfield(state, -2, key);
		}

		std::string colorText(std::uint32_t color) {
			char text[8] = {};
			std::snprintf(text, sizeof(text), "#%06X", color & 0xFFFFFF);
			return text;
		}

		static int fieldInt(lua_State* state, int index, const char* key, int fallback) {
			return static_cast<int>(fieldNumber(state, index, key, fallback));
		}

		static float fieldFloat(lua_State* state, int index, const char* key, float fallback) {
			return static_cast<float>(fieldNumber(state, index, key, fallback));
		}

		static const char* cursorStyleName(CursorStyle style) {
			switch (style) {
			case CursorStyle::Bar:       return "bar";
			case CursorStyle::Underline: return "underline";
			case CursorStyle::Block:
			default:                     return "block";
			}
		}

		static CursorStyle cursorStyleFrom(const std::string& name) {
			if (name == "bar") return CursorStyle::Bar;
			if (name == "underline") return CursorStyle::Underline;
			return CursorStyle::Block;
		}

		static void pushFont(lua_State* state, const FontSettings& font) {
			lua_newtable(state);
			setField(state, "family", narrowUtf8(font.family));
			setField(state, "size", static_cast<double>(font.size));
			setField(state, "line_height", static_cast<double>(font.line_height));

			lua_newtable(state);
			for (std::size_t i = 0; i < font.fallback.size(); ++i) {
				const std::string family = narrowUtf8(font.fallback[i]);
				lua_pushlstring(state, family.data(), family.size());
				lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
			}
			lua_setfield(state, -2, "fallback");

			lua_setfield(state, -2, "font");
		}

		static void pushWindow(lua_State* state, const WindowSettings& window) {
			lua_newtable(state);
			setField(state, "padding", window.padding);
			setField(state, "opacity", static_cast<double>(window.opacity));
			setField(state, "columns", window.columns);
			setField(state, "rows", window.rows);
			lua_setfield(state, -2, "window");
		}

		static void pushCursor(lua_State* state, const CursorSettings& cursor) {
			lua_newtable(state);
			setField(state, "style", std::string(cursorStyleName(cursor.style)));
			setField(state, "blink", cursor.blink);
			lua_setfield(state, -2, "cursor");
		}

		static void pushTitleBar(lua_State* state, const TitleBarSettings& titlebar) {
			lua_newtable(state);
			setField(state, "custom", titlebar.custom);
			setField(state, "height", titlebar.height);
			setField(state, "buttons", std::string(titlebar.on_right ? "right" : "left"));
			lua_setfield(state, -2, "titlebar");
		}

		static void pushKeyboard(lua_State* state, const KeyboardSettings& keyboard) {
			lua_newtable(state);
			setField(state, "right_alt",
				std::string(keyboard.right_alt == RightAltRole::Meta ? "meta" : "altgr"));
			setField(state, "new_window", keyboard.new_window);
			lua_setfield(state, -2, "keyboard");
		}

		static void pushPanes(lua_State* state, const PaneSettings& panes) {
			lua_newtable(state);
			setField(state, "prefix", panes.prefix);
			setField(state, "divider", panes.divider);
			setField(state, "focus_border", panes.focus_border);
			setField(state, "status", panes.status);
			lua_setfield(state, -2, "panes");
		}

		static void pushStatusBar(lua_State* state, const StatusBarSettings& bar) {
			lua_newtable(state);
			setField(state, "enabled", bar.enabled);
			setField(state, "cpu", bar.cpu);
			setField(state, "memory", bar.memory);
			setField(state, "disk", bar.disk);
			setField(state, "battery", bar.battery);
			setField(state, "clock", bar.clock);
			setField(state, "refresh_ms", bar.refresh_ms);
			lua_setfield(state, -2, "statusbar");
		}

		void pushPalette(lua_State* state, const Palette& palette) {
			lua_newtable(state);
			setField(state, "background", colorText(palette.background));
			setField(state, "foreground", colorText(palette.foreground));
			setField(state, "cursor", colorText(palette.cursor));
			setField(state, "selection", colorText(palette.selection));

			lua_newtable(state);
			for (int i = 0; i < 16; ++i) {
				const std::string text = colorText(palette.ansi[i]);
				lua_pushlstring(state, text.data(), text.size());
				lua_rawseti(state, -2, i + 1);
			}
			lua_setfield(state, -2, "ansi");
		}

		static void pushTheme(lua_State* state, const Config& config) {
			pushPalette(state, config.palette);
			setField(state, "name", config.theme_name);
			lua_setfield(state, -2, "theme");
		}

		static void pushScrollbackAndStartup(lua_State* state, const Config& config) {
			lua_newtable(state);
			setField(state, "lines", config.scrollback_lines);
			setField(state, "disk_mb", config.scrollback_disk_mb);
			lua_setfield(state, -2, "scrollback");

			lua_newtable(state);
			setField(state, "fetch", config.startup_fetch);
			lua_setfield(state, -2, "startup");
		}

		void pushConfig(lua_State* state, const Config& config) {
			lua_newtable(state);
			pushFont(state, config.font);
			pushWindow(state, config.window);
			pushCursor(state, config.cursor);
			pushTitleBar(state, config.titlebar);
			pushKeyboard(state, config.keyboard);
			pushPanes(state, config.panes);
			pushStatusBar(state, config.statusbar);
			pushTheme(state, config);
			pushScrollbackAndStartup(state, config);
		}

		static void pullFallbackFonts(lua_State* state, FontSettings& font) {
			if (lua_getfield(state, -1, "fallback") == LUA_TTABLE) {
				font.fallback.clear();
				const lua_Integer count = luaL_len(state, -1);
				for (lua_Integer i = 1; i <= count; ++i) {
					if (lua_rawgeti(state, -1, i) == LUA_TSTRING) {
						font.fallback.push_back(widenUtf8(lua_tostring(state, -1)));
					}
					lua_pop(state, 1);
				}
			}

			lua_pop(state, 1);
		}

		static void pullFont(lua_State* state, FontSettings& font) {
			if (lua_getfield(state, -1, "font") == LUA_TTABLE) {
				const std::string current = narrowUtf8(font.family);
				font.family      = widenUtf8(fieldString(state, -1, "family", current));
				font.size        = fieldFloat(state, -1, "size", font.size);
				font.line_height = fieldFloat(state, -1, "line_height", font.line_height);
				pullFallbackFonts(state, font);
			}

			lua_pop(state, 1);
		}

		static void pullWindow(lua_State* state, WindowSettings& window) {
			if (lua_getfield(state, -1, "window") == LUA_TTABLE) {
				window.padding = fieldInt(state, -1, "padding", window.padding);
				window.opacity = fieldFloat(state, -1, "opacity", window.opacity);
				window.columns = fieldInt(state, -1, "columns", window.columns);
				window.rows    = fieldInt(state, -1, "rows", window.rows);
			}

			lua_pop(state, 1);
		}

		static void pullCursor(lua_State* state, CursorSettings& cursor) {
			if (lua_getfield(state, -1, "cursor") == LUA_TTABLE) {
				cursor.style = cursorStyleFrom(fieldString(state, -1, "style",
					cursorStyleName(cursor.style)));
				cursor.blink = fieldBool(state, -1, "blink", cursor.blink);
			}

			lua_pop(state, 1);
		}

		static void pullTitleBar(lua_State* state, TitleBarSettings& titlebar) {
			if (lua_getfield(state, -1, "titlebar") == LUA_TTABLE) {
				titlebar.custom   = fieldBool(state, -1, "custom", titlebar.custom);
				titlebar.height   = fieldInt(state, -1, "height", titlebar.height);
				titlebar.on_right = fieldString(state, -1, "buttons",
					titlebar.on_right ? "right" : "left") != "left";
			}

			lua_pop(state, 1);
		}

		static void pullKeyboard(lua_State* state, KeyboardSettings& keyboard) {
			if (lua_getfield(state, -1, "keyboard") == LUA_TTABLE) {
				const std::string role = fieldString(state, -1, "right_alt",
					keyboard.right_alt == RightAltRole::Meta ? "meta" : "altgr");
				keyboard.right_alt  = role == "meta" ? RightAltRole::Meta : RightAltRole::AltGr;
				keyboard.new_window = fieldString(state, -1, "new_window", keyboard.new_window);
			}

			lua_pop(state, 1);
		}

		static void pullPanes(lua_State* state, PaneSettings& panes) {
			if (lua_getfield(state, -1, "panes") == LUA_TTABLE) {
				panes.prefix       = fieldString(state, -1, "prefix", panes.prefix);
				panes.divider      = fieldInt(state, -1, "divider", panes.divider);
				panes.focus_border = fieldBool(state, -1, "focus_border", panes.focus_border);
				panes.status       = fieldBool(state, -1, "status", panes.status);
			}

			lua_pop(state, 1);
		}

		static void pullStatusBar(lua_State* state, StatusBarSettings& bar) {
			if (lua_getfield(state, -1, "statusbar") == LUA_TTABLE) {
				bar.enabled    = fieldBool(state, -1, "enabled", bar.enabled);
				bar.cpu        = fieldBool(state, -1, "cpu", bar.cpu);
				bar.memory     = fieldBool(state, -1, "memory", bar.memory);
				bar.disk       = fieldBool(state, -1, "disk", bar.disk);
				bar.battery    = fieldBool(state, -1, "battery", bar.battery);
				bar.clock      = fieldBool(state, -1, "clock", bar.clock);
				bar.refresh_ms = fieldInt(state, -1, "refresh_ms", bar.refresh_ms);
			}

			lua_pop(state, 1);
		}

		// The table still holds the colours it was given, so only a value the
		// script changed is applied; the rest must not paint over a theme the
		// script has just named.
		static void pullChangedColor(const std::string& key, const std::string& text,
				const std::string& given, Palette& palette) {
			if (text != given) applyPaletteKey(key, text, palette);
		}

		static void pullThemeColors(lua_State* state, const Palette& given, Palette& palette) {
			pullChangedColor("background", fieldString(state, -1, "background", ""),
				colorText(given.background), palette);
			pullChangedColor("foreground", fieldString(state, -1, "foreground", ""),
				colorText(given.foreground), palette);
			pullChangedColor("cursor", fieldString(state, -1, "cursor", ""),
				colorText(given.cursor), palette);
			pullChangedColor("selection", fieldString(state, -1, "selection", ""),
				colorText(given.selection), palette);

			if (lua_getfield(state, -1, "ansi") == LUA_TTABLE) {
				for (int i = 0; i < 16; ++i) {
					if (lua_rawgeti(state, -1, i + 1) == LUA_TSTRING) {
						pullChangedColor("ansi" + std::to_string(i), lua_tostring(state, -1),
							colorText(given.ansi[i]), palette);
					}
					lua_pop(state, 1);
				}
			}

			lua_pop(state, 1);
		}

		void pullPalette(lua_State* state, int index, Palette& palette) {
			static const char* const kKeys[] = {
				"background", "foreground", "cursor", "selection",
			};

			lua_pushvalue(state, index);
			for (const char* key : kKeys) {
				applyPaletteKey(key, fieldString(state, -1, key, ""), palette);
			}

			if (lua_getfield(state, -1, "ansi") == LUA_TTABLE) {
				for (int i = 0; i < 16; ++i) {
					if (lua_rawgeti(state, -1, i + 1) == LUA_TSTRING) {
						const std::string key = "ansi" + std::to_string(i);
						applyPaletteKey(key, lua_tostring(state, -1), palette);
					}
					lua_pop(state, 1);
				}
			}

			lua_pop(state, 2);
		}

		// A new theme name resets every colour before the script's own
		// colours go on top, the same order the config file applies them in.
		static void pullTheme(lua_State* state, const std::wstring& themes_directory,
				Config& config) {
			if (lua_getfield(state, -1, "theme") == LUA_TTABLE) {
				const Palette given = config.palette;
				const std::string name = fieldString(state, -1, "name", config.theme_name);
				const bool renamed = name != config.theme_name
					&& findTheme(themes_directory, name, config.palette);
				if (renamed) config.theme_name = name;

				pullThemeColors(state, given, config.palette);
			}

			lua_pop(state, 1);
		}

		static void pullScrollbackAndStartup(lua_State* state, Config& config) {
			if (lua_getfield(state, -1, "scrollback") == LUA_TTABLE) {
				config.scrollback_lines   = fieldInt(state, -1, "lines", config.scrollback_lines);
				config.scrollback_disk_mb = fieldInt(state, -1, "disk_mb",
					config.scrollback_disk_mb);
			}
			lua_pop(state, 1);

			if (lua_getfield(state, -1, "startup") == LUA_TTABLE) {
				config.startup_fetch = fieldBool(state, -1, "fetch", config.startup_fetch);
			}
			lua_pop(state, 1);
		}

		void pullConfig(lua_State* state, int index, const std::wstring& themes_directory,
				Config& config) {
			lua_pushvalue(state, index);

			pullFont(state, config.font);
			pullWindow(state, config.window);
			pullCursor(state, config.cursor);
			pullTitleBar(state, config.titlebar);
			pullKeyboard(state, config.keyboard);
			pullPanes(state, config.panes);
			pullStatusBar(state, config.statusbar);
			pullTheme(state, themes_directory, config);
			pullScrollbackAndStartup(state, config);

			lua_pop(state, 1);
		}

		static const char* tintName(int tint) {
			switch (tint) {
			case kTintRed:    return "red";
			case kTintGreen:  return "green";
			case kTintYellow: return "yellow";
			default:          return "";
			}
		}

		static int tintFrom(const std::string& name) {
			if (name == "red") return kTintRed;
			if (name == "green") return kTintGreen;
			if (name == "yellow") return kTintYellow;
			return kTintPlain;
		}

		static void pushSegments(lua_State* state, const std::vector<StatusSegment>& segments) {
			lua_newtable(state);

			for (std::size_t i = 0; i < segments.size(); ++i) {
				lua_newtable(state);
				setField(state, "label", segments[i].label);
				setField(state, "value", segments[i].value);
				if (segments[i].tint != kTintPlain) {
					setField(state, "tint", std::string(tintName(segments[i].tint)));
				}
				lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
			}
		}

		static void pushSample(lua_State* state, const SystemSample& sample) {
			setField(state, "cpu", sample.cpu_percent);
			setField(state, "memory_used_mb", static_cast<double>(sample.memory_used_mb));
			setField(state, "memory_total_mb", static_cast<double>(sample.memory_total_mb));
			setField(state, "disk_letter", std::string(sample.disk_letter != 0
				? std::string(1, sample.disk_letter) : std::string()));
			setField(state, "disk_free_gb", static_cast<double>(sample.disk_free_gb));
			setField(state, "disk_total_gb", static_cast<double>(sample.disk_total_gb));
			setField(state, "battery", sample.battery_percent);
			setField(state, "on_mains", sample.on_mains);
		}

		static void pushPaneNames(lua_State* state, const StatusContext& context) {
			lua_newtable(state);
			for (std::size_t i = 0; i < context.panes.size(); ++i) {
				lua_pushlstring(state, context.panes[i].data(), context.panes[i].size());
				lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
			}
			lua_setfield(state, -2, "panes");

			setField(state, "focused_pane", static_cast<int>(context.focused_pane + 1));
			setField(state, "zoomed", context.zoomed);
		}

		void pushStatusContext(lua_State* state, const StatusContext& context,
				const std::vector<StatusSegment>& segments) {
			lua_newtable(state);
			pushSample(state, context.sample);
			setField(state, "user", context.user);
			setField(state, "host", context.host);
			setField(state, "clock", context.clock);
			setField(state, "directory", context.directory);
			setField(state, "tmux", context.tmux);
			pushPaneNames(state, context);

			pushSegments(state, segments);
			lua_setfield(state, -2, "segments");
		}

		static bool pullSegment(lua_State* state, StatusSegment& out_segment) {
			if (lua_type(state, -1) == LUA_TSTRING) {
				out_segment.value = lua_tostring(state, -1);
				return true;
			}

			if (lua_type(state, -1) != LUA_TTABLE) return false;

			out_segment.label = fieldString(state, -1, "label", "");
			out_segment.value = fieldString(state, -1, "value", "");
			out_segment.tint  = tintFrom(fieldString(state, -1, "tint", ""));
			return true;
		}

		bool pullSegments(lua_State* state, int index, std::vector<StatusSegment>& segments) {
			if (lua_type(state, index) != LUA_TTABLE) return false;

			lua_pushvalue(state, index);
			const lua_Integer count = luaL_len(state, -1);

			segments.clear();
			for (lua_Integer i = 1; i <= count; ++i) {
				lua_rawgeti(state, -1, i);
				StatusSegment segment;
				if (pullSegment(state, segment)) segments.push_back(segment);
				lua_pop(state, 1);
			}

			lua_pop(state, 1);
			return true;
		}

		void pushFetchRows(lua_State* state, const std::vector<FetchRow>& rows) {
			lua_newtable(state);

			for (std::size_t i = 0; i < rows.size(); ++i) {
				lua_newtable(state);
				setField(state, "label", rows[i].label);
				setField(state, "value", rows[i].value);
				lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
			}
		}

		bool pullFetchRows(lua_State* state, int index, std::vector<FetchRow>& rows) {
			if (lua_type(state, index) != LUA_TTABLE) return false;

			lua_pushvalue(state, index);
			const lua_Integer count = luaL_len(state, -1);

			rows.clear();
			for (lua_Integer i = 1; i <= count; ++i) {
				if (lua_rawgeti(state, -1, i) == LUA_TTABLE) {
					rows.push_back({ fieldString(state, -1, "label", ""),
						fieldString(state, -1, "value", "") });
				}
				lua_pop(state, 1);
			}

			lua_pop(state, 1);
			return true;
		}

	} /* namespace script_detail */

} /* namespace wbshterm */

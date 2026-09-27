/**
 * @file script_actions.cpp
 * @brief wbshterm.actions: what a key binding can make the window do.
 */

#include "script_internal.h"

#include <string>

namespace wbshterm {

	namespace script_detail {

		ScriptHost* hostOf(lua_State* state) {
			lua_getfield(state, LUA_REGISTRYINDEX, kHostKey);
			ScriptHost* host = static_cast<ScriptHost*>(lua_touserdata(state, -1));
			lua_pop(state, 1);
			return host;
		}

		// Actions reach a window only while one of its bindings is running;
		// from the top of the script there is no window yet to act on.
		static ScriptActions& actionsOf(lua_State* state) {
			ScriptHost* host = hostOf(state);
			ScriptActions* actions = host != nullptr ? host->actions() : nullptr;
			if (actions == nullptr) {
				luaL_error(state, "wbshterm.actions can only be used from a key binding");
			}

			return *actions;
		}

		static int actionNewWindow(lua_State* state) {
			actionsOf(state).newWindow();
			return 0;
		}

		static int actionCopy(lua_State* state) {
			actionsOf(state).copySelected();
			return 0;
		}

		static int actionPaste(lua_State* state) {
			actionsOf(state).pasteClipboard();
			return 0;
		}

		static int actionCopyLastOutput(lua_State* state) {
			actionsOf(state).copyLastOutput();
			return 0;
		}

		static int actionSend(lua_State* state) {
			std::size_t length = 0;
			const char* text = luaL_checklstring(state, 1, &length);
			actionsOf(state).sendText(std::string(text, length));
			return 0;
		}

		static int actionScroll(lua_State* state) {
			const int lines = static_cast<int>(luaL_checkinteger(state, 1));
			actionsOf(state).scrollLines(lines);
			return 0;
		}

		static int actionFontSize(lua_State* state) {
			const int delta = static_cast<int>(luaL_optinteger(state, 1, 0));
			if (delta == 0) actionsOf(state).fontSizeReset();
			else actionsOf(state).fontSizeStep(delta);
			return 0;
		}

		static int actionTheme(lua_State* state) {
			const char* name = luaL_checkstring(state, 1);
			lua_pushboolean(state, actionsOf(state).setTheme(name) ? 1 : 0);
			return 1;
		}

		static int actionSearch(lua_State* state) {
			actionsOf(state).openSearch();
			return 0;
		}

		static int actionPreviousCommand(lua_State* state) {
			actionsOf(state).jumpCommand(true);
			return 0;
		}

		static int actionNextCommand(lua_State* state) {
			actionsOf(state).jumpCommand(false);
			return 0;
		}

		static int actionSplit(lua_State* state) {
			static const char* const kAxes[] = { "columns", "rows", nullptr };
			const int axis = luaL_checkoption(state, 1, "columns", kAxes);
			actionsOf(state).splitPane(axis == 0 ? SplitAxis::Columns : SplitAxis::Rows);
			return 0;
		}

		static int actionClosePane(lua_State* state) {
			actionsOf(state).closePane();
			return 0;
		}

		static int actionZoom(lua_State* state) {
			actionsOf(state).zoomPane();
			return 0;
		}

		static int actionFocus(lua_State* state) {
			static const char* const kMoves[] = { "left", "right", "up", "down", "next", nullptr };
			static const PaneMove kMoveOf[] = { PaneMove::Left, PaneMove::Right, PaneMove::Up,
				PaneMove::Down, PaneMove::Next };

			const int move = luaL_checkoption(state, 1, "next", kMoves);
			actionsOf(state).movePane(kMoveOf[move]);
			return 0;
		}

		static int actionReload(lua_State* state) {
			actionsOf(state).reloadSettings();
			return 0;
		}

		static const luaL_Reg kActions[] = {
			{ "new_window",       actionNewWindow },
			{ "copy",             actionCopy },
			{ "paste",            actionPaste },
			{ "copy_last_output", actionCopyLastOutput },
			{ "send",             actionSend },
			{ "scroll",           actionScroll },
			{ "font_size",        actionFontSize },
			{ "theme",            actionTheme },
			{ "search",           actionSearch },
			{ "previous_command", actionPreviousCommand },
			{ "next_command",     actionNextCommand },
			{ "split",            actionSplit },
			{ "close_pane",       actionClosePane },
			{ "zoom",             actionZoom },
			{ "focus",            actionFocus },
			{ "reload",           actionReload },
			{ nullptr,            nullptr },
		};

		void openActions(lua_State* state, ScriptHost* host) {
			lua_pushlightuserdata(state, host);
			lua_setfield(state, LUA_REGISTRYINDEX, kHostKey);

			luaL_newlib(state, kActions);
			lua_setfield(state, -2, "actions");
		}

	} /* namespace script_detail */

} /* namespace wbshterm */

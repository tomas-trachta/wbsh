#pragma once

/**
 * @file script_internal.h
 * @brief What the three script_*.cpp files share: table helpers, the
 *        config bridge and the actions library.
 */

#include "script.h"

#include "lua.hpp"

#include <cstdint>
#include <string>

namespace wbshterm {

	namespace script_detail {

		static const char* const kGlobal      = "wbshterm";
		static const char* const kHandlersKey = "wbshterm.handlers";
		static const char* const kBindingsKey = "wbshterm.bindings";
		static const char* const kHostKey     = "wbshterm.host";

		std::wstring widenUtf8(const std::string& text);
		std::string narrowUtf8(const std::wstring& text);

		/** Reads table[key] at @p index, leaving the stack as it was. */
		std::string fieldString(lua_State* state, int index, const char* key,
			const std::string& fallback);
		bool fieldBool(lua_State* state, int index, const char* key, bool fallback);
		double fieldNumber(lua_State* state, int index, const char* key, double fallback);
		bool fieldIsTable(lua_State* state, int index, const char* key);

		void setField(lua_State* state, const char* key, const std::string& value);
		void setField(lua_State* state, const char* key, bool value);
		void setField(lua_State* state, const char* key, double value);
		void setField(lua_State* state, const char* key, int value);

		std::string colorText(std::uint32_t color);

		void pushPalette(lua_State* state, const Palette& palette);
		void pullPalette(lua_State* state, int index, Palette& palette);
		void pushConfig(lua_State* state, const Config& config);
		void pullConfig(lua_State* state, int index, const std::wstring& themes_directory,
			Config& config);

		void pushStatusContext(lua_State* state, const StatusContext& context,
			const std::vector<StatusSegment>& segments);
		bool pullSegments(lua_State* state, int index, std::vector<StatusSegment>& segments);

		void pushFetchRows(lua_State* state, const std::vector<FetchRow>& rows);
		bool pullFetchRows(lua_State* state, int index, std::vector<FetchRow>& rows);

		/** The actions library, on top of the wbshterm table at the top of the stack. */
		void openActions(lua_State* state, ScriptHost* host);
		ScriptHost* hostOf(lua_State* state);

	} /* namespace script_detail */

} /* namespace wbshterm */

/**
 * @file script.cpp
 * @brief The Lua state behind init.lua: loading, handlers, key bindings.
 */

#include "script.h"

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

		static const char* const kEventNames[] = {
			"status_left", "status_right", "title", "fetch", "tick",
		};
		static const int kEventCount = sizeof(kEventNames) / sizeof(kEventNames[0]);

		static const char* eventName(ScriptEvent event) {
			return kEventNames[static_cast<int>(event)];
		}

		static bool eventFromName(const std::string& name, ScriptEvent& out_event) {
			for (int i = 0; i < kEventCount; ++i) {
				if (name == kEventNames[i]) {
					out_event = static_cast<ScriptEvent>(i);
					return true;
				}
			}

			return false;
		}

		static bool sameKey(const KeyPress& press, const KeyPress& binding) {
			return press.virtual_key == binding.virtual_key
				&& press.control == binding.control
				&& press.alt == binding.alt
				&& press.shift == binding.shift;
		}

		static bool readFileBytes(const std::wstring& path, std::string& out_text) {
			FILE* file = _wfopen(path.c_str(), L"rb");
			if (file == nullptr) return false;

			char buffer[4096];
			std::size_t got = 0;
			while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
				out_text.append(buffer, got);
			}

			std::fclose(file);
			return true;
		}

		static std::wstring directoryOf(const std::wstring& path) {
			const std::size_t cut = path.find_last_of(L"/\\");
			return cut == std::wstring::npos ? std::wstring() : path.substr(0, cut);
		}

		static std::wstring fileNameOf(const std::wstring& path) {
			const std::size_t cut = path.find_last_of(L"/\\");
			return cut == std::wstring::npos ? path : path.substr(cut + 1);
		}

		static int scriptOn(lua_State* state) {
			const std::string name = luaL_checkstring(state, 1);
			luaL_checktype(state, 2, LUA_TFUNCTION);

			ScriptEvent event = ScriptEvent::StatusLeft;
			if (!eventFromName(name, event)) {
				return luaL_error(state, "wbshterm.on: unknown event '%s'", name.c_str());
			}

			lua_getfield(state, LUA_REGISTRYINDEX, kHandlersKey);
			lua_pushvalue(state, 2);
			lua_setfield(state, -2, eventName(event));
			lua_pop(state, 1);
			return 0;
		}

		static int scriptBind(lua_State* state) {
			const std::string keys = luaL_checkstring(state, 1);
			luaL_checktype(state, 2, LUA_TFUNCTION);

			KeyPress press;
			if (!parseKeyBinding(keys, press)) {
				return luaL_error(state, "wbshterm.bind: '%s' is not a key binding", keys.c_str());
			}

			ScriptHost* host = hostOf(state);
			if (host == nullptr) return 0;

			host->addBinding(press);
			lua_getfield(state, LUA_REGISTRYINDEX, kBindingsKey);
			lua_pushvalue(state, 2);
			lua_rawseti(state, -2, static_cast<lua_Integer>(host->bindingCount()));
			lua_pop(state, 1);
			return 0;
		}

		static int scriptLog(lua_State* state) {
			const int count = lua_gettop(state);
			std::string line;

			for (int i = 1; i <= count; ++i) {
				std::size_t length = 0;
				const char* text = luaL_tolstring(state, i, &length);
				if (i > 1) line += '\t';
				line.append(text, length);
				lua_pop(state, 1);
			}

			ScriptHost* host = hostOf(state);
			if (host != nullptr) host->appendLog(line);
			return 0;
		}

		static const luaL_Reg kLibrary[] = {
			{ "on",   scriptOn },
			{ "bind", scriptBind },
			{ "log",  scriptLog },
			{ nullptr, nullptr },
		};

		static void setPackagePath(lua_State* state, const std::wstring& directory) {
			const std::string folder = narrowUtf8(directory);
			const std::string path = folder + "\\?.lua;" + folder + "\\?\\init.lua";

			lua_getglobal(state, "package");
			if (lua_type(state, -1) == LUA_TTABLE) setField(state, "path", path);
			lua_pop(state, 1);
		}

		static void openLibrary(lua_State* state, ScriptHost* host, const Config& config) {
			lua_newtable(state);
			lua_setfield(state, LUA_REGISTRYINDEX, kHandlersKey);
			lua_newtable(state);
			lua_setfield(state, LUA_REGISTRYINDEX, kBindingsKey);

			luaL_newlib(state, kLibrary);
			openActions(state, host);
			pushConfig(state, config);
			lua_setfield(state, -2, "config");
			lua_setglobal(state, kGlobal);
		}

	} /* namespace script_detail */

	ScriptHost::ScriptHost() = default;

	ScriptHost::~ScriptHost() {
		unload();
	}

	void ScriptHost::unload() {
		if (state_ != nullptr) lua_close(state_);

		state_ = nullptr;
		actions_ = nullptr;
		bindings_.clear();
	}

	void ScriptHost::appendLog(const std::string& line) {
		if (log_path_.empty()) return;

		FILE* file = _wfopen(log_path_.c_str(), L"ab");
		if (file == nullptr) return;

		std::fwrite(line.data(), 1, line.size(), file);
		std::fputc('\n', file);
		std::fclose(file);
	}

	void ScriptHost::keepError() {
		const char* message = lua_tostring(state_, -1);
		error_ = message != nullptr ? message : "unknown error";
		lua_pop(state_, 1);
		appendLog("error: " + error_);
	}

	bool ScriptHost::openState(const std::wstring& path) {
		state_ = luaL_newstate();
		if (state_ == nullptr) {
			error_ = "out of memory";
			return false;
		}

		luaL_openlibs(state_);
		script_detail::setPackagePath(state_, script_detail::directoryOf(path));
		return true;
	}

	bool ScriptHost::runFile(const std::wstring& path) {
		std::string text;
		if (!script_detail::readFileBytes(path, text)) {
			error_ = "cannot read " + script_detail::narrowUtf8(path);
			return false;
		}

		const std::string chunk = "@" + script_detail::narrowUtf8(script_detail::fileNameOf(path));
		if (luaL_loadbuffer(state_, text.data(), text.size(), chunk.c_str()) != LUA_OK) {
			keepError();
			return false;
		}

		if (lua_pcall(state_, 0, 0, 0) != LUA_OK) {
			keepError();
			return false;
		}

		return true;
	}

	bool ScriptHost::load(const std::wstring& path, const std::wstring& config_path,
			Config& config) {
		unload();
		error_.clear();
		if (path.empty() || configStamp(path) == 0) return true;

		log_path_ = script_detail::directoryOf(path) + L"\\init.log";
		::DeleteFileW(log_path_.c_str());

		if (!openState(path)) return false;
		script_detail::openLibrary(state_, this, config);

		if (!runFile(path)) {
			unload();
			return false;
		}

		lua_getglobal(state_, script_detail::kGlobal);
		if (lua_getfield(state_, -1, "config") == LUA_TTABLE) {
			script_detail::pullConfig(state_, -1, themesDirectory(config_path), config);
		}
		lua_pop(state_, 2);
		return true;
	}

	bool ScriptHost::pushHandler(ScriptEvent event) const {
		if (state_ == nullptr) return false;

		lua_getfield(state_, LUA_REGISTRYINDEX, script_detail::kHandlersKey);
		const int type = lua_getfield(state_, -1, script_detail::eventName(event));
		lua_remove(state_, -2);

		if (type == LUA_TFUNCTION) return true;

		lua_pop(state_, 1);
		return false;
	}

	bool ScriptHost::handles(ScriptEvent event) const {
		if (!pushHandler(event)) return false;

		lua_pop(state_, 1);
		return true;
	}

	// The handler and its arguments are already on the stack; on success the
	// results replace them, on failure nothing is left and the error is kept.
	bool ScriptHost::callHandler(ScriptEvent event, int arguments, int results) {
		if (lua_pcall(state_, arguments, results, 0) == LUA_OK) return true;

		keepError();
		appendLog(std::string("in handler ") + script_detail::eventName(event));
		return false;
	}

	bool ScriptHost::statusSegments(ScriptEvent side, const StatusContext& context,
			std::vector<StatusSegment>& segments) {
		if (!pushHandler(side)) return false;

		script_detail::pushStatusContext(state_, context, segments);
		if (!callHandler(side, 1, 1)) return false;

		const bool replaced = script_detail::pullSegments(state_, -1, segments);
		lua_pop(state_, 1);
		return replaced;
	}

	bool ScriptHost::title(const std::string& shown, const std::string& directory,
			std::string& out_title) {
		if (!pushHandler(ScriptEvent::Title)) return false;

		lua_newtable(state_);
		script_detail::setField(state_, "title", shown);
		script_detail::setField(state_, "directory", directory);
		if (!callHandler(ScriptEvent::Title, 1, 1)) return false;

		const bool replaced = lua_type(state_, -1) == LUA_TSTRING;
		if (replaced) out_title = lua_tostring(state_, -1);
		lua_pop(state_, 1);
		return replaced;
	}

	bool ScriptHost::fetchRows(std::vector<FetchRow>& rows) {
		if (!pushHandler(ScriptEvent::Fetch)) return false;

		script_detail::pushFetchRows(state_, rows);
		if (!callHandler(ScriptEvent::Fetch, 1, 1)) return false;

		const bool replaced = script_detail::pullFetchRows(state_, -1, rows);
		lua_pop(state_, 1);
		return replaced;
	}

	bool ScriptHost::tick(double seconds, const Palette& base, Palette& out_palette) {
		if (!pushHandler(ScriptEvent::Tick)) return false;

		lua_newtable(state_);
		script_detail::setField(state_, "time", seconds);
		script_detail::pushPalette(state_, base);
		lua_setfield(state_, -2, "palette");
		if (!callHandler(ScriptEvent::Tick, 1, 1)) return false;

		const bool painted = lua_type(state_, -1) == LUA_TTABLE;
		if (painted) {
			out_palette = base;
			script_detail::pullPalette(state_, -1, out_palette);
		}

		lua_pop(state_, 1);
		return painted;
	}

	bool ScriptHost::binds(const KeyPress& press) const {
		for (const KeyPress& binding : bindings_) {
			if (script_detail::sameKey(press, binding)) return true;
		}

		return false;
	}

	bool ScriptHost::pushBinding(std::size_t index) {
		lua_getfield(state_, LUA_REGISTRYINDEX, script_detail::kBindingsKey);
		const int type = lua_rawgeti(state_, -1, static_cast<lua_Integer>(index + 1));
		lua_remove(state_, -2);

		if (type == LUA_TFUNCTION) return true;

		lua_pop(state_, 1);
		return false;
	}

	bool ScriptHost::runBinding(const KeyPress& press, ScriptActions& actions) {
		if (state_ == nullptr) return false;

		for (std::size_t i = 0; i < bindings_.size(); ++i) {
			if (!script_detail::sameKey(press, bindings_[i])) continue;
			if (!pushBinding(i)) return false;

			actions_ = &actions;
			const bool ran = lua_pcall(state_, 0, 0, 0) == LUA_OK;
			actions_ = nullptr;

			if (!ran) keepError();
			return true;
		}

		return false;
	}

	std::wstring scriptPath(const std::wstring& config_path) {
		if (config_path.empty()) return std::wstring();

		return script_detail::directoryOf(config_path) + L"\\init.lua";
	}

	unsigned long long scriptedSettingsStamp(const std::wstring& config_path,
			const std::string& theme_name) {
		return settingsStamp(config_path, theme_name)
			^ (configStamp(scriptPath(config_path)) * 7919ull);
	}

	static const char* const kExampleScript =
		"-- wbshterm init.lua\n"
		"--\n"
		"-- Runs after wbshterm.conf, and again whenever this file is saved.\n"
		"-- Everything below is a comment: uncomment what you want to try.\n"
		"-- Errors are shown in the status bar and written to init.log.\n"
		"\n"
		"-- Settings: the same keys as wbshterm.conf, and this file wins.\n"
		"-- wbshterm.config.font.size = 12\n"
		"-- wbshterm.config.theme.name = \"nord\"\n"
		"-- wbshterm.config.theme.background = \"#101418\"\n"
		"\n"
		"-- The right end of the status bar. info.segments holds what would\n"
		"-- be shown; return the list to show instead, or nil for the default.\n"
		"-- wbshterm.on(\"status_right\", function(info)\n"
		"--     table.insert(info.segments, 1, { label = \"cpu\", value = info.cpu .. \"%\",\n"
		"--         tint = info.cpu > 85 and \"red\" or nil })\n"
		"--     return info.segments\n"
		"-- end)\n"
		"\n"
		"-- The left end: info.user, info.host, info.directory, info.panes.\n"
		"-- wbshterm.on(\"status_left\", function(info)\n"
		"--     return { { value = info.user .. \" in \" .. info.directory, tint = \"green\" } }\n"
		"-- end)\n"
		"\n"
		"-- The window title: info.title is what would be shown.\n"
		"-- wbshterm.on(\"title\", function(info)\n"
		"--     return \"wbsh \\u{2014} \" .. info.title\n"
		"-- end)\n"
		"\n"
		"-- The startup panel: rows is the list of { label, value } pairs.\n"
		"-- wbshterm.on(\"fetch\", function(rows)\n"
		"--     table.insert(rows, { label = \"Mood\", value = \"good\" })\n"
		"--     return rows\n"
		"-- end)\n"
		"\n"
		"-- An animation: called about thirty times a second with info.time in\n"
		"-- seconds and info.palette, the theme's colours. Return the colours to\n"
		"-- paint this frame, or nil to leave the theme alone.\n"
		"-- wbshterm.on(\"tick\", function(info)\n"
		"--     local pulse = math.floor(128 + 127 * math.sin(info.time * 3))\n"
		"--     return { cursor = string.format(\"#%02X40%02X\", pulse, 255 - pulse) }\n"
		"-- end)\n"
		"\n"
		"-- Key bindings run before the shell sees the key.\n"
		"-- wbshterm.bind(\"ctrl+shift+n\", function() wbshterm.actions.new_window() end)\n"
		"-- wbshterm.bind(\"ctrl+shift+d\", function() wbshterm.actions.split(\"columns\") end)\n"
		"-- wbshterm.bind(\"ctrl+shift+t\", function() wbshterm.actions.theme(\"dracula\") end)\n"
		"-- wbshterm.bind(\"ctrl+shift+l\", function() wbshterm.actions.send(\"ls -la\\r\") end)\n"
		"\n"
		"-- Other actions: copy(), paste(), copy_last_output(), scroll(lines),\n"
		"-- font_size(delta), search(), previous_command(), next_command(),\n"
		"-- close_pane(), zoom(), focus(\"left\"|\"right\"|\"up\"|\"down\"|\"next\"), reload().\n"
		"-- wbshterm.log(...) appends a line to init.log.\n";

	bool writeExampleScript(const std::wstring& path) {
		if (path.empty() || configStamp(path) != 0) return true;

		FILE* file = _wfopen(path.c_str(), L"wb");
		if (file == nullptr) return false;

		const std::size_t length = std::char_traits<char>::length(kExampleScript);
		const bool written = std::fwrite(kExampleScript, 1, length, file) == length;
		std::fclose(file);
		return written;
	}

} /* namespace wbshterm */

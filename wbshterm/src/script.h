#pragma once

/**
 * @file script.h
 * @brief init.lua: the script that customises the terminal's chrome.
 */

#include "config.h"
#include "fetch.h"
#include "keymap.h"
#include "pane_tree.h"
#include "status.h"
#include "sysinfo.h"

#include <cstddef>
#include <string>
#include <vector>

struct lua_State;

namespace wbshterm {

	enum class ScriptEvent {
		StatusLeft,
		StatusRight,
		Title,
		Fetch,
		Tick,
	};

	/** What a status handler is told about the machine and the window. */
	struct StatusContext {
		SystemSample             sample;
		std::string              user;
		std::string              host;
		std::string              clock;
		std::string              directory;
		bool                     tmux = false;
		std::vector<std::string> panes;
		std::size_t              focused_pane = 0;
		bool                     zoomed = false;
	};

	enum class PaneMove {
		Left,
		Right,
		Up,
		Down,
		Next,
	};

	/** What a key binding's function may do to the window that runs it. */
	class ScriptActions {
	public:
		virtual ~ScriptActions() = default;

		virtual void newWindow() = 0;
		virtual void copySelected() = 0;
		virtual void pasteClipboard() = 0;
		virtual void copyLastOutput() = 0;
		virtual void sendText(const std::string& text) = 0;
		virtual void scrollLines(int lines) = 0;
		virtual void fontSizeStep(int delta) = 0;
		virtual void fontSizeReset() = 0;
		virtual bool setTheme(const std::string& name) = 0;
		virtual void openSearch() = 0;
		virtual void jumpCommand(bool backwards) = 0;
		virtual void splitPane(SplitAxis axis) = 0;
		virtual void closePane() = 0;
		virtual void zoomPane() = 0;
		virtual void movePane(PaneMove move) = 0;
		virtual void reloadSettings() = 0;
	};

	/**
	 * @brief One Lua state running the user's init.lua.
	 *
	 * Every call is made on the thread that owns the window; nothing here
	 * is locked. A script error never reaches the caller as anything but
	 * false and lastError(): the terminal keeps running on its defaults.
	 */
	class ScriptHost {
	public:
		ScriptHost();
		~ScriptHost();
		ScriptHost(const ScriptHost&) = delete;
		ScriptHost& operator=(const ScriptHost&) = delete;

		/**
		 * @brief Runs the script at @p path over @p config.
		 *
		 * A missing file leaves the config alone and counts as success;
		 * loaded() then says no. The script's edits to wbshterm.config are
		 * read back into @p config when it returns, with @p config_path
		 * naming where its themes folder is.
		 */
		bool load(const std::wstring& path, const std::wstring& config_path, Config& config);
		void unload();
		bool loaded() const { return state_ != nullptr; }
		const std::string& lastError() const { return error_; }

		bool handles(ScriptEvent event) const;

		/** Each returns false when the script has no say; the defaults stand. */
		bool statusSegments(ScriptEvent side, const StatusContext& context,
			std::vector<StatusSegment>& segments);
		bool title(const std::string& shown, const std::string& directory, std::string& out_title);
		bool fetchRows(std::vector<FetchRow>& rows);

		/**
		 * @brief One animation frame: the script is told the time and the
		 *        theme's palette, and may answer with the palette to paint now.
		 */
		bool tick(double seconds, const Palette& base, Palette& out_palette);

		bool binds(const KeyPress& press) const;
		bool runBinding(const KeyPress& press, ScriptActions& actions);

		/** For the actions library: the window a binding is running against. */
		ScriptActions* actions() const { return actions_; }
		void addBinding(const KeyPress& press) { bindings_.push_back(press); }
		std::size_t bindingCount() const { return bindings_.size(); }
		void appendLog(const std::string& line);

	private:
		bool openState(const std::wstring& path);
		bool runFile(const std::wstring& path);
		bool callHandler(ScriptEvent event, int arguments, int results);
		bool pushHandler(ScriptEvent event) const;
		bool pushBinding(std::size_t index);
		void keepError();

		lua_State*            state_    = nullptr;
		ScriptActions*        actions_  = nullptr;
		std::string           error_;
		std::wstring          log_path_;
		std::vector<KeyPress> bindings_;
	};

	/** init.lua beside the config file, or empty when there is no config path. */
	std::wstring scriptPath(const std::wstring& config_path);

	/** Writes the documented example script if nothing is at @p path yet. */
	bool writeExampleScript(const std::wstring& path);

	/** One stamp covering the config, its theme, and the script. */
	unsigned long long scriptedSettingsStamp(const std::wstring& config_path,
		const std::string& theme_name);

} /* namespace wbshterm */

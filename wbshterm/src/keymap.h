#pragma once

/**
 * @file keymap.h
 * @brief Key presses to the bytes a terminal sends for them.
 */

#include <string>
#include <vector>

namespace wbshterm {

	struct KeyPress {
		unsigned int virtual_key = 0;
		bool         control     = false;
		bool         alt         = false;
		bool         shift       = false;
	};

	/** Modes the shell turns on that change what a key means. */
	struct KeyModes {
		bool application_cursor = false;
		bool bracketed_paste    = false;
	};

	/**
	 * @brief Encodes a key press, or returns "" when the key is text.
	 *
	 * An empty result means this press carries no meaning of its own and
	 * the character message that follows it should be sent instead —
	 * which is how layouts, dead keys and AltGr keep working without
	 * this file knowing anything about them.
	 */
	std::string encodeKeyPress(const KeyPress& key, const KeyModes& modes);

	/**
	 * @brief Wraps pasted text so the shell can tell it from typing.
	 *
	 * Newlines become CR (what Enter sends), and when bracketed paste is
	 * off the brackets are omitted rather than pasted as literal text.
	 */
	std::string encodePaste(const std::string& text, const KeyModes& modes);

	/**
	 * @brief Spells dropped files the way a shell reads them back.
	 *
	 * Paths are joined with single spaces and a path holding a space is
	 * double-quoted, which is what Windows Terminal does and what programs
	 * that pick file names out of pasted text expect.
	 */
	std::string joinDroppedPaths(const std::vector<std::string>& paths);

	/**
	 * @brief Reads a binding as a config file spells it: "ctrl+a".
	 *
	 * Names are case-insensitive and the modifiers may come in any
	 * order; false means the text named no key this can encode.
	 */
	bool parseKeyBinding(const std::string& text, KeyPress& out_key);

	/** The xterm modifier parameter: 1 + shift + 2*alt + 4*ctrl. */
	int modifierParameter(const KeyPress& key);

	bool isModifierKey(unsigned int virtual_key);

	/** The modifier keys and locks held, as GetKeyState reports them. */
	struct Win32KeyState {
		bool left_alt      = false;
		bool right_alt     = false;
		bool left_control  = false;
		bool right_control = false;
		bool shift         = false;
		bool num_lock      = false;
		bool scroll_lock   = false;
		bool caps_lock     = false;
		bool enhanced      = false;
	};

	/** One key event the way a console input record holds it. */
	struct Win32KeyEvent {
		unsigned int virtual_key   = 0;
		unsigned int scan_code     = 0;
		unsigned int unicode       = 0;
		bool         down          = true;
		unsigned int control_state = 0;
		unsigned int repeat        = 1;
	};

	/** dwControlKeyState for @p state: AltGr is RIGHT_ALT plus the LEFT_CTRL Windows adds. */
	unsigned int win32ControlState(const Win32KeyState& state);

	/**
	 * @brief win32-input-mode (DECSET 9001): CSI Vk ; Sc ; Uc ; Kd ; Cs ; Rc _
	 *
	 * The pseudoconsole turns the sequence back into the very input record
	 * a console program would have read, key-ups and scan codes included.
	 * Uc is one UTF-16 unit, so a character past the BMP takes two events.
	 */
	std::string encodeWin32Key(const Win32KeyEvent& event);

} /* namespace wbshterm */

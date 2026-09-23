#pragma once

/**
 * @file keymap.h
 * @brief Key presses to the bytes a terminal sends for them.
 */

#include <string>

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

	/** The xterm modifier parameter: 1 + shift + 2*alt + 4*ctrl. */
	int modifierParameter(const KeyPress& key);

} /* namespace wbshterm */

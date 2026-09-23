/**
 * @file keymap.cpp
 * @brief The xterm encodings, grouped by the shape of the sequence.
 */

#include "keymap.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif /* WIN32_LEAN_AND_MEAN */
#include <windows.h>

namespace wbshterm {

	static const char kEsc = '\x1b';

	struct CursorKey {
		unsigned int virtual_key;
		char         final_byte;
	};

	struct NumberedKey {
		unsigned int virtual_key;
		int          number;
	};

	static const CursorKey kCursorKeys[] = {
		{ VK_UP,    'A' },
		{ VK_DOWN,  'B' },
		{ VK_RIGHT, 'C' },
		{ VK_LEFT,  'D' },
		{ VK_HOME,  'H' },
		{ VK_END,   'F' },
	};

	static const NumberedKey kNumberedKeys[] = {
		{ VK_INSERT, 2 },
		{ VK_DELETE, 3 },
		{ VK_PRIOR,  5 },
		{ VK_NEXT,   6 },
		{ VK_F5,     15 },
		{ VK_F6,     17 },
		{ VK_F7,     18 },
		{ VK_F8,     19 },
		{ VK_F9,     20 },
		{ VK_F10,    21 },
		{ VK_F11,    23 },
		{ VK_F12,    24 },
	};

	static const CursorKey kFunctionKeys[] = {
		{ VK_F1, 'P' },
		{ VK_F2, 'Q' },
		{ VK_F3, 'R' },
		{ VK_F4, 'S' },
	};

	int modifierParameter(const KeyPress& key) {
		int parameter = 1;
		if (key.shift)   parameter += 1;
		if (key.alt)     parameter += 2;
		if (key.control) parameter += 4;
		return parameter;
	}

	static bool hasModifier(const KeyPress& key) {
		return modifierParameter(key) != 1;
	}

	static std::string csi(const std::string& body) {
		std::string bytes(1, kEsc);
		bytes += '[';
		bytes += body;
		return bytes;
	}

	static std::string ss3(char final_byte) {
		std::string bytes(1, kEsc);
		bytes += 'O';
		bytes += final_byte;
		return bytes;
	}

	static std::string encodeCursorKey(const KeyPress& key, const KeyModes& modes,
			char final_byte) {
		if (hasModifier(key)) {
			return csi("1;" + std::to_string(modifierParameter(key)) + final_byte);
		}

		if (modes.application_cursor) return ss3(final_byte);
		return csi(std::string(1, final_byte));
	}

	static std::string encodeNumberedKey(const KeyPress& key, int number) {
		const std::string prefix = std::to_string(number);
		if (hasModifier(key)) {
			return csi(prefix + ";" + std::to_string(modifierParameter(key)) + "~");
		}

		return csi(prefix + "~");
	}

	static std::string encodeFunctionKey(const KeyPress& key, char final_byte) {
		if (hasModifier(key)) {
			return csi("1;" + std::to_string(modifierParameter(key)) + final_byte);
		}

		return ss3(final_byte);
	}

	// Keys Windows would otherwise turn into a character, but which a
	// terminal spells differently once a modifier is held.
	// ConPTY follows the xterm convention, where DEL is Backspace and BS is
	// Ctrl+Backspace. Windows hands us BS for a plain press, so sending the
	// character message through unchanged arrives as Ctrl+Backspace and the
	// shell deletes a word, or nothing at all.
	static std::string encodeBackspace(const KeyPress& key) {
		const char erase = key.control ? '\x08' : '\x7f';
		if (!key.alt) return std::string(1, erase);

		std::string bytes(1, kEsc);
		bytes += erase;
		return bytes;
	}

	static std::string encodeModifiedTextKey(const KeyPress& key) {
		if (key.virtual_key == VK_SPACE && key.control && !key.alt) return std::string(1, '\0');
		if (key.virtual_key == VK_BACK) return encodeBackspace(key);

		if (key.virtual_key == VK_TAB && key.shift && !key.control && !key.alt) {
			return csi("Z");
		}

		return std::string();
	}

	std::string encodeKeyPress(const KeyPress& key, const KeyModes& modes) {
		for (const CursorKey& entry : kCursorKeys) {
			if (entry.virtual_key == key.virtual_key) {
				return encodeCursorKey(key, modes, entry.final_byte);
			}
		}

		for (const NumberedKey& entry : kNumberedKeys) {
			if (entry.virtual_key == key.virtual_key) return encodeNumberedKey(key, entry.number);
		}

		for (const CursorKey& entry : kFunctionKeys) {
			if (entry.virtual_key == key.virtual_key) {
				return encodeFunctionKey(key, entry.final_byte);
			}
		}

		return encodeModifiedTextKey(key);
	}

	std::string encodePaste(const std::string& text, const KeyModes& modes) {
		std::string body;
		for (std::size_t i = 0; i < text.size(); ++i) {
			if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;
			body.push_back(text[i] == '\n' ? '\r' : text[i]);
		}

		if (!modes.bracketed_paste) return body;

		std::string bytes = csi("200~");
		bytes += body;
		bytes += csi("201~");
		return bytes;
	}

} /* namespace wbshterm */

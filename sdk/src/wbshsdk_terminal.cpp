/**
 * @file wbshsdk_terminal.cpp
 * @brief The console side of the SDK: raw keys in, escape sequences out.
 *
 * None of this goes through the host. A terminal is the console device
 * the process is attached to, opened by name, so it is there even when
 * stdin and stdout are pipes and it is absent in a host that has no
 * console at all, which is what wbshterm.exe is.
 */

#include "wbshsdk.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif /* WIN32_LEAN_AND_MEAN */
#include <windows.h>

#include <io.h>

#include <cstring>
#include <new>

struct WbshTerminal {
	HANDLE input  = INVALID_HANDLE_VALUE;
	HANDLE output = INVALID_HANDLE_VALUE;
	DWORD  saved_input_mode  = 0;
	DWORD  saved_output_mode = 0;
};

namespace wbshsdk_terminal_detail {

	static HANDLE openConsoleDevice(const wchar_t* name) {
		return ::CreateFileW(name, GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	}

	static bool openDevices(WbshTerminal& terminal) {
		terminal.input  = openConsoleDevice(L"CONIN$");
		terminal.output = openConsoleDevice(L"CONOUT$");

		return terminal.input != INVALID_HANDLE_VALUE
			&& terminal.output != INVALID_HANDLE_VALUE;
	}

	static void closeDevices(WbshTerminal& terminal) {
		if (terminal.input != INVALID_HANDLE_VALUE) ::CloseHandle(terminal.input);
		if (terminal.output != INVALID_HANDLE_VALUE) ::CloseHandle(terminal.output);

		terminal.input  = INVALID_HANDLE_VALUE;
		terminal.output = INVALID_HANDLE_VALUE;
	}

	// Processed input is turned off so Ctrl+C reaches the util as a key
	// instead of ending the process; window input is turned on so a
	// resize arrives as a key too.
	static bool enterRawMode(WbshTerminal& terminal) {
		if (!::GetConsoleMode(terminal.input, &terminal.saved_input_mode)) return false;
		if (!::GetConsoleMode(terminal.output, &terminal.saved_output_mode)) return false;

		DWORD input_mode = terminal.saved_input_mode;
		input_mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT
			| ENABLE_MOUSE_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT);
		input_mode |= ENABLE_WINDOW_INPUT;

		const DWORD output_mode = terminal.saved_output_mode
			| ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT;

		return ::SetConsoleMode(terminal.input, input_mode) != 0
			&& ::SetConsoleMode(terminal.output, output_mode) != 0;
	}

	static void leaveRawMode(WbshTerminal& terminal) {
		::SetConsoleMode(terminal.input, terminal.saved_input_mode);
		::SetConsoleMode(terminal.output, terminal.saved_output_mode);
	}

	static HANDLE standardHandle(int fd) {
		const intptr_t from_crt = ::_get_osfhandle(fd);
		if (from_crt != -1 && from_crt != -2) return reinterpret_cast<HANDLE>(from_crt);

		switch (fd) {
		case 0:  return ::GetStdHandle(STD_INPUT_HANDLE);
		case 1:  return ::GetStdHandle(STD_OUTPUT_HANDLE);
		case 2:  return ::GetStdHandle(STD_ERROR_HANDLE);
		default: return INVALID_HANDLE_VALUE;
		}
	}

	static void clearKey(WbshKey& key) {
		key.kind = WBSH_KEY_NONE;
		key.modifiers = 0;
		std::memset(key.text, 0, sizeof(key.text));
	}

	static bool hasCtrl(DWORD state) {
		return (state & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
	}

	static bool hasAlt(DWORD state) {
		return (state & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
	}

	// AltGr shows up as Ctrl+Alt with the layout's character already
	// resolved; reading its Ctrl bit as a modifier would turn every
	// accented letter into a chord.
	static bool isAltGr(const KEY_EVENT_RECORD& event) {
		const DWORD both = LEFT_CTRL_PRESSED | RIGHT_ALT_PRESSED;
		return (event.dwControlKeyState & both) == both && event.uChar.UnicodeChar != 0;
	}

	static WbshKeyKind specialKeyKind(WORD virtual_key) {
		if (virtual_key >= VK_F1 && virtual_key <= VK_F12) {
			return static_cast<WbshKeyKind>(WBSH_KEY_F1 + (virtual_key - VK_F1));
		}

		switch (virtual_key) {
		case VK_RETURN: return WBSH_KEY_ENTER;
		case VK_ESCAPE: return WBSH_KEY_ESCAPE;
		case VK_BACK:   return WBSH_KEY_BACKSPACE;
		case VK_TAB:    return WBSH_KEY_TAB;
		case VK_DELETE: return WBSH_KEY_DELETE;
		case VK_INSERT: return WBSH_KEY_INSERT;
		case VK_UP:     return WBSH_KEY_UP;
		case VK_DOWN:   return WBSH_KEY_DOWN;
		case VK_LEFT:   return WBSH_KEY_LEFT;
		case VK_RIGHT:  return WBSH_KEY_RIGHT;
		case VK_HOME:   return WBSH_KEY_HOME;
		case VK_END:    return WBSH_KEY_END;
		case VK_PRIOR:  return WBSH_KEY_PAGE_UP;
		case VK_NEXT:   return WBSH_KEY_PAGE_DOWN;
		default:        return WBSH_KEY_NONE;
		}
	}

	static void encodeUtf8(const WCHAR* units, int count, WbshKey& out_key) {
		const int written = ::WideCharToMultiByte(CP_UTF8, 0, units, count,
			out_key.text, static_cast<int>(sizeof(out_key.text)) - 1, nullptr, nullptr);

		out_key.text[written > 0 ? written : 0] = '\0';
		out_key.kind = written > 0 ? WBSH_KEY_CHAR : WBSH_KEY_NONE;
	}

	// A character outside the BMP is delivered as two key-down records,
	// one per UTF-16 unit, so the second one is fetched before encoding.
	static void readCharacter(HANDLE input, WCHAR first, WbshKey& out_key) {
		WCHAR units[2] = { first, 0 };
		int count = 1;

		if (first >= 0xD800 && first <= 0xDBFF) {
			INPUT_RECORD next{};
			DWORD read = 0;
			if (::ReadConsoleInputW(input, &next, 1, &read) && read == 1
				&& next.EventType == KEY_EVENT && next.Event.KeyEvent.bKeyDown) {
				units[1] = next.Event.KeyEvent.uChar.UnicodeChar;
				count = 2;
			}
		}

		encodeUtf8(units, count, out_key);
	}

	// With Ctrl held the console reports a control code, not the letter,
	// so the letter comes back from the virtual key instead: Ctrl+C is
	// "c" with WBSH_MOD_CTRL, the way a util wants to spell it.
	static void readCtrlChord(WORD virtual_key, WbshKey& out_key) {
		const bool letter = virtual_key >= 'A' && virtual_key <= 'Z';
		const bool digit  = virtual_key >= '0' && virtual_key <= '9';
		if (!letter && !digit) return;

		out_key.text[0] = static_cast<char>(letter ? virtual_key - 'A' + 'a' : virtual_key);
		out_key.text[1] = '\0';
		out_key.kind = WBSH_KEY_CHAR;
	}

	static void translateKeyDown(HANDLE input, const KEY_EVENT_RECORD& event,
			WbshKey& out_key) {
		const bool altgr = isAltGr(event);
		const DWORD state = event.dwControlKeyState;

		out_key.kind = specialKeyKind(event.wVirtualKeyCode);
		if (out_key.kind != WBSH_KEY_NONE) {
			if ((state & SHIFT_PRESSED) != 0) out_key.modifiers |= WBSH_MOD_SHIFT;
			if (hasCtrl(state)) out_key.modifiers |= WBSH_MOD_CTRL;
			if (hasAlt(state)) out_key.modifiers |= WBSH_MOD_ALT;
			return;
		}

		if (!altgr && hasCtrl(state)) out_key.modifiers |= WBSH_MOD_CTRL;
		if (!altgr && hasAlt(state)) out_key.modifiers |= WBSH_MOD_ALT;

		if (event.uChar.UnicodeChar >= 0x20) {
			readCharacter(input, event.uChar.UnicodeChar, out_key);
			return;
		}

		if ((out_key.modifiers & WBSH_MOD_CTRL) != 0) readCtrlChord(event.wVirtualKeyCode, out_key);
	}

	// Alt+numpad input arrives as a key-up of the Alt key itself, carrying
	// the character it composed; every other key-up is silence.
	static void translateKeyUp(const KEY_EVENT_RECORD& event, WbshKey& out_key) {
		if (event.wVirtualKeyCode != VK_MENU) return;
		if (event.uChar.UnicodeChar < 0x20) return;

		const WCHAR unit = event.uChar.UnicodeChar;
		encodeUtf8(&unit, 1, out_key);
	}

	static void translateRecord(HANDLE input, const INPUT_RECORD& record, WbshKey& out_key) {
		clearKey(out_key);

		if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
			out_key.kind = WBSH_KEY_RESIZE;
			return;
		}

		if (record.EventType != KEY_EVENT) return;

		const KEY_EVENT_RECORD& event = record.Event.KeyEvent;
		if (event.bKeyDown) {
			translateKeyDown(input, event, out_key);
		} else {
			translateKeyUp(event, out_key);
		}
	}

	static DWORD remainingWait(int timeout_ms, ULONGLONG deadline) {
		if (timeout_ms < 0) return INFINITE;

		const ULONGLONG now = ::GetTickCount64();
		return now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
	}

	// Records that mean nothing to a util (key-ups, focus changes, mouse
	// noise) are swallowed here, so the timeout covers the whole wait
	// for something worth returning and not just the first record.
	static int waitForKey(WbshTerminal& terminal, WbshKey& out_key, int timeout_ms) {
		const ULONGLONG deadline = ::GetTickCount64()
			+ (timeout_ms > 0 ? static_cast<ULONGLONG>(timeout_ms) : 0);

		while (true) {
			const DWORD waited = ::WaitForSingleObject(terminal.input,
				remainingWait(timeout_ms, deadline));
			if (waited == WAIT_TIMEOUT) return 0;
			if (waited != WAIT_OBJECT_0) return WBSH_ERR_NO_TERMINAL;

			INPUT_RECORD record{};
			DWORD read = 0;
			if (!::ReadConsoleInputW(terminal.input, &record, 1, &read) || read == 0) {
				return WBSH_ERR_NO_TERMINAL;
			}

			translateRecord(terminal.input, record, out_key);
			if (out_key.kind != WBSH_KEY_NONE) return 1;
		}
	}

}  /* namespace wbshsdk_terminal_detail */

using namespace wbshsdk_terminal_detail;

extern "C" {

int wbshIsTerminal(int fd) {
	if (fd < 0 || fd > 2) return 0;

	const HANDLE handle = standardHandle(fd);
	if (handle == INVALID_HANDLE_VALUE || handle == nullptr) return 0;

	DWORD mode = 0;
	return ::GetConsoleMode(handle, &mode) != 0 ? 1 : 0;
}

WbshTerminal* wbshTerminalOpen(void) {
	WbshTerminal* terminal = new (std::nothrow) WbshTerminal();
	if (terminal == nullptr) return nullptr;

	if (!openDevices(*terminal) || !enterRawMode(*terminal)) {
		closeDevices(*terminal);
		delete terminal;
		return nullptr;
	}

	return terminal;
}

void wbshTerminalClose(WbshTerminal* terminal) {
	if (terminal == nullptr) return;

	leaveRawMode(*terminal);
	closeDevices(*terminal);
	delete terminal;
}

int wbshTerminalSize(WbshTerminal* terminal, int* out_columns, int* out_rows) {
	if (terminal == nullptr) return WBSH_ERR_NO_TERMINAL;

	CONSOLE_SCREEN_BUFFER_INFO info{};
	if (!::GetConsoleScreenBufferInfo(terminal->output, &info)) return WBSH_ERR_NO_TERMINAL;

	if (out_columns != nullptr) *out_columns = info.srWindow.Right - info.srWindow.Left + 1;
	if (out_rows != nullptr) *out_rows = info.srWindow.Bottom - info.srWindow.Top + 1;
	return WBSH_OK;
}

void wbshTerminalWrite(WbshTerminal* terminal, const char* bytes, size_t length) {
	if (terminal == nullptr || bytes == nullptr || length == 0) return;

	DWORD written = 0;
	::WriteFile(terminal->output, bytes, static_cast<DWORD>(length), &written, nullptr);
}

void wbshTerminalPrint(WbshTerminal* terminal, const char* text) {
	if (text == nullptr) return;

	wbshTerminalWrite(terminal, text, std::strlen(text));
}

int wbshTerminalReadKey(WbshTerminal* terminal, WbshKey* out_key, int timeout_ms) {
	if (out_key == nullptr) return WBSH_ERR_BAD_NAME;
	clearKey(*out_key);
	if (terminal == nullptr) return WBSH_ERR_NO_TERMINAL;

	return waitForKey(*terminal, *out_key, timeout_ms);
}

}  /* extern "C" */

/**
 * @file vi_terminal.cpp
 * @brief vi's terminal: the console in raw mode on the alternate screen,
 *        keys from console input records, or a plain byte stream when
 *        stdin is not a console (scripts and tests).
 *
 * The console is driven with VT sequences, so the same code serves the
 * classic console, Windows Terminal and wbshterm. Keys are taken from
 * input records rather than VT input: AltGr arrives there as Ctrl+Alt
 * with the layout's character, which is typed as that character.
 */

#include "vi_internal.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif /* _WIN32 */

#include <io.h>

#include <cstdio>
#include <string>

#include "consolekeys.h"
#include "coreutils_internal.h"
#include "stdinsync.h"
#include "utf8.h"

namespace wbsh {

	namespace vi_detail {

		static const char* const kEnterScreen = "\x1b[?1049h\x1b[H\x1b[2J";
		static const char* const kLeaveScreen = "\x1b[0 q\x1b[?1049l";
		static const DWORD kCtrlBits = LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED;
		static const DWORD kAltBits = LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED;

		static HANDLE inputHandle(const Editor& ed) {
			return static_cast<HANDLE>(ed.term.in);
		}

		static HANDLE outputHandle(const Editor& ed) {
			return static_cast<HANDLE>(ed.term.out);
		}

		void writeTerminal(const Editor& ed, const std::string& text) {
			if (ed.term.headless) return;

			DWORD written = 0;
			const DWORD length = static_cast<DWORD>(text.size());
			::WriteFile(outputHandle(ed), text.data(), length, &written, nullptr);
		}

		static void enterRawModes(const Editor& ed) {
			const DWORD cooked = ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT
				| ENABLE_MOUSE_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT;
			const DWORD in_mode = (ed.term.saved_in & ~cooked) | ENABLE_WINDOW_INPUT;
			::SetConsoleMode(inputHandle(ed), in_mode);
			const DWORD vt = ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT;
			::SetConsoleMode(outputHandle(ed), ed.term.saved_out | vt);
		}

		static void restoreModes(const Editor& ed) {
			::SetConsoleMode(inputHandle(ed), ed.term.saved_in);
			::SetConsoleMode(outputHandle(ed), ed.term.saved_out);
		}

		static HANDLE openConsoleFile(const wchar_t* name) {
			const DWORD access = GENERIC_READ | GENERIC_WRITE;
			const DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE;
			return ::CreateFileW(name, access, sharing, nullptr, OPEN_EXISTING, 0, nullptr);
		}

		static void closeHandles(Terminal& term) {
			if (term.in != nullptr) ::CloseHandle(static_cast<HANDLE>(term.in));
			if (term.out != nullptr) ::CloseHandle(static_cast<HANDLE>(term.out));
			term.in = nullptr;
			term.out = nullptr;
		}

		static bool openConsole(Terminal& term) {
			const HANDLE in = openConsoleFile(L"CONIN$");
			const HANDLE out = openConsoleFile(L"CONOUT$");
			term.in = in == INVALID_HANDLE_VALUE ? nullptr : in;
			term.out = out == INVALID_HANDLE_VALUE ? nullptr : out;

			DWORD in_mode = 0;
			DWORD out_mode = 0;
			const bool ok = term.in != nullptr && term.out != nullptr
				&& ::GetConsoleMode(in, &in_mode) != 0 && ::GetConsoleMode(out, &out_mode) != 0;
			if (!ok) {
				closeHandles(term);
				return false;
			}

			term.saved_in = in_mode;
			term.saved_out = out_mode;
			return true;
		}

		bool openTerminal(Editor& ed) {
			ed.term.headless = !isConsoleStream(stdin) || !openConsole(ed.term);
			if (ed.term.headless) return true;

			enterRawModes(ed);
			writeTerminal(ed, kEnterScreen);
			ed.term.screen_active = true;
			refreshTerminalSize(ed);
			return true;
		}

		void suspendTerminal(Editor& ed) {
			if (ed.term.headless || !ed.term.screen_active) return;

			writeTerminal(ed, kLeaveScreen);
			restoreModes(ed);
			ed.term.screen_active = false;
		}

		void resumeTerminal(Editor& ed) {
			if (ed.term.headless || ed.term.screen_active) return;

			enterRawModes(ed);
			writeTerminal(ed, kEnterScreen);
			ed.term.screen_active = true;
		}

		void closeTerminal(Editor& ed) {
			if (ed.term.headless) return;

			suspendTerminal(ed);
			closeHandles(ed.term);
			ed.term.headless = true;
		}

		void refreshTerminalSize(Editor& ed) {
			CONSOLE_SCREEN_BUFFER_INFO info{};
			if (ed.term.headless || ::GetConsoleScreenBufferInfo(outputHandle(ed), &info) == 0) {
				return;
			}

			const int columns = info.srWindow.Right - info.srWindow.Left + 1;
			const int rows = info.srWindow.Bottom - info.srWindow.Top + 1;
			if (columns > 0) ed.term.columns = static_cast<std::size_t>(columns);
			if (rows > 0) ed.term.rows = static_cast<std::size_t>(rows);
		}

		bool terminalInputQueued(const Editor& ed) {
			DWORD queued = 0;
			if (ed.term.headless) return false;

			return ::GetNumberOfConsoleInputEvents(inputHandle(ed), &queued) != 0 && queued > 0;
		}

		// Past what the CRT has already buffered, fd 0 is read a byte at a
		// time: a shell command that redirects fd 0 drops that buffer, and
		// with it the keys still to come.
		static int nextStdinByte(Terminal& term) {
			if (term.lookahead >= 0) {
				const int byte = term.lookahead;
				term.lookahead = -1;
				return byte;
			}

			if (stdinBufferedBytes() > 0) return std::fgetc(stdin);

			unsigned char byte = 0;
			return _read(_fileno(stdin), &byte, 1) == 1 ? byte : EOF;
		}

		static std::size_t sequenceLength(int lead) {
			if (lead >= 0xC2 && lead <= 0xDF) return 2;
			if (lead >= 0xE0 && lead <= 0xEF) return 3;
			if (lead >= 0xF0 && lead <= 0xF4) return 4;
			return 1;
		}

		static Key pipedCharacter(Terminal& term, int lead) {
			const std::size_t length = sequenceLength(lead);
			std::string bytes(1, static_cast<char>(lead));
			while (bytes.size() < length) {
				const int next = nextStdinByte(term);
				if (next == EOF || (next & 0xC0) != 0x80) {
					if (next != EOF) term.lookahead = next;
					break;
				}

				bytes.push_back(static_cast<char>(next));
			}

			if (bytes.size() < length || length == 1) return kKeyRawByte + static_cast<Key>(lead);
			return decodeUtf8(bytes, 0).code;
		}

		// CR, LF and CR LF are all Enter; DEL and BS both Backspace.
		static bool readPipedKey(Editor& ed, Key& key) {
			const int byte = nextStdinByte(ed.term);
			if (byte == EOF) return false;

			if (byte == '\r') {
				const int next = nextStdinByte(ed.term);
				if (next != '\n' && next != EOF) ed.term.lookahead = next;
				key = kKeyEnter;
			} else if (byte == '\n') {
				key = kKeyEnter;
			} else if (byte == 0x7F || byte == 0x08) {
				key = kKeyBackspace;
			} else {
				key = byte < 0x80 ? static_cast<Key>(byte) : pipedCharacter(ed.term, byte);
			}

			return true;
		}

		static Key cursorKey(WORD virtual_key) {
			switch (virtual_key) {
				case VK_UP:
					return kKeyUp;
				case VK_DOWN:
					return kKeyDown;
				case VK_LEFT:
					return kKeyLeft;
				case VK_RIGHT:
					return kKeyRight;
				case VK_HOME:
					return kKeyHome;
				case VK_END:
					return kKeyEnd;
				case VK_PRIOR:
					return kKeyPageUp;
				case VK_NEXT:
					return kKeyPageDown;
				case VK_INSERT:
					return kKeyInsert;
				case VK_DELETE:
					return kKeyDelete;
				default:
					return 0;
			}
		}

		static Key editingKey(WORD virtual_key) {
			switch (virtual_key) {
				case VK_ESCAPE:
					return kKeyEscape;
				case VK_RETURN:
					return kKeyEnter;
				case VK_BACK:
					return kKeyBackspace;
				case VK_TAB:
					return kKeyTab;
				default:
					return cursorKey(virtual_key);
			}
		}

		static Key characterKey(const Editor& ed, wchar_t unit) {
			const std::string text = readConsoleCharacter(ed.term.in, unit);
			if (text.empty()) return 0;

			return decodeUtf8(text, 0).code;
		}

		// Windows reports AltGr as Ctrl+Alt, so Ctrl+Alt with a printable
		// character is that character from the layout's third level. Alt
		// alone with a character is ESC and the character, as a terminal
		// sends it.
		static bool translateKey(Editor& ed, const KEY_EVENT_RECORD& record, Key& key) {
			const DWORD state = record.dwControlKeyState;
			const wchar_t unit = record.uChar.UnicodeChar;
			const bool printable = unit >= 0x20;
			const Key special = editingKey(record.wVirtualKeyCode);
			if (special != 0 && !printable) {
				key = special;
				return true;
			}

			if (unit == 0) return false;

			key = printable ? characterKey(ed, unit) : static_cast<Key>(unit);
			const bool alt_gr = (state & kCtrlBits) != 0 && (state & kAltBits) != 0;
			if (printable && (state & kAltBits) != 0 && !alt_gr) {
				ed.pending.push_back(key);
				key = kKeyEscape;
			}

			return key != 0;
		}

		static void queueRepeats(Editor& ed, const KEY_EVENT_RECORD& record, Key key) {
			for (WORD k = 1; k < record.wRepeatCount; ++k) ed.pending.push_back(key);
		}

		// Alt plus numpad digits delivers its character with the Alt release.
		static bool altCodeKey(const KEY_EVENT_RECORD& record, Key& key) {
			if (record.wVirtualKeyCode != VK_MENU || record.uChar.UnicodeChar < 0x20) return false;

			key = static_cast<Key>(record.uChar.UnicodeChar);
			return true;
		}

		static bool readConsoleKey(Editor& ed, Key& key) {
			for (;;) {
				INPUT_RECORD record;
				DWORD read = 0;
				if (::ReadConsoleInputW(inputHandle(ed), &record, 1, &read) == 0 || read == 0) {
					return false;
				}

				if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
					renderScreen(ed);
					continue;
				}

				if (record.EventType != KEY_EVENT) continue;

				const KEY_EVENT_RECORD& event = record.Event.KeyEvent;
				if (!event.bKeyDown) {
					if (altCodeKey(event, key)) return true;
					continue;
				}

				if (!translateKey(ed, event, key)) continue;
				queueRepeats(ed, event, key);
				return true;
			}
		}

		bool readTerminalKey(Editor& ed, Key& key) {
			if (ed.term.headless) return readPipedKey(ed, key);
			return readConsoleKey(ed, key);
		}

		// A `:` typed at the prompt starts the next command line, as in vim.
		void waitForKeyPress(Editor& ed) {
			if (ed.term.headless) return;

			enterRawModes(ed);
			Key key = 0;
			if (readConsoleKey(ed, key) && key == ':') ed.pending.push_back(key);
		}

	}  // namespace vi_detail

}  // namespace wbsh

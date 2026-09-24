/**
 * @file termreq.cpp
 * @brief Writing an OSC request to the console device, and escaping it.
 */

#include "termreq.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif /* _WIN32 */

namespace wbsh {

	std::string percentEncodeRequest(const std::string& text) {
		static const char* kHexDigits = "0123456789ABCDEF";

		std::string encoded;
		for (unsigned char letter : text) {
			const bool plain = letter >= 0x20 && letter != 0x7F && letter != '%'
				&& letter != ';';
			if (plain) {
				encoded.push_back(static_cast<char>(letter));
				continue;
			}

			encoded.push_back('%');
			encoded.push_back(kHexDigits[letter >> 4]);
			encoded.push_back(kHexDigits[letter & 0x0F]);
		}

		return encoded;
	}

#ifdef _WIN32

	// conhost only forwards a sequence it has no meaning for once something
	// else moves the screen along; without that the request sits in it until
	// the next write, which is long after it was needed. A save and restore
	// of the cursor is the cheapest nudge that leaves nothing behind.
	static const char* const kPassThroughNudge = "\x1b[s\x1b[u";

	// Virtual terminal processing is left on afterwards on purpose. conhost
	// forwards the sequence to the terminal on its own schedule, and putting
	// the old mode back straight after the write loses it on the way out.
	// Only a run under wbshterm gets here, where VT output is wanted anyway.
	void writeTerminalRequest(const std::string& request) {
		const HANDLE out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
		if (out == INVALID_HANDLE_VALUE) return;

		DWORD mode = 0;
		GetConsoleMode(out, &mode);
		SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT);

		const std::string bytes = request + kPassThroughNudge;

		DWORD wrote = 0;
		WriteFile(out, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr);

		CloseHandle(out);
	}

#else /* _WIN32 */

	void writeTerminalRequest(const std::string&) {
	}

#endif /* _WIN32 */

}  // namespace wbsh

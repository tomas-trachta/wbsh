/**
 * @file consolekeys.cpp
 * @brief Joining the two halves of a surrogate pair from console key records.
 */

#include "consolekeys.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif /* _WIN32 */

namespace wbsh {

#ifdef _WIN32

	namespace consolekeys_detail {

		// The low half is normally queued before the high half is read; the
		// wait only covers a pseudoconsole that writes the two separately.
		static const DWORD kPairWaitMs = 100;

		enum class PairStep {
			kTake,
			kSkip,
			kStop,
		};

		static bool isHighSurrogate(wchar_t unit) {
			return unit >= 0xD800 && unit <= 0xDBFF;
		}

		static bool isLowSurrogate(wchar_t unit) {
			return unit >= 0xDC00 && unit <= 0xDFFF;
		}

		static PairStep stepFor(const INPUT_RECORD& record) {
			if (record.EventType == FOCUS_EVENT || record.EventType == MENU_EVENT) {
				return PairStep::kSkip;
			}

			if (record.EventType != KEY_EVENT) return PairStep::kStop;

			const KEY_EVENT_RECORD& key = record.Event.KeyEvent;
			if (!key.bKeyDown) return PairStep::kSkip;
			return isLowSurrogate(key.uChar.UnicodeChar) ? PairStep::kTake : PairStep::kStop;
		}

		static bool peekRecord(HANDLE input, INPUT_RECORD& record) {
			DWORD count = 0;
			if (::PeekConsoleInputW(input, &record, 1, &count) && count == 1) return true;
			if (::WaitForSingleObject(input, kPairWaitMs) != WAIT_OBJECT_0) return false;

			return ::PeekConsoleInputW(input, &record, 1, &count) && count == 1;
		}

		static void dropRecord(HANDLE input) {
			INPUT_RECORD record;
			DWORD read = 0;
			::ReadConsoleInputW(input, &record, 1, &read);
		}

		static bool readLowSurrogate(HANDLE input, WCHAR& out_low) {
			INPUT_RECORD record;
			while (peekRecord(input, record)) {
				const PairStep step = stepFor(record);
				if (step == PairStep::kStop) return false;

				dropRecord(input);
				if (step == PairStep::kTake) {
					out_low = record.Event.KeyEvent.uChar.UnicodeChar;
					return true;
				}
			}

			return false;
		}

		static std::string utf8Of(const WCHAR* units, int count) {
			char bytes[8] = {};
			const int length = ::WideCharToMultiByte(CP_UTF8, 0, units, count, bytes,
				static_cast<int>(sizeof(bytes)), nullptr, nullptr);
			if (length <= 0) return std::string();
			return std::string(bytes, static_cast<std::size_t>(length));
		}

	} /* namespace consolekeys_detail */

	std::string readConsoleCharacter(void* input, wchar_t first) {
		WCHAR units[2] = { first, 0 };
		int count = 1;

		const bool pair = consolekeys_detail::isHighSurrogate(first)
			&& consolekeys_detail::readLowSurrogate(static_cast<HANDLE>(input), units[1]);
		if (pair) count = 2;

		return consolekeys_detail::utf8Of(units, count);
	}

#endif /* _WIN32 */

} /* namespace wbsh */

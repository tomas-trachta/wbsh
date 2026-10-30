/**
 * @file signals.cpp
 * @brief The signal name table.
 */

#include "signals.h"

#include <cctype>
#include <string>

#include "numparse.h"

namespace wbsh {

	static const char* const kSignalNames[] = {
		"HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "BUS", "FPE", "KILL", "USR1",
		"SEGV", "USR2", "PIPE", "ALRM", "TERM", "STKFLT", "CHLD", "CONT", "STOP", "TSTP",
		"TTIN", "TTOU", "URG", "XCPU", "XFSZ", "VTALRM", "PROF", "WINCH", "IO", "PWR",
		"SYS",
	};

	static const int kSignalCount =
		static_cast<int>(sizeof(kSignalNames) / sizeof(kSignalNames[0]));

	int signalCount() {
		return kSignalCount;
	}

	const char* signalName(int number) {
		if (number < 1 || number > kSignalCount) return nullptr;
		return kSignalNames[number - 1];
	}

	static std::string upperAscii(std::string text) {
		for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		return text;
	}

	bool parseSignalSpec(const std::string& text, int& number) {
		int parsed = 0;
		std::size_t used = 0;
		if (parseInt(text, parsed, 10, &used) && used == text.size()) {
			if (parsed < 0 || parsed > kSignalCount) return false;
			number = parsed;
			return true;
		}

		std::string name = upperAscii(text);
		if (name.size() > 3 && name.compare(0, 3, "SIG") == 0) name = name.substr(3);
		for (int candidate = 1; candidate <= kSignalCount; ++candidate) {
			if (name != kSignalNames[candidate - 1]) continue;

			number = candidate;
			return true;
		}

		return false;
	}

}  // namespace wbsh

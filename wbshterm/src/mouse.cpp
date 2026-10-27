/**
 * @file mouse.cpp
 * @brief Mouse arithmetic kept apart from the window: pixels to DIPs, wheel deltas to
 *        lines, and the reports a program that asked for the mouse is sent.
 */

#include "mouse.h"

#include "utf8.h"

#include <windows.h>

namespace wbshterm {

	float pixelsToDips(int pixels, unsigned int dpi) {
		const unsigned int effective = dpi != 0 ? dpi : USER_DEFAULT_SCREEN_DPI;
		return static_cast<float>(pixels) * static_cast<float>(USER_DEFAULT_SCREEN_DPI)
			/ static_cast<float>(effective);
	}

	static bool oppositeSigns(int left, int right) {
		return (left < 0 && right > 0) || (left > 0 && right < 0);
	}

	int WheelAccumulator::linesFor(int delta, int lines_per_notch) {
		if (lines_per_notch <= 0) {
			remainder_ = 0;
			return 0;
		}

		if (oppositeSigns(delta, remainder_)) remainder_ = 0;

		const int total = remainder_ + delta * lines_per_notch;
		const int lines = total / WHEEL_DELTA;
		remainder_ = total - lines * WHEEL_DELTA;
		return lines;
	}

	namespace mouse_detail {

		static const int kMotionBit  = 32;
		static const int kShiftBit   = 4;
		static const int kAltBit     = 8;
		static const int kControlBit = 16;
		static const int kReleased   = 3;
		static const int kOffset     = 32;
		static const int kByteLimit  = 255;
		static const int kUtf8Limit  = 2047;

		static bool isWheel(MouseButton button) {
			return button == MouseButton::kWheelUp || button == MouseButton::kWheelDown
				|| button == MouseButton::kWheelLeft || button == MouseButton::kWheelRight;
		}

		static int buttonNumber(MouseButton button) {
			switch (button) {
			case MouseButton::kLeft:
				return 0;
			case MouseButton::kMiddle:
				return 1;
			case MouseButton::kRight:
				return 2;
			case MouseButton::kWheelUp:
				return 64;
			case MouseButton::kWheelDown:
				return 65;
			case MouseButton::kWheelLeft:
				return 66;
			case MouseButton::kWheelRight:
				return 67;
			case MouseButton::kNone:
				return kReleased;
			default:
				return kReleased;
			}
		}

		static int modifierBits(const MouseReport& report) {
			int bits = 0;
			if (report.shift) bits |= kShiftBit;
			if (report.alt) bits |= kAltBit;
			if (report.control) bits |= kControlBit;
			return bits;
		}

		// Only SGR can say which button came up; the older encodings send
		// "released" and leave the program to remember which one was down.
		static int buttonCode(const MouseModes& modes, const MouseReport& report) {
			const bool sgr = modes.encoding == MouseEncoding::kSgr;
			int code = buttonNumber(report.button);
			if (report.action == MouseAction::kRelease && !sgr) code = kReleased;
			if (report.action == MouseAction::kMotion) code += kMotionBit;
			if (modes.tracking != MouseTracking::kX10) code |= modifierBits(report);
			return code;
		}

		static std::string encodeSgr(int code, const MouseReport& report) {
			std::string bytes = "\x1b[<" + std::to_string(code);
			bytes += ";" + std::to_string(report.column + 1);
			bytes += ";" + std::to_string(report.row + 1);
			bytes += report.action == MouseAction::kRelease ? "m" : "M";
			return bytes;
		}

		static void appendValue(std::string& bytes, int value, bool utf8) {
			if (utf8) appendUtf8(bytes, static_cast<char32_t>(value));
			else bytes.push_back(static_cast<char>(static_cast<unsigned char>(value)));
		}

		// X10 bytes stop at column 223, UTF-8 at 2015; past them the cell
		// has no spelling, and xterm sends nothing rather than a wrong one.
		static std::string encodeLegacy(int code, const MouseReport& report, bool utf8) {
			const int limit = utf8 ? kUtf8Limit : kByteLimit;
			const int column = report.column + 1 + kOffset;
			const int row    = report.row + 1 + kOffset;
			if (column > limit || row > limit) return std::string();

			std::string bytes = "\x1b[M";
			appendValue(bytes, code + kOffset, utf8);
			appendValue(bytes, column, utf8);
			appendValue(bytes, row, utf8);
			return bytes;
		}

	} /* namespace mouse_detail */

	bool mouseTrackingWants(MouseTracking tracking, const MouseReport& report) {
		const bool wheel = mouse_detail::isWheel(report.button);
		if (wheel && report.action != MouseAction::kPress) return false;

		switch (tracking) {
		case MouseTracking::kX10:
			return report.action == MouseAction::kPress;
		case MouseTracking::kClicks:
			return report.action != MouseAction::kMotion;
		case MouseTracking::kDrags:
			return report.action != MouseAction::kMotion || report.button != MouseButton::kNone;
		case MouseTracking::kMotion:
			return true;
		case MouseTracking::kOff:
			return false;
		default:
			return false;
		}
	}

	std::string encodeMouseReport(const MouseModes& modes, const MouseReport& report) {
		if (report.column < 0 || report.row < 0) return std::string();

		const int code = mouse_detail::buttonCode(modes, report);
		switch (modes.encoding) {
		case MouseEncoding::kSgr:
			return mouse_detail::encodeSgr(code, report);
		case MouseEncoding::kUtf8:
			return mouse_detail::encodeLegacy(code, report, true);
		case MouseEncoding::kDefault:
			return mouse_detail::encodeLegacy(code, report, false);
		default:
			return std::string();
		}
	}

} /* namespace wbshterm */

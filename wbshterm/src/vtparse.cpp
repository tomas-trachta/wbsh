/**
 * @file vtparse.cpp
 * @brief The state machine transitions, one method per state.
 */

#include "vtparse.h"

namespace wbshterm {

	static const char32_t kReplacementChar = 0xFFFD;

	int VtSequence::param(std::size_t index, int fallback) const {
		if (index >= params.size()) return fallback;
		return params[index] < 0 ? fallback : params[index];
	}

	static bool isControl(unsigned char byte) {
		return byte < 0x20 || byte == 0x7F;
	}

	static bool isIntermediate(unsigned char byte) {
		return byte >= 0x20 && byte <= 0x2F;
	}

	static bool isFinal(unsigned char byte) {
		return byte >= 0x40 && byte <= 0x7E;
	}

	static bool isParamByte(unsigned char byte) {
		return byte >= 0x30 && byte <= 0x3F;
	}

	void VtParser::consume(const char* data, std::size_t length) {
		for (std::size_t i = 0; i < length; ++i) {
			step(static_cast<unsigned char>(data[i]));
		}
	}

	void VtParser::step(unsigned char byte) {
		switch (state_) {
		case State::Ground:             stepGround(byte); break;
		case State::Escape:             stepEscape(byte); break;
		case State::EscapeIntermediate: stepEscapeIntermediate(byte); break;
		case State::CsiEntry:           stepCsiEntry(byte); break;
		case State::CsiParam:           stepCsiParam(byte); break;
		case State::CsiIntermediate:    stepCsiIntermediate(byte); break;
		case State::CsiIgnore:          stepCsiIgnore(byte); break;
		case State::OscString:          stepOscString(byte); break;
		case State::StringIgnore:       stepStringIgnore(byte); break;
		default:                        state_ = State::Ground; break;
		}
	}

	// ESC and CAN/SUB abandon whatever sequence is in flight, from any
	// state. Returning true means the byte was consumed here.
	bool VtParser::handleSharedControl(unsigned char byte) {
		if (byte == 0x1B) {
			beginSequence();
			state_ = State::Escape;
			return true;
		}

		if (byte == 0x18 || byte == 0x1A) {
			state_ = State::Ground;
			return true;
		}

		return false;
	}

	void VtParser::beginSequence() {
		sequence_.params.clear();
		sequence_.intermediates.clear();
		sequence_.private_byte = 0;
		sequence_.final_byte = 0;
		osc_.clear();
		resetUtf8();
	}

	void VtParser::resetUtf8() {
		utf8_code_ = 0;
		utf8_pending_ = 0;
	}

	void VtParser::printUtf8Byte(unsigned char byte) {
		if (utf8_pending_ > 0) {
			if ((byte & 0xC0) != 0x80) {
				sink_.vtPrint(kReplacementChar);
				resetUtf8();
				printUtf8Byte(byte);
				return;
			}

			utf8_code_ = (utf8_code_ << 6) | static_cast<char32_t>(byte & 0x3F);
			if (--utf8_pending_ == 0) sink_.vtPrint(utf8_code_);
			return;
		}

		if (byte < 0x80) {
			sink_.vtPrint(byte);
			return;
		}

		if ((byte & 0xE0) == 0xC0) {
			utf8_code_ = byte & 0x1F;
			utf8_pending_ = 1;
			return;
		}

		if ((byte & 0xF0) == 0xE0) {
			utf8_code_ = byte & 0x0F;
			utf8_pending_ = 2;
			return;
		}

		if ((byte & 0xF8) == 0xF0) {
			utf8_code_ = byte & 0x07;
			utf8_pending_ = 3;
			return;
		}

		sink_.vtPrint(kReplacementChar);
	}

	void VtParser::stepGround(unsigned char byte) {
		if (handleSharedControl(byte)) return;

		if (isControl(byte)) {
			resetUtf8();
			sink_.vtExecute(byte);
			return;
		}

		printUtf8Byte(byte);
	}

	void VtParser::stepEscape(unsigned char byte) {
		if (handleSharedControl(byte)) return;

		if (byte == '[') {
			state_ = State::CsiEntry;
			return;
		}

		if (byte == ']') {
			state_ = State::OscString;
			return;
		}

		if (byte == 'P' || byte == 'X' || byte == '^' || byte == '_') {
			state_ = State::StringIgnore;
			return;
		}

		if (isIntermediate(byte)) {
			sequence_.intermediates.push_back(static_cast<char>(byte));
			state_ = State::EscapeIntermediate;
			return;
		}

		if (isControl(byte)) {
			sink_.vtExecute(byte);
			return;
		}

		sequence_.final_byte = static_cast<char>(byte);
		sink_.vtEsc(sequence_);
		state_ = State::Ground;
	}

	void VtParser::stepEscapeIntermediate(unsigned char byte) {
		if (handleSharedControl(byte)) return;

		if (isIntermediate(byte)) {
			sequence_.intermediates.push_back(static_cast<char>(byte));
			return;
		}

		if (isControl(byte)) {
			sink_.vtExecute(byte);
			return;
		}

		sequence_.final_byte = static_cast<char>(byte);
		sink_.vtEsc(sequence_);
		state_ = State::Ground;
	}

	void VtParser::pushParamDigit(unsigned char byte) {
		if (sequence_.params.empty()) sequence_.params.push_back(-1);
		if (sequence_.params.size() > kMaxParams) return;

		int& value = sequence_.params.back();
		if (value < 0) value = 0;
		if (value < 100000) value = value * 10 + (byte - '0');
	}

	void VtParser::pushParamSeparator() {
		if (sequence_.params.empty()) sequence_.params.push_back(-1);
		if (sequence_.params.size() <= kMaxParams) sequence_.params.push_back(-1);
	}

	void VtParser::stepCsiEntry(unsigned char byte) {
		if (handleSharedControl(byte)) return;

		if (isControl(byte)) {
			sink_.vtExecute(byte);
			return;
		}

		if (byte >= 0x3C && byte <= 0x3F) {
			sequence_.private_byte = static_cast<char>(byte);
			state_ = State::CsiParam;
			return;
		}

		if (isParamByte(byte)) {
			state_ = State::CsiParam;
			stepCsiParam(byte);
			return;
		}

		if (isIntermediate(byte)) {
			sequence_.intermediates.push_back(static_cast<char>(byte));
			state_ = State::CsiIntermediate;
			return;
		}

		sequence_.final_byte = static_cast<char>(byte);
		sink_.vtCsi(sequence_);
		state_ = State::Ground;
	}

	void VtParser::stepCsiParam(unsigned char byte) {
		if (handleSharedControl(byte)) return;

		if (isControl(byte)) {
			sink_.vtExecute(byte);
			return;
		}

		if (byte >= '0' && byte <= '9') {
			pushParamDigit(byte);
			return;
		}

		if (byte == ';' || byte == ':') {
			pushParamSeparator();
			return;
		}

		if (byte >= 0x3C && byte <= 0x3F) {
			state_ = State::CsiIgnore;
			return;
		}

		if (isIntermediate(byte)) {
			sequence_.intermediates.push_back(static_cast<char>(byte));
			state_ = State::CsiIntermediate;
			return;
		}

		sequence_.final_byte = static_cast<char>(byte);
		sink_.vtCsi(sequence_);
		state_ = State::Ground;
	}

	void VtParser::stepCsiIntermediate(unsigned char byte) {
		if (handleSharedControl(byte)) return;

		if (isControl(byte)) {
			sink_.vtExecute(byte);
			return;
		}

		if (isIntermediate(byte)) {
			sequence_.intermediates.push_back(static_cast<char>(byte));
			return;
		}

		if (isParamByte(byte)) {
			state_ = State::CsiIgnore;
			return;
		}

		sequence_.final_byte = static_cast<char>(byte);
		sink_.vtCsi(sequence_);
		state_ = State::Ground;
	}

	void VtParser::stepCsiIgnore(unsigned char byte) {
		if (handleSharedControl(byte)) return;
		if (isFinal(byte)) state_ = State::Ground;
	}

	// Terminated by BEL or by ST (ESC \), which arrives here as ESC and is
	// handled by the shared control path.
	void VtParser::stepOscString(unsigned char byte) {
		if (byte == 0x07) {
			sink_.vtOsc(osc_);
			state_ = State::Ground;
			return;
		}

		if (byte == 0x1B) {
			sink_.vtOsc(osc_);
			beginSequence();
			state_ = State::Escape;
			return;
		}

		if (byte == 0x18 || byte == 0x1A) {
			state_ = State::Ground;
			return;
		}

		if (byte >= 0x20 && osc_.size() < 4096) osc_.push_back(static_cast<char>(byte));
	}

	void VtParser::stepStringIgnore(unsigned char byte) {
		if (byte == 0x07) {
			state_ = State::Ground;
			return;
		}

		if (byte == 0x1B) {
			beginSequence();
			state_ = State::Escape;
		}
	}

} /* namespace wbshterm */

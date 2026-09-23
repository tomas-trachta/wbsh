#pragma once

/**
 * @file vtparse.h
 * @brief Byte stream to VT actions, as a DEC-compatible state machine.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wbshterm {

	static const std::size_t kMaxParams = 16;

	/** A parsed escape sequence. Missing parameters are -1, not 0. */
	struct VtSequence {
		std::vector<int> params;
		std::string      intermediates;
		char             private_byte = 0;
		char             final_byte   = 0;

		int param(std::size_t index, int fallback) const;
		int paramCount() const { return static_cast<int>(params.size()); }
	};

	/** Where a terminal's answers to queries go: back to the shell. */
	class VtResponder {
	public:
		virtual ~VtResponder() = default;

		virtual void vtRespond(const std::string& bytes) = 0;
	};

	class VtSink {
	public:
		virtual ~VtSink() = default;

		virtual void vtPrint(char32_t code) = 0;
		virtual void vtExecute(unsigned char control) = 0;
		virtual void vtCsi(const VtSequence& sequence) = 0;
		virtual void vtEsc(const VtSequence& sequence) = 0;
		virtual void vtOsc(const std::string& text) = 0;
	};

	/**
	 * @brief Feeds bytes to a sink as print / control / sequence actions.
	 *
	 * Holds parse state across consume() calls, so a sequence split over
	 * two reads behaves the same as one delivered whole. Malformed input
	 * is discarded rather than reported: a terminal keeps going.
	 */
	class VtParser {
	public:
		explicit VtParser(VtSink& sink) : sink_(sink) {}

		void consume(const char* data, std::size_t length);

	private:
		enum class State {
			Ground,
			Escape,
			EscapeIntermediate,
			CsiEntry,
			CsiParam,
			CsiIntermediate,
			CsiIgnore,
			OscString,
			StringIgnore,
		};

		void step(unsigned char byte);
		void stepGround(unsigned char byte);
		void stepEscape(unsigned char byte);
		void stepEscapeIntermediate(unsigned char byte);
		void stepCsiEntry(unsigned char byte);
		void stepCsiParam(unsigned char byte);
		void stepCsiIntermediate(unsigned char byte);
		void stepCsiIgnore(unsigned char byte);
		void stepOscString(unsigned char byte);
		void stepStringIgnore(unsigned char byte);

		void beginSequence();
		void pushParamDigit(unsigned char byte);
		void pushParamSeparator();
		bool handleSharedControl(unsigned char byte);
		void printUtf8Byte(unsigned char byte);
		void resetUtf8();

		VtSink&     sink_;
		State       state_ = State::Ground;
		VtSequence  sequence_;
		std::string osc_;

		char32_t utf8_code_    = 0;
		int      utf8_pending_ = 0;
	};

} /* namespace wbshterm */

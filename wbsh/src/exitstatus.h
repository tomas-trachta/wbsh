#pragma once

/**
 * @file exitstatus.h
 * @brief Map process exit codes and `exit` / `return` operands into `$?`'s 0..255.
 *
 * A Windows exit code is a full 32-bit value. A process that dies from an
 * unhandled exception or Ctrl+C reports the NTSTATUS code, which is the
 * Windows counterpart of a POSIX death by signal. Those codes become
 * 128 + signal number, the way bash reports a signalled child. Every
 * other code keeps its low byte, like a POSIX wait status.
 */

#include <cstdint>

namespace wbsh {

	namespace exitstatus_detail {

		struct SignalStatus {
			std::uint32_t ntstatus;
			int           signal;
		};

		constexpr int kSigInt  = 2;
		constexpr int kSigIll  = 4;
		constexpr int kSigAbrt = 6;
		constexpr int kSigBus  = 7;
		constexpr int kSigFpe  = 8;
		constexpr int kSigSegv = 11;

		constexpr SignalStatus kSignalStatuses[] = {
			{ 0xC000013Au, kSigInt  },  // STATUS_CONTROL_C_EXIT
			{ 0xC0000005u, kSigSegv },  // STATUS_ACCESS_VIOLATION
			{ 0xC00000FDu, kSigSegv },  // STATUS_STACK_OVERFLOW
			{ 0xC0000006u, kSigBus  },  // STATUS_IN_PAGE_ERROR
			{ 0xC000001Du, kSigIll  },  // STATUS_ILLEGAL_INSTRUCTION
			{ 0xC0000096u, kSigIll  },  // STATUS_PRIVILEGED_INSTRUCTION
			{ 0xC0000094u, kSigFpe  },  // STATUS_INTEGER_DIVIDE_BY_ZERO
			{ 0xC0000095u, kSigFpe  },  // STATUS_INTEGER_OVERFLOW
			{ 0xC000008Eu, kSigFpe  },  // STATUS_FLOAT_DIVIDE_BY_ZERO
			{ 0xC0000409u, kSigAbrt },  // STATUS_STACK_BUFFER_OVERRUN (__fastfail, abort)
		};

	}  // namespace exitstatus_detail

	constexpr int kSignalStatusBase = 128;

	/** What `$?` shows for a command killed by SIGPIPE, which wbsh imitates. */
	constexpr int kSigPipeStatus = kSignalStatusBase + 13;

	/** `exit N` / `return N`: only the low 8 bits survive, so -1 is 255. */
	inline int shellStatusFromInt(long long value) {
		return static_cast<int>(static_cast<unsigned long long>(value) & 0xFFu);
	}

	/** A finished child's GetExitCodeProcess value as `$?` sees it. */
	inline int shellStatusFromExitCode(std::uint32_t code) {
		for (const exitstatus_detail::SignalStatus& entry : exitstatus_detail::kSignalStatuses) {
			if (entry.ntstatus == code) return kSignalStatusBase + entry.signal;
		}

		return static_cast<int>(code & 0xFFu);
	}

}  // namespace wbsh

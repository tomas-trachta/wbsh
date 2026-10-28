/**
 * @file builtins_ulimit.cpp
 * @brief The `ulimit` builtin.
 *
 * Windows has no per-process resource limits for most of what `ulimit`
 * reports. Open files (the C runtime's stream limit) and the stack size
 * are real; pipe size is the default pipe buffer; everything else reads
 * `unlimited` (or 0 for the priorities) and a new value is remembered and
 * shown again, so scripts that set and check limits keep working.
 */

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "executor.h"
#include "numparse.h"

namespace wbsh::ulimit_detail {

	static const long long kUnlimited = -1;
	static const long long kFromProcess = -2;
	static const long long kMaxOpenStreams = 8192;
	static const long long kPipeBlocks = 8;

	struct Limit {
		char option;
		const char* description;
		const char* units;
		long long soft;
		long long hard;
		bool fixed;
	};

	// bash's order and wording; `units` is empty for a plain count.
	static Limit g_limits[] = {
		{ 'c', "core file size",       "blocks",    kUnlimited, kUnlimited, false },
		{ 'd', "data seg size",        "kbytes",    kUnlimited, kUnlimited, false },
		{ 'e', "scheduling priority",  "",          0,          kUnlimited, false },
		{ 'f', "file size",            "blocks",    kUnlimited, kUnlimited, false },
		{ 'i', "pending signals",      "",          kUnlimited, kUnlimited, false },
		{ 'l', "max locked memory",    "kbytes",    kUnlimited, kUnlimited, false },
		{ 'm', "max memory size",      "kbytes",    kUnlimited, kUnlimited, false },
		{ 'n', "open files",           "",          0,          kMaxOpenStreams, false },
		{ 'p', "pipe size",            "512 bytes", kPipeBlocks, kPipeBlocks, true },
		{ 'q', "POSIX message queues", "bytes",     kUnlimited, kUnlimited, false },
		{ 'r', "real-time priority",   "",          0,          kUnlimited, false },
		{ 's', "stack size",           "kbytes",    kFromProcess, kUnlimited, false },
		{ 't', "cpu time",             "seconds",   kUnlimited, kUnlimited, false },
		{ 'u', "max user processes",   "",          kUnlimited, kUnlimited, false },
		{ 'v', "virtual memory",       "kbytes",    kUnlimited, kUnlimited, false },
		{ 'x', "file locks",           "",          kUnlimited, kUnlimited, false },
	};

	struct Request {
		bool soft = false;
		bool hard = false;
		bool all = false;
		std::vector<Limit*> limits;
		std::string value;
		bool has_value = false;
	};

	static Limit* findLimit(char option) {
		for (Limit& limit : g_limits) {
			if (limit.option == option) return &limit;
		}

		return nullptr;
	}

	static long long stackKbytes() {
#ifdef _WIN32
		ULONG_PTR low = 0;
		ULONG_PTR high = 0;
		::GetCurrentThreadStackLimits(&low, &high);
		return static_cast<long long>((high - low) / 1024);
#else
		return kUnlimited;
#endif
	}

	// Open files and the stack size are read from the process each time.
	static long long softValue(const Limit& limit) {
		if (limit.option == 'n') return _getmaxstdio();
		if (limit.soft == kFromProcess) return stackKbytes();
		return limit.soft;
	}

	static long long hardValue(const Limit& limit) {
		return limit.hard;
	}

	static void printUsage() {
		std::fprintf(stderr, "ulimit: usage: ulimit [-SHacdefilmnpqrstuvx] [limit]\n");
	}

	static bool applyOptionLetter(char letter, Request& request) {
		if (letter == 'S') request.soft = true;
		else if (letter == 'H') request.hard = true;
		else if (letter == 'a') request.all = true;
		else if (Limit* limit = findLimit(letter)) request.limits.push_back(limit);
		else return false;

		return true;
	}

	static bool parseOptionWord(const std::string& word, Request& request) {
		for (std::size_t k = 1; k < word.size(); ++k) {
			if (applyOptionLetter(word[k], request)) continue;

			std::fprintf(stderr, "wbsh: ulimit: -%c: invalid option\n", word[k]);
			printUsage();
			return false;
		}

		return true;
	}

	static bool parseRequest(const std::vector<std::string>& args, Request& request) {
		std::size_t i = 0;
		while (i < args.size() && args[i].size() > 1 && args[i][0] == '-') {
			const std::string& word = args[i++];
			if (word == "--") break;
			if (!parseOptionWord(word, request)) return false;
		}

		if (i < args.size()) {
			request.value = args[i];
			request.has_value = true;
		}

		if (request.limits.empty() && !request.all) request.limits.push_back(findLimit('f'));
		return true;
	}

	static std::string valueText(long long value) {
		return value == kUnlimited ? "unlimited" : std::to_string(value);
	}

	static std::string unitsLabel(const Limit& limit) {
		if (limit.units[0] == '\0') return "(-" + std::string(1, limit.option) + ") ";
		return "(" + std::string(limit.units) + ", -" + std::string(1, limit.option) + ") ";
	}

	static void printLimit(const Limit& limit, bool hard, bool labelled) {
		const std::string value = valueText(hard ? hardValue(limit) : softValue(limit));
		if (!labelled) {
			std::printf("%s\n", value.c_str());
			return;
		}

		std::printf("%-20s %16s%s\n", limit.description, unitsLabel(limit).c_str(), value.c_str());
	}

	static int printLimits(const Request& request) {
		const bool hard = request.hard && !request.soft;
		if (request.all) {
			for (const Limit& limit : g_limits) printLimit(limit, hard, true);
			return 0;
		}

		const bool labelled = request.limits.size() > 1;
		for (const Limit* limit : request.limits) printLimit(*limit, hard, labelled);
		return 0;
	}

	static bool parseLimitValue(const Limit& limit, const std::string& text, long long& value) {
		if (text == "unlimited") value = kUnlimited;
		else if (text == "hard") value = hardValue(limit);
		else if (text == "soft") value = softValue(limit);
		else if (!parseLL(text, value) || value < 0) {
			std::fprintf(stderr, "wbsh: ulimit: %s: invalid number\n", text.c_str());
			return false;
		}

		return true;
	}

	static bool exceeds(long long value, long long ceiling) {
		if (ceiling == kUnlimited) return false;
		return value == kUnlimited || value > ceiling;
	}

	static int refuseChange(const Limit& limit, const char* reason) {
		std::fprintf(stderr, "wbsh: ulimit: %s: cannot modify limit: %s\n",
			limit.description, reason);
		return 1;
	}

	static bool setOpenStreams(long long value) {
		const long long wanted = value == kUnlimited ? kMaxOpenStreams : value;
		return _setmaxstdio(static_cast<int>(wanted)) != -1;
	}

	// A hard limit can only go down, and the soft limit stays at or below it.
	static const char* changeRefusal(const Limit& limit, long long value, bool soft, bool hard) {
		if (limit.fixed) return "Invalid argument";
		if (hard && exceeds(value, limit.hard)) return "Operation not permitted";
		if (soft && exceeds(value, limit.hard)) return "Invalid argument";
		if (!soft && exceeds(softValue(limit), value)) return "Invalid argument";
		if (limit.option == 'n' && soft && !setOpenStreams(value)) return "Invalid argument";
		return nullptr;
	}

	static int setLimit(Limit& limit, long long value, bool soft, bool hard) {
		const char* refusal = changeRefusal(limit, value, soft, hard);
		if (refusal != nullptr) return refuseChange(limit, refusal);

		if (hard) limit.hard = value;
		if (soft && limit.option != 'n') limit.soft = value;
		return 0;
	}

	// Without -S or -H a new value sets both limits, as in bash.
	static int setLimits(const Request& request) {
		const bool soft = request.soft || !request.hard;
		const bool hard = request.hard || !request.soft;
		int status = 0;
		for (Limit* limit : request.limits) {
			long long value = 0;
			if (!parseLimitValue(*limit, request.value, value)) return 1;
			if (setLimit(*limit, value, soft, hard) != 0) status = 1;
		}

		return status;
	}

	static int builtin_ulimit(Executor&, const std::vector<std::string>& args) {
		Request request;
		if (!parseRequest(args, request)) return 2;
		if (!request.has_value || request.all) return printLimits(request);

		return setLimits(request);
	}

}  // namespace wbsh::ulimit_detail

namespace wbsh {

	void registerUlimitBuiltin(Executor& exec) {
		exec.registerBuiltin("ulimit", ulimit_detail::builtin_ulimit);
	}

}  // namespace wbsh

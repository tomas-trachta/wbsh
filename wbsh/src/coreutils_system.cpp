/**
 * @file coreutils_system.cpp
 * @brief Small system and terminal builtins: printenv, logname, groups,
 *        usleep, factor, uuidgen, cal, ascii, getopt, stty, reset,
 *        ttysize.
 */

#include "coreutils_internal.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <bcrypt.h>
#  pragma comment(lib, "bcrypt.lib")
#endif /* _WIN32 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "environment.h"
#include "executor.h"
#include "numparse.h"

namespace wbsh {

	namespace system_detail {

		enum class GetoptArg {
			None,
			Required,
			Optional,
		};

		struct LongOption {
			std::string name;
			GetoptArg arg = GetoptArg::None;
		};

		struct GetoptSpec {
			std::string name = "getopt";
			std::string shorts;
			std::vector<LongOption> longs;
			bool quiet = false;
			bool quiet_output = false;
			bool alternative = false;
			bool quote = true;
			bool test_only = false;
			std::vector<std::string> params;
		};

		struct GetoptOutput {
			std::string options;
			std::string operands;
			bool failed = false;
		};

		struct CalendarRequest {
			int year = 0;
			int month = 0;
			bool whole_year = false;
		};

	}  // namespace system_detail

	using system_detail::CalendarRequest;
	using system_detail::GetoptArg;
	using system_detail::GetoptOutput;
	using system_detail::GetoptSpec;
	using system_detail::LongOption;

	static const int kGetoptParseError  = 1;
	static const int kGetoptUsageError  = 2;
	static const int kGetoptEnhanced    = 4;
	static const int kCalendarWidth     = 20;
	static const int kCalendarGap       = 2;
	static const int kCalendarWeekRows  = 6;
	static const int kMonthsPerRow      = 3;
	static const int kFallbackColumns   = 80;
	static const int kFallbackRows      = 24;
	static const int kAsciiColumns      = 8;
	static const int kAsciiRows         = 16;

	static const char* const kMonthNames[] = {
		"January", "February", "March", "April", "May", "June", "July",
		"August", "September", "October", "November", "December",
	};

	static const char* const kControlNames[] = {
		"NUL", "SOH", "STX", "ETX", "EOT", "ENQ", "ACK", "BEL",
		"BS",  "HT",  "LF",  "VT",  "FF",  "CR",  "SO",  "SI",
		"DLE", "DC1", "DC2", "DC3", "DC4", "NAK", "SYN", "ETB",
		"CAN", "EM",  "SUB", "ESC", "FS",  "GS",  "RS",  "US",
	};

	static int printExportedVariables(Executor& exec) {
		std::vector<std::pair<std::string, std::string>> exported;
		for (const auto& entry : exec.env().vars()) {
			if (exec.env().isExported(entry.first)) exported.push_back(entry);
		}

		exec.appendExportedFunctions(exported);
		exec.appendShadowedExports(exported);
		std::sort(exported.begin(), exported.end());
		for (const auto& entry : exported) {
			std::printf("%s=%s\n", entry.first.c_str(), entry.second.c_str());
		}

		return 0;
	}

	static int builtin_printenv(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) return printExportedVariables(exec);

		int status = 0;
		for (const std::string& name : args) {
			std::string value;
			if (!exec.exportedValue(name, value)) {
				status = 1;
				continue;
			}

			std::printf("%s\n", value.c_str());
		}

		return status;
	}

	static int builtin_logname(Executor& exec, const std::vector<std::string>&) {
		std::printf("%s\n", currentUserName(exec).c_str());
		return 0;
	}

	static int builtin_groups(Executor& exec, const std::vector<std::string>& args) {
		const std::string name = args.empty() ? currentUserName(exec) : args[0];
		if (!args.empty()) std::printf("%s : ", name.c_str());
		std::printf("%s\n", name.c_str());
		return 0;
	}

	static int builtin_usleep(Executor&, const std::vector<std::string>& args) {
		long long microseconds = 0;
		if (args.empty() || !parseLL(args[0], microseconds) || microseconds < 0) {
			perr("usleep", "usage: usleep MICROSECONDS");
			return 1;
		}

		std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
		return 0;
	}

	// Portable 64-bit mulmod: MSVC has no 128-bit integer type, and the
	// _umul128 intrinsic does not exist on the 32-bit build.
	static std::uint64_t mulMod(std::uint64_t a, std::uint64_t b, std::uint64_t m) {
		std::uint64_t result = 0;
		a %= m;
		while (b != 0) {
			if ((b & 1) != 0) result = (result >= m - a) ? result - (m - a) : result + a;
			a = (a >= m - a) ? a - (m - a) : a + a;
			b >>= 1;
		}

		return result;
	}

	static std::uint64_t powMod(std::uint64_t base, std::uint64_t exponent, std::uint64_t m) {
		std::uint64_t result = 1;
		base %= m;
		while (exponent != 0) {
			if ((exponent & 1) != 0) result = mulMod(result, base, m);
			base = mulMod(base, base, m);
			exponent >>= 1;
		}

		return result;
	}

	static bool millerRabinWitness(std::uint64_t n, std::uint64_t d, int r, std::uint64_t a) {
		std::uint64_t x = powMod(a, d, n);
		if (x == 1 || x == n - 1) return false;

		for (int i = 1; i < r; ++i) {
			x = mulMod(x, x, n);
			if (x == n - 1) return false;
		}

		return true;
	}

	// These twelve bases make Miller-Rabin exact for every 64-bit n.
	static bool isPrime(std::uint64_t n) {
		static const std::uint64_t kBases[] = { 2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37 };
		if (n < 2) return false;
		for (const std::uint64_t base : kBases) {
			if (n % base == 0) return n == base;
		}

		std::uint64_t d = n - 1;
		int r = 0;
		while ((d & 1) == 0) {
			d >>= 1;
			++r;
		}

		for (const std::uint64_t base : kBases) {
			if (millerRabinWitness(n, d, r, base)) return false;
		}

		return true;
	}

	static std::uint64_t gcd(std::uint64_t a, std::uint64_t b) {
		while (b != 0) {
			const std::uint64_t t = a % b;
			a = b;
			b = t;
		}

		return a;
	}

	static std::uint64_t pollardRho(std::uint64_t n, std::uint64_t seed) {
		std::uint64_t x = seed;
		std::uint64_t y = seed;
		std::uint64_t divisor = 1;
		while (divisor == 1) {
			x = (mulMod(x, x, n) + seed) % n;
			y = (mulMod(y, y, n) + seed) % n;
			y = (mulMod(y, y, n) + seed) % n;
			divisor = gcd(x > y ? x - y : y - x, n);
		}

		return divisor;
	}

	static void collectFactors(std::uint64_t n, std::vector<std::uint64_t>& factors) {
		if (n == 1) return;
		if (isPrime(n)) {
			factors.push_back(n);
			return;
		}

		for (std::uint64_t seed = 1;; ++seed) {
			const std::uint64_t divisor = pollardRho(n, seed);
			if (divisor == n) continue;
			collectFactors(divisor, factors);
			collectFactors(n / divisor, factors);
			return;
		}
	}

	static void stripSmallFactors(std::uint64_t& n, std::vector<std::uint64_t>& factors) {
		for (std::uint64_t p = 2; p < 1000 && p * p <= n; ++p) {
			while (n % p == 0) {
				factors.push_back(p);
				n /= p;
			}
		}
	}

	static bool printFactorization(const std::string& text) {
		unsigned long long value = 0;
		std::size_t parsed = 0;
		if (text.empty() || text[0] == '-' || !parseULL(text, value, &parsed)
		    || parsed != text.size()) {
			perr("factor", "'" + text + "' is not a valid positive integer");
			return false;
		}

		std::uint64_t n = value;
		std::vector<std::uint64_t> factors;
		stripSmallFactors(n, factors);
		collectFactors(n, factors);
		std::sort(factors.begin(), factors.end());

		std::printf("%llu:", value);
		for (const std::uint64_t factor : factors) {
			std::printf(" %llu", static_cast<unsigned long long>(factor));
		}

		std::printf("\n");
		return true;
	}

	static int builtin_factor(Executor&, const std::vector<std::string>& args) {
		std::vector<std::string> numbers = args;
		if (numbers.empty()) {
			std::string word;
			while (std::cin >> word) numbers.push_back(word);
		}

		int status = 0;
		for (const std::string& number : numbers) {
			if (!printFactorization(number)) status = 1;
		}

		std::fflush(stdout);
		return status;
	}

	static bool randomBytes(unsigned char* bytes, std::size_t count) {
#ifdef _WIN32
		return ::BCryptGenRandom(nullptr, bytes, static_cast<ULONG>(count),
			BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
		(void)bytes;
		(void)count;
		return false;
#endif
	}

	static int builtin_uuidgen(Executor&, const std::vector<std::string>&) {
		unsigned char bytes[16];
		if (!randomBytes(bytes, sizeof(bytes))) {
			perr("uuidgen", "no random source");
			return 1;
		}

		bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);
		bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);
		for (std::size_t i = 0; i < sizeof(bytes); ++i) {
			if (i == 4 || i == 6 || i == 8 || i == 10) std::fputc('-', stdout);
			std::printf("%02x", bytes[i]);
		}

		std::printf("\n");
		return 0;
	}

	static bool isLeapYear(int year) {
		return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
	}

	static int daysInMonth(int year, int month) {
		static const int kDays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
		if (month == 2 && isLeapYear(year)) return 29;
		return kDays[month - 1];
	}

	// Sakamoto's method; 0 = Sunday.
	static int weekday(int year, int month, int day) {
		static const int kOffsets[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
		if (month < 3) year -= 1;
		return (year + year / 4 - year / 100 + year / 400 + kOffsets[month - 1] + day) % 7;
	}

	static std::string centered(const std::string& text, int width) {
		const int left = std::max(0, (width - static_cast<int>(text.size())) / 2);
		std::string line(static_cast<std::size_t>(left), ' ');
		line += text;
		line.resize(static_cast<std::size_t>(std::max(width, static_cast<int>(line.size()))), ' ');
		return line;
	}

	static std::vector<std::string> monthLines(int year, int month, bool with_year) {
		std::string title = kMonthNames[month - 1];
		if (with_year) title += " " + std::to_string(year);

		std::vector<std::string> lines = { centered(title, kCalendarWidth) };
		lines.emplace_back("Su Mo Tu We Th Fr Sa");
		std::string week(static_cast<std::size_t>(weekday(year, month, 1)) * 3, ' ');
		for (int day = 1; day <= daysInMonth(year, month); ++day) {
			char cell[4];
			std::snprintf(cell, sizeof(cell), "%2d", day);
			week += cell;
			if (weekday(year, month, day) == 6) {
				lines.push_back(week);
				week.clear();
				continue;
			}

			week.push_back(' ');
		}

		if (!week.empty()) lines.push_back(week);
		while (lines.size() < static_cast<std::size_t>(kCalendarWeekRows) + 2) lines.emplace_back();
		for (std::string& line : lines) line.resize(kCalendarWidth, ' ');
		return lines;
	}

	static std::string rightTrimmed(std::string line) {
		while (!line.empty() && line.back() == ' ') line.pop_back();
		return line;
	}

	static void printMonth(int year, int month) {
		for (const std::string& line : monthLines(year, month, true)) {
			std::printf("%s\n", rightTrimmed(line).c_str());
		}
	}

	static void printMonthRow(int year, int first_month) {
		std::vector<std::vector<std::string>> months;
		for (int k = 0; k < kMonthsPerRow; ++k) {
			months.push_back(monthLines(year, first_month + k, false));
		}

		for (std::size_t row = 0; row < months[0].size(); ++row) {
			std::string line;
			for (std::size_t k = 0; k < months.size(); ++k) {
				if (k != 0) line.append(kCalendarGap, ' ');
				line += months[k][row];
			}

			std::printf("%s\n", rightTrimmed(line).c_str());
		}
	}

	static void printYear(int year) {
		const int width = kCalendarWidth * kMonthsPerRow + kCalendarGap * (kMonthsPerRow - 1);
		std::printf("%s\n\n", rightTrimmed(centered(std::to_string(year), width)).c_str());
		for (int month = 1; month <= 12; month += kMonthsPerRow) {
			printMonthRow(year, month);
			if (month + kMonthsPerRow <= 12) std::printf("\n");
		}
	}

	static bool parseCalendarArgs(const std::vector<std::string>& args, CalendarRequest& request) {
		const std::time_t now = std::time(nullptr);
		std::tm local{};
		localtime_s(&local, &now);
		request.year = local.tm_year + 1900;
		request.month = local.tm_mon + 1;

		std::vector<std::string> numbers;
		for (const std::string& arg : args) {
			if (arg == "-y") request.whole_year = true;
			else if (!arg.empty() && arg[0] != '-') numbers.push_back(arg);
		}

		if (numbers.size() == 1) {
			request.whole_year = true;
			return parseInt(numbers[0], request.year) && request.year >= 1 && request.year <= 9999;
		}

		if (numbers.size() == 2) {
			return parseInt(numbers[0], request.month) && request.month >= 1 && request.month <= 12
				&& parseInt(numbers[1], request.year) && request.year >= 1 && request.year <= 9999;
		}

		return numbers.empty();
	}

	static int builtin_cal(Executor&, const std::vector<std::string>& args) {
		CalendarRequest request;
		if (!parseCalendarArgs(args, request)) {
			perr("cal", "usage: cal [-y] [[MONTH] YEAR]");
			return 1;
		}

		if (request.whole_year) printYear(request.year);
		else                    printMonth(request.year, request.month);
		std::fflush(stdout);
		return 0;
	}

	static std::string asciiCell(int code) {
		char glyph[2] = { static_cast<char>(code), '\0' };
		const char* name = glyph;
		if (code < 32)   name = kControlNames[code];
		if (code == 127) name = "DEL";

		char cell[16];
		std::snprintf(cell, sizeof(cell), "%3d %02x %-3s", code, code, name);
		return cell;
	}

	static int builtin_ascii(Executor&, const std::vector<std::string>&) {
		std::string header;
		for (int column = 0; column < kAsciiColumns; ++column) {
			header += column == 0 ? "Dec Hex" : "     Dec Hex";
		}

		std::printf("%s\n", header.c_str());
		for (int row = 0; row < kAsciiRows; ++row) {
			std::string line;
			for (int column = 0; column < kAsciiColumns; ++column) {
				if (column != 0) line += "  ";
				line += asciiCell(column * kAsciiRows + row);
			}

			std::printf("%s\n", rightTrimmed(line).c_str());
		}

		return 0;
	}

	static LongOption parseLongOptionSpec(std::string name) {
		LongOption option;
		if (name.size() > 2 && name.compare(name.size() - 2, 2, "::") == 0) {
			option.arg = GetoptArg::Optional;
			name.resize(name.size() - 2);
		}

		if (option.arg == GetoptArg::None && !name.empty() && name.back() == ':') {
			option.arg = GetoptArg::Required;
			name.pop_back();
		}

		option.name = name;
		return option;
	}

	static std::vector<LongOption> parseLongOptionList(const std::string& list) {
		std::vector<LongOption> options;
		std::size_t start = 0;
		for (;;) {
			const std::size_t comma = list.find(',', start);
			const std::size_t length = comma == std::string::npos ? std::string::npos
				: comma - start;
			const LongOption option = parseLongOptionSpec(list.substr(start, length));
			if (!option.name.empty()) options.push_back(option);
			if (comma == std::string::npos) return options;
			start = comma + 1;
		}
	}

	static bool takeGetoptValue(const std::vector<std::string>& args, std::size_t& i,
	                            const std::string& flag, std::string& value) {
		const std::string& arg = args[i];
		const std::string prefix = flag + "=";
		if (arg.compare(0, prefix.size(), prefix) == 0) {
			value = arg.substr(prefix.size());
			return true;
		}

		if (arg != flag) return false;
		if (i + 1 >= args.size()) return false;
		value = args[++i];
		return true;
	}

	static bool applyGetoptValueOption(const std::vector<std::string>& args, std::size_t& i,
	                                   GetoptSpec& spec, bool& have_shorts) {
		std::string value;
		if (takeGetoptValue(args, i, "-o", value) || takeGetoptValue(args, i, "--options", value)) {
			spec.shorts = value;
			have_shorts = true;
			return true;
		}

		if (takeGetoptValue(args, i, "-l", value)
		    || takeGetoptValue(args, i, "--longoptions", value)) {
			const std::vector<LongOption> longs = parseLongOptionList(value);
			spec.longs.insert(spec.longs.end(), longs.begin(), longs.end());
			return true;
		}

		if (takeGetoptValue(args, i, "-n", value) || takeGetoptValue(args, i, "--name", value)) {
			spec.name = value;
			return true;
		}

		return takeGetoptValue(args, i, "-s", value) || takeGetoptValue(args, i, "--shell", value);
	}

	static bool applyGetoptFlag(const std::string& arg, GetoptSpec& spec) {
		if (arg == "-q" || arg == "--quiet")             spec.quiet = true;
		else if (arg == "-Q" || arg == "--quiet-output") spec.quiet_output = true;
		else if (arg == "-a" || arg == "--alternative")  spec.alternative = true;
		else if (arg == "-u" || arg == "--unquoted")     spec.quote = false;
		else if (arg == "-T" || arg == "--test")         spec.test_only = true;
		else return false;
		return true;
	}

	static bool parseGetoptArgs(const std::vector<std::string>& args, GetoptSpec& spec) {
		if (!args.empty() && !args[0].empty() && args[0][0] != '-') {
			spec.shorts = args[0];
			spec.quote = false;
			spec.params.assign(args.begin() + 1, args.end());
			return true;
		}

		bool have_shorts = false;
		std::size_t i = 0;
		for (; i < args.size(); ++i) {
			if (args[i] == "--") {
				++i;
				break;
			}

			if (applyGetoptFlag(args[i], spec)) continue;
			if (applyGetoptValueOption(args, i, spec, have_shorts)) continue;
			if (args[i].empty() || args[i][0] != '-') break;
			perr(spec.name, "invalid option -- '" + args[i] + "'");
			return false;
		}

		if (!have_shorts) {
			if (i >= args.size()) return spec.test_only;
			spec.shorts = args[i++];
		}

		spec.params.assign(args.begin() + static_cast<std::ptrdiff_t>(i), args.end());
		return true;
	}

	static std::string getoptQuoted(const std::string& word, bool quote) {
		if (!quote) return word;

		std::string quoted = "'";
		for (const char c : word) {
			if (c == '\'') quoted += "'\\''";
			else           quoted.push_back(c);
		}

		return quoted + "'";
	}

	static void getoptError(const GetoptSpec& spec, GetoptOutput& out, const std::string& message) {
		out.failed = true;
		if (!spec.quiet) std::fprintf(stderr, "%s: %s\n", spec.name.c_str(), message.c_str());
	}

	static GetoptArg shortOptionArg(const std::string& shorts, char flag, bool& known) {
		const std::size_t at = shorts.find(flag);
		known = flag != ':' && at != std::string::npos;
		if (!known) return GetoptArg::None;
		if (shorts.compare(at + 1, 2, "::") == 0) return GetoptArg::Optional;

		if (at + 1 < shorts.size() && shorts[at + 1] == ':') return GetoptArg::Required;
		return GetoptArg::None;
	}

	static void appendOption(GetoptOutput& out, const std::string& flag) {
		out.options += " " + flag;
	}

	static void appendOptionValue(GetoptOutput& out, const GetoptSpec& spec,
	                              const std::string& value) {
		out.options += " " + getoptQuoted(value, spec.quote);
	}

	static void parseShortCluster(const GetoptSpec& spec, std::size_t& i, GetoptOutput& out) {
		const std::string& cluster = spec.params[i];
		const bool has_mode_prefix = !spec.shorts.empty()
			&& (spec.shorts[0] == '+' || spec.shorts[0] == '-');
		const std::string shorts = has_mode_prefix ? spec.shorts.substr(1) : spec.shorts;
		for (std::size_t k = 1; k < cluster.size(); ++k) {
			bool known = false;
			const GetoptArg arg = shortOptionArg(shorts, cluster[k], known);
			if (!known) {
				getoptError(spec, out, std::string("invalid option -- '") + cluster[k] + "'");
				continue;
			}

			appendOption(out, std::string("-") + cluster[k]);
			if (arg == GetoptArg::None) continue;

			const std::string rest = cluster.substr(k + 1);
			if (!rest.empty() || arg == GetoptArg::Optional) {
				appendOptionValue(out, spec, rest);
				return;
			}

			if (i + 1 >= spec.params.size()) {
				getoptError(spec, out, std::string("option requires an argument -- '")
					+ cluster[k] + "'");
				return;
			}

			appendOptionValue(out, spec, spec.params[++i]);
			return;
		}
	}

	static const LongOption* findLongOption(const GetoptSpec& spec, const std::string& name,
	                                        bool& ambiguous) {
		const LongOption* prefix_match = nullptr;
		int prefix_matches = 0;
		for (const LongOption& option : spec.longs) {
			if (option.name == name) return &option;
			if (option.name.compare(0, name.size(), name) != 0) continue;
			prefix_match = &option;
			++prefix_matches;
		}

		ambiguous = prefix_matches > 1;
		return prefix_matches == 1 ? prefix_match : nullptr;
	}

	static bool parseLongOption(const GetoptSpec& spec, std::size_t& i, std::size_t dashes,
	                            GetoptOutput& out) {
		const std::string body = spec.params[i].substr(dashes);
		const std::size_t eq = body.find('=');
		const std::string name = body.substr(0, eq);
		bool ambiguous = false;
		const LongOption* option = findLongOption(spec, name, ambiguous);
		if (option == nullptr) {
			if (dashes == 1) return false;
			getoptError(spec, out, std::string(ambiguous ? "option '--" : "unrecognized option '--")
				+ name + (ambiguous ? "' is ambiguous" : "'"));
			return true;
		}

		appendOption(out, "--" + option->name);
		if (option->arg == GetoptArg::None) {
			if (eq != std::string::npos) {
				getoptError(spec, out, "option '--" + option->name + "' doesn't allow an argument");
			}

			return true;
		}

		if (eq != std::string::npos || option->arg == GetoptArg::Optional) {
			appendOptionValue(out, spec, eq == std::string::npos ? "" : body.substr(eq + 1));
			return true;
		}

		if (i + 1 >= spec.params.size()) {
			getoptError(spec, out, "option '--" + option->name + "' requires an argument");
			return true;
		}

		appendOptionValue(out, spec, spec.params[++i]);
		return true;
	}

	static void appendOperand(GetoptOutput& out, const GetoptSpec& spec, const std::string& word) {
		out.operands += " " + getoptQuoted(word, spec.quote);
	}

	static void appendRemainingOperands(const GetoptSpec& spec, std::size_t from,
	                                    GetoptOutput& out) {
		for (std::size_t k = from; k < spec.params.size(); ++k) {
			appendOperand(out, spec, spec.params[k]);
		}
	}

	static GetoptOutput runGetopt(const GetoptSpec& spec) {
		GetoptOutput out;
		const bool stop_at_operand = !spec.shorts.empty() && spec.shorts[0] == '+';
		for (std::size_t i = 0; i < spec.params.size(); ++i) {
			const std::string& param = spec.params[i];
			if (param == "--") {
				appendRemainingOperands(spec, i + 1, out);
				break;
			}

			if (param.size() > 2 && param.compare(0, 2, "--") == 0) {
				parseLongOption(spec, i, 2, out);
				continue;
			}

			if (param.size() > 1 && param[0] == '-') {
				if (spec.alternative && parseLongOption(spec, i, 1, out)) continue;
				parseShortCluster(spec, i, out);
				continue;
			}

			if (stop_at_operand) {
				appendRemainingOperands(spec, i, out);
				break;
			}

			appendOperand(out, spec, param);
		}

		return out;
	}

	static int builtin_getopt(Executor&, const std::vector<std::string>& args) {
		GetoptSpec spec;
		if (!parseGetoptArgs(args, spec)) return kGetoptUsageError;
		if (spec.test_only) return kGetoptEnhanced;

		const GetoptOutput out = runGetopt(spec);
		if (!spec.quiet_output) std::printf("%s --%s\n", out.options.c_str(), out.operands.c_str());
		std::fflush(stdout);
		return out.failed ? kGetoptParseError : 0;
	}

#ifdef _WIN32

	static const DWORD kCookedInputMode =
		ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT;

	static HANDLE consoleInputHandle() {
		const HANDLE input = ::GetStdHandle(STD_INPUT_HANDLE);
		DWORD mode = 0;
		if (input != INVALID_HANDLE_VALUE && ::GetConsoleMode(input, &mode) != 0) return input;

		return ::CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	}

	static bool consoleWindowSize(int& columns, int& rows) {
		if (queryConsoleSize(columns, rows)) return true;

		const HANDLE output = ::CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
		if (output == INVALID_HANDLE_VALUE) return false;

		CONSOLE_SCREEN_BUFFER_INFO info{};
		const bool ok = ::GetConsoleScreenBufferInfo(output, &info) != 0;
		::CloseHandle(output);
		if (!ok) return false;

		columns = info.srWindow.Right - info.srWindow.Left + 1;
		rows    = info.srWindow.Bottom - info.srWindow.Top + 1;
		return true;
	}

	// The POSIX settings Windows has no equivalent for: accepted so the
	// scripts that set them keep working.
	static bool isIgnoredTtySetting(const std::string& setting) {
		static const char* const kIgnored[] = {
			"ixon", "ixoff", "ixany", "opost", "onlcr", "ocrnl", "icrnl", "inlcr", "igncr",
			"iexten", "echoe", "echok", "echoctl", "echoke", "cs8", "parenb", "istrip",
			"brkint", "ignbrk", "cbreak", "pass8", "crtscts", "clocal", "hupcl",
		};
		const std::string flag = setting[0] == '-' ? setting.substr(1) : setting;
		for (const char* ignored : kIgnored) {
			if (flag == ignored) return true;
		}

		return false;
	}

	static bool applyTtySetting(const std::string& setting, DWORD& mode) {
		unsigned long raw_mode = 0;
		std::size_t parsed = 0;
		if (parseUL(setting, raw_mode, 16, &parsed) && parsed == setting.size()) {
			mode = static_cast<DWORD>(raw_mode);
			return true;
		}

		const DWORD line_and_echo = ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT;
		const bool cooked = setting == "-raw" || setting == "sane" || setting == "cooked";
		if (setting == "echo")         mode |= line_and_echo;
		else if (setting == "-echo")   mode &= ~static_cast<DWORD>(ENABLE_ECHO_INPUT);
		else if (setting == "icanon")  mode |= ENABLE_LINE_INPUT;
		else if (setting == "-icanon") mode &= ~line_and_echo;
		else if (setting == "isig")    mode |= ENABLE_PROCESSED_INPUT;
		else if (setting == "-isig")   mode &= ~static_cast<DWORD>(ENABLE_PROCESSED_INPUT);
		else if (setting == "raw")     mode &= ~kCookedInputMode;
		else if (cooked)               mode |= kCookedInputMode;
		else return isIgnoredTtySetting(setting);
		return true;
	}

	static void printTtyState(DWORD mode) {
		int columns = kFallbackColumns;
		int rows = kFallbackRows;
		consoleWindowSize(columns, rows);
		std::printf("speed 38400 baud; rows %d; columns %d;\n", rows, columns);
		std::printf("%sisig %sicanon %secho\n",
			(mode & ENABLE_PROCESSED_INPUT) != 0 ? "" : "-",
			(mode & ENABLE_LINE_INPUT) != 0 ? "" : "-",
			(mode & ENABLE_ECHO_INPUT) != 0 ? "" : "-");
	}

	static int reportTtyQuery(const std::string& query, DWORD mode) {
		int columns = kFallbackColumns;
		int rows = kFallbackRows;
		if (query == "size") {
			if (!consoleWindowSize(columns, rows)) {
				perr("stty", "standard input: not a tty");
				return 1;
			}

			std::printf("%d %d\n", rows, columns);
			return 0;
		}

		if (query == "-g" || query == "--save") {
			std::printf("%lx\n", static_cast<unsigned long>(mode));
			return 0;
		}

		printTtyState(mode);
		return 0;
	}

	static bool isTtyQuery(const std::vector<std::string>& args) {
		if (args.empty()) return true;
		const std::string& first = args[0];
		return first == "size" || first == "-g" || first == "--save" || first == "-a"
			|| first == "--all";
	}

	static int builtin_stty(Executor&, const std::vector<std::string>& args) {
		const HANDLE input = consoleInputHandle();
		DWORD mode = 0;
		if (input == INVALID_HANDLE_VALUE || ::GetConsoleMode(input, &mode) == 0) {
			perr("stty", "standard input: not a tty");
			return 1;
		}

		if (isTtyQuery(args)) return reportTtyQuery(args.empty() ? "-a" : args[0], mode);

		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& setting = args[i];
			if (setting == "rows" || setting == "cols" || setting == "columns") {
				++i;
				continue;
			}

			if (!applyTtySetting(setting, mode)) {
				perr("stty", "invalid argument '" + setting + "'");
				return 1;
			}
		}

		if (::SetConsoleMode(input, mode) != 0) return 0;
		perr("stty", "cannot set console mode");
		return 1;
	}

	static int builtin_reset(Executor&, const std::vector<std::string>&) {
		const HANDLE input = consoleInputHandle();
		DWORD mode = 0;
		if (input != INVALID_HANDLE_VALUE && ::GetConsoleMode(input, &mode) != 0) {
			::SetConsoleMode(input, mode | kCookedInputMode);
		}

		std::fputs("\x1b" "c\x1b[0m\x1b[?25h", stdout);
		std::fflush(stdout);
		return 0;
	}

	static int builtin_ttysize(Executor&, const std::vector<std::string>& args) {
		int columns = kFallbackColumns;
		int rows = kFallbackRows;
		consoleWindowSize(columns, rows);
		if (args.empty()) {
			std::printf("%d %d\n", columns, rows);
			return 0;
		}

		for (std::size_t i = 0; i < args.size(); ++i) {
			if (i != 0) std::fputc(' ', stdout);
			std::printf("%d", args[i] == "h" ? rows : columns);
		}

		std::printf("\n");
		return 0;
	}

#endif /* _WIN32 */

	void registerSystemExtraBuiltins(Executor& exec) {
		exec.registerBuiltin("printenv", builtin_printenv);
		exec.registerBuiltin("logname",  builtin_logname);
		exec.registerBuiltin("groups",   builtin_groups);
		exec.registerBuiltin("usleep",   builtin_usleep);
		exec.registerBuiltin("factor",   builtin_factor);
		exec.registerBuiltin("uuidgen",  builtin_uuidgen);
		exec.registerBuiltin("cal",      builtin_cal);
		exec.registerBuiltin("ascii",    builtin_ascii);
		exec.registerBuiltin("getopt",   builtin_getopt);
#ifdef _WIN32
		exec.registerBuiltin("stty",     builtin_stty);
		exec.registerBuiltin("reset",    builtin_reset);
		exec.registerBuiltin("ttysize",  builtin_ttysize);
#endif /* _WIN32 */
	}

}  // namespace wbsh

/**
 * @file coreutils_bc.cpp
 * @brief bc(1): the command line and the driver that feeds sources to the parser.
 *
 * The language lives in bc_lexer.cpp / bc_parser.cpp / bc_interp.cpp, the
 * numbers in bcnum.cpp, number output in bc_output.cpp and the `-l` math
 * library in bc_mathlib.cpp.
 */

#include "coreutils_internal.h"

#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "bc_internal.h"
#include "executor.h"
#include "numparse.h"

namespace wbsh {

	namespace bc_detail {

		struct BcInvocation {
			bool mathlib = false;
			std::vector<std::string> expressions;
			std::vector<std::string> files;
			int line_length = kDefaultLineLength;
		};

		enum class SourceResult { Continue, Stop };

		static const char kUsage[] =
			"usage: bc [options] [file ...]\n"
			"  -h  --help         print this usage and exit\n"
			"  -l  --mathlib      use the predefined math routines\n"
			"  -q  --quiet        don't print initial banner\n"
			"  -s  --standard     non-standard bc constructs are errors\n"
			"  -w  --warn         warn about non-standard bc constructs\n"
			"  -v  --version      print version information and exit\n"
			"  -e  --expression   run EXPR before the files\n";

		static int usageError(const std::string& message) {
			std::fprintf(stderr, "bc: %s\n%s", message.c_str(), kUsage);
			return 1;
		}

		static bool applyFlag(char flag, BcInvocation& invocation) {
			switch (flag) {
				case 'l':
					invocation.mathlib = true;
					return true;
				case 'q':
				case 's':
				case 'w':
				case 'i':
				case 'c':
					return true;
				default:
					return false;
			}
		}

		static bool longFlag(const std::string& arg, BcInvocation& invocation) {
			if (arg == "--mathlib") return applyFlag('l', invocation);
			return arg == "--quiet" || arg == "--standard" || arg == "--warn" || arg == "--interactive";
		}

		static int parseShortFlags(const std::vector<std::string>& args, std::size_t& i, BcInvocation& invocation) {
			const std::string& arg = args[i];
			for (std::size_t k = 1; k < arg.size(); ++k) {
				const char flag = arg[k];
				if (flag == 'e') {
					std::string text = arg.substr(k + 1);
					if (text.empty() && ++i >= args.size()) return usageError("option requires an argument -- 'e'");
					invocation.expressions.push_back(text.empty() ? args[i] : text);
					return -1;
				}

				if (!applyFlag(flag, invocation)) return usageError(std::string("invalid option -- '") + flag + "'");
			}

			return -1;
		}

		static int parseOption(const std::vector<std::string>& args, std::size_t& i, BcInvocation& invocation) {
			const std::string& arg = args[i];
			if (arg == "-h" || arg == "--help") {
				std::fputs(kUsage, stdout);
				return 0;
			}

			if (arg == "-v" || arg == "--version") {
				std::fputs("bc 1.07.1 (wbsh)\n", stdout);
				return 0;
			}

			if (arg.rfind("--expression=", 0) == 0) {
				invocation.expressions.push_back(arg.substr(13));
				return -1;
			}

			if (arg == "--expression") {
				if (++i >= args.size()) return usageError("option '--expression' requires an argument");
				invocation.expressions.push_back(args[i]);
				return -1;
			}

			if (arg.rfind("--", 0) != 0) return parseShortFlags(args, i, invocation);
			return longFlag(arg, invocation) ? -1 : usageError("unrecognized option '" + arg + "'");
		}

		// Returns -1 to run, otherwise the exit status.
		static int parseArgs(const std::vector<std::string>& args, BcInvocation& invocation) {
			bool options_done = false;
			for (std::size_t i = 0; i < args.size(); ++i) {
				const std::string& arg = args[i];
				if (!options_done && arg == "--") {
					options_done = true;
					continue;
				}

				if (options_done || arg.size() < 2 || arg[0] != '-') {
					invocation.files.push_back(arg);
					continue;
				}

				const int status = parseOption(args, i, invocation);
				if (status >= 0) return status;
			}

			return -1;
		}

		static int lineLengthFrom(const std::string& text) {
			int length = 0;
			if (text.empty() || !parseInt(text, length)) return kDefaultLineLength;
			if (length == 0) return 0;
			return length < 3 ? kDefaultLineLength : length;
		}

		static SourceResult runSource(BcState& state, BcSource& source) {
			BcParser parser(state, source);
			for (;;) {
				StmtList item;
				switch (parser.next(item)) {
					case ParseStatus::End:
						state.out.flush();
						return SourceResult::Continue;
					case ParseStatus::Quit:
						state.out.flush();
						return SourceResult::Stop;
					case ParseStatus::Item:
						if (!runItem(state, item)) {
							state.out.flush();
							return SourceResult::Stop;
						}

						state.out.flush();
						break;
					default:
						break;
				}
			}
		}

		static SourceResult runText(BcState& state, const std::string& text) {
			BcSource source;
			source.text = text + "\n";
			source.name = "(standard_in)";
			return runSource(state, source);
		}

		static int runFile(Executor& exec, BcState& state, const std::string& path, SourceResult& result) {
			BcSource source;
			source.file = fopenNative(exec, path, "rb");
			source.name = path;
			if (source.file == nullptr) {
				std::fprintf(stderr, "File %s is unavailable.\n", path.c_str());
				return 1;
			}

			result = runSource(state, source);
			std::fclose(source.file);
			return 0;
		}

		static int runInvocation(Executor& exec, const BcInvocation& invocation) {
			BcState state;
			state.out.line_length = invocation.line_length;
			state.mathlib = invocation.mathlib;
			if (invocation.mathlib) state.scale = kMathLibScale;

			for (const std::string& text : invocation.expressions) {
				if (runText(state, text) == SourceResult::Stop) return 0;
			}

			for (const std::string& path : invocation.files) {
				SourceResult result = SourceResult::Continue;
				if (runFile(exec, state, path, result) != 0) return 1;
				if (result == SourceResult::Stop) return 0;
			}

			if (!invocation.expressions.empty()) return 0;

			BcSource input;
			input.file = stdin;
			input.name = "(standard_in)";
			runSource(state, input);
			return 0;
		}

		struct BcRun {
			Executor& exec;
			const BcInvocation& invocation;
			int status = 0;
		};

#ifdef _WIN32
		// Reserved, not committed: deep bc recursion needs more than the
		// default 1 MB stack of the shell's own thread.
		static const SIZE_T kInterpreterStackBytes = 256u * 1024u * 1024u;

		static DWORD WINAPI bcThread(LPVOID param) {
			BcRun& run = *static_cast<BcRun*>(param);
			run.status = runInvocation(run.exec, run.invocation);
			return 0;
		}

		static int runOnLargeStack(BcRun& run) {
			HANDLE thread = ::CreateThread(nullptr, kInterpreterStackBytes, bcThread, &run,
				STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
			if (thread == nullptr) return runInvocation(run.exec, run.invocation);

			::WaitForSingleObject(thread, INFINITE);
			::CloseHandle(thread);
			return run.status;
		}
#else
		static int runOnLargeStack(BcRun& run) {
			return runInvocation(run.exec, run.invocation);
		}
#endif

		static int builtin_bc(Executor& exec, const std::vector<std::string>& args) {
			BcInvocation invocation;
			const int status = parseArgs(args, invocation);
			if (status >= 0) return status;

			invocation.line_length = lineLengthFrom(exec.env().get("BC_LINE_LENGTH"));
			BcRun run{ exec, invocation };
			return runOnLargeStack(run);
		}

	}  // namespace bc_detail

	void registerBcBuiltin(Executor& exec) {
		exec.registerBuiltin("bc", bc_detail::builtin_bc);
	}

}  // namespace wbsh

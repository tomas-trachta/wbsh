/**
 * @file awk.cpp
 * @brief Built-in awk(1): command-line handling and the run of one program.
 *
 * The language lives in awk_lexer.cpp, awk_parser.cpp and the awk_interp.h
 * interpreter (awk_interp.cpp, awk_fields.cpp, awk_builtins.cpp, awk_io.cpp).
 */

#include "awk.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "awk_interp.h"
#include "environment.h"
#include "executor.h"

namespace wbsh {

	namespace awk_detail {

		struct Invocation {
			std::string program_text;
			std::string field_separator;
			bool has_field_separator = false;
			std::vector<std::string> operands;
			std::vector<std::string> var_assigns;
			bool have_program = false;
			bool program_from_file = false;
		};

		static bool loadProgramFile(Executor& exec, const std::string& path,
				Invocation& invocation) {
			const std::string native = exec.pathConv().toWin32(path);
			std::ifstream file(utf8ToPath(native), std::ios::binary);
			if (!file) {
				std::fprintf(stderr, "awk: cannot open program file: %s\n", path.c_str());
				return false;
			}

			std::ostringstream contents;
			contents << file.rdbuf();
			if (invocation.program_from_file) invocation.program_text += "\n";
			invocation.program_text += contents.str();
			invocation.have_program = true;
			invocation.program_from_file = true;
			return true;
		}

		static bool addVarAssign(const std::string& assignment, Invocation& invocation) {
			if (!isAssignmentOperand(assignment)) {
				std::fprintf(stderr, "awk: invalid -v argument: %s\n", assignment.c_str());
				return false;
			}

			invocation.var_assigns.push_back(assignment);
			return true;
		}

		static void takeProgramOrOperand(const std::string& arg, Invocation& invocation) {
			if (invocation.have_program) {
				invocation.operands.push_back(arg);
				return;
			}

			invocation.program_text = arg;
			invocation.have_program = true;
		}

		static bool hasFlagPrefix(const std::string& arg, const char* flag) {
			return arg.size() > 2 && arg.compare(0, 2, flag) == 0;
		}

		static void setFieldSeparator(const std::string& value, Invocation& invocation) {
			invocation.field_separator = value;
			invocation.has_field_separator = true;
		}

		// Handles the option at args[i]; false when it is not an option.
		static bool takeOption(Executor& exec, const std::vector<std::string>& args, std::size_t& i,
				Invocation& invocation, int& status) {
			const std::string& arg = args[i];
			const bool has_value = i + 1 < args.size();
			if (arg == "-F" && has_value) {
				setFieldSeparator(args[++i], invocation);
			} else if (hasFlagPrefix(arg, "-F")) {
				setFieldSeparator(arg.substr(2), invocation);
			} else if (arg == "-f" && has_value) {
				if (!loadProgramFile(exec, args[++i], invocation)) status = kUsageError;
			} else if (arg == "-v" && has_value) {
				if (!addVarAssign(args[++i], invocation)) status = kUsageError;
			} else if (hasFlagPrefix(arg, "-v")) {
				if (!addVarAssign(arg.substr(2), invocation)) status = kUsageError;
			} else {
				return false;
			}

			return true;
		}

		static bool looksLikeOption(const std::string& arg) {
			return arg.size() > 1 && arg[0] == '-';
		}

		// Options come first; the program text (unless -f gave one) and the
		// operands follow.
		static int parseArgs(Executor& exec, const std::vector<std::string>& args,
				Invocation& invocation) {
			std::size_t i = 0;
			for (; i < args.size() && looksLikeOption(args[i]); ++i) {
				if (args[i] == "--") {
					++i;
					break;
				}

				int status = 0;
				if (!takeOption(exec, args, i, invocation, status)) break;
				if (status != 0) return status;
			}

			for (; i < args.size(); ++i) takeProgramOrOperand(args[i], invocation);
			return 0;
		}

		static void populateEnviron(Executor& exec, Interpreter& interp) {
			const Environment& env = exec.env();
			AwkArray& environ_array = interp.globalArray(kENVIRON);
			for (const auto& var : env.vars()) {
				if (!env.isExported(var.first)) continue;

				environ_array[var.first] = AwkValue::input(var.second);
			}
		}

		static void populateArgv(const Invocation& invocation, Interpreter& interp) {
			AwkArray& argv = interp.globalArray(kARGV);
			argv["0"] = AwkValue::str("awk");
			for (std::size_t i = 0; i < invocation.operands.size(); ++i) {
				argv[std::to_string(i + 1)] = AwkValue::input(invocation.operands[i]);
			}

			const auto argc = static_cast<double>(invocation.operands.size() + 1);
			interp.setGlobal(kARGC, AwkValue::num(argc));
		}

		// POSIX: `-F t` means a tab; other values get the string escapes.
		static std::string fieldSeparatorValue(const std::string& raw) {
			if (raw == "t") return "\t";
			return expandEscapes(raw);
		}

		static void applyInvocation(const Invocation& invocation, Interpreter& interp) {
			if (invocation.has_field_separator) {
				const std::string separator = fieldSeparatorValue(invocation.field_separator);
				interp.setGlobal(kFS, AwkValue::str(separator));
			}

			for (const auto& assignment : invocation.var_assigns) {
				applyAssignmentOperand(interp, assignment);
			}
		}

		// A program made only of BEGIN blocks (and functions) never reads its input.
		static bool readsInput(const AwkProgram& program) {
			for (const auto& rule : program.rules) {
				if (rule.pattern.kind != AwkPattern::Begin) return true;
			}

			return false;
		}

		static int runProgram(Executor& exec, const Invocation& invocation, AwkProgram& program) {
			Interpreter interp(program, exec);
			populateEnviron(exec, interp);
			populateArgv(invocation, interp);
			applyInvocation(invocation, interp);

			interp.runBegins();
			if (!interp.exiting && readsInput(program)) interp.runMainInput();
			interp.runEnds();
			interp.closeAllStreams();

			if (interp.input_error && interp.exit_status == 0) return kUsageError;
			return interp.exit_status;
		}

		struct ProgramRun {
			Executor& exec;
			const Invocation& invocation;
			AwkProgram& program;
			int status = 0;
		};

#ifdef _WIN32
		// Reserved, not committed: deep awk recursion needs far more than the
		// default 1 MB stack of the shell's own thread.
		static const SIZE_T kInterpreterStackBytes = 512u * 1024u * 1024u;

		static DWORD WINAPI programThread(LPVOID param) {
			ProgramRun& run = *static_cast<ProgramRun*>(param);
			run.status = runProgram(run.exec, run.invocation, run.program);
			return 0;
		}

		static int runOnLargeStack(ProgramRun& run) {
			HANDLE thread = ::CreateThread(nullptr, kInterpreterStackBytes, programThread, &run,
				STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
			if (thread == nullptr) return runProgram(run.exec, run.invocation, run.program);

			::WaitForSingleObject(thread, INFINITE);
			::CloseHandle(thread);
			return run.status;
		}
#else
		static int runOnLargeStack(ProgramRun& run) {
			return runProgram(run.exec, run.invocation, run.program);
		}
#endif

	}  // namespace awk_detail

	int builtin_awk(Executor& exec, const std::vector<std::string>& args) {
		using namespace awk_detail;

		Invocation invocation;
		const int status = parseArgs(exec, args, invocation);
		if (status != 0) return status;
		if (!invocation.have_program) {
			std::fprintf(stderr, "awk: missing program text\n");
			return kUsageError;
		}

		AwkProgram program;
		std::string error;
		if (!parseAwkProgram(invocation.program_text, program, error)) {
			std::fprintf(stderr, "%s\n", error.c_str());
			return kUsageError;
		}

		ProgramRun run{ exec, invocation, program };
		return runOnLargeStack(run);
	}

}  // namespace wbsh

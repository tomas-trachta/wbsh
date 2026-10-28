/**
 * @file coreutils_ed.cpp
 * @brief ed: the POSIX line editor (with GNU's W, wq, z, E and -p / -s / -v).
 *
 * Commands are read from stdin. An error prints `?` (and the reason
 * after H or with -v); when stdin is a regular file, as for
 * `ed file < script`, the first error ends the session, as POSIX asks.
 * The exit status is 1 once any command has failed.
 */

#include "coreutils_internal.h"

#include <io.h>

#include <cstdio>
#include <string>
#include <vector>

#include "ed_internal.h"
#include "executor.h"
#include "optscan.h"
#include "stdinsync.h"

namespace wbsh {

	namespace ed_detail {

		enum EdOption {
			kOptSilent = 1,
			kOptPrompt,
			kOptVerbose,
			kOptLoose,
			kOptCompatible,
		};

		bool fail(EdState& st, const std::string& message) {
			st.error = message;
			return false;
		}

		// Past what the CRT has already buffered, fd 0 is read a byte at a
		// time: a shell command that redirects fd 0 drops that buffer, and
		// with it the rest of the script.
		static int nextStdinByte() {
			if (stdinBufferedBytes() > 0) return std::fgetc(stdin);

			unsigned char byte = 0;
			return _read(_fileno(stdin), &byte, 1) == 1 ? byte : EOF;
		}

		bool readUserLine(EdState& st, std::string& line) {
			line.clear();
			if (st.input_ended) return false;

			bool any = false;
			for (int c = nextStdinByte(); c != EOF; c = nextStdinByte()) {
				any = true;
				if (c == '\n') break;
				line.push_back(static_cast<char>(c));
			}

			if (!any) {
				st.input_ended = true;
				return false;
			}

			if (!line.empty() && line.back() == '\r') line.pop_back();
			return true;
		}

		bool readInputLine(EdState& st, std::string& line) {
			if (!st.in_global) return readUserLine(st, line);
			if (st.global_next >= st.global_input.size()) return false;

			line = st.global_input[st.global_next++];
			return true;
		}

		static void reportError(EdState& st) {
			std::printf("?\n");
			if (st.verbose) std::printf("%s\n", st.error.c_str());
			if (!st.loose) st.status = 1;
			if (isDiskStream(stdin)) st.quit = true;
		}

		static void runLine(EdState& st, const std::string& line) {
			st.warning_active = st.warned;
			st.warned = false;

			CommandText cmd;
			cmd.text = line;
			if (!executeCommand(st, cmd)) reportError(st);
		}

		static void showPrompt(const EdState& st) {
			if (st.prompt_on) std::printf("%s", st.prompt.c_str());
			std::fflush(stdout);
		}

		// End of input is a q: refused once when the buffer has unsaved changes.
		static void runSession(EdState& st) {
			while (!st.quit) {
				showPrompt(st);
				std::string line;
				if (!readUserLine(st, line)) line = "q";
				runLine(st, line);
			}

			std::fflush(stdout);
		}

		static void loadInitialFile(EdState& st, const std::string& name) {
			if (editFile(st, name)) return;
			if (name[0] != '!') st.filename = name;
		}

		static bool applyOption(const OptHit& hit, EdState& st) {
			switch (hit.id) {
				case kOptSilent:
					st.silent = true;
					return true;
				case kOptPrompt:
					st.prompt = hit.value;
					st.prompt_on = true;
					return true;
				case kOptVerbose:
					st.verbose = true;
					return true;
				case kOptLoose:
					st.loose = true;
					return true;
				case kOptCompatible:
					return true;
				default:
					return false;
			}
		}

		static bool parseEdArgs(const std::vector<std::string>& args, EdState& st,
		                        std::vector<std::string>& operands) {
			static const std::vector<OptDef> defs = {
				{ kOptSilent, 's', "quiet", OptArg::None },
				{ kOptSilent, 0, "silent", OptArg::None },
				{ kOptPrompt, 'p', "prompt", OptArg::Required },
				{ kOptVerbose, 'v', "verbose", OptArg::None },
				{ kOptLoose, 'l', "loose-exit-status", OptArg::None },
				{ kOptCompatible, 'G', "traditional", OptArg::None },
			};

			OptScanResult scanned;
			if (!scanOptions("ed", args, defs, scanned)) return false;
			for (const OptHit& hit : scanned.hits) applyOption(hit, st);

			for (const std::string& operand : scanned.operands) {
				if (operand == "-") st.silent = true;
				else operands.push_back(operand);
			}

			return operands.size() <= 1;
		}

	}  // namespace ed_detail

	static int builtin_ed(Executor& exec, const std::vector<std::string>& args) {
		ed_detail::EdState st;
		st.exec = &exec;

		std::vector<std::string> operands;
		if (!ed_detail::parseEdArgs(args, st, operands)) {
			perr("ed", "usage: ed [-lsvG] [-p PROMPT] [FILE]");
			return 1;
		}

		if (!operands.empty() && !operands[0].empty()) ed_detail::loadInitialFile(st, operands[0]);
		ed_detail::runSession(st);
		return st.status;
	}

	void registerEdBuiltin(Executor& exec) {
		exec.registerBuiltin("ed", builtin_ed);
	}

}  // namespace wbsh

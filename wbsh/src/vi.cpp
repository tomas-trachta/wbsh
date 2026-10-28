/**
 * @file vi.cpp
 * @brief vi and view: a screen editor in the spirit of busybox vi.
 *
 * On a console the editor runs full screen on the alternate screen.
 * When stdin is not a console it reads its keys from stdin as bytes
 * (ESC is Escape, CR or LF is Enter) and draws nothing; status messages
 * go to stderr instead, one per line. That makes it scriptable:
 * `printf 'ihello\033:wq\n' | vi file`.
 */

#include "coreutils_internal.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif /* _WIN32 */

#include <cstdio>
#include <string>
#include <vector>

#include "environment.h"
#include "executor.h"
#include "pathconv.h"
#include "vi_internal.h"

namespace wbsh {

	namespace vi_detail {

		static const char* const kContinuePrompt = "Press ENTER or type command to continue";

		struct ViArgs {
			std::vector<std::string> files;
			std::vector<std::string> commands;
			bool readonly = false;
		};

		bool nextKey(Editor& ed, Key& key) {
			if (!ed.injected.empty()) {
				key = ed.injected.front();
				ed.injected.pop_front();
				return true;
			}

			if (!ed.pending.empty()) {
				key = ed.pending.front();
				ed.pending.pop_front();
			} else {
				if (ed.input_ended) return false;
				if (!ed.term.headless && !terminalInputQueued(ed)) renderScreen(ed);
				if (!readTerminalKey(ed, key)) {
					ed.input_ended = true;
					return false;
				}
			}

			if (ed.record_on) ed.recording.push_back(key);
			return true;
		}

		static void printHeadless(const Editor& ed, const std::string& text) {
			if (!ed.term.headless) return;

			std::fflush(stdout);
			std::fprintf(stderr, "%s\n", text.c_str());
			std::fflush(stderr);
		}

		void setMessage(Editor& ed, const std::string& text) {
			ed.message = text;
			ed.message_error = false;
			printHeadless(ed, text);
		}

		void setError(Editor& ed, const std::string& text) {
			ed.message = text;
			ed.message_error = true;
			printHeadless(ed, text);
		}

		void appendMessageLine(Editor& ed, const std::string& text) {
			ed.message = ed.message.empty() ? text : ed.message + "\n" + text;
			ed.message_error = false;
			printHeadless(ed, text);
		}

		bool isModified(const Editor& ed) {
			return ed.state != ed.saved_state || (ed.group_open && !ed.group.changes.empty());
		}

		static std::string crlfLines(const std::string& text) {
			std::string out;
			for (char c : text) {
				if (c == '\n') out += "\r\n";
				else out.push_back(c);
			}

			return out;
		}

		void showLongMessage(Editor& ed) {
			writeTerminal(ed, "\x1b[H\x1b[2J" + crlfLines(ed.message) + "\r\n" + kContinuePrompt);
			ed.message.clear();
			waitForKeyPress(ed);
		}

		void runShell(Editor& ed, const std::string& command) {
			suspendTerminal(ed);
			std::fflush(stdout);
			ed.exec->executeText(command + "\n", "vi");
			std::fflush(stdout);
			if (ed.term.headless) return;

			writeTerminal(ed, std::string("\r\n") + kContinuePrompt);
			waitForKeyPress(ed);
			resumeTerminal(ed);
		}

		std::string makeTempPath(Executor& exec) {
			const std::wstring dir = shellTempDirectory(exec).wstring();
			wchar_t path[MAX_PATH];
			if (dir.empty()) return std::string();
			if (::GetTempFileNameW(dir.c_str(), L"vi", 0, path) == 0) return std::string();
			return wideToUtf8(path);
		}

		static std::string startupCommand(const std::string& arg) {
			const std::string body = arg.substr(1);
			return body.empty() ? "$" : body;
		}

		static bool parseViArgs(const std::vector<std::string>& args, ViArgs& parsed) {
			bool options_done = false;
			for (std::size_t k = 0; k < args.size(); ++k) {
				const std::string& arg = args[k];
				const bool command = !options_done && !arg.empty() && arg[0] == '+';
				if (!command && (options_done || arg.size() < 2 || arg[0] != '-')) {
					parsed.files.push_back(arg);
				} else if (command) {
					parsed.commands.push_back(startupCommand(arg));
				} else if (arg == "--") {
					options_done = true;
				} else if (arg == "-R") {
					parsed.readonly = true;
				} else if (arg == "-c" && k + 1 < args.size()) {
					parsed.commands.push_back(args[++k]);
				} else {
					perr("vi", "unknown option: " + arg);
					return false;
				}
			}

			return true;
		}

		static void runCommandsInGroup(Editor& ed, const std::vector<std::string>& commands) {
			for (const std::string& command : commands) {
				beginUndoGroup(ed);
				runExCommand(ed, command);
				endUndoGroup(ed);
				scrollToCursor(ed);
			}
		}

		static void finishSession(Editor& ed) {
			if (ed.quit || !isModified(ed)) return;

			printHeadless(ed, "vi: input ended with unsaved changes");
			ed.status = 1;
		}

		static int runEditor(Executor& exec, const ViArgs& args) {
			Editor ed;
			ed.exec = &exec;
			ed.files = args.files;
			ed.options.readonly = args.readonly;
			openTerminal(ed);

			if (!ed.files.empty()) loadFile(ed, ed.files[0]);
			runCommandsInGroup(ed, args.commands);
			while (!ed.quit && !ed.input_ended) runNormalCommand(ed);

			finishSession(ed);
			closeTerminal(ed);
			return ed.status;
		}

	}  // namespace vi_detail

	static int startEditor(Executor& exec, const std::vector<std::string>& args, bool readonly) {
		vi_detail::ViArgs parsed;
		parsed.readonly = readonly;
		if (!vi_detail::parseViArgs(args, parsed)) {
			perr("vi", "usage: vi [-R] [-c CMD] [+CMD] [FILE]...");
			return 2;
		}

		return vi_detail::runEditor(exec, parsed);
	}

	static int builtin_vi(Executor& exec, const std::vector<std::string>& args) {
		return startEditor(exec, args, false);
	}

	static int builtin_view(Executor& exec, const std::vector<std::string>& args) {
		return startEditor(exec, args, true);
	}

	void registerViBuiltin(Executor& exec) {
		exec.registerBuiltin("vi", builtin_vi);
		exec.registerBuiltin("view", builtin_view);
	}

}  // namespace wbsh

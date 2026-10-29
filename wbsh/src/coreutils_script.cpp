/**
 * @file coreutils_script.cpp
 * @brief script and unbuffer: running a command on a pseudoconsole of its own.
 *
 * Both give the command a real console however the shell's own streams
 * are connected, as a Unix pty does: `script -qc CMD /dev/null | ...`
 * and `unbuffer CMD | ...` capture what CMD writes when it believes it
 * is interactive. script also records the session to a typescript file,
 * headed and footed as util-linux script does.
 */

#include "coreutils_internal.h"

#include <cerrno>
#include <cstdio>
#include <ctime>
#include <string>
#include <system_error>
#include <vector>

#include "executor.h"
#include "optscan.h"
#include "pathconv.h"
#include "pty_relay.h"

namespace wbsh {

	namespace script_detail {

		enum ScriptOption {
			kScriptAppend = 1, kScriptCommand, kScriptReturn, kScriptFlush, kScriptQuiet,
			kScriptLogIn, kScriptLogOut, kScriptLogIo, kScriptHelp, kScriptVersion,
		};

		struct ScriptOptions {
			bool append = false;
			bool return_status = false;
			bool flush = false;
			bool quiet = false;
			bool help = false;
			bool version = false;
			bool has_command = false;
			std::string command;
			std::string output_path;
			std::string input_path;
		};

		struct ScriptLogs {
			std::FILE* output = nullptr;
			std::FILE* input = nullptr;
		};

	}  // namespace script_detail

	using script_detail::ScriptLogs;
	using script_detail::ScriptOptions;

	static const int kStatusCannotInvoke = 126;
	static const int kStatusNotFound     = 127;

	static const char* const kDefaultTypescript = "typescript";

	static const char* const kScriptUsage =
		"Usage: script [options] [file]\n"
		"Make a typescript of a terminal session: run wbsh, or wbsh -rc with the\n"
		"command given, on a pseudoconsole of its own, and copy everything it\n"
		"prints to the file as well (default 'typescript').\n"
		"\n"
		"Options:\n"
		" -I, --log-in <file>     log stdin to file\n"
		" -O, --log-out <file>    log stdout to file (default)\n"
		" -B, --log-io <file>     log stdin and stdout to file\n"
		" -a, --append            append to the log file\n"
		" -c, --command <command> run command rather than an interactive shell\n"
		" -e, --return            return exit code of the child process\n"
		" -f, --flush             run flush after each write\n"
		" -q, --quiet             be quiet\n"
		" -h, --help              display this help\n"
		" -V, --version           display version\n";

	static const char* const kUnbufferUsage =
		"Usage: unbuffer [-p] program [args]\n"
		"Run program on a pseudoconsole of its own, so it writes as it would to\n"
		"a terminal however its output is redirected, and copy that to stdout.\n"
		"\n"
		" -p  pass stdin on to program, for use inside a pipeline\n";

	static int showUsage(const char* usage) {
		std::fputs(usage, stdout);
		std::fflush(stdout);
		return 0;
	}

	static int showVersion(const char* cmd) {
		std::printf("%s (wbsh) %d.%d.%d\n", cmd, WBSH_VERSION_MAJOR, WBSH_VERSION_MINOR,
			WBSH_VERSION_PATCH);
		std::fflush(stdout);
		return 0;
	}

	static std::string localTimestamp() {
		const std::time_t now = std::time(nullptr);
		std::tm local{};
		localtime_s(&local, &now);

		char stamp[32];
		char zone[8];
		std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
		std::strftime(zone, sizeof(zone), "%z", &local);

		const std::string offset = zone;
		if (offset.size() != 5) return stamp;
		return std::string(stamp) + offset.substr(0, 3) + ":" + offset.substr(3);
	}

	static const std::vector<OptDef>& scriptOptionDefs() {
		static const std::vector<OptDef> defs = {
			{ script_detail::kScriptAppend,  'a', "append",  OptArg::None },
			{ script_detail::kScriptCommand, 'c', "command", OptArg::Required },
			{ script_detail::kScriptReturn,  'e', "return",  OptArg::None },
			{ script_detail::kScriptFlush,   'f', "flush",   OptArg::None },
			{ script_detail::kScriptQuiet,   'q', "quiet",   OptArg::None },
			{ script_detail::kScriptLogIn,   'I', "log-in",  OptArg::Required },
			{ script_detail::kScriptLogOut,  'O', "log-out", OptArg::Required },
			{ script_detail::kScriptLogIo,   'B', "log-io",  OptArg::Required },
			{ script_detail::kScriptHelp,    'h', "help",    OptArg::None },
			{ script_detail::kScriptVersion, 'V', "version", OptArg::None },
		};

		return defs;
	}

	static void applyScriptOption(const OptHit& hit, ScriptOptions& options) {
		switch (hit.id) {
			case script_detail::kScriptAppend:  options.append = true;        break;
			case script_detail::kScriptReturn:  options.return_status = true; break;
			case script_detail::kScriptFlush:   options.flush = true;         break;
			case script_detail::kScriptQuiet:   options.quiet = true;         break;
			case script_detail::kScriptHelp:    options.help = true;          break;
			case script_detail::kScriptVersion: options.version = true;       break;
			case script_detail::kScriptLogIn:   options.input_path = hit.value;  break;
			case script_detail::kScriptLogOut:  options.output_path = hit.value; break;
			case script_detail::kScriptLogIo:
				options.input_path = hit.value;
				options.output_path = hit.value;
				break;
			case script_detail::kScriptCommand:
				options.has_command = true;
				options.command = hit.value;
				break;
			default:
				break;
		}
	}

	static bool parseScriptArgs(const std::vector<std::string>& args, ScriptOptions& options) {
		OptScanResult scanned;
		if (!scanOptions("script", args, scriptOptionDefs(), scanned)) return false;

		for (const OptHit& hit : scanned.hits) applyScriptOption(hit, options);

		if (scanned.operands.size() > 1) {
			perr("script", "unexpected number of arguments");
			return false;
		}

		if (!scanned.operands.empty()) options.output_path = scanned.operands[0];
		const bool no_log_named = options.output_path.empty() && options.input_path.empty();
		if (no_log_named) options.output_path = kDefaultTypescript;
		return true;
	}

	static std::FILE* openLog(Executor& exec, const std::string& path, bool append) {
		std::FILE* log = fopenNative(exec, path, append ? "ab" : "wb");
		if (log == nullptr) perr("script", path, std::error_code(errno, std::generic_category()));
		return log;
	}

	static bool openScriptLogs(Executor& exec, const ScriptOptions& options, ScriptLogs& logs) {
		if (!options.output_path.empty()) {
			logs.output = openLog(exec, options.output_path, options.append);
			if (logs.output == nullptr) return false;
		}

		if (options.input_path.empty()) return true;
		if (options.input_path == options.output_path) {
			logs.input = logs.output;
			return true;
		}

		logs.input = openLog(exec, options.input_path, options.append);
		return logs.input != nullptr;
	}

	static void closeScriptLogs(ScriptLogs& logs) {
		if (logs.input != nullptr && logs.input != logs.output) std::fclose(logs.input);
		if (logs.output != nullptr) std::fclose(logs.output);
	}

	static std::string sessionFields(Executor& exec, const ScriptOptions& options) {
		std::string fields;
		if (options.has_command) fields += "COMMAND=\"" + options.command + "\" ";

		const std::string term = exec.env().get("TERM");
		if (!term.empty()) fields += "TERM=\"" + term + "\" ";

		int columns = 0;
		int rows = 0;
		if (!controllingConsoleSize(columns, rows)) return fields + "<not executed on terminal>";

		const std::string lines = std::to_string(rows);
		return fields + "COLUMNS=\"" + std::to_string(columns) + "\" LINES=\"" + lines + "\"";
	}

	static void writeLogLine(const ScriptLogs& logs, const std::string& line) {
		const bool separate_input = logs.input != nullptr && logs.input != logs.output;
		if (logs.output != nullptr) std::fputs(line.c_str(), logs.output);
		if (separate_input) std::fputs(line.c_str(), logs.input);
	}

	static std::string logDescription(const ScriptOptions& options) {
		const std::string output = "output log file is '" + options.output_path + "'";
		const std::string input = "input log file is '" + options.input_path + "'";
		if (options.input_path.empty()) return output;
		if (options.output_path.empty()) return input;
		if (options.input_path != options.output_path) return output + ", " + input;
		return "log file is '" + options.output_path + "'";
	}

	static void announceStart(const ScriptOptions& options) {
		if (options.quiet) return;

		std::printf("Script started, %s.\n", logDescription(options).c_str());
		std::fflush(stdout);
	}

	static void announceDone(const ScriptOptions& options) {
		if (options.quiet) return;

		std::printf("Script done.\n");
		std::fflush(stdout);
	}

	// util-linux runs $SHELL, but on Windows that is usually some other
	// environment's shell (Git's bash.exe), inherited from wherever wbsh
	// was started; the session's shell is this wbsh instead.
	static std::vector<std::string> sessionArgv(const ScriptOptions& options) {
		std::vector<std::string> argv = { selfExecutablePath() };
		if (!options.has_command) return argv;

		argv.push_back("-r");
		argv.push_back("-c");
		argv.push_back(options.command);
		return argv;
	}

	static bool runScriptSession(Executor& exec, const ScriptOptions& options,
	                             const ScriptLogs& logs, int& status) {
		PtyRun run;
		run.argv = sessionArgv(options);
		run.logs.output = logs.output;
		run.logs.input = logs.input;
		run.logs.flush = options.flush;

		const std::string fields = sessionFields(exec, options);
		writeLogLine(logs, "Script started on " + localTimestamp() + " [" + fields + "]\n");
		announceStart(options);

		status = kStatusCannotInvoke;
		const bool ran = runOnPseudoConsole(exec, run, status);
		const std::string exit_code = "[COMMAND_EXIT_CODE=\"" + std::to_string(status) + "\"]";
		writeLogLine(logs, "\nScript done on " + localTimestamp() + " " + exit_code + "\n");
		announceDone(options);
		return ran;
	}

	static int builtin_script(Executor& exec, const std::vector<std::string>& args) {
		ScriptOptions options;
		if (!parseScriptArgs(args, options)) return 1;
		if (options.help) return showUsage(kScriptUsage);
		if (options.version) return showVersion("script");

		ScriptLogs logs;
		if (!openScriptLogs(exec, options, logs)) {
			closeScriptLogs(logs);
			return 1;
		}

		int status = 0;
		const bool ran = runScriptSession(exec, options, logs, status);
		closeScriptLogs(logs);
		if (!ran) return kStatusCannotInvoke;
		return options.return_status ? status : 0;
	}

	static bool commandExists(Executor& exec, const std::string& name) {
		if (exec.isFunction(name) || exec.isBuiltin(name) || exec.isAlias(name)) return true;
		return !exec.findExecutable(name).empty();
	}

	static int builtin_unbuffer(Executor& exec, const std::vector<std::string>& args) {
		std::size_t first = 0;
		const bool pipeline = !args.empty() && args[0] == "-p";
		if (pipeline) ++first;
		if (first < args.size() && args[first] == "--") ++first;
		if (first >= args.size()) {
			std::fputs(kUnbufferUsage, stderr);
			return 1;
		}

		PtyRun run;
		run.argv.assign(args.begin() + static_cast<std::ptrdiff_t>(first), args.end());
		run.forward_input = pipeline;
		if (!commandExists(exec, run.argv[0])) {
			perr("unbuffer", run.argv[0] + ": command not found");
			return kStatusNotFound;
		}

		int status = 0;
		if (!runOnPseudoConsole(exec, run, status)) return kStatusCannotInvoke;
		return status;
	}

	void registerScriptBuiltins(Executor& exec) {
		exec.registerBuiltin("script",   builtin_script);
		exec.registerBuiltin("unbuffer", builtin_unbuffer);
	}

}  // namespace wbsh

#pragma once

/**
 * @file executor.h
 * @brief AST executor: pipelines, redirection, control flow, builtins,
 *        and the registries the rest of the shell hangs off of.
 *
 * The Executor owns the runtime tables for builtins, functions,
 * aliases, history, the directory stack, traps, jobs, and
 * programmable completion specs. It implements CommandSubstitutor so
 * the Expander can call back into it to evaluate `$(...)` and
 * `<(...)` bodies in the current shell context.
 *
 * Control flow (`break`, `continue`, `return`, `exit`) is propagated
 * as a value: the raising builtin stores a FlowSignal on the
 * Executor, every frame checks flowPending() after running a child,
 * and the frame that owns the construct (loop, function call,
 * subshell, script, top-level driver) consumes the signal.
 */

#include "arena.h"
#include "ast.h"
#include "completion.h"
#include "environment.h"
#include "expander.h"
#include "history.h"
#include "lexer.h"
#include "pathconv.h"
#include "procsubst.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct WbshCompletion;

namespace wbsh {

	/**
	 * @brief Non-local control-flow signal, carried as a value.
	 *
	 * Raised by the `break` / `continue` / `return` / `exit` builtins
	 * (and by `set -e`) instead of throwing. The signal lives on the
	 * Executor; frames between the raise site and the construct that
	 * owns it propagate by returning early, and the owning frame
	 * (loop, function call, subshell, script, top-level driver)
	 * consumes it via Executor::consumeFlow().
	 */
	struct FlowSignal {
		enum class Kind {
			None,       ///< No signal pending — normal execution.
			Break,      ///< `break N` — unwind N enclosing loops.
			Continue,   ///< `continue N` — next iteration of the Nth loop.
			Return,     ///< `return [N]` — unwind to the function call.
			Exit,       ///< `exit [N]` — unwind to the shell driver.
			Discard,    ///< Expansion error — drop the rest of the input line.
		};
		Kind kind = Kind::None;
		int  status = 0;    ///< Return / Exit payload.
		int  count = 1;     ///< Break / Continue: loop levels left to unwind.
		bool errexit = false;   ///< Exit: set -e ended the shell, not `exit`.
	};

	/**
	 * @brief What a loop should do after running its body.
	 *
	 * Result of Executor::dispatchLoopFlow(), the shared loop-body
	 * epilogue that consumes Break / Continue signals aimed at the
	 * current loop (decrementing multi-level `break N` counts).
	 */
	enum class LoopFlowAction {
		Normal,      ///< No signal — fall through to the next iteration.
		NextIter,    ///< `continue` consumed — jump to the next iteration.
		ExitLoop,    ///< `break` consumed — leave the loop normally.
		Propagate,   ///< Signal belongs to an outer frame — unwind.
	};

	class Executor;
	struct CoprocPipes;
	/// Signature of a registered shell builtin / coreutil. Plain function
	/// pointer — every builtin in this codebase is a named static function
	/// with no captured state, so a typed function pointer is enough.
	typedef int (*BuiltinFn)(Executor&, const std::vector<std::string>&);

	/**
	 * @brief A command a third-party util registered through the SDK.
	 *
	 * Kept apart from the builtins so the bundled commands keep their
	 * plain function pointers, and so every one of these can be dropped
	 * again before the DLLs behind them are let go.
	 */
	struct PluginCommand {
		int (*fn)(void* user, int argc, const char* const* argv) = nullptr;
		void (*complete)(void* user, int argc, const char* const* argv,
			WbshCompletion* completion) = nullptr;
		void* user = nullptr;
		std::string summary;
		std::string usage;
	};

	/**
	 * @brief Split a PATH string into directory entries.
	 *
	 * Accepts both `:` (POSIX, wbsh-internal) and `;` separators. A `:`
	 * right after a single alpha char is treated as a Windows drive
	 * letter (`C:\foo`), not a separator.
	 */
	std::vector<std::string> splitPathList(const std::string& path);

	/**
	 * @brief Change the process's working directory and update PWD/OLDPWD.
	 *
	 * Shared by the `cd` builtin and anything else that needs `cd`'s exact
	 * env-var bookkeeping (currently `fzf`'s directory-jump on Enter).
	 *
	 * @param target Path to change into, POSIX-or-Win32 (same convention
	 *               as other builtins). Not resolved via `-`/HOME lookup.
	 * @param[out] err Set to the OS error message on failure.
	 * @return true on success.
	 */
	bool changeDirectory(Executor& exec, const std::string& target, std::string& err,
		bool physical = false);

#ifdef _WIN32
	/// Where spawnCommand starts a child: inside @c job, assigned while the
	/// child is still suspended so nothing it starts can escape the job, and
	/// optionally in a console process group of its own (so a Ctrl+Break
	/// can be aimed at it alone). @c creation_flags go to CreateProcess as
	/// well: a priority class, or CREATE_NO_WINDOW to outlive the console.
	/// With @c pseudo_console the child's console and all three standard
	/// streams are that pseudoconsole, and the shell's stdio is not passed on.
	/// Unless @c outlives_shell (a background job, nohup, setsid) the child
	/// also starts in childLifetimeJob(), so killing the shell ends it.
	struct SpawnPlacement {
		HANDLE job = nullptr;
		bool new_process_group = false;
		bool outlives_shell = false;
		DWORD creation_flags = 0;
		HPCON pseudo_console = nullptr;
		std::string argv0;        ///< `exec -a NAME`: the program's argv[0].
	};

	/// CREATE_BREAKAWAY_FROM_JOB when the shell runs in a job that lets a
	/// child leave it but does not already make it leave, else 0. Breaking
	/// away keeps a host that kills its job on exit (a terminal tab, an IDE)
	/// from taking a command that has to outlive the shell along.
	DWORD jobBreakawayFlag();

	/// Where a command that has to outlive the console starts: a process
	/// group of its own, out of the shell's job, and on a hidden console of
	/// its own unless @p keep_console.
	SpawnPlacement detachedPlacement(bool keep_console);

#endif

	/// After `exec` ran a program: the process ends with that program's own
	/// exit code (an NTSTATUS, or a code past 255) rather than the shell
	/// status it maps to, when it ends with that status.
	void rememberExecExitCode(unsigned long code);

	/// An `exec` inside a subshell that runs in this process ended only
	/// the subshell.
	void forgetExecExitCode();

	/// The exit code for a process whose shell ends with @p status.
	int processExitCode(int status);

	/// @p word as bash shows it for re-use (`set -x`, `declare`): bare when
	/// that is safe, else single-quoted, or `$'...'` with control characters.
	std::string shellQuoteForReuse(const std::string& word);

	/// @p word always quoted, as `${x@Q}` shows it: single-quoted, or
	/// `$'...'` with control characters.
	std::string shellQuoteAlways(const std::string& word);

	/// Whether bash would show @p word as `$'...'`: it holds a control
	/// character or a byte that is not valid UTF-8.
	bool needsAnsiCQuote(const std::string& word);

	/// bash's printable_filename: @p name as given, or `$'...'` when it
	/// holds a byte that would not print.
	std::string printableName(const std::string& name);

	/// `declare -p`'s line for @p name (`declare -a a=([0]="x")`).
	std::string declarationText(const Environment& env, const std::string& name);

	/// `set` / `declare` with no arguments: every variable, then the functions.
	int listShellVariables(Executor& exec);

	/// The element of a pipeline that runs in this process, if any.
	enum class PipelineLocalStage {
		None,
		LastHere,
		FirstInSubshell,
	};

	/**
	 * @brief AST executor and runtime registry.
	 *
	 * Constructed with a borrowed Environment. Owns the live tables
	 * for builtins, functions, aliases, history, traps, jobs,
	 * directory stack, and completion specs. Implements
	 * CommandSubstitutor so the Expander can recursively evaluate
	 * command-substitution bodies.
	 */
	class Executor : public CommandSubstitutor {
	public:
		explicit Executor(Environment& env);

		/**
		 * @brief Top-level entry — execute a parsed program.
		 *
		 * @return Exit status of the last command. When the program
		 *         invoked `exit`, an Exit FlowSignal is left pending
		 *         for the caller (REPL / script driver) to consume
		 *         via consumeFlow().
		 */
		int execute(const Node& root);

		std::string run(const std::string& body) override;
		std::string runRaw(const std::string& body) override;
		std::string runInCurrentShell(const std::string& body) override;
		std::string runForReply(const std::string& body) override;
		std::string startProcessSubstitution(const std::string& body, char direction) override;

		Environment& env()        { return env_; }
		const Environment& env() const { return env_; }
		Expander&    expander()   { return expander_; }
		const PathConv& pathConv() const { return path_conv_; }
		int          lastStatus() const { return last_status_; }
		void         setLastStatus(int s) { last_status_ = s; env_.setLastStatus(s); }

		/// BASH_ALIASES mirrors the alias table.
		/// The lexer expands the aliases defined now, when they expand.
		void lexWithAliases(Lexer& lexer) const {
			if (env_.expand_aliases()) lexer.expandAliases(&aliases_);
		}

		void setAlias(const std::string& name, std::string value) {
			env_.setAssocElement("BASH_ALIASES", name, value);
			aliases_[name] = std::move(value);
			++alias_generation_;
		}
		void unsetAlias(const std::string& name) {
			aliases_.erase(name);
			env_.unsetElement("BASH_ALIASES", 0, name);
			++alias_generation_;
		}
		bool isAlias(const std::string& name) const {
			return aliases_.count(name) != 0;
		}
		std::string aliasValue(const std::string& name) const {
			auto it = aliases_.find(name);
			return (it == aliases_.end()) ? std::string() : it->second;
		}
		const std::unordered_map<std::string, std::string>& aliases() const { return aliases_; }

		// history() and historyStatus() are strictly parallel: same size,
		// same indexing. historyStatus()[i] is the exit code of the most
		// recent execution of history()[i] (0 = success, anything else =
		// failure). Entries loaded from legacy on-disk format default to 0.
		// The list keeps at most HISTSIZE entries.
		const std::vector<std::string>& history() const { return history_.commands; }
		const std::vector<int>& historyStatus() const { return history_.statuses; }
		const std::vector<long long>& historyTimes() const { return history_.times; }
		bool  writesHistoryTimes() const;
		void  addHistoryEntry(std::string line);
		void  eraseHistoryEntries(const std::string& line);
		/// The newest entry becomes @p line, as when `fc` runs a command again.
		void  replaceLastHistoryEntry(std::string line);
		/// Whether the command running now, read at the prompt, is the newest entry.
		bool  currentCommandInHistory() const { return current_in_history_; }
		void  setCurrentCommandInHistory(bool held) { current_in_history_ = held; }
		/// As bash with `-i` given: `-n` holds in the interactive shell too.
		void  keepNoexec() { keeps_noexec_ = true; }
		void  markLastHistoryStatus(int status);
		void  setHistoryEntryStatus(std::size_t index, int status);
		void  clearHistory();
		/// The number `history` shows for the oldest entry.
		std::size_t historyBase() const { return history_base_; }
		void  deleteHistoryEntries(std::size_t first, std::size_t last);
		bool  dropCurrentHistoryEntry();
		// Entries read from a file count as already saved there.
		bool  loadHistoryFromFile(const std::string& path);
		bool  saveHistoryToFile(const std::string& path) const;
		// Writes the whole list, after which every entry counts as saved.
		bool  writeHistory(const std::string& path);
		// Appends the entries added since the last load, write or append.
		bool  appendNewHistory(const std::string& path);

		// dir_stack_.front() is the current directory; entries below are
		// previously-pushed dirs (POSIX form).
		std::vector<std::string>& dirStack() { return dir_stack_; }
		const std::vector<std::string>& dirStack() const { return dir_stack_; }

		struct Job {
			int  id = 0;                ///< Job ID (`%N`).
#ifdef _WIN32
			HANDLE handle = nullptr;    ///< Win32 process handle.
#else
			void* handle = nullptr;     ///< Opaque process handle (POSIX placeholder).
#endif
			long long pid = 0;          ///< OS process id.
			std::string cmd_text;       ///< Original command text for `jobs` output.
			bool running = true;        ///< False once the process has exited.
			int  exit_code = 0;         ///< Exit code (valid once `running == false`).
			bool stopped = false;       ///< Suspended by Ctrl+Z or `kill -STOP`.
			unsigned long long recency = 0; ///< When last started or stopped: the latest is %+.
			bool with_job_control = true;   ///< Started while set -m or interactive.
			bool inherited = false;         ///< The parent shell's, listed but not ours to run.
#ifdef _WIN32
			HANDLE group = nullptr;     ///< Job object holding every process of the job.
			std::vector<HANDLE> stages; ///< A pipeline's other processes.
#endif
		};
		/// Add a launched background process.  @return the assigned job id.
		int  registerJob(void* handle, long long pid, std::string cmd_text);
		/// A job of the parent shell, as a self-spawned subshell inherits it.
		void adoptInheritedJob(const Job& job);
		/// Update statuses of finished jobs.  @return true if any newly finished.
		bool reapJobs();
		std::vector<Job>& jobsTable() { return jobs_; }
		/// bash's current (`%+`) and previous (`%-`) jobs: the two that
		/// started or stopped last.
		const Job* currentJob() const;
		const Job* previousJob() const;
		/// Wait for one job; returns its exit status, or -1 if id unknown.
		/// A job that was waited for leaves the table, as in bash. When
		/// @p interruptible (the `wait` builtin), a posted signal that is not
		/// ignored ends the wait with 128 + its number, the job left running.
		int  waitForJob(int id, bool interruptible = false);
		/// 0, or 128 + the signal that ended an interruptible wait.
		int  waitForAllJobs(bool interruptible = false);
		/// `wait -n`: the status of the next job to finish, of those @p only
		/// names or of all, or -1 with none.
		int  waitForAnyJob(const std::vector<int>& only = {});
		/// Ctrl+Z and stopped jobs exist only in an interactive shell on a console.
		bool jobControlActive() const;
		/// `fg`: resume a stopped job and wait for it in the foreground.
		int  foregroundJob(int id);
		/// `bg`: resume a stopped job in the background.  @return false if not stopped.
		bool continueJobInBackground(int id);
		/// `kill -STOP` / `-CONT` / `-TERM` ... on a whole job.
		bool signalJob(Job& job, int signum);
		/// A builtin `sleep` stopped by Ctrl+Z goes on as a stopped job.
		int  stopRemainingSleep(unsigned long milliseconds, const std::string& text);
		/// Ends jobs left stopped when the interactive shell exits.
		void endStoppedJobs();
		/// `%N`, `%%`, `%+`, `%`, `%-`, `%name`, `%?text`; nullptr if no job matches.
		Job* findJob(const std::string& spec);
		/// The status of a finished background process that left the table.
		bool finishedJobStatus(long long pid, int& status) const;
		/// `[N]+  Done  cmd` for every job that ended since the last report;
		/// reported jobs leave the table.
		void reportFinishedJobs(std::FILE* out);
		std::string jobLine(const Job& job) const;
		void forgetJob(int id);
		int  nextJobId() const;
		int  waitForFirstRunningJob(const std::vector<int>& only);
		/// Ctrl+C at an interactive prompt's command: stops the loop or
		/// list running it with status 130, as SIGINT does in bash.
		bool stopForCtrlC(int& status) const;
		/// `wait PID` for a `<(...)` / `>(...)` process; false if @p pid is not one.
		bool waitForProcSubstChild(long long pid, int& status);
		void waitForProcSubstChildren();
		/// `eval` re-reads its words, which would strip the backslashes of a
		/// `\\.\pipe\...` path that `<(...)` put there; this doubles them.
		std::string protectProcSubstPaths(std::string text) const;

		/// Install the trap action for @p sig. Replaces any prior action.
		///
		/// `EXIT` fires when a script, `-c` string, subshell, command
		/// substitution or the REPL ends, `ERR` after a failed command,
		/// `DEBUG` before a command, `RETURN` when a function or `source`
		/// ends; a signal's trap when handlePendingSignals() takes it, and INT
		/// on Ctrl+C in the REPL.
		void setTrap(const std::string& sig, std::string cmd) {
			if (startup_ignored_.count(sig) != 0) return;

			trap_handlers_[sig] = std::move(cmd);
			lists_inherited_traps_ = false;
			publishSignalDispositions();
		}
		void clearTrap(const std::string& sig) {
			if (startup_ignored_.count(sig) != 0) return;

			trap_handlers_.erase(sig);
			lists_inherited_traps_ = false;
			publishSignalDispositions();
		}
		bool hasTrap(const std::string& sig) const {
			return trap_handlers_.count(sig) != 0;
		}

		/// Acts on each signal another wbsh (or this one) posted, lowest
		/// number first: runs its trap, ignores it, or raises the exit its
		/// default action means. Called at the shell's safe points.
		void handlePendingSignals();

		/// With job control on and a CHLD trap set, bash runs the trap once
		/// for every child that exits; this counts one such exit.
		void noteChildExited();

		/// `ulimit -n`: from this descriptor up none can be made, as on
		/// Linux; negative for no limit.
		void setOpenFileLimit(long long limit) { open_file_limit_ = limit; }

		/// Whether @p signum would be ignored now: `trap ''`, or no trap and
		/// a signal this shell ignores by default (CHLD, CONT, URG, WINCH;
		/// INT, QUIT and TERM too when interactive).
		bool signalIgnored(int signum) const;

		/// Drops the pending signals that would be ignored, so that they wake
		/// no wait, and returns the lowest one left (0 for none).
		int interruptingSignal();

		/// Tells the mailbox which signals this shell traps or ignores, so
		/// that one it does neither ends the process even while a long
		/// builtin keeps the shell from its safe points. Called whenever the
		/// trap table changes.
		void publishSignalDispositions() const;
		std::string trapAction(const std::string& sig) const {
			auto it = trap_handlers_.find(sig);
			return it == trap_handlers_.end() ? std::string() : it->second;
		}
		/// What `trap` lists. A subshell does not run its parent's traps but
		/// still lists them until it changes one, so `saved=$(trap)` works.
		const std::unordered_map<std::string, std::string>& listedTraps() const {
			return lists_inherited_traps_ ? inherited_traps_ : trap_handlers_;
		}
		/// `trap -- 'action' NAME` lines for what `trap` lists, so a
		/// self-spawned subshell can list them too (WBSH_TRAPS).
		std::string serializeListedTraps() const;
		/// Replay a parent's WBSH_TRAPS: listed, but only ignored signals
		/// (and ERR / DEBUG / RETURN under errtrace / functrace) stay active.
		void adoptInheritedTraps(const std::string& text);
		// Run (and clear, so it fires once) the EXIT trap for a shell that
		// is exiting with @p status. Returns the status to exit with: the
		// trap's own `exit N`, otherwise @p status.
		int  fireExitTrap(int status);
		// `exit` without an operand: inside the EXIT trap that is the status
		// the shell was exiting with, not the trap's last command.
		int  defaultExitStatus() const {
			if (inTrapReturnContext()) return trap_return_context_.status;
			return in_exit_trap_ ? exit_trap_status_ : last_status_;
		}
		int  defaultReturnStatus() const {
			return inTrapReturnContext() ? trap_return_context_.status : last_status_;
		}
		bool inTrapReturnContext() const {
			return trap_return_context_.depth == func_depth_ + source_depth_;
		}

		// Internal getopts state: we keep the per-arg subindex across calls.
		int& getoptsSubindex() { return getopts_subindex_; }
		void resetGetopts() { getopts_subindex_ = 1; }
		/// bash restarts getopts inside an argument whenever OPTIND is
		/// assigned by anyone but getopts itself.
		void syncGetoptsWithOptind() {
			if (env_.optindWrites() != getopts_seen_writes_) resetGetopts();
			getopts_seen_writes_ = env_.optindWrites();
		}
		void noteGetoptsWroteOptind() { getopts_seen_writes_ = env_.optindWrites(); }

		using CompletionSpec = wbsh::CompletionSpec;
		void setCompletionSpec(const std::string& cmd, CompletionSpec spec) {
			const auto existing = completion_specs_.find(cmd);
			spec.defined_at = existing != completion_specs_.end() ? existing->second.defined_at
				: ++completion_specs_defined_;
			completion_specs_[cmd] = std::move(spec);
		}
		void removeCompletionSpec(const std::string& cmd) {
			completion_specs_.erase(cmd);
		}
		const CompletionSpec* completionSpec(const std::string& cmd) const {
			auto it = completion_specs_.find(cmd);
			return it == completion_specs_.end() ? nullptr : &it->second;
		}
		const std::unordered_map<std::string, CompletionSpec>& completionSpecs() const {
			return completion_specs_;
		}

		void registerBuiltin(std::string name, BuiltinFn fn);
		bool registerPluginCommand(std::string name, PluginCommand command);
		void clearPluginCommands() { plugin_commands_.clear(); }
		const PluginCommand* pluginCommand(const std::string& name) const {
			auto it = plugin_commands_.find(name);
			return it == plugin_commands_.end() ? nullptr : &it->second;
		}
		std::vector<std::string> pluginCommandNames() const {
			std::vector<std::string> v;
			v.reserve(plugin_commands_.size());
			for (const auto& kv : plugin_commands_) v.push_back(kv.first);
			return v;
		}
		/// A builtin `enable -n` turned off is not a builtin for the shell.
		bool isBuiltin(const std::string& name) const;
		/// Registered as a builtin, enabled or not (what `enable` accepts).
		bool isKnownBuiltin(const std::string& name) const;
		void setBuiltinEnabled(const std::string& name, bool enabled);
		const std::unordered_set<std::string>& disabledBuiltins() const {
			return disabled_builtins_;
		}
		bool isFunction(const std::string& name) const;
		/// The enabled builtins.
		std::vector<std::string> builtinNames() const;
		/// Every builtin, enabled or not.
		std::vector<std::string> allBuiltinNames() const;
		std::vector<std::string> functionNames() const {
			std::vector<std::string> v;
			v.reserve(functions_.size());
			for (const auto& kv : functions_) v.push_back(kv.first);
			return v;
		}
		int  callBuiltin(const std::string& name, const std::vector<std::string>& args);
		int  callFunction(const std::string& name, const std::vector<std::string>& args);

		// Re-entry from `source` / `.` and similar. The caller owns the
		// AST and keeps it alive for the lifetime of the call.
		int executeText(const std::string& source_text, const std::string& origin,
		                std::size_t first_line = 1);

		// Run text the shell reads as input (a script, a REPL line, `eval`,
		// `source`): under `set -v` each line is echoed to stderr as bash
		// reads it, before the command on it runs.
		int executeInputText(const std::string& source_text, const std::string& origin,
		                     std::size_t first_line = 1);
		int executeInput(const Node& root, const std::string& source_text);

		// bash parses input a line at a time, so a line read after the
		// posix option changed is parsed by the new setting: when that
		// happened, run the rest of @p source_text, lexed again.
		struct RelexInput {
			const std::string&              origin;
			std::size_t                     first_line;
			const std::string&              lexed_source;
			const std::vector<AliasSplice>& splices;
		};
		int relexRestOfInput(const std::string& source_text, const RelexInput& input, int status);
		bool relexPending() const { return relex_offset_ != std::string::npos; }
		/// Runs the rest of a script a line at a time, as bash reads it,
		/// once the history option is on.
		using LineRunner = int (*)(Executor& exec, const std::string& text,
			const std::string& origin, std::size_t first_line);
		void setLineRunner(LineRunner runner) { line_runner_ = runner; }

		// Hand AST ownership to the executor by adopting the Arena that
		// backs it (see Parser::takeArena). Used by the REPL: each line
		// is parsed locally, but functions defined there must outlive
		// the iteration that ran them. Multi-call safe; arenas accumulate.
		void adoptArena(Arena arena) {
			owned_arenas_.push_back(std::move(arena));
		}

		// How many function definitions have run so far. A caller that
		// parsed text itself keeps its arena (adoptArena) only when this
		// count changed while the text ran.
		std::size_t functionDefinitionCount() const { return function_definitions_; }

		// The original source the AST was parsed from. Used by the pipeline
		// executor to spawn child wbsh processes for non-external elements.
		void setSourceText(std::string s) { source_text_ = std::move(s); }
		const std::string& sourceText() const { return source_text_; }

		// Concatenated `name() <body>` definitions for every currently-
		// registered function. Used to populate WBSH_FUNCTIONS for child
		// shells. Empty string when no functions are defined.
		std::string serializeFunctions() const;

		// `alias name='value'` lines (single-quote escaped). Populates
		// WBSH_ALIASES so self-spawned children pick them up.
		std::string serializeAliases() const;

		// Indexed / associative array assignments rendered as runnable
		// shell. Populates WBSH_ARRAYS so self-spawned children rebuild
		// the parent's array state. Without this, pipeline self-spawn
		// drops arrays — only scalars survive the env-block round-trip.
		std::string serializeArrays() const;

		// `declare -ilur name` lines for the variables with attributes a
		// value does not carry. Populates WBSH_ATTRIBUTES.
		std::string serializeAttributes() const;

		int  loopDepth() const  { return loop_depth_; }
		int  funcDepth() const  { return func_depth_; }
		long long lastWaitedPid() const { return last_waited_pid_; }
		void forgetLastWaitedPid() { last_waited_pid_ = -1; }
		bool canReturn() const {
			return func_depth_ > 0 || source_depth_ > 0 || inherited_return_context_;
		}
		void inheritReturnContext() { inherited_return_context_ = true; }
		/// The line the running simple command's text ends on, where bash
		/// starts counting the lines of an `eval` string.
		int  commandEndLine() const { return command_end_line_; }
		/// POSIX mode in a non-interactive shell: the errors that end the shell.
		bool posixErrorsExit() const;
		/// A `.`, `source` or `eval` error that ends a POSIX-mode shell
		/// (a missing file, a syntax error), as bash's special_builtin_failed.
		void markSpecialBuiltinFailure() { special_builtin_failed_ = true; }

		// errexit suppression — incremented while we're inside a context
		// where `set -e` should NOT trigger (if/while conditions, left
		// side of && / ||, bang-prefixed pipelines).
		int  errexitSuppress() const { return errexit_suppress_; }
		void pushErrexitSuppress() { ++errexit_suppress_; }
		void popErrexitSuppress()  { --errexit_suppress_; }

		/// Is a control-flow signal pending? Frames that don't own the
		/// signal check this after running a child and return early.
		bool flowPending() const { return flow_.kind != FlowSignal::Kind::None; }
		const FlowSignal& flow() const { return flow_; }
		void clearFlow() { flow_ = FlowSignal{}; }
		void raiseBreak(int count) {
			flow_.kind = FlowSignal::Kind::Break;
			flow_.status = 0;
			flow_.count = count;
		}
		void raiseContinue(int count) {
			flow_.kind = FlowSignal::Kind::Continue;
			flow_.status = 0;
			flow_.count = count;
		}
		void raiseReturn(int status) {
			flow_.kind = FlowSignal::Kind::Return;
			flow_.status = status;
			flow_.count = 1;
		}
		void raiseExit(int status) {
			flow_.kind = FlowSignal::Kind::Exit;
			flow_.status = status;
			flow_.count = 1;
			flow_.errexit = false;
		}
		void raiseErrexit(int status) {
			raiseExit(status);
			flow_.errexit = true;
		}
		void raiseDiscard() {
			flow_.kind = FlowSignal::Kind::Discard;
			flow_.status = 1;
			flow_.count = 1;
		}
		/**
		 * @brief Consume the pending signal if it is of kind @p k.
		 *
		 * @param payload Optional: receives the signal's status.
		 * @return true when a signal of kind @p k was pending (now
		 *         cleared); false otherwise (signal left untouched).
		 */
		bool consumeFlow(FlowSignal::Kind k, int* payload = nullptr) {
			if (flow_.kind != k) return false;
			if (payload) *payload = flow_.status;
			clearFlow();
			return true;
		}
		/// CommandSubstitutor: tell the Expander to stop expanding when
		/// a `$(...)` body raised break / continue / return / exit.
		bool interrupted() const override { return flowPending(); }
		bool hasPendingExit() const { return flow_.kind == FlowSignal::Kind::Exit; }

		PromptCounts promptCounts() const override {
			return { jobs_.size(), history_.commands.size() + 1, command_number_ };
		}
		/// One more command read at the prompt, as `\#` counts them.
		void advanceCommandNumber() { ++command_number_; }

		// Scoped variable declaration. Used by the `local` builtin: records
		// the prior state of `name` so it can be restored when the current
		// function scope is popped, then assigns `value`.
		/// False outside a function. A new local starts out unset, keeping
		/// only the export attribute of the variable it shadows.
		bool makeLocal(const std::string& name, bool inherit = false);
		bool localInherits(const std::string& name) const;
		bool isTempEnvName(const std::string& name) const;
		std::set<std::string> takePersistedTempNames();
		/// A function further up the call chain made @p name local.
		bool isCallerLocal(const std::string& name) const;
		bool isLocalHere(const std::string& name) const;
		/// The running function did `local -`.
		bool hasLocalOptions() const;
		/// What a child process gets for @p name: its exported value, or the
		/// exported value an unset local hides. False when it gets nothing.
		bool exportedValue(const std::string& name, std::string& out) const;
		void appendShadowedExports(std::vector<std::pair<std::string, std::string>>& out) const;
		/// A prefix assignment `name=v typeset name` made a local: what the
		/// local hides is the variable as it was before that assignment.
		void hideBehindLocal(const std::string& name, bool existed, const std::string& value,
		                     bool exported);
		/// The prefix assignments of the function or builtin call running
		/// now: a `local` of one of those names starts with its value.
		std::size_t pushTempEnvNames(const std::vector<std::pair<std::string, std::string>>& assigns);
		void popTempEnvNames(std::size_t size) { temp_env_names_.resize(size); }
		/// `declare -g` inside a function: @p body runs with every local
		/// set aside, so it reads and writes the global variables.
		int runInGlobalScope(const std::function<int()>& body);
		bool unsetCallerLocal(const std::string& name);
		std::vector<std::string> localNames() const;

		/// Assign `name=(...)` or `name+=(...)`, expanding the items.
		bool assignCompound(const std::string& text);
		/// `local -a a=( "${a[@]}" )` expands its items before the local hides
		/// the outer `a`; this is that first half, the second applies them.
		bool expandCompound(const std::string& text, std::function<void()>& apply);
		/// Which arguments of the running declaration builtin were written
		/// as `name=(...)`, by argv index. Taken once, by that builtin.
		bool typedAsArrayRef(const std::string& arg) const {
			return std::find(array_ref_words_.begin(), array_ref_words_.end(), arg)
				!= array_ref_words_.end();
		}
		std::vector<bool> takeCompoundArgs() {
			std::vector<bool> taken;
			taken.swap(compound_args_);
			return taken;
		}
		/// bash's this_command_name between commands: the function just
		/// called until its first simple command ends, otherwise empty.
		const std::string& thisCommandName() const { return this_command_name_; }

		const FunctionDef* findFunction(const std::string& name) const;
		bool unsetFunction(const std::string& name);

		/// `export -f`: the function reaches child processes as `BASH_FUNC_name%%`.
		void exportFunction(const std::string& name) { exported_functions_.insert(name); }
		void unexportFunction(const std::string& name) { exported_functions_.erase(name); }
		bool isFunctionExported(const std::string& name) const {
			return exported_functions_.count(name) != 0;
		}
		/// `declare -ft`: the function inherits the DEBUG and RETURN traps.
		void setFunctionTraced(const std::string& name, bool traced) {
			if (traced) traced_functions_.insert(name);
			else        traced_functions_.erase(name);
		}
		bool isFunctionTraced(const std::string& name) const {
			return traced_functions_.count(name) != 0;
		}
		/// `readonly -f`: the function can be neither redefined nor unset.
		void setFunctionReadonly(const std::string& name) { readonly_functions_.insert(name); }
		bool isFunctionReadonly(const std::string& name) const {
			return readonly_functions_.count(name) != 0;
		}
		/// The `BASH_FUNC_name%%=() { ... }` entries a child process gets.
		void appendExportedFunctions(std::vector<std::pair<std::string, std::string>>& out) const;
		/// Defines the functions the environment's `BASH_FUNC_name%%` entries carry.
		void importEnvironmentFunctions();

		int  sourceDepth() const { return source_depth_; }
		/// eval, `source`, a trap action and a command substitution each
		/// nest one level deeper in the xtrace prefix.
		/// `set -x` line for one test of a `[[ ]]`: operators and operands as
		/// given, an empty operand as ''.
		void traceConditional(const std::vector<std::string>& parts);
		void enterTraceLevel() { ++xtrace_level_; }
		void leaveTraceLevel() { --xtrace_level_; }
		/// Enter / leave a `source`d @p file: also its FUNCNAME / BASH_SOURCE frame.
		void beginSource(const std::string& file);
		void endSource();

		/// The `main` frame of FUNCNAME / BASH_SOURCE for a shell running script @p path.
		void beginScriptFrame(const std::string& path);
		/// A self-spawned subshell rebuilds its parent's call stack from the
		/// FUNCNAME / BASH_SOURCE / BASH_LINENO arrays it inherited.
		void adoptInheritedCallStack();
		/// Run the RETURN trap: a function or a `source`d file is ending.
		void fireReturnTrap();
		/// A name line and a file line per function defined in a file (WBSH_FUNCTION_FILES).
		std::string serializeFunctionFiles() const;
		void adoptInheritedFunctionFiles(const std::string& text);

		// Run argv as a resolved command (function, builtin, or external) with
		// `assigns` in its environment only, the way a `NAME=value cmd` prefix does.
		int  runResolvedCommand(
			const std::vector<std::string>& argv,
			const std::vector<std::pair<std::string, std::string>>& assigns);

		// Run argv[0] from PATH even when a builtin or function shares its
		// name; `temp_env` is added to the child's environment only.
		int  runExternal(const std::vector<std::string>& argv,
		                 const std::vector<std::pair<std::string, std::string>>& temp_env);

#ifdef _WIN32
		// Start argv as its own process on the current stdio without waiting:
		// an external directly, a builtin, function or alias through a child
		// wbsh that inherits this shell's state. The caller owns the returned
		// handle; INVALID_HANDLE_VALUE (already reported) when nothing started.
		HANDLE spawnCommand(const std::vector<std::string>& argv,
		                    const SpawnPlacement& placement = SpawnPlacement());

		// Start the program at `exe_path` with `command_line` as it stands and
		// exactly `environment` (PATH and HOME go out in Win32 form) on the
		// current stdio, without waiting; a child wbsh is told it is one.
		// INVALID_HANDLE_VALUE (already reported) when nothing started.
		HANDLE spawnWithEnvironment(const std::string& exe_path, const std::string& command_line,
		                            const std::vector<std::pair<std::string, std::string>>& environment);
#endif

		// Walk PATH for an executable named `name`. Returns the resolved
		// path (UTF-8) or empty if not found. Honours Windows PATHEXT-style
		// .exe/.cmd/.bat resolution. Used by `type`, `command -v`, etc.
		std::string findExecutable(const std::string& name);
		std::string searchPath() const;
		/// `/usr/bin/env`, `/bin/sh`: a Linux path that names no file here is
		/// the bare NAME (a builtin, or found on PATH). Other words as given.
		/// Covers /bin, /sbin, /usr/bin, /usr/sbin and /usr/local/bin.
		std::string systemBinCommand(const std::string& word);
		/// Every PATH match for `name`, in PATH order (`type -a`).
		std::vector<std::string> findAllExecutables(const std::string& name);

		// The PATH walk behind findExecutable(); a hit is memoized.
		std::string lookupInPathDirs(const std::string& name, const std::string& path);

		// Drop the PATH-lookup memoization built up by findExecutable().
		// Used by `hash -r` to force re-resolution after PATH-visible
		// executables change on disk (installs, PATHEXT edits, etc.).
		void clearExecutablePathCache() {
			exec_path_cache_.clear();
			hashed_.clear();
			hash_hits_.clear();
			env_.resetAssocArray("BASH_CMDS");
		}
		/// `hash`: names remembered with the path they run from, as shown;
		/// BASH_CMDS mirrors them.
		void hashCommand(const std::string& name, std::string path) {
			env_.setAssocElement("BASH_CMDS", name, path);
			hashed_[name] = std::move(path);
			hash_hits_.erase(name);
		}
		bool unhashCommand(const std::string& name) {
			env_.unsetElement("BASH_CMDS", 0, name);
			hash_hits_.erase(name);
			return hashed_.erase(name) != 0;
		}
		/// As bash's hash table, every lookup that finds a name counts.
		void noteHashHit(const std::string& name) { ++hash_hits_[name]; }
		int hashHits(const std::string& name) const {
			const auto found = hash_hits_.find(name);
			return found == hash_hits_.end() ? 0 : found->second;
		}
		/// Takes up what a script assigned to BASH_ALIASES or BASH_CMDS.
		void adoptShellTableArrays();
		void adoptAliasArray();
		const std::map<std::string, std::string>& hashedCommands() const { return hashed_; }

		// Pre-expanded assignment forms used by execSimpleCommand. Built from
		// the SimpleCommand AST's `assignments` list before the command runs.
		// Public because the implementation file's small helper functions
		// (applyArrayAssignToEnv etc.) need to consume them by name.
		struct ScalarAssign {
			std::string name;
			std::string value;
			bool append = false;    ///< true for `name+=value`
		};
		struct ElemAssign {
			std::string name;
			Word subscript;
			std::string value;
			bool append = false;    ///< true for `name[k]+=value`
		};
		struct ArrayItem {
			bool has_key = false;  ///< written `[key]=value`
			std::string key;
			std::string value;
			bool append = false;   ///< written `[key]+=value`
			std::string word;      ///< an unkeyed item as written, for errors
		};
		struct ArrayAssign  {
			std::string name;
			bool sparse = false;   ///< true iff any keyed item is present
			bool append = false;   ///< true for `name+=(...)`
			std::vector<ArrayItem> items;  ///< in source order
		};
		struct SimpleCmdAssigns {
			std::vector<ScalarAssign> scalar;
			std::vector<ElemAssign>   elem;
			std::vector<ArrayAssign>  array;

			// Flat (name,value) view of just the scalar entries — kept because
			// some downstream paths (function / builtin / external prefix)
			// still consume that older format.
			std::vector<std::pair<std::string, std::string>> scalarPairs() const {
				std::vector<std::pair<std::string, std::string>> out;
				out.reserve(scalar.size());
				for (const auto& s : scalar) out.emplace_back(s.name, s.value);
				return out;
			}
		};

	private:
		struct InputEcho {
			const List*        list = nullptr;
			const std::string* text = nullptr;
			std::size_t        cursor = 0;
			bool               lexed_posix = false;
			unsigned long long lexed_aliases = 0;
		};

		// The arena owns the tree; it moves to owned_arenas_ only when the
		// text defined a function, whose body must outlive the run.
		struct ParsedText {
			Node* root = nullptr;
			std::string diagnostics;
			Arena arena;
			std::size_t resume_offset = std::string::npos;
			std::string lexed_source;
			std::vector<AliasSplice> splices;
		};

		/// One FUNCNAME / BASH_SOURCE / BASH_LINENO entry: a function, a
		/// `source`d file or the script itself (`main`).
		struct CallFrame {
			std::string name;
			std::string source;
			int line = 0;
			bool function = false;
		};
		bool inFunctionFrame() const;
		bool pushBashArgv(const std::vector<std::string>& args);
		void popBashArgv(bool pushed, std::size_t count);
		void pushCallFrame(CallFrame frame);
		void popCallFrame();
		void publishCallStack();
		std::string currentSourceFile() const;
		std::string functionFile(const std::string& name) const;
		int  runFunctionBody(const FunctionDef& def);
		bool functionNestingAllowed(const std::string& name);
		bool reportDirectoryCommand(const std::string& name) const;
		std::string findExecutableWith(const std::string& name,
		                               const std::vector<std::pair<std::string, std::string>>& temp_env);

		const ParsedText& parsedTrapAction(const std::string& action);
		int  runTrapAction(const std::string& action);
		bool runsAsPosixSpecialBuiltin(const std::string& name) const;
		int  runPosixSpecialBuiltin(const std::string& name, const std::vector<std::string>& args,
		                            const std::vector<std::pair<std::string, std::string>>& assigns);
		bool rejectSpecialBuiltinFunction(const FunctionDef& fd);
		bool abandonsAfterAssignError(const std::string& command);
		bool expandUnsplitRedirWord(const Word& target, std::string& out);
		int  runTrapPublishingSignal(const std::string& action, int trap_number);
		int  runNotFoundHandler(const std::vector<std::string>& argv);
		static bool assignThroughCycle(void* context, const std::string& name,
		                               const std::string& value);
		static bool isFunctionLocal(void* context, const std::string& name);
		static bool readThroughCycle(void* context, const std::string& name, std::string& out);
		int  nodeLine(const Node& node) const;
		int  firstWordEndLine(const SimpleCommand& sc) const;
		int  substitutionBaseLine() const;
		int  parseBaseLine() const;
		bool readsByLines() const;
		bool runsHashedPath(const std::vector<std::string>& argv,
		                    const std::vector<std::pair<std::string, std::string>>& assigns,
		                    int& status);
	public:
		void enterInheritedStage(const Node& root);
	private:
		int  runExitTrapText(const std::string& action);

		// ERR fires where errexit is judged, once per failure: a compound
		// command whose status came from an already judged inner command,
		// or from the left of `&&` / `||` or a `!`, is not judged again.
		bool handleCommandFailure(int status);
		void fireErrTrap(int status);
		int  finishLeafCommand(int status);
		int  negateBangStatus(int status);
		void setPipeStatus(const std::vector<int>& statuses);

		// A function inherits ERR only under errtrace, DEBUG and RETURN only
		// under functrace; so do subshells and `$(...)`.
		struct SuspendedTrap {
			const char* name;
			std::string action;
			bool kept;
		};
		bool inheritsTrap(const std::string& name) const;
		SuspendedTrap suspendTrap(const char* name, bool traced);
		std::vector<SuspendedTrap> suspendTrapsForFunction(bool traced);
		void restoreTrap(SuspendedTrap& trap);
		void restoreTrapsAfterFunction(std::vector<SuspendedTrap>& suspended);

		bool trapActionFor(const char* name, std::string& action) const;
		int  runTrapKeepingStatus(const std::string& action, int trap_number);
		void actOnSignal(int signum);
#ifdef _WIN32
		void endForegroundOnFatalSignal(const std::vector<HANDLE>& processes);
#endif
		bool handling_signals_ = false;
		int exited_children_ = 0;
		long long open_file_limit_ = -1;
		bool refusesFdAtLimit(int fd, const Redirection& redir);
		bool running_child_traps_ = false;
		void runChildExitTraps();
		bool runsAsProgram(const std::string& name) const;
		bool fireDebugTrap();
		// Sets BASH_COMMAND to @p text and runs the DEBUG trap before it;
		// false when that trap says to skip the command.
		bool announceCommand(std::string text);
		bool announceNode(const Node& node);
		bool announceLeaf(const Node& node);
		std::string loopHeaderText(const ForClause& clause) const;
		bool evalLoopArith(const std::string& expr, long long& value);
		ParsedText parseText(const std::string& source_text, const std::string& origin,
		                     std::size_t first_line = 1);
		int finishParsedText(const ParsedText& parsed, int status);
		int resumeAfterDiscardedCommand(const ParsedText& parsed, const std::string& source_text,
		                                const std::string& origin, std::size_t first_line);
		void keepArenaIfDefined(ParsedText& parsed, std::size_t definitions_before);
		void  echoInputBefore(const List& list, std::size_t index);
		void  echoInputUpTo(std::size_t end);

		int execNode(const Node& n);
		bool nodeTracksLineno(const Node& node) const;
		int dispatchNode(const Node& n);
		void exitOnFatalExpansionError();
		void discardAfterExpansionError();
		bool dropRestOfInputLine(const List& list, std::size_t& index);
		bool stopToRelex(const List& list, std::size_t index);
		int execList(const List& l);
		int execAndOr(const AndOr& a);
		int execPipeline(const Pipeline& p);
		int execSimpleCommand(const SimpleCommand& sc);
		int runSimpleCommand(const SimpleCommand& sc);
		int execBraceGroup(const BraceGroup& bg);
		int execSubshell(const Subshell& ss);
		int execIf(const IfClause& ic);
		int execWhile(const WhileClause& wc);
		int execFor(const ForClause& fc);
		int execForArith(const ForClause& fc);
		int execSelect(const ForClause& fc);
		bool expandForWordList(const ForClause& fc, std::vector<std::string>& values,
		                       int* out_status);
		bool loopVariableIsValid(const ForClause& fc);
		int execCase(const CaseClause& cc);
		bool functionNameAccepted(const FunctionDef& fd);
		bool rejectsReadonlyFunction(const FunctionDef& fd);
		bool assignLoopVariable(const std::string& name, const std::string& value);
		int execFunctionDef(const FunctionDef& fd);
		int execDBracket(const DBracketCond& dc);
		int execArithCommand(const ArithCommand& ac);
		int execCoproc(const CoprocCommand& coproc);
#ifdef _WIN32
		void publishCoproc(const std::string& name, const CoprocPipes& pipes,
		                   HANDLE process);
#endif

		// Loop-body epilogue: consume a Break / Continue signal aimed at
		// the current loop and report what the loop should do next.
		LoopFlowAction dispatchLoopFlow();

		// Run a construct's body and keep its status unless a flow signal
		// is now pending; a missing body counts as status 0.
		void runBody(const Node* body, int& status);
		int  runIfBranches(const IfClause& clause);
		int  runWhileLoop(const WhileClause& clause);
		int  runArithLoop(const ForClause& clause);
		int  runForLoop(const ForClause& clause);
		int  runSelectLoop(const ForClause& clause);
		bool caseItemMatches(const CaseClause::Item& item, const std::string& subject, bool& failed);
		int  runCaseItems(const CaseClause& clause, const std::string& subject);
		std::size_t runCaseArmsFrom(const CaseClause& clause, std::size_t first, int& status);

		struct RedirState {
			// Saved fds: target_fd -> dup'd backup fd, or -1 if target was closed
			std::vector<std::pair<int, int>> saved;
			// Temp files to delete after the command finishes.
			std::vector<std::string> temps;
		};
		bool applyRedirections(const std::vector<Redirection>& rs, RedirState& out);
		bool applyRedirection(const Redirection& redir, int target, RedirState& state);
		bool installRedirection(const Redirection& redir, int target, RedirState& state);
		bool applyFdVariableRedir(const Redirection& redir, RedirState& state);
		std::string fdVariableValue(const std::string& variable);
		bool closeFdVariable(const std::string& variable);
		bool assignFdVariable(const std::string& variable, int fd);
		void exitIfStdoutReaderGone();
		void undoRedirections(RedirState& s);
		// `exec` redirections made inside an in-process subshell or script
		// are recorded here and undone when it ends, as a child's fds would
		// close with it.
		void beginExecRedirJournal() { exec_redir_journal_.emplace_back(); }
		void endExecRedirJournal();
		void keepExecRedirs(const RedirState& state);
		// The copies this shell saves while setting up what bash would run
		// in a forked child (a subshell's redirections, `$(...)`, a pipeline
		// stage run here) are closed fds in that child, so `>&N` there
		// cannot reach them. The mark undoes the hiding, innermost first.
		std::size_t hideSavedFds(const RedirState& state);
		void showHiddenFds(std::size_t mark) { hidden_fds_.resize(mark); }
		bool savedFdHidden(int fd) const;
		// applyRedirections, but a failure also undoes whatever was
		// installed so the caller can return at once.
		bool applyRedirectionsOrUndo(const std::vector<Redirection>& rs, RedirState& out);
		bool applyCompoundRedirections(const std::vector<Redirection>& redirs, RedirState& state);

		// Helpers for applyRedirections — one per redirection-op family. Each
		// either installs the redirection (saving the prior fd into `s`) and
		// returns true, or prints a diagnostic and returns false.
		void saveFd(RedirState& s, int fd) const;
		void evacuateSavedCopy(int fd) const;
		int  takeSavedCopy(int copy, std::vector<int>* vacated = nullptr) const;
		// Expand a redirection target word. On expansion failure prints a
		// `wbsh: <msg>` diagnostic and returns false (the redirection —
		// and with it the command — then fails with status 1).
		bool expandRedirTarget(const Word& target, std::string& out);
		// Open a virtual `/dev/{stdin,stdout,stderr,fd/N}` path as a fresh fd,
		// or return -1 if `path` is not a recognised dev-fd alias.
		static int dupSpecialDevFd(const std::string& path);
		// Open a `/dev/tcp/HOST/PORT` or `/dev/udp/HOST/PORT` socket and wrap
		// it as a CRT fd. Returns -1 on no-match or any failure.
		static int openTcpUdpStream(const std::string& path);
		// Composite of the two above: try special-fd dispatch first, then
		// fall back to opening `path` (translated via PathConv) on the FS.
		int openRedirSourceFd(const std::string& path, int flags) const;
		int openProcRedirSource(const std::string& path, int flags) const;
		// Open `path`, dup it onto `target`, save the prior fd into `s`, and
		// print a `wbsh: <path>: <err>` diagnostic on failure.
		bool redirectFdFromPath(const std::string& path, int flags,
		                        int target, RedirState& s);
		// Spill `body` into a temp file, redirect `target` to read from it,
		// and remember the temp path so undoRedirections cleans it up.
		bool installRedirFromTempBody(std::string body, int target, RedirState& s);
		bool applyLessRedir       (const Redirection& r, int target, RedirState& s);
		bool refusesClobber(const std::string& path) const;
		bool applyTruncOrClobber  (const Redirection& r, int target, RedirState& s);
		bool applyAppendRedir     (const Redirection& r, int target, RedirState& s);
		bool applyAmpRedir        (const Redirection& r, int extra_flags, RedirState& s);
		bool applyDupWord(const Redirection& r, const std::string& word, RedirState& s);
		bool redirectOutputAndErrors(const std::string& path, int extra_flags, RedirState& s);
		bool applyLessGreatRedir  (const Redirection& r, int target, RedirState& s);
		bool applyDupRedir        (const Redirection& r, int target, RedirState& s);
		void reportBadDupSource(const Redirection& r, int target);
		void closeRedirFd(int fd, RedirState& s);
		void forgetCoprocFd(int fd);
		bool applyHeredocRedir    (const Redirection& r, int target, RedirState& s);
		bool applyHerestringRedir (const Redirection& r, int target, RedirState& s);
		void warnHeredocAtEof     (const Redirection& r);

		// Helpers for execSimpleCommand. Each returns true / a status; on a
		// recoverable expansion error they print a diagnostic and return false
		// (or the appropriate non-zero status).
		bool expandSimpleCmdAssigns(const SimpleCommand& sc, SimpleCmdAssigns& out, bool bare);
		bool expandOneAssign(const Assignment& assignment, SimpleCmdAssigns& out, bool bare,
		                     std::vector<std::pair<std::string, Environment::VarState>>& prior);
		bool expandSimpleCmdArgv(const SimpleCommand& sc, std::vector<std::string>& argv,
		                         std::vector<bool>& compound);
		std::string expandDeclarationArg(const DeclarationArg& arg);
		// `exec` with no command: install the redirections on the current
		// shell permanently and return the resulting status.
		int  execBareRedirsForExec(const std::vector<Redirection>& redirs);
		void applyBareAssignmentsToShell(const SimpleCmdAssigns& a);
		int  execSimpleCommandNoArgv(const SimpleCommand& sc, SimpleCmdAssigns& assigns_data);
		void traceXtrace(const std::vector<std::string>& argv);
		// `set -x` lines for what is not a simple command (executor_xtrace.cpp).
		void traceText(const std::string& text);
		std::string expandedPs4();
		std::string traceIndent();
		void traceAssignment(const Assignment& assignment, const SimpleCmdAssigns& expanded);
		void traceLoopHeader(const char* keyword, const ForClause& clause);
		void traceCaseHeader(const CaseClause& clause);
		bool evalTracedArith(const std::string& expr, bool loop_part, long long& value);
		void checkXtraceFdAssignment(const SimpleCmdAssigns& expanded);
		void dropRestrictedHashAssignments(SimpleCmdAssigns& expanded);
	public:
		/// A restricted shell hashes only commands that a PATH search finds.
		bool restrictedHashTargetAllowed(const std::string& target, const char* label);
	private:

		// State snapshotted at the start of a shell-script invocation
		// (`./script.sh` and similar). Restored after the script returns,
		// regardless of whether it returned normally, called `exit`, or
		// left a flow signal pending. The asymmetry between the normal-
		// and propagating-signal restore (set vs forceSet) is preserved
		// from the original exception-based code.
		struct ShellScriptScope {
			std::unordered_map<std::string, std::string> saved_vars;
			std::vector<std::string> saved_pos;
			std::string saved_name;
			std::unordered_map<std::string, const FunctionDef*> saved_funcs;
			std::unordered_map<std::string, std::string> saved_function_files;
			std::set<std::string> saved_exported_functions;
			std::vector<CallFrame> saved_call_stack;
			std::unordered_map<std::string, std::string> saved_traps;
			std::set<std::string> saved_startup_ignored;
			bool saved_lists_inherited_traps = false;
			SetOptions saved_options;
			bool saved_expand_aliases = false;
			std::filesystem::path saved_cwd;
		};
		ShellScriptScope snapshotShellScriptScope() const;
		void resetTrapsForScript();
		void restoreShellScriptScope(ShellScriptScope& snap, bool force_set);

		// A `( ... )`, `$(...)` or `<(...)` body runs in this process, so
		// everything it can change is snapshotted on entry and put back on
		// the way out, as if it had been a forked child.
		/// `local -` is the entry named `-`; it saves the `set` options.
		struct ScopeEntry {
			std::string name;
			Environment::VarState saved;
			SetOptions options;
			int getopts_subindex = 1;
		};
		ScopeEntry* outermostLocal(const std::string& name);

		struct SubshellScope {
			Environment env;
			std::unordered_map<std::string, const FunctionDef*> functions;
			std::unordered_map<std::string, std::string> function_files;
			std::unordered_map<std::string, std::string> aliases;
			std::unordered_set<std::string> disabled_builtins;
			std::unordered_map<std::string, std::string> traps;
			std::unordered_map<std::string, std::string> inherited_traps;
			bool lists_inherited_traps = false;
			std::vector<std::string> dir_stack;
			std::vector<std::vector<ScopeEntry>> scope_stack;
			int getopts_subindex = 1;
			std::filesystem::path cwd;
			std::vector<int> owned_jobs;
		};
		SubshellScope enterSubshell();
		std::vector<int> lendJobsToSubshell();
		void takeBackJobs(const std::vector<int>& owned);
		void resetSubshellTraps();
		void leaveSubshell(SubshellScope& snap);
		int endSubshell(int status);
		int consumeSubshellFlow(int status);
		int runExitTrapAction(const std::string& action, int status);
		void dropErrexitForSubstitution();
		int runCommandSubstitutionBody(const std::string& body);
		int executeSubstitutionText(const std::string& body, std::size_t first_line);
		int writeFileToStdout(const Redirection& redir);
		int runSubstitutionInCurrentShell(const std::string& body);
		int runSubstitutionBody(const std::string& body, bool in_current_shell);
		std::string finishSubstitution(std::string out, int status);
		std::string captureSubstitution(const std::string& body, int& status,
			bool in_current_shell = false);

#ifdef _WIN32
		// Build the argv passed to CreateProcess: replaces argv[0] with the
		// resolved exec_path and POSIX-to-Win32-translates each arg unless
		// the callee is an MSYS binary, or WBSH_NO_PATHCONV / MSYS_NO_PATHCONV
		// is set (in the shell env OR in @p temp_env's prefix assignments).
		// MSYS2_ARG_CONV_EXCL keeps the args it names as they are.
		std::vector<std::string>
		prepareExternalArgv(const std::vector<std::string>& argv,
		                    const std::string& exec_path,
		                    const std::vector<std::pair<std::string, std::string>>& temp_env);
		// Build env overrides for a Win32 child, layered on top of `temp_env`.
		// Translates PATH to `;`-separated Win32 form and adds HOME in Win32
		// 8.3 short form so MinGW tools with diacritics in profile paths work.
		std::vector<std::pair<std::string, std::string>>
		prepareExternalEnvOverrides(
			const std::vector<std::pair<std::string, std::string>>& temp_env);

		// Run CreateProcessW + Wait + GetExitCodeProcess; on success returns
		// true and writes the exit status to *exit_status. On failure
		// returns false; the caller can inspect GetLastError() (still valid
		// because we didn't call any other Win32 API after the failure).
		bool spawnExternalAndWait(const std::wstring& exe,
		                          std::wstring& cmdline,
		                          std::wstring& envblock,
		                          int* exit_status,
		                          const std::string& job_text);
#endif

		// Heuristic: does the file at `path` look like a shell script we
		// should interpret in-process rather than handing to CreateProcess?
		bool looksLikeShellScript(const std::string& path) const;
		/// The file starts with `#!`: a shell script, or a script for the
		/// interpreter its first line names. Not for CreateProcess.
		bool startsWithShebang(const std::string& path) const;

		// `#!INTERP ARGS` naming a program that is not a shell: run INTERP
		// with ARGS, the script and argv's arguments, as the kernel does.
		int runShebangInterpreter(const std::string& path,
		                          const std::vector<std::string>& argv,
		                          const std::vector<std::pair<std::string, std::string>>& temp_env);

		// Read `path` and execute its contents in-process with subshell-style
		// isolation (env / positional / $0 / function table / CWD all reset
		// on return). Returns the script's exit status.
		int runShellScript(const std::string& path,
		                   const std::vector<std::string>& argv,
		                   const std::vector<std::pair<std::string, std::string>>& temp_env);
		std::string scriptName(const std::string& typed, const std::string& path) const;

#ifdef _WIN32
		// Multi-command branch of execPipeline (the n>=2 case). Sets up the
		// inter-stage pipes, launches each element, and waits for completion.
		// Returns the resolved pipeline status (pipefail-aware) BEFORE any
		// `! pipeline` negation by the caller.
		int execPipelineMultiCmd(const Pipeline& p);
		bool runsLastStageHere() const;
		int runLastStageHere(const Node& stage, HANDLE input);
		bool runsFirstStageInSubshell() const;
		PipelineLocalStage chooseLocalStage() const;
		int runFirstStageInSubshell(const Node& stage, HANDLE output, bool merge_stderr);
		int runLocalStage(const Pipeline& pipeline, PipelineLocalStage local, HANDLE pipe_end);

		// `cmd &` — spawn a detached child sharing our std{in,out,err} fds,
		// register it as a job, announce `[id] pid` to stderr when interactive.
		// Returns 0 on success, 1 if the spawn failed.
		int launchBackgroundCommand(const Node& cmd);

		// Pipeline element launch. Returns a process HANDLE the parent waits
		// on, or INVALID_HANDLE_VALUE on failure; then launch_status_ has
		// the element's status (127 not found, 126 cannot run, 1 otherwise).
		HANDLE launchPipelineElement(const Node& elem,
		                             HANDLE h_in, HANDLE h_out, HANDLE h_err);
		int launch_status_ = 1;
		// Fast-path branch of launchPipelineElement: try to launch `elem` as
		// a direct external (skipping the intermediate `wbsh.exe -r -c ...`
		// shell). Returns the spawned HANDLE on success, INVALID_HANDLE_VALUE
		// on a hard failure (command-not-found etc.), or sets `*tried = false`
		// and returns INVALID_HANDLE_VALUE when the element is not eligible
		// for the fast path and we should fall through to the self-spawn.
		HANDLE tryDirectExternalLaunch(const Node& elem,
		                               HANDLE h_in, HANDLE h_out, HANDLE h_err,
		                               bool* tried);
		bool   runsInsideShell(const std::string& name) const;
		HANDLE spawnShellCommand(const std::vector<std::string>& argv,
		                         const SpawnPlacement& placement);
		// Re-enter wbsh on the original source slice that produced this AST
		// node. Used as the fallback when a pipeline element is a builtin,
		// function, alias, or anything else that can't run as a separate exe.
		HANDLE selfSpawnPipelineElement(const Node& elem,
		                                HANDLE h_in, HANDLE h_out, HANDLE h_err);
		// Append `<<DELIM` heredoc bodies onto a self-spawn slice. Required
		// because the SimpleCommand's source span ends at the delimiter
		// word — the body lives on later lines and would be lost otherwise.
		void   appendHeredocBodiesToSlice(const SimpleCommand& sc,
		                                  std::string& slice);
		// Env-block overrides passed to a self-spawned child shell so it
		// inherits our full state (translated PATH, serialised functions
		// and aliases, non-exported var marker).
		std::vector<std::pair<std::string, std::string>> buildSelfSpawnOverrides();
		void addSubshellState(std::vector<std::pair<std::string, std::string>>& overrides);
		std::vector<std::string> selfSpawnArgv(const std::string& self,
		                                       const std::string& command) const;
		HANDLE launchExternalRedirected(const SimpleCommand& command,
		                                const std::vector<std::string>& argv,
		                                const std::string& exec_path);
		HANDLE launchExternalDirect(const SimpleCommand& sc,
		                            const std::vector<std::string>& argv,
		                            const std::string& exec_path,
		                            HANDLE h_in, HANDLE h_out, HANDLE h_err);
		HANDLE spawnSelfCommand(const std::string& command_text,
		                        HANDLE h_in, HANDLE h_out, HANDLE h_err);
		SpawnPlacement backgroundPlacement() const { return spawn_placement_; }
		HANDLE startForegroundGroup();
		void   endForegroundGroup(HANDLE group);
		int    stopForegroundJob(std::vector<HANDLE> processes, HANDLE group,
		                         const std::string& text);
		int    waitForegroundProcesses(std::vector<HANDLE>& processes, HANDLE group,
		                               const std::string& text, std::vector<int>& statuses);
		int    waitForegroundJob(Job& job);
		void   printJobState(const Job& job) const;
		void   rememberProcSubstChild(HANDLE process);

		// Process substitutions started while a node runs are released
		// once it is done (see releaseProcSubstRelay).
		std::size_t procSubstWatermark() const { return proc_substs_.size(); }
		void releaseProcessSubstitutionsSince(std::size_t watermark);
		// `exec 4< <(cmd)` keeps the pipe open past the command, as bash
		// keeps the fd.
		void keepProcessSubstitutionsSince(std::size_t watermark);
		std::vector<ProcSubstRelay> proc_substs_;
		std::vector<ProcSubstRelay> kept_proc_substs_;
		std::vector<HANDLE> proc_subst_children_;
#else
		std::size_t procSubstWatermark() const { return 0; }
		void releaseProcessSubstitutionsSince(std::size_t) {}
		void keepProcessSubstitutionsSince(std::size_t) {}
#endif
		bool isAbsoluteOrRelativePath(const std::string& name) const;
		bool patternMatches(const std::string& pat, const std::string& s);
		// Pop the top frame off `scope_stack_` and restore each entry's prior
		// value (or unset if the name didn't exist before). Called when a
		// function body finishes for any reason.
		void popLocalScope();
		void runExitTrapInFunction();
		void restoreLocalOptions(const SetOptions& options);
		void restoreGetoptsPosition(int subindex);

		Environment& env_;
		Expander expander_;
		PathConv path_conv_;
		std::string source_text_;
		std::unordered_map<std::string, BuiltinFn> builtins_;
		std::unordered_map<std::string, PluginCommand> plugin_commands_;
		std::unordered_set<std::string> disabled_builtins_;
		std::unordered_map<std::string, const FunctionDef*> functions_;
		std::set<std::string> exported_functions_;
		std::set<std::string> traced_functions_;
		std::set<std::string> readonly_functions_;
		bool importEnvironmentFunction(const std::string& name, const std::string& value);
		std::unordered_map<std::string, std::string> function_files_;
		std::vector<CallFrame> call_stack_;
		std::unordered_map<std::string, std::string> aliases_;
		HistoryList history_;
		std::size_t history_saved_ = 0;
		std::size_t history_base_ = 1;
		std::vector<std::string> dir_stack_;
		std::unordered_map<std::string, std::string> trap_handlers_;
		// Signals a script found ignored when it started: as in bash, it
		// can neither trap nor reset them.
		std::set<std::string> startup_ignored_;
		std::vector<std::string> temp_env_names_;
		unsigned long long alias_generation_ = 0;
		int input_command_end_line_ = 0;
		long long last_waited_pid_ = -1;
		unsigned long long job_recency_ = 0;
		void touchJob(Job& job) { job.recency = ++job_recency_; }
		int definitionErrorLine(const FunctionDef& fd) const;
		unsigned long long aliasState() const;
		void relexAfterSoleCommand(const Node& command, const std::string& source_text);
		std::set<std::string> persisted_temp_names_;

		struct CoprocFds {
			std::string name;
			int read_fd = -1;
			int write_fd = -1;
			long long pid = 0;
		};
		CoprocFds coproc_;
		void noteJobEnded(long long pid);
		void disposeCoproc();
		void publishCoprocArray(const std::string& name, int read_fd, int write_fd);
		std::unordered_map<std::string, std::string> inherited_traps_;
		bool lists_inherited_traps_ = false;
		std::unordered_map<std::string, ParsedText> trap_asts_;
		const std::string* frozen_lineno_source_ = nullptr;
		std::vector<RedirState> exec_redir_journal_;
		std::vector<int> hidden_fds_;
		bool in_exit_trap_ = false;
		int  exit_trap_status_ = 0;
		std::unordered_map<std::string, CompletionSpec> completion_specs_;
		unsigned long long completion_specs_defined_ = 0;
		// findExecutable() memoizes name -> resolved path here (bash-style
		// command hash) so repeated invocations of the same command skip
		// re-walking PATH and re-probing the filesystem. Invalidated
		// wholesale whenever PATH itself changes.
		std::unordered_map<std::string, std::string> exec_path_cache_;
		std::map<std::string, int> hash_hits_;
		std::string exec_path_cache_path_;
		std::map<std::string, std::string> hashed_;
		std::vector<Job> jobs_;
		std::unordered_map<long long, int> finished_job_statuses_;
		// Set while a `&` command is being started: it gets its own
		// process group, so the console's Ctrl+C does not reach it.
#ifdef _WIN32
		SpawnPlacement spawn_placement_;
#endif
		int getopts_subindex_ = 1;
		unsigned long long getopts_seen_writes_ = 0;
		// Arenas of parsed text that defined functions (`source`, `eval`,
		// inherited `WBSH_FUNCTIONS`, ...), so the raw FunctionDef* pointers
		// in `functions_` stay valid. Text that defined none is freed once
		// it has run; keeping every arena grew by one per `$(...)`.
		std::vector<Arena> owned_arenas_;
		std::size_t function_definitions_ = 0;
		// Length of the alias value that leads the next text parseText reads.
		std::vector<std::vector<ScopeEntry>> scope_stack_;
		int  last_status_ = 0;
		// The status of the last `$(...)` since the current simple command
		// started expanding; a command with no name returns it.
		bool cmdsubst_ran_ = false;
		bool stage_function_bump_ = false;
		int command_first_line_ = 0;
		LineRunner line_runner_ = nullptr;
		bool by_lines_ = false;
		bool lines_requested_ = false;
		std::unordered_map<const Node*, int> line_overrides_;
		// Active redirections of fd 1, so a failed write is blamed on the
		// pipe this process was started with only when nothing replaced it.
		mutable int stdout_redirects_ = 0;
		// The descriptors holding what a redirection replaced, and where
		// one went when a later redirection took its number (`exec 10>&1`
		// inside `{ ...; } > file` must not lose the saved stdout).
		mutable std::set<int> saved_copies_;
		mutable std::map<int, int> moved_saved_copies_;
		bool stdout_started_as_pipe_ = false;
		// The stdout_redirects_ depth at which fd 1 is that pipe: 0, or
		// deeper while a pipeline element runs here writing into its pipe.
		int  sigpipe_redirects_ = 0;
		int  cmdsubst_status_ = 0;
		int  loop_depth_ = 0;
		int  func_depth_ = 0;
		bool inherited_return_context_ = false;
		// Set when a call went past FUNCNEST or the end of the stack; calls
		// fail at once until the outermost function returns.
		bool nesting_exceeded_ = false;
		int  source_depth_ = 0;
		int  xtrace_level_ = 1;
		struct TrapReturnContext {
			int depth = -1;
			int status = 0;
		};
		TrapReturnContext trap_return_context_;
		TrapReturnContext enterTrapReturnContext(int trap_number, int status);
		// Scripts started through a `#!` interpreter that are still running
		// (Linux stops at 4 levels of interpreter scripts).
		int  shebang_depth_ = 0;
		std::vector<bool> compound_args_;
		std::vector<std::string> array_ref_words_;
		std::string this_command_name_;
		int  errexit_suppress_ = 0;
		bool status_judged_ = false;
		bool in_err_trap_ = false;
		int  trap_depth_ = 0;
		bool running_debug_trap_ = false;
		bool running_return_trap_ = false;
		std::vector<SuspendedTrap> source_debug_traps_;
		int  command_end_line_ = 0;
		bool special_builtin_failed_ = false;
		bool prefix_assign_failed_ = false;
		bool call_set_err_trap_ = false;
		InputEcho input_echo_;
		std::size_t relex_offset_ = std::string::npos;
		FlowSignal flow_;
		std::size_t command_number_ = 1;
		bool current_in_history_ = false;
		bool keeps_noexec_ = false;
	};

	void registerCoreBuiltins(Executor& exec);
	void registerDeclarationBuiltins(Executor& exec);
	void registerReadBuiltin(Executor& exec);
	void registerUlimitBuiltin(Executor& exec);
	void registerEnableBuiltin(Executor& exec);
	/// `.`, `:`, `break`, `eval`, `export`, ... : the builtins POSIX calls special.
	bool isPosixSpecialBuiltin(const std::string& name);
	void registerTrapBuiltin(Executor& exec);
	void registerFcBuiltin(Executor& exec);
	void registerTestBuiltins(Executor& exec);
	void registerCoreutils(Executor& exec);

}  // namespace wbsh

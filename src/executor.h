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
 * by throwing the LoopBreak / LoopContinue / FunctionReturn /
 * ShellExit tag types, which are caught at the appropriate enclosing
 * frame.
 */

#include "ast.h"
#include "environment.h"
#include "expander.h"
#include "pathconv.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace wbsh {

	/// Thrown by `break N;` to unwind out of N enclosing loops.
	struct LoopBreak       { int count = 1; };
	/// Thrown by `continue N;` to skip to the next iteration of an outer loop.
	struct LoopContinue    { int count = 1; };
	/// Thrown by `return [N]` from inside a function body.
	struct FunctionReturn  { int status = 0; };
	/// Thrown by the `exit` builtin to unwind to the top-level driver.
	struct ShellExit       { int status = 0; };

	class Executor;
	/// Signature of a registered shell builtin / coreutil. Plain function
	/// pointer — every builtin in this codebase is a named static function
	/// with no captured state, so a typed function pointer is enough.
	typedef int (*BuiltinFn)(Executor&, const std::vector<std::string>&);

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
		/// Construct over @p env (borrowed; must outlive the Executor).
		explicit Executor(Environment& env);

		/**
		 * @brief Top-level entry — execute a parsed program.
		 *
		 * @return Exit status of the last command, or whatever
		 *         `exit` set if it was invoked.
		 */
		int execute(const Node& root);

		/// CommandSubstitutor: run a script body and capture stdout.
		std::string run(const std::string& body) override;
		/// CommandSubstitutor: like run(), but preserves trailing bytes.
		std::string runRaw(const std::string& body) override;

		/// Borrowed reference to the Environment.
		Environment& env()        { return env_; }
		/// Borrowed reference to the Expander used by execution.
		Expander&    expander()   { return expander_; }
		/// Read-only POSIX↔Windows path translator.
		const PathConv& pathConv() const { return path_conv_; }
		/// `$?` — exit status of the last command.
		int          lastStatus() const { return last_status_; }
		/// Update `$?`.
		void         setLastStatus(int s) { last_status_ = s; env_.setLastStatus(s); }

		// ---- Aliases ----
		/// Define / replace an alias. Used by the `alias` builtin.
		void setAlias(const std::string& name, std::string value) { aliases_[name] = std::move(value); }
		void unsetAlias(const std::string& name)                  { aliases_.erase(name); }
		bool isAlias(const std::string& name) const               { return aliases_.count(name) != 0; }
		std::string aliasValue(const std::string& name) const {
			auto it = aliases_.find(name);
			return (it == aliases_.end()) ? std::string() : it->second;
		}
		const std::unordered_map<std::string, std::string>& aliases() const { return aliases_; }

		// ---- History ----
		const std::vector<std::string>& history() const { return history_; }
		void  addHistoryEntry(std::string line);
		void  clearHistory() { history_.clear(); }
		bool  loadHistoryFromFile(const std::string& path);
		bool  saveHistoryToFile(const std::string& path) const;

		// ---- Directory stack (pushd/popd/dirs) ----
		// dir_stack_.front() is the current directory; entries below are
		// previously-pushed dirs (POSIX form).
		std::vector<std::string>& dirStack() { return dir_stack_; }
		const std::vector<std::string>& dirStack() const { return dir_stack_; }

		// ---- Job control ----
		/// Tracked background job entry.
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
		};
		/// Add a launched background process.  @return the assigned job id.
		int  registerJob(void* handle, long long pid, std::string cmd_text);
		/// Update statuses of finished jobs.  @return true if any newly finished.
		bool reapJobs();
		/// Live job table.
		std::vector<Job>& jobsTable() { return jobs_; }
		/// Wait for one job; returns its exit status, or -1 if id unknown.
		int  waitForJob(int id);
		/// Block until every running job has exited.
		void waitForAllJobs();

		// ---- Trap handlers ----
		/// Install the trap action for @p sig. Replaces any prior action.
		///
		/// Only `EXIT` is currently fired by the REPL; signal-driven
		/// traps need real signal plumbing on Windows (deferred).
		void setTrap(const std::string& sig, std::string cmd) { trap_handlers_[sig] = std::move(cmd); }
		void clearTrap(const std::string& sig)                { trap_handlers_.erase(sig); }
		bool hasTrap(const std::string& sig) const            { return trap_handlers_.count(sig) != 0; }
		std::string trapAction(const std::string& sig) const {
			auto it = trap_handlers_.find(sig);
			return it == trap_handlers_.end() ? std::string() : it->second;
		}
		const std::unordered_map<std::string, std::string>& trapHandlers() const { return trap_handlers_; }
		// Fire (and clear so it doesn't loop) the EXIT trap, if any.
		void fireExitTrap();

		// Internal getopts state: we keep the per-arg subindex across calls.
		int& getoptsSubindex() { return getopts_subindex_; }
		void resetGetopts() { getopts_subindex_ = 1; }

		// ---- Programmable completion (`complete` / `compgen`) ----
		/// Per-command completion spec registered via the `complete` builtin.
		struct CompletionSpec {
			std::vector<std::string> words;     ///< `-W` word list.
			std::string function;               ///< `-F` function name.
			std::string command;                ///< `-C` command to run for candidates.
			bool include_files = false;         ///< `-f`.
			bool include_dirs = false;          ///< `-d`.
			bool default_fallback = false;      ///< `-o default`.
			bool plusdirs = false;              ///< `-o plusdirs`.
			bool nospace = false;               ///< `-o nospace`.
		};
		void setCompletionSpec(const std::string& cmd, CompletionSpec spec) {
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
		bool isBuiltin(const std::string& name) const;
		bool isFunction(const std::string& name) const;
		// Used by completion: enumerate registered builtin / function names.
		std::vector<std::string> builtinNames() const {
			std::vector<std::string> v;
			v.reserve(builtins_.size());
			for (const auto& kv : builtins_) v.push_back(kv.first);
			return v;
		}
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
		int executeText(const std::string& source_text, const std::string& origin);

		// Hand AST ownership to the executor. Used by the REPL: each line
		// is parsed locally, but functions defined there must outlive the
		// iteration that ran them. Multi-call safe; ASTs accumulate.
		void adoptAst(NodePtr root) {
			if (root) owned_asts_.push_back(std::move(root));
		}

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

		// Loop / function bookkeeping (for builtins to use).
		int  loopDepth() const  { return loop_depth_; }
		int  funcDepth() const  { return func_depth_; }

		// errexit suppression — incremented while we're inside a context
		// where `set -e` should NOT trigger (if/while conditions, left
		// side of && / ||, bang-prefixed pipelines).
		int  errexitSuppress() const { return errexit_suppress_; }
		void pushErrexitSuppress() { ++errexit_suppress_; }
		void popErrexitSuppress()  { --errexit_suppress_; }

		// Scoped variable declaration. Used by the `local` builtin: records
		// the prior state of `name` so it can be restored when the current
		// function scope is popped, then assigns `value`.
		void declareLocal(const std::string& name, const std::string& value);

		// Walk PATH for an executable named `name`. Returns the resolved
		// path (UTF-8) or empty if not found. Honours Windows PATHEXT-style
		// .exe/.cmd/.bat resolution. Used by `type`, `command -v`, etc.
		std::string findExecutable(const std::string& name);

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
		struct ArrayAssign  {
			std::string name;
			bool sparse = false;   ///< true iff any keyed item is present
			bool append = false;   ///< true for `name+=(...)`
			std::vector<std::pair<std::string, std::string>> keyed;   // key,value
			std::vector<std::string> items;                            // unkeyed
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
		// AST execution.
		int execNode(const Node& n);
		int execList(const List& l);
		int execAndOr(const AndOr& a);
		int execPipeline(const Pipeline& p);
		int execSimpleCommand(const SimpleCommand& sc);
		int execBraceGroup(const BraceGroup& bg);
		int execSubshell(const Subshell& ss);
		int execIf(const IfClause& ic);
		int execWhile(const WhileClause& wc);
		int execFor(const ForClause& fc);
		int execCase(const CaseClause& cc);
		int execFunctionDef(const FunctionDef& fd);
		int execDBracket(const DBracketCond& dc);

		// Helpers.
		struct RedirState {
			// Saved fds: target_fd -> dup'd backup fd
			std::vector<std::pair<int, int>> saved;
			// Files opened during this redirection batch (so we can close them).
			std::vector<int> opened;
			// Temp files to delete after the command finishes.
			std::vector<std::string> temps;
		};
		bool applyRedirections(const std::vector<Redirection>& rs, RedirState& out);
		void undoRedirections(RedirState& s);

		// Helpers for applyRedirections — one per redirection-op family. Each
		// either installs the redirection (saving the prior fd into `s`) and
		// returns true, or prints a diagnostic and returns false.
		void saveFd(RedirState& s, int fd) const;
		// Open a virtual `/dev/{stdin,stdout,stderr,fd/N}` path as a fresh fd,
		// or return -1 if `path` is not a recognised dev-fd alias.
		static int dupSpecialDevFd(const std::string& path);
		// Open a `/dev/tcp/HOST/PORT` or `/dev/udp/HOST/PORT` socket and wrap
		// it as a CRT fd. Returns -1 on no-match or any failure.
		static int openTcpUdpStream(const std::string& path);
		// Composite of the two above: try special-fd dispatch first, then
		// fall back to opening `path` (translated via PathConv) on the FS.
		int openRedirSourceFd(const std::string& path, int flags) const;
		// Open `path`, dup it onto `target`, save the prior fd into `s`, and
		// print a `wbsh: <path>: <err>` diagnostic on failure.
		bool redirectFdFromPath(const std::string& path, int flags,
		                        int target, RedirState& s);
		// Spill `body` into a temp file, redirect `target` to read from it,
		// and remember the temp path so undoRedirections cleans it up.
		bool installRedirFromTempBody(std::string body, int target, RedirState& s);
		bool applyLessRedir       (const Redirection& r, int target, RedirState& s);
		bool applyTruncOrClobber  (const Redirection& r, int target, RedirState& s);
		bool applyAppendRedir     (const Redirection& r, int target, RedirState& s);
		bool applyAmpGreatRedir   (const Redirection& r, RedirState& s);
		bool applyAmpDGreatRedir  (const Redirection& r, RedirState& s);
		bool applyLessGreatRedir  (const Redirection& r, int target, RedirState& s);
		bool applyDupRedir        (const Redirection& r, int target, RedirState& s);
		bool applyHeredocRedir    (const Redirection& r, int target, RedirState& s);
		bool applyHerestringRedir (const Redirection& r, int target, RedirState& s);

		// Helpers for execSimpleCommand. Each returns true / a status; on a
		// recoverable expansion error they print a diagnostic and return false
		// (or the appropriate non-zero status).
		bool expandSimpleCmdAssigns(const SimpleCommand& sc, SimpleCmdAssigns& out);
		bool expandSimpleCmdArgv(const SimpleCommand& sc, std::vector<std::string>& argv);
		void aliasExpandArgvHead(std::vector<std::string>& argv);
		// `exec` with no command: install the redirections on the current
		// shell permanently and return the resulting status.
		int  execBareRedirsForExec(const std::vector<Redirection>& redirs);
		// No-command form: just apply the assignments to the current shell.
		void applyBareAssignmentsToShell(const SimpleCmdAssigns& a);
		// `set -x` trace announcement to stderr.
		void traceXtrace(const std::vector<std::string>& argv);

		// State snapshotted at the start of a shell-script invocation
		// (`./script.sh` and similar). Restored after the script returns,
		// regardless of whether it returned normally, called `exit`, or
		// threw out. The asymmetry between the normal- and exception-path
		// restore (set vs forceSet) is preserved from the original code.
		struct ShellScriptScope {
			std::unordered_map<std::string, std::string> saved_vars;
			std::vector<std::string> saved_pos;
			std::string saved_name;
			std::unordered_map<std::string, const FunctionDef*> saved_funcs;
			bool s_errexit, s_nounset, s_xtrace, s_pipefail, s_noglob;
			std::filesystem::path saved_cwd;
		};
		ShellScriptScope snapshotShellScriptScope() const;
		void restoreShellScriptScope(ShellScriptScope& snap, bool force_set);

		// State snapshotted at the start of a `( ... )` subshell. Restored
		// before returning. Trap handlers are included because POSIX
		// requires a subshell's trap installs to be local to it.
		struct SubshellScope {
			std::unordered_map<std::string, std::string> saved_vars;
			bool errexit, nounset, xtrace, pipefail, noglob;
			std::unordered_map<std::string, std::string> traps;
			std::filesystem::path saved_cwd;
		};
		SubshellScope snapshotSubshellScope() const;
		void restoreSubshellScope(SubshellScope& snap);

		int  runExternal(const std::vector<std::string>& argv,
		                 const std::vector<std::pair<std::string, std::string>>& temp_env);

#ifdef _WIN32
		// Build the argv passed to CreateProcess: replaces argv[0] with the
		// resolved exec_path and POSIX-to-Win32-translates each arg unless
		// the callee is an MSYS binary or WBSH_NO_PATHCONV is set.
		std::vector<std::string>
		prepareExternalArgv(const std::vector<std::string>& argv,
		                    const std::string& exec_path);
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
		                          int* exit_status);
#endif

		// Heuristic: does the file at `path` look like a shell script we
		// should interpret in-process rather than handing to CreateProcess?
		bool looksLikeShellScript(const std::string& path) const;

		// Read `path` and execute its contents in-process with subshell-style
		// isolation (env / positional / $0 / function table / CWD all reset
		// on return). Returns the script's exit status.
		int runShellScript(const std::string& path,
		                   const std::vector<std::string>& argv,
		                   const std::vector<std::pair<std::string, std::string>>& temp_env);

#ifdef _WIN32
		// Multi-command branch of execPipeline (the n>=2 case). Sets up the
		// inter-stage pipes, launches each element, and waits for completion.
		// Returns the resolved pipeline status (pipefail-aware) BEFORE any
		// `! pipeline` negation by the caller.
		int execPipelineMultiCmd(const Pipeline& p);

		// `cmd &` — spawn a detached child sharing our std{in,out,err} fds,
		// register it as a job, announce `[id] pid` to stderr. Returns 0 on
		// success, 1 if the spawn failed.
		int launchBackgroundCommand(const Node& cmd);

		// Pipeline element launch. Returns a process HANDLE the parent waits
		// on, or INVALID_HANDLE_VALUE on failure.
		HANDLE launchPipelineElement(const Node& elem,
		                             HANDLE h_in, HANDLE h_out, HANDLE h_err);
		// Fast-path branch of launchPipelineElement: try to launch `elem` as
		// a direct external (skipping the intermediate `wbsh.exe -r -c ...`
		// shell). Returns the spawned HANDLE on success, INVALID_HANDLE_VALUE
		// on a hard failure (command-not-found etc.), or sets `*tried = false`
		// and returns INVALID_HANDLE_VALUE when the element is not eligible
		// for the fast path and we should fall through to the self-spawn.
		HANDLE tryDirectExternalLaunch(const Node& elem,
		                               HANDLE h_in, HANDLE h_out, HANDLE h_err,
		                               bool* tried);
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
		HANDLE launchExternalDirect(const SimpleCommand& sc,
		                            const std::vector<std::string>& argv,
		                            const std::string& exec_path,
		                            HANDLE h_in, HANDLE h_out, HANDLE h_err);
#endif
		bool isAbsoluteOrRelativePath(const std::string& name) const;
		bool patternMatches(const std::string& pat, const std::string& s);
		// Pop the top frame off `scope_stack_` and restore each entry's prior
		// value (or unset if the name didn't exist before). Called when a
		// function body finishes for any reason.
		void popLocalScope();

		struct ScopeEntry {
			std::string name;
			bool had_prev;
			std::string prev_value;
		};

		Environment& env_;
		Expander expander_;
		PathConv path_conv_;
		std::string source_text_;
		std::unordered_map<std::string, BuiltinFn> builtins_;
		std::unordered_map<std::string, const FunctionDef*> functions_;
		std::unordered_map<std::string, std::string> aliases_;
		std::vector<std::string> history_;
		std::vector<std::string> dir_stack_;
		std::unordered_map<std::string, std::string> trap_handlers_;
		std::unordered_map<std::string, CompletionSpec> completion_specs_;
		std::vector<Job> jobs_;
		int next_job_id_ = 0;
		int getopts_subindex_ = 1;
		// ASTs that the executor must keep alive (e.g. those parsed via
		// `source` / `eval` / inherited `WBSH_FUNCTIONS` evaluations) so the
		// raw FunctionDef* pointers in `functions_` stay valid.
		std::vector<NodePtr> owned_asts_;
		std::vector<std::vector<ScopeEntry>> scope_stack_;
		int  last_status_ = 0;
		int  loop_depth_ = 0;
		int  func_depth_ = 0;
		int  errexit_suppress_ = 0;
	};

	/// Register every shell builtin (defined in builtins.cpp).
	void registerCoreBuiltins(Executor& exec);
	/// Register every bundled coreutil (defined in coreutils.cpp).
	void registerCoreutils(Executor& exec);

}  // namespace wbsh

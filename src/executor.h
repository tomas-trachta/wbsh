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

#include <functional>
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
	/// Signature of a registered shell builtin / coreutil.
	using BuiltinFn = std::function<int(Executor&, const std::vector<std::string>&)>;

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

		int  runExternal(const std::vector<std::string>& argv,
		                 const std::vector<std::pair<std::string, std::string>>& temp_env);

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
		// Pipeline element launch. Returns a process HANDLE the parent waits
		// on, or INVALID_HANDLE_VALUE on failure.
		HANDLE launchPipelineElement(const Node& elem,
		                             HANDLE h_in, HANDLE h_out, HANDLE h_err);
		HANDLE launchExternalDirect(const SimpleCommand& sc,
		                            const std::vector<std::string>& argv,
		                            const std::string& exec_path,
		                            HANDLE h_in, HANDLE h_out, HANDLE h_err);
#endif
		bool isAbsoluteOrRelativePath(const std::string& name) const;
		bool patternMatches(const std::string& pat, const std::string& s);

		void execCompoundRedirsWrapper(const std::vector<Redirection>& redirs,
		                               const std::function<int()>& body, int& out_status);

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

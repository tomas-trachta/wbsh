/**
 * @file executor_traps.cpp
 * @brief Running traps: ERR after a failed command, DEBUG before one,
 *        RETURN when a function or `source` ends, and the signals other
 *        wbsh processes post (signalbox.h) at the shell's safe points.
 */

#include "executor.h"

#include <cstring>
#include <iterator>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "exitstatus.h"
#include "signalbox.h"
#include "signals.h"

namespace wbsh {

	static const char* const kFunctionScopedTraps[] = { "ERR", "DEBUG", "RETURN" };

	const Executor::ParsedText& Executor::parsedTrapAction(const std::string& action) {
		const auto cached = trap_asts_.find(action);
		if (cached != trap_asts_.end()) return cached->second;

		return trap_asts_.emplace(action, parseText(action, "trap")).first->second;
	}

	// Inside a trap action `$LINENO` stays the line of the command that
	// triggered it; functions the action calls still count their own lines.
	int Executor::runTrapAction(const std::string& action) {
		const ParsedText& parsed = parsedTrapAction(action);
		if (parsed.root == nullptr) return finishParsedText(parsed, 0);

		const std::string* saved_frozen = frozen_lineno_source_;
		const int saved_line = env_.currentLineno();
		frozen_lineno_source_ = parsed.root->source_text;
		++trap_depth_;
		enterTraceLevel();

		const int status = execute(*parsed.root);

		leaveTraceLevel();
		--trap_depth_;
		frozen_lineno_source_ = saved_frozen;
		env_.setCurrentLineno(saved_line);
		return finishParsedText(parsed, status);
	}

	static bool isPseudoSignal(const std::string& name) {
		if (name == "EXIT") return true;

		for (const char* scoped : kFunctionScopedTraps) {
			if (name == scoped) return true;
		}

		return false;
	}

	// A script starts with no traps, but the signals its parent ignored stay
	// ignored for good.
	void Executor::resetTrapsForScript() {
		std::unordered_map<std::string, std::string> ignored;
		std::set<std::string> names;
		for (const auto& trap : trap_handlers_) {
			if (!trap.second.empty() || isPseudoSignal(trap.first)) continue;

			ignored.insert(trap);
			names.insert(trap.first);
		}

		trap_handlers_ = std::move(ignored);
		startup_ignored_ = std::move(names);
		lists_inherited_traps_ = false;
		publishSignalDispositions();
	}

	static Environment::IndexedArray savedPipeStatus(const Environment& env) {
		const Environment::IndexedArray* array = env.getIndexedArray("PIPESTATUS");
		return array == nullptr ? Environment::IndexedArray() : *array;
	}

	namespace trap_signal {
		// bash numbers its pseudo-signals after the last real one (NSIG on Linux).
		static const int kDebug = 65;
		static const int kErr = 66;
		static const int kReturn = 67;
		static const char kVariable[] = "BASH_TRAPSIG";

		struct Saved {
			bool was_set;
			std::string value;
		};

		static Saved publish(Environment& env, int trap_number) {
			Saved saved{ env.has(kVariable), env.get(kVariable) };
			env.set(kVariable, std::to_string(trap_number));
			return saved;
		}

		static void restore(Environment& env, const Saved& saved) {
			if (saved.was_set) env.set(kVariable, saved.value);
			else env.unset(kVariable);
		}
	}

	int Executor::runTrapPublishingSignal(const std::string& action, int trap_number) {
		const trap_signal::Saved saved = trap_signal::publish(env_, trap_number);
		const int status = runTrapAction(action);
		trap_signal::restore(env_, saved);
		return status;
	}

	int Executor::runExitTrapText(const std::string& action) {
		const trap_signal::Saved saved = trap_signal::publish(env_, 0);
		enterTraceLevel();
		const int status = executeText(action, "<EXIT trap>");
		leaveTraceLevel();
		trap_signal::restore(env_, saved);
		return status;
	}

	// POSIX interp 1602: `return` or `exit` without a status, run by the
	// trap action itself rather than a function it calls, gives `$?` as it
	// was before the trap; the DEBUG trap is the exception.
	Executor::TrapReturnContext Executor::enterTrapReturnContext(int trap_number, int status) {
		const TrapReturnContext saved = trap_return_context_;
		if (trap_number != trap_signal::kDebug) {
			trap_return_context_ = { func_depth_ + source_depth_, status };
		}

		return saved;
	}

	// The command the trap interrupts sees `$?`, PIPESTATUS and its errexit
	// judgement as they were before the trap ran.
	int Executor::runTrapKeepingStatus(const std::string& action, int trap_number) {
		const int status = last_status_;
		const bool judged = status_judged_;
		Environment::IndexedArray pipe_status = savedPipeStatus(env_);

		const TrapReturnContext saved_context = enterTrapReturnContext(trap_number, status);
		const int trap_status = runTrapPublishingSignal(action, trap_number);
		trap_return_context_ = saved_context;

		status_judged_ = judged;
		if (flowPending()) return trap_status;

		setLastStatus(status);
		env_.setIndexedArraySparse("PIPESTATUS", std::move(pipe_status));
		return trap_status;
	}

	static const int kSigInt = 2;
	static const int kSigQuit = 3;
	static const int kSigTerm = 15;
	static const int kIgnoredByDefault[] = { 17, 18, 23, 28 };

	static bool ignoredByDefault(int signum, bool interactive) {
		for (const int ignored : kIgnoredByDefault) {
			if (signum == ignored) return true;
		}

		return interactive && (signum == kSigInt || signum == kSigQuit || signum == kSigTerm);
	}

	bool Executor::signalIgnored(int signum) const {
		const char* name = signalName(signum);
		if (name == nullptr) return true;
		if (hasTrap(name)) return trapAction(name).empty();
		return ignoredByDefault(signum, env_.interactive());
	}

	void Executor::publishSignalDispositions() const {
		unsigned long caught = 0;
		for (int signum = 1; signum <= signalCount(); ++signum) {
			const bool trapped = hasTrap(signalName(signum));
			if (trapped || ignoredByDefault(signum, env_.interactive())) caught |= 1UL << signum;
		}

		setCaughtShellSignals(caught);
	}

	int Executor::interruptingSignal() {
		const unsigned long pending = pendingShellSignals();
		for (int signum = 1; signum <= signalCount(); ++signum) {
			if ((pending & (1UL << signum)) == 0) continue;
			if (!signalIgnored(signum)) return signum;

			takeShellSignal(signum);
		}

		return 0;
	}

	// As in bash, a signal with no trap that is not ignored ends the shell
	// with 128 + its number, and the EXIT trap still runs.
	void Executor::actOnSignal(int signum) {
		const char* name = signalName(signum);
		if (name == nullptr) return;

		if (hasTrap(name)) {
			const std::string action = trapAction(name);
			if (!action.empty()) runTrapKeepingStatus(action, signum);
			return;
		}

		if (!ignoredByDefault(signum, env_.interactive())) raiseExit(kSignalStatusBase + signum);
	}

	void Executor::noteChildExited() {
		std::string action;
		if (!env_.options().monitor || !trapActionFor("CHLD", action)) return;

		++exited_children_;
	}

	void Executor::runChildExitTraps() {
		if (running_child_traps_) return;

		running_child_traps_ = true;
		std::string action;
		int signum = 0;
		parseSignalSpec("CHLD", signum);
		while (exited_children_ > 0 && !flowPending() && trapActionFor("CHLD", action)) {
			--exited_children_;
			runTrapKeepingStatus(action, signum);
		}

		exited_children_ = 0;
		running_child_traps_ = false;
	}

	// A signal posted while its own trap runs waits for the next safe point.
	void Executor::handlePendingSignals() {
		runChildExitTraps();
		if (handling_signals_ || pendingShellSignals() == 0) return;

		handling_signals_ = true;
		for (int signum = 1; signum <= signalCount() && !flowPending(); ++signum) {
			if (takeShellSignal(signum)) actOnSignal(signum);
		}

		handling_signals_ = false;
	}

	bool Executor::trapActionFor(const char* name, std::string& action) const {
		const auto it = trap_handlers_.find(name);
		if (it == trap_handlers_.end() || it->second.empty()) return false;

		action = it->second;
		return true;
	}

	void Executor::fireErrTrap(int status) {
		std::string action;
		if (in_err_trap_ || !trapActionFor("ERR", action)) return;

		in_err_trap_ = true;
		setLastStatus(status);
		runTrapKeepingStatus(action, trap_signal::kErr);
		in_err_trap_ = false;
	}

	// Under extdebug a failing DEBUG trap skips the command, and status 2
	// inside a function or sourced file returns from it.
	bool Executor::fireDebugTrap() {
		std::string action;
		if (running_debug_trap_ || flowPending() || !trapActionFor("DEBUG", action)) return true;

		running_debug_trap_ = true;
		const int status = runTrapKeepingStatus(action, trap_signal::kDebug);
		running_debug_trap_ = false;
		if (!env_.shopts().extdebug || status == 0 || flowPending()) return true;

		if (status == 2 && (funcDepth() > 0 || sourceDepth() > 0)) raiseReturn(status);
		setLastStatus(0);
		return false;
	}

	void Executor::fireReturnTrap() {
		std::string action;
		if (running_return_trap_ || flowPending() || !trapActionFor("RETURN", action)) return;

		running_return_trap_ = true;
		runTrapKeepingStatus(action, trap_signal::kReturn);
		running_return_trap_ = false;
	}

	// BASH_COMMAND keeps naming the interrupted command while a trap runs;
	// the DEBUG trap still runs before the trap action's commands.
	bool Executor::announceCommand(std::string text) {
		if (trap_depth_ == 0) env_.setCurrentCommand(std::move(text));
		return fireDebugTrap();
	}

	static bool isErrTrap(const char* name) {
		return std::strcmp(name, "ERR") == 0;
	}

	bool Executor::inheritsTrap(const std::string& name) const {
		if (name == "ERR") return env_.options().errtrace;
		if (name == "DEBUG" || name == "RETURN") return env_.options().functrace;
		return false;
	}

	// `declare -ft` makes one function inherit DEBUG and RETURN as functrace
	// does; a function the DEBUG trap calls never runs the RETURN trap.
	Executor::SuspendedTrap Executor::suspendTrap(const char* name, bool traced) {
		SuspendedTrap trap{ name, {}, false };
		const auto it = trap_handlers_.find(name);
		if (it == trap_handlers_.end()) return trap;

		const bool traced_trap = traced && !isErrTrap(name);
		const bool inherited = inheritsTrap(name) || traced_trap;
		const bool return_in_debug = running_debug_trap_ && std::strcmp(name, "RETURN") == 0;
		trap.kept = it->second.empty() || (inherited && !return_in_debug);
		if (trap.kept) return trap;

		trap.action = std::move(it->second);
		trap_handlers_.erase(it);
		return trap;
	}

	std::vector<Executor::SuspendedTrap> Executor::suspendTrapsForFunction(bool traced) {
		std::vector<SuspendedTrap> suspended;
		suspended.reserve(std::size(kFunctionScopedTraps));
		for (const char* name : kFunctionScopedTraps) suspended.push_back(suspendTrap(name, traced));
		return suspended;
	}

	// A trap the function set itself stays after it returns. When no ERR
	// trap was set at the call, bash does not run the one the function
	// installed for the failed call of that function.
	void Executor::restoreTrap(SuspendedTrap& trap) {
		const bool set_inside = hasTrap(trap.name);
		const bool first_err_trap = set_inside && trap.action.empty() && !trap.kept;
		if (isErrTrap(trap.name) && first_err_trap) call_set_err_trap_ = true;
		if (trap.action.empty() || set_inside) return;

		trap_handlers_[trap.name] = std::move(trap.action);
	}

	// The replay runs as if inside a trap, so a DEBUG trap it installs does
	// not fire for the `trap` commands after it.
	void Executor::adoptInheritedTraps(const std::string& text) {
		++trap_depth_;
		running_debug_trap_ = true;
		executeText(text, "<inherited traps>");
		running_debug_trap_ = false;
		--trap_depth_;
		resetSubshellTraps();
	}

	void Executor::restoreTrapsAfterFunction(std::vector<SuspendedTrap>& suspended) {
		for (SuspendedTrap& trap : suspended) restoreTrap(trap);
	}

}  // namespace wbsh

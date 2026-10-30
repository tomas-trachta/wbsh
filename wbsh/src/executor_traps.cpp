/**
 * @file executor_traps.cpp
 * @brief Running traps: ERR after a failed command, DEBUG before one,
 *        RETURN when a function or `source` ends, and the signals other
 *        wbsh processes post (signalbox.h) at the shell's safe points.
 */

#include "executor.h"

#include <cstring>
#include <string>
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

		const int status = execute(*parsed.root);

		--trap_depth_;
		frozen_lineno_source_ = saved_frozen;
		env_.setCurrentLineno(saved_line);
		return finishParsedText(parsed, status);
	}

	static Environment::IndexedArray savedPipeStatus(const Environment& env) {
		const Environment::IndexedArray* array = env.getIndexedArray("PIPESTATUS");
		return array == nullptr ? Environment::IndexedArray() : *array;
	}

	// The command the trap interrupts sees `$?`, PIPESTATUS and its errexit
	// judgement as they were before the trap ran.
	void Executor::runTrapKeepingStatus(const std::string& action) {
		const int status = last_status_;
		const bool judged = status_judged_;
		Environment::IndexedArray pipe_status = savedPipeStatus(env_);

		runTrapAction(action);

		status_judged_ = judged;
		if (flowPending()) return;

		setLastStatus(status);
		env_.setIndexedArraySparse("PIPESTATUS", std::move(pipe_status));
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
			if (!action.empty()) runTrapKeepingStatus(action);
			return;
		}

		if (!ignoredByDefault(signum, env_.interactive())) raiseExit(kSignalStatusBase + signum);
	}

	// A signal posted while its own trap runs waits for the next safe point.
	void Executor::handlePendingSignals() {
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
		runTrapKeepingStatus(action);
		in_err_trap_ = false;
	}

	void Executor::fireDebugTrap() {
		std::string action;
		if (trap_depth_ > 0 || flowPending() || !trapActionFor("DEBUG", action)) return;

		runTrapKeepingStatus(action);
	}

	void Executor::fireReturnTrap() {
		std::string action;
		if (trap_depth_ > 0 || flowPending() || !trapActionFor("RETURN", action)) return;

		runTrapKeepingStatus(action);
	}

	// BASH_COMMAND keeps naming the interrupted command while a trap runs.
	void Executor::announceCommand(std::string text) {
		if (trap_depth_ > 0) return;

		env_.setCurrentCommand(std::move(text));
		fireDebugTrap();
	}

	static bool isErrTrap(const char* name) {
		return std::strcmp(name, "ERR") == 0;
	}

	bool Executor::inheritsTrap(const std::string& name) const {
		if (name == "ERR") return env_.options().errtrace;
		if (name == "DEBUG" || name == "RETURN") return env_.options().functrace;
		return false;
	}

	Executor::SuspendedTrap Executor::suspendTrap(const char* name) {
		SuspendedTrap trap{ name, {}, false };
		const auto it = trap_handlers_.find(name);
		if (it == trap_handlers_.end()) return trap;

		trap.kept = it->second.empty() || inheritsTrap(name);
		if (trap.kept) return trap;

		trap.action = std::move(it->second);
		trap_handlers_.erase(it);
		return trap;
	}

	std::vector<Executor::SuspendedTrap> Executor::suspendTrapsForFunction() {
		std::vector<SuspendedTrap> suspended;
		for (const char* name : kFunctionScopedTraps) suspended.push_back(suspendTrap(name));
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
		executeText(text, "<inherited traps>");
		--trap_depth_;
		resetSubshellTraps();
	}

	void Executor::restoreTrapsAfterFunction(std::vector<SuspendedTrap>& suspended) {
		for (SuspendedTrap& trap : suspended) restoreTrap(trap);
	}

}  // namespace wbsh

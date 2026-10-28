/**
 * @file executor_traps.cpp
 * @brief Running the traps that are not signals: ERR after a failed
 *        command, DEBUG before one, RETURN when a function or `source` ends.
 */

#include "executor.h"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

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

	void Executor::restoreTrapsAfterFunction(std::vector<SuspendedTrap>& suspended) {
		for (SuspendedTrap& trap : suspended) restoreTrap(trap);
	}

}  // namespace wbsh

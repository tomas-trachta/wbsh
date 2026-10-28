/**
 * @file executor_traps.cpp
 * @brief Running the traps that are not signals: ERR after a failed command.
 */

#include "executor.h"

#include <string>
#include <utility>

namespace wbsh {

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

		const int status = execute(*parsed.root);

		frozen_lineno_source_ = saved_frozen;
		env_.setCurrentLineno(saved_line);
		return finishParsedText(parsed, status);
	}

	void Executor::fireErrTrap(int status) {
		if (in_err_trap_) return;

		const auto it = trap_handlers_.find("ERR");
		if (it == trap_handlers_.end() || it->second.empty()) return;

		const std::string action = it->second;
		const bool judged = status_judged_;
		in_err_trap_ = true;

		runTrapAction(action);

		in_err_trap_ = false;
		status_judged_ = judged;
		if (!flowPending()) setLastStatus(status);
	}

	std::string Executor::suspendErrTrapForFunction() {
		if (env_.options().errtrace) return {};

		const auto it = trap_handlers_.find("ERR");
		if (it == trap_handlers_.end() || it->second.empty()) return {};

		std::string saved = std::move(it->second);
		trap_handlers_.erase(it);
		return saved;
	}

	// When no ERR trap was set at the call, bash does not run the one the
	// function installed for the failed call of that function.
	void Executor::restoreErrTrapAfterFunction(std::string saved, bool kept) {
		const auto own = trap_handlers_.find("ERR");
		const bool set_first = own != trap_handlers_.end() && saved.empty() && !kept;
		if (set_first) call_set_err_trap_ = true;
		if (saved.empty() || own != trap_handlers_.end()) return;

		trap_handlers_["ERR"] = std::move(saved);
	}

}  // namespace wbsh

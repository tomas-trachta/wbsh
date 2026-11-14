/**
 * @file executor_restricted.cpp
 * @brief What a restricted shell (`set -r`) lets into its command hash
 *        table, through `hash -p` or BASH_CMDS.
 */

#include "executor.h"

#include <algorithm>
#include <string>

#include "diagnostics.h"

namespace wbsh {

	bool Executor::restrictedHashTargetAllowed(const std::string& target, const char* label) {
		if (!env_.restricted()) return true;

		if (target.find('/') != std::string::npos) {
			shellErrorf("%s%s: restricted\n", label, target.c_str());
			return false;
		}

		if (!findExecutable(target).empty()) return true;

		shellErrorf("%s%s: not found\n", label, target.c_str());
		return false;
	}

	void Executor::dropRestrictedHashAssignments(SimpleCmdAssigns& expanded) {
		if (!env_.restricted()) return;

		const auto refused = [this](const ElemAssign& elem) {
			return elem.name == "BASH_CMDS" && !restrictedHashTargetAllowed(elem.value, "");
		};
		expanded.elem.erase(std::remove_if(expanded.elem.begin(), expanded.elem.end(), refused),
			expanded.elem.end());
	}

}  // namespace wbsh

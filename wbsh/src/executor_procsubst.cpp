/**
 * @file executor_procsubst.cpp
 * @brief Process-substitution paths that outlive their command or are
 *        read again by `eval`.
 */

#include "executor.h"

#include <string>

namespace wbsh {

#ifdef _WIN32
	static std::string withDoubledBackslashes(const std::string& path) {
		std::string escaped;
		escaped.reserve(path.size() * 2);
		for (const char c : path) {
			escaped.push_back(c);
			if (c == '\\') escaped.push_back('\\');
		}

		return escaped;
	}

	static void escapeEachOccurrence(std::string& text, const std::string& path) {
		if (path.empty()) return;

		const std::string escaped = withDoubledBackslashes(path);
		std::size_t pos = text.find(path);
		while (pos != std::string::npos) {
			text.replace(pos, path.size(), escaped);
			pos = text.find(path, pos + escaped.size());
		}
	}

	std::string Executor::protectProcSubstPaths(std::string text) const {
		for (const ProcSubstRelay& relay : proc_substs_) {
			escapeEachOccurrence(text, relay.path);
		}

		for (const ProcSubstRelay& relay : kept_proc_substs_) {
			escapeEachOccurrence(text, relay.path);
		}

		return text;
	}

	void Executor::keepProcessSubstitutionsSince(std::size_t watermark) {
		if (watermark >= proc_substs_.size()) return;

		const auto first = proc_substs_.begin() + static_cast<long>(watermark);
		kept_proc_substs_.insert(kept_proc_substs_.end(), first, proc_substs_.end());
		proc_substs_.erase(first, proc_substs_.end());
	}
#else
	std::string Executor::protectProcSubstPaths(std::string text) const {
		return text;
	}
#endif /* _WIN32 */

}  // namespace wbsh

/**
 * @file executor_callstack.cpp
 * @brief The shell's call stack: functions, `source`d files and the script
 *        itself, as FUNCNAME, BASH_SOURCE and BASH_LINENO show it.
 */

#include "executor.h"

#include <string>
#include <utility>
#include <vector>

#include "numparse.h"

namespace wbsh {

	static const char* const kFuncNameVar   = "FUNCNAME";
	static const char* const kBashSourceVar = "BASH_SOURCE";
	static const char* const kBashLinenoVar = "BASH_LINENO";

	std::string Executor::currentSourceFile() const {
		return call_stack_.empty() ? std::string() : call_stack_.back().source;
	}

	std::string Executor::functionFile(const std::string& name) const {
		const auto found = function_files_.find(name);
		return found == function_files_.end() ? std::string() : found->second;
	}

	std::string Executor::serializeFunctionFiles() const {
		std::string text;
		for (const auto& entry : function_files_) {
			if (entry.second.empty() || functions_.count(entry.first) == 0) continue;

			text += entry.first + '\n' + entry.second + '\n';
		}

		return text;
	}

	void Executor::adoptInheritedFunctionFiles(const std::string& text) {
		std::size_t start = 0;
		while (start < text.size()) {
			const std::size_t name_end = text.find('\n', start);
			if (name_end == std::string::npos) return;

			std::size_t file_end = text.find('\n', name_end + 1);
			if (file_end == std::string::npos) file_end = text.size();

			const std::string name = text.substr(start, name_end - start);
			if (functions_.count(name) != 0) {
				function_files_[name] = text.substr(name_end + 1, file_end - name_end - 1);
			}

			start = file_end + 1;
		}
	}

	void Executor::pushCallFrame(CallFrame frame) {
		call_stack_.push_back(std::move(frame));
		publishCallStack();
	}

	void Executor::popCallFrame() {
		if (!call_stack_.empty()) call_stack_.pop_back();
		publishCallStack();
	}

	void Executor::publishCallStack() {
		if (call_stack_.empty()) {
			env_.unset(kFuncNameVar);
			env_.unset(kBashSourceVar);
			env_.unset(kBashLinenoVar);
			return;
		}

		std::vector<std::string> names;
		std::vector<std::string> sources;
		std::vector<std::string> lines;
		for (auto frame = call_stack_.rbegin(); frame != call_stack_.rend(); ++frame) {
			names.push_back(frame->name);
			sources.push_back(frame->source);
			lines.push_back(std::to_string(frame->line));
		}

		env_.setIndexedArrayFromList(kFuncNameVar, std::move(names));
		env_.setIndexedArrayFromList(kBashSourceVar, std::move(sources));
		env_.setIndexedArrayFromList(kBashLinenoVar, std::move(lines));
	}

	void Executor::beginScriptFrame(const std::string& path) {
		pushCallFrame({ "main", path, 0 });
	}

	void Executor::beginSource(const std::string& file) {
		++source_depth_;
		pushCallFrame({ "source", file, env_.currentLineno() });
	}

	void Executor::endSource() {
		--source_depth_;
		popCallFrame();
	}

	static std::vector<std::string> arrayValues(const Environment& env, const char* name) {
		std::vector<std::string> values;
		const Environment::IndexedArray* array = env.getIndexedArray(name);
		if (array == nullptr) return values;

		for (const auto& element : *array) values.push_back(element.second);
		return values;
	}

	void Executor::adoptInheritedCallStack() {
		const std::vector<std::string> names   = arrayValues(env_, kFuncNameVar);
		const std::vector<std::string> sources = arrayValues(env_, kBashSourceVar);
		const std::vector<std::string> lines   = arrayValues(env_, kBashLinenoVar);
		if (names.empty() || names.size() != sources.size() || names.size() != lines.size()) return;

		call_stack_.clear();
		for (std::size_t i = names.size(); i-- > 0; ) {
			int line = 0;
			parseInt(lines[i], line);
			call_stack_.push_back({ names[i], sources[i], line });
		}
	}

}  // namespace wbsh

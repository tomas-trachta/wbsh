/**
 * @file executor_callstack.cpp
 * @brief The shell's call stack: functions, `source`d files and the script
 *        itself, as FUNCNAME, BASH_SOURCE and BASH_LINENO show it.
 */

#include "executor.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <charconv>
#include <initializer_list>
#include <string>
#include <string_view>
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

	// Every function call pushes and pops a frame, so while the three
	// arrays still read as the stack they were published from, a call
	// edits them in place instead of building them again.
	namespace callstack_detail {

		enum class Column {
			Name,
			Source,
			Line,
		};

		static const Column kColumns[] = { Column::Name, Column::Source, Column::Line };
		static const std::size_t kColumnCount = 3;

		using Digits = std::array<char, 24>;
		using PublishedArrays = std::array<Environment::IndexedArray*, kColumnCount>;

		static const std::string& columnVar(Column column) {
			static const std::string kNames[kColumnCount] = {
				kFuncNameVar, kBashSourceVar, kBashLinenoVar,
			};
			return kNames[static_cast<std::size_t>(column)];
		}

		static std::string_view lineText(int line, Digits& digits) {
			const std::to_chars_result end = std::to_chars(digits.data(),
				digits.data() + digits.size(), line);
			const auto length = static_cast<std::size_t>(end.ptr - digits.data());
			return std::string_view(digits.data(), length);
		}

		template <typename Frame>
		static std::string_view columnText(const Frame& frame, Column column, Digits& digits) {
			switch (column) {
			case Column::Name:
				return frame.name;
			case Column::Source:
				return frame.source;
			case Column::Line:
			default:
				return lineText(frame.line, digits);
			}
		}

		template <typename Frame>
		static bool publishedAs(const Environment::IndexedArray& array,
		                        const std::vector<Frame>& frames, Column column) {
			if (array.size() != frames.size()) return false;

			Digits digits{};
			long long index = 0;
			auto element = array.begin();
			for (auto frame = frames.rbegin(); frame != frames.rend(); ++frame, ++element) {
				if (element->first != index++) return false;
				if (element->second != columnText(*frame, column, digits)) return false;
			}

			return true;
		}

		template <typename Frame>
		static bool findPublished(Environment& env, const std::vector<Frame>& frames,
		                          PublishedArrays& out_arrays) {
			for (const Column column : kColumns) {
				Environment::IndexedArray* array = env.plainIndexedArray(columnVar(column));
				if (array == nullptr || !publishedAs(*array, frames, column)) return false;

				out_arrays[static_cast<std::size_t>(column)] = array;
			}

			return true;
		}

		static void moveKey(Environment::IndexedArray& array, long long from, long long to) {
			auto node = array.extract(from);
			node.key() = to;
			array.insert(std::move(node));
		}

		template <typename Frame>
		static void publishPushed(const PublishedArrays& arrays, const Frame& frame) {
			Digits digits{};
			for (const Column column : kColumns) {
				Environment::IndexedArray& array = *arrays[static_cast<std::size_t>(column)];
				for (auto index = static_cast<long long>(array.size()); index-- > 0; ) {
					moveKey(array, index, index + 1);
				}

				array.emplace(0, std::string(columnText(frame, column, digits)));
			}
		}

		static void publishPopped(const PublishedArrays& arrays) {
			for (Environment::IndexedArray* array : arrays) {
				array->erase(0);
				const auto count = static_cast<long long>(array->size());
				for (long long index = 1; index <= count; ++index) {
					moveKey(*array, index, index - 1);
				}
			}
		}

	}  // namespace callstack_detail

	bool Executor::inFunctionFrame() const {
		for (const CallFrame& frame : call_stack_) {
			if (frame.function) return true;
		}

		return false;
	}

	void Executor::pushCallFrame(CallFrame frame) {
		callstack_detail::PublishedArrays arrays{};
		const bool in_place = callstack_detail::findPublished(env_, call_stack_, arrays);
		call_stack_.push_back(std::move(frame));
		if (in_place) callstack_detail::publishPushed(arrays, call_stack_.back());
		else publishCallStack();
	}

	void Executor::popCallFrame() {
		callstack_detail::PublishedArrays arrays{};
		const bool in_place = call_stack_.size() > 1
			&& callstack_detail::findPublished(env_, call_stack_, arrays);
		if (!call_stack_.empty()) call_stack_.pop_back();
		if (in_place && inFunctionFrame()) callstack_detail::publishPopped(arrays);
		else publishCallStack();
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

		if (!inFunctionFrame()) names.clear();
		const bool outside_functions = names.empty();
		env_.setIndexedArrayFromList(kFuncNameVar, std::move(names));
		env_.setIndexedArrayFromList(kBashSourceVar, std::move(sources));
		env_.setIndexedArrayFromList(kBashLinenoVar, std::move(lines));

		// Outside functions bash's FUNCNAME is declared but holds nothing.
		if (outside_functions) env_.markValueless(kFuncNameVar);
	}

	// bash starts a script with BASH_ARGV and BASH_ARGC set, and empty.
	void Executor::beginScriptFrame(const std::string& path) {
		pushCallFrame({ "main", path, 0 });
		for (const char* name : { "BASH_ARGV", "BASH_ARGC" }) {
			if (!env_.isIndexedArray(name)) env_.setIndexedArrayFromList(name, {});
		}
	}

	// Without functrace a sourced file does not see the DEBUG trap.
	void Executor::beginSource(const std::string& file) {
		++source_depth_;
		source_debug_traps_.push_back(suspendTrap("DEBUG", false));
		enterTraceLevel();
		pushCallFrame({ "source", file, env_.currentLineno() });
	}

	// The RETURN trap runs once the file's frame is gone, at the line of
	// the `source` command.
	void Executor::endSource() {
		const int line = call_stack_.empty() ? env_.currentLineno() : call_stack_.back().line;
		--source_depth_;
		leaveTraceLevel();
		popCallFrame();
		env_.setCurrentLineno(line);
		fireReturnTrap();
		restoreTrap(source_debug_traps_.back());
		source_debug_traps_.pop_back();
	}

	static std::vector<std::string> arrayValues(const Environment& env, const char* name) {
		std::vector<std::string> values;
		const Environment::IndexedArray* array = env.getIndexedArray(name);
		if (array == nullptr) return values;

		for (const auto& element : *array) values.push_back(element.second);
		return values;
	}

	// Under extdebug each call puts its arguments, last first, on top of
	// BASH_ARGV and their count on top of BASH_ARGC.
	bool Executor::pushBashArgv(const std::vector<std::string>& args) {
		if (!env_.shopts().extdebug) return false;

		std::vector<std::string> argv(args.rbegin(), args.rend());
		const std::vector<std::string> outer = arrayValues(env_, "BASH_ARGV");
		argv.insert(argv.end(), outer.begin(), outer.end());
		env_.setIndexedArrayFromList("BASH_ARGV", std::move(argv));

		std::vector<std::string> argc{ std::to_string(args.size()) };
		const std::vector<std::string> outer_counts = arrayValues(env_, "BASH_ARGC");
		argc.insert(argc.end(), outer_counts.begin(), outer_counts.end());
		env_.setIndexedArrayFromList("BASH_ARGC", std::move(argc));
		return true;
	}

	void Executor::popBashArgv(bool pushed, std::size_t count) {
		if (!pushed) return;

		std::vector<std::string> argv = arrayValues(env_, "BASH_ARGV");
		const auto popped = static_cast<std::ptrdiff_t>((std::min)(count, argv.size()));
		argv.erase(argv.begin(), argv.begin() + popped);
		env_.setIndexedArrayFromList("BASH_ARGV", std::move(argv));

		std::vector<std::string> argc = arrayValues(env_, "BASH_ARGC");
		if (!argc.empty()) argc.erase(argc.begin());
		env_.setIndexedArrayFromList("BASH_ARGC", std::move(argc));
	}

	// FUNCNAME is empty outside functions; the frames are then the script
	// (line 0, at the bottom) and the files `source`d on top of it.
	static std::vector<std::string> framesWithoutFunctions(const std::vector<std::string>& lines) {
		std::vector<std::string> names(lines.size(), "source");
		if (!lines.empty() && lines.back() == "0") names.back() = "main";
		return names;
	}

	void Executor::adoptInheritedCallStack() {
		std::vector<std::string> names = arrayValues(env_, kFuncNameVar);
		const std::vector<std::string> sources = arrayValues(env_, kBashSourceVar);
		const std::vector<std::string> lines   = arrayValues(env_, kBashLinenoVar);
		const bool functions = !names.empty();
		if (!functions) names = framesWithoutFunctions(lines);
		if (names.empty() || names.size() != sources.size() || names.size() != lines.size()) return;

		call_stack_.clear();
		for (std::size_t i = names.size(); i-- > 0; ) {
			int line = 0;
			parseInt(lines[i], line);
			call_stack_.push_back({ names[i], sources[i], line, functions });
		}
	}

}  // namespace wbsh

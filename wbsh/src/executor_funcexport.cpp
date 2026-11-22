/**
 * @file executor_funcexport.cpp
 * @brief Functions passed through the environment as bash does it:
 *        `BASH_FUNC_name%%=() { body }`, written for `export -f` and
 *        read back at startup.
 */

#include "executor.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "ast.h"
#include "diagnostics.h"
#include "funcprint.h"

namespace wbsh {

	namespace funcexport_detail {

		static const char kPrefix[] = "BASH_FUNC_";
		static const char kSuffix[] = "%%";
		static const char kValueStart[] = "() {";
		static const std::size_t kPrefixLength = sizeof(kPrefix) - 1;
		static const std::size_t kSuffixLength = sizeof(kSuffix) - 1;

		static bool hasAffixes(const std::string& name) {
			return name.size() > kPrefixLength + kSuffixLength
				&& name.compare(0, kPrefixLength, kPrefix) == 0
				&& name.compare(name.size() - kSuffixLength, kSuffixLength, kSuffix) == 0;
		}

		static std::string functionName(const std::string& variable) {
			return variable.substr(kPrefixLength, variable.size() - kPrefixLength - kSuffixLength);
		}

		static std::string variableName(const std::string& function) {
			return kPrefix + function + kSuffix;
		}

		// The definition without its name: `() { ... }`.
		static std::string exportedValue(const FunctionDef& def) {
			const std::string text = functionDefinitionText(def);
			const std::size_t body = text.find('\n');
			return "() " + (body == std::string::npos ? text : text.substr(body + 1));
		}

		static const Node* soleCommand(const Node* node) {
			if (node == nullptr || node->kind != Node::Kind::List) return node;

			const List& list = static_cast<const List&>(*node);
			if (list.items.size() != 1 || list.items[0].background) return nullptr;
			return list.items[0].command;
		}

		static const Node* unwrapPipeline(const Node* node) {
			if (node == nullptr || node->kind != Node::Kind::Pipeline) return node;

			const Pipeline& pipeline = static_cast<const Pipeline&>(*node);
			if (pipeline.commands.size() != 1 || pipeline.bang || pipeline.timed) return nullptr;
			return pipeline.commands[0];
		}

		// Anything but one definition of exactly @p name is an attack or a
		// mangled name (CVE-2014-6271 and later): nothing of it runs.
		static const FunctionDef* soleDefinitionOf(const Node* root, const std::string& name) {
			const Node* node = unwrapPipeline(soleCommand(root));
			if (node == nullptr || node->kind != Node::Kind::FunctionDef) return nullptr;

			const FunctionDef* def = static_cast<const FunctionDef*>(node);
			return def->name == name ? def : nullptr;
		}

		static bool isIdentifier(const std::string& name) {
			if (name.empty() || std::isdigit(static_cast<unsigned char>(name[0])) != 0) return false;

			return std::all_of(name.begin(), name.end(), [](char c) {
				return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
			});
		}

		static bool importableName(const std::string& name, bool posix) {
			if (name.find('/') != std::string::npos) return false;
			return !posix || isIdentifier(name);
		}

	}  // namespace funcexport_detail

	void Executor::appendExportedFunctions(std::vector<std::pair<std::string, std::string>>& out) const {
		for (const std::string& name : exported_functions_) {
			const FunctionDef* def = findFunction(name);
			if (def == nullptr) continue;

			out.emplace_back(funcexport_detail::variableName(name),
				funcexport_detail::exportedValue(*def));
		}
	}

	bool Executor::importEnvironmentFunction(const std::string& name, const std::string& value) {
		if (!funcexport_detail::importableName(name, env_.options().posix)) return false;

		ParsedText parsed = parseText(name + " " + value, name);
		if (!parsed.diagnostics.empty()) {
			std::fputs(parsed.diagnostics.c_str(), stderr);
			return false;
		}

		const FunctionDef* def = funcexport_detail::soleDefinitionOf(parsed.root, name);
		if (def == nullptr) {
			shellErrorf("warning: %s: ignoring function definition attempt\n", name.c_str());
			return false;
		}

		execFunctionDef(*def);
		owned_arenas_.push_back(std::move(parsed.arena));
		exported_functions_.insert(name);
		return true;
	}

	void Executor::importEnvironmentFunctions() {
		std::vector<std::pair<std::string, std::string>> candidates;
		for (const auto& var : env_.vars()) {
			if (!funcexport_detail::hasAffixes(var.first)) continue;
			if (var.second.compare(0, 4, funcexport_detail::kValueStart) != 0) continue;

			candidates.emplace_back(var.first, var.second);
		}

		for (const auto& candidate : candidates) {
			const std::string name = funcexport_detail::functionName(candidate.first);
			if (!importEnvironmentFunction(name, candidate.second)) {
				setLastStatus(1);
				shellErrorf("error importing function definition for `%s'\n", name.c_str());
				continue;
			}

			env_.unset(candidate.first);
		}
	}

}  // namespace wbsh

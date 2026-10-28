/**
 * @file make_vars.cpp
 * @brief The variable database: precedence by origin, lookup through the
 *        target-specific levels down to the globals, and the environment
 *        a recipe's shell gets.
 */

#include "make_internal.h"

#include <cctype>
#include <set>
#include <string>
#include <vector>

namespace wbsh::make_detail {

	bool defineVariable(VarTable& table, const std::string& name, const Variable& variable) {
		const auto found = table.find(name);
		if (found == table.end()) {
			table.emplace(name, variable);
			return true;
		}

		Variable& existing = found->second;
		if (existing.origin > variable.origin) return false;

		const ExportMode kept = existing.export_mode;
		existing = variable;
		if (variable.export_mode == ExportMode::Default) existing.export_mode = kept;
		return true;
	}

	void setGlobal(Make& make, const std::string& name, const std::string& value, Origin origin) {
		Variable variable;
		variable.value = value;
		variable.flavor = Flavor::Simple;
		variable.origin = origin;
		defineVariable(make.globals, name, variable);
	}

	Variable* findGlobal(Make& make, const std::string& name) {
		const auto found = make.globals.find(name);
		return found == make.globals.end() ? nullptr : &found->second;
	}

	static VarRef searchFrom(const Make& make, const std::string& name, const VarScope* level,
	                         const ExpandContext& ctx) {
		for (; level != nullptr; level = level->parent) {
			const auto found = level->table->find(name);
			if (found == level->table->end()) continue;
			if (found->second.is_private && level->owner != ctx.target) continue;

			return VarRef{ &found->second, level->parent, false };
		}

		const auto global = make.globals.find(name);
		if (global == make.globals.end()) return VarRef{};

		return VarRef{ &global->second, nullptr, true };
	}

	VarRef lookupVariable(const Make& make, const std::string& name, const ExpandContext& ctx) {
		return searchFrom(make, name, ctx.scope, ctx);
	}

	VarRef lookupBelow(const Make& make, const std::string& name, const VarRef& found,
	                   const ExpandContext& ctx) {
		if (found.global || found.variable == nullptr) return VarRef{};

		return searchFrom(make, name, found.rest, ctx);
	}

	std::string originName(Origin origin) {
		switch (origin) {
			case Origin::Default:
				return "default";
			case Origin::Environment:
				return "environment";
			case Origin::File:
				return "file";
			case Origin::EnvironmentOverride:
				return "environment override";
			case Origin::CommandLine:
				return "command line";
			case Origin::Override:
				return "override";
			case Origin::Automatic:
				return "automatic";
			default:
				return "undefined";
		}
	}

	static bool isExportableName(const std::string& name) {
		if (name.empty() || std::isdigit(static_cast<unsigned char>(name[0])) != 0) return false;

		for (const char c : name) {
			if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '_') return false;
		}

		return true;
	}

	static bool isExported(const Make& make, const std::string& name, const Variable& variable) {
		switch (variable.export_mode) {
			case ExportMode::Export:
				return true;
			case ExportMode::Unexport:
				return false;
			case ExportMode::Default:
			default:
				break;
		}

		if (variable.origin == Origin::Default || variable.origin == Origin::Automatic) return false;
		if (!isExportableName(name)) return false;

		const bool from_outside = variable.origin == Origin::CommandLine
			|| variable.origin == Origin::Environment
			|| variable.origin == Origin::EnvironmentOverride;
		return from_outside || make.special.export_all;
	}

	// A target-specific value without an export word of its own goes out
	// exactly when the global variable of that name would.
	static bool exportedInScope(const Make& make, const std::string& name, const VarRef& found) {
		const Variable& variable = *found.variable;
		if (found.global || variable.export_mode != ExportMode::Default) {
			return isExported(make, name, variable);
		}

		const auto global = make.globals.find(name);
		if (global == make.globals.end()) return isExported(make, name, variable);

		return isExported(make, name, global->second);
	}

	static std::set<std::string> visibleNames(const Make& make, const ExpandContext& ctx) {
		std::set<std::string> names;
		for (const auto& entry : make.globals) names.insert(entry.first);
		for (const VarScope* level = ctx.scope; level != nullptr; level = level->parent) {
			for (const auto& entry : *level->table) names.insert(entry.first);
		}

		return names;
	}

	static std::string baseEnvironmentValue(const Make& make, const std::string& name) {
		for (const auto& entry : make.base_environment) {
			if (entry.first == name) return entry.second;
		}

		return {};
	}

	static void addInheritedShell(const Make& make, NameValueList& environment) {
		const std::string shell = baseEnvironmentValue(make, "SHELL");
		if (!shell.empty()) environment.emplace_back("SHELL", shell);
	}

	NameValueList childEnvironment(Make& make, const ExpandContext& ctx) {
		NameValueList environment;
		const bool was_computing = make.computing_environment;
		make.computing_environment = true;

		bool shell_exported = false;
		for (const std::string& name : visibleNames(make, ctx)) {
			const VarRef found = lookupVariable(make, name, ctx);
			if (found.variable == nullptr || name == "MAKELEVEL") continue;
			if (!exportedInScope(make, name, found)) continue;

			shell_exported = shell_exported || name == "SHELL";
			environment.emplace_back(name, expandVariable(make, name, ctx));
		}

		make.computing_environment = was_computing;
		if (!shell_exported) addInheritedShell(make, environment);
		environment.emplace_back("MAKELEVEL", std::to_string(make.level + 1));
		return environment;
	}

	std::vector<std::string> variableNames(const Make& make) {
		std::vector<std::string> names;
		for (const auto& entry : make.globals) names.push_back(entry.first);
		return names;
	}

}  // namespace wbsh::make_detail

/**
 * @file make_dump.cpp
 * @brief `make -p`: the variables, explicit targets, pattern rules and vpath
 *        directories in the layout of GNU make's database dump.
 */

#include "make_internal.h"

#include <string>
#include <vector>

namespace wbsh::make_detail {

	static void printVariable(const std::string& name, const Variable& variable) {
		printLine("# " + originName(variable.origin));
		const char* op = variable.flavor == Flavor::Simple ? " := " : " = ";
		printLine(name + op + variable.value);
	}

	static void printVariables(const Make& make) {
		printLine("");
		printLine("# Variables");
		printLine("");
		for (const auto& entry : make.globals) printVariable(entry.first, entry.second);
	}

	static std::string recipeSource(const Recipe& recipe) {
		if (recipe.where.file == "<builtin>") return "builtin";

		return "from '" + recipe.where.file + "', line " + std::to_string(recipe.where.line);
	}

	static void printRecipe(const Recipe& recipe) {
		if (!recipe.defined) return;

		printLine("#  recipe to execute (" + recipeSource(recipe) + "):");
		for (const RecipeLine& line : recipe.lines) printLine("\t" + line.text);
	}

	static std::string ruleLine(const std::string& targets, const std::string& colon,
	                            const std::vector<std::string>& prereqs,
	                            const std::vector<std::string>& order_only) {
		std::string line = targets + colon;
		if (!prereqs.empty()) line += " " + joinWords(prereqs);
		if (!order_only.empty()) line += " | " + joinWords(order_only);
		return line;
	}

	static void printFile(const FileNode& file) {
		printLine("");
		if (!file.is_target) printLine("# Not a target:");
		if (file.double_colon) {
			for (const DoubleColonRule& rule : file.double_colon_rules) {
				printLine(ruleLine(file.name, "::", rule.prereqs, rule.order_only));
				printRecipe(rule.recipe);
			}

			return;
		}

		printLine(ruleLine(file.name, ":", file.prereqs, file.order_only));
		if (file.phony) printLine("#  Phony target (prerequisite of .PHONY).");
		if (file.precious) printLine("#  Precious file (prerequisite of .PRECIOUS).");
		for (const auto& entry : file.vars) printLine("# " + entry.first + " = " + entry.second.value);
		printRecipe(file.recipe);
	}

	static void printFiles(const Make& make) {
		printLine("");
		printLine("# Files");
		for (const std::string& name : make.file_order) printFile(make.files.at(name));
	}

	static void printPatternRules(const Make& make) {
		printLine("");
		printLine("# Implicit Rules");
		for (const PatternRule& rule : make.pattern_rules) {
			printLine("");
			const std::string colon = rule.terminal ? "::" : ":";
			printLine(ruleLine(joinWords(rule.targets), colon, rule.prereqs, rule.order_only));
			printRecipe(rule.recipe);
		}
	}

	static void printVpaths(const Make& make) {
		printLine("");
		printLine("# VPATH Search Paths");
		for (const VpathEntry& entry : make.vpaths) {
			printLine("");
			printLine("vpath " + entry.pattern + " " + joinWords(entry.dirs));
		}
	}

	void printDatabase(Make& make) {
		printLine("# GNU Make-compatible make (wbsh) 4.4.1");
		printVariables(make);
		printFiles(make);
		printPatternRules(make);
		printVpaths(make);
		printLine("");
		printLine("# finished Make data base");
	}

}  // namespace wbsh::make_detail

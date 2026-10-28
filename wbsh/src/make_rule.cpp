/**
 * @file make_rule.cpp
 * @brief Rule lines: splitting targets from prerequisites (`:`, `::`, `&:`,
 *        `|` order-only, static patterns, `;` recipes), target- and
 *        pattern-specific variables, and recording rules and the special
 *        targets in the database.
 */

#include "make_internal.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace wbsh::make_detail {

	static std::string canonicalName(const std::string& name) {
		std::string canonical = name;
		while (canonical.size() > 2 && canonical.compare(0, 2, "./") == 0) {
			canonical.erase(0, 2);
		}

		return canonical;
	}

	FileNode& enterFile(Make& make, const std::string& name) {
		const std::string canonical = canonicalName(name);
		const auto found = make.files.find(canonical);
		if (found != make.files.end()) return found->second;

		FileNode& file = make.files[canonical];
		file.name = canonical;
		make.file_order.push_back(canonical);
		return file;
	}

	FileNode* findFile(Make& make, const std::string& name) {
		const auto found = make.files.find(canonicalName(name));
		return found == make.files.end() ? nullptr : &found->second;
	}

	static const char* const kSpecialTargets[] = {
		".PHONY", ".SUFFIXES", ".DEFAULT", ".PRECIOUS", ".INTERMEDIATE", ".SECONDARY",
		".NOTINTERMEDIATE", ".SECONDEXPANSION", ".DELETE_ON_ERROR", ".IGNORE",
		".LOW_RESOLUTION_TIME", ".SILENT", ".EXPORT_ALL_VARIABLES", ".NOTPARALLEL",
		".ONESHELL", ".POSIX", ".WAIT",
	};

	bool isSpecialTarget(const std::string& name) {
		for (const char* special : kSpecialTargets) {
			if (name == special) return true;
		}

		return false;
	}

	// A `C:` drive prefix in a target name is not the rule separator.
	static bool isDriveColon(const std::string& text, std::size_t colon) {
		if (colon == 0 || colon + 1 >= text.size()) return false;
		if (std::isalpha(static_cast<unsigned char>(text[colon - 1])) == 0) return false;
		if (colon >= 2 && !isBlank(text[colon - 2])) return false;

		return text[colon + 1] == '/' || text[colon + 1] == '\\';
	}

	static std::size_t findRuleColon(const std::string& text) {
		std::size_t from = 0;
		for (;;) {
			const std::size_t colon = findUnquoted(text, ":", from);
			if (colon == std::string::npos || !isDriveColon(text, colon)) return colon;
			from = colon + 1;
		}
	}

	static void setSpecialFlag(Make& make, const std::vector<std::string>& names,
	                           bool FileNode::*flag) {
		for (const std::string& name : names) enterFile(make, name).*flag = true;
	}

	static void applySuffixes(Make& make, const std::vector<std::string>& names) {
		if (names.empty()) {
			make.suffixes.clear();
			return;
		}

		for (const std::string& name : names) {
			if (std::find(make.suffixes.begin(), make.suffixes.end(), name) == make.suffixes.end()) {
				make.suffixes.push_back(name);
			}
		}
	}

	static void applyPrecious(Make& make, const std::vector<std::string>& names) {
		for (const std::string& name : names) {
			if (name.find('%') != std::string::npos) make.precious_patterns.push_back(name);
			else enterFile(make, name).precious = true;
		}
	}

	static void applyFlagOrAll(Make& make, const RuleSpec& spec, bool FileNode::*flag,
	                           bool& all) {
		if (spec.prereqs.empty()) all = true;
		else setSpecialFlag(make, spec.prereqs, flag);
	}

	static void applySpecialTarget(Make& make, const std::string& name, const RuleSpec& spec) {
		const std::vector<std::string>& names = spec.prereqs;
		if (name == ".PHONY") setSpecialFlag(make, names, &FileNode::phony);
		else if (name == ".SUFFIXES") applySuffixes(make, names);
		else if (name == ".DEFAULT" && spec.recipe.defined) enterFile(make, name).recipe = spec.recipe;
		else if (name == ".PRECIOUS") applyPrecious(make, names);
		else if (name == ".INTERMEDIATE") setSpecialFlag(make, names, &FileNode::intermediate);
		else if (name == ".NOTINTERMEDIATE") setSpecialFlag(make, names, &FileNode::not_intermediate);
		else if (name == ".SECONDARY") applyFlagOrAll(make, spec, &FileNode::secondary,
			make.special.secondary_all);
		else if (name == ".IGNORE") applyFlagOrAll(make, spec, &FileNode::ignore_errors,
			make.special.ignore_all);
		else if (name == ".SILENT") applyFlagOrAll(make, spec, &FileNode::silent,
			make.special.silent_all);
		else if (name == ".DELETE_ON_ERROR") make.special.delete_on_error = true;
		else if (name == ".EXPORT_ALL_VARIABLES") make.special.export_all = true;
		else if (name == ".ONESHELL") make.special.oneshell = true;
		else if (name == ".POSIX") make.special.posix = true;
		else if (name == ".SECONDEXPANSION") make.special.second_expansion = true;
	}

	static void addPrereqs(FileNode& file, const std::vector<std::string>& prereqs,
	                       bool has_recipe) {
		if (has_recipe) {
			file.prereqs.insert(file.prereqs.begin(), prereqs.begin(), prereqs.end());
			return;
		}

		file.prereqs.insert(file.prereqs.end(), prereqs.begin(), prereqs.end());
	}

	static void replaceRecipe(Make& make, FileNode& file, const Recipe& recipe) {
		if (file.recipe.defined && file.recipe.where.file != "<builtin>") {
			reportWarning(make, recipe.where, "warning: overriding recipe for target '"
				+ file.name + "'");
			reportWarning(make, file.recipe.where, "warning: ignoring old recipe for target '"
				+ file.name + "'");
		}

		file.recipe = recipe;
	}

	static bool checkColonKind(Make& make, const FileNode& file, bool double_colon) {
		if (!file.is_target || file.double_colon == double_colon) return true;

		reportFatal(make, make.where, "target file '" + file.name + "' has both : and :: entries");
		return false;
	}

	struct ExplicitRule {
		const RuleSpec& spec;
		std::vector<std::string> prereqs;
		std::vector<std::string> order_only;
		std::string stem;
	};

	static void mentionAll(Make& make, const std::vector<std::string>& names) {
		for (const std::string& name : names) {
			if (!isDeferredPrereq(name)) enterFile(make, name).mentioned = true;
		}
	}

	static void recordExplicit(Make& make, const std::string& name, const ExplicitRule& rule) {
		if (isSpecialTarget(name)) {
			applySpecialTarget(make, name, rule.spec);
			return;
		}

		FileNode& file = enterFile(make, name);
		if (!checkColonKind(make, file, rule.spec.double_colon)) return;

		file.is_target = true;
		file.mentioned = true;
		if (rule.spec.double_colon) {
			file.double_colon = true;
			file.double_colon_rules.push_back(
				DoubleColonRule{ rule.prereqs, rule.order_only, rule.spec.recipe });
		} else {
			addPrereqs(file, rule.prereqs, rule.spec.recipe.defined);
			file.order_only.insert(file.order_only.end(), rule.order_only.begin(),
				rule.order_only.end());
			if (rule.spec.recipe.defined) replaceRecipe(make, file, rule.spec.recipe);
			if (!rule.stem.empty()) file.stem = rule.stem;
		}

		if (rule.spec.grouped) file.group = rule.spec.targets;
		mentionAll(make, rule.prereqs);
		mentionAll(make, rule.order_only);
	}

	static bool isSuffixRuleName(const Make& make, const std::string& name) {
		for (const std::string& suffix : make.suffixes) {
			if (name == suffix) return true;
			if (name.compare(0, suffix.size(), suffix) != 0) continue;

			const std::string rest = name.substr(suffix.size());
			if (std::find(make.suffixes.begin(), make.suffixes.end(), rest) != make.suffixes.end()) {
				return true;
			}
		}

		return false;
	}

	static bool qualifiesAsDefaultGoal(const Make& make, const std::string& name) {
		if (name.find('%') != std::string::npos) return false;
		if (name[0] == '.' && name.find('/') == std::string::npos) return false;

		return !isSuffixRuleName(make, name);
	}

	static void considerDefaultGoal(Make& make, const std::vector<std::string>& targets) {
		const Variable* goal = findGlobal(make, ".DEFAULT_GOAL");
		if (make.reading_builtins || (goal != nullptr && !goal->value.empty())) return;

		for (const std::string& target : targets) {
			if (!qualifiesAsDefaultGoal(make, target)) continue;

			setGlobal(make, ".DEFAULT_GOAL", target, Origin::File);
			return;
		}
	}

	static std::vector<std::string> substituteAll(const std::vector<std::string>& patterns,
	                                              const std::string& stem) {
		std::vector<std::string> names;
		for (const std::string& pattern : patterns) {
			names.push_back(isDeferredPrereq(pattern)
				? deferStaticPrereqs(pattern)
				: substituteStem(pattern, stem));
		}

		return names;
	}

	static void recordStaticPattern(Make& make, const RuleSpec& spec) {
		for (const std::string& target : spec.targets) {
			std::string stem;
			ExplicitRule rule{ spec, spec.prereqs, spec.order_only, std::string() };
			if (matchPattern(spec.target_pattern, target, stem)) {
				rule.prereqs = substituteAll(spec.prereqs, stem);
				rule.order_only = substituteAll(spec.order_only, stem);
				rule.stem = stem;
			} else {
				reportWarning(make, make.where,
					"target '" + target + "' doesn't match the target pattern");
			}

			recordExplicit(make, target, rule);
		}
	}

	static void recordPatternRule(Make& make, const RuleSpec& spec) {
		PatternRule rule;
		rule.targets = spec.targets;
		rule.prereqs = spec.prereqs;
		rule.order_only = spec.order_only;
		rule.recipe = spec.recipe;
		rule.terminal = spec.double_colon;
		addPatternRule(make, rule);
	}

	static std::size_t countPatternTargets(const RuleSpec& spec) {
		std::size_t count = 0;
		for (const std::string& target : spec.targets) {
			if (target.find('%') != std::string::npos) ++count;
		}

		return count;
	}

	static void recordExplicitTargets(Make& make, const RuleSpec& spec) {
		for (const std::string& target : spec.targets) {
			recordExplicit(make, target, ExplicitRule{ spec, spec.prereqs, spec.order_only, {} });
		}
	}

	static void recordRule(Make& make, const RuleSpec& spec) {
		const std::size_t patterned = countPatternTargets(spec);
		if (spec.static_pattern) {
			considerDefaultGoal(make, spec.targets);
			recordStaticPattern(make, spec);
			return;
		}

		if (patterned == 0) {
			considerDefaultGoal(make, spec.targets);
			recordExplicitTargets(make, spec);
			return;
		}

		if (patterned != spec.targets.size()) {
			reportFatal(make, make.where, "mixed implicit and normal rules");
			return;
		}

		recordPatternRule(make, spec);
	}

	void finishRule(Make& make, Reader& reader) {
		if (!reader.in_rule) return;

		reader.in_rule = false;
		if (reader.rule_has_targets) recordRule(make, reader.rule);
		reader.rule = RuleSpec{};
	}

	static std::vector<std::string> expandNames(Make& make, const std::string& text) {
		std::vector<std::string> names;
		for (const std::string& word : splitWords(text)) {
			if (word == ".WAIT") continue;

			const std::vector<std::string> matches = hasGlobMeta(word)
				? globWord(make, word)
				: std::vector<std::string>();
			if (matches.empty()) names.push_back(word);
			else names.insert(names.end(), matches.begin(), matches.end());
		}

		return names;
	}

	static void reportMissingSeparator(Make& make, const std::string& line) {
		const bool eight_spaces = line.compare(0, 8, "        ") == 0;
		reportFatal(make, make.where, eight_spaces
			? "missing separator (did you mean TAB instead of 8 spaces?)"
			: "missing separator");
	}

	static bool splitStaticPattern(Make& make, std::string& rest, RuleSpec& spec) {
		const std::size_t second = findRuleColon(rest);
		if (second == std::string::npos) return true;

		const std::vector<std::string> patterns = splitWords(rest.substr(0, second));
		if (patterns.size() != 1) {
			reportFatal(make, make.where, "multiple target patterns");
			return false;
		}

		if (patterns[0].find('%') == std::string::npos) {
			reportFatal(make, make.where, "target pattern contains no '%'");
			return false;
		}

		spec.static_pattern = true;
		spec.target_pattern = patterns[0];
		rest = rest.substr(second + 1);
		return true;
	}

	static bool splitRule(Make& make, const std::string& expanded, RuleSpec& spec) {
		const std::size_t colon = findRuleColon(expanded);
		std::string targets = expanded.substr(0, colon);
		if (!targets.empty() && targets.back() == '&') {
			spec.grouped = true;
			targets.pop_back();
		}

		std::size_t after = colon + 1;
		if (after < expanded.size() && expanded[after] == ':') {
			spec.double_colon = true;
			++after;
		}

		std::string rest = expanded.substr(std::min(after, expanded.size()));
		if (!splitStaticPattern(make, rest, spec)) return false;

		spec.targets = expandNames(make, targets);
		if (make.special.second_expansion && rest.find('$') != std::string::npos) {
			spec.prereqs = { deferPrereqs(rest) };
			return true;
		}

		const std::size_t bar = rest.find('|');
		spec.prereqs = expandNames(make, rest.substr(0, bar));
		if (bar != std::string::npos) spec.order_only = expandNames(make, rest.substr(bar + 1));
		return true;
	}

	static bool parseTargetVariable(const std::string& text, std::string& targets,
	                                Assignment& assignment) {
		const std::size_t colon = findRuleColon(text);
		if (colon == std::string::npos) return false;

		std::size_t after = colon + 1;
		if (after < text.size() && text[after] == ':') ++after;
		if (!parseAssignment(text.substr(after), false, assignment)) return false;

		targets = text.substr(0, colon);
		if (!targets.empty() && targets.back() == '&') targets.pop_back();
		return true;
	}

	// The line is a target-specific assignment only when its operator comes
	// before any `;`; the value then runs on through the `;` to the end.
	static bool applyTargetVariableLine(Make& make, const std::string& rule_text,
	                                    const std::string& whole_line) {
		std::string targets;
		Assignment assignment;
		if (!parseTargetVariable(rule_text, targets, assignment)) return false;

		assignment = Assignment{};
		parseTargetVariable(whole_line, targets, assignment);
		applyTargetAssignment(make, expandNames(make, expand(make, targets, ExpandContext{})),
			assignment);
		return true;
	}

	static void startRule(Make& make, Reader& reader, const RuleSpec& spec) {
		reader.rule = spec;
		reader.in_rule = true;
		reader.rule_has_targets = !spec.targets.empty();
		reader.rule.recipe.where = make.where;
	}

	void parseRuleLine(Make& make, Reader& reader, const std::string& line) {
		const std::size_t cut = findUnquoted(line, ";#", 0);
		const bool inline_recipe = cut != std::string::npos && line[cut] == ';';
		const std::string rule_text = collapseContinuations(line.substr(0, cut));
		const std::string recipe_text = inline_recipe ? line.substr(cut + 1) : std::string();

		const std::string with_recipe = inline_recipe ? rule_text + ";" + recipe_text : rule_text;
		if (applyTargetVariableLine(make, rule_text, with_recipe)) return;

		const std::string expanded = expand(make, rule_text, ExpandContext{});
		if (make.fatal) return;
		if (findRuleColon(expanded) == std::string::npos) {
			if (!trimBlanks(expanded).empty()) reportMissingSeparator(make, line);
			return;
		}

		RuleSpec spec;
		if (!splitRule(make, expanded, spec)) return;

		startRule(make, reader, spec);
		if (!inline_recipe) return;

		reader.rule.recipe.defined = true;
		reader.rule.recipe.lines.push_back(RecipeLine{ dropRecipePrefixes(make, recipe_text),
			make.where });
	}

}  // namespace wbsh::make_detail

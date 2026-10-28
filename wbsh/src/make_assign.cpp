/**
 * @file make_assign.cpp
 * @brief Variable assignments: the `=` `:=` `::=` `:::=` `?=` `+=` `!=`
 *        operators with their modifiers (override, export, unexport,
 *        private, define, undefine), globally, per target and per pattern,
 *        plus the export / unexport directives and command-line assignments.
 */

#include "make_internal.h"

#include <string>
#include <vector>

namespace wbsh::make_detail {

	struct Operator {
		AssignOp op = AssignOp::Recursive;
		std::size_t length = 0;
	};

	static bool operatorAt(const std::string& text, std::size_t i, Operator& found) {
		static const struct {
			const char* text;
			AssignOp op;
		} kOperators[] = {
			{ ":::=", AssignOp::Immediate }, { "::=", AssignOp::Simple }, { ":=", AssignOp::Simple },
			{ "+=", AssignOp::Append }, { "?=", AssignOp::Conditional }, { "!=", AssignOp::Shell },
			{ "=", AssignOp::Recursive },
		};

		for (const auto& candidate : kOperators) {
			const std::string spelled = candidate.text;
			if (text.compare(i, spelled.size(), spelled) != 0) continue;

			found.op = candidate.op;
			found.length = spelled.size();
			return true;
		}

		return false;
	}

	// The name may be computed (`$(x)_y = 1`) but holds no blanks: a word
	// after a blank that does not start an operator makes this a rule line.
	static bool parseDefinition(const std::string& text, Assignment& out) {
		bool seen_blank = false;
		for (std::size_t i = 0; i < text.size(); ++i) {
			const char c = text[i];
			Operator found;
			if (operatorAt(text, i, found)) {
				out.name = trimBlanks(text.substr(0, i));
				out.op = found.op;
				out.value = text.substr(skipBlanks(text, i + found.length));
				return !out.name.empty();
			}

			if (isBlank(c)) {
				seen_blank = true;
				continue;
			}

			if (seen_blank || c == ':') return false;
			if (!opensReference(text, i)) continue;

			const std::size_t end = findReferenceEnd(text, i);
			if (end == std::string::npos) return false;
			i = end;
		}

		return false;
	}

	static bool parseDefineHead(const std::string& text, Assignment& out) {
		std::size_t end = 0;
		while (end < text.size() && !isBlank(text[end]) && text[end] != '=') ++end;
		if (end == 0) return false;

		out.name = text.substr(0, end);
		const std::size_t op_start = skipBlanks(text, end);
		Operator found;
		if (op_start < text.size() && operatorAt(text, op_start, found)) out.op = found.op;
		return true;
	}

	static bool applyModifier(const std::string& word, Assignment& out) {
		if (word == "export") out.export_mode = ExportMode::Export;
		else if (word == "unexport") out.export_mode = ExportMode::Unexport;
		else if (word == "override") out.is_override = true;
		else if (word == "private") out.is_private = true;
		else return false;

		return true;
	}

	bool parseAssignment(const std::string& line, bool allow_define, Assignment& out) {
		std::size_t start = skipBlanks(line, 0);
		while (start < line.size()) {
			const std::string text = line.substr(start);
			if (parseDefinition(text, out)) return true;

			std::size_t end = 0;
			while (end < text.size() && !isBlank(text[end])) ++end;
			const std::string word = text.substr(0, end);
			const std::string rest = text.substr(skipBlanks(text, end));
			if (allow_define && (word == "define" || word == "undefine")) {
				out.is_define = word == "define";
				out.is_undefine = word == "undefine";
				return parseDefineHead(rest, out);
			}

			if (!applyModifier(word, out)) return false;
			start += skipBlanks(text, end);
		}

		return false;
	}

	static std::string escapeDollars(const std::string& text) {
		std::string escaped;
		for (const char c : text) {
			escaped.push_back(c);
			if (c == '$') escaped.push_back('$');
		}

		return escaped;
	}

	static std::string shellAssignmentValue(Make& make, const std::string& command) {
		std::string output;
		int status = 0;
		captureShell(make, expand(make, command, ExpandContext{}), ExpandContext{}, output, status);
		setGlobal(make, ".SHELLSTATUS", std::to_string(status), Origin::Override);
		return shellOutputValue(output);
	}

	static Variable evaluatedValue(Make& make, const Assignment& assignment) {
		Variable variable;
		variable.value = assignment.value;
		switch (assignment.op) {
			case AssignOp::Simple:
				variable.value = expand(make, assignment.value, ExpandContext{});
				variable.flavor = Flavor::Simple;
				break;
			case AssignOp::Immediate:
				variable.value = escapeDollars(expand(make, assignment.value, ExpandContext{}));
				break;
			case AssignOp::Shell:
				variable.value = shellAssignmentValue(make, assignment.value);
				break;
			case AssignOp::Recursive:
			case AssignOp::Conditional:
			case AssignOp::Append:
			default:
				break;
		}

		return variable;
	}

	static Variable appendedValue(Make& make, const Variable& existing, const std::string& text) {
		Variable combined = existing;
		const std::string addition = existing.flavor == Flavor::Simple
			? expand(make, text, ExpandContext{})
			: text;
		if (!combined.value.empty()) combined.value.push_back(' ');
		combined.value += addition;
		return combined;
	}

	static void assignGlobal(Make& make, const std::string& name, const Assignment& assignment,
	                         Origin origin) {
		Variable* existing = findGlobal(make, name);
		if (assignment.op == AssignOp::Conditional && existing != nullptr) return;

		Variable variable = assignment.op == AssignOp::Append && existing != nullptr
			? appendedValue(make, *existing, assignment.value)
			: evaluatedValue(make, assignment);
		variable.origin = origin;
		variable.export_mode = assignment.export_mode;
		variable.is_private = assignment.is_private;
		variable.append = false;
		defineVariable(make.globals, name, variable);
	}

	static Origin assignmentOrigin(const Make& make, const Assignment& assignment) {
		if (assignment.is_override) return Origin::Override;
		return make.reading_builtins ? Origin::Default : Origin::File;
	}

	static void undefineGlobal(Make& make, const std::string& name, Origin origin) {
		const Variable* existing = findGlobal(make, name);
		if (existing == nullptr || existing->origin > origin) return;

		make.globals.erase(name);
	}

	void applyAssignment(Make& make, Reader& reader, Assignment& assignment) {
		const std::string name = trimBlanks(expand(make, assignment.name, ExpandContext{}));
		if (name.empty()) {
			reportFatal(make, make.where, "empty variable name");
			return;
		}

		const Origin origin = assignmentOrigin(make, assignment);
		if (assignment.is_undefine) {
			undefineGlobal(make, name, origin);
			return;
		}

		if (assignment.is_define && !readDefineBody(make, reader, assignment.value)) return;

		assignGlobal(make, name, assignment, origin);
	}

	static VarTable& patternVarTable(Make& make, const std::string& pattern) {
		for (PatternVars& entry : make.pattern_vars) {
			if (entry.pattern == pattern) return entry.vars;
		}

		make.pattern_vars.push_back(PatternVars{ pattern, VarTable{} });
		return make.pattern_vars.back().vars;
	}

	static bool commandLineWins(Make& make, const std::string& name, const Assignment& assignment) {
		if (assignment.is_override) return false;

		const Variable* global = findGlobal(make, name);
		return global != nullptr
			&& (global->origin == Origin::CommandLine || global->origin == Origin::EnvironmentOverride);
	}

	static Variable targetVariable(Make& make, VarTable& table, const std::string& name,
	                               const Assignment& assignment) {
		const auto existing = table.find(name);
		if (assignment.op == AssignOp::Append && existing != table.end()) {
			return appendedValue(make, existing->second, assignment.value);
		}

		Variable variable = evaluatedValue(make, assignment);
		variable.append = assignment.op == AssignOp::Append;
		return variable;
	}

	static void defineTargetVariable(Make& make, VarTable& table, const Assignment& assignment) {
		const std::string name = trimBlanks(expand(make, assignment.name, ExpandContext{}));
		if (name.empty() || commandLineWins(make, name, assignment)) return;

		const bool defined = table.count(name) != 0 || findGlobal(make, name) != nullptr;
		if (assignment.op == AssignOp::Conditional && defined) return;

		Variable variable = targetVariable(make, table, name, assignment);
		variable.origin = assignment.is_override ? Origin::Override : Origin::File;
		variable.export_mode = assignment.export_mode;
		variable.is_private = assignment.is_private;
		table[name] = variable;
	}

	void applyTargetAssignment(Make& make, const std::vector<std::string>& targets,
	                           const Assignment& assignment) {
		for (const std::string& target : targets) {
			VarTable& table = target.find('%') != std::string::npos
				? patternVarTable(make, target)
				: enterFile(make, target).vars;
			defineTargetVariable(make, table, assignment);
		}
	}

	static void markExport(Make& make, const std::string& name, ExportMode mode) {
		Variable* variable = findGlobal(make, name);
		if (variable == nullptr) {
			setGlobal(make, name, std::string(), Origin::File);
			variable = findGlobal(make, name);
		}

		variable->export_mode = mode;
	}

	bool applyExportDirective(Make& make, const std::string& line) {
		const std::size_t word_end = line.find_first_of(" \t");
		const bool exporting = line.compare(0, word_end, "export") == 0;
		const std::string rest = word_end == std::string::npos ? std::string() : line.substr(word_end);
		const std::vector<std::string> names = splitWords(expand(make, rest, ExpandContext{}));
		if (names.empty()) {
			make.special.export_all = exporting;
			return true;
		}

		for (const std::string& name : names) {
			markExport(make, name, exporting ? ExportMode::Export : ExportMode::Unexport);
		}

		return true;
	}

	void applyCommandLineAssignment(Make& make, const std::string& text) {
		Assignment assignment;
		if (!parseDefinition(text, assignment)) return;

		assignGlobal(make, assignment.name, assignment, Origin::CommandLine);
	}

	bool isCommandLineAssignment(const std::string& text) {
		Assignment assignment;
		return parseDefinition(text, assignment);
	}

}  // namespace wbsh::make_detail

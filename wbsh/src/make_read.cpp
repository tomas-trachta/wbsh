/**
 * @file make_read.cpp
 * @brief Reading makefiles: logical lines, comments, recipe lines, the
 *        conditional directives, include and the other directives, and the
 *        dispatch of each line to assignment or rule parsing.
 */

#include "make_internal.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "coreutils_internal.h"
#include "executor.h"

namespace wbsh::make_detail {

	static std::vector<std::string> splitLines(const std::string& text) {
		std::vector<std::string> lines;
		std::size_t start = 0;
		while (start < text.size()) {
			std::size_t end = text.find('\n', start);
			if (end == std::string::npos) end = text.size();

			std::string line = text.substr(start, end - start);
			if (!line.empty() && line.back() == '\r') line.pop_back();
			lines.push_back(std::move(line));
			start = end + 1;
		}

		return lines;
	}

	static bool endsWithContinuation(const std::string& line) {
		std::size_t backslashes = 0;
		for (std::size_t i = line.size(); i > 0 && line[i - 1] == '\\'; --i) ++backslashes;
		return backslashes % 2 == 1;
	}

	std::string takeLogicalLine(Reader& reader) {
		if (reader.fixed_line == 0) reader.line_number = static_cast<int>(reader.next) + 1;

		std::string line = reader.lines[reader.next++];
		while (endsWithContinuation(line) && reader.next < reader.lines.size()) {
			line.push_back('\n');
			line += reader.lines[reader.next++];
		}

		return line;
	}

	static void trimTrailingBlanks(std::string& text) {
		while (!text.empty() && isBlank(text.back())) text.pop_back();
	}

	std::string collapseContinuations(const std::string& line) {
		std::string result;
		std::size_t i = 0;
		for (;;) {
			const std::size_t newline = line.find('\n', i);
			if (newline == std::string::npos) {
				result.append(line, i, std::string::npos);
				return result;
			}

			result.append(line, i, newline - 1 - i);
			trimTrailingBlanks(result);
			result.push_back(' ');
			i = skipBlanks(line, newline + 1);
		}
	}

	// `\#` is a literal `#`; of the backslashes before a `#`, every pair
	// stands for one backslash and an odd one out escapes the `#`.
	static bool takeBackslashRun(const std::string& line, std::size_t& i, std::string& out) {
		std::size_t run = 0;
		while (i + run < line.size() && line[i + run] == '\\') ++run;
		if (i + run >= line.size() || line[i + run] != '#') {
			out.append(line, i, run);
			i += run;
			return false;
		}

		out.append(run / 2, '\\');
		i += run;
		if (run % 2 == 0) return true;

		out.push_back('#');
		++i;
		return false;
	}

	std::string stripComment(const std::string& line) {
		std::string out;
		std::size_t i = 0;
		while (i < line.size()) {
			const char c = line[i];
			if (c == '$' && i + 1 < line.size() && line[i + 1] == '$') {
				out.append("$$");
				i += 2;
				continue;
			}

			if (opensReference(line, i)) {
				const std::size_t end = findReferenceEnd(line, i);
				const std::size_t stop = end == std::string::npos ? line.size() : end + 1;
				out.append(line, i, stop - i);
				i = stop;
				continue;
			}

			if (c == '\\') {
				if (takeBackslashRun(line, i, out)) return out;
				continue;
			}

			if (c == '#') return out;

			out.push_back(c);
			++i;
		}

		return out;
	}

	char recipePrefix(Make& make) {
		const Variable* prefix = findGlobal(make, ".RECIPEPREFIX");
		if (prefix == nullptr || prefix->value.empty()) return '\t';

		return prefix->value[0];
	}

	bool ignoring(const Reader& reader) {
		return !reader.conditionals.empty() && reader.conditionals.back().ignoring;
	}

	static std::string firstWord(const std::string& text, std::string& rest) {
		std::size_t end = 0;
		while (end < text.size() && !isBlank(text[end])) ++end;
		rest = text.substr(skipBlanks(text, end));
		return text.substr(0, end);
	}

	static bool isDefineLine(const std::string& text, const char* keyword) {
		std::string rest;
		return firstWord(text, rest) == keyword;
	}

	bool readDefineBody(Make& make, Reader& reader, std::string& body) {
		const Location start = make.where;
		int depth = 1;
		std::vector<std::string> lines;
		while (reader.next < reader.lines.size()) {
			const std::string line = collapseContinuations(takeLogicalLine(reader));
			const bool directive_line = line.empty() || line[0] != recipePrefix(make);
			const std::string text = line.substr(skipBlanks(line, 0));
			if (directive_line && isDefineLine(text, "define")) ++depth;
			if (directive_line && isDefineLine(stripComment(text), "endef") && --depth == 0) {
				body = joinLines(lines);
				return true;
			}

			lines.push_back(line);
		}

		reportFatal(make, start, "missing 'endef', unterminated 'define'");
		return false;
	}

	std::string joinLines(const std::vector<std::string>& lines) {
		std::string joined;
		for (std::size_t i = 0; i < lines.size(); ++i) {
			if (i > 0) joined.push_back('\n');
			joined += lines[i];
		}

		return joined;
	}

	static bool splitParenArguments(const std::string& text, std::string& first,
	                                std::string& second, std::string& trailing) {
		int depth = 0;
		std::size_t comma = std::string::npos;
		for (std::size_t i = 1; i < text.size() && comma == std::string::npos; ++i) {
			if (text[i] == '(') ++depth;
			else if (text[i] == ')') --depth;
			else if (text[i] == ',' && depth <= 0) comma = i;
		}

		if (comma == std::string::npos) return false;

		first = text.substr(1, comma - 1);
		trimTrailingBlanks(first);
		const std::size_t second_start = skipBlanks(text, comma + 1);
		depth = 0;
		for (std::size_t i = second_start; i < text.size(); ++i) {
			if (text[i] == '(') ++depth;
			if (text[i] != ')') continue;
			if (depth-- > 0) continue;

			second = text.substr(second_start, i - second_start);
			trailing = text.substr(i + 1);
			return true;
		}

		return false;
	}

	static bool splitQuotedArguments(const std::string& text, std::string& first,
	                                 std::string& second, std::string& trailing) {
		const std::size_t first_end = text.find(text[0], 1);
		if (first_end == std::string::npos) return false;

		first = text.substr(1, first_end - 1);
		const std::size_t second_start = skipBlanks(text, first_end + 1);
		if (second_start >= text.size() || (text[second_start] != '"' && text[second_start] != '\'')) {
			return false;
		}

		const std::size_t second_end = text.find(text[second_start], second_start + 1);
		if (second_end == std::string::npos) return false;

		second = text.substr(second_start + 1, second_end - second_start - 1);
		trailing = text.substr(second_end + 1);
		return true;
	}

	static bool splitConditionArguments(const std::string& rest, std::string& first,
	                                    std::string& second, std::string& trailing) {
		if (rest.empty()) return false;
		if (rest[0] == '(') return splitParenArguments(rest, first, second, trailing);
		if (rest[0] != '"' && rest[0] != '\'') return false;

		return splitQuotedArguments(rest, first, second, trailing);
	}

	static bool evaluateEquality(Make& make, const std::string& word, const std::string& rest,
	                             bool& value) {
		std::string first;
		std::string second;
		std::string trailing;
		if (!splitConditionArguments(rest, first, second, trailing)) {
			reportFatal(make, make.where, "invalid syntax in conditional");
			return false;
		}

		if (!trimBlanks(trailing).empty()) {
			reportWarning(make, make.where, "extraneous text after '" + word + "' directive");
		}

		const ExpandContext global;
		const bool equal = expand(make, first, global) == expand(make, second, global);
		value = equal == (word == "ifeq");
		return true;
	}

	static bool evaluateDefined(Make& make, const std::string& word, const std::string& rest,
	                            bool& value) {
		const std::string name = trimBlanks(expand(make, rest, ExpandContext{}));
		if (name.find_first_of(" \t") != std::string::npos) {
			reportFatal(make, make.where, "invalid syntax in conditional");
			return false;
		}

		const VarRef found = lookupVariable(make, name, ExpandContext{});
		const bool defined = found.variable != nullptr && !found.variable->value.empty();
		value = defined == (word == "ifdef");
		return true;
	}

	static bool isConditionalWord(const std::string& word) {
		return word == "ifeq" || word == "ifneq" || word == "ifdef" || word == "ifndef";
	}

	static bool evaluateCondition(Make& make, const std::string& word, const std::string& rest,
	                              bool& value) {
		if (word == "ifeq" || word == "ifneq") return evaluateEquality(make, word, rest, value);
		return evaluateDefined(make, word, rest, value);
	}

	static void pushConditional(Make& make, Reader& reader, const std::string& word,
	                            const std::string& rest) {
		Conditional conditional;
		conditional.outer_ignoring = ignoring(reader);
		if (conditional.outer_ignoring) {
			conditional.ignoring = true;
			conditional.taken = true;
			reader.conditionals.push_back(conditional);
			return;
		}

		bool value = false;
		if (!evaluateCondition(make, word, rest, value)) return;

		conditional.ignoring = !value;
		conditional.taken = value;
		reader.conditionals.push_back(conditional);
	}

	static void chainElseCondition(Make& make, Conditional& conditional, const std::string& rest) {
		std::string condition_rest;
		const std::string word = firstWord(rest, condition_rest);
		if (!isConditionalWord(word)) {
			reportWarning(make, make.where, "extraneous text after 'else' directive");
			conditional.seen_else = true;
			conditional.ignoring = conditional.outer_ignoring || conditional.taken;
			conditional.taken = true;
			return;
		}

		if (conditional.outer_ignoring || conditional.taken) {
			conditional.ignoring = true;
			return;
		}

		bool value = false;
		if (!evaluateCondition(make, word, condition_rest, value)) return;

		conditional.ignoring = !value;
		conditional.taken = value;
	}

	static void processElse(Make& make, Reader& reader, const std::string& rest) {
		if (reader.conditionals.empty()) {
			reportFatal(make, make.where, "extraneous 'else'");
			return;
		}

		Conditional& conditional = reader.conditionals.back();
		if (conditional.seen_else) {
			reportFatal(make, make.where, "only one 'else' per conditional");
			return;
		}

		if (!rest.empty()) {
			chainElseCondition(make, conditional, rest);
			return;
		}

		conditional.seen_else = true;
		conditional.ignoring = conditional.outer_ignoring || conditional.taken;
		conditional.taken = true;
	}

	static bool processConditional(Make& make, Reader& reader, const std::string& text) {
		std::string rest;
		const std::string word = firstWord(text, rest);
		if (isConditionalWord(word)) {
			pushConditional(make, reader, word, rest);
			return true;
		}

		if (word == "else") {
			processElse(make, reader, rest);
			return true;
		}

		if (word != "endif") return false;

		if (reader.conditionals.empty()) reportFatal(make, make.where, "extraneous 'endif'");
		else reader.conditionals.pop_back();
		return true;
	}

	static std::string locateInclude(Make& make, const std::string& name) {
		if (fileExists(make, name)) return name;
		if (name.empty() || name[0] == '/' || (name.size() > 1 && name[1] == ':')) return {};

		for (const std::string& dir : make.options.include_dirs) {
			const std::string candidate = dir + "/" + name;
			if (fileExists(make, candidate)) return candidate;
		}

		return {};
	}

	static void includeOne(Make& make, const std::string& name, bool optional) {
		const std::string found = locateInclude(make, name);
		IncludedMakefile record;
		record.name = found.empty() ? name : found;
		record.where = make.where;
		record.optional = optional;
		record.missing = found.empty();
		make.includes.push_back(record);
		if (found.empty()) return;

		const Location saved = make.where;
		readMakefile(make, found);
		make.where = saved;
	}

	static void processInclude(Make& make, Reader& reader, const std::string& word,
	                           const std::string& rest) {
		finishRule(make, reader);
		const bool optional = word != "include";
		for (const std::string& name : splitWords(expand(make, rest, ExpandContext{}))) {
			if (make.fatal) return;
			if (!hasGlobMeta(name)) {
				includeOne(make, name, optional);
				continue;
			}

			for (const std::string& match : globWord(make, name)) includeOne(make, match, optional);
		}
	}

	static bool processDirective(Make& make, Reader& reader, const std::string& text) {
		std::string rest;
		const std::string word = firstWord(text, rest);
		if (word == "export" || word == "unexport") {
			finishRule(make, reader);
			return applyExportDirective(make, text);
		}

		if (word == "vpath") {
			finishRule(make, reader);
			applyVpathDirective(make, expand(make, rest, ExpandContext{}));
			return true;
		}

		if (word == "include" || word == "-include" || word == "sinclude") {
			processInclude(make, reader, word, rest);
			return true;
		}

		if (word == "endef") {
			reportFatal(make, make.where, "extraneous 'endef'");
			return true;
		}

		return false;
	}

	// A continued recipe line keeps its backslash-newlines for the shell;
	// only the recipe prefix starting each next line goes.
	std::string dropRecipePrefixes(Make& make, const std::string& text) {
		const char prefix = recipePrefix(make);
		std::string out;
		for (std::size_t i = 0; i < text.size(); ++i) {
			out.push_back(text[i]);
			if (text[i] == '\n' && i + 1 < text.size() && text[i + 1] == prefix) ++i;
		}

		return out;
	}

	static void addRecipeLine(Make& make, Reader& reader, const std::string& line) {
		if (ignoring(reader) || !reader.rule_has_targets) return;

		reader.rule.recipe.defined = true;
		reader.rule.recipe.lines.push_back(RecipeLine{ dropRecipePrefixes(make, line.substr(1)),
			make.where });
	}

	static bool processAssignmentLine(Make& make, Reader& reader, const std::string& text) {
		Assignment assignment;
		if (!parseAssignment(text, true, assignment)) return false;

		if (ignoring(reader)) {
			std::string discarded;
			if (assignment.is_define) readDefineBody(make, reader, discarded);
			return true;
		}

		finishRule(make, reader);
		applyAssignment(make, reader, assignment);
		return true;
	}

	static void processLine(Make& make, Reader& reader, const std::string& line) {
		const char prefix = recipePrefix(make);
		if (reader.in_rule && !line.empty() && line[0] == prefix) {
			addRecipeLine(make, reader, line);
			return;
		}

		const std::string collapsed = stripComment(collapseContinuations(line));
		const std::string text = collapsed.substr(skipBlanks(collapsed, 0));
		if (processAssignmentLine(make, reader, text)) return;
		if (trimBlanks(text).empty()) return;
		if (processConditional(make, reader, text)) return;
		if (ignoring(reader)) return;
		if (processDirective(make, reader, text)) return;

		if (!line.empty() && line[0] == prefix) {
			reportFatal(make, make.where, "recipe commences before first target");
			return;
		}

		finishRule(make, reader);
		parseRuleLine(make, reader, line);
	}

	static void evalReader(Make& make, Reader& reader) {
		while (reader.next < reader.lines.size() && !make.fatal) {
			const std::string line = takeLogicalLine(reader);
			make.where = Location{ reader.name, reader.line_number };
			processLine(make, reader, line);
		}

		finishRule(make, reader);
		if (!reader.conditionals.empty() && !make.fatal) {
			reportFatal(make, make.where, "missing 'endif'");
		}
	}

	static void appendMakefileList(Make& make, const std::string& name) {
		Variable* list = findGlobal(make, "MAKEFILE_LIST");
		const std::string value = list == nullptr ? std::string() : list->value;
		setGlobal(make, "MAKEFILE_LIST", value + " " + name, Origin::File);
	}

	bool readMakefileText(Make& make, const std::string& name, const std::string& text) {
		if (!make.reading_builtins) appendMakefileList(make, name);

		Reader reader;
		reader.name = name;
		reader.lines = splitLines(text);
		evalReader(make, reader);
		return !make.fatal;
	}

	static bool readStream(std::FILE* file, std::string& text) {
		char buffer[8192];
		std::size_t got = 0;
		while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, got);
		return std::ferror(file) == 0;
	}

	bool readMakefile(Make& make, const std::string& name) {
		std::string text;
		if (name == "-") {
			readStream(stdin, text);
			return readMakefileText(make, name, text);
		}

		std::FILE* file = fopenNative(make.exec, name, "rb");
		if (file == nullptr) return false;

		readStream(file, text);
		std::fclose(file);
		return readMakefileText(make, name, text);
	}

	void evalText(Make& make, const std::string& text) {
		const Location saved = make.where;
		Reader reader;
		reader.name = saved.file;
		reader.fixed_line = saved.line;
		reader.line_number = saved.line;
		reader.lines = splitLines(text);
		evalReader(make, reader);
		make.where = saved;
	}

}  // namespace wbsh::make_detail

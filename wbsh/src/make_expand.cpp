/**
 * @file make_expand.cpp
 * @brief Variable expansion: `$X`, `$(name)`, `${name}`, computed names,
 *        substitution references, automatic variables with their D / F
 *        forms, `+=` chains through target-specific levels, and the
 *        self-reference check.
 */

#include "make_internal.h"

#include <algorithm>
#include <string>
#include <vector>

namespace wbsh::make_detail {

	static void appendVariable(Make& make, const std::string& name, const ExpandContext& ctx,
	                           std::string& out);

	static bool isAutomaticBase(char c) {
		return std::string("@<^+?*|%").find(c) != std::string::npos;
	}

	bool isAutomaticName(const std::string& name) {
		if (name.empty() || name.size() > 2 || !isAutomaticBase(name[0])) return false;
		return name.size() == 1 || name[1] == 'D' || name[1] == 'F';
	}

	static std::vector<std::string> automaticWords(const AutoVars& autos, char base) {
		switch (base) {
			case '@':
				return { autos.target };
			case '<':
				if (autos.prereqs.empty()) return {};
				return { autos.prereqs.front() };
			case '^':
				return autos.prereqs;
			case '+':
				return autos.all_prereqs;
			case '?':
				return autos.newer;
			case '|':
				return autos.order_only;
			case '*':
				return { autos.stem };
			default:
				return {};
		}
	}

	static std::string directoryWithoutSlash(const std::string& word) {
		const std::size_t slash = word.find_last_of("/\\");
		if (slash == std::string::npos) return ".";
		if (slash == 0) return word.substr(0, 1);

		return word.substr(0, slash);
	}

	static void appendAutomatic(const ExpandContext& ctx, const std::string& name,
	                            std::string& out) {
		if (ctx.autos == nullptr) return;

		std::vector<std::string> words = automaticWords(*ctx.autos, name[0]);
		const char modifier = name.size() == 2 ? name[1] : '\0';
		for (std::string& word : words) {
			if (modifier == 'D') word = directoryWithoutSlash(word);
			if (modifier == 'F') word = notDirectoryOf(word);
		}

		if (words.size() == 1 && words.front().empty()) return;
		out += joinWords(words);
	}

	static void appendRecursive(Make& make, const std::string& name, const Variable& variable,
	                            const ExpandContext& ctx, std::string& out) {
		const bool active = std::find(make.expanding.begin(), make.expanding.end(), &variable)
			!= make.expanding.end();
		if (active) {
			reportFatal(make, make.where,
				"Recursive variable '" + name + "' references itself (eventually)");
			return;
		}

		const std::string value = variable.value;
		make.expanding.push_back(&variable);
		expandInto(make, value, ctx, out);
		make.expanding.pop_back();
	}

	static void appendFlavored(Make& make, const std::string& name, const Variable& variable,
	                           const ExpandContext& ctx, std::string& out) {
		if (variable.flavor == Flavor::Simple) {
			out += variable.value;
			return;
		}

		appendRecursive(make, name, variable, ctx, out);
	}

	// A target-specific `+=` adds to whatever the levels below it say.
	static void appendFound(Make& make, const std::string& name, const VarRef& found,
	                        const ExpandContext& ctx, std::string& out) {
		if (!found.variable->append) {
			appendFlavored(make, name, *found.variable, ctx, out);
			return;
		}

		std::string combined;
		const VarRef below = lookupBelow(make, name, found, ctx);
		if (below.variable != nullptr) appendFound(make, name, below, ctx, combined);
		if (!combined.empty()) combined.push_back(' ');
		appendFlavored(make, name, *found.variable, ctx, combined);
		out += combined;
	}

	static void appendUndefined(Make& make, const std::string& name, std::string& out) {
		if (name == ".VARIABLES") {
			out += joinWords(variableNames(make));
			return;
		}

		if (make.options.warn_undefined) {
			reportWarning(make, make.where, "warning: undefined variable '" + name + "'");
		}
	}

	static void appendVariable(Make& make, const std::string& name, const ExpandContext& ctx,
	                           std::string& out) {
		if (isAutomaticName(name)) {
			appendAutomatic(ctx, name, out);
			return;
		}

		const VarRef found = lookupVariable(make, name, ctx);
		if (found.variable == nullptr) {
			appendUndefined(make, name, out);
			return;
		}

		appendFound(make, name, found, ctx, out);
	}

	static bool appendSubstitutionRef(Make& make, const std::string& name,
	                                  const ExpandContext& ctx, std::string& out) {
		const std::size_t colon = name.find(':');
		if (colon == std::string::npos) return false;

		const std::size_t equals = name.find('=', colon + 1);
		if (equals == std::string::npos) return false;

		const std::string value = expandVariable(make, name.substr(0, colon), ctx);
		if (value.empty()) return true;

		std::string pattern = name.substr(colon + 1, equals - colon - 1);
		std::string replacement = name.substr(equals + 1);
		if (pattern.find('%') == std::string::npos) {
			pattern = "%" + pattern;
			replacement = "%" + replacement;
		}

		out += patsubstText(pattern, replacement, value);
		return true;
	}

	static void expandReference(Make& make, const std::string& inner, char close,
	                            const ExpandContext& ctx, std::string& out) {
		if (callFunction(make, inner, close, ctx, out)) return;

		const std::string name = inner.find('$') == std::string::npos
			? inner
			: expand(make, inner, ctx);
		if (appendSubstitutionRef(make, name, ctx, out)) return;

		appendVariable(make, name, ctx, out);
	}

	static std::size_t expandBracketed(Make& make, const std::string& text, std::size_t dollar,
	                                   const ExpandContext& ctx, std::string& out) {
		const std::size_t end = findReferenceEnd(text, dollar);
		if (end == std::string::npos) {
			reportFatal(make, make.where, "unterminated variable reference");
			return text.size();
		}

		const std::string inner = text.substr(dollar + 2, end - dollar - 2);
		expandReference(make, inner, text[end], ctx, out);
		return end + 1;
	}

	static std::size_t expandDollar(Make& make, const std::string& text, std::size_t dollar,
	                                const ExpandContext& ctx, std::string& out) {
		if (dollar + 1 >= text.size()) return text.size();

		const char next = text[dollar + 1];
		if (next == '$') {
			out.push_back('$');
			return dollar + 2;
		}

		if (next == '(' || next == '{') return expandBracketed(make, text, dollar, ctx, out);

		appendVariable(make, std::string(1, next), ctx, out);
		return dollar + 2;
	}

	void expandInto(Make& make, const std::string& text, const ExpandContext& ctx,
	                std::string& out) {
		std::size_t i = 0;
		while (i < text.size() && !make.fatal) {
			const std::size_t dollar = text.find('$', i);
			if (dollar == std::string::npos) {
				out.append(text, i, std::string::npos);
				return;
			}

			out.append(text, i, dollar - i);
			i = expandDollar(make, text, dollar, ctx, out);
		}
	}

	std::string expand(Make& make, const std::string& text, const ExpandContext& ctx) {
		std::string out;
		expandInto(make, text, ctx, out);
		return out;
	}

	std::string expandVariable(Make& make, const std::string& name, const ExpandContext& ctx) {
		std::string out;
		appendVariable(make, name, ctx, out);
		return out;
	}

	std::string rawVariableValue(const Make& make, const std::string& name,
	                             const ExpandContext& ctx) {
		const VarRef found = lookupVariable(make, name, ctx);
		if (found.variable == nullptr) return {};

		return found.variable->value;
	}

}  // namespace wbsh::make_detail

/**
 * @file make_functions.cpp
 * @brief The function table, argument splitting, and the text functions:
 *        subst, patsubst, strip, findstring, filter, filter-out, sort, word,
 *        wordlist, words, firstword, lastword, dir, notdir, suffix, basename,
 *        addsuffix, addprefix and join.
 */

#include "make_internal.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "numparse.h"

namespace wbsh::make_detail {

	using FunctionImpl = void (*)(FunctionCall& call, std::string& out);

	struct FunctionSpec {
		const char* name;
		std::size_t min_args;
		std::size_t max_args;
		bool expand_args;
		FunctionImpl impl;
	};

	static void functionSubst(FunctionCall& call, std::string& out) {
		const std::string& from = call.args[0];
		const std::string& to = call.args[1];
		const std::string& text = call.args[2];
		if (from.empty()) {
			out += text + to;
			return;
		}

		std::size_t start = 0;
		for (;;) {
			const std::size_t hit = text.find(from, start);
			if (hit == std::string::npos) break;
			out.append(text, start, hit - start);
			out += to;
			start = hit + from.size();
		}

		out.append(text, start, std::string::npos);
	}

	static void functionPatsubst(FunctionCall& call, std::string& out) {
		out += patsubstText(call.args[0], call.args[1], call.args[2]);
	}

	static void functionStrip(FunctionCall& call, std::string& out) {
		out += joinWords(splitWords(call.args[0]));
	}

	static void functionFindstring(FunctionCall& call, std::string& out) {
		if (call.args[1].find(call.args[0]) != std::string::npos) out += call.args[0];
	}

	static bool matchesAny(const std::vector<std::string>& patterns, const std::string& word) {
		std::string stem;
		for (const std::string& pattern : patterns) {
			if (matchPattern(pattern, word, stem)) return true;
		}

		return false;
	}

	static void filterWords(FunctionCall& call, bool keep_matches, std::string& out) {
		const std::vector<std::string> patterns = splitWords(call.args[0]);
		std::vector<std::string> kept;
		for (const std::string& word : splitWords(call.args[1])) {
			if (matchesAny(patterns, word) == keep_matches) kept.push_back(word);
		}

		out += joinWords(kept);
	}

	static void functionFilter(FunctionCall& call, std::string& out) {
		filterWords(call, true, out);
	}

	static void functionFilterOut(FunctionCall& call, std::string& out) {
		filterWords(call, false, out);
	}

	static void functionSort(FunctionCall& call, std::string& out) {
		std::vector<std::string> words = splitWords(call.args[0]);
		std::sort(words.begin(), words.end());
		words.erase(std::unique(words.begin(), words.end()), words.end());
		out += joinWords(words);
	}

	static bool parseCount(FunctionCall& call, std::size_t index, const char* ordinal,
	                       long long& count) {
		const std::string text = trimBlanks(call.args[index]);
		std::size_t consumed = 0;
		if (parseLL(text, count, 10, &consumed) && consumed == text.size()) return true;

		reportFatal(call.make, call.make.where, std::string("non-numeric ") + ordinal
			+ " argument to '" + call.name + "' function: '" + text + "'");
		return false;
	}

	static void functionWord(FunctionCall& call, std::string& out) {
		long long index = 0;
		if (!parseCount(call, 0, "first", index)) return;
		if (index < 1) {
			reportFatal(call.make, call.make.where,
				"first argument to 'word' function must be greater than 0");
			return;
		}

		const std::vector<std::string> words = splitWords(call.args[1]);
		const std::size_t position = static_cast<std::size_t>(index);
		if (position <= words.size()) out += words[position - 1];
	}

	static void functionWordlist(FunctionCall& call, std::string& out) {
		long long first = 0;
		long long last = 0;
		if (!parseCount(call, 0, "first", first) || !parseCount(call, 1, "second", last)) return;
		if (first < 1) {
			reportFatal(call.make, call.make.where, "invalid first argument to 'wordlist' "
				"function: '" + std::to_string(first) + "'");
			return;
		}

		const std::vector<std::string> words = splitWords(call.args[2]);
		const std::size_t end = std::min(words.size(), static_cast<std::size_t>(std::max(last, 0LL)));
		std::vector<std::string> picked;
		for (std::size_t i = static_cast<std::size_t>(first) - 1; i < end; ++i) {
			picked.push_back(words[i]);
		}

		out += joinWords(picked);
	}

	static void functionWords(FunctionCall& call, std::string& out) {
		out += std::to_string(splitWords(call.args[0]).size());
	}

	static void functionFirstword(FunctionCall& call, std::string& out) {
		const std::vector<std::string> words = splitWords(call.args[0]);
		if (!words.empty()) out += words.front();
	}

	static void functionLastword(FunctionCall& call, std::string& out) {
		const std::vector<std::string> words = splitWords(call.args[0]);
		if (!words.empty()) out += words.back();
	}

	static std::size_t suffixDot(const std::string& word) {
		const std::size_t dot = word.rfind('.');
		const std::size_t slash = word.find_last_of("/\\");
		if (dot == std::string::npos) return std::string::npos;
		if (slash != std::string::npos && dot < slash) return std::string::npos;

		return dot;
	}

	using WordMap = std::string (*)(const std::string& word);

	static void mapWords(const std::string& text, WordMap map, std::string& out) {
		std::vector<std::string> mapped;
		for (const std::string& word : splitWords(text)) {
			std::string result = map(word);
			if (!result.empty()) mapped.push_back(std::move(result));
		}

		out += joinWords(mapped);
	}

	static std::string suffixOf(const std::string& word) {
		const std::size_t dot = suffixDot(word);
		return dot == std::string::npos ? std::string() : word.substr(dot);
	}

	static std::string basenameOf(const std::string& word) {
		const std::size_t dot = suffixDot(word);
		return dot == std::string::npos ? word : word.substr(0, dot);
	}

	static void functionDir(FunctionCall& call, std::string& out) {
		mapWords(call.args[0], directoryOf, out);
	}

	static void functionNotdir(FunctionCall& call, std::string& out) {
		std::vector<std::string> mapped;
		for (const std::string& word : splitWords(call.args[0])) mapped.push_back(notDirectoryOf(word));
		out += joinWords(mapped);
	}

	static void functionSuffix(FunctionCall& call, std::string& out) {
		mapWords(call.args[0], suffixOf, out);
	}

	static void functionBasename(FunctionCall& call, std::string& out) {
		mapWords(call.args[0], basenameOf, out);
	}

	static void functionAddsuffix(FunctionCall& call, std::string& out) {
		std::vector<std::string> words = splitWords(call.args[1]);
		for (std::string& word : words) word += call.args[0];
		out += joinWords(words);
	}

	static void functionAddprefix(FunctionCall& call, std::string& out) {
		std::vector<std::string> words = splitWords(call.args[1]);
		for (std::string& word : words) word = call.args[0] + word;
		out += joinWords(words);
	}

	static void functionJoin(FunctionCall& call, std::string& out) {
		const std::vector<std::string> first = splitWords(call.args[0]);
		const std::vector<std::string> second = splitWords(call.args[1]);
		std::vector<std::string> joined;
		for (std::size_t i = 0; i < std::max(first.size(), second.size()); ++i) {
			const std::string left = i < first.size() ? first[i] : std::string();
			const std::string right = i < second.size() ? second[i] : std::string();
			joined.push_back(left + right);
		}

		out += joinWords(joined);
	}

	static const std::size_t kAnyCount = 0;

	static const FunctionSpec kFunctions[] = {
		{ "abspath",    0, 1,         true,  functionAbspath },
		{ "addprefix",  2, 2,         true,  functionAddprefix },
		{ "addsuffix",  2, 2,         true,  functionAddsuffix },
		{ "and",        1, kAnyCount, false, functionAnd },
		{ "basename",   0, 1,         true,  functionBasename },
		{ "call",       1, kAnyCount, true,  functionCall },
		{ "dir",        0, 1,         true,  functionDir },
		{ "error",      0, 1,         true,  functionError },
		{ "eval",       0, 1,         true,  functionEval },
		{ "file",       1, 2,         true,  functionFile },
		{ "filter",     2, 2,         true,  functionFilter },
		{ "filter-out", 2, 2,         true,  functionFilterOut },
		{ "findstring", 2, 2,         true,  functionFindstring },
		{ "firstword",  0, 1,         true,  functionFirstword },
		{ "flavor",     0, 1,         true,  functionFlavor },
		{ "foreach",    3, 3,         false, functionForeach },
		{ "if",         2, 3,         false, functionIf },
		{ "info",       0, 1,         true,  functionInfo },
		{ "intcmp",     2, 5,         false, functionIntcmp },
		{ "join",       2, 2,         true,  functionJoin },
		{ "lastword",   0, 1,         true,  functionLastword },
		{ "let",        3, 3,         false, functionLet },
		{ "notdir",     0, 1,         true,  functionNotdir },
		{ "or",         1, kAnyCount, false, functionOr },
		{ "origin",     0, 1,         true,  functionOrigin },
		{ "patsubst",   3, 3,         true,  functionPatsubst },
		{ "realpath",   0, 1,         true,  functionRealpath },
		{ "shell",      0, 1,         true,  functionShell },
		{ "sort",       0, 1,         true,  functionSort },
		{ "strip",      0, 1,         true,  functionStrip },
		{ "subst",      3, 3,         true,  functionSubst },
		{ "suffix",     0, 1,         true,  functionSuffix },
		{ "value",      0, 1,         true,  functionValue },
		{ "warning",    0, 1,         true,  functionWarning },
		{ "wildcard",   0, 1,         true,  functionWildcard },
		{ "word",       2, 2,         true,  functionWord },
		{ "wordlist",   3, 3,         true,  functionWordlist },
		{ "words",      0, 1,         true,  functionWords },
	};

	static const FunctionSpec* findFunction(const std::string& name) {
		for (const FunctionSpec& spec : kFunctions) {
			if (name == spec.name) return &spec;
		}

		return nullptr;
	}

	bool isFunctionName(const std::string& name) {
		return findFunction(name) != nullptr;
	}

	// Commas nest only inside the bracket kind that opened the call, and past
	// the last argument a function takes they are part of that argument.
	static std::vector<std::string> splitArguments(const std::string& text, char close,
	                                               std::size_t max_args) {
		const char open = close == ')' ? '(' : '{';
		std::vector<std::string> args;
		std::size_t start = 0;
		int depth = 0;
		for (std::size_t i = 0; i < text.size(); ++i) {
			const char c = text[i];
			if (c == open) ++depth;
			if (c == close) --depth;
			if (c != ',' || depth > 0) continue;
			if (max_args != kAnyCount && args.size() + 1 >= max_args) continue;

			args.push_back(text.substr(start, i - start));
			start = i + 1;
		}

		args.push_back(text.substr(start));
		return args;
	}

	bool callFunction(Make& make, const std::string& inner, char close,
	                  const ExpandContext& ctx, std::string& out) {
		const std::size_t name_end = inner.find_first_of(" \t");
		if (name_end == std::string::npos) return false;

		const FunctionSpec* spec = findFunction(inner.substr(0, name_end));
		if (spec == nullptr) return false;

		const std::string name = spec->name;
		const std::string text = inner.substr(skipBlanks(inner, name_end));
		FunctionCall call{ make, ctx, name, splitArguments(text, close, spec->max_args) };
		if (call.args.size() < spec->min_args) {
			reportFatal(make, make.where, "insufficient number of arguments ("
				+ std::to_string(call.args.size()) + ") to function '" + name + "'");
			return true;
		}

		if (spec->expand_args) {
			for (std::string& arg : call.args) arg = expand(make, arg, ctx);
		}

		if (!make.fatal) spec->impl(call, out);
		return true;
	}

}  // namespace wbsh::make_detail

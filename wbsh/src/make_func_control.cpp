/**
 * @file make_func_control.cpp
 * @brief The functions that steer expansion or look at the database: if, or,
 *        and, foreach, let, call, value, eval, origin, flavor, intcmp, and the
 *        error / warning / info reports.
 */

#include "make_internal.h"

#include <algorithm>
#include <string>
#include <vector>

#include "numparse.h"

namespace wbsh::make_detail {

	static const int kMaxCallDepth = 400;

	static std::string expandStripped(FunctionCall& call, std::size_t index) {
		return expand(call.make, trimBlanks(call.args[index]), call.ctx);
	}

	void functionIf(FunctionCall& call, std::string& out) {
		const bool condition = !expandStripped(call, 0).empty();
		if (condition) {
			expandInto(call.make, call.args[1], call.ctx, out);
			return;
		}

		if (call.args.size() > 2) expandInto(call.make, call.args[2], call.ctx, out);
	}

	void functionOr(FunctionCall& call, std::string& out) {
		for (std::size_t i = 0; i < call.args.size() && !call.make.fatal; ++i) {
			const std::string value = expandStripped(call, i);
			if (value.empty()) continue;

			out += value;
			return;
		}
	}

	void functionAnd(FunctionCall& call, std::string& out) {
		std::string value;
		for (std::size_t i = 0; i < call.args.size() && !call.make.fatal; ++i) {
			value = expandStripped(call, i);
			if (value.empty()) return;
		}

		out += value;
	}

	static Variable loopVariable(const std::string& value) {
		Variable variable;
		variable.value = value;
		variable.flavor = Flavor::Simple;
		variable.origin = Origin::Automatic;
		return variable;
	}

	static void expandWithLocals(FunctionCall& call, const VarTable& locals,
	                             const std::string& body, std::string& out) {
		const VarScope level{ &locals, call.ctx.scope, call.ctx.target };
		ExpandContext inner = call.ctx;
		inner.scope = &level;
		expandInto(call.make, body, inner, out);
	}

	void functionForeach(FunctionCall& call, std::string& out) {
		const std::string name = expandStripped(call, 0);
		const std::vector<std::string> words = splitWords(expand(call.make, call.args[1], call.ctx));

		std::vector<std::string> results;
		for (const std::string& word : words) {
			if (call.make.fatal) return;

			VarTable locals;
			locals[name] = loopVariable(word);
			std::string result;
			expandWithLocals(call, locals, call.args[2], result);
			results.push_back(result);
		}

		out += joinWords(results);
	}

	void functionLet(FunctionCall& call, std::string& out) {
		const std::vector<std::string> names = splitWords(expand(call.make, call.args[0], call.ctx));
		std::vector<std::string> words = splitWords(expand(call.make, call.args[1], call.ctx));

		VarTable locals;
		for (std::size_t i = 0; i < names.size(); ++i) {
			const bool last = i + 1 == names.size();
			std::string value;
			if (last && i < words.size()) {
				value = joinWords(std::vector<std::string>(words.begin() + i, words.end()));
			} else if (i < words.size()) {
				value = words[i];
			}

			locals[names[i]] = loopVariable(value);
		}

		expandWithLocals(call, locals, call.args[2], out);
	}

	// $(1) .. $(N) of an outer call must not show through an inner one that
	// passes fewer arguments.
	static VarTable callParameters(FunctionCall& call, const std::string& name) {
		VarTable params;
		params["0"] = loopVariable(name);
		const std::size_t count = call.args.size() - 1;
		const std::size_t covered = std::max(count, static_cast<std::size_t>(call.make.call_args));
		for (std::size_t i = 1; i <= covered; ++i) {
			params[std::to_string(i)] = loopVariable(i <= count ? call.args[i] : std::string());
		}

		return params;
	}

	static std::string joinArguments(const std::vector<std::string>& args, std::size_t from) {
		std::string joined;
		for (std::size_t i = from; i < args.size(); ++i) {
			if (i > from) joined.push_back(',');
			joined += args[i];
		}

		return joined;
	}

	static void callBuiltin(FunctionCall& call, const std::string& name, std::string& out) {
		callFunction(call.make, name + " " + joinArguments(call.args, 1), ')', call.ctx, out);
	}

	static void expandCalledBody(FunctionCall& call, const VarRef& found, const VarTable& params,
	                             std::string& out) {
		if (found.variable->flavor == Flavor::Simple) {
			out += found.variable->value;
			return;
		}

		const std::string body = found.variable->value;
		const int saved_args = call.make.call_args;
		call.make.call_args = static_cast<int>(call.args.size() - 1);
		++call.make.call_depth;
		expandWithLocals(call, params, body, out);
		--call.make.call_depth;
		call.make.call_args = saved_args;
	}

	void functionCall(FunctionCall& call, std::string& out) {
		const std::string name = trimBlanks(call.args[0]);
		if (name.empty()) return;

		if (isFunctionName(name)) {
			callBuiltin(call, name, out);
			return;
		}

		const VarRef found = lookupVariable(call.make, name, call.ctx);
		if (found.variable == nullptr || found.variable->value.empty()) return;

		if (call.make.call_depth >= kMaxCallDepth) {
			reportFatal(call.make, call.make.where,
				"Recursive variable '" + name + "' references itself (eventually)");
			return;
		}

		expandCalledBody(call, found, callParameters(call, name), out);
	}

	void functionValue(FunctionCall& call, std::string& out) {
		out += rawVariableValue(call.make, call.args[0], call.ctx);
	}

	void functionEval(FunctionCall& call, std::string& out) {
		static_cast<void>(out);
		evalText(call.make, call.args[0]);
	}

	void functionOrigin(FunctionCall& call, std::string& out) {
		const std::string& name = call.args[0];
		if (isAutomaticName(name)) {
			out += call.ctx.autos != nullptr ? "automatic" : "undefined";
			return;
		}

		const VarRef found = lookupVariable(call.make, name, call.ctx);
		out += found.variable == nullptr ? "undefined" : originName(found.variable->origin);
	}

	void functionFlavor(FunctionCall& call, std::string& out) {
		const VarRef found = lookupVariable(call.make, call.args[0], call.ctx);
		if (found.variable == nullptr) {
			out += "undefined";
			return;
		}

		out += found.variable->flavor == Flavor::Simple ? "simple" : "recursive";
	}

	void functionError(FunctionCall& call, std::string& out) {
		static_cast<void>(out);
		reportFatal(call.make, call.make.where, call.args[0]);
	}

	void functionWarning(FunctionCall& call, std::string& out) {
		static_cast<void>(out);
		reportWarning(call.make, call.make.where, call.args[0]);
	}

	void functionInfo(FunctionCall& call, std::string& out) {
		static_cast<void>(out);
		printLine(call.args[0]);
	}

	static bool parseIntcmpOperand(FunctionCall& call, std::size_t index, const char* ordinal,
	                               long long& value) {
		const std::string text = expandStripped(call, index);
		std::size_t consumed = 0;
		if (parseLL(text, value, 10, &consumed) && consumed == text.size() && !text.empty()) {
			return true;
		}

		reportFatal(call.make, call.make.where, std::string("non-numeric ") + ordinal
			+ " argument to 'intcmp' function: '" + text + "'");
		return false;
	}

	void functionIntcmp(FunctionCall& call, std::string& out) {
		long long left = 0;
		long long right = 0;
		if (!parseIntcmpOperand(call, 0, "first", left)) return;
		if (!parseIntcmpOperand(call, 1, "second", right)) return;

		if (call.args.size() == 2) {
			if (left == right) out += std::to_string(left);
			return;
		}

		std::size_t branch = 2;
		if (left == right) branch = 3;
		if (left > right) branch = call.args.size() > 4 ? 4 : 3;
		if (branch < call.args.size()) expandInto(call.make, call.args[branch], call.ctx, out);
	}

}  // namespace wbsh::make_detail

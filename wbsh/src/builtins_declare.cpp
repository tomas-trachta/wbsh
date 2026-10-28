/**
 * @file builtins_declare.cpp
 * @brief The variable declaration builtins (`declare` / `typeset`, `local`,
 *        `export`, `readonly`) and `unset`.
 */

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "executor.h"

namespace wbsh::declare_detail {

	struct Command {
		const char* name;
		const char* letters;
		const char* usage;
	};

	static const Command kDeclare  = { "declare", "aAfFgilnprtuxI",
		"declare [-aAfFgilnrtux] [-p] [name[=value] ...]" };
	static const Command kTypeset  = { "typeset", "aAfFgilnprtuxI",
		"typeset [-aAfFgilnrtux] [-p] name[=value] ..." };
	static const Command kLocal    = { "local", "aAfFgilnprtuxI",
		"local [option] name[=value] ..." };
	static const Command kExport   = { "export", "fnp",
		"export [-fn] [name[=value] ...] or export -p" };
	static const Command kReadonly = { "readonly", "aAfp",
		"readonly [-aAf] [name[=value] ...] or readonly -p" };
	static const Command kUnset    = { "unset", "fvn",
		"unset [-f] [-v] [-n] [name ...]" };

	struct Options {
		unsigned add_attrs    = 0;
		unsigned remove_attrs = 0;
		bool indexed        = false;
		bool assoc          = false;
		bool functions      = false;
		bool function_names = false;
		bool global         = false;
		bool print          = false;
		bool export_on      = false;
		bool export_off     = false;
		bool readonly_on    = false;
		bool readonly_off   = false;
		bool n_flag         = false;
		bool nameref_on     = false;
		bool nameref_off    = false;
		bool variables_only = false;
	};

	struct Operand {
		std::string text;
		std::string name;
		std::string subscript;
		std::string value;
		bool has_subscript = false;
		bool has_value     = false;
		bool append        = false;
		bool compound      = false;
	};

	static void applyAttributeLetter(char letter, bool on, Options& opts) {
		const unsigned bit = letter == 'i' ? kAttrInteger
			: letter == 'l' ? kAttrLowercase
			: kAttrUppercase;
		if (on) opts.add_attrs |= bit;
		else    opts.remove_attrs |= bit;
	}

	static void applyOptionLetter(char letter, bool on, Options& opts) {
		switch (letter) {
			case 'a': opts.indexed = on; break;
			case 'A': opts.assoc = on; break;
			case 'f': opts.functions = true; break;
			case 'F': opts.function_names = true; break;
			case 'g': opts.global = true; break;
			case 'p': opts.print = true; break;
			case 'n':
				opts.n_flag = true;
				if (on) opts.nameref_on = true;
				else    opts.nameref_off = true;
				break;
			case 'v': opts.variables_only = true; break;
			case 'r':
				if (on) opts.readonly_on = true;
				else    opts.readonly_off = true;
				break;
			case 'x':
				if (on) opts.export_on = true;
				else    opts.export_off = true;
				break;
			case 'i':
			case 'l':
			case 'u':
				applyAttributeLetter(letter, on, opts);
				break;
			default:
				break;
		}
	}

	static int reportInvalidOption(const Command& cmd, char sign, char letter) {
		std::fprintf(stderr, "wbsh: %s: %c%c: invalid option\n", cmd.name, sign, letter);
		std::fprintf(stderr, "%s: usage: %s\n", cmd.name, cmd.usage);
		return 2;
	}

	static bool isOptionWord(const std::string& arg) {
		return arg.size() >= 2 && (arg[0] == '-' || arg[0] == '+');
	}

	static int parseOptions(const Command& cmd, const std::vector<std::string>& args,
			Options& opts, std::size_t& first) {
		first = 0;
		while (first < args.size() && isOptionWord(args[first])) {
			const std::string& arg = args[first++];
			if (arg == "--") break;

			for (std::size_t k = 1; k < arg.size(); ++k) {
				if (std::strchr(cmd.letters, arg[k]) == nullptr) {
					return reportInvalidOption(cmd, arg[0], arg[k]);
				}

				applyOptionLetter(arg[k], arg[0] == '-', opts);
			}
		}

		return 0;
	}

	static bool isNameStart(char c) {
		return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
	}

	static bool isNameChar(char c) {
		return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
	}

	static std::size_t nameLength(const std::string& text) {
		if (text.empty() || !isNameStart(text[0])) return 0;

		std::size_t length = 1;
		while (length < text.size() && isNameChar(text[length])) ++length;
		return length;
	}

	static bool parseSubscript(const std::string& text, std::size_t& pos, Operand& out) {
		if (pos >= text.size() || text[pos] != '[') return true;

		const std::size_t close = text.find(']', pos);
		if (close == std::string::npos) return false;

		out.has_subscript = true;
		out.subscript = text.substr(pos + 1, close - pos - 1);
		pos = close + 1;
		return true;
	}

	static bool parseOperand(const std::string& text, Operand& out) {
		out.text = text;
		const std::size_t length = nameLength(text);
		if (length == 0) return false;

		out.name = text.substr(0, length);
		std::size_t pos = length;
		if (!parseSubscript(text, pos, out)) return false;
		if (pos == text.size()) return true;

		if (text.compare(pos, 2, "+=") == 0) {
			out.append = true;
			pos += 2;
		} else if (text[pos] == '=') {
			pos += 1;
		} else {
			return false;
		}

		out.has_value = true;
		out.value = text.substr(pos);
		return true;
	}

	static int reportInvalidIdentifier(const Command& cmd, const std::string& text) {
		std::fprintf(stderr, "wbsh: %s: `%s': not a valid identifier\n", cmd.name, text.c_str());
		return 1;
	}

	// Like bash, only the builtins that create variables name themselves.
	static int reportReadonly(const Command& cmd, const std::string& name) {
		const bool named = &cmd != &kExport && &cmd != &kReadonly;
		std::fprintf(stderr, "wbsh: %s%s%s: readonly variable\n", named ? cmd.name : "",
			named ? ": " : "", name.c_str());
		return 1;
	}

	static bool isKnownVariable(const Environment& env, const std::string& name) {
		return env.has(name) || env.isExported(name) || env.isReadonly(name)
			|| env.attributes(name) != 0 || env.isNameref(name);
	}

	static std::string doubleQuoted(const std::string& text) {
		std::string quoted = "\"";
		for (const char c : text) {
			if (c == '"' || c == '\\' || c == '$' || c == '`') quoted.push_back('\\');
			quoted.push_back(c);
		}

		quoted.push_back('"');
		return quoted;
	}

	static std::string assocKeyText(const std::string& key) {
		if (!key.empty() && shellQuoteForReuse(key) == key) return key;
		return doubleQuoted(key);
	}

	static std::string indexedArrayText(const Environment::IndexedArray& array) {
		std::string text = "(";
		for (const auto& element : array) {
			if (text.size() > 1) text.push_back(' ');
			text += "[" + std::to_string(element.first) + "]=" + doubleQuoted(element.second);
		}

		return text + ")";
	}

	static std::string assocArrayText(const Environment::AssocArray& array) {
		std::string text = "(";
		for (const auto& element : array) {
			text += "[" + assocKeyText(element.first) + "]=" + doubleQuoted(element.second) + " ";
		}

		return text + ")";
	}

	static bool arrayText(const Environment& env, const std::string& name, std::string& out) {
		if (const auto* indexed = env.getIndexedArray(name)) {
			out = indexedArrayText(*indexed);
			return true;
		}

		if (const auto* assoc = env.getAssocArray(name)) {
			out = assocArrayText(*assoc);
			return true;
		}

		return false;
	}

	static std::string namerefLine(const Environment& env, const std::string& name) {
		const std::string target = env.namerefTarget(name);
		const std::string line = "declare -n " + name;
		return target.empty() ? line : line + "=" + doubleQuoted(target);
	}

	static std::string declareLine(const Environment& env, const std::string& name) {
		if (env.isNameref(name)) return namerefLine(env, name);

		const std::string letters = env.attributeLetters(name);
		std::string line = "declare -" + (letters.empty() ? std::string("-") : letters) + " " + name;
		if (!env.has(name)) return line;

		std::string array;
		if (arrayText(env, name, array)) return line + "=" + array;
		return line + "=" + doubleQuoted(env.get(name));
	}

	static std::string listingLine(const Environment& env, const std::string& name) {
		if (env.isNameref(name)) return name + "=" + shellQuoteForReuse(env.namerefTarget(name));

		std::string array;
		if (arrayText(env, name, array)) return name + "=" + array;
		return name + "=" + shellQuoteForReuse(env.get(name));
	}

	static void printLine(const std::string& line) {
		std::fwrite(line.data(), 1, line.size(), stdout);
		std::fputc('\n', stdout);
	}

	template <typename Container>
	static void insertKeys(std::set<std::string>& names, const Container& container) {
		for (const auto& entry : container) names.insert(entry.first);
	}

	static std::vector<std::string> knownVariableNames(const Environment& env) {
		std::set<std::string> names;
		insertKeys(names, env.vars());
		insertKeys(names, env.indexedArrays());
		insertKeys(names, env.assocArrays());
		insertKeys(names, env.namerefs());
		names.insert(env.exportedSet().begin(), env.exportedSet().end());
		names.insert(env.readonlySet().begin(), env.readonlySet().end());
		return { names.begin(), names.end() };
	}

	static bool hasFilter(const Options& opts) {
		return opts.add_attrs != 0 || opts.indexed || opts.assoc || opts.export_on
			|| opts.readonly_on || opts.nameref_on;
	}

	static bool matchesFilter(const Environment& env, const Options& opts, const std::string& name) {
		if (opts.nameref_on) return env.isNameref(name);
		if (opts.indexed && !env.isIndexedArray(name)) return false;
		if (opts.assoc && !env.isAssocArray(name)) return false;
		if (opts.export_on && !env.isExported(name)) return false;
		if (opts.readonly_on && !env.isReadonly(name)) return false;
		return (env.attributes(name) & opts.add_attrs) == opts.add_attrs;
	}

	static void printFunctionDefinition(const FunctionDef& def) {
		std::printf("%s () \n%s\n", def.name.c_str(), def.body_text.c_str());
	}

	static std::vector<std::string> sortedFunctionNames(Executor& exec) {
		std::vector<std::string> names = exec.functionNames();
		std::sort(names.begin(), names.end());
		return names;
	}

	static void printAllFunctions(Executor& exec, bool names_only) {
		for (const std::string& name : sortedFunctionNames(exec)) {
			if (names_only) std::printf("declare -f %s\n", name.c_str());
			else            printFunctionDefinition(*exec.findFunction(name));
		}
	}

	static std::vector<std::string> sortedLocalNames(Executor& exec) {
		std::vector<std::string> names = exec.localNames();
		std::sort(names.begin(), names.end());
		return names;
	}

	static int listDeclarations(Executor& exec, const Options& opts, bool locals_only) {
		const Environment& env = exec.env();
		const bool plain = !opts.print && !hasFilter(opts);
		const std::vector<std::string> names = locals_only
			? sortedLocalNames(exec)
			: knownVariableNames(env);

		for (const std::string& name : names) {
			if (!matchesFilter(env, opts, name)) continue;
			if (plain && !env.has(name) && !env.isNameref(name)) continue;

			printLine(plain ? listingLine(env, name) : declareLine(env, name));
		}

		if (plain && !locals_only) printAllFunctions(exec, false);
		std::fflush(stdout);
		return 0;
	}

	static int printNamedDeclarations(Executor& exec, const Command& cmd,
			const std::vector<std::string>& args, std::size_t first) {
		int status = 0;
		for (std::size_t i = first; i < args.size(); ++i) {
			const std::string name = args[i].substr(0, args[i].find('='));
			if (!isKnownVariable(exec.env(), name)) {
				std::fprintf(stderr, "wbsh: %s: %s: not found\n", cmd.name, name.c_str());
				status = 1;
				continue;
			}

			printLine(declareLine(exec.env(), name));
		}

		std::fflush(stdout);
		return status;
	}

	static int declareFunctions(Executor& exec, const Options& opts,
			const std::vector<std::string>& args, std::size_t first) {
		if (first == args.size()) {
			printAllFunctions(exec, opts.function_names);
			return 0;
		}

		int status = 0;
		for (std::size_t i = first; i < args.size(); ++i) {
			const FunctionDef* def = exec.findFunction(args[i]);
			if (def == nullptr)           status = 1;
			else if (opts.print || opts.function_names) std::printf("%s\n", def->name.c_str());
			else                          printFunctionDefinition(*def);
		}

		std::fflush(stdout);
		return status;
	}

	static int requireFunctions(Executor& exec, const Command& cmd,
			const std::vector<std::string>& args, std::size_t first) {
		int status = 0;
		for (std::size_t i = first; i < args.size(); ++i) {
			if (exec.findFunction(args[i]) != nullptr) continue;

			std::fprintf(stderr, "wbsh: %s: %s: not a function\n", cmd.name, args[i].c_str());
			status = 1;
		}

		return status;
	}

	static bool isLocalHere(Executor& exec, const std::string& name) {
		const std::vector<std::string> names = exec.localNames();
		return std::find(names.begin(), names.end(), name) != names.end();
	}

	static bool rejectsReadonly(Executor& exec, const Options& opts, const Operand& op,
			bool make_local) {
		if (!exec.env().isReadonly(op.name)) return false;
		if (op.has_value || opts.readonly_off) return true;
		return make_local && !isLocalHere(exec, op.name);
	}

	static bool reportConversion(const std::string& name, const char* from, const char* to) {
		std::fprintf(stderr, "wbsh: %s: cannot convert %s to %s array\n", name.c_str(), from, to);
		return false;
	}

	static bool makeAssocArray(Environment& env, const std::string& name) {
		if (env.isAssocArray(name)) return true;
		if (env.isIndexedArray(name)) return reportConversion(name, "indexed", "associative");

		const bool had_value = env.has(name);
		const std::string value = env.get(name);
		env.declareAssocArray(name);
		if (had_value) env.setAssocElement(name, "0", value);
		return true;
	}

	static bool makeIndexedArray(Environment& env, const std::string& name) {
		if (env.isIndexedArray(name)) return true;
		if (env.isAssocArray(name)) return reportConversion(name, "associative", "indexed");

		std::vector<std::string> values;
		if (env.has(name)) values.push_back(env.get(name));
		env.setIndexedArrayFromList(name, std::move(values));
		return true;
	}

	static bool applyArrayKind(Environment& env, const Options& opts, const std::string& name) {
		if (opts.assoc) return makeAssocArray(env, name);
		if (opts.indexed) return makeIndexedArray(env, name);
		return true;
	}

	static void applyValueAttributes(Environment& env, const Options& opts,
			const std::string& name) {
		unsigned attrs = env.attributes(name) | opts.add_attrs;
		if ((opts.add_attrs & kAttrLowercase) != 0) attrs &= ~kAttrUppercase;
		if ((opts.add_attrs & kAttrUppercase) != 0) attrs &= ~kAttrLowercase;
		env.setAttributes(name, attrs & ~opts.remove_attrs);
	}

	static std::string currentElement(const Environment& env, const std::string& name,
			long long index, const std::string& key) {
		if (const auto* assoc = env.getAssocArray(name)) {
			const auto found = assoc->find(key);
			return found == assoc->end() ? std::string() : found->second;
		}

		if (const auto* indexed = env.getIndexedArray(name)) {
			const auto found = indexed->find(index);
			return found == indexed->end() ? std::string() : found->second;
		}

		return index == 0 ? env.get(name) : std::string();
	}

	static void assignElement(Executor& exec, const Operand& op) {
		Environment& env = exec.env();
		long long index = 0;
		const bool assoc = env.isAssocArray(op.name);
		if (!assoc && !exec.expander().tryEvalArith(op.subscript, index)) index = 0;

		std::string value = op.value;
		if (op.append) value = currentElement(env, op.name, index, op.subscript) + value;

		if (assoc) env.setAssocElement(op.name, op.subscript, std::move(value));
		else       env.setIndexedElement(op.name, index, std::move(value));
	}

	static bool looksCompound(const std::string& value) {
		return value.size() >= 2 && value.front() == '(' && value.back() == ')';
	}

	static bool isArray(const Environment& env, const std::string& name) {
		return env.isIndexedArray(name) || env.isAssocArray(name);
	}

	static bool assignOperand(Executor& exec, const Operand& op) {
		Environment& env = exec.env();
		if (op.has_subscript) {
			assignElement(exec, op);
			return true;
		}

		if (op.compound || (isArray(env, op.name) && looksCompound(op.value))) {
			return exec.assignCompound(op.text);
		}

		env.set(op.name, op.append ? env.appendedValue(op.name, op.value) : op.value);
		return true;
	}

	static void applyVisibility(Environment& env, const Options& opts, const std::string& name) {
		if (opts.export_on)   env.exportVar(name);
		if (opts.export_off)  env.unexportVar(name);
		if (opts.readonly_on) env.markReadonly(name);
	}

	static int declareOperand(Executor& exec, const Command& cmd, const Options& opts,
			const Operand& op, bool make_local) {
		Environment& env = exec.env();
		if (rejectsReadonly(exec, opts, op, make_local)) return reportReadonly(cmd, op.name);
		if (make_local) exec.makeLocal(op.name);
		if (!applyArrayKind(env, opts, op.name)) return 1;

		applyValueAttributes(env, opts, op.name);
		if (op.has_value && !assignOperand(exec, op)) return 1;

		applyVisibility(env, opts, op.name);
		return 0;
	}

	static bool isCompoundArg(const std::vector<bool>& compound, std::size_t arg_index) {
		return arg_index + 1 < compound.size() && compound[arg_index + 1];
	}

	static int declareOperands(Executor& exec, const Command& cmd, const Options& opts,
			const std::vector<std::string>& args, std::size_t first, bool make_local) {
		const std::vector<bool> compound = exec.takeCompoundArgs();
		int status = 0;
		for (std::size_t i = first; i < args.size(); ++i) {
			Operand op;
			if (!parseOperand(args[i], op)) {
				status = reportInvalidIdentifier(cmd, args[i]);
				continue;
			}

			op.compound = isCompoundArg(compound, i);
			if (declareOperand(exec, cmd, opts, op, make_local) != 0) status = 1;
		}

		return status;
	}

	static bool isValidRefTarget(const std::string& target) {
		Operand probe;
		return target.empty() || (parseOperand(target, probe) && !probe.has_value);
	}

	// Without a value, a variable that already holds one keeps it as its target.
	static std::string namerefTargetFor(const Environment& env, const Operand& op) {
		if (op.has_value) return op.value;
		if (env.isNameref(op.name)) return env.namerefTarget(op.name);

		const auto found = env.vars().find(op.name);
		return found == env.vars().end() ? std::string() : found->second;
	}

	static int reportBadRefTarget(const Command& cmd, const std::string& target) {
		std::fprintf(stderr, "wbsh: %s: `%s': invalid variable name for name reference\n",
			cmd.name, target.c_str());
		return 1;
	}

	static int reportSelfReference(const Command& cmd, const std::string& name) {
		std::fprintf(stderr, "wbsh: %s: %s: nameref variable self references not allowed\n",
			cmd.name, name.c_str());
		return 1;
	}

	static int declareNamerefOperand(Executor& exec, const Command& cmd, const Operand& op,
			bool make_local) {
		if (op.has_subscript) return reportBadRefTarget(cmd, op.text);
		if (make_local) exec.makeLocal(op.name);

		Environment& env = exec.env();
		const std::string target = namerefTargetFor(env, op);
		if (!isValidRefTarget(target)) return reportBadRefTarget(cmd, target);
		if (target == op.name) return reportSelfReference(cmd, op.name);

		env.setNameref(op.name, target);
		return 0;
	}

	static int removeNamerefOperand(Executor& exec, const Operand& op) {
		exec.env().clearNameref(op.name);
		if (op.has_value) exec.env().set(op.name, op.value);
		return 0;
	}

	static int declareNamerefs(Executor& exec, const Command& cmd, const Options& opts,
			const std::vector<std::string>& args, std::size_t first, bool make_local) {
		int status = 0;
		for (std::size_t i = first; i < args.size(); ++i) {
			Operand op;
			if (!parseOperand(args[i], op)) {
				status = reportInvalidIdentifier(cmd, args[i]);
				continue;
			}

			const int result = opts.nameref_off ? removeNamerefOperand(exec, op)
				: declareNamerefOperand(exec, cmd, op, make_local);
			if (result != 0) status = result;
		}

		return status;
	}

	static int runDeclare(Executor& exec, const Command& cmd, const std::vector<std::string>& args,
			bool locals_only) {
		Options opts;
		std::size_t first = 0;
		const int status = parseOptions(cmd, args, opts, first);
		if (status != 0) return status;

		if (opts.functions || opts.function_names) return declareFunctions(exec, opts, args, first);
		if (first == args.size()) return listDeclarations(exec, opts, locals_only);
		if (opts.print) return printNamedDeclarations(exec, cmd, args, first);

		const bool make_local = locals_only || (exec.funcDepth() > 0 && !opts.global);
		if (opts.nameref_on || opts.nameref_off) {
			return declareNamerefs(exec, cmd, opts, args, first, make_local);
		}

		return declareOperands(exec, cmd, opts, args, first, make_local);
	}

	static int listWithFilter(Executor& exec, bool exported, bool readonly) {
		Options filter;
		filter.print = true;
		filter.export_on = exported;
		filter.readonly_on = readonly;
		return listDeclarations(exec, filter, false);
	}

	static bool unsetArrayElement(Executor& exec, const std::string& arg) {
		const std::size_t bracket = arg.find('[');
		if (bracket == std::string::npos || arg.back() != ']') return false;

		const std::string name = arg.substr(0, bracket);
		const std::string subscript = arg.substr(bracket + 1, arg.size() - bracket - 2);
		if (!isArray(exec.env(), name)) return false;

		long long index = 0;
		if (!exec.env().isAssocArray(name)) exec.expander().tryEvalArith(subscript, index);
		exec.env().unsetElement(name, index, subscript);
		return true;
	}

	static int unsetOperand(Executor& exec, const Options& opts, const std::string& text) {
		if (opts.functions) {
			exec.unsetFunction(text);
			return 0;
		}

		if (!text.empty() && unsetArrayElement(exec, text)) return 0;
		if (nameLength(text) != text.size()) return reportInvalidIdentifier(kUnset, text);

		Environment& env = exec.env();
		if (env.isReadonly(text)) {
			std::fprintf(stderr, "wbsh: unset: %s: cannot unset: readonly variable\n", text.c_str());
			return 1;
		}

		if (opts.n_flag) {
			env.unsetNameref(text);
			return 0;
		}

		if (!opts.variables_only && !isKnownVariable(env, text)) {
			exec.unsetFunction(text);
			return 0;
		}

		env.unset(text);
		return 0;
	}

	static int builtin_declare(Executor& exec, const std::vector<std::string>& args) {
		return runDeclare(exec, kDeclare, args, false);
	}

	static int builtin_typeset(Executor& exec, const std::vector<std::string>& args) {
		return runDeclare(exec, kTypeset, args, false);
	}

	static int builtin_local(Executor& exec, const std::vector<std::string>& args) {
		if (exec.funcDepth() == 0) {
			std::fprintf(stderr, "wbsh: local: can only be used in a function\n");
			return 1;
		}

		return runDeclare(exec, kLocal, args, true);
	}

	static int builtin_export(Executor& exec, const std::vector<std::string>& args) {
		Options opts;
		std::size_t first = 0;
		const int status = parseOptions(kExport, args, opts, first);
		if (status != 0) return status;

		if (opts.functions) return requireFunctions(exec, kExport, args, first);
		if (first == args.size()) return listWithFilter(exec, true, false);

		opts.export_on = !opts.n_flag;
		opts.export_off = opts.n_flag;
		return declareOperands(exec, kExport, opts, args, first, false);
	}

	static int builtin_readonly(Executor& exec, const std::vector<std::string>& args) {
		Options opts;
		std::size_t first = 0;
		const int status = parseOptions(kReadonly, args, opts, first);
		if (status != 0) return status;

		if (opts.functions) return requireFunctions(exec, kReadonly, args, first);
		if (first == args.size()) return listWithFilter(exec, false, true);

		opts.readonly_on = true;
		return declareOperands(exec, kReadonly, opts, args, first, false);
	}

	static int builtin_unset(Executor& exec, const std::vector<std::string>& args) {
		Options opts;
		std::size_t first = 0;
		int status = parseOptions(kUnset, args, opts, first);
		if (status != 0) return status;

		for (std::size_t i = first; i < args.size(); ++i) {
			if (unsetOperand(exec, opts, args[i]) != 0) status = 1;
		}

		return status;
	}

}  // namespace wbsh::declare_detail

namespace wbsh {

	std::string declarationText(const Environment& env, const std::string& name) {
		return declare_detail::declareLine(env, name);
	}

	void registerDeclarationBuiltins(Executor& exec) {
		exec.registerBuiltin("declare",  declare_detail::builtin_declare);
		exec.registerBuiltin("typeset",  declare_detail::builtin_typeset);
		exec.registerBuiltin("local",    declare_detail::builtin_local);
		exec.registerBuiltin("export",   declare_detail::builtin_export);
		exec.registerBuiltin("readonly", declare_detail::builtin_readonly);
		exec.registerBuiltin("unset",    declare_detail::builtin_unset);
	}

}  // namespace wbsh

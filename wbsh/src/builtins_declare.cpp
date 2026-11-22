/**
 * @file builtins_declare.cpp
 * @brief The variable declaration builtins (`declare` / `typeset`, `local`,
 *        `export`, `readonly`) and `unset`.
 */

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "diagnostics.h"
#include "executor.h"
#include "funcprint.h"
#include "lexer.h"
#include "numparse.h"

namespace wbsh::declare_detail {

	struct Command {
		const char* name;
		const char* letters;
		const char* usage;
	};

	static const Command kDeclare  = { "declare", "aAcfFgilnprtuxI",
		"declare [-aAfFgiIlnrtux] [name[=value] ...] or declare -p [-aAfFilnrtux] [name ...]" };
	static const Command kTypeset  = { "typeset", "aAcfFgilnprtuxI",
		"typeset [-aAfFgiIlnrtux] name[=value] ... or typeset -p [-aAfFilnrtux] [name ...]" };
	static const Command kLocal    = { "local", "aAcfFgilnprtuxI",
		"local [option] name[=value] ..." };
	static const Command kExport   = { "export", "aAfnp",
		"export [-fn] [name[=value] ...] or export -p [-f]" };
	static const Command kReadonly = { "readonly", "aAfnp",
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
		bool trace_on       = false;
		bool trace_off      = false;
		bool variables_only = false;
		bool array_off      = false;    ///< `+a` or `+A`
		bool inherit        = false;    ///< `-I`: a new local starts as what it hides
		/// `readonly` or `export` listing in POSIX mode, which prints that
		/// command instead of declare.
		const char* posix_listing = nullptr;
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
			: letter == 'c' ? kAttrCapitalize
			: kAttrUppercase;
		if (on) opts.add_attrs |= bit;
		else    opts.remove_attrs |= bit;

		// As in bash, turning one case attribute on turns the other two
		// off, so `declare -lu` ends up with neither.
		const unsigned case_attrs = kAttrLowercase | kAttrUppercase | kAttrCapitalize;
		if (on && (bit & case_attrs) != 0) opts.remove_attrs |= case_attrs & ~bit;
	}

	static void applyOptionLetter(char letter, bool on, Options& opts) {
		switch (letter) {
			case 'a': opts.indexed = on; opts.array_off = opts.array_off || !on; break;
			case 'A': opts.assoc = on; opts.array_off = opts.array_off || !on; break;
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
			case 'I': opts.inherit = true; break;
			case 'r':
				if (on) opts.readonly_on = true;
				else    opts.readonly_off = true;
				break;
			case 't':
				if (on) opts.trace_on = true;
				else    opts.trace_off = true;
				break;
			case 'x':
				if (on) opts.export_on = true;
				else    opts.export_off = true;
				break;
			case 'c':
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
		shellErrorf("%s: %c%c: invalid option\n", cmd.name, sign, letter);
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

	// The `]` that balances the `[`, as bash's skipsubscript finds it:
	// `foo[foo[bar]]` has the subscript `foo[bar]`.
	static std::size_t closingBracket(const std::string& text, std::size_t open) {
		int depth = 0;
		for (std::size_t i = open; i < text.size(); ++i) {
			if (text[i] == '[') ++depth;
			else if (text[i] == ']' && --depth == 0) return i;
		}

		return std::string::npos;
	}

	// Under assoc_expand_once a key with an unbalanced `[`
	// (`a["foo[bar"]=x`) ends at the first `]` an assignment operator
	// follows.
	static std::size_t keyClosingBracket(const std::string& text, std::size_t open,
			bool loose_keys) {
		const std::size_t balanced = closingBracket(text, open);
		if (balanced != std::string::npos || !loose_keys) return balanced;

		const std::size_t assigned = text.find("]=", open);
		const std::size_t appended = text.find("]+=", open);
		return (std::min)(assigned, appended);
	}

	static bool parseSubscript(const std::string& text, std::size_t& pos, Operand& out,
			bool loose_keys) {
		if (pos >= text.size() || text[pos] != '[') return true;

		const std::size_t close = keyClosingBracket(text, pos, loose_keys);
		if (close == std::string::npos) return false;

		out.has_subscript = true;
		out.subscript = text.substr(pos + 1, close - pos - 1);
		pos = close + 1;
		return true;
	}

	static bool parseOperand(const std::string& text, Operand& out, bool loose_keys = false) {
		out.text = text;
		const std::size_t length = nameLength(text);
		if (length == 0) return false;

		out.name = text.substr(0, length);
		std::size_t pos = length;
		if (!parseSubscript(text, pos, out, loose_keys)) return false;
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
		shellErrorf("%s: `%s': not a valid identifier\n", cmd.name, text.c_str());
		return 1;
	}

	static bool isDeclareFamily(const Command& cmd) {
		return &cmd != &kExport && &cmd != &kReadonly;
	}

	static bool makesArray(const Options& opts) {
		return opts.indexed || opts.assoc;
	}

	static void printReadonlyError(const std::string& prefix, const std::string& name) {
		shellErrorf("%s%s%s: readonly variable\n", prefix.c_str(), prefix.empty() ? "" : ": ",
			name.c_str());
	}

	// Like bash, export and readonly assign as a plain assignment does, which
	// names no builtin, unless -a or -A hands the operand to declare.
	static int reportReadonly(const Command& cmd, const Options& opts, const std::string& name) {
		const bool named = isDeclareFamily(cmd) || makesArray(opts);
		printReadonlyError(named ? cmd.name : "", name);
		return 1;
	}

	static bool isKnownVariable(const Environment& env, const std::string& name) {
		return env.has(name) || env.isExported(name) || env.isReadonly(name)
			|| env.attributes(name) != 0 || env.isNameref(name) || env.isValueless(name);
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

	// bash switches to $'...' as soon as a value holds a control character
	// or a byte that is not valid UTF-8.
	static std::string valueQuoted(const std::string& text) {
		return needsAnsiCQuote(text) ? shellQuoteAlways(text) : doubleQuoted(text);
	}

	static bool isWholeArrayKey(const std::string& key) {
		return key == "@" || key == "*";
	}

	static std::string assocKeyText(const std::string& key) {
		if (needsAnsiCQuote(key)) return shellQuoteAlways(key);
		if (!key.empty() && !isWholeArrayKey(key) && shellQuoteForReuse(key) == key) return key;
		return doubleQuoted(key);
	}

	static std::string indexedArrayText(const Environment::IndexedArray& array) {
		std::string text = "(";
		for (const auto& element : array) {
			if (text.size() > 1) text.push_back(' ');
			text += "[" + std::to_string(element.first) + "]=" + valueQuoted(element.second);
		}

		return text + ")";
	}

	static std::string assocArrayText(const Environment::AssocArray& array) {
		std::string text = "(";
		for (const auto& element : array) {
			text += "[" + assocKeyText(element.first) + "]=" + valueQuoted(element.second) + " ";
		}

		return text + ")";
	}

	static bool isValuelessArray(const Environment& env, const std::string& name) {
		if (!env.isValueless(name)) return false;

		const auto* indexed = env.getIndexedArray(name);
		const auto* assoc = env.getAssocArray(name);
		return (indexed != nullptr && indexed->empty()) || (assoc != nullptr && assoc->empty());
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

	// The option lists are made up when read; listings show them too.
	static bool optionListValue(const Environment& env, const std::string& name,
			std::string& out) {
		if (name == "SHELLOPTS") out = env.enabledSetOptionNames();
		else if (name == "BASHOPTS") out = env.enabledShoptNames();
		else return false;
		return true;
	}

	static std::string declareLine(const Environment& env, const std::string& name) {
		if (env.isNameref(name)) return namerefLine(env, name);

		std::string option_list;
		if (optionListValue(env, name, option_list)) {
			return "declare -r " + name + "=" + valueQuoted(option_list);
		}

		const std::string letters = env.attributeLetters(name);
		std::string line = "declare -" + (letters.empty() ? std::string("-") : letters) + " " + name;
		if (!env.has(name) || isValuelessArray(env, name)) return line;

		std::string array;
		if (arrayText(env, name, array)) return line + "=" + array;
		return line + "=" + valueQuoted(env.get(name));
	}

	static std::string listingLine(const Environment& env, const std::string& name) {
		if (env.isNameref(name)) return name + "=" + shellQuoteForReuse(env.namerefTarget(name));

		std::string array;
		if (arrayText(env, name, array)) return name + "=" + array;
		return name + "=" + shellQuoteForReuse(env.get(name));
	}

	// POSIX mode: `readonly -a a=(...)`, `export X="1"`; only the array
	// kind of the attributes is shown.
	static std::string posixListingLine(const Environment& env, const char* command,
			const std::string& name) {
		std::string line = command;
		if (env.isIndexedArray(name)) line += " -a";
		else if (env.isAssocArray(name)) line += " -A";
		line += " " + name;

		std::string option_list;
		if (optionListValue(env, name, option_list)) return line + "=" + valueQuoted(option_list);
		if (!env.has(name) || isValuelessArray(env, name)) return line;

		std::string array;
		if (arrayText(env, name, array)) return line + "=" + array;
		return line + "=" + valueQuoted(env.get(name));
	}

	// bash fills DIRSTACK only when something reads it by name, so a
	// listing before pushd shows it empty.
	static bool listsEmptyDirStack(const Environment& env, const std::string& name) {
		const Environment::IndexedArray* stack = env.getIndexedArray(name);
		return name == "DIRSTACK" && stack != nullptr && stack->size() <= 1;
	}

	static std::string listedLine(const Environment& env, const Options& opts, bool plain,
			const std::string& name) {
		if (listsEmptyDirStack(env, name)) return plain ? "DIRSTACK=()" : "declare -a DIRSTACK=()";
		if (plain) return listingLine(env, name);
		if (opts.posix_listing != nullptr) return posixListingLine(env, opts.posix_listing, name);

		return declareLine(env, name);
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
		std::set<std::string> names = { "BASHOPTS", "SHELLOPTS" };
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
		std::printf("%s\n", functionDefinitionText(def).c_str());
	}

	static std::vector<std::string> sortedFunctionNames(Executor& exec) {
		std::vector<std::string> names = exec.functionNames();
		std::sort(names.begin(), names.end());
		return names;
	}

	static std::string functionAttributes(const Executor& exec, const std::string& name) {
		std::string letters = "-f";
		if (exec.isFunctionReadonly(name)) letters += 'r';
		if (exec.isFunctionTraced(name)) letters += 't';
		if (exec.isFunctionExported(name)) letters += 'x';
		return letters;
	}

	// bash follows a definition with the attributes it cannot show.
	static void printFunctionWithAttributes(const Executor& exec, const FunctionDef& def) {
		printFunctionDefinition(def);
		const std::string attributes = functionAttributes(exec, def.name);
		if (attributes != "-f") std::printf("declare %s %s\n", attributes.c_str(), def.name.c_str());
	}

	static bool functionPassesFilter(const Executor& exec, const Options& opts,
			const std::string& name) {
		if (opts.export_on && !exec.isFunctionExported(name)) return false;
		return !opts.readonly_on || exec.isFunctionReadonly(name);
	}

	// `declare -xF` lists only the exported functions, `-rF` the readonly ones.
	static void printAllFunctions(Executor& exec, const Options& opts) {
		const bool names_only = opts.function_names;
		for (const std::string& name : sortedFunctionNames(exec)) {
			if (!functionPassesFilter(exec, opts, name)) continue;
			if (names_only) std::printf("declare %s %s\n", functionAttributes(exec, name).c_str(),
				name.c_str());
			else            printFunctionWithAttributes(exec, *exec.findFunction(name));
		}
	}

	static void printExportedFunctions(Executor& exec) {
		for (const std::string& name : sortedFunctionNames(exec)) {
			if (exec.isFunctionExported(name)) printFunctionWithAttributes(exec, *exec.findFunction(name));
		}

		std::fflush(stdout);
	}

	static bool exportableFunctionName(const std::string& name) {
		return name.find('/') == std::string::npos && name.find('=') == std::string::npos;
	}

	static int setFunctionExport(Executor& exec, const Command& cmd, const std::string& name,
			bool on) {
		if (exec.findFunction(name) == nullptr) {
			shellErrorf("%s: %s: not a function\n", cmd.name, name.c_str());
			return 1;
		}

		if (on && !exportableFunctionName(name)) {
			shellErrorf("%s: %s: cannot export\n", cmd.name, name.c_str());
			return 1;
		}

		if (on) exec.exportFunction(name);
		else    exec.unexportFunction(name);
		return 0;
	}

	static bool changesFunctionAttributes(const Options& opts) {
		return opts.export_on || opts.export_off || opts.trace_on || opts.trace_off
			|| opts.readonly_on || opts.readonly_off;
	}

	static int setFunctionReadonly(Executor& exec, const Command& cmd, const Options& opts,
			const std::string& name) {
		if (opts.readonly_off && exec.isFunctionReadonly(name)) {
			shellErrorf("%s: %s: readonly function\n", cmd.name, name.c_str());
			return 1;
		}

		if (opts.readonly_on) exec.setFunctionReadonly(name);
		return 0;
	}

	static int setFunctionAttributes(Executor& exec, const Command& cmd, const Options& opts,
			const std::string& name) {
		if (exec.findFunction(name) == nullptr) return 1;

		const bool exports = opts.export_on || opts.export_off;
		if (exports && setFunctionExport(exec, cmd, name, opts.export_on) != 0) return 1;

		if (opts.trace_on || opts.trace_off) exec.setFunctionTraced(name, opts.trace_on);
		return setFunctionReadonly(exec, cmd, opts, name);
	}

	static int declareFunctionAttributes(Executor& exec, const Command& cmd, const Options& opts,
			const std::vector<std::string>& args, std::size_t first) {
		if (first == args.size()) {
			if (opts.readonly_on)    printAllFunctions(exec, opts);
			else if (opts.export_on) printExportedFunctions(exec);
			return 0;
		}

		int status = 0;
		for (std::size_t i = first; i < args.size(); ++i) {
			if (setFunctionAttributes(exec, cmd, opts, args[i]) != 0) status = 1;
		}

		return status;
	}

	static int exportFunctions(Executor& exec, const Command& cmd,
			const std::vector<std::string>& args, std::size_t first, bool on) {
		if (first == args.size()) {
			printExportedFunctions(exec);
			return 0;
		}

		int status = 0;
		for (std::size_t i = first; i < args.size(); ++i) {
			if (setFunctionExport(exec, cmd, args[i], on) != 0) status = 1;
		}

		return status;
	}

	static std::vector<std::string> sortedLocalNames(Executor& exec) {
		std::vector<std::string> names = exec.localNames();
		std::sort(names.begin(), names.end());
		return names;
	}

	static int listDeclarations(Executor& exec, const Options& opts, bool locals_only) {
		const Environment& env = exec.env();
		const bool plain = !opts.print && !hasFilter(opts) && !locals_only;
		const std::vector<std::string> names = locals_only
			? sortedLocalNames(exec)
			: knownVariableNames(env);

		if (locals_only && exec.hasLocalOptions()) printLine("local -");
		for (const std::string& name : names) {
			if (!matchesFilter(env, opts, name)) continue;
			if (plain && !env.has(name) && !env.isNameref(name)) continue;

			printLine(listedLine(env, opts, plain, name));
		}

		if (plain && !locals_only) printAllFunctions(exec, Options{});
		std::fflush(stdout);
		return 0;
	}

	static bool isLocalHere(Executor& exec, const std::string& name);

	static int printNamedDeclarations(Executor& exec, const Command& cmd,
			const std::vector<std::string>& args, std::size_t first) {
		int status = 0;
		for (std::size_t i = first; i < args.size(); ++i) {
			const std::string name = args[i].substr(0, args[i].find('='));
			const bool outside = &cmd == &kLocal && !isLocalHere(exec, name);
			if (outside || !isKnownVariable(exec.env(), name)) {
				shellErrorf("%s: %s: not found\n", cmd.name, name.c_str());
				status = 1;
				continue;
			}

			printLine(declareLine(exec.env(), name));
		}

		std::fflush(stdout);
		return status;
	}

	static int reportFunctionAssignment() {
		shellErrorf("declare: cannot use `-f' to make functions\n");
		return 1;
	}

	// As bash's declare_invalid_opts: -f with an attribute only a variable
	// can have.
	static int rejectFunctionAttributes(const Command& cmd, const Options& opts) {
		const char* letter = opts.nameref_on ? "-n"
			: (opts.add_attrs & kAttrInteger) != 0 ? "-i"
			: opts.assoc ? "-A"
			: opts.indexed ? "-a"
			: nullptr;
		if (letter == nullptr) return 0;

		shellErrorf("%s: %s: invalid option\n", cmd.name, letter);
		return 1;
	}

	static void printNamedFunction(const Executor& exec, const Options& opts,
			const FunctionDef& def) {
		if (!opts.function_names) {
			printFunctionWithAttributes(exec, def);
			return;
		}

		if (!opts.print) {
			std::printf("%s\n", def.name.c_str());
			return;
		}

		std::printf("declare %s %s\n", functionAttributes(exec, def.name).c_str(), def.name.c_str());
	}

	static int declareFunctions(Executor& exec, const Options& opts,
			const std::vector<std::string>& args, std::size_t first) {
		if (first == args.size()) {
			printAllFunctions(exec, opts);
			return 0;
		}

		int status = 0;
		for (std::size_t i = first; i < args.size(); ++i) {
			const FunctionDef* def = exec.findFunction(args[i]);
			if (def == nullptr && args[i].find('=') != std::string::npos) {
				return reportFunctionAssignment();
			}

			if (def == nullptr && opts.print) {
				shellErrorf("declare: %s: not found\n", args[i].c_str());
			}

			if (def == nullptr) status = 1;
			else                printNamedFunction(exec, opts, *def);
		}

		std::fflush(stdout);
		return status;
	}

	static void printReadonlyFunctions(Executor& exec) {
		for (const std::string& name : sortedFunctionNames(exec)) {
			if (exec.isFunctionReadonly(name)) printFunctionWithAttributes(exec, *exec.findFunction(name));
		}

		std::fflush(stdout);
	}

	static int markFunctionsReadonly(Executor& exec, const Command& cmd,
			const std::vector<std::string>& args, std::size_t first) {
		if (first == args.size()) {
			printReadonlyFunctions(exec);
			return 0;
		}

		int status = 0;
		for (std::size_t i = first; i < args.size(); ++i) {
			if (exec.findFunction(args[i]) != nullptr) {
				exec.setFunctionReadonly(args[i]);
				continue;
			}

			shellErrorf("%s: %s: not a function\n", cmd.name, args[i].c_str());
			status = 1;
		}

		return status;
	}

	static bool isLocalHere(Executor& exec, const std::string& name) {
		return exec.isLocalHere(name);
	}

	static bool changesValueAttributes(const Options& opts) {
		return makesArray(opts) || opts.array_off || opts.add_attrs != 0 || opts.remove_attrs != 0;
	}

	// A new local may hide a calling function's readonly local, not a
	// readonly global.
	static bool shadowsCallerLocal(Executor& exec, const Operand& op, bool make_local) {
		return make_local && !isLocalHere(exec, op.name) && exec.isCallerLocal(op.name);
	}

	static bool rejectsReadonly(Executor& exec, const Command& cmd, const Options& opts,
			const Operand& op, bool make_local) {
		if (!exec.env().isReadonly(op.name)) return false;
		if (shadowsCallerLocal(exec, op, make_local)) return false;
		if (op.has_value || opts.readonly_off) return true;
		if (isDeclareFamily(cmd) && changesValueAttributes(opts)) return true;
		return make_local && !isLocalHere(exec, op.name);
	}

	// The array kind -a or -A asks for that @p name has the other of: what
	// the conversion error names, or false when there is no conflict.
	static bool conflictingKind(const Environment& env, const Options& opts,
			const std::string& name, const char*& from, const char*& to) {
		if (opts.assoc && env.isIndexedArray(name)) {
			from = "indexed";
			to = "associative";
			return true;
		}

		if (!opts.indexed || !env.isAssocArray(name)) return false;

		from = "associative";
		to = "indexed";
		return true;
	}

	static bool reportConversion(const std::string& prefix, const std::string& name,
			const char* from, const char* to) {
		shellErrorf("%s%s%s: cannot convert %s to %s array\n", prefix.c_str(),
			prefix.empty() ? "" : ": ", name.c_str(), from, to);
		return false;
	}

	enum class LocalKind { None, New, Earlier };

	// bash converts a variable that exists, which gives it a smaller table;
	// a new local starts with a full one, and a local the function made
	// before is made afresh, empty.
	static bool makeAssocArray(Environment& env, const std::string& name, LocalKind local) {
		if (env.isAssocArray(name)) return true;

		const bool had_value = env.has(name);
		const std::string value = env.get(name);
		const bool existed = had_value || (env.isValueless(name) && local == LocalKind::None);
		env.declareAssocArray(name, local != LocalKind::Earlier && existed);
		if (local == LocalKind::Earlier) return true;

		if (had_value) env.setAssocElement(name, "0", value);
		else env.markValueless(name);
		return true;
	}

	static bool makeIndexedArray(Environment& env, const std::string& name) {
		if (env.isIndexedArray(name)) return true;
		if (env.isAssocArray(name)) return false;

		std::vector<std::string> values;
		const bool had_value = env.has(name);
		if (had_value) values.push_back(env.get(name));
		env.setIndexedArrayFromList(name, std::move(values));
		if (!had_value) env.markValueless(name);
		return true;
	}

	static bool applyArrayKind(Environment& env, const Command& cmd, const Options& opts,
			const std::string& name, LocalKind local) {
		const char* from = nullptr;
		const char* to = nullptr;
		if (conflictingKind(env, opts, name, from, to)) {
			return reportConversion(cmd.name, name, from, to);
		}

		if (opts.assoc) return makeAssocArray(env, name, local);
		if (opts.indexed) return makeIndexedArray(env, name);
		return true;
	}

	// `declare -r c[100]` declares the array c, as `declare -a c` would.
	static bool makesSubscriptedArray(const Environment& env, const Options& opts,
			const Operand& op) {
		return op.has_subscript && !op.has_value && !opts.assoc && !env.isAssocArray(op.name)
			&& !env.isIndexedArray(op.name);
	}

	static void applyValueAttributes(Environment& env, const Options& opts,
			const std::string& name) {
		const unsigned case_attrs = kAttrLowercase | kAttrUppercase | kAttrCapitalize;
		const unsigned case_added = opts.add_attrs & case_attrs;
		unsigned attrs = env.attributes(name) | opts.add_attrs;
		if (case_added != 0) attrs = (attrs & ~case_attrs) | case_added;
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

	// An error in a subscript stops this operand; as in bash, the error
	// drops the rest of the line once the command has ended.
	// bash expands the subscript a builtin was given once more, unless
	// array_expand_once is on; `unset` never does.
	// declare removes the double quotes of a subscript, as bash's word
	// expansion of its operand does, assoc_expand_once or not.
	static bool evalSubscript(Executor& exec, const std::string& subscript, bool expands,
			long long& index) {
		Expander& expander = exec.expander();
		const std::string text = Lexer::withoutArithmeticDoubleQuotes(subscript);
		index = expands ? expander.evalArith(text) : expander.evalArithLiteral(text);
		if (!expander.failed()) return true;

		shellErrorf("%s\n", expander.takeError().c_str());
		expander.dropRestOfLine();
		return false;
	}

	static void assignElement(Executor& exec, const Operand& op) {
		if (op.subscript.empty()) {
			shellErrorf("%s[]: bad array subscript\n", op.name.c_str());
			return;
		}

		Environment& env = exec.env();
		long long index = 0;
		const bool assoc = env.isAssocArray(op.name);
		const bool expands = !env.shopts().array_expand_once;
		if (!assoc && !evalSubscript(exec, op.subscript, expands, index)) return;

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

	// export and readonly take a quoted `name=(...)` as a plain value unless
	// -a or -A hands it to declare.
	static bool takesQuotedCompound(const Command& cmd, const Options& opts) {
		return isDeclareFamily(cmd) || makesArray(opts);
	}

	static bool assignOperand(Executor& exec, const Command& cmd, const Options& opts,
			const Operand& op) {
		Environment& env = exec.env();
		const bool quoted_compound = isArray(env, op.name) && looksCompound(op.value);
		if (op.has_subscript && quoted_compound && makesArray(opts)) {
			return exec.assignCompound(op.name + "=" + op.value);
		}

		if (op.has_subscript) {
			if (looksCompound(op.value) && !isArray(env, op.name)) {
				shellWarningAt(static_cast<std::size_t>(env.currentLineno()),
					op.text + ": quoted compound array assignment deprecated");
			}

			assignElement(exec, op);
			return true;
		}

		if (op.compound || (quoted_compound && takesQuotedCompound(cmd, opts))) {
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
			const Operand& op, bool make_local);

	// bash performs `name=(...)` before the builtin runs: it declares the
	// variable with the array and value options first, which fails with
	// this_command_name's prefix, then assigns, which fails bare. Either
	// error drops the command line, except where declare, typeset or local
	// make a function's variable: that builtin then runs on the name alone.
	static int rejectCompoundOnReadonly(Executor& exec, const Command& cmd, const Options& opts,
			const Operand& op, bool make_local) {
		const bool declare_fails = changesValueAttributes(opts)
			|| (make_local && !isLocalHere(exec, op.name));
		printReadonlyError(declare_fails ? exec.thisCommandName() : std::string(), op.name);
		if (!declare_fails || !isDeclareFamily(cmd) || exec.funcDepth() == 0) {
			exec.raiseDiscard();
			return 1;
		}

		Operand bare = op;
		bare.has_value = false;
		bare.compound = false;
		return declareOperand(exec, cmd, opts, bare, make_local);
	}

	// The same goes for `name=(...)` on an array of the other kind, except
	// that the builtin then fails on the name alone as well.
	static int rejectCompoundConversion(Executor& exec, const Command& cmd, const Operand& op,
			const char* from, const char* to) {
		reportConversion(exec.thisCommandName(), op.name, from, to);
		if (!isDeclareFamily(cmd) || exec.funcDepth() == 0) {
			exec.raiseDiscard();
			return 1;
		}

		reportConversion(cmd.name, op.name, from, to);
		return 1;
	}

	static int declareOperand(Executor& exec, const Command& cmd, const Options& opts,
			const Operand& op, bool make_local) {
		Environment& env = exec.env();
		if (op.compound && env.isReadonly(op.name) && !shadowsCallerLocal(exec, op, make_local)) {
			return rejectCompoundOnReadonly(exec, cmd, opts, op, make_local);
		}

		const char* from = nullptr;
		const char* to = nullptr;
		if (op.compound && conflictingKind(env, opts, op.name, from, to)) {
			return rejectCompoundConversion(exec, cmd, op, from, to);
		}

		if (rejectsReadonly(exec, cmd, opts, op, make_local)) {
			return reportReadonly(cmd, opts, op.name);
		}

		if (opts.array_off && isArray(env, op.name)) {
			shellErrorf("%s: %s: cannot destroy array variables in this way\n", cmd.name,
				op.name.c_str());
			return 1;
		}

		// The items of a new local's compound value still see what it hides.
		std::function<void()> assign_items;
		const bool items_first = op.compound && make_local;
		if (items_first && !exec.expandCompound(op.text, assign_items)) return 1;

		const LocalKind local = isLocalHere(exec, op.name) ? LocalKind::Earlier
			: make_local ? LocalKind::New : LocalKind::None;
		if (make_local) exec.makeLocal(op.name, opts.inherit);
		if (!applyArrayKind(env, cmd, opts, op.name, local)) return 1;
		if (makesSubscriptedArray(env, opts, op) && !makeIndexedArray(env, op.name)) return 1;

		applyValueAttributes(env, opts, op.name);
		if (items_first) assign_items();
		else if (op.has_value && !assignOperand(exec, cmd, opts, op)) return 1;
		if (!op.has_value && !env.has(op.name) && !isArray(env, op.name)) env.markValueless(op.name);

		applyVisibility(env, opts, op.name);
		return 0;
	}

	// export and readonly take whole variables: bash rejects `readonly a[5]`
	// and `export a[1]=v` unless -a or -A hands the operand to declare.
	static bool rejectsSubscript(const Command& cmd, const Options& opts, const Operand& op) {
		return op.has_subscript && !isDeclareFamily(cmd) && !makesArray(opts);
	}

	static bool isCompoundArg(const std::vector<bool>& compound, std::size_t arg_index) {
		return arg_index + 1 < compound.size() && compound[arg_index + 1];
	}

	// In POSIX mode the error ends the shell before export or readonly
	// looks at another operand.
	static bool endsAtFirstError(const Executor& exec, const Command& cmd) {
		return exec.posixErrorsExit() && (&cmd == &kExport || &cmd == &kReadonly);
	}

	static int declareOperands(Executor& exec, const Command& cmd, const Options& opts,
			const std::vector<std::string>& args, std::size_t first, bool make_local) {
		const std::vector<bool> compound = exec.takeCompoundArgs();
		const bool loose_keys = exec.env().shopts().array_expand_once;
		int status = 0;
		for (std::size_t i = first; i < args.size(); ++i) {
			Operand op;
			if (!parseOperand(args[i], op, loose_keys)) {
				status = reportInvalidIdentifier(cmd, args[i]);
				if (endsAtFirstError(exec, cmd)) return status;
				continue;
			}

			if (rejectsSubscript(cmd, opts, op)) {
				status = reportInvalidIdentifier(cmd, op.name + "[" + op.subscript + "]");
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
		shellErrorf("%s: `%s': invalid variable name for name reference\n",
			cmd.name, target.c_str());
		return 1;
	}

	static int reportSelfReference(const Command& cmd, const std::string& name) {
		shellErrorf("%s: %s: nameref variable self references not allowed\n",
			cmd.name, name.c_str());
		return 1;
	}

	static int reportArrayReference(const Command& cmd, const std::string& name) {
		shellErrorf("%s: %s: reference variable cannot be an array\n", cmd.name, name.c_str());
		return 1;
	}

	static int reportEmptyReference(const Command& cmd) {
		shellErrorf("%s: `': not a valid identifier\n", cmd.name);
		return 1;
	}

	static int declareNamerefOperand(Executor& exec, const Command& cmd, const Operand& op,
			bool make_local) {
		if (op.has_subscript) return reportArrayReference(cmd, op.name + "[" + op.subscript + "]");

		Environment& env = exec.env();
		const bool held = !op.has_value && !env.isNameref(op.name);
		if (!make_local && held && env.isReadonly(op.name)) {
			shellErrorf("%s: %s: readonly variable\n", cmd.name, op.name.c_str());
			return 1;
		}

		if (make_local) exec.makeLocal(op.name);
		if (!env.isNameref(op.name) && isArray(env, op.name)) return reportArrayReference(cmd, op.name);
		if (op.has_value && op.value.empty()) return reportEmptyReference(cmd);

		const std::string target = namerefTargetFor(env, op);
		if (held && env.has(op.name) && target.empty()) return reportBadRefTarget(cmd, target);
		if (!isValidRefTarget(target)) return reportBadRefTarget(cmd, target);
		if (target == op.name) return reportSelfReference(cmd, op.name);

		env.setNameref(op.name, target);
		return 0;
	}

	// bash assigns through the reference before it drops the attribute:
	// `typeset +n foo=other` sets foo's target, and foo keeps its name.
	static int removeNamerefOperand(Executor& exec, const Operand& op) {
		if (op.has_value) exec.env().set(op.name, op.value);
		exec.env().clearNameref(op.name);
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

		if (opts.functions && changesFunctionAttributes(opts)) {
			return declareFunctionAttributes(exec, cmd, opts, args, first);
		}
		const bool functions = opts.functions || opts.function_names;
		if (functions && first < args.size() && rejectFunctionAttributes(cmd, opts) != 0) return 1;
		if (functions) return declareFunctions(exec, opts, args, first);
		if (first == args.size()) return listDeclarations(exec, opts, locals_only);
		if (opts.print) return printNamedDeclarations(exec, cmd, args, first);

		const bool make_local = locals_only || (exec.funcDepth() > 0 && !opts.global);
		if (opts.nameref_on || opts.nameref_off) {
			return declareNamerefs(exec, cmd, opts, args, first, make_local);
		}

		if (opts.global && exec.funcDepth() > 0) {
			return exec.runInGlobalScope([&]() {
				return declareOperands(exec, cmd, opts, args, first, false);
			});
		}

		return declareOperands(exec, cmd, opts, args, first, make_local);
	}

	// `-a` and `-A` narrow the listing down to arrays of that kind.
	static int listWithFilter(Executor& exec, const Command& cmd, const Options& opts) {
		Options filter;
		filter.print = true;
		filter.indexed = opts.indexed;
		filter.assoc = opts.assoc;
		filter.export_on = &cmd == &kExport;
		filter.readonly_on = &cmd == &kReadonly;
		if (exec.env().options().posix) filter.posix_listing = cmd.name;
		return listDeclarations(exec, filter, false);
	}

	static bool splitElementOperand(const std::string& arg, std::string& name,
			std::string& subscript) {
		const std::size_t bracket = arg.find('[');
		if (bracket == std::string::npos || bracket == 0 || arg.back() != ']') return false;
		if (nameLength(arg) != bracket || bracket + 2 > arg.size() - 1) return false;

		name = arg.substr(0, bracket);
		subscript = arg.substr(bracket + 1, arg.size() - bracket - 2);
		return true;
	}

	// bash 5.2+ (compat > 51): `unset 'a[@]'` empties an indexed array but
	// keeps it declared; on an assoc array `@` and `*` are ordinary keys.
	// A word typed as `name[key]`, unquoted, gives unset the key its own
	// expansion made; a quoted one is a string whose key unset expands,
	// unless assoc_expand_once says it was expanded already.
	static int unsetAssocElement(Executor& exec, const std::string& name,
			const std::string& subscript, bool typed_as_reference) {
		Expander& expander = exec.expander();
		const bool expand = !typed_as_reference && !exec.env().shopts().array_expand_once;
		const std::string key = expand ? expander.assocKeyText(subscript) : subscript;
		if (expander.failed()) {
			shellErrorf("%s\n", expander.takeError().c_str());
			return 1;
		}

		if (key.empty()) {
			shellErrorf("unset: [%s]: bad array subscript\n", subscript.c_str());
			return 1;
		}

		exec.env().unsetElement(name, 0, key);
		return 0;
	}

	static const int kCurrentCompatLevel = 53;

	// A negative index counts back from one past the last element.
	static bool countFromEnd(const Environment& env, const std::string& name, long long& index) {
		const Environment::IndexedArray* indexed = env.getIndexedArray(name);
		const bool empty = indexed == nullptr || indexed->empty();
		const long long end = empty ? 0 : indexed->rbegin()->first + 1;
		if (index + end < 0) return false;

		index += end;
		return true;
	}

	// BASH_COMPAT as a number: `5.1` and `51` are both 51; unset, the
	// current level.
	static int compatLevel(const Environment& env) {
		std::string digits;
		for (const char c : env.get("BASH_COMPAT")) {
			if (std::isdigit(static_cast<unsigned char>(c)) != 0) digits.push_back(c);
		}

		int level = 0;
		if (digits.empty() || !parseInt(digits, level)) return kCurrentCompatLevel;
		return level;
	}

	static int unsetIndexedElement(Executor& exec, const std::string& name,
			const std::string& subscript) {
		if (subscript == "@" || subscript == "*") {
			if (compatLevel(exec.env()) <= 51) exec.env().unset(name);
			else exec.env().setIndexedArraySparse(name, {});
			return 0;
		}

		long long index = 0;
		if (!evalSubscript(exec, subscript, false, index)) return 1;
		if (index < 0 && !countFromEnd(exec.env(), name, index)) {
			shellErrorf("unset: [%s]: bad array subscript\n", subscript.c_str());
			return 1;
		}

		exec.env().unsetElement(name, index, subscript);
		return 0;
	}

	static int unsetScalarElement(Executor& exec, const std::string& name,
			const std::string& subscript) {
		if (!exec.env().has(name)) return 0;

		long long index = -1;
		const bool whole = subscript == "@" || subscript == "*";
		if (!whole && !evalSubscript(exec, subscript, false, index)) return 1;
		if (index == 0) {
			exec.env().unset(name);
			return 0;
		}

		shellErrorf("unset: %s: not an array variable\n", name.c_str());
		return 1;
	}

	// -1: not an element operand at all.
	static int unsetArrayElement(Executor& exec, const std::string& arg) {
		std::string name;
		std::string subscript;
		if (!splitElementOperand(arg, name, subscript)) return -1;

		const Environment& env = exec.env();
		if (env.isReadonly(name)) {
			shellErrorf("unset: %s: cannot unset: readonly variable\n", name.c_str());
			return 1;
		}

		if (env.isAssocArray(name)) {
			return unsetAssocElement(exec, name, subscript, exec.typedAsArrayRef(arg));
		}
		if (env.isIndexedArray(name)) return unsetIndexedElement(exec, name, subscript);
		return unsetScalarElement(exec, name, subscript);
	}

	// The call-stack arrays bash marks att_nounset.
	static bool isUnunsettable(const std::string& name) {
		return name == "BASH_ARGC" || name == "BASH_ARGV" || name == "BASH_SOURCE"
			|| name == "BASH_LINENO";
	}

	static int unsetFunctionOperand(Executor& exec, const std::string& name) {
		if (exec.isFunctionReadonly(name)) {
			shellErrorf("unset: %s: cannot unset: readonly function\n", name.c_str());
			return 1;
		}

		exec.unsetFunction(name);
		return 0;
	}

	// Without -v or -n a name no variable can have may still be a function's.
	static int unsetInvalidName(Executor& exec, const Options& opts, const std::string& text) {
		if (opts.variables_only || opts.n_flag) return reportInvalidIdentifier(kUnset, text);

		return unsetFunctionOperand(exec, text);
	}

	// A local of this function stays declared, attributes and all, with no
	// value: `declare -p` still shows it.
	static void unsetKeepingLocal(Executor& exec, const std::string& name) {
		Environment& env = exec.env();
		const bool local = isLocalHere(exec, name);
		const bool exported = env.isExported(name);
		const unsigned attributes = env.attributes(name);
		env.unset(name);
		if (!local) return;

		env.markValueless(name);
		env.setAttributes(name, attributes);
		if (exported) env.exportVar(name);
	}

	static int unsetOperand(Executor& exec, const Options& opts, const std::string& text) {
		if (opts.functions) return unsetFunctionOperand(exec, text);

		const int element_status = unsetArrayElement(exec, text);
		if (element_status >= 0) return element_status;
		if (nameLength(text) != text.size()) return unsetInvalidName(exec, opts, text);

		Environment& env = exec.env();
		if (!opts.n_flag && isUnunsettable(text)) {
			shellErrorf("unset: %s: cannot unset\n", text.c_str());
			return 1;
		}

		if (env.isReadonly(text)) {
			const std::string& shown = opts.n_flag ? text : env.resolveRef(text);
			shellErrorf("unset: %s: cannot unset: readonly variable\n", shown.c_str());
			return 1;
		}

		if (opts.n_flag) {
			env.unsetNameref(text);
			return 0;
		}

		if (!opts.variables_only && !isKnownVariable(env, text)) {
			return unsetFunctionOperand(exec, text);
		}

		const bool upvar = !env.shopts().localvar_unset;
		if (upvar && exec.unsetCallerLocal(text)) return 0;

		unsetKeepingLocal(exec, text);
		return 0;
	}

	static int builtin_declare(Executor& exec, const std::vector<std::string>& args) {
		return runDeclare(exec, kDeclare, args, false);
	}

	static int builtin_typeset(Executor& exec, const std::vector<std::string>& args) {
		return runDeclare(exec, kTypeset, args, false);
	}

	// `local -` makes the function's `set` option changes its own.
	static std::vector<std::string> takeLocalOptions(Executor& exec,
			const std::vector<std::string>& args) {
		std::vector<std::string> rest;
		for (const std::string& arg : args) {
			if (arg == "-") exec.makeLocal(arg);
			else rest.push_back(arg);
		}

		return rest;
	}

	static int builtin_local(Executor& exec, const std::vector<std::string>& args) {
		if (exec.funcDepth() == 0) {
			shellErrorf("local: can only be used in a function\n");
			return 1;
		}

		const std::vector<std::string> rest = takeLocalOptions(exec, args);
		if (rest.empty() && !args.empty()) return 0;

		return runDeclare(exec, kLocal, rest, true);
	}

	static int builtin_export(Executor& exec, const std::vector<std::string>& args) {
		Options opts;
		std::size_t first = 0;
		const int status = parseOptions(kExport, args, opts, first);
		if (status != 0) return status;

		if (opts.functions) return exportFunctions(exec, kExport, args, first, !opts.n_flag);
		if (first == args.size()) return listWithFilter(exec, kExport, opts);

		opts.export_on = !opts.n_flag;
		opts.export_off = opts.n_flag;
		return declareOperands(exec, kExport, opts, args, first, false);
	}

	static int builtin_readonly(Executor& exec, const std::vector<std::string>& args) {
		Options opts;
		std::size_t first = 0;
		const int status = parseOptions(kReadonly, args, opts, first);
		if (status != 0) return status;

		if (opts.functions) return markFunctionsReadonly(exec, kReadonly, args, first);
		if (first == args.size()) return listWithFilter(exec, kReadonly, opts);

		opts.readonly_on = true;
		return declareOperands(exec, kReadonly, opts, args, first, false);
	}

	static int builtin_unset(Executor& exec, const std::vector<std::string>& args) {
		Options opts;
		std::size_t first = 0;
		int status = parseOptions(kUnset, args, opts, first);
		if (status != 0) return status;
		if (opts.functions && opts.variables_only) {
			shellErrorf("unset: cannot simultaneously unset a function and a variable\n");
			return 1;
		}

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

	int listShellVariables(Executor& exec) {
		return declare_detail::listDeclarations(exec, declare_detail::Options{}, false);
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

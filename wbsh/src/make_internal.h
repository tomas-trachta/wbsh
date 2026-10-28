#pragma once

/**
 * @file make_internal.h
 * @brief What the translation units of `make` share: the variable and rule
 *        database, the run state, and the entry points of reading, expansion,
 *        functions, implicit-rule search, updating and recipes.
 */

#include <cstddef>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wbsh {
	class Executor;
}

namespace wbsh::make_detail {

	using NameValueList = std::vector<std::pair<std::string, std::string>>;

	struct Location {
		std::string file;
		int line = 0;
	};

	enum class Flavor { Recursive, Simple };

	/// In order of precedence: a definition only replaces one of no higher origin.
	enum class Origin {
		Default, Environment, File, EnvironmentOverride, CommandLine, Override, Automatic,
	};

	enum class ExportMode { Default, Export, Unexport };

	struct Variable {
		std::string value;
		Flavor flavor = Flavor::Recursive;
		Origin origin = Origin::File;
		ExportMode export_mode = ExportMode::Default;
		bool append = false;
		bool is_private = false;
	};

	using VarTable = std::map<std::string, Variable>;

	struct PatternVars {
		std::string pattern;
		VarTable vars;
	};

	struct FileNode;

	/// One level of target-specific variables; the global table follows the last level.
	struct VarScope {
		const VarTable* table = nullptr;
		const VarScope* parent = nullptr;
		const FileNode* owner = nullptr;
	};

	struct AutoVars {
		std::string target;
		std::string stem;
		std::vector<std::string> prereqs;
		std::vector<std::string> all_prereqs;
		std::vector<std::string> newer;
		std::vector<std::string> order_only;
	};

	struct ExpandContext {
		const VarScope* scope = nullptr;
		const FileNode* target = nullptr;
		const AutoVars* autos = nullptr;
	};

	struct RecipeLine {
		std::string text;
		Location where;
	};

	struct Recipe {
		std::vector<RecipeLine> lines;
		Location where;
		bool defined = false;
	};

	struct FileTime {
		bool exists = false;
		long long seconds = 0;
		long nanos = 0;
	};

	enum class UpdateState { Pending, Running, Done };

	enum class UpdateStatus { Success, Question, Failed };

	struct DoubleColonRule {
		std::vector<std::string> prereqs;
		std::vector<std::string> order_only;
		Recipe recipe;
	};

	struct FileNode {
		std::string name;
		std::vector<std::string> prereqs;
		std::vector<std::string> order_only;
		Recipe recipe;
		std::string stem;
		std::vector<DoubleColonRule> double_colon_rules;
		std::vector<std::string> group;
		VarTable vars;

		bool is_target = false;
		bool mentioned = false;
		bool double_colon = false;
		bool phony = false;
		bool precious = false;
		bool intermediate = false;
		bool secondary = false;
		bool not_intermediate = false;
		bool silent = false;
		bool ignore_errors = false;
		bool assume_old = false;
		bool assume_new = false;

		bool implicit_searched = false;
		std::vector<std::string> implicit_prereqs;
		std::vector<std::string> implicit_order_only;
		std::vector<std::string> implicit_siblings;

		UpdateState state = UpdateState::Pending;
		UpdateStatus status = UpdateStatus::Success;
		bool mtime_known = false;
		FileTime mtime;
		std::string vpath_name;
		std::string needed_by;
	};

	struct PatternRule {
		std::vector<std::string> targets;
		std::vector<std::string> prereqs;
		std::vector<std::string> order_only;
		Recipe recipe;
		bool terminal = false;
		bool in_use = false;
	};

	struct VpathEntry {
		std::string pattern;
		std::vector<std::string> dirs;
	};

	struct IncludedMakefile {
		std::string name;
		Location where;
		bool optional = false;
		bool missing = false;
	};

	struct MakeOptions {
		std::vector<std::string> makefiles;
		std::vector<std::string> directories;
		std::vector<std::string> include_dirs;
		std::vector<std::string> goals;
		std::vector<std::string> assignments;
		std::vector<std::string> evals;
		std::vector<std::string> old_files;
		std::vector<std::string> new_files;
		int jobs = 1;
		bool dry_run = false;
		bool silent = false;
		bool keep_going = false;
		bool ignore_errors = false;
		bool question = false;
		bool touch = false;
		bool always_make = false;
		bool environment_overrides = false;
		bool no_builtin_rules = false;
		bool no_builtin_variables = false;
		bool print_database = false;
		bool print_directory = false;
		bool no_print_directory = false;
		bool warn_undefined = false;
		bool show_help = false;
		bool show_version = false;
	};

	struct SpecialTargets {
		bool export_all = false;
		bool oneshell = false;
		bool delete_on_error = false;
		bool posix = false;
		bool silent_all = false;
		bool ignore_all = false;
		bool secondary_all = false;
		bool second_expansion = false;
	};

	struct Make {
		explicit Make(Executor& executor) : exec(executor) {}

		Executor& exec;
		MakeOptions options;
		VarTable globals;
		std::vector<PatternVars> pattern_vars;
		std::unordered_map<std::string, FileNode> files;
		std::vector<std::string> file_order;
		std::vector<PatternRule> pattern_rules;
		std::vector<VpathEntry> vpaths;
		std::vector<std::string> suffixes;
		std::vector<std::string> precious_patterns;
		std::vector<IncludedMakefile> includes;
		std::vector<std::string> intermediates_made;
		SpecialTargets special;
		NameValueList base_environment;
		Location where;
		std::vector<const Variable*> expanding;
		std::string self_path;
		int level = 0;
		int restarts = 0;
		int commands_started = 0;
		int call_args = 0;
		int call_depth = 0;
		bool reading_builtins = false;
		bool computing_environment = false;
		bool fatal = false;
		bool interrupted = false;
	};

	inline bool operator==(const FileTime& a, const FileTime& b) {
		return a.exists == b.exists && a.seconds == b.seconds && a.nanos == b.nanos;
	}

	inline bool operator!=(const FileTime& a, const FileTime& b) {
		return !(a == b);
	}

	inline bool isNewer(const FileTime& a, const FileTime& b) {
		if (a.seconds != b.seconds) return a.seconds > b.seconds;
		return a.nanos > b.nanos;
	}

	// make_report.cpp
	std::string programName(const Make& make);
	void reportFatal(Make& make, const Location& where, const std::string& message);
	void reportWarning(const Make& make, const Location& where, const std::string& message);
	void reportError(const Make& make, const std::string& message);
	void reportMessage(const Make& make, const std::string& message);
	void printLine(const std::string& text);

	// make_text.cpp
	bool isBlank(char c);
	std::vector<std::string> splitWords(const std::string& text);
	std::string joinWords(const std::vector<std::string>& words);
	std::string trimBlanks(const std::string& text);
	std::size_t skipBlanks(const std::string& text, std::size_t from);
	bool opensReference(const std::string& text, std::size_t i);
	std::size_t findReferenceEnd(const std::string& text, std::size_t dollar);
	std::size_t findUnquoted(const std::string& text, const std::string& wanted, std::size_t from);
	bool matchPattern(const std::string& pattern, const std::string& text, std::string& stem);
	std::string substituteStem(const std::string& pattern, const std::string& stem);
	std::string patsubstText(const std::string& pattern, const std::string& replacement,
	                         const std::string& text);
	std::string directoryOf(const std::string& word);
	std::string notDirectoryOf(const std::string& word);
	bool hasGlobMeta(const std::string& word);
	std::vector<std::string> uniqueWords(const std::vector<std::string>& words);

	// make_vars.cpp
	struct VarRef {
		const Variable* variable = nullptr;
		const VarScope* rest = nullptr;
		bool global = false;
	};

	bool defineVariable(VarTable& table, const std::string& name, const Variable& variable);
	void setGlobal(Make& make, const std::string& name, const std::string& value, Origin origin);
	Variable* findGlobal(Make& make, const std::string& name);
	VarRef lookupVariable(const Make& make, const std::string& name, const ExpandContext& ctx);
	VarRef lookupBelow(const Make& make, const std::string& name, const VarRef& found,
	                   const ExpandContext& ctx);
	std::string originName(Origin origin);
	NameValueList childEnvironment(Make& make, const ExpandContext& ctx);
	std::vector<std::string> variableNames(const Make& make);

	// make_expand.cpp
	std::string expand(Make& make, const std::string& text, const ExpandContext& ctx);
	void expandInto(Make& make, const std::string& text, const ExpandContext& ctx,
	                std::string& out);
	std::string expandVariable(Make& make, const std::string& name, const ExpandContext& ctx);
	std::string rawVariableValue(const Make& make, const std::string& name,
	                             const ExpandContext& ctx);
	bool isAutomaticName(const std::string& name);

	// make_functions.cpp, make_func_control.cpp, make_func_system.cpp
	struct FunctionCall {
		Make& make;
		const ExpandContext& ctx;
		const std::string& name;
		std::vector<std::string> args;
	};

	bool callFunction(Make& make, const std::string& inner, char close,
	                  const ExpandContext& ctx, std::string& out);
	void functionIf(FunctionCall& call, std::string& out);
	void functionOr(FunctionCall& call, std::string& out);
	void functionAnd(FunctionCall& call, std::string& out);
	void functionForeach(FunctionCall& call, std::string& out);
	void functionLet(FunctionCall& call, std::string& out);
	void functionCall(FunctionCall& call, std::string& out);
	void functionValue(FunctionCall& call, std::string& out);
	void functionEval(FunctionCall& call, std::string& out);
	void functionOrigin(FunctionCall& call, std::string& out);
	void functionFlavor(FunctionCall& call, std::string& out);
	void functionError(FunctionCall& call, std::string& out);
	void functionWarning(FunctionCall& call, std::string& out);
	void functionInfo(FunctionCall& call, std::string& out);
	void functionIntcmp(FunctionCall& call, std::string& out);
	void functionShell(FunctionCall& call, std::string& out);
	void functionFile(FunctionCall& call, std::string& out);
	void functionWildcard(FunctionCall& call, std::string& out);
	void functionRealpath(FunctionCall& call, std::string& out);
	void functionAbspath(FunctionCall& call, std::string& out);
	bool isFunctionName(const std::string& name);
	std::string shellOutputValue(std::string output);
	std::vector<std::string> globWord(Make& make, const std::string& pattern);

	// make_read.cpp
	struct Conditional {
		bool outer_ignoring = false;
		bool ignoring = false;
		bool taken = false;
		bool seen_else = false;
	};

	struct RuleSpec {
		std::vector<std::string> targets;
		std::vector<std::string> prereqs;
		std::vector<std::string> order_only;
		std::string target_pattern;
		bool static_pattern = false;
		bool double_colon = false;
		bool grouped = false;
		Recipe recipe;
	};

	struct Reader {
		std::string name;
		std::vector<std::string> lines;
		std::size_t next = 0;
		int line_number = 0;
		int fixed_line = 0;
		std::vector<Conditional> conditionals;
		RuleSpec rule;
		bool in_rule = false;
		bool rule_has_targets = false;
	};

	bool readMakefile(Make& make, const std::string& name);
	bool readMakefileText(Make& make, const std::string& name, const std::string& text);
	void evalText(Make& make, const std::string& text);
	bool ignoring(const Reader& reader);
	std::string takeLogicalLine(Reader& reader);
	std::string collapseContinuations(const std::string& line);
	std::string stripComment(const std::string& line);
	std::string joinLines(const std::vector<std::string>& lines);
	bool readDefineBody(Make& make, Reader& reader, std::string& body);
	std::string dropRecipePrefixes(Make& make, const std::string& text);
	char recipePrefix(Make& make);

	// make_assign.cpp
	enum class AssignOp { Recursive, Simple, Immediate, Conditional, Append, Shell };

	struct Assignment {
		std::string name;
		std::string value;
		AssignOp op = AssignOp::Recursive;
		ExportMode export_mode = ExportMode::Default;
		bool is_override = false;
		bool is_private = false;
		bool is_define = false;
		bool is_undefine = false;
	};

	bool parseAssignment(const std::string& line, bool allow_define, Assignment& out);
	void applyAssignment(Make& make, Reader& reader, Assignment& assignment);
	void applyTargetAssignment(Make& make, const std::vector<std::string>& targets,
	                           const Assignment& assignment);
	bool applyExportDirective(Make& make, const std::string& line);
	void applyCommandLineAssignment(Make& make, const std::string& text);
	bool isCommandLineAssignment(const std::string& text);

	// make_rule.cpp
	FileNode& enterFile(Make& make, const std::string& name);
	FileNode* findFile(Make& make, const std::string& name);
	void parseRuleLine(Make& make, Reader& reader, const std::string& line);
	void finishRule(Make& make, Reader& reader);
	bool isSpecialTarget(const std::string& name);

	// make_implicit.cpp
	bool findImplicitRule(Make& make, FileNode& file, int depth);
	void convertSuffixRules(Make& make);
	void addPatternRule(Make& make, PatternRule rule);
	std::string searchVpath(Make& make, const std::string& name);
	void applyVpathDirective(Make& make, const std::string& args);
	FileTime statFile(Make& make, const std::string& name);
	bool fileExists(Make& make, const std::string& name);

	// make_second.cpp
	struct SecondContext {
		std::string target;
		std::string stem;
		const VarScope* scope = nullptr;
		const FileNode* file = nullptr;
	};

	bool isDeferredPrereq(const std::string& entry);
	bool hasDeferredPrereqs(const std::vector<std::string>& entries);
	std::string deferPrereqs(const std::string& text);
	std::string deferStaticPrereqs(const std::string& entry);
	std::string deferredText(const std::string& entry);
	void expandDeferredPrereqs(Make& make, const SecondContext& context,
	                           std::vector<std::string>& prereqs,
	                           std::vector<std::string>& order_only);

	// make_defaults.cpp
	void defineDefaultVariables(Make& make);
	void readBuiltinRules(Make& make);

	// make_update.cpp
	int updateGoals(Make& make, const std::vector<std::string>& goals);
	bool remakeIncludedMakefiles(Make& make, bool& restart);
	UpdateStatus updateFile(Make& make, FileNode& file, const VarScope* inherited, int depth);
	void removeIntermediates(Make& make);

	// make_recipe.cpp
	struct RecipeJob {
		FileNode& file;
		const Recipe& recipe;
		const ExpandContext& ctx;
	};

	UpdateStatus runRecipe(Make& make, const RecipeJob& job);
	bool recipeRecurses(const Recipe& recipe);
	void touchTarget(Make& make, FileNode& file);

	// make_shell.cpp
	struct ShellProgram {
		std::string exe;
		std::vector<std::string> flags;
		bool is_wbsh = false;
		bool is_cmd = false;
	};

	ShellProgram resolveShell(Make& make, const ExpandContext& ctx);
	struct ShellResult {
		int status = 0;
		bool control_c = false;
	};

	ShellResult runShellLine(Make& make, const ShellProgram& shell, const std::string& line,
	                         const NameValueList& environment);
	bool captureShell(Make& make, const std::string& command, const ExpandContext& ctx,
	                  std::string& output, int& status);

	// make_options.cpp
	bool parseMakeArgs(const std::vector<std::string>& args, MakeOptions& options);
	bool absorbMakeflags(const std::string& value, MakeOptions& options);
	bool absorbMakefileSwitches(const std::string& value, MakeOptions& options);
	std::string composeMakeflags(const MakeOptions& options);

	// make_dump.cpp
	void printDatabase(Make& make);

}  // namespace wbsh::make_detail

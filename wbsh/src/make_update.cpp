/**
 * @file make_update.cpp
 * @brief Bringing targets up to date the way GNU make decides it: depth-first
 *        through the prerequisites, time stamps compared with sub-second
 *        precision, intermediate files only made when something needs them,
 *        double-colon rules, the goal messages, and the remaking of included
 *        makefiles.
 */

#include "make_internal.h"

#include <algorithm>
#include <climits>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "coreutils_internal.h"
#include "interrupt.h"

namespace wbsh::make_detail {

	namespace fs = std::filesystem;

	static const FileTime kNewTime{ true, LLONG_MAX, 0 };

	static const FileTime kOldTime{ true, LLONG_MIN, 0 };

	struct RuleView {
		std::vector<std::string> prereqs;
		std::vector<std::string> order_only;
		const Recipe* recipe = nullptr;
		bool double_colon = false;
	};

	struct DepResult {
		FileNode* file = nullptr;
		bool changed = false;
	};

	struct RuleUpdate {
		FileNode& file;
		const RuleView& view;
		const VarScope* scope;
		int depth;
		FileTime this_time;
		bool noexist = false;
		bool must_make = false;
		UpdateStatus status = UpdateStatus::Success;
		std::vector<DepResult> deps;
	};

	struct FileScope {
		std::vector<VarScope> levels;
		const VarScope* inherited = nullptr;

		const VarScope* top() const {
			return levels.empty() ? inherited : &levels.front();
		}
	};

	static UpdateStatus worst(UpdateStatus a, UpdateStatus b) {
		return static_cast<int>(a) > static_cast<int>(b) ? a : b;
	}

	static FileTime currentTime(Make& make, FileNode& file) {
		if (file.mtime_known) return file.mtime;

		FileTime time = statFile(make, file.name);
		if (!time.exists && !file.phony) {
			const std::string found = searchVpath(make, file.name);
			if (!found.empty()) {
				file.vpath_name = found;
				time = statFile(make, found);
			}
		}

		if (file.phony) time = FileTime{};
		if (file.assume_new) time = kNewTime;
		file.mtime = time;
		file.mtime_known = true;
		return time;
	}

	static void refreshTime(Make& make, FileNode& file, bool simulated) {
		file.vpath_name.clear();
		file.mtime_known = true;
		if (simulated) {
			file.mtime = kNewTime;
			return;
		}

		file.mtime = file.phony ? FileTime{} : statFile(make, file.name);
	}

	static void prepareRule(Make& make, FileNode& file) {
		const bool searchable = !file.phony && !file.recipe.defined && !file.double_colon;
		if (searchable && !file.implicit_searched) {
			file.implicit_searched = true;
			findImplicitRule(make, file, 0);
		}

		if (file.recipe.defined || file.is_target) return;

		const FileNode* fallback = findFile(make, ".DEFAULT");
		if (fallback != nullptr && fallback->recipe.defined) file.recipe = fallback->recipe;
	}

	static bool longerPatternFirst(const PatternVars* a, const PatternVars* b) {
		return a->pattern.size() > b->pattern.size();
	}

	static void collectPatternTables(Make& make, const FileNode& file,
	                                 std::vector<const VarTable*>& tables) {
		std::vector<const PatternVars*> matching;
		std::string stem;
		for (const PatternVars& entry : make.pattern_vars) {
			if (matchPattern(entry.pattern, file.name, stem)) matching.push_back(&entry);
		}

		std::stable_sort(matching.begin(), matching.end(), longerPatternFirst);
		for (const PatternVars* entry : matching) tables.push_back(&entry->vars);
	}

	static void buildScope(Make& make, const FileNode& file, const VarScope* inherited,
	                       FileScope& scope) {
		std::vector<const VarTable*> tables;
		if (!file.vars.empty()) tables.push_back(&file.vars);
		collectPatternTables(make, file, tables);

		scope.inherited = inherited;
		scope.levels.resize(tables.size());
		for (std::size_t i = 0; i < tables.size(); ++i) {
			const VarScope* parent = i + 1 < tables.size() ? &scope.levels[i + 1] : inherited;
			scope.levels[i] = VarScope{ tables[i], parent, &file };
		}
	}

	static RuleView singleColonView(const FileNode& file) {
		RuleView view;
		view.prereqs = file.implicit_prereqs;
		view.prereqs.insert(view.prereqs.end(), file.prereqs.begin(), file.prereqs.end());
		view.order_only = file.implicit_order_only;
		view.order_only.insert(view.order_only.end(), file.order_only.begin(), file.order_only.end());
		view.recipe = &file.recipe;
		return view;
	}

	static void reportCircular(const Make& make, const FileNode& file, const FileNode& dep) {
		reportError(make, "Circular " + file.name + " <- " + dep.name + " dependency dropped.");
	}

	static bool takePrereq(Make& make, RuleUpdate& update, const std::string& name,
	                       FileNode*& dep) {
		dep = &enterFile(make, name);
		if (dep->state == UpdateState::Running) {
			reportCircular(make, update.file, *dep);
			return false;
		}

		if (dep->needed_by.empty()) dep->needed_by = update.file.name;
		return true;
	}

	static bool beatsTarget(const FileTime& dep_time, const FileTime& this_time) {
		return !dep_time.exists || isNewer(dep_time, this_time);
	}

	static UpdateStatus checkIntermediate(Make& make, RuleUpdate& update, FileNode& dep);

	static UpdateStatus checkIntermediatePrereqs(Make& make, RuleUpdate& update, FileNode& dep) {
		UpdateStatus status = UpdateStatus::Success;
		const RuleView view = singleColonView(dep);
		for (const std::string& name : view.prereqs) {
			FileNode* prereq = nullptr;
			if (!takePrereq(make, update, name, prereq)) continue;

			if (prereq->intermediate && !prereq->phony) {
				status = worst(status, checkIntermediate(make, update, *prereq));
			} else {
				status = worst(status, updateFile(make, *prereq, update.scope, update.depth + 1));
				if (beatsTarget(currentTime(make, *prereq), update.this_time)) update.must_make = true;
			}

			if (status == UpdateStatus::Failed && !make.options.keep_going) break;
		}

		return status;
	}

	// A missing intermediate file is only made when something it is made
	// from is newer than the target that needs it.
	static UpdateStatus checkIntermediate(Make& make, RuleUpdate& update, FileNode& dep) {
		prepareRule(make, dep);
		const FileTime time = currentTime(make, dep);
		if (time.exists && isNewer(time, update.this_time)) {
			update.must_make = true;
			return UpdateStatus::Success;
		}

		dep.state = UpdateState::Running;
		const UpdateStatus status = checkIntermediatePrereqs(make, update, dep);
		dep.state = UpdateState::Pending;
		return status;
	}

	static void updateChild(Make& make, RuleUpdate& update, FileNode& child) {
		update.status = worst(update.status, updateFile(make, child, update.scope, update.depth + 1));
	}

	static void updatePrereq(Make& make, RuleUpdate& update, FileNode& dep) {
		const FileTime before = currentTime(make, dep);
		if (dep.intermediate && !dep.phony) {
			update.status = worst(update.status, checkIntermediate(make, update, dep));
		} else {
			updateChild(make, update, dep);
			if (beatsTarget(currentTime(make, dep), update.this_time)) update.must_make = true;
		}

		const FileTime after = currentTime(make, dep);
		update.deps.push_back(DepResult{ &dep, after != before || !before.exists });
	}

	static bool stopOnFailure(const Make& make, const RuleUpdate& update) {
		if (make.fatal || make.interrupted) return true;
		return update.status != UpdateStatus::Success && !make.options.keep_going;
	}

	static void updatePrereqs(Make& make, RuleUpdate& update) {
		for (const std::string& name : update.view.prereqs) {
			FileNode* dep = nullptr;
			if (!takePrereq(make, update, name, dep)) continue;

			updatePrereq(make, update, *dep);
			if (stopOnFailure(make, update)) return;
		}

		for (const std::string& name : update.view.order_only) {
			FileNode* dep = nullptr;
			if (!takePrereq(make, update, name, dep)) continue;

			updateChild(make, update, *dep);
			if (stopOnFailure(make, update)) return;
		}
	}

	static void buildIntermediates(Make& make, RuleUpdate& update) {
		for (DepResult& dep : update.deps) {
			FileNode& file = *dep.file;
			if (!file.intermediate || file.phony || file.state != UpdateState::Pending) continue;

			const FileTime before = currentTime(make, file);
			updateChild(make, update, file);
			dep.changed = currentTime(make, file) != before;
			if (stopOnFailure(make, update)) return;
		}
	}

	static void decideRemake(Make& make, RuleUpdate& update) {
		bool deps_changed = false;
		for (const DepResult& dep : update.deps) {
			const FileTime time = currentTime(make, *dep.file);
			if (!time.exists && !dep.file->intermediate) update.must_make = true;
			deps_changed = deps_changed || dep.changed;
		}

		const bool has_recipe = update.view.recipe->defined;
		const bool always = make.options.always_make;
		if (update.view.double_colon && update.view.prereqs.empty()) update.must_make = true;
		else if (!update.noexist && update.file.is_target && !deps_changed && !has_recipe && !always) {
			update.must_make = false;
		} else if (has_recipe && always) {
			update.must_make = true;
		}
	}

	static std::string prereqName(Make& make, const std::string& name) {
		const FileNode* file = findFile(make, name);
		if (file == nullptr || file->vpath_name.empty()) return name;

		return file->vpath_name;
	}

	static std::string explicitStem(const Make& make, const std::string& name) {
		for (const std::string& suffix : make.suffixes) {
			if (name.size() <= suffix.size()) continue;
			if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
				return name.substr(0, name.size() - suffix.size());
			}
		}

		return {};
	}

	static std::string ruleStem(const Make& make, const FileNode& file) {
		return file.stem.empty() ? explicitStem(make, file.name) : file.stem;
	}

	static void expandSecondPrereqs(Make& make, FileNode& file, const VarScope* scope) {
		const SecondContext context{ file.name, ruleStem(make, file), scope, &file };
		if (hasDeferredPrereqs(file.prereqs) || hasDeferredPrereqs(file.order_only)) {
			expandDeferredPrereqs(make, context, file.prereqs, file.order_only);
		}

		for (DoubleColonRule& rule : file.double_colon_rules) {
			if (!hasDeferredPrereqs(rule.prereqs) && !hasDeferredPrereqs(rule.order_only)) continue;
			expandDeferredPrereqs(make, context, rule.prereqs, rule.order_only);
		}
	}

	static AutoVars buildAutos(Make& make, const RuleUpdate& update) {
		AutoVars autos;
		autos.target = update.file.name;
		autos.stem = ruleStem(make, update.file);
		for (const std::string& name : update.view.prereqs) {
			autos.all_prereqs.push_back(prereqName(make, name));
		}

		autos.prereqs = uniqueWords(autos.all_prereqs);
		for (const std::string& name : update.view.order_only) {
			autos.order_only.push_back(prereqName(make, name));
		}

		autos.order_only = uniqueWords(autos.order_only);
		for (const DepResult& dep : update.deps) {
			const bool newer = dep.changed || update.noexist
				|| isNewer(currentTime(make, *dep.file), update.this_time);
			if (newer) autos.newer.push_back(prereqName(make, dep.file->name));
		}

		autos.newer = uniqueWords(autos.newer);
		return autos;
	}

	static UpdateStatus complainNoRule(Make& make, const FileNode& file) {
		std::string message = "No rule to make target '" + file.name + "'";
		if (!file.needed_by.empty()) message += ", needed by '" + file.needed_by + "'";

		if (make.options.keep_going) reportError(make, "*** " + message + ".");
		else reportFatal(make, Location{}, message);
		return UpdateStatus::Failed;
	}

	static void markSiblingsDone(Make& make, const FileNode& file, UpdateStatus status,
	                             bool simulated) {
		std::vector<std::string> siblings = file.implicit_siblings;
		siblings.insert(siblings.end(), file.group.begin(), file.group.end());
		for (const std::string& name : siblings) {
			if (name == file.name) continue;

			FileNode& sibling = enterFile(make, name);
			sibling.state = UpdateState::Done;
			sibling.status = status;
			refreshTime(make, sibling, simulated);
		}
	}

	static bool simulatesRecipe(const Make& make, const Recipe& recipe) {
		const bool pretending = make.options.dry_run || make.options.question || make.options.touch;
		return pretending && !recipeRecurses(recipe);
	}

	static void noteIntermediateMade(Make& make, const FileNode& file) {
		if (file.intermediate && !file.secondary && !file.precious) {
			make.intermediates_made.push_back(file.name);
		}
	}

	static UpdateStatus runRuleRecipe(Make& make, RuleUpdate& update) {
		const Recipe& recipe = *update.view.recipe;
		const bool recurses = recipeRecurses(recipe);
		if (make.options.question && !recurses) return UpdateStatus::Question;

		if (make.options.touch && !recurses) {
			touchTarget(make, update.file);
			refreshTime(make, update.file, false);
			return UpdateStatus::Success;
		}

		const AutoVars autos = buildAutos(make, update);
		const ExpandContext ctx{ update.scope, &update.file, &autos };
		const UpdateStatus status = runRecipe(make, RecipeJob{ update.file, recipe, ctx });
		refreshTime(make, update.file, simulatesRecipe(make, recipe));
		noteIntermediateMade(make, update.file);
		markSiblingsDone(make, update.file, status, simulatesRecipe(make, recipe));
		return make.options.question ? worst(status, UpdateStatus::Question) : status;
	}

	static UpdateStatus remake(Make& make, RuleUpdate& update) {
		if (update.view.recipe->defined) return runRuleRecipe(make, update);
		if (update.file.phony || update.file.is_target) return UpdateStatus::Success;

		return complainNoRule(make, update.file);
	}

	static UpdateStatus updateRuleView(Make& make, FileNode& file, const RuleView& view,
	                                   const VarScope* scope, int depth) {
		RuleUpdate update{ file, view, scope, depth, currentTime(make, file) };
		update.noexist = !update.this_time.exists || file.phony;
		update.must_make = update.noexist;

		updatePrereqs(make, update);
		if (update.status != UpdateStatus::Success) return update.status;

		decideRemake(make, update);
		if (!update.must_make) return UpdateStatus::Success;

		buildIntermediates(make, update);
		if (update.status != UpdateStatus::Success) return update.status;

		return remake(make, update);
	}

	static UpdateStatus updateDoubleColon(Make& make, FileNode& file, const VarScope* scope,
	                                      int depth) {
		UpdateStatus status = UpdateStatus::Success;
		for (std::size_t i = 0; i < file.double_colon_rules.size(); ++i) {
			const DoubleColonRule rule = file.double_colon_rules[i];
			RuleView view;
			view.prereqs = rule.prereqs;
			view.order_only = rule.order_only;
			view.recipe = &rule.recipe;
			view.double_colon = true;
			status = worst(status, updateRuleView(make, file, view, scope, depth));
			if (status == UpdateStatus::Failed && !make.options.keep_going) break;
		}

		return status;
	}

	UpdateStatus updateFile(Make& make, FileNode& file, const VarScope* inherited, int depth) {
		if (file.state == UpdateState::Done) return file.status;
		if (make.interrupted || ctrlCPending()) {
			make.interrupted = true;
			return UpdateStatus::Failed;
		}

		file.state = UpdateState::Running;
		if (file.assume_old) {
			file.mtime = kOldTime;
			file.mtime_known = true;
			file.state = UpdateState::Done;
			return UpdateStatus::Success;
		}

		FileScope scope;
		buildScope(make, file, inherited, scope);
		expandSecondPrereqs(make, file, scope.top());
		prepareRule(make, file);
		const UpdateStatus status = file.double_colon
			? updateDoubleColon(make, file, scope.top(), depth)
			: updateRuleView(make, file, singleColonView(file), scope.top(), depth);
		file.state = UpdateState::Done;
		file.status = status;
		return status;
	}

	static bool hasAnyRecipe(const FileNode& file) {
		if (file.recipe.defined) return true;
		for (const DoubleColonRule& rule : file.double_colon_rules) {
			if (rule.recipe.defined) return true;
		}

		return false;
	}

	static void reportUpToDate(const Make& make, const FileNode& file) {
		if (make.options.silent || make.options.question) return;

		if (file.phony || !hasAnyRecipe(file)) {
			reportMessage(make, "Nothing to be done for '" + file.name + "'.");
			return;
		}

		reportMessage(make, "'" + file.name + "' is up to date.");
	}

	static int updateGoal(Make& make, const std::string& goal) {
		FileNode& file = enterFile(make, goal);
		const int started = make.commands_started;
		const UpdateStatus status = updateFile(make, file, nullptr, 0);
		if (make.fatal || make.interrupted) return 2;

		if (status == UpdateStatus::Failed) {
			if (make.options.keep_going) {
				reportError(make, "Target '" + goal + "' not remade because of errors.");
			}

			return 2;
		}

		if (status == UpdateStatus::Question) return 1;
		if (started == make.commands_started) reportUpToDate(make, file);
		return 0;
	}

	int updateGoals(Make& make, const std::vector<std::string>& goals) {
		int exit_status = 0;
		for (const std::string& goal : goals) {
			const int status = updateGoal(make, goal);
			if (status > exit_status) exit_status = status;
			if (make.fatal || make.interrupted) return 2;
			if (status == 2 && !make.options.keep_going) break;
			if (status == 1 && !make.options.keep_going) break;
		}

		return exit_status;
	}

	static bool removeQuietly(Make& make, const std::string& name) {
		std::error_code ec;
		return fs::remove(toNative(make.exec, name), ec) && !ec;
	}

	void removeIntermediates(Make& make) {
		if (make.options.question || make.options.touch || make.special.secondary_all) return;

		std::vector<std::string> removed;
		for (const std::string& name : uniqueWords(make.intermediates_made)) {
			const FileNode* file = findFile(make, name);
			if (file == nullptr || file->secondary || file->precious) continue;
			if (!make.options.dry_run && !removeQuietly(make, name)) continue;

			removed.push_back(name);
		}

		if (!removed.empty() && !make.options.silent) printLine("rm " + joinWords(removed));
	}

	static bool hasRule(Make& make, FileNode& file) {
		prepareRule(make, file);
		return file.recipe.defined || file.is_target || file.double_colon;
	}

	static bool reportMissingInclude(Make& make, const IncludedMakefile& include) {
		if (include.optional) return true;

		reportWarning(make, include.where, include.name + ": No such file or directory");
		reportFatal(make, Location{}, "No rule to make target '" + include.name + "'");
		return false;
	}

	// A `::` rule without prerequisites would remake its makefile on every
	// pass, so such a makefile is left alone.
	static bool alwaysRemade(const FileNode& file) {
		for (const DoubleColonRule& rule : file.double_colon_rules) {
			if (rule.prereqs.empty()) return true;
		}

		return false;
	}

	static bool remakeInclude(Make& make, const IncludedMakefile& include, bool& restart) {
		FileNode* known = findFile(make, include.name);
		if (!include.missing && (known == nullptr || !known->is_target)) return true;
		if (known != nullptr && alwaysRemade(*known)) return true;

		FileNode& file = enterFile(make, include.name);
		if (include.missing && !hasRule(make, file)) return reportMissingInclude(make, include);

		const FileTime before = statFile(make, include.name);
		const UpdateStatus status = updateFile(make, file, nullptr, 0);
		if (status == UpdateStatus::Failed && !include.optional) return false;

		const FileTime after = statFile(make, include.name);
		if (after.exists && after != before) restart = true;
		if (include.missing && !after.exists) return reportMissingInclude(make, include);

		return true;
	}

	// GNU make really runs the recipes that remake makefiles, even under
	// -n, -q and -t.
	bool remakeIncludedMakefiles(Make& make, bool& restart) {
		restart = false;
		const MakeOptions saved = make.options;
		make.options.dry_run = false;
		make.options.question = false;
		make.options.touch = false;

		bool ok = true;
		const std::vector<IncludedMakefile> includes = make.includes;
		for (const IncludedMakefile& include : includes) {
			ok = remakeInclude(make, include, restart) && !make.fatal;
			if (!ok) break;
		}

		make.options = saved;
		return ok;
	}

}  // namespace wbsh::make_detail

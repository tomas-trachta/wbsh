/**
 * @file make_implicit.cpp
 * @brief Implicit rules: the pattern rule list, suffix rules turned into
 *        pattern rules, GNU make's two-pass search (shortest stem first,
 *        then chains through intermediate files), VPATH / vpath lookup and
 *        file time stamps.
 */

#include "make_internal.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "coreutils_internal.h"
#include "filetimes.h"

namespace wbsh::make_detail {

	namespace fs = std::filesystem;

	FileTime statFile(Make& make, const std::string& name) {
		FileTime time;
		FileTimes times;
		if (name.empty() || !readFileTimes(toNative(make.exec, name), true, times)) return time;

		time.exists = true;
		time.seconds = times.modify.seconds;
		time.nanos = times.modify.nanos;
		return time;
	}

	bool fileExists(Make& make, const std::string& name) {
		if (name.empty()) return false;

		std::error_code ec;
		return fs::exists(toNative(make.exec, name), ec);
	}

	static bool isAbsoluteName(const std::string& name) {
		if (!name.empty() && (name[0] == '/' || name[0] == '\\')) return true;
		return name.size() > 1 && name[1] == ':';
	}

	// Directories are separated by blanks, `:` or `;`; the colon of a
	// `C:/` drive prefix is part of the directory.
	static std::vector<std::string> splitSearchPath(const std::string& text) {
		std::vector<std::string> dirs;
		std::string current;
		for (std::size_t i = 0; i < text.size(); ++i) {
			const char c = text[i];
			const bool drive_colon = c == ':' && current.size() == 1
				&& std::isalpha(static_cast<unsigned char>(current[0])) != 0
				&& i + 1 < text.size() && (text[i + 1] == '/' || text[i + 1] == '\\');
			const bool separator = !drive_colon && (isBlank(c) || c == ':' || c == ';');
			if (!separator) {
				current.push_back(c);
				continue;
			}

			if (!current.empty()) dirs.push_back(current);
			current.clear();
		}

		if (!current.empty()) dirs.push_back(current);
		return dirs;
	}

	static std::string findInDirs(Make& make, const std::vector<std::string>& dirs,
	                              const std::string& name) {
		for (const std::string& dir : dirs) {
			const std::string candidate = dir.back() == '/' ? dir + name : dir + "/" + name;
			if (fileExists(make, candidate)) return candidate;
		}

		return {};
	}

	std::string searchVpath(Make& make, const std::string& name) {
		if (name.empty() || isAbsoluteName(name)) return {};

		std::string stem;
		for (const VpathEntry& entry : make.vpaths) {
			if (!matchPattern(entry.pattern, name, stem)) continue;

			const std::string found = findInDirs(make, entry.dirs, name);
			if (!found.empty()) return found;
		}

		if (findGlobal(make, "VPATH") == nullptr) return {};

		const std::string vpath = expandVariable(make, "VPATH", ExpandContext{});
		return findInDirs(make, splitSearchPath(vpath), name);
	}

	void applyVpathDirective(Make& make, const std::string& args) {
		const std::vector<std::string> words = splitWords(args);
		if (words.empty()) {
			make.vpaths.clear();
			return;
		}

		const std::string& pattern = words[0];
		if (words.size() == 1) {
			make.vpaths.erase(std::remove_if(make.vpaths.begin(), make.vpaths.end(),
				[&pattern](const VpathEntry& entry) { return entry.pattern == pattern; }),
				make.vpaths.end());
			return;
		}

		const std::string dirs = args.substr(args.find(pattern) + pattern.size());
		make.vpaths.push_back(VpathEntry{ pattern, splitSearchPath(dirs) });
	}

	// A rule without a recipe cancels the same rule defined earlier; one that
	// arrives without override (a converted suffix rule) keeps the old one.
	static void insertPatternRule(Make& make, const PatternRule& rule, bool replace) {
		for (auto it = make.pattern_rules.begin(); it != make.pattern_rules.end(); ++it) {
			if (it->targets != rule.targets || it->prereqs != rule.prereqs) continue;
			if (!replace) return;

			make.pattern_rules.erase(it);
			break;
		}

		make.pattern_rules.push_back(rule);
	}

	void addPatternRule(Make& make, PatternRule rule) {
		insertPatternRule(make, rule, true);
	}

	static void convertOne(Make& make, const std::string& rule_name, const std::string& target,
	                       const std::string& source) {
		const FileNode* file = findFile(make, rule_name);
		if (file == nullptr || !file->recipe.defined || !file->prereqs.empty()) return;

		PatternRule rule;
		rule.targets = { "%" + target };
		rule.prereqs = { "%" + source };
		rule.recipe = file->recipe;
		insertPatternRule(make, rule, false);
	}

	void convertSuffixRules(Make& make) {
		for (const std::string& source : make.suffixes) {
			convertOne(make, source, std::string(), source);
			for (const std::string& target : make.suffixes) {
				convertOne(make, source + target, target, source);
			}
		}
	}

	struct Candidate {
		std::size_t rule = 0;
		std::string target;
		std::string stem;
		std::string dir_prefix;
	};

	static bool hasKnownSuffix(const Make& make, const std::string& name) {
		for (const std::string& suffix : make.suffixes) {
			if (name.size() > suffix.size()
				&& name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
				return true;
			}
		}

		return false;
	}

	static bool matchTarget(const std::string& pattern, const std::string& name,
	                        Candidate& candidate) {
		const bool with_dir = pattern.find('/') != std::string::npos;
		const std::string base = notDirectoryOf(name);
		const std::string& subject = with_dir ? name : base;
		if (!matchPattern(pattern, subject, candidate.stem)) return false;

		candidate.dir_prefix = with_dir ? std::string() : name.substr(0, name.size() - base.size());
		return true;
	}

	struct CandidateScan {
		std::vector<Candidate> candidates;
		std::vector<bool> match_anything;
		bool specific = false;
	};

	static void scanRule(const Make& make, std::size_t index, const std::string& name, int depth,
	                     CandidateScan& scan) {
		const PatternRule& rule = make.pattern_rules[index];
		if (rule.in_use) return;

		for (const std::string& target : rule.targets) {
			const bool anything = target == "%";
			if (depth > 0 && anything && !rule.terminal) continue;

			Candidate candidate;
			candidate.rule = index;
			candidate.target = name;
			if (!matchTarget(target, name, candidate)) continue;

			if (!anything) scan.specific = true;
			if (!rule.recipe.defined) continue;

			scan.candidates.push_back(candidate);
			scan.match_anything.push_back(anything && !rule.terminal);
			return;
		}
	}

	static std::vector<Candidate> collectCandidates(const Make& make, const std::string& name,
	                                                int depth) {
		CandidateScan scan;
		scan.specific = hasKnownSuffix(make, name);
		for (std::size_t i = 0; i < make.pattern_rules.size(); ++i) {
			scanRule(make, i, name, depth, scan);
		}

		std::vector<Candidate> kept;
		for (std::size_t i = 0; i < scan.candidates.size(); ++i) {
			if (scan.specific && scan.match_anything[i]) continue;
			kept.push_back(scan.candidates[i]);
		}

		std::stable_sort(kept.begin(), kept.end(), [](const Candidate& a, const Candidate& b) {
			return a.stem.size() < b.stem.size();
		});
		return kept;
	}

	// As GNU make does, a `%` in a list kept for the second expansion becomes
	// `$*`, which that expansion turns into the stem with its directory.
	static std::string stemReferences(const std::string& entry) {
		std::string replaced;
		for (const char c : entry) {
			if (c == '%') replaced += "$*";
			else replaced.push_back(c);
		}

		return replaced;
	}

	static std::vector<std::string> candidateNames(const Candidate& candidate,
	                                               const std::vector<std::string>& patterns) {
		std::vector<std::string> names;
		for (const std::string& pattern : patterns) {
			if (isDeferredPrereq(pattern)) names.push_back(stemReferences(pattern));
			else if (pattern.find('%') == std::string::npos) names.push_back(pattern);
			else names.push_back(candidate.dir_prefix + substituteStem(pattern, candidate.stem));
		}

		return names;
	}

	struct CandidatePrereqs {
		std::vector<std::string> prereqs;
		std::vector<std::string> order_only;
	};

	static CandidatePrereqs candidatePrereqs(Make& make, const Candidate& candidate) {
		const PatternRule& rule = make.pattern_rules[candidate.rule];
		CandidatePrereqs names{ candidateNames(candidate, rule.prereqs),
			candidateNames(candidate, rule.order_only) };
		if (!hasDeferredPrereqs(names.prereqs) && !hasDeferredPrereqs(names.order_only)) return names;

		const SecondContext context{ candidate.target, candidate.dir_prefix + candidate.stem,
			nullptr, nullptr };
		expandDeferredPrereqs(make, context, names.prereqs, names.order_only);
		return names;
	}

	static bool oughtToExist(Make& make, const std::string& name) {
		const FileNode* file = findFile(make, name);
		if (file != nullptr && (file->is_target || file->mentioned)) return true;

		return fileExists(make, name) || !searchVpath(make, name).empty();
	}

	static void adoptRule(Make& make, FileNode& file, const Candidate& candidate) {
		const PatternRule& rule = make.pattern_rules[candidate.rule];
		const CandidatePrereqs names = candidatePrereqs(make, candidate);
		file.implicit_prereqs = names.prereqs;
		file.implicit_order_only = names.order_only;
		if (!file.recipe.defined) file.recipe = rule.recipe;
		if (file.stem.empty()) file.stem = candidate.dir_prefix + candidate.stem;

		file.implicit_siblings.clear();
		for (const std::string& target : candidateNames(candidate, rule.targets)) {
			if (target != file.name) file.implicit_siblings.push_back(target);
		}
	}

	static void enterIntermediate(Make& make, FileNode& probe) {
		FileNode& file = enterFile(make, probe.name);
		if (file.implicit_searched) return;

		file.implicit_searched = true;
		file.implicit_prereqs = probe.implicit_prereqs;
		file.implicit_order_only = probe.implicit_order_only;
		file.implicit_siblings = probe.implicit_siblings;
		file.recipe = probe.recipe;
		file.stem = probe.stem;
		if (!file.mentioned && !file.not_intermediate) file.intermediate = true;
	}

	static bool chainPrereqs(Make& make, const Candidate& candidate, int depth) {
		PatternRule& rule = make.pattern_rules[candidate.rule];
		if (rule.terminal) return false;

		const std::vector<std::string> prereqs = candidatePrereqs(make, candidate).prereqs;
		std::vector<FileNode> probes;
		rule.in_use = true;
		bool chained = true;
		for (const std::string& prereq : prereqs) {
			if (oughtToExist(make, prereq)) continue;

			FileNode probe;
			probe.name = prereq;
			chained = findImplicitRule(make, probe, depth + 1);
			if (!chained) break;
			probes.push_back(probe);
		}

		make.pattern_rules[candidate.rule].in_use = false;
		if (!chained) return false;

		for (FileNode& probe : probes) enterIntermediate(make, probe);
		return true;
	}

	static bool prereqsReady(Make& make, const Candidate& candidate) {
		for (const std::string& prereq : candidatePrereqs(make, candidate).prereqs) {
			if (!oughtToExist(make, prereq)) return false;
		}

		return true;
	}

	bool findImplicitRule(Make& make, FileNode& file, int depth) {
		const std::vector<Candidate> candidates = collectCandidates(make, file.name, depth);
		for (const Candidate& candidate : candidates) {
			if (!prereqsReady(make, candidate)) continue;

			adoptRule(make, file, candidate);
			return true;
		}

		for (const Candidate& candidate : candidates) {
			if (!chainPrereqs(make, candidate, depth)) continue;

			adoptRule(make, file, candidate);
			return true;
		}

		return false;
	}

}  // namespace wbsh::make_detail

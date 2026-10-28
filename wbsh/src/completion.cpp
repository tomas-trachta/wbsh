/**
 * @file completion.cpp
 * @brief Completion specs: generating their matches, and `complete`, `compgen` and `compopt`.
 */

#include "completion.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "environment.h"
#include "executor.h"
#include "fnmatch.h"
#include "keymap.h"
#include "pathconv.h"

namespace wbsh {

	namespace fs = std::filesystem;

	const char* const kDefaultCompletionName = "_DefaultCmD_";
	const char* const kEmptyCompletionName = "_EmptycmD_";
	const char* const kInitialCompletionName = "_InitialWorD_";

	static bool containsName(const std::vector<std::string>& names, const std::string& name) {
		return std::find(names.begin(), names.end(), name) != names.end();
	}

	bool CompletionSpec::hasOption(const std::string& name) const {
		return containsName(options, name);
	}

	bool CompletionResult::hasOption(const std::string& name) const {
		return containsName(options, name);
	}

	bool completionMatches(const std::string& text, const std::string& prefix) {
		if (text.size() < prefix.size()) return false;
		if (!shellKeymap().isOn("completion-ignore-case")) {
			return text.compare(0, prefix.size(), prefix) == 0;
		}

		for (std::size_t k = 0; k < prefix.size(); ++k) {
			const int left = std::tolower(static_cast<unsigned char>(text[k]));
			const int right = std::tolower(static_cast<unsigned char>(prefix[k]));
			if (left != right) return false;
		}

		return true;
	}

	namespace completion_detail {

		struct ActionName {
			const char* name;
			char letter;
		};

		// In the order `complete -p` prints them: the ones with a letter of
		// their own first.
		static const ActionName kActions[] = {
			{ "alias", 'a' }, { "builtin", 'b' }, { "command", 'c' }, { "directory", 'd' },
			{ "export", 'e' }, { "file", 'f' }, { "group", 'g' }, { "job", 'j' },
			{ "keyword", 'k' }, { "service", 's' }, { "user", 'u' }, { "variable", 'v' },
			{ "arrayvar", 0 }, { "binding", 0 }, { "disabled", 0 }, { "enabled", 0 },
			{ "function", 0 }, { "helptopic", 0 }, { "hostname", 0 }, { "running", 0 },
			{ "setopt", 0 }, { "shopt", 0 }, { "signal", 0 }, { "stopped", 0 },
		};

		static const char* const kOptionNames[] = {
			"bashdefault", "default", "dirnames", "filenames", "noquote", "nosort", "nospace",
			"plusdirs", nullptr,
		};

		static const char* const kKeywords[] = {
			"!", "[[", "]]", "case", "coproc", "do", "done", "elif", "else", "esac", "fi", "for",
			"function", "if", "in", "select", "then", "time", "until", "while", "{", "}", nullptr,
		};

		static const char* const kSignals[] = {
			"SIGHUP", "SIGINT", "SIGQUIT", "SIGILL", "SIGTRAP", "SIGABRT", "SIGBUS", "SIGFPE",
			"SIGKILL", "SIGUSR1", "SIGSEGV", "SIGUSR2", "SIGPIPE", "SIGALRM", "SIGTERM",
			"SIGCHLD", "SIGCONT", "SIGSTOP", "SIGTSTP", "SIGTTIN", "SIGTTOU", "SIGURG",
			"SIGWINCH", nullptr,
		};

		static const char* const kExecutableSuffixes[] = {
			".exe", ".cmd", ".bat", ".com", nullptr,
		};

		static const char* const kCompleteUsage =
			"complete [-abcdefgjksuv] [-pr] [-DEI] [-o option] [-A action] [-G globpat] "
			"[-W wordlist] [-F function] [-C command] [-X filterpat] [-P prefix] [-S suffix] "
			"[name ...]";
		static const char* const kCompgenUsage =
			"compgen [-abcdefgjksuv] [-o option] [-A action] [-G globpat] [-W wordlist] "
			"[-F function] [-C command] [-X filterpat] [-P prefix] [-S suffix] [word]";
		static const char* const kCompoptUsage = "compopt [-o|+o option] [-DEI] [name ...]";

		// The -F function running now, whose options `compopt` without a
		// name changes.
		static std::vector<std::string>* g_running_options = nullptr;

		static const ActionName* actionNamed(const std::string& name) {
			for (const ActionName& action : kActions) {
				if (name == action.name) return &action;
			}

			return nullptr;
		}

		static const ActionName* actionLettered(char letter) {
			for (const ActionName& action : kActions) {
				if (action.letter == letter) return &action;
			}

			return nullptr;
		}

		static bool isOptionName(const std::string& name) {
			for (int i = 0; kOptionNames[i] != nullptr; ++i) {
				if (name == kOptionNames[i]) return true;
			}

			return false;
		}

		static void addUnique(std::vector<std::string>& names, const std::string& name) {
			if (!containsName(names, name)) names.push_back(name);
		}

		static void appendMatching(std::vector<std::string>& out,
		                           const std::vector<std::string>& names,
		                           const std::string& word) {
			for (const std::string& name : names) {
				if (completionMatches(name, word)) out.push_back(name);
			}
		}

		static std::vector<std::string> fromTable(const char* const* table) {
			std::vector<std::string> names;
			for (int i = 0; table[i] != nullptr; ++i) names.emplace_back(table[i]);
			return names;
		}

		template <typename Map>
		static std::vector<std::string> keysOf(const Map& map) {
			std::vector<std::string> keys;
			for (const auto& entry : map) keys.push_back(entry.first);
			return keys;
		}

	}  // namespace completion_detail

	std::vector<std::string> shellKeywords() {
		return completion_detail::fromTable(completion_detail::kKeywords);
	}

	namespace completion_detail {

		static std::string withoutExecutableSuffix(const std::string& name) {
			for (int i = 0; kExecutableSuffixes[i] != nullptr; ++i) {
				const std::size_t length = std::strlen(kExecutableSuffixes[i]);
				if (name.size() <= length) continue;

				std::string tail = name.substr(name.size() - length);
				for (char& letter : tail) {
					letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
				}

				if (tail == kExecutableSuffixes[i]) return name.substr(0, name.size() - length);
			}

			return name;
		}

		static std::vector<std::string> pathCommands(Executor& exec) {
			std::vector<std::string> names;
			for (const std::string& dir : splitPathList(exec.env().get("PATH"))) {
				if (dir.empty()) continue;

				std::error_code ec;
				fs::directory_iterator entries(utf8ToPath(exec.pathConv().toWin32(dir)), ec);
				if (ec) continue;

				for (const auto& entry : entries) {
					names.push_back(withoutExecutableSuffix(pathToUtf8(entry.path().filename())));
				}
			}

			return names;
		}

		static std::vector<std::string> commandNames(Executor& exec) {
			std::vector<std::string> names = keysOf(exec.aliases());
			const std::vector<std::string> builtins = exec.builtinNames();
			const std::vector<std::string> functions = exec.functionNames();
			const std::vector<std::string> keywords = shellKeywords();
			const std::vector<std::string> path = pathCommands(exec);
			for (const auto* list : { &builtins, &functions, &keywords, &path }) {
				names.insert(names.end(), list->begin(), list->end());
			}

			return names;
		}

		static std::vector<std::string> variableNames(const Environment& env, bool exported_only) {
			std::vector<std::string> names;
			for (const auto& entry : env.vars()) {
				if (!exported_only || env.isExported(entry.first)) names.push_back(entry.first);
			}

			if (exported_only) return names;

			for (const std::string& name : keysOf(env.indexedArrays())) names.push_back(name);
			for (const std::string& name : keysOf(env.assocArrays())) names.push_back(name);
			return names;
		}

		static std::vector<std::string> arrayNames(const Environment& env) {
			std::vector<std::string> names = keysOf(env.indexedArrays());
			for (const std::string& name : keysOf(env.assocArrays())) names.push_back(name);
			return names;
		}

		static std::vector<std::string> jobNames(Executor& exec, bool running_only) {
			std::vector<std::string> names;
			for (const Executor::Job& job : exec.jobsTable()) {
				if (running_only && !job.running) continue;

				const std::size_t space = job.cmd_text.find(' ');
				names.push_back(job.cmd_text.substr(0, space));
			}

			return names;
		}

		static std::vector<std::string> optionNames(bool shopt) {
			std::vector<std::string> names;
			if (shopt) {
				for (const ShoptSpec& spec : shoptTable()) names.emplace_back(spec.name);
			} else {
				for (const SetOptionSpec& spec : setOptionTable()) names.emplace_back(spec.name);
			}

			return names;
		}

		static std::vector<std::string> oneName(const std::string& name) {
			return name.empty() ? std::vector<std::string>() : std::vector<std::string>{ name };
		}

	}  // namespace completion_detail

	namespace completion_detail {

		static bool hiddenUnlessAsked(const std::string& name, const std::string& base) {
			return !name.empty() && name[0] == '.' && (base.empty() || base[0] != '.');
		}

		static std::string expandedDir(Executor& exec, const std::string& dir) {
			const std::string home = exec.env().get("HOME");
			if (dir == "~" && !home.empty()) return home;
			if (dir.compare(0, 2, "~/") == 0 && !home.empty()) return home + dir.substr(1);

			return dir;
		}

		// The files and directories that complete `word`, as bash's
		// compgen -f / -d lists them: with the directory part as typed and
		// no slash after a directory.
		static std::vector<std::string> fileNames(Executor& exec, const std::string& word,
		                                          bool directories_only) {
			const std::size_t slash = word.find_last_of('/');
			const std::string dir = slash == std::string::npos ? "." : word.substr(0, slash + 1);
			const std::string base = slash == std::string::npos ? word : word.substr(slash + 1);
			const std::string typed = slash == std::string::npos ? "" : dir;

			std::vector<std::string> names;
			std::error_code ec;
			const fs::path native = utf8ToPath(exec.pathConv().toWin32(expandedDir(exec, dir)));
			fs::directory_iterator entries(native, ec);
			if (ec) return names;

			for (const auto& entry : entries) {
				const std::string name = pathToUtf8(entry.path().filename());
				if (!completionMatches(name, base) || hiddenUnlessAsked(name, base)) continue;

				std::error_code type_ec;
				if (directories_only && !entry.is_directory(type_ec)) continue;

				names.push_back(typed + name);
			}

			return names;
		}

		static std::vector<std::string> globMatches(Executor& exec, const std::string& pattern) {
			const std::size_t slash = pattern.find_last_of('/');
			const std::string dir = slash == std::string::npos ? "." : pattern.substr(0, slash + 1);
			const std::string base =
				slash == std::string::npos ? pattern : pattern.substr(slash + 1);
			const std::string typed = slash == std::string::npos ? "" : dir;

			std::vector<std::string> names;
			std::error_code ec;
			const fs::path native = utf8ToPath(exec.pathConv().toWin32(expandedDir(exec, dir)));
			fs::directory_iterator entries(native, ec);
			if (ec) return names;

			for (const auto& entry : entries) {
				const std::string name = pathToUtf8(entry.path().filename());
				if (hiddenUnlessAsked(name, base) || !fnmatchFull(base, name)) continue;

				names.push_back(typed + name);
			}

			return names;
		}

	}  // namespace completion_detail

	namespace completion_detail {

		static std::vector<std::string> actionCandidates(Executor& exec, const std::string& action,
		                                                 const std::string& word) {
			const Environment& env = exec.env();
			if (action == "alias")     return keysOf(exec.aliases());
			if (action == "arrayvar")  return arrayNames(env);
			if (action == "binding")   return editingCommandNames();
			if (action == "command")   return commandNames(exec);
			if (action == "directory") return fileNames(exec, word, true);
			if (action == "export")    return variableNames(env, true);
			if (action == "file")      return fileNames(exec, word, false);
			if (action == "function")  return exec.functionNames();
			if (action == "hostname")  return oneName(env.get("COMPUTERNAME"));
			if (action == "job")       return jobNames(exec, false);
			if (action == "keyword")   return shellKeywords();
			if (action == "running")   return jobNames(exec, true);
			if (action == "setopt")    return optionNames(false);
			if (action == "shopt")     return optionNames(true);
			if (action == "signal")    return fromTable(kSignals);
			if (action == "user")      return oneName(env.get("USERNAME"));
			if (action == "variable")  return variableNames(env, false);

			if (action == "disabled") {
				return { exec.disabledBuiltins().begin(), exec.disabledBuiltins().end() };
			}

			const bool builtins = action == "builtin" || action == "enabled"
				|| action == "helptopic";
			return builtins ? exec.builtinNames() : std::vector<std::string>();
		}

		static std::vector<std::string> splitLines(const std::string& text) {
			std::vector<std::string> lines;
			std::size_t start = 0;
			while (start < text.size()) {
				std::size_t end = text.find('\n', start);
				if (end == std::string::npos) end = text.size();

				std::string line = text.substr(start, end - start);
				if (!line.empty() && line.back() == '\r') line.pop_back();
				if (!line.empty()) lines.push_back(std::move(line));
				start = end + 1;
			}

			return lines;
		}

		static std::vector<std::string> splitBlanks(const std::string& text) {
			std::vector<std::string> words;
			std::string current;
			for (const char letter : text) {
				if (letter != ' ' && letter != '\t' && letter != '\n') {
					current.push_back(letter);
					continue;
				}

				if (!current.empty()) words.push_back(current);
				current.clear();
			}

			if (!current.empty()) words.push_back(current);
			return words;
		}

		static std::string singleQuoted(const std::string& text) {
			std::string quoted = "'";
			for (const char letter : text) {
				if (letter == '\'') {
					quoted += "'\\''";
				} else {
					quoted.push_back(letter);
				}
			}

			return quoted + "'";
		}

		// The word list is expanded as bash does -- parameters, command
		// substitution, arithmetic, but no file names -- and split into words.
		static std::vector<std::string> wordList(Executor& exec, const std::string& words) {
			if (words.find_first_of("$`~{\\\"'") == std::string::npos) return splitBlanks(words);

			return splitLines(exec.runRaw("set -f; printf '%s\\n' " + words));
		}

		static std::vector<std::string> replyOf(const Environment& env) {
			std::vector<std::string> reply;
			const auto* array = env.getIndexedArray("COMPREPLY");
			if (array == nullptr) return oneName(env.get("COMPREPLY"));

			for (const auto& entry : *array) reply.push_back(entry.second);
			return reply;
		}

		struct RunningOptions {
			explicit RunningOptions(std::vector<std::string>& options) : outer(g_running_options) {
				g_running_options = &options;
			}

			~RunningOptions() { g_running_options = outer; }

			RunningOptions(const RunningOptions&) = delete;
			RunningOptions& operator=(const RunningOptions&) = delete;

			std::vector<std::string>* outer;
		};

		// A function calling exit / break / return must not tear the
		// interactive shell down, so any signal it leaves is dropped.
		static std::vector<std::string> functionReply(Executor& exec,
		                                              const CompletionRequest& request,
		                                              const std::string& function,
		                                              CompletionResult& result) {
			if (function.empty() || !exec.isFunction(function)) return {};

			exec.env().unset("COMPREPLY");
			const RunningOptions running(result.options);
			const int saved_status = exec.lastStatus();
			result.function_status =
				exec.callFunction(function, { request.command, request.word, request.previous });
			exec.clearFlow();
			if (exec.expander().failed()) exec.expander().takeError();
			exec.setLastStatus(saved_status);

			return replyOf(exec.env());
		}

		static std::vector<std::string> commandOutput(Executor& exec,
		                                              const CompletionRequest& request,
		                                              const std::string& command) {
			if (command.empty()) return {};

			const std::string line = command + " " + singleQuoted(request.command) + " "
				+ singleQuoted(request.word) + " " + singleQuoted(request.previous);
			return splitLines(exec.runRaw(line));
		}

		static std::string filterPattern(const std::string& filter, const std::string& word) {
			std::string pattern;
			for (std::size_t i = 0; i < filter.size(); ++i) {
				if (filter[i] == '\\' && i + 1 < filter.size() && filter[i + 1] == '&') {
					pattern.push_back('&');
					++i;
				} else if (filter[i] == '&') {
					for (const char letter : word) {
						if (isFnmatchSpecial(letter)) pattern.push_back('\\');
						pattern.push_back(letter);
					}
				} else {
					pattern.push_back(filter[i]);
				}
			}

			return pattern;
		}

		// -X removes the matches the pattern matches -- or, after a leading
		// `!`, those it does not.
		static void applyFilter(const std::string& filter, const std::string& word,
		                        std::vector<std::string>& matches) {
			if (filter.empty()) return;

			const bool keep_matching = filter[0] == '!';
			const std::string body = keep_matching ? filter.substr(1) : filter;
			const std::string pattern = filterPattern(body, word);
			const auto removed = [&](const std::string& match) {
				return fnmatchFull(pattern, match) != keep_matching;
			};
			matches.erase(std::remove_if(matches.begin(), matches.end(), removed), matches.end());
		}

		static void applyAffixes(const CompletionSpec& spec, std::vector<std::string>& matches) {
			if (spec.prefix.empty() && spec.suffix.empty()) return;

			for (std::string& match : matches) match = spec.prefix + match + spec.suffix;
		}

		static void sortUnique(std::vector<std::string>& names) {
			std::sort(names.begin(), names.end());
			names.erase(std::unique(names.begin(), names.end()), names.end());
		}

	}  // namespace completion_detail

	namespace completion_detail {

		static void appendGenerated(Executor& exec, const CompletionSpec& spec,
		                            const std::string& word, CompletionResult& result) {
			for (const std::string& action : spec.actions) {
				appendMatching(result.matches, actionCandidates(exec, action, word), word);
				if (action == "file" || action == "directory") result.file_names = true;
			}

			if (!spec.glob.empty()) {
				appendMatching(result.matches, globMatches(exec, spec.glob), word);
				result.file_names = true;
			}

			if (spec.words.empty()) return;

			appendMatching(result.matches, wordList(exec, spec.words), word);
		}

		// -o plusdirs adds directories; -o dirnames, default and bashdefault
		// fall back to directories or files when nothing else matched.
		static void applyFallbacks(Executor& exec, const std::string& word,
		                           CompletionResult& result) {
			if (result.hasOption("plusdirs")) {
				appendMatching(result.matches, fileNames(exec, word, true), word);
				result.file_names = true;
			}

			if (!result.matches.empty()) return;

			const bool files = result.hasOption("default") || result.hasOption("bashdefault");
			if (!files && !result.hasOption("dirnames")) return;

			result.matches = fileNames(exec, word, !files);
			result.file_names = true;
		}

	}  // namespace completion_detail

	CompletionResult generateCompletions(Executor& exec, const CompletionSpec& spec,
	                                     const CompletionRequest& request) {
		CompletionResult result;
		result.options = spec.options;
		completion_detail::appendGenerated(exec, spec, request.word, result);

		std::vector<std::string> reply =
			completion_detail::functionReply(exec, request, spec.function, result);
		result.matches.insert(result.matches.end(), reply.begin(), reply.end());
		reply = completion_detail::commandOutput(exec, request, spec.command);
		result.matches.insert(result.matches.end(), reply.begin(), reply.end());

		completion_detail::applyFilter(spec.filter, request.word, result.matches);
		completion_detail::applyAffixes(spec, result.matches);
		completion_detail::applyFallbacks(exec, request.word, result);
		if (result.hasOption("filenames")) result.file_names = true;
		if (!result.hasOption("nosort")) completion_detail::sortUnique(result.matches);
		return result;
	}

	std::string completionSpecText(const std::string& name, const CompletionSpec& spec) {
		std::string text = "complete";
		for (int i = 0; completion_detail::kOptionNames[i] != nullptr; ++i) {
			if (spec.hasOption(completion_detail::kOptionNames[i])) {
				text += std::string(" -o ") + completion_detail::kOptionNames[i];
			}
		}

		for (const completion_detail::ActionName& action : completion_detail::kActions) {
			if (!containsName(spec.actions, action.name)) continue;

			text += action.letter != 0 ? std::string(" -") + action.letter
			                           : std::string(" -A ") + action.name;
		}

		const std::pair<const char*, const std::string*> quoted[] = {
			{ " -G ", &spec.glob }, { " -W ", &spec.words }, { " -P ", &spec.prefix },
			{ " -S ", &spec.suffix }, { " -X ", &spec.filter }, { " -C ", &spec.command },
		};
		for (const auto& entry : quoted) {
			if (entry.second->empty()) continue;

			text += entry.first + completion_detail::singleQuoted(*entry.second);
		}

		if (!spec.function.empty()) text += " -F " + spec.function;
		if (name == kDefaultCompletionName) return text + " -D";
		if (name == kEmptyCompletionName) return text + " -E";
		if (name == kInitialCompletionName) return text + " -I";

		return text + " " + name;
	}

	std::string serializeCompletionSpecs(const Executor& exec) {
		std::vector<std::string> names;
		for (const auto& entry : exec.completionSpecs()) names.push_back(entry.first);
		std::sort(names.begin(), names.end());

		std::string text;
		for (const std::string& name : names) {
			text += completionSpecText(name, *exec.completionSpec(name)) + "\n";
		}

		return text;
	}

	namespace completion_detail {

		enum class Builtin { kComplete, kCompgen, kCompopt };

		struct ParsedArgs {
			CompletionSpec spec;
			std::vector<std::string> names;
			std::vector<std::string> unset_options;
			bool print = false;
			bool remove = false;
			bool default_spec = false;
			bool empty_spec = false;
			bool initial_spec = false;
		};

		static const char* builtinName(Builtin builtin) {
			switch (builtin) {
			case Builtin::kCompgen: return "compgen";
			case Builtin::kCompopt: return "compopt";
			default:                return "complete";
			}
		}

		static const char* usageOf(Builtin builtin) {
			switch (builtin) {
			case Builtin::kCompgen: return kCompgenUsage;
			case Builtin::kCompopt: return kCompoptUsage;
			default:                return kCompleteUsage;
			}
		}

		static int usageError(Builtin builtin) {
			std::fprintf(stderr, "%s: usage: %s\n", builtinName(builtin), usageOf(builtin));
			return 2;
		}

		static int invalidName(Builtin builtin, const std::string& name, const char* kind) {
			std::fprintf(stderr, "wbsh: %s: %s: invalid %s name\n", builtinName(builtin),
				name.c_str(), kind);
			return 2;
		}

		static int fail(Builtin builtin, const std::string& message, bool usage) {
			std::fprintf(stderr, "wbsh: %s: %s\n", builtinName(builtin), message.c_str());
			if (usage) usageError(builtin);
			return 2;
		}

		static bool takesValue(char letter) {
			return std::strchr("oAGWFCXPS", letter) != nullptr;
		}

		static bool allowedFlag(Builtin builtin, char letter) {
			if (builtin == Builtin::kCompopt) return std::strchr("oDEI", letter) != nullptr;
			if (std::strchr("pr", letter) != nullptr) return builtin == Builtin::kComplete;
			if (std::strchr("DEI", letter) != nullptr) return builtin == Builtin::kComplete;

			return actionLettered(letter) != nullptr || takesValue(letter);
		}

		static int setValue(Builtin builtin, char letter, const std::string& value,
		                    ParsedArgs& parsed) {
			CompletionSpec& spec = parsed.spec;
			switch (letter) {
			case 'o':
				if (!isOptionName(value)) return invalidName(builtin, value, "option");
				addUnique(spec.options, value);
				return 0;
			case 'A':
				if (actionNamed(value) == nullptr) return invalidName(builtin, value, "action");
				addUnique(spec.actions, value);
				return 0;
			case 'G': spec.glob = value;     return 0;
			case 'W': spec.words = value;    return 0;
			case 'F': spec.function = value; return 0;
			case 'C': spec.command = value;  return 0;
			case 'X': spec.filter = value;   return 0;
			case 'P': spec.prefix = value;   return 0;
			case 'S': spec.suffix = value;   return 0;
			default:  return 0;
			}
		}

		static void setFlag(char letter, ParsedArgs& parsed) {
			switch (letter) {
			case 'p': parsed.print = true;        break;
			case 'r': parsed.remove = true;       break;
			case 'D': parsed.default_spec = true; break;
			case 'E': parsed.empty_spec = true;   break;
			case 'I': parsed.initial_spec = true; break;
			default:
				addUnique(parsed.spec.actions, actionLettered(letter)->name);
				break;
			}
		}

		// One `-abc` / `-W words` / `+o option` argument; `i` moves past a
		// value taken from the next argument.
		static int parseOptionArg(Builtin builtin, const std::vector<std::string>& args,
		                          std::size_t& i, ParsedArgs& parsed) {
			const std::string& arg = args[i];
			const bool plus = arg[0] == '+';
			for (std::size_t k = 1; k < arg.size(); ++k) {
				const char letter = arg[k];
				const std::string spelled = std::string(1, arg[0]) + letter;
				const bool allowed = plus ? builtin == Builtin::kCompopt && letter == 'o'
				                          : allowedFlag(builtin, letter);
				if (!allowed) return fail(builtin, spelled + ": invalid option", true);

				if (!takesValue(letter)) {
					setFlag(letter, parsed);
					continue;
				}

				std::string value = arg.substr(k + 1);
				if (value.empty() && i + 1 >= args.size()) {
					return fail(builtin, spelled + ": option requires an argument", true);
				}

				if (value.empty()) value = args[++i];
				if (plus) {
					if (!isOptionName(value)) return invalidName(builtin, value, "option");
					addUnique(parsed.unset_options, value);
					return 0;
				}

				return setValue(builtin, letter, value, parsed);
			}

			return 0;
		}

		static int parseArgs(Builtin builtin, const std::vector<std::string>& args,
		                     ParsedArgs& parsed) {
			bool options_done = false;
			for (std::size_t i = 0; i < args.size(); ++i) {
				const std::string& arg = args[i];
				if (!options_done && arg == "--") {
					options_done = true;
					continue;
				}

				const bool signed_arg = arg[0] == '-' || arg[0] == '+';
				const bool option = !options_done && arg.size() > 1 && signed_arg;
				if (!option) {
					parsed.names.push_back(arg);
					continue;
				}

				const int status = parseOptionArg(builtin, args, i, parsed);
				if (status != 0) return status;
			}

			return 0;
		}

		static std::vector<std::string> targetNames(const ParsedArgs& parsed) {
			std::vector<std::string> names = parsed.names;
			if (parsed.default_spec) names.push_back(kDefaultCompletionName);
			if (parsed.empty_spec) names.push_back(kEmptyCompletionName);
			if (parsed.initial_spec) names.push_back(kInitialCompletionName);
			return names;
		}

		static bool anySpecPart(const CompletionSpec& spec) {
			return !spec.actions.empty() || !spec.options.empty() || !spec.glob.empty()
			    || !spec.words.empty() || !spec.function.empty() || !spec.command.empty()
			    || !spec.filter.empty() || !spec.prefix.empty() || !spec.suffix.empty();
		}

	}  // namespace completion_detail

	namespace completion_detail {

		static int noSpec(Builtin builtin, const std::string& name) {
			std::fprintf(stderr, "wbsh: %s: %s: no completion specification\n",
				builtinName(builtin), name.c_str());
			return 1;
		}

		static int printSpecs(Executor& exec, const std::vector<std::string>& names) {
			if (names.empty()) {
				std::fputs(serializeCompletionSpecs(exec).c_str(), stdout);
				return 0;
			}

			int status = 0;
			for (const std::string& name : names) {
				const CompletionSpec* spec = exec.completionSpec(name);
				if (spec == nullptr) {
					status = noSpec(Builtin::kComplete, name);
					continue;
				}

				std::printf("%s\n", completionSpecText(name, *spec).c_str());
			}

			return status;
		}

		static int removeSpecs(Executor& exec, const std::vector<std::string>& names) {
			if (names.empty()) {
				for (const std::string& name : keysOf(exec.completionSpecs())) {
					exec.removeCompletionSpec(name);
				}

				return 0;
			}

			int status = 0;
			for (const std::string& name : names) {
				if (exec.completionSpec(name) == nullptr) {
					status = noSpec(Builtin::kComplete, name);
					continue;
				}

				exec.removeCompletionSpec(name);
			}

			return status;
		}

		static int completeBuiltin(Executor& exec, const std::vector<std::string>& args) {
			ParsedArgs parsed;
			const int status = parseArgs(Builtin::kComplete, args, parsed);
			if (status != 0) return status;

			const std::vector<std::string> names = targetNames(parsed);
			if (parsed.print || args.empty()) return printSpecs(exec, names);
			if (parsed.remove) return removeSpecs(exec, names);
			if (names.empty()) {
				if (anySpecPart(parsed.spec)) return usageError(Builtin::kComplete);

				return printSpecs(exec, names);
			}

			for (const std::string& name : names) exec.setCompletionSpec(name, parsed.spec);
			return 0;
		}

		static int compgenBuiltin(Executor& exec, const std::vector<std::string>& args) {
			ParsedArgs parsed;
			const int status = parseArgs(Builtin::kCompgen, args, parsed);
			if (status != 0) return status;

			CompletionRequest request;
			if (!parsed.names.empty()) request.word = parsed.names.back();

			const CompletionResult result = generateCompletions(exec, parsed.spec, request);
			for (const std::string& match : result.matches) std::printf("%s\n", match.c_str());
			return result.matches.empty() ? 1 : 0;
		}

		static void changeOptions(std::vector<std::string>& options, const ParsedArgs& parsed) {
			for (const std::string& name : parsed.spec.options) addUnique(options, name);
			for (const std::string& name : parsed.unset_options) {
				options.erase(std::remove(options.begin(), options.end(), name), options.end());
			}
		}

		static void printOptions(const std::vector<std::string>& options, const std::string& name) {
			std::string text = "compopt";
			for (int i = 0; kOptionNames[i] != nullptr; ++i) {
				text += containsName(options, kOptionNames[i]) ? " -o " : " +o ";
				text += kOptionNames[i];
			}

			if (!name.empty()) text += " " + name;
			std::printf("%s\n", text.c_str());
		}

		static bool changesOptions(const ParsedArgs& parsed) {
			return !parsed.spec.options.empty() || !parsed.unset_options.empty();
		}

		static int compoptRunning(const ParsedArgs& parsed) {
			if (g_running_options == nullptr) {
				std::fputs("wbsh: compopt: not currently executing completion function\n", stderr);
				return 1;
			}

			if (!changesOptions(parsed)) printOptions(*g_running_options, "");
			changeOptions(*g_running_options, parsed);
			return 0;
		}

		static int compoptBuiltin(Executor& exec, const std::vector<std::string>& args) {
			ParsedArgs parsed;
			const int status = parseArgs(Builtin::kCompopt, args, parsed);
			if (status != 0) return status;

			const std::vector<std::string> names = targetNames(parsed);
			if (names.empty()) return compoptRunning(parsed);

			int result = 0;
			for (const std::string& name : names) {
				const CompletionSpec* found = exec.completionSpec(name);
				if (found == nullptr) {
					result = noSpec(Builtin::kCompopt, name);
					continue;
				}

				CompletionSpec spec = *found;
				if (!changesOptions(parsed)) printOptions(spec.options, name);
				changeOptions(spec.options, parsed);
				exec.setCompletionSpec(name, spec);
			}

			return result;
		}

	}  // namespace completion_detail

	void registerCompletionBuiltins(Executor& exec) {
		exec.registerBuiltin("complete", completion_detail::completeBuiltin);
		exec.registerBuiltin("compgen",  completion_detail::compgenBuiltin);
		exec.registerBuiltin("compopt",  completion_detail::compoptBuiltin);
	}

}  // namespace wbsh

/**
 * @file lineedit_complete.cpp
 * @brief Tab completion: the word at the cursor, commands, paths, per-tool
 *        and programmable completions, and how a match goes into the line.
 */

#include "lineedit.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "completion.h"
#include "keymap.h"
#include "numparse.h"
#include "pathconv.h"
#include "strscan.h"
#include "textcells.h"
#include "utils.h"

namespace wbsh {

	namespace fs = std::filesystem;

	static bool isSpaceByte(char letter) {
		return std::isspace(static_cast<unsigned char>(letter)) != 0;
	}

	static bool hasPrefix(const std::string& text, const std::string& prefix) {
		return text.compare(0, prefix.size(), prefix) == 0;
	}

	static void stripLineEnding(std::string& line) {
		while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
	}

	static const int kCompletionRetryStatus = 124;

	static bool ignoresCase() {
		return shellKeymap().isOn("completion-ignore-case");
	}

	static char foldedCase(char letter) {
		return static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
	}

	static bool sameLetter(char left, char right, bool ignore_case) {
		return ignore_case ? foldedCase(left) == foldedCase(right) : left == right;
	}

	// readline's completion-ignore-case, on unless an inputrc turns it off:
	// Windows file and command names ignore case.
	static bool matchesPrefix(const std::string& text, const std::string& prefix) {
		return completionMatches(text, prefix);
	}

	// Where the command being completed starts and which words it has so
	// far, the way bash's programmable completion splits the line: quotes,
	// $(...), ${...} and `...` stay inside one word, and the command
	// starts after the last ; | & ( or { -- not after the & of a 2>&1 --
	// and after any NAME=value assignments in front of it.
	namespace complete_detail {

		struct CommandWords {
			std::size_t start = 0;
			std::vector<std::string> words;
		};

		static std::size_t skipQuoted(const std::string& text, std::size_t i, std::size_t end) {
			const char quote = text[i];
			for (++i; i < end; ++i) {
				if (text[i] == quote) return i + 1;
				if (quote == '"' && text[i] == '\\') ++i;
			}

			return end;
		}

		static std::size_t skipGroup(const std::string& text, std::size_t i, std::size_t end) {
			const char open = text[i];
			const char close = open == '(' ? ')' : '}';
			int depth = 0;
			while (i < end) {
				const char letter = text[i];
				if (letter == '\'' || letter == '"') {
					i = skipQuoted(text, i, end);
					continue;
				}

				if (letter == open) ++depth;
				if (letter == close && --depth == 0) return i + 1;
				i += letter == '\\' ? 2 : 1;
			}

			return end;
		}

		static std::size_t skipBackquoted(const std::string& text, std::size_t i, std::size_t end) {
			for (++i; i < end; ++i) {
				if (text[i] == '`') return i + 1;
				if (text[i] == '\\') ++i;
			}

			return end;
		}

		static bool opensGroup(const std::string& text, std::size_t i, std::size_t end) {
			return text[i] == '$' && i + 1 < end && (text[i + 1] == '(' || text[i + 1] == '{');
		}

		// One quoted string, expansion or escaped character as a unit.
		static std::size_t skipUnit(const std::string& text, std::size_t i, std::size_t end) {
			const char letter = text[i];
			if (letter == '\\') return std::min<std::size_t>(i + 2, end);
			if (letter == '\'' || letter == '"') return skipQuoted(text, i, end);
			if (letter == '`') return skipBackquoted(text, i, end);
			if (opensGroup(text, i, end)) return skipGroup(text, i + 1, end);

			return i + 1;
		}

		static bool isRedirectionAmpersand(const std::string& text, std::size_t i) {
			const bool after_redirect = i > 0 && (text[i - 1] == '>' || text[i - 1] == '<');
			const bool before_redirect = i + 1 < text.size() && text[i + 1] == '>';
			return after_redirect || before_redirect;
		}

		static bool isStandaloneBrace(const std::string& text, std::size_t i) {
			const bool word_start = i == 0 || isSpaceByte(text[i - 1]);
			const bool word_end = i + 1 >= text.size() || isSpaceByte(text[i + 1]);
			return word_start && word_end;
		}

		static bool separatesCommands(const std::string& text, std::size_t i) {
			switch (text[i]) {
			case ';':
			case '|':
			case '(':
			case '\n':
				return true;
			case '&':
				return !isRedirectionAmpersand(text, i);
			case '{':
				return isStandaloneBrace(text, i);
			default:
				return false;
			}
		}

		static bool isNameByte(char letter, bool first) {
			const auto byte = static_cast<unsigned char>(letter);
			if (letter == '_' || std::isalpha(byte) != 0) return true;

			return !first && std::isdigit(byte) != 0;
		}

		static bool isAssignmentWord(const std::string& word) {
			std::size_t i = 0;
			while (i < word.size() && isNameByte(word[i], i == 0)) ++i;
			if (i == 0 || i >= word.size()) return false;

			if (word[i] == '[') {
				const std::size_t close = word.find(']', i);
				if (close == std::string::npos) return false;

				i = close + 1;
			}

			if (i < word.size() && word[i] == '+') ++i;
			return i < word.size() && word[i] == '=';
		}

		struct WordSpan {
			std::size_t start;
			std::size_t end;
		};

		static std::vector<WordSpan> wordSpansBefore(const std::string& text, std::size_t end) {
			std::vector<WordSpan> spans;
			std::size_t word_start = std::string::npos;
			std::size_t i = 0;
			while (i < end) {
				const bool breaks = isSpaceByte(text[i]) || separatesCommands(text, i);
				if (breaks && word_start != std::string::npos) {
					spans.push_back({ word_start, i });
					word_start = std::string::npos;
				}

				if (separatesCommands(text, i)) spans.clear();
				if (breaks) {
					++i;
					continue;
				}

				if (word_start == std::string::npos) word_start = i;
				i = skipUnit(text, i, end);
			}

			if (word_start != std::string::npos) spans.push_back({ word_start, end });
			return spans;
		}

		static CommandWords commandWordsBefore(const std::string& text, std::size_t end) {
			const std::vector<WordSpan> spans = wordSpansBefore(text, end);

			CommandWords command;
			command.start = end;
			for (const WordSpan& span : spans) {
				std::string word = text.substr(span.start, span.end - span.start);
				if (command.words.empty() && isAssignmentWord(word)) continue;

				if (command.words.empty()) command.start = span.start;
				command.words.push_back(std::move(word));
			}

			return command;
		}

		static std::size_t wordEndAfter(const std::string& text, std::size_t from) {
			std::size_t i = from;
			while (i < text.size() && !isSpaceByte(text[i]) && !separatesCommands(text, i)) {
				i = skipUnit(text, i, text.size());
			}

			return std::min<std::size_t>(i, text.size());
		}

		// COMP_WORDS: the words before the cursor's word, that whole word,
		// and the words after it up to the end of the command.
		static std::vector<std::string> wordsOfCommand(const std::string& text,
		                                               const std::vector<std::string>& before,
		                                               std::size_t word_start,
		                                               std::size_t word_end,
		                                               std::size_t command_end) {
			std::vector<std::string> words = before;
			words.push_back(text.substr(word_start, word_end - word_start));
			for (const WordSpan& span : wordSpansBefore(text, command_end)) {
				if (span.start < word_end) continue;

				words.push_back(text.substr(span.start, span.end - span.start));
			}

			return words;
		}

		static std::size_t commandEndAfter(const std::string& text, std::size_t from) {
			std::size_t i = from;
			while (i < text.size() && !separatesCommands(text, i)) {
				i = skipUnit(text, i, text.size());
			}

			return std::min<std::size_t>(i, text.size());
		}

		static bool isWordBreak(char letter) {
			return isSpaceByte(letter)
			    || letter == '|' || letter == '&' || letter == ';'
			    || letter == '<' || letter == '>'
			    || letter == '(' || letter == ')';
		}

		// The word that ends at the cursor, read as the shell reads it.
		struct WordScan {
			std::size_t start = 0;
			std::size_t value_start = 0;
			bool assigned = false;
			char quote = 0;
			std::string word;
			std::string value;
		};

		static void appendLetter(WordScan& scan, char letter) {
			scan.word.push_back(letter);
			scan.value.push_back(letter);
		}

		// After an unquoted `=` -- and after a `:` once there was one, as in
		// PATH=/a:/b -- the completion starts afresh, as with bash's
		// COMP_WORDBREAKS.
		static bool startsValue(const WordScan& scan, char letter) {
			return letter == '=' || (letter == ':' && scan.assigned);
		}

		static void scanUnquoted(const std::string& text, std::size_t& i, std::size_t end,
		                         WordScan& scan) {
			const char letter = text[i];
			if (isWordBreak(letter)) {
				scan = WordScan();
				scan.start = i + 1;
				scan.value_start = i + 1;
				return;
			}

			if (letter == '\\' && i + 1 < end) {
				appendLetter(scan, text[++i]);
			} else if (letter == '\'' || letter == '"') {
				scan.quote = letter;
			} else {
				appendLetter(scan, letter);
			}

			if (!startsValue(scan, letter)) return;

			scan.assigned = true;
			scan.value_start = i + 1;
			scan.value.clear();
		}

		static void scanQuoted(const std::string& text, std::size_t& i, std::size_t end,
		                       WordScan& scan) {
			const char letter = text[i];
			if (letter == scan.quote) {
				scan.quote = 0;
				return;
			}

			const bool escapes = scan.quote == '"' && letter == '\\' && i + 1 < end
				&& std::strchr("$`\"\\\n", text[i + 1]) != nullptr;
			appendLetter(scan, escapes ? text[++i] : letter);
		}

		static WordScan scanWordBefore(const std::string& text, std::size_t end) {
			WordScan scan;
			for (std::size_t i = 0; i < end; ++i) {
				if (scan.quote != 0) {
					scanQuoted(text, i, end, scan);
				} else {
					scanUnquoted(text, i, end, scan);
				}
			}

			return scan;
		}

		static bool inCommandPosition(const std::string& text, std::size_t start) {
			std::size_t before = start;
			while (before > 0 && isSpaceByte(text[before - 1])) --before;
			if (before == 0) return true;

			const char prev = text[before - 1];
			return prev == '|' || prev == '&' || prev == ';' || prev == '(' || prev == '{'
				|| commandWordsBefore(text, start).words.empty();
		}

	}  // namespace complete_detail

	// Quotes and backslashes are taken out of the word, and in an unquoted
	// `name=value` or `--option=value` only the value is completed.
	LineEditor::Tok LineEditor::currentToken() const {
		const complete_detail::WordScan scan = complete_detail::scanWordBefore(buffer_, cursor_);

		Tok token;
		token.start = scan.start;
		token.end = cursor_;
		token.insert_start = scan.assigned ? scan.value_start : scan.start;
		token.quote = scan.quote;
		token.word = scan.assigned ? scan.value : scan.word;
		token.first = !scan.assigned && complete_detail::inCommandPosition(buffer_, scan.start);
		return token;
	}

	static bool endsWithLowered(const std::string& name, const char* suffix) {
		const std::size_t length = std::strlen(suffix);
		if (name.size() <= length) return false;

		const std::size_t offset = name.size() - length;
		for (std::size_t k = 0; k < length; ++k) {
			const int lowered = std::tolower(static_cast<unsigned char>(name[offset + k]));
			if (lowered != suffix[k]) return false;
		}

		return true;
	}

	static std::string stripExeExtension(std::string name) {
		static const char* const kExtensions[] = { ".exe", ".cmd", ".bat", ".com", nullptr };

		for (int i = 0; kExtensions[i] != nullptr; ++i) {
			if (!endsWithLowered(name, kExtensions[i])) continue;

			name.resize(name.size() - std::strlen(kExtensions[i]));
			break;
		}

		return name;
	}

	const std::set<std::string>& LineEditor::pathCommandNames() {
		const std::string path = env_.get("PATH");
		if (path == path_command_cache_path_) return path_command_cache_;

		path_command_cache_.clear();
		for (const std::string& dir : splitPathList(path)) {
			if (dir.empty()) continue;

			std::error_code ec;
			const fs::path base = utf8ToPath(exec_.pathConv().toWin32(dir));
			fs::directory_iterator entries(base, ec);
			if (ec) continue;

			for (const auto& entry : entries) {
				const std::string name = pathToUtf8(entry.path().filename());
				path_command_cache_.insert(stripExeExtension(name));
			}
		}

		path_command_cache_path_ = path;
		return path_command_cache_;
	}

	template <typename Names>
	static void insertWithPrefix(std::set<std::string>& into, const Names& names,
	                             const std::string& prefix) {
		for (const auto& name : names) {
			if (matchesPrefix(name, prefix)) into.insert(name);
		}
	}

	std::vector<std::string> LineEditor::commandCompletions(const std::string& prefix) {
		std::set<std::string> names;
		for (const auto& alias : exec_.aliases()) {
			if (matchesPrefix(alias.first, prefix)) names.insert(alias.first);
		}

		insertWithPrefix(names, shellKeywords(), prefix);
		insertWithPrefix(names, exec_.builtinNames(), prefix);
		insertWithPrefix(names, exec_.functionNames(), prefix);
		insertWithPrefix(names, pathCommandNames(), prefix);
		return std::vector<std::string>(names.begin(), names.end());
	}

	static bool startsWithTilde(const std::string& dir) {
		if (dir == "~") return true;

		return dir.size() >= 2 && dir[0] == '~' && (dir[1] == '/' || dir[1] == '\\');
	}

	static std::string expandTildeDir(const std::string& dir, const std::string& home) {
		if (!startsWithTilde(dir) || home.empty()) return dir;
		if (dir.size() == 1) return home;

		return home + dir.substr(1);
	}

	static bool hiddenUnlessAsked(const std::string& name, const std::string& base) {
		return name[0] == '.' && (base.empty() || base[0] != '.');
	}

	std::vector<std::string> LineEditor::pathCompletions(const std::string& prefix) {
		filename_matches_ = true;
		const std::size_t slash = prefix.find_last_of("/\\");
		std::string dir = ".";
		std::string base = prefix;
		if (slash != std::string::npos) {
			dir = prefix.substr(0, slash);
			if (dir.empty()) dir = "/";
			base = prefix.substr(slash + 1);
		}

		const std::string lookup_dir = expandTildeDir(dir, exec_.env().get("HOME"));
		const fs::path native = utf8ToPath(exec_.pathConv().toWin32(lookup_dir));

		std::vector<std::string> matches;
		std::error_code ec;
		fs::directory_iterator entries(native, ec);
		if (ec) return matches;

		for (const auto& entry : entries) {
			const std::string name = pathToUtf8(entry.path().filename());
			if (name.empty()) continue;
			if (!matchesPrefix(name, base)) continue;
			if (hiddenUnlessAsked(name, base)) continue;

			std::string match = slash == std::string::npos
				? name : prefix.substr(0, slash + 1) + name;
			std::error_code type_ec;
			if (entry.is_directory(type_ec)) match.push_back('/');
			matches.push_back(std::move(match));
		}

		std::sort(matches.begin(), matches.end());
		return matches;
	}

	// The -E spec answers on an empty line, the -I spec for any command
	// word; without them command names are completed.
	std::vector<std::string> LineEditor::commandWordCompletions(const std::string& prefix,
	                                                            const Tok& tok) {
		const bool empty_line = buffer_.find_first_not_of(" \t") == std::string::npos;
		const CompletionSpec* empty =
			empty_line ? exec_.completionSpec(kEmptyCompletionName) : nullptr;
		const CompletionSpec* spec =
			empty != nullptr ? empty : exec_.completionSpec(kInitialCompletionName);
		if (spec == nullptr) return commandCompletions(prefix);

		const CompletionSpec copy = *spec;
		return specCompletions(copy, prefix, tok, {});
	}

	std::vector<std::string> LineEditor::completionsFor(const std::string& prefix,
	                                                    bool command_pos) {
		filename_matches_ = false;
		completion_nospace_ = false;
		spec_answered_ = false;
		const Tok current = currentToken();
		if (command_pos && prefix.find('/') == std::string::npos) {
			return commandWordCompletions(prefix, current);
		}

		const std::vector<std::string> tools = toolCompletions(prefix, current);
		if (!tools.empty() || spec_answered_) return tools;

		return pathCompletions(prefix);
	}

	std::vector<std::string> LineEditor::prevTokensBefore(const Tok& tok) const {
		return complete_detail::commandWordsBefore(buffer_, tok.start).words;
	}

	static void publishCompWords(Environment& env, const std::vector<std::string>& comp_words,
	                             std::size_t cword) {
		std::map<long long, std::string> indexed;
		for (std::size_t i = 0; i < comp_words.size(); ++i) {
			indexed[static_cast<long long>(i)] = comp_words[i];
		}

		env.setIndexedArraySparse("COMP_WORDS", std::move(indexed));
		env.set("COMP_CWORD", std::to_string(cword));
	}

	static void publishCompLine(Environment& env, const std::string& line, std::size_t point) {
		env.set("COMP_LINE", line);
		env.set("COMP_POINT", std::to_string(point));
		env.unset("COMPREPLY");
	}

	// COMP_LINE, COMP_POINT, COMP_WORDS and COMP_CWORD for the command the
	// cursor is in, as a -F function or -C command reads them.
	void LineEditor::publishCompletionContext(const Tok& tok,
	                                          const std::vector<std::string>& prev) {
		Environment& env = exec_.env();

		const std::size_t start = complete_detail::commandWordsBefore(buffer_, tok.start).start;
		const std::size_t end = complete_detail::commandEndAfter(buffer_, tok.end);
		const std::size_t word_end = complete_detail::wordEndAfter(buffer_, tok.end);
		const std::vector<std::string> words =
			complete_detail::wordsOfCommand(buffer_, prev, tok.start, word_end, end);
		publishCompWords(env, words, prev.size());
		publishCompLine(env, buffer_.substr(start, end - start), tok.end - start);
	}

	void LineEditor::markDirectories(std::vector<std::string>& matches) const {
		const std::string home = exec_.env().get("HOME");
		for (std::string& match : matches) {
			if (match.empty() || match.back() == '/') continue;

			std::error_code ec;
			const std::string path = expandTildeDir(match, home);
			const fs::path native = utf8ToPath(exec_.pathConv().toWin32(path));
			if (fs::is_directory(native, ec)) match.push_back('/');
		}
	}

	std::vector<std::string> LineEditor::specCompletions(const CompletionSpec& spec,
	                                                     const std::string& prefix, const Tok& tok,
	                                                     const std::vector<std::string>& prev) {
		publishCompletionContext(tok, prev);
		CompletionRequest request;
		request.command = prev.empty() ? std::string() : prev.front();
		request.word = prefix;
		request.previous = prev.empty() ? std::string() : prev.back();

		const CompletionResult result = generateCompletions(exec_, spec, request);
		spec_answered_ = true;
		last_completion_status_ = result.function_status;
		filename_matches_ = result.file_names && !result.hasOption("noquote");
		completion_nospace_ = result.hasOption("nospace");

		std::vector<std::string> matches = result.matches;
		if (result.file_names) markDirectories(matches);
		return matches;
	}

	// A spec is looked up by the command as typed and by its last path
	// component, as bash does.
	const CompletionSpec* LineEditor::specFor(const std::string& command) const {
		const CompletionSpec* spec = exec_.completionSpec(command);
		const std::size_t slash = command.find_last_of('/');
		if (spec != nullptr || slash == std::string::npos) return spec;

		return exec_.completionSpec(command.substr(slash + 1));
	}

	// The -D spec covers commands without one of their own. Its function
	// returning 124 means it has just defined that spec -- bash-completion
	// loads them on demand that way -- which then gets the word instead.
	std::vector<std::string> LineEditor::defaultSpecCompletions(
		const std::string& prefix, const Tok& tok, const std::vector<std::string>& prev) {
		const CompletionSpec* fallback = exec_.completionSpec(kDefaultCompletionName);
		if (fallback == nullptr) return {};

		const CompletionSpec spec = *fallback;
		std::vector<std::string> matches = specCompletions(spec, prefix, tok, prev);
		const CompletionSpec* loaded = specFor(prev.front());
		if (last_completion_status_ != kCompletionRetryStatus || loaded == nullptr) return matches;

		const CompletionSpec retry = *loaded;
		return specCompletions(retry, prefix, tok, prev);
	}

	std::vector<std::string> LineEditor::toolCompletions(const std::string& prefix,
	                                                     const Tok& tok) {
		const std::vector<std::string> prev = prevTokensBefore(tok);
		if (prev.empty()) return {};

		const std::string& head = prev.front();
		const CompletionSpec* spec = specFor(head);
		if (spec != nullptr) {
			const CompletionSpec copy = *spec;
			return specCompletions(copy, prefix, tok, prev);
		}

		const PluginCommand* plugin = exec_.pluginCommand(head);
		if (plugin != nullptr && plugin->complete != nullptr) {
			return pluginCompletions(*plugin, prefix, prev);
		}

		std::vector<std::string> matches = builtinToolCompletions(head, prefix, prev);
		if (!matches.empty()) return matches;

		return defaultSpecCompletions(prefix, tok, prev);
	}

	std::vector<std::string> LineEditor::builtinToolCompletions(
		const std::string& head, const std::string& prefix,
		const std::vector<std::string>& prev) {
		if (head == "git")     return gitCompletions(prefix, prev);
		if (head == "docker")  return dockerCompletions(prefix, prev);
		if (head == "npm")     return npmCompletions(prefix, prev);
		if (head == "cargo")   return cargoCompletions(prefix, prev);
		if (head == "kubectl" || head == "k") return kubectlCompletions(prefix, prev);
		return {};
	}

	// A util is asked the way a program sees its arguments: the words so
	// far with the one being typed last, and it answers through the SDK's
	// add function into the list handed to it here.
	std::vector<std::string> LineEditor::pluginCompletions(const PluginCommand& plugin,
	                                                       const std::string& prefix,
	                                                       const std::vector<std::string>& prev) {
		std::vector<const char*> argv;
		argv.reserve(prev.size() + 2);
		for (const std::string& word : prev) argv.push_back(word.c_str());
		argv.push_back(prefix.c_str());
		argv.push_back(nullptr);

		WbshCompletion completion;
		plugin.complete(plugin.user, static_cast<int>(argv.size()) - 1, argv.data(), &completion);

		std::vector<std::string> matches;
		for (const std::string& candidate : completion.items) {
			if (matchesPrefix(candidate, prefix)) matches.push_back(candidate);
		}

		return matches;
	}

	static void appendLooseRefs(const fs::path& common_dir, const std::string& kind,
	                            std::vector<std::string>& names) {
		std::error_code ec;
		const fs::path root = common_dir / utf8ToPath(kind);
		if (!fs::is_directory(root, ec)) return;

		fs::recursive_directory_iterator it(root, ec);
		if (ec) return;

		for (auto entry = it; entry != fs::recursive_directory_iterator(); entry.increment(ec)) {
			if (ec) break;

			std::error_code file_ec;
			if (!entry->is_regular_file(file_ec)) continue;

			std::string relative = pathToUtf8(fs::relative(entry->path(), root, file_ec));
			std::replace(relative.begin(), relative.end(), '\\', '/');
			names.push_back(std::move(relative));
		}
	}

	static void appendPackedRefs(const fs::path& common_dir, const std::string& kind,
	                             std::vector<std::string>& names) {
		const std::string prefix = kind + "/";
		std::ifstream packed(common_dir / "packed-refs");
		std::string line;
		while (std::getline(packed, line)) {
			if (line.empty() || line[0] == '#' || line[0] == '^') continue;

			const std::size_t space = line.find(' ');
			if (space == std::string::npos) continue;

			std::string ref = line.substr(space + 1);
			stripLineEnding(ref);
			if (hasPrefix(ref, prefix)) names.push_back(ref.substr(prefix.size()));
		}
	}

	static void sortUnique(std::vector<std::string>& names) {
		std::sort(names.begin(), names.end());
		names.erase(std::unique(names.begin(), names.end()), names.end());
	}


	// A reftable repository keeps no ref files to read, so git itself is
	// asked; it runs through the shell so a `git` function can stand in.
	void LineEditor::appendRefsFromGit(const std::string& kind, std::vector<std::string>& names) {
		const int saved_status = exec_.lastStatus();
		const std::string listing =
			exec_.run("git for-each-ref '--format=%(refname)' " + kind + " 2>/dev/null");
		exec_.setLastStatus(saved_status);

		const std::string prefix = kind + "/";
		std::size_t start = 0;
		while (start < listing.size()) {
			std::size_t end = listing.find('\n', start);
			if (end == std::string::npos) end = listing.size();

			std::string ref = listing.substr(start, end - start);
			stripLineEnding(ref);
			if (hasPrefix(ref, prefix)) names.push_back(ref.substr(prefix.size()));
			start = end + 1;
		}
	}

	std::vector<std::string> LineEditor::gitRefs(const std::string& kind) {
		std::vector<std::string> names;
		const fs::path gitdir = findGitDir();
		if (gitdir.empty()) return names;

		const fs::path common = findGitCommonDir(gitdir);
		if (usesRefTable(common)) {
			appendRefsFromGit(kind, names);
		} else {
			appendLooseRefs(common, kind, names);
			appendPackedRefs(common, kind, names);
		}

		sortUnique(names);
		return names;
	}

	std::vector<std::string> LineEditor::gitBranches() {
		return gitRefs("refs/heads");
	}

	static void appendAll(std::vector<std::string>& into, const std::vector<std::string>& names) {
		into.insert(into.end(), names.begin(), names.end());
	}

	static bool isRemoteHead(const std::string& name) {
		const std::string suffix = "/HEAD";
		return name.size() >= suffix.size()
		    && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
	}

	// `git checkout foo` / `git switch foo` create a local branch from the
	// one remote branch called foo, so those names are offered too -- only
	// when exactly one remote has them, as git requires.
	static std::vector<std::string> guessedBranches(const std::vector<std::string>& remote_refs) {
		std::map<std::string, int> seen;
		for (const std::string& ref : remote_refs) {
			const std::size_t slash = ref.find('/');
			if (slash == std::string::npos || isRemoteHead(ref)) continue;

			++seen[ref.substr(slash + 1)];
		}

		std::vector<std::string> names;
		for (const auto& entry : seen) {
			if (entry.second == 1) names.push_back(entry.first);
		}

		return names;
	}

	std::vector<std::string> LineEditor::gitRefTargets(bool tags_and_remotes, bool guesses) {
		std::vector<std::string> names = gitBranches();
		const std::vector<std::string> remote_refs = gitRefs("refs/remotes");
		if (tags_and_remotes) {
			appendAll(names, gitRefs("refs/tags"));
			appendAll(names, remote_refs);
		}

		if (guesses) appendAll(names, guessedBranches(remote_refs));
		sortUnique(names);
		return names;
	}

	std::vector<std::string> LineEditor::gitRemotes() {
		std::vector<std::string> remotes;
		const fs::path gitdir = findGitDir();
		if (gitdir.empty()) return remotes;

		std::ifstream config(findGitCommonDir(gitdir) / "config");
		std::string line;
		while (std::getline(config, line)) {
			StrScan in(line);
			in.skipSpaces();

			std::string name;
			if (!in.consume("[remote \"") || !in.readUpTo('"', name)) continue;

			remotes.push_back(std::move(name));
		}

		sortUnique(remotes);
		return remotes;
	}

	static std::vector<std::string> filterPrefix(const char* const* table,
	                                             const std::string& prefix) {
		std::vector<std::string> out;
		for (int i = 0; table[i] != nullptr; ++i) {
			std::string word = table[i];
			if (matchesPrefix(word, prefix)) out.push_back(std::move(word));
		}

		return out;
	}

	static std::vector<std::string> filterPrefix(std::vector<std::string> words,
	                                             const std::string& prefix) {
		std::vector<std::string> out;
		for (std::string& word : words) {
			if (matchesPrefix(word, prefix)) out.push_back(std::move(word));
		}

		return out;
	}

	static bool tableContains(const char* const* table, const std::string& word) {
		for (int i = 0; table[i] != nullptr; ++i) {
			if (word == table[i]) return true;
		}

		return false;
	}

	static std::size_t countNonFlagArgs(const std::vector<std::string>& prev, std::size_t from) {
		std::size_t count = 0;
		for (std::size_t i = from; i < prev.size(); ++i) {
			if (!prev[i].empty() && prev[i][0] != '-') ++count;
		}

		return count;
	}

	std::vector<std::string> LineEditor::gitCompletions(const std::string& prefix,
	                                                    const std::vector<std::string>& prev) {
		if (prev.size() == 1) {
			static const char* const kSubs[] = {
				"add", "am", "apply", "archive", "bisect", "blame", "branch",
				"checkout", "cherry-pick", "clean", "clone", "commit", "config",
				"describe", "diff", "fetch", "format-patch", "grep", "init",
				"log", "ls-files", "ls-tree", "merge", "mv", "pull", "push",
				"rebase", "reflog", "remote", "reset", "restore", "revert",
				"rm", "show", "stash", "status", "submodule", "switch", "tag",
				"worktree",
				nullptr,
			};
			return filterPrefix(kSubs, prefix);
		}

		const std::string& sub = prev[1];
		if (sub == "pull" || sub == "push" || sub == "fetch") {
			const bool wants_remote = countNonFlagArgs(prev, 2) == 0;
			return filterPrefix(wants_remote ? gitRemotes() : gitBranches(), prefix);
		}

		if (sub == "checkout") return filterPrefix(gitRefTargets(true, true), prefix);
		if (sub == "switch")   return filterPrefix(gitRefTargets(false, true), prefix);
		if (sub == "branch")   return filterPrefix(gitBranches(), prefix);

		static const char* const kRefSubs[] = {
			"merge", "rebase", "reset", "diff", "log", "show", "cherry-pick", "revert",
			nullptr,
		};
		if (!tableContains(kRefSubs, sub)) return {};

		return filterPrefix(gitRefTargets(true, false), prefix);
	}

	static std::vector<std::string> dockerSecondLevelCompletions(
		const std::string& sub, const std::string& prefix) {
		if (sub == "container") {
			static const char* const kSubs[] = {
				"attach", "commit", "cp", "create", "diff", "exec",
				"export", "inspect", "kill", "logs", "ls", "pause", "port",
				"prune", "rename", "restart", "rm", "run", "start",
				"stats", "stop", "top", "unpause", "update", "wait",
				nullptr,
			};
			return filterPrefix(kSubs, prefix);
		}

		if (sub == "image") {
			static const char* const kSubs[] = {
				"build", "history", "import", "inspect", "load", "ls",
				"prune", "pull", "push", "rm", "save", "tag", nullptr,
			};
			return filterPrefix(kSubs, prefix);
		}

		if (sub == "network") {
			static const char* const kSubs[] = {
				"connect", "create", "disconnect", "inspect", "ls",
				"prune", "rm", nullptr,
			};
			return filterPrefix(kSubs, prefix);
		}

		if (sub == "volume") {
			static const char* const kSubs[] = {
				"create", "inspect", "ls", "prune", "rm", nullptr,
			};
			return filterPrefix(kSubs, prefix);
		}

		if (sub == "compose") {
			static const char* const kSubs[] = {
				"build", "config", "create", "down", "events", "exec",
				"images", "kill", "logs", "ls", "pause", "port", "ps",
				"pull", "push", "restart", "rm", "run", "start", "stop",
				"top", "unpause", "up", "version", nullptr,
			};
			return filterPrefix(kSubs, prefix);
		}

		return {};
	}

	std::vector<std::string> LineEditor::dockerCompletions(const std::string& prefix,
	                                                       const std::vector<std::string>& prev) {
		if (prev.size() == 1) {
			static const char* const kSubs[] = {
				"attach", "build", "builder", "buildx", "commit", "compose",
				"container", "context", "cp", "create", "diff", "events",
				"exec", "export", "history", "image", "images", "import",
				"info", "inspect", "kill", "load", "login", "logout", "logs",
				"manifest", "network", "node", "pause", "plugin", "port", "ps",
				"pull", "push", "rename", "restart", "rm", "rmi", "run",
				"save", "search", "secret", "service", "stack", "start",
				"stats", "stop", "swarm", "system", "tag", "top", "trust",
				"unpause", "update", "version", "volume", "wait",
				nullptr,
			};
			return filterPrefix(kSubs, prefix);
		}

		if (prev.size() == 2) return dockerSecondLevelCompletions(prev[1], prefix);

		return {};
	}

	// Tolerant scan of package.json: the names of the "scripts" object's
	// keys, up to its first '}'. Good enough for completions; not JSON.
	static std::vector<std::string> packageJsonScripts() {
		std::error_code ec;
		std::ifstream file(fs::current_path(ec) / "package.json");
		if (!file) return {};

		const std::string json((std::istreambuf_iterator<char>(file)),
		                       std::istreambuf_iterator<char>());
		StrScan in(json);
		if (!in.skipPast("\"scripts\"") || !in.skipPast("{") || !in.stopAt('}')) return {};

		std::vector<std::string> names;
		std::string name;
		std::string value;
		while (in.readQuoted(name) && in.skipPast(":") && in.readQuoted(value)) {
			names.push_back(name);
		}

		return names;
	}

	std::vector<std::string> LineEditor::npmCompletions(const std::string& prefix,
	                                                    const std::vector<std::string>& prev) {
		if (prev.size() == 1) {
			static const char* const kSubs[] = {
				"access", "adduser", "audit", "bin", "bugs", "cache", "ci",
				"completion", "config", "dedupe", "deprecate", "diff",
				"dist-tag", "doctor", "edit", "exec", "explain", "explore",
				"find-dupes", "fund", "get", "help", "hook", "i", "init",
				"install", "install-ci-test", "install-test", "link", "ll",
				"login", "logout", "ls", "org", "outdated", "owner", "pack",
				"ping", "pkg", "prefix", "profile", "prune", "publish",
				"query", "rebuild", "repo", "restart", "root", "run",
				"run-script", "search", "set", "shrinkwrap", "star", "stars",
				"start", "stop", "team", "test", "token", "uninstall",
				"unpublish", "unstar", "update", "version", "view", "whoami",
				nullptr,
			};
			return filterPrefix(kSubs, prefix);
		}

		if (prev.size() == 2 && (prev[1] == "run" || prev[1] == "run-script")) {
			std::vector<std::string> scripts = filterPrefix(packageJsonScripts(), prefix);
			std::sort(scripts.begin(), scripts.end());
			return scripts;
		}

		return {};
	}

	std::vector<std::string> LineEditor::cargoCompletions(const std::string& prefix,
	                                                      const std::vector<std::string>& prev) {
		if (prev.size() != 1) return {};

		static const char* const kSubs[] = {
			"add", "bench", "build", "check", "clean", "clippy", "config",
			"doc", "fetch", "fix", "fmt", "generate-lockfile", "help",
			"init", "install", "locate-project", "login", "logout",
			"metadata", "new", "owner", "package", "pkgid", "publish",
			"read-manifest", "remove", "report", "run", "rustc", "rustdoc",
			"search", "test", "tree", "uninstall", "update", "vendor",
			"verify-project", "version", "yank",
			nullptr,
		};
		return filterPrefix(kSubs, prefix);
	}

	static std::vector<std::string> kubectlResourceCompletions(const std::string& verb,
	                                                           const std::string& prefix) {
		static const char* const kResources[] = {
			"pods", "po", "services", "svc", "deployments", "deploy",
			"replicasets", "rs", "statefulsets", "sts", "daemonsets", "ds",
			"jobs", "cronjobs", "cj", "nodes", "no", "namespaces", "ns",
			"configmaps", "cm", "secrets", "ingresses", "ing",
			"persistentvolumes", "pv", "persistentvolumeclaims", "pvc",
			"serviceaccounts", "sa", "roles", "rolebindings",
			"clusterroles", "clusterrolebindings", "events", "ev",
			"endpoints", "ep",
			nullptr,
		};
		static const char* const kVerbs[] = {
			"get", "describe", "delete", "edit", "label", "annotate",
			"patch", "scale", "rollout", "logs", "exec", "port-forward",
			"top", "wait", "set", "expose", "autoscale",
			nullptr,
		};
		if (!tableContains(kVerbs, verb)) return {};

		return filterPrefix(kResources, prefix);
	}

	std::vector<std::string> LineEditor::kubectlCompletions(const std::string& prefix,
	                                                        const std::vector<std::string>& prev) {
		if (prev.size() == 1) {
			static const char* const kSubs[] = {
				"alpha", "annotate", "api-resources", "api-versions", "apply",
				"attach", "auth", "autoscale", "certificate", "cluster-info",
				"completion", "config", "convert", "cordon", "cp", "create",
				"debug", "delete", "describe", "diff", "drain", "edit",
				"events", "exec", "explain", "expose", "get", "kustomize",
				"label", "logs", "options", "patch", "plugin", "port-forward",
				"proxy", "replace", "rollout", "run", "scale", "set", "taint",
				"top", "uncordon", "version", "wait",
				nullptr,
			};
			return filterPrefix(kSubs, prefix);
		}

		if (prev.size() == 2) return kubectlResourceCompletions(prev[1], prefix);

		return {};
	}

	std::string LineEditor::longestCommonPrefix(const std::vector<std::string>& words) {
		if (words.empty()) return {};

		const bool ignore_case = ignoresCase();
		std::string prefix = words.front();
		for (std::size_t i = 1; i < words.size(); ++i) {
			const std::string& word = words[i];
			const std::size_t limit = std::min<std::size_t>(prefix.size(), word.size());
			std::size_t k = 0;
			while (k < limit && sameLetter(prefix[k], word[k], ignore_case)) ++k;

			prefix.resize(k);
			if (prefix.empty()) break;
		}

		return prefix;
	}

	namespace complete_detail {

		static const char* const kShellSpecials = " \t\n\\\"'<>;|&()#$`?*[]!{}";
		static const char* const kDoubleQuoteSpecials = "\"\\$`";

		static std::string escaped(const std::string& text, const char* specials) {
			std::string out;
			for (const char letter : text) {
				if (std::strchr(specials, letter) != nullptr) out.push_back('\\');
				out.push_back(letter);
			}

			return out;
		}

		static std::string singleQuoteBody(const std::string& text) {
			std::string out;
			for (const char letter : text) {
				if (letter == '\'') {
					out += "'\\''";
				} else {
					out.push_back(letter);
				}
			}

			return out;
		}

		// A match goes into the line quoted the way its word already is:
		// inside an open '...' or "...", or -- for file names -- with
		// backslashes before what the shell would read specially.
		static std::string quotedForLine(const std::string& text, char quote, bool file_name) {
			if (quote == '\'') return "'" + singleQuoteBody(text);
			if (quote == '"') return "\"" + escaped(text, kDoubleQuoteSpecials);
			if (!file_name) return text;

			return escaped(text, kShellSpecials);
		}

		// A file's matches are listed by their last path component.
		static std::string shownName(const std::string& match) {
			const bool directory = !match.empty() && match.back() == '/';
			const std::size_t end = directory ? match.size() - 1 : match.size();
			if (end == 0) return match;

			const std::size_t slash = match.find_last_of('/', end - 1);
			return slash == std::string::npos ? match : match.substr(slash + 1);
		}

		static std::vector<std::string> shownNames(const std::vector<std::string>& matches) {
			std::vector<std::string> names;
			for (const std::string& match : matches) names.push_back(shownName(match));
			return names;
		}

		static bool extendsWord(const std::string& common, const std::string& word) {
			return common.size() > word.size() || (common.size() == word.size() && common != word);
		}

	}  // namespace complete_detail

	static std::size_t cellsOf(const std::string& text) {
		return displayWidth(text, text.size());
	}

	static std::size_t longestLength(const std::vector<std::string>& matches) {
		std::size_t longest = 0;
		for (const std::string& match : matches) {
			longest = std::max<std::size_t>(longest, cellsOf(match));
		}

		return longest;
	}

	static std::string matchesRow(const std::vector<std::string>& matches, std::size_t row,
	                              std::size_t rows, std::size_t cols, std::size_t pad) {
		std::string line;
		for (std::size_t col = 0; col < cols; ++col) {
			const std::size_t index = col * rows + row;
			if (index >= matches.size()) break;

			line += matches[index];
			const bool more_in_row = col + 1 < cols && (col + 1) * rows + row < matches.size();
			if (more_in_row) line.append(pad - cellsOf(matches[index]), ' ');
		}

		return line;
	}

	// The list goes below the whole line, and the prompt comes back whole
	// under it.
	void LineEditor::printMatches(const std::vector<std::string>& all_matches) {
		const std::size_t cursor = cursor_;
		moveToLineEnd();
		emit("\r\n");
		if (confirmLongList(all_matches.size())) listMatches(all_matches);

		cursor_ = cursor;
		showPrompt();
		redraw();
	}

	// As readline's completion-query-items: a list this long is shown
	// only when the user says so; 0 never asks.
	bool LineEditor::confirmLongList(std::size_t count) {
		static const long long kDefaultQueryItems = 100;

		long long limit = kDefaultQueryItems;
		const std::string setting = shellKeymap().variable("completion-query-items");
		if (!parseLL(setting, limit)) limit = kDefaultQueryItems;
		if (limit <= 0 || static_cast<long long>(count) < limit) return true;

		emit("Display all " + std::to_string(count) + " possibilities? (y or n)");
		const bool shown = askYesNo();
		emit("\r\n");
		return shown;
	}

	void LineEditor::listMatches(const std::vector<std::string>& all_matches) {
		const std::vector<std::string> matches = filename_matches_
			? complete_detail::shownNames(all_matches) : all_matches;
		const std::size_t pad = longestLength(matches) + 2;
		const std::size_t cols = std::max<std::size_t>(1, screenColumns() / pad);
		const std::size_t rows = (matches.size() + cols - 1) / cols;
		for (std::size_t row = 0; row < rows; ++row) {
			emit(matchesRow(matches, row, rows, cols, pad) + "\r\n");
		}
	}

	void LineEditor::applyCompletion(const Tok& tok,
	                                 const std::vector<std::string>& matches) {
		if (matches.empty()) {
			ding();
			last_was_tab_ = false;
			return;
		}

		if (matches.size() == 1) {
			insertMatch(tok, matches[0], true);
			last_was_tab_ = false;
			return;
		}

		const std::string common = longestCommonPrefix(matches);
		const bool extended = complete_detail::extendsWord(common, tok.word);
		last_was_tab_ = true;
		last_tab_word_ = extended ? common : tok.word;
		if (extended) insertMatch(tok, common, false);

		if (listsAmbiguous(extended)) {
			printMatches(matches);
		} else if (!extended) {
			ding();
		}
	}

	// readline's choice for an ambiguous word: show-all-if-ambiguous lists
	// at once, show-all-if-unmodified when nothing could be added, and
	// otherwise a second Tab lists.
	bool LineEditor::listsAmbiguous(bool extended) const {
		if (shellKeymap().isOn("show-all-if-ambiguous")) return true;
		if (extended) return false;

		return shellKeymap().isOn("show-all-if-unmodified") || last_command_ == "complete";
	}

	// A whole match is finished off -- its quote closed and a space after
	// it -- unless it is a directory, which the next Tab goes into.
	void LineEditor::insertMatch(const Tok& tok, const std::string& match, bool whole) {
		std::string replacement =
			complete_detail::quotedForLine(match, tok.quote, filename_matches_);
		const bool directory = !match.empty() && match.back() == '/';
		if (whole && !directory) {
			if (tok.quote != 0) replacement.push_back(tok.quote);
			if (!completion_nospace_) replacement.push_back(' ');
		}

		buffer_.replace(tok.insert_start, tok.end - tok.insert_start, replacement);
		cursor_ = tok.insert_start + replacement.size();
	}

}  // namespace wbsh

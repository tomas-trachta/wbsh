/**
 * @file keymap.cpp
 * @brief The emacs keymap, readline's key-sequence notation, inputrc files and `bind`.
 */

#include "keymap.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "executor.h"
#include "pathconv.h"

namespace wbsh {

	namespace keymap_detail {

		static const char kEscape = '\x1b';
		static const char kRubout = '\x7f';
		static const int kMaxIncludeDepth = 8;

		static const char* const kCommandNames[] = {
			"abort", "accept-line", "backward-char", "backward-delete-char",
			"backward-kill-line", "backward-kill-word", "backward-word", "beginning-of-history",
			"beginning-of-line", "bracketed-paste-begin", "capitalize-word", "character-search",
			"character-search-backward", "clear-screen", "complete", "complete-command",
			"complete-filename", "complete-variable", "copy-backward-word", "copy-forward-word",
			"copy-region-as-kill", "delete-char", "delete-char-or-list",
			"delete-horizontal-space", "downcase-word", "edit-and-execute-command",
			"end-of-history", "end-of-line", "exchange-point-and-mark",
			"forward-backward-delete-char", "forward-char", "forward-search-history",
			"forward-word", "history-search-backward", "history-search-forward",
			"history-substring-search-backward", "history-substring-search-forward",
			"insert-comment", "insert-completions", "kill-line", "kill-region",
			"kill-whole-line", "kill-word", "menu-complete", "menu-complete-backward",
			"next-history", "non-incremental-forward-search-history",
			"non-incremental-reverse-search-history", "operate-and-get-next", "overwrite-mode",
			"paste-from-clipboard", "possible-completions", "previous-history", "quoted-insert",
			"re-read-init-file", "redraw-current-line", "reverse-search-history",
			"revert-line", "self-insert", "set-mark", "tab-insert", "transpose-chars",
			"transpose-words", "undo", "unix-filename-rubout", "unix-line-discard",
			"unix-word-rubout", "upcase-word", "yank", "yank-last-arg", "yank-nth-arg",
			"yank-pop", nullptr,
		};

		struct DefaultBinding {
			const char* keys;
			const char* command;
		};

		static const DefaultBinding kEmacsBindings[] = {
			{ "\x01", "beginning-of-line" },     { "\x02", "backward-char" },
			{ "\x04", "delete-char" },           { "\x05", "end-of-line" },
			{ "\x06", "forward-char" },          { "\x07", "abort" },
			{ "\x08", "backward-delete-char" },  { "\t", "complete" },
			{ "\n", "accept-line" },             { "\x0b", "kill-line" },
			{ "\x0c", "clear-screen" },          { "\r", "accept-line" },
			{ "\x0e", "next-history" },          { "\x10", "previous-history" },
			{ "\x0f", "operate-and-get-next" },  { "\x11", "quoted-insert" },
			{ "\x1d", "character-search" },      { "\x1b\x1d", "character-search-backward" },
			{ "\x18\x18", "exchange-point-and-mark" },
			{ "\x18\x7f", "backward-kill-line" }, { "\x1b\t", "tab-insert" },
			{ "\x1b/", "complete-filename" },     { "\x1b!", "complete-command" },
			{ "\x1b$", "complete-variable" },     { "\x1b*", "insert-completions" },
			{ "\x12", "reverse-search-history" }, { "\x13", "forward-search-history" },
			{ "\x14", "transpose-chars" },       { "\x15", "unix-line-discard" },
			{ "\x16", "paste-from-clipboard" },  { "\x17", "unix-word-rubout" },
			{ "\x19", "yank" },                  { "\x1f", "undo" },
			{ "\x7f", "backward-delete-char" },
			{ "\x18\x05", "edit-and-execute-command" },
			{ "\x18\x12", "re-read-init-file" }, { "\x18\x15", "undo" },
			{ "\x1b\x08", "backward-kill-word" }, { "\x1b\x19", "yank-nth-arg" },
			{ "\x1b\x7f", "backward-kill-word" }, { "\x1b#", "insert-comment" },
			{ "\x1b.", "yank-last-arg" },        { "\x1b<", "beginning-of-history" },
			{ "\x1b>", "end-of-history" },       { "\x1b?", "possible-completions" },
			{ "\x1b\\", "delete-horizontal-space" }, { "\x1b_", "yank-last-arg" },
			{ "\x1b" "b", "backward-word" },     { "\x1b" "c", "capitalize-word" },
			{ "\x1b" "d", "kill-word" },         { "\x1b" "f", "forward-word" },
			{ "\x1bl", "downcase-word" },        { "\x1br", "revert-line" },
			{ "\x1bt", "transpose-words" },      { "\x1bu", "upcase-word" },
			{ "\x1by", "yank-pop" },
			{ "\x1b[A", "previous-history" },    { "\x1b[B", "next-history" },
			{ "\x1b[C", "forward-char" },        { "\x1b[D", "backward-char" },
			{ "\x1b[H", "beginning-of-line" },   { "\x1b[F", "end-of-line" },
			{ "\x1b[3~", "delete-char" },        { "\x1b[3;5~", "kill-word" },
			{ "\x1b[1;5C", "forward-word" },     { "\x1b[1;5D", "backward-word" },
			{ "\x1b[1;3C", "forward-word" },     { "\x1b[1;3D", "backward-word" },
			{ "\x1b[200~", "bracketed-paste-begin" },
		};

		struct DefaultVariable {
			const char* name;
			const char* value;
		};

		static const DefaultVariable kVariables[] = {
			{ "bell-style", "audible" },
			{ "completion-ignore-case", "on" },
			{ "completion-query-items", "100" },
			{ "editing-mode", "emacs" },
			{ "enable-bracketed-paste", "on" },
			{ "mark-directories", "on" },
			{ "show-all-if-ambiguous", "off" },
			{ "show-all-if-unmodified", "on" },
		};

		static std::string trimmed(const std::string& text) {
			const std::size_t first = text.find_first_not_of(" \t\r\n");
			if (first == std::string::npos) return {};

			const std::size_t last = text.find_last_not_of(" \t\r\n");
			return text.substr(first, last - first + 1);
		}

		static std::string lowered(std::string text) {
			for (char& letter : text) {
				letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
			}

			return text;
		}

		static bool isOctal(char letter) {
			return letter >= '0' && letter <= '7';
		}

		static int hexValue(char letter) {
			if (letter >= '0' && letter <= '9') return letter - '0';

			const int lower = std::tolower(static_cast<unsigned char>(letter));
			return lower >= 'a' && lower <= 'f' ? lower - 'a' + 10 : -1;
		}

		static char controlOf(char letter) {
			if (letter == '?') return kRubout;

			return static_cast<char>(std::toupper(static_cast<unsigned char>(letter)) & 0x1f);
		}

		static char simpleEscape(char letter) {
			switch (letter) {
			case 'a': return '\a';
			case 'b': return '\b';
			case 'd': return kRubout;
			case 'e': return kEscape;
			case 'f': return '\f';
			case 'n': return '\n';
			case 'r': return '\r';
			case 't': return '\t';
			case 'v': return '\v';
			default:  return letter;
			}
		}

		static std::size_t readNumericEscape(const std::string& text, std::size_t i,
		                                     std::string& out) {
			int value = 0;
			std::size_t used = 0;
			if (text[i] == 'x') {
				while (used < 2 && i + 1 + used < text.size()) {
					const int digit = hexValue(text[i + 1 + used]);
					if (digit < 0) break;

					value = value * 16 + digit;
					++used;
				}

				out.push_back(static_cast<char>(value));
				return i + 1 + used;
			}

			while (used < 3 && i + used < text.size() && isOctal(text[i + used])) {
				value = value * 8 + (text[i + used] - '0');
				++used;
			}

			out.push_back(static_cast<char>(value));
			return i + used;
		}

		// One `\...` escape starting after the backslash at `i`; returns
		// where the next character is.
		static std::size_t readEscape(const std::string& text, std::size_t i, std::string& out) {
			const bool control = text.compare(i, 2, "C-") == 0 && i + 2 < text.size();
			const bool meta = text.compare(i, 2, "M-") == 0 && i + 2 < text.size();
			if (control || meta) {
				std::string inner;
				std::size_t next = i + 3;
				if (text[i + 2] == '\\' && i + 3 < text.size()) {
					next = readEscape(text, i + 3, inner);
				} else {
					inner.push_back(text[i + 2]);
				}

				if (meta) out.push_back(kEscape);
				if (control && !inner.empty()) inner.back() = controlOf(inner.back());
				out += inner;
				return next;
			}

			if (text[i] == 'x' || isOctal(text[i])) return readNumericEscape(text, i, out);

			out.push_back(simpleEscape(text[i]));
			return i + 1;
		}

		static std::string translateEscapes(const std::string& text) {
			std::string out;
			std::size_t i = 0;
			while (i < text.size()) {
				if (text[i] == '\\' && i + 1 < text.size()) {
					i = readEscape(text, i + 1, out);
					continue;
				}

				out.push_back(text[i]);
				++i;
			}

			return out;
		}

		static bool namedKey(const std::string& name, char& key) {
			const std::string lower = lowered(name);
			if (lower == "rubout" || lower == "del") key = kRubout;
			else if (lower == "esc" || lower == "escape") key = kEscape;
			else if (lower == "lfd" || lower == "newline") key = '\n';
			else if (lower == "ret" || lower == "return") key = '\r';
			else if (lower == "space" || lower == "spc") key = ' ';
			else if (lower == "tab") key = '\t';
			else if (name.size() == 1) key = name[0];
			else return false;

			return true;
		}

		static bool stripPrefix(std::string& name, const char* longer, const char* shorter) {
			const std::string lower = lowered(name);
			for (const char* prefix : { longer, shorter }) {
				const std::size_t length = std::char_traits<char>::length(prefix);
				if (lower.compare(0, length, prefix) != 0) continue;

				name = name.substr(length);
				return true;
			}

			return false;
		}

		// `Control-u`, `Meta-Rubout`, `C-M-x`: a key written by name.
		static bool keyFromName(std::string name, std::string& keys) {
			bool control = false;
			bool meta = false;
			for (;;) {
				if (stripPrefix(name, "control-", "c-")) {
					control = true;
				} else if (stripPrefix(name, "meta-", "m-")) {
					meta = true;
				} else {
					break;
				}
			}

			char key = 0;
			if (!namedKey(name, key)) return false;

			keys.clear();
			if (meta) keys.push_back(kEscape);
			keys.push_back(control ? controlOf(key) : key);
			return true;
		}

		static std::size_t closingQuote(const std::string& text, std::size_t open) {
			const char quote = text[open];
			for (std::size_t i = open + 1; i < text.size(); ++i) {
				if (text[i] == '\\') {
					++i;
					continue;
				}

				if (text[i] == quote) return i;
			}

			return std::string::npos;
		}

		static std::string describeKey(char key) {
			if (key == kEscape) return "\\e";
			if (key == kRubout) return "\\C-?";
			if (key == '\\' || key == '"') return std::string("\\") + key;

			const auto byte = static_cast<unsigned char>(key);
			const char letter = static_cast<char>(std::tolower(byte | 0x40));
			if (byte < 0x20) return std::string("\\C-") + letter;

			return std::string(1, key);
		}

		static std::string describeKeys(const std::string& keys) {
			std::string text;
			for (const char key : keys) text += describeKey(key);
			return text;
		}

		static std::string describeMacro(const std::string& macro) {
			std::string text;
			for (const char letter : macro) {
				const auto byte = static_cast<unsigned char>(letter);
				text += byte < 0x20 || letter == kRubout || letter == '\\' || letter == '"'
					? describeKey(letter) : std::string(1, letter);
			}

			return text;
		}

	}  // namespace keymap_detail

	Keymap::Keymap() {
		for (const keymap_detail::DefaultBinding& entry : keymap_detail::kEmacsBindings) {
			bindings_[entry.keys] = { KeyBinding::Kind::kCommand, entry.command };
		}

		bindings_[std::string(1, '\0')] = { KeyBinding::Kind::kCommand, "set-mark" };

		for (const keymap_detail::DefaultVariable& entry : keymap_detail::kVariables) {
			variables_[entry.name] = entry.value;
		}
	}

	KeyLookup Keymap::lookup(const std::string& keys, KeyBinding& binding) const {
		const auto found = bindings_.lower_bound(keys);
		if (found == bindings_.end()) return KeyLookup::kUnbound;

		if (found->first == keys) {
			const auto next = std::next(found);
			const bool longer = next != bindings_.end()
				&& next->first.compare(0, keys.size(), keys) == 0;
			if (!longer) {
				binding = found->second;
				return KeyLookup::kBound;
			}

			return KeyLookup::kPrefix;
		}

		const bool prefix = found->first.compare(0, keys.size(), keys) == 0;
		return prefix ? KeyLookup::kPrefix : KeyLookup::kUnbound;
	}

	void Keymap::bind(const std::string& keys, const KeyBinding& binding) {
		if (keys.empty()) return;

		bindings_[keys] = binding;
	}

	void Keymap::unbind(const std::string& keys) {
		bindings_.erase(keys);
	}

	void Keymap::setVariable(const std::string& name, const std::string& value) {
		variables_[name] = value;
	}

	std::string Keymap::variable(const std::string& name) const {
		const auto found = variables_.find(name);
		return found == variables_.end() ? std::string() : found->second;
	}

	bool Keymap::isOn(const std::string& name) const {
		const std::string value = keymap_detail::lowered(variable(name));
		return value == "on" || value == "1";
	}

	Keymap& shellKeymap() {
		static Keymap keymap;
		return keymap;
	}

	const std::vector<std::string>& editingCommandNames() {
		static const std::vector<std::string> names = [] {
			std::vector<std::string> list;
			for (int i = 0; keymap_detail::kCommandNames[i] != nullptr; ++i) {
				list.emplace_back(keymap_detail::kCommandNames[i]);
			}

			return list;
		}();
		return names;
	}

	namespace keymap_detail {

		static bool isCommandName(const std::string& name) {
			const std::vector<std::string>& names = editingCommandNames();
			return std::find(names.begin(), names.end(), name) != names.end();
		}

		static bool readKeyPart(const std::string& line, std::string& keys, std::size_t& after,
		                        std::string& error) {
			if (line[0] == '"') {
				const std::size_t close = closingQuote(line, 0);
				if (close == std::string::npos) {
					error = "no closing `\"' in key binding";
					return false;
				}

				keys = translateEscapes(line.substr(1, close - 1));
				after = line.find(':', close);
			} else {
				after = line.find(':');
				const bool named = after != std::string::npos
					&& keyFromName(trimmed(line.substr(0, after)), keys);
				if (!named) {
					error = "`" + line + "': unknown key name";
					return false;
				}
			}

			if (after == std::string::npos || keys.empty()) {
				error = "`" + line + "': missing colon separator";
				return false;
			}

			return true;
		}

		static KeyBinding bindingFrom(const std::string& value) {
			if (!value.empty() && (value[0] == '"' || value[0] == '\'')) {
				const std::size_t close = closingQuote(value, 0);
				const std::string body = value.substr(1, close == std::string::npos
					? std::string::npos : close - 1);
				return { KeyBinding::Kind::kMacro, translateEscapes(body) };
			}

			const std::size_t end = value.find_first_of(" \t");
			return { KeyBinding::Kind::kCommand, value.substr(0, end) };
		}

		static bool applySet(const std::string& line) {
			const std::string rest = trimmed(line.substr(4));
			const std::size_t space = rest.find_first_of(" \t");
			const std::string name = rest.substr(0, space);
			const std::string value = space == std::string::npos
				? "on" : trimmed(rest.substr(space));
			shellKeymap().setVariable(name, value);
			return true;
		}

		static bool startsWithSet(const std::string& line) {
			return line.size() > 4 && line.compare(0, 3, "set") == 0
			    && (line[3] == ' ' || line[3] == '\t');
		}

		// As readline, a function the editor does not have leaves the key
		// as it was without a word, so a .bashrc or .inputrc written for
		// bash loads cleanly.
		static bool applyBindingLine(const std::string& line, std::string& error) {
			std::string keys;
			std::size_t colon = 0;
			if (!readKeyPart(line, keys, colon, error)) return false;

			const KeyBinding binding = bindingFrom(trimmed(line.substr(colon + 1)));
			if (binding.kind == KeyBinding::Kind::kCommand && !isCommandName(binding.value)) {
				return true;
			}

			shellKeymap().bind(keys, binding);
			return true;
		}

		struct Conditional {
			bool enclosing;
			bool met;
		};

		// What a `$if` asks about: the editing mode, the terminal, or the
		// application, which wbsh answers to as both Bash and wbsh.
		static bool conditionHolds(const std::string& condition, Executor& exec) {
			const std::string lower = lowered(trimmed(condition));
			if (lower.compare(0, 5, "mode=") == 0) return lower.substr(5) == "emacs";
			if (lower.compare(0, 5, "term=") == 0) {
				const std::string term = lowered(exec.env().get("TERM"));
				return !term.empty() && term.compare(0, lower.size() - 5, lower.substr(5)) == 0;
			}

			return lower == "bash" || lower == "wbsh";
		}

		static bool active(const std::vector<Conditional>& stack) {
			return stack.empty() || (stack.back().enclosing && stack.back().met);
		}

		static void readInitFile(const std::string& path, Executor& exec, int depth);

		static void applyDirective(const std::string& line, std::vector<Conditional>& stack,
		                           Executor& exec, int depth) {
			const std::size_t space = line.find_first_of(" \t");
			const std::string word = line.substr(0, space);
			const std::string rest = space == std::string::npos ? "" : trimmed(line.substr(space));
			if (word == "$if") {
				stack.push_back({ active(stack), conditionHolds(rest, exec) });
			} else if (word == "$else" && !stack.empty()) {
				stack.back().met = !stack.back().met;
			} else if (word == "$endif" && !stack.empty()) {
				stack.pop_back();
			} else if (word == "$include" && active(stack)) {
				readInitFile(exec.pathConv().toWin32(rest), exec, depth + 1);
			}
		}

		static void readInitFile(const std::string& path, Executor& exec, int depth) {
			if (depth > kMaxIncludeDepth) return;

			std::ifstream file(utf8ToPath(path), std::ios::binary);
			std::string raw;
			std::vector<Conditional> stack;
			while (std::getline(file, raw)) {
				const std::string line = trimmed(raw);
				if (line.empty() || line[0] == '#') continue;
				if (line[0] == '$') {
					applyDirective(line, stack, exec, depth);
					continue;
				}

				std::string error;
				if (active(stack)) applyInputrcLine(line, error);
			}
		}

	}  // namespace keymap_detail

	bool applyInputrcLine(const std::string& raw, std::string& error) {
		const std::string line = keymap_detail::trimmed(raw);
		if (line.empty() || line[0] == '#') return true;
		if (keymap_detail::startsWithSet(line)) return keymap_detail::applySet(line);

		return keymap_detail::applyBindingLine(line, error);
	}

	void loadInputrc(Executor& exec) {
		std::string path = exec.env().get("INPUTRC");
		if (path.empty()) {
			const std::string home = exec.env().get("HOME");
			if (home.empty()) return;

			path = home + "/.inputrc";
		}

		keymap_detail::readInitFile(exec.pathConv().toWin32(path), exec, 0);
	}

	namespace bind_detail {

		struct BindRequest {
			std::string keymap_prefix;
			std::vector<std::string> lines;
		};

		static void printErr(const std::string& message) {
			std::fprintf(stderr, "wbsh: bind: %s\n", message.c_str());
		}

		static std::vector<std::string> keysFor(const std::string& command) {
			std::vector<std::string> keys;
			for (const auto& entry : shellKeymap().bindings()) {
				const KeyBinding& binding = entry.second;
				if (binding.kind == KeyBinding::Kind::kCommand && binding.value == command) {
					keys.push_back(entry.first);
				}
			}

			return keys;
		}

		static void listNames() {
			for (const std::string& name : editingCommandNames()) std::printf("%s\n", name.c_str());
		}

		static void listCommands(bool readable) {
			for (const std::string& name : editingCommandNames()) {
				const std::vector<std::string> keys = keysFor(name);
				if (keys.empty()) {
					if (readable) std::printf("# %s (not bound)\n", name.c_str());
					else std::printf("%s is not bound to any keys\n", name.c_str());
					continue;
				}

				if (readable) {
					for (const std::string& key : keys) {
						const std::string described = keymap_detail::describeKeys(key);
						std::printf("\"%s\": %s\n", described.c_str(), name.c_str());
					}

					continue;
				}

				std::string list;
				for (const std::string& key : keys) {
					if (!list.empty()) list += ", ";
					list += "\"" + keymap_detail::describeKeys(key) + "\"";
				}

				std::printf("%s can be found on %s.\n", name.c_str(), list.c_str());
			}
		}

		static void listOfKind(KeyBinding::Kind kind, bool readable) {
			for (const auto& entry : shellKeymap().bindings()) {
				if (entry.second.kind != kind) continue;

				const std::string keys = keymap_detail::describeKeys(entry.first);
				const std::string value = keymap_detail::describeMacro(entry.second.value);
				if (readable) std::printf("\"%s\": \"%s\"\n", keys.c_str(), value.c_str());
				else std::printf("%s outputs %s\n", keys.c_str(), value.c_str());
			}
		}

		static void listVariables(bool readable) {
			for (const auto& entry : shellKeymap().variables()) {
				if (readable) std::printf("set %s %s\n", entry.first.c_str(), entry.second.c_str());
				else std::printf("%s is set to `%s'\n", entry.first.c_str(), entry.second.c_str());
			}
		}

		static int query(const std::string& name) {
			if (!keymap_detail::isCommandName(name)) {
				printErr("`" + name + "': unknown function name");
				return 1;
			}

			const std::vector<std::string> keys = keysFor(name);
			if (keys.empty()) {
				std::printf("%s is not bound to any keys.\n", name.c_str());
				return 1;
			}

			std::string list;
			for (const std::string& key : keys) {
				if (!list.empty()) list += ", ";
				list += "\"" + keymap_detail::describeKeys(key) + "\"";
			}

			std::printf("%s can be invoked via %s.\n", name.c_str(), list.c_str());
			return 0;
		}

		static int unbindCommand(const std::string& name) {
			if (!keymap_detail::isCommandName(name)) {
				printErr("`" + name + "': unknown function name");
				return 1;
			}

			for (const std::string& key : keysFor(name)) shellKeymap().unbind(key);
			return 0;
		}

		static int removeKeys(const BindRequest& request, const std::string& spec) {
			const std::string text = spec.size() >= 2 && spec.front() == '"' && spec.back() == '"'
				? spec.substr(1, spec.size() - 2) : spec;
			shellKeymap().unbind(request.keymap_prefix + keymap_detail::translateEscapes(text));
			return 0;
		}

		static int bindShellCommand(const BindRequest& request, const std::string& spec) {
			std::string keys;
			std::size_t colon = 0;
			std::string error;
			const std::string line = keymap_detail::trimmed(spec);
			if (line.empty() || !keymap_detail::readKeyPart(line, keys, colon, error)) {
				printErr(error.empty() ? "`" + spec + "': missing colon separator" : error);
				return 1;
			}

			std::string command = keymap_detail::trimmed(line.substr(colon + 1));
			const bool quotes = command.size() >= 2 && command.back() == command.front();
			if (quotes && (command.front() == '"' || command.front() == '\'')) {
				command = command.substr(1, command.size() - 2);
			}

			const KeyBinding binding = { KeyBinding::Kind::kShellCommand, command };
			shellKeymap().bind(request.keymap_prefix + keys, binding);
			return 0;
		}

		static int applyLine(const BindRequest& request, const std::string& line) {
			std::string adjusted = keymap_detail::trimmed(line);
			const bool quoted_keys = !adjusted.empty() && adjusted[0] == '"';
			if (!request.keymap_prefix.empty() && quoted_keys) {
				adjusted.insert(1, keymap_detail::describeKeys(request.keymap_prefix));
			}

			std::string error;
			if (applyInputrcLine(adjusted, error)) return 0;

			printErr(error);
			return 1;
		}

		static bool setKeymap(BindRequest& request, const std::string& name) {
			if (name == "emacs" || name == "emacs-standard") request.keymap_prefix.clear();
			else if (name == "emacs-meta") request.keymap_prefix = "\x1b";
			else if (name == "emacs-ctlx") request.keymap_prefix = "\x18";
			else return false;

			return true;
		}

	}  // namespace bind_detail

	namespace bind_detail {

		static int runListing(char option) {
			switch (option) {
			case 'l': listNames();                                      return 0;
			case 'p': listCommands(true);                               return 0;
			case 'P': listCommands(false);                              return 0;
			case 's': listOfKind(KeyBinding::Kind::kMacro, true);         return 0;
			case 'S': listOfKind(KeyBinding::Kind::kMacro, false);        return 0;
			case 'X': listOfKind(KeyBinding::Kind::kShellCommand, true);  return 0;
			case 'v': listVariables(true);                              return 0;
			case 'V': listVariables(false);                             return 0;
			default:  return -1;
			}
		}

		static int runWithArgument(char option, const std::string& value, BindRequest& request,
		                           Executor& exec) {
			switch (option) {
			case 'm':
				if (setKeymap(request, value)) return 0;
				printErr("`" + value + "': invalid keymap name");
				return 1;
			case 'f':
				keymap_detail::readInitFile(exec.pathConv().toWin32(value), exec, 0);
				return 0;
			case 'q': return query(value);
			case 'u': return unbindCommand(value);
			case 'r': return removeKeys(request, value);
			case 'x': return bindShellCommand(request, value);
			default:  return -1;
			}
		}

		static bool takesArgument(char option) {
			return option == 'm' || option == 'f' || option == 'q' || option == 'u'
			    || option == 'r' || option == 'x';
		}

		static int runOptions(const std::string& flags, const std::vector<std::string>& args,
		                      std::size_t& i, BindRequest& request, Executor& exec) {
			for (std::size_t k = 1; k < flags.size(); ++k) {
				const char option = flags[k];
				if (!takesArgument(option)) {
					const int status = runListing(option);
					if (status < 0) {
						printErr("-" + std::string(1, option) + ": invalid option");
						return 2;
					}

					continue;
				}

				std::string value = flags.substr(k + 1);
				if (value.empty() && i + 1 < args.size()) value = args[++i];
				if (value.empty()) {
					printErr("-" + std::string(1, option) + ": option requires an argument");
					return 2;
				}

				return runWithArgument(option, value, request, exec);
			}

			return 0;
		}

	}  // namespace bind_detail

	namespace bind_detail {

		static std::string shellQuoted(const std::string& text) {
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

		static bool sameBinding(const KeyBinding& left, const KeyBinding& right) {
			return left.kind == right.kind && left.value == right.value;
		}

		static std::string bindCommandFor(const std::string& keys, const KeyBinding& binding) {
			const std::string quoted_keys = "\"" + keymap_detail::describeKeys(keys) + "\": ";
			switch (binding.kind) {
			case KeyBinding::Kind::kMacro:
				return "bind " + shellQuoted(quoted_keys + "\""
					+ keymap_detail::describeMacro(binding.value) + "\"");
			case KeyBinding::Kind::kShellCommand:
				return "bind -x " + shellQuoted(quoted_keys + "\"" + binding.value + "\"");
			default:
				return "bind " + shellQuoted(quoted_keys + binding.value);
			}
		}

		static std::string removedDefaults(const Keymap& defaults) {
			std::string text;
			for (const auto& entry : defaults.bindings()) {
				if (shellKeymap().bindings().count(entry.first) != 0) continue;

				const std::string keys = "\"" + keymap_detail::describeKeys(entry.first) + "\"";
				text += "bind -r " + shellQuoted(keys) + "\n";
			}

			return text;
		}

		static std::string changedBindings(const Keymap& defaults) {
			std::string text;
			for (const auto& entry : shellKeymap().bindings()) {
				const auto found = defaults.bindings().find(entry.first);
				const bool unchanged = found != defaults.bindings().end()
					&& sameBinding(found->second, entry.second);
				if (!unchanged) text += bindCommandFor(entry.first, entry.second) + "\n";
			}

			return text;
		}

		static std::string changedVariables(const Keymap& defaults) {
			std::string text;
			for (const auto& entry : shellKeymap().variables()) {
				if (defaults.variable(entry.first) == entry.second) continue;

				text += "bind " + shellQuoted("set " + entry.first + " " + entry.second) + "\n";
			}

			return text;
		}

	}  // namespace bind_detail

	std::string serializeKeyBindings() {
		const Keymap defaults;
		return bind_detail::removedDefaults(defaults) + bind_detail::changedBindings(defaults)
			+ bind_detail::changedVariables(defaults);
	}

	int bindBuiltin(Executor& exec, const std::vector<std::string>& args) {
		bind_detail::BindRequest request;
		int status = 0;
		bool options_done = false;
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (!options_done && arg == "--") {
				options_done = true;
				continue;
			}

			const bool option = !options_done && arg.size() > 1 && arg[0] == '-';
			const int result = option ? bind_detail::runOptions(arg, args, i, request, exec)
			                          : bind_detail::applyLine(request, arg);
			if (result == 2) return 2;
			if (result != 0) status = 1;
		}

		return status;
	}

}  // namespace wbsh

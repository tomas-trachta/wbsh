#include "lineedit.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>

#  include <io.h>
#endif /* _WIN32 */

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "strscan.h"

namespace wbsh {

	namespace fs = std::filesystem;

#ifdef _WIN32
	static bool stdinIsTty() {
		return _isatty(_fileno(stdin)) != 0;
	}

	// Display columns: skips ANSI CSI sequences, counts UTF-8 codepoints
	// not bytes. Wide CJK / combining marks are not handled.
	static std::size_t visibleLen(const std::string& s) {
		std::size_t n = 0;
		std::size_t i = 0;
		while (i < s.size()) {
			unsigned char c = static_cast<unsigned char>(s[i]);
			if (c == 0x1b && i + 1 < s.size() && s[i + 1] == '[') {
				i += 2;
				while (i < s.size()
				       && !((s[i] >= '@' && s[i] <= '~'))) ++i;
				if (i < s.size()) ++i;
				continue;
			}

			if ((c & 0xC0) != 0x80) ++n;
			++i;
		}

		return n;
	}

	static std::size_t utf8Cols(const std::string& s, std::size_t end_byte) {
		std::size_t n = 0;
		std::size_t lim = end_byte < s.size() ? end_byte : s.size();
		for (std::size_t i = 0; i < lim; ++i) {
			if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) ++n;
		}

		return n;
	}
#endif /* _WIN32 */

	static bool isWordBreak(char c) {
		return std::isspace(static_cast<unsigned char>(c))
		    || c == '|' || c == '&' || c == ';'
		    || c == '<' || c == '>'
		    || c == '(' || c == ')';
	}

	std::size_t findReverseSearchMatch(const std::vector<std::string>& history,
	                                   const std::string& query,
	                                   std::size_t start_index,
	                                   bool forward) {
		if (query.empty() || history.empty()) return history.size();
		std::size_t start = start_index < history.size()
			? start_index : history.size() - 1;
		if (forward) {
			for (std::size_t i = start; i < history.size(); ++i) {
				if (history[i].find(query) != std::string::npos) return i;
			}

			return history.size();
		}
		// Backward (toward older entries). i is unsigned, so guard the
		// underflow by walking until i hits 0 and then breaking after the
		// final compare. The loop body runs for i == 0 too.
		for (std::size_t i = start;; --i) {
			if (history[i].find(query) != std::string::npos) return i;
			if (i == 0) break;
		}

		return history.size();
	}

	std::string findInlinePrediction(const std::vector<std::string>& history,
	                                 const std::vector<int>& history_status,
	                                 const std::string& prefix) {
		if (prefix.empty()) return {};
		for (std::size_t i = history.size(); i > 0; --i) {
			const std::size_t idx = i - 1;
			if (idx < history_status.size() && history_status[idx] != 0) continue;
			const std::string& entry = history[idx];
			if (entry.size() > prefix.size()
			    && entry.compare(0, prefix.size(), prefix) == 0) {
				return entry.substr(prefix.size());
			}
		}

		return {};
	}

	fs::path findGitDir() {
		std::error_code ec;
		fs::path probe = fs::current_path(ec);
		if (ec) return {};

		fs::path gitdir;
		while (true) {
			fs::path candidate = probe / ".git";
			if (fs::exists(candidate, ec)) { gitdir = candidate; break; }
			fs::path parent = probe.parent_path();
			if (parent == probe) return {};
			probe = parent;
		}

		if (!fs::is_directory(gitdir, ec)) {
			std::ifstream f(gitdir);
			std::string line;
			if (!std::getline(f, line)) return {};
			const std::string prefix = "gitdir: ";
			if (line.compare(0, prefix.size(), prefix) != 0) return {};
			fs::path g(line.substr(prefix.size()));
			if (!g.is_absolute()) g = (probe / g).lexically_normal();
			gitdir = g;
		}

		return gitdir;
	}

	LineEditor::LineEditor(Environment& env, Executor& exec)
		: env_(env), exec_(exec) {}

	bool LineEditor::readLine(const std::string& prompt, std::string& out) {
		buffer_.clear();
		cursor_ = 0;
		history_pos_ = 0;
		saved_partial_.clear();
		last_was_tab_ = false;
		last_tab_word_.clear();
		last_cursor_row_ = 0;
		suggestion_.clear();
		revsearch_active_ = false;
		prompt_raw_ = prompt;
#ifdef _WIN32
		prompt_visible_len_ = visibleLen(prompt);
		if (stdinIsTty()) {
			return readLineRaw(prompt, out);
		}
#endif /* _WIN32 */
		std::fputs(prompt.c_str(), stdout);
		std::fflush(stdout);
		return readLineCooked(out);
	}

	bool LineEditor::readLineCooked(std::string& out) {
		out.clear();
		int c;
		while ((c = std::fgetc(stdin)) != EOF) {
			if (c == '\n') return true;
			out.push_back(static_cast<char>(c));
		}

		return !out.empty();
	}

#ifdef _WIN32

	void LineEditor::emit(const std::string& s) {
		HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
		DWORD wrote;
		WriteFile(h, s.data(), static_cast<DWORD>(s.size()), &wrote, nullptr);
	}

	void LineEditor::redraw() {
		HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
		CONSOLE_SCREEN_BUFFER_INFO info{};
		std::size_t width = 80;
		if (h != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(h, &info)) {
			int w = info.srWindow.Right - info.srWindow.Left + 1;
			if (w > 0) width = static_cast<std::size_t>(w);
		}

		refreshSuggestion();

		std::string out;
		if (last_cursor_row_ > 0) {
			out += "\x1b[" + std::to_string(last_cursor_row_) + "A";
		}

		out += "\r";
		out += "\x1b[J";
		out += prompt_raw_;
		out += buffer_;
		if (!suggestion_.empty()) {
			out += "\x1b[90m";
			out += suggestion_;
			out += "\x1b[0m";
		}

		// After emitting buffer + suggestion, the cursor sits at column
		// (prompt_visible_len_ + buffer.cols + suggestion.cols) modulo
		// width, on a row that many rows below the prompt's first row --
		// except for the "deferred wrap" case where the content fills
		// exactly to the right edge: in VT mode the cursor stays at the
		// boundary column until the next char arrives, so we emit \r\n
		// to materialise the wrap and land on a fresh row.
		std::size_t buf_cols  = utf8Cols(buffer_, buffer_.size());
		std::size_t sugg_cols = utf8Cols(suggestion_, suggestion_.size());
		std::size_t end_cols  = prompt_visible_len_ + buf_cols + sugg_cols;
		std::size_t want_cols = prompt_visible_len_ + utf8Cols(buffer_, cursor_);
		std::size_t end_row   = end_cols / width;
		std::size_t want_row  = want_cols / width;
		std::size_t want_col  = want_cols % width;
		bool at_boundary = end_cols > 0 && (end_cols % width) == 0;
		std::size_t cur_row = end_row;
		if (at_boundary) out += "\r\n";

		out += "\r";
		if (cur_row > want_row) {
			out += "\x1b[" + std::to_string(cur_row - want_row) + "A";
		}

		if (want_col > 0) {
			out += "\x1b[" + std::to_string(want_col) + "C";
		}

		last_cursor_row_ = want_row;
		emit(out);
	}

	void LineEditor::refreshSuggestion() {
		suggestion_.clear();
		if (revsearch_active_) return;
		if (buffer_.empty() || cursor_ != buffer_.size()) return;
		suggestion_ = findInlinePrediction(exec_.history(),
		                                   exec_.historyStatus(),
		                                   buffer_);
	}

	bool LineEditor::acceptInlineSuggestion() {
		if (suggestion_.empty()) return false;
		insertChars(suggestion_);
		suggestion_.clear();
		return true;
	}

	void LineEditor::handleRightArrow() {
		if (cursor_ < buffer_.size()) {
			++cursor_;
			redraw();
			return;
		}

		if (acceptInlineSuggestion()) {
			redraw();
		}
	}

	void LineEditor::insertChars(const std::string& s) {
		buffer_.insert(cursor_, s);
		cursor_ += s.size();
	}

	void LineEditor::handleBackspace() {
		if (cursor_ == 0) return;
		buffer_.erase(cursor_ - 1, 1);
		--cursor_;
	}

	void LineEditor::handleDelete() {
		if (cursor_ >= buffer_.size()) return;
		buffer_.erase(cursor_, 1);
	}

	void LineEditor::handleKillToEnd() {
		buffer_.erase(cursor_);
	}

	void LineEditor::handleKillToStart() {
		buffer_.erase(0, cursor_);
		cursor_ = 0;
	}

	void LineEditor::handleKillWordBack() {
		std::size_t i = cursor_;
		while (i > 0 && std::isspace(static_cast<unsigned char>(buffer_[i - 1]))) --i;
		while (i > 0 && !isWordBreak(buffer_[i - 1])) --i;
		buffer_.erase(i, cursor_ - i);
		cursor_ = i;
	}

	void LineEditor::handleClearScreen() {
		emit("\x1b[H\x1b[2J\x1b[3J");
		last_cursor_row_ = 0;
		redraw();
	}

	void LineEditor::handleHistoryUp() {
		const auto& h = exec_.history();
		if (h.empty()) return;
		if (history_pos_ == 0) saved_partial_ = buffer_;
		if (history_pos_ < h.size()) ++history_pos_;
		buffer_ = h[h.size() - history_pos_];
		cursor_ = buffer_.size();
	}

	void LineEditor::handleHistoryDown() {
		if (history_pos_ == 0) return;
		const auto& h = exec_.history();
		--history_pos_;
		if (history_pos_ == 0) buffer_ = saved_partial_;
		else                   buffer_ = h[h.size() - history_pos_];
		cursor_ = buffer_.size();
	}

	void LineEditor::handleEnter(std::string& out, bool& done) {
		emit("\r\n");
		out = buffer_;
		done = true;
	}

	LineEditor::Tok LineEditor::currentToken() const {
		Tok t{ cursor_, cursor_, true };
		std::size_t i = cursor_;
		while (i > 0 && !isWordBreak(buffer_[i - 1])) --i;
		t.start = i;
		t.end = cursor_;
		std::size_t k = t.start;
		while (k > 0 && std::isspace(static_cast<unsigned char>(buffer_[k - 1]))) --k;
		if (k == 0) { t.first = true; return t; }
		char prev = buffer_[k - 1];
		t.first = (prev == '|' || prev == '&' || prev == ';'
		        || prev == '(' || prev == '{');
		return t;
	}

	static std::string stripExeExtension(std::string name) {
		static const char* exts[] = { ".exe", ".cmd", ".bat", ".com", nullptr };
		for (int i = 0; exts[i]; ++i) {
			const std::size_t L = std::strlen(exts[i]);
			if (name.size() <= L) continue;
			const std::size_t off = name.size() - L;
			bool match = true;
			for (std::size_t k = 0; k < L; ++k) {
				if (std::tolower(static_cast<unsigned char>(name[off + k]))
				    != exts[i][k]) { match = false; break; }
			}

			if (match) {
				name.resize(off);
				break;
			}
		}

		return name;
	}

	std::vector<std::string> LineEditor::commandCompletions(const std::string& prefix) {
		std::set<std::string> set;
		for (const auto& n : exec_.builtinNames()) {
			if (n.compare(0, prefix.size(), prefix) == 0) set.insert(n);
		}

		for (const auto& n : exec_.functionNames()) {
			if (n.compare(0, prefix.size(), prefix) == 0) set.insert(n);
		}

		const std::vector<std::string> dirs = splitPathList(env_.get("PATH"));
		for (const auto& d : dirs) {
			if (d.empty()) continue;
			std::error_code ec;
			fs::path base = utf8ToPath(exec_.pathConv().toWin32(d));
			fs::directory_iterator it(base, ec);
			if (ec) continue;
			for (const auto& de : it) {
				std::string nice = stripExeExtension(pathToUtf8(de.path().filename()));
				if (nice.compare(0, prefix.size(), prefix) == 0) {
					set.insert(std::move(nice));
				}
			}
		}

		return std::vector<std::string>(set.begin(), set.end());
	}

	std::vector<std::string> LineEditor::pathCompletions(const std::string& prefix) {
		std::string dir, base;
		auto slash = prefix.find_last_of("/\\");
		if (slash == std::string::npos) {
			dir = ".";
			base = prefix;
		} else {
			dir = prefix.substr(0, slash);
			if (dir.empty()) dir = "/";
			base = prefix.substr(slash + 1);
		}

		std::string lookup_dir = dir;
		if (lookup_dir == "~"
		    || (lookup_dir.size() >= 2 && lookup_dir[0] == '~'
		        && (lookup_dir[1] == '/' || lookup_dir[1] == '\\'))) {
			std::string home = exec_.env().get("HOME");
			if (!home.empty()) {
				lookup_dir = (lookup_dir.size() == 1)
					? home
					: home + lookup_dir.substr(1);
			}
		}

		fs::path native = utf8ToPath(exec_.pathConv().toWin32(lookup_dir));
		std::vector<std::string> matches;
		std::error_code ec;
		fs::directory_iterator it(native, ec);
		if (ec) return matches;
		for (const auto& de : it) {
			const std::string name = pathToUtf8(de.path().filename());
			if (name.empty()) continue;
			if (name.compare(0, base.size(), base) != 0) continue;
			if (name[0] == '.' && (base.empty() || base[0] != '.')) continue;
			std::string out;
			if (slash == std::string::npos) {
				out = name;
			} else {
				out = prefix.substr(0, slash + 1) + name;
			}

			std::error_code ec2;
			if (de.is_directory(ec2)) out.push_back('/');
			matches.push_back(std::move(out));
		}

		std::sort(matches.begin(), matches.end());
		return matches;
	}

	std::vector<std::string> LineEditor::completionsFor(const std::string& prefix, bool cmd) {
		if (cmd && prefix.find('/') == std::string::npos) {
			return commandCompletions(prefix);
		}

		Tok cur = currentToken();
		auto tools = toolCompletions(prefix, cur);
		if (!tools.empty()) return tools;
		return pathCompletions(prefix);
	}

	std::vector<std::string> LineEditor::prevTokensBefore(const Tok& tok) const {
		std::vector<std::string> prev;
		std::string cur;
		for (std::size_t i = 0; i < tok.start; ++i) {
			char c = buffer_[i];
			if (std::isspace(static_cast<unsigned char>(c))) {
				if (!cur.empty()) { prev.push_back(cur); cur.clear(); }
			} else cur.push_back(c);
		}

		if (!cur.empty()) prev.push_back(cur);
		return prev;
	}

	std::vector<std::string> LineEditor::toolCompletions(const std::string& prefix,
	                                                     const Tok& tok) {
		auto prev = prevTokensBefore(tok);
		if (prev.empty()) return {};
		const std::string& head = prev.front();
		if (auto* spec = exec_.completionSpec(head)) {
			std::vector<std::string> matches;
			for (const auto& w : spec->words) {
				if (w.compare(0, prefix.size(), prefix) == 0) matches.push_back(w);
			}

			if (!spec->function.empty() && exec_.isFunction(spec->function)) {
				auto& env = exec_.env();
				std::vector<std::string> comp_words = prev;
				comp_words.push_back(prefix);
				std::map<long long, std::string> ia;
				for (std::size_t i = 0; i < comp_words.size(); ++i)
					ia[(long long)i] = comp_words[i];
				env.setIndexedArraySparse("COMP_WORDS", std::move(ia));
				env.set("COMP_CWORD", std::to_string(comp_words.size() - 1));
				std::string line = buffer_.substr(0, tok.start) + prefix;
				env.set("COMP_LINE", line);
				env.set("COMP_POINT", std::to_string(line.size()));
				env.unset("COMPREPLY");
				std::vector<std::string> args = { head, prefix,
					prev.size() > 1 ? prev.back() : std::string() };
				exec_.callFunction(spec->function, args);
				// A completion function calling exit / break / return must
				// not tear down the interactive shell — drop any signal
				// (and stray expansion error) it left behind.
				exec_.clearFlow();
				if (exec_.expander().failed()) exec_.expander().takeError();
				if (auto* arr = env.getIndexedArray("COMPREPLY")) {
					for (const auto& kv : *arr) {
						const std::string& s = kv.second;
						if (s.compare(0, prefix.size(), prefix) == 0)
							matches.push_back(s);
					}
				}
			}

			if (!matches.empty()) return matches;
			if (spec->include_dirs || spec->include_files
			    || spec->default_fallback) {
				return pathCompletions(prefix);
			}

			return {};
		}

		if (head == "git")     return gitCompletions(prefix, prev);
		if (head == "docker")  return dockerCompletions(prefix, prev);
		if (head == "npm")     return npmCompletions(prefix, prev);
		if (head == "cargo")   return cargoCompletions(prefix, prev);
		if (head == "kubectl" || head == "k") return kubectlCompletions(prefix, prev);
		return {};
	}

	static void appendLooseBranches(const fs::path& gitdir,
	                                std::vector<std::string>& branches) {
		std::error_code ec;
		fs::path heads = gitdir / "refs" / "heads";
		if (!fs::is_directory(heads, ec)) return;
		fs::recursive_directory_iterator it(heads, ec);
		if (ec) return;
		for (auto p = it; p != fs::recursive_directory_iterator(); p.increment(ec)) {
			if (ec) break;
			std::error_code fec;
			if (p->is_regular_file(fec)) {
				auto rel = pathToUtf8(fs::relative(p->path(), heads, fec));
				std::replace(rel.begin(), rel.end(), '\\', '/');
				branches.push_back(std::move(rel));
			}
		}
	}

	static void appendPackedBranches(const fs::path& gitdir,
	                                 std::vector<std::string>& branches) {
		std::ifstream pr(gitdir / "packed-refs");
		std::string line;
		while (std::getline(pr, line)) {
			if (line.empty() || line[0] == '#' || line[0] == '^') continue;
			auto sp = line.find(' ');
			if (sp == std::string::npos) continue;
			std::string ref = line.substr(sp + 1);
			while (!ref.empty() && (ref.back() == '\r' || ref.back() == '\n'))
				ref.pop_back();
			const std::string p = "refs/heads/";
			if (ref.compare(0, p.size(), p) == 0) {
				branches.push_back(ref.substr(p.size()));
			}
		}
	}

	std::vector<std::string> LineEditor::gitBranches() {
		std::vector<std::string> branches;
		fs::path gitdir = findGitDir();
		if (gitdir.empty()) return branches;
		appendLooseBranches(gitdir, branches);
		appendPackedBranches(gitdir, branches);
		std::sort(branches.begin(), branches.end());
		branches.erase(std::unique(branches.begin(), branches.end()), branches.end());
		return branches;
	}

	std::vector<std::string> LineEditor::gitRemotes() {
		std::vector<std::string> remotes;
		fs::path gitdir = findGitDir();
		if (gitdir.empty()) return remotes;
		std::ifstream cfg(gitdir / "config");
		std::string line;
		while (std::getline(cfg, line)) {
			StrScan in(line);
			in.skipSpaces();
			std::string name;
			if (in.consume("[remote \"") && in.readUpTo('"', name)) {
				remotes.push_back(std::move(name));
			}
		}

		std::sort(remotes.begin(), remotes.end());
		remotes.erase(std::unique(remotes.begin(), remotes.end()), remotes.end());
		return remotes;
	}

	static std::vector<std::string> filterPrefix(const char* const* table,
	                                             const std::string& prefix) {
		std::vector<std::string> out;
		for (int i = 0; table[i]; ++i) {
			std::string s = table[i];
			if (s.compare(0, prefix.size(), prefix) == 0) out.push_back(std::move(s));
		}

		return out;
	}

	static std::vector<std::string> filterPrefix(std::vector<std::string> v,
	                                             const std::string& prefix) {
		std::vector<std::string> out;
		for (auto& s : v) {
			if (s.compare(0, prefix.size(), prefix) == 0) out.push_back(std::move(s));
		}

		return out;
	}

	std::vector<std::string> LineEditor::gitCompletions(const std::string& prefix,
	                                                    const std::vector<std::string>& prev) {
		if (prev.size() == 1) {
			static const char* kSubs[] = {
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
			std::size_t non_flag = 0;
			for (std::size_t i = 2; i < prev.size(); ++i) {
				if (!prev[i].empty() && prev[i][0] != '-') ++non_flag;
			}

			return filterPrefix((non_flag == 0) ? gitRemotes() : gitBranches(), prefix);
		}

		static const char* kBranchSubs[] = {
			"checkout", "switch", "branch", "merge", "rebase", "reset",
			"diff", "log", "show", "cherry-pick", "revert",
			nullptr,
		};
		bool branchy = false;
		for (int i = 0; kBranchSubs[i]; ++i) {
			if (sub == kBranchSubs[i]) { branchy = true; break; }
		}

		if (!branchy) return {};
		return filterPrefix(gitBranches(), prefix);
	}

	static std::vector<std::string> dockerSecondLevelCompletions(
		const std::string& sub, const std::string& prefix) {
		if (sub == "container") {
			static const char* kSubs[] = {
				"attach", "commit", "cp", "create", "diff", "exec",
				"export", "inspect", "kill", "logs", "ls", "pause", "port",
				"prune", "rename", "restart", "rm", "run", "start",
				"stats", "stop", "top", "unpause", "update", "wait",
				nullptr,
			};
			return filterPrefix(kSubs, prefix);
		}

		if (sub == "image") {
			static const char* kSubs[] = {
				"build", "history", "import", "inspect", "load", "ls",
				"prune", "pull", "push", "rm", "save", "tag", nullptr,
			};
			return filterPrefix(kSubs, prefix);
		}

		if (sub == "network") {
			static const char* kSubs[] = {
				"connect", "create", "disconnect", "inspect", "ls",
				"prune", "rm", nullptr,
			};
			return filterPrefix(kSubs, prefix);
		}

		if (sub == "volume") {
			static const char* kSubs[] = {
				"create", "inspect", "ls", "prune", "rm", nullptr,
			};
			return filterPrefix(kSubs, prefix);
		}

		if (sub == "compose") {
			static const char* kSubs[] = {
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
			static const char* kSubs[] = {
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

		if (prev.size() == 2) {
			return dockerSecondLevelCompletions(prev[1], prefix);
		}

		return {};
	}

	/* Tolerant scan of package.json: the names of the "scripts" object's
	 * keys, up to its first '}'. Good enough for completions; not JSON. */
	static std::vector<std::string> packageJsonScripts() {
		std::error_code ec;
		std::ifstream f(fs::current_path(ec) / "package.json");
		if (!f) return {};
		const std::string json((std::istreambuf_iterator<char>(f)),
		                       std::istreambuf_iterator<char>());

		StrScan in(json);
		if (!in.skipPast("\"scripts\"") || !in.skipPast("{") || !in.stopAt('}'))
			return {};

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
			static const char* kSubs[] = {
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
		if (prev.size() == 1) {
			static const char* kSubs[] = {
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

		return {};
	}

	std::vector<std::string> LineEditor::kubectlCompletions(const std::string& prefix,
	                                                        const std::vector<std::string>& prev) {
		if (prev.size() == 1) {
			static const char* kSubs[] = {
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

		if (prev.size() == 2) {
			static const char* kRes[] = {
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
			static const char* kVerbs[] = {
				"get", "describe", "delete", "edit", "label", "annotate",
				"patch", "scale", "rollout", "logs", "exec", "port-forward",
				"top", "wait", "set", "expose", "autoscale",
				nullptr,
			};
			const std::string& sub = prev[1];
			for (int i = 0; kVerbs[i]; ++i) {
				if (sub == kVerbs[i]) return filterPrefix(kRes, prefix);
			}
		}

		return {};
	}

	std::string LineEditor::longestCommonPrefix(const std::vector<std::string>& v) {
		if (v.empty()) return {};
		std::string p = v.front();
		for (std::size_t i = 1; i < v.size(); ++i) {
			std::size_t k = 0;
			while (k < p.size() && k < v[i].size() && p[k] == v[i][k]) ++k;
			p.resize(k);
			if (p.empty()) break;
		}

		return p;
	}

	void LineEditor::printMatches(const std::vector<std::string>& matches) {
		emit("\r\n");
		HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
		CONSOLE_SCREEN_BUFFER_INFO info{};
		int width = 80;
		if (h != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(h, &info)) {
			int w = info.srWindow.Right - info.srWindow.Left + 1;
			if (w > 0) width = w;
		}

		std::size_t maxlen = 0;
		for (const auto& m : matches) if (m.size() > maxlen) maxlen = m.size();
		std::size_t pad = maxlen + 2;
		std::size_t cols = std::max<std::size_t>(1, width / pad);
		std::size_t rows = (matches.size() + cols - 1) / cols;
		for (std::size_t r = 0; r < rows; ++r) {
			std::string line;
			for (std::size_t c = 0; c < cols; ++c) {
				std::size_t idx = c * rows + r;
				if (idx >= matches.size()) break;
				line += matches[idx];
				if (c + 1 < cols && (c + 1) * rows + r < matches.size()) {
					for (std::size_t k = matches[idx].size(); k < pad; ++k) line.push_back(' ');
				}
			}

			emit(line + "\r\n");
		}

		last_cursor_row_ = 0;
		redraw();
	}

	void LineEditor::applyCompletion(const Tok& tok,
	                                 const std::vector<std::string>& matches) {
		if (matches.empty()) {
			emit("\a");
			last_was_tab_ = false;
			return;
		}

		std::string current = buffer_.substr(tok.start, tok.end - tok.start);
		if (matches.size() == 1) {
			std::string repl = matches[0];
			if (repl.empty() || repl.back() != '/') repl.push_back(' ');
			buffer_.replace(tok.start, tok.end - tok.start, repl);
			cursor_ = tok.start + repl.size();
			last_was_tab_ = false;
			return;
		}

		std::string lcp = longestCommonPrefix(matches);
		if (lcp.size() > current.size()) {
			buffer_.replace(tok.start, tok.end - tok.start, lcp);
			cursor_ = tok.start + lcp.size();
			last_was_tab_ = true;
			last_tab_word_ = lcp;
			return;
		}

		printMatches(matches);
		last_was_tab_ = true;
		last_tab_word_ = current;
	}

	void LineEditor::handleTab() {
		Tok tok = currentToken();
		std::string prefix = buffer_.substr(tok.start, tok.end - tok.start);
		auto matches = completionsFor(prefix, tok.first);
		applyCompletion(tok, matches);
	}

	void LineEditor::pasteFromClipboard() {
		if (!OpenClipboard(nullptr)) return;
		HANDLE h_clip = GetClipboardData(CF_UNICODETEXT);
		if (!h_clip) {
			CloseClipboard();
			return;
		}

		auto* wptr = static_cast<const WCHAR*>(GlobalLock(h_clip));
		if (!wptr) {
			CloseClipboard();
			return;
		}

		std::wstring w(wptr);
		GlobalUnlock(h_clip);
		CloseClipboard();

		// Strip CR/LF: a multi-line paste would corrupt our single-line
		// redraw. The user can press Enter themselves between lines.
		std::wstring filtered;
		filtered.reserve(w.size());
		for (WCHAR c : w) {
			if (c == L'\r' || c == L'\n') continue;
			filtered.push_back(c);
		}

		if (filtered.empty()) return;

		int n = WideCharToMultiByte(CP_UTF8, 0,
			filtered.data(), (int)filtered.size(),
			nullptr, 0, nullptr, nullptr);
		if (n <= 0) return;
		std::string utf8(static_cast<std::size_t>(n), '\0');
		WideCharToMultiByte(CP_UTF8, 0,
			filtered.data(), (int)filtered.size(),
			utf8.data(), n, nullptr, nullptr);
		insertChars(utf8);
		redraw();
	}

	void LineEditor::insertWideCharFromConsole(wchar_t ch) {
		WCHAR pair[2] = { static_cast<WCHAR>(ch), 0 };
		int len = 1;
		if (ch >= 0xD800 && ch <= 0xDBFF) {
			HANDLE h_in = GetStdHandle(STD_INPUT_HANDLE);
			INPUT_RECORD r2;
			DWORD nr = 0;
			if (h_in != INVALID_HANDLE_VALUE
			    && ReadConsoleInputW(h_in, &r2, 1, &nr) && nr == 1
			    && r2.EventType == KEY_EVENT
			    && r2.Event.KeyEvent.bKeyDown) {
				pair[1] = r2.Event.KeyEvent.uChar.UnicodeChar;
				len = 2;
			}
		}

		char buf[8] = {};
		int n = WideCharToMultiByte(CP_UTF8, 0, pair, len, buf, sizeof(buf), nullptr, nullptr);
		if (n > 0) {
			insertChars(std::string(buf, n));
			redraw();
		}
	}

	static bool isAltGrActive(const KEY_EVENT_RECORD& k);

	void LineEditor::revsearchRefresh(const std::string& query,
	                                  std::size_t match_index) {
		std::string p = "(reverse-i-search)`" + query + "': ";
		prompt_raw_ = p;
		prompt_visible_len_ = visibleLen(p);
		const auto& history = exec_.history();
		if (match_index < history.size()) {
			buffer_ = history[match_index];
			std::size_t pos = buffer_.find(query);
			cursor_ = (pos == std::string::npos)
				? buffer_.size()
				: pos + query.size();
		} else {
			buffer_.clear();
			cursor_ = 0;
		}

		last_cursor_row_ = 0;
		emit("\r\x1b[J");
		redraw();
	}

	void LineEditor::revsearchStepOlder(const std::string& query,
	                                    std::size_t& match_index) {
		const auto& history = exec_.history();
		if (match_index == 0) { emit("\a"); return; }
		std::size_t start = (match_index < history.size())
			? match_index - 1
			: (history.empty() ? 0 : history.size() - 1);
		std::size_t next = findReverseSearchMatch(history, query, start, false);
		if (next == history.size()) { emit("\a"); return; }
		match_index = next;
		revsearchRefresh(query, match_index);
	}

	void LineEditor::revsearchStepNewer(const std::string& query,
	                                    std::size_t& match_index) {
		const auto& history = exec_.history();
		if (match_index >= history.size() || match_index + 1 >= history.size()) {
			emit("\a");
			return;
		}

		std::size_t next = findReverseSearchMatch(history, query,
		                                          match_index + 1, true);
		if (next == history.size()) { emit("\a"); return; }
		match_index = next;
		revsearchRefresh(query, match_index);
	}

	bool LineEditor::revsearchHandleKey(const KEY_EVENT_RECORD& k,
	                                    std::string& query,
	                                    std::size_t& match_index,
	                                    bool& cancel, bool& submit) {
		const bool altgr = isAltGrActive(k);
		const bool is_ctrl = !altgr
			&& (k.dwControlKeyState
			    & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
		const WCHAR ch = k.uChar.UnicodeChar;
		const auto& history = exec_.history();

		if (k.wVirtualKeyCode == VK_RETURN) { submit = true; return false; }
		if (k.wVirtualKeyCode == VK_ESCAPE) { cancel = true; return false; }
		if (k.wVirtualKeyCode == VK_BACK) {
			if (query.empty()) { emit("\a"); return true; }
			query.pop_back();
			std::size_t start = history.empty() ? 0 : history.size() - 1;
			match_index = findReverseSearchMatch(history, query, start, false);
			revsearchRefresh(query, match_index);
			return true;
		}

		if (is_ctrl && k.wVirtualKeyCode == 'R') {
			revsearchStepOlder(query, match_index);
			return true;
		}

		if (is_ctrl && k.wVirtualKeyCode == 'S') {
			revsearchStepNewer(query, match_index);
			return true;
		}

		if (is_ctrl && (k.wVirtualKeyCode == 'G' || k.wVirtualKeyCode == 'C')) {
			cancel = true;
			return false;
		}

		if (is_ctrl) return false;
		if (ch >= 0x20 && ch < 0x7F) {
			query.push_back(static_cast<char>(ch));
			std::size_t start = (match_index < history.size())
				? match_index
				: (history.empty() ? 0 : history.size() - 1);
			match_index = findReverseSearchMatch(history, query, start, false);
			revsearchRefresh(query, match_index);
			return true;
		}

		return false;
	}

	void LineEditor::runReverseSearch(std::string& out, bool& done, bool& eof) {
		std::string saved_buffer = buffer_;
		std::size_t saved_cursor = cursor_;
		std::string saved_prompt = prompt_raw_;
		std::size_t saved_visible = prompt_visible_len_;

		revsearch_active_ = true;
		std::string query;
		std::size_t match_index = exec_.history().size();
		revsearchRefresh(query, match_index);

		HANDLE h_in = GetStdHandle(STD_INPUT_HANDLE);
		bool cancel = false;
		bool submit = false;
		bool looping = true;
		while (looping) {
			INPUT_RECORD rec;
			DWORD nread = 0;
			if (h_in == INVALID_HANDLE_VALUE
			    || !ReadConsoleInputW(h_in, &rec, 1, &nread)
			    || nread == 0) {
				cancel = true; eof = true; break;
			}

			if (rec.EventType != KEY_EVENT) continue;
			KEY_EVENT_RECORD& k = rec.Event.KeyEvent;
			if (!k.bKeyDown) { handleAltKeyUp(k); continue; }
			looping = revsearchHandleKey(k, query, match_index, cancel, submit);
		}

		prompt_raw_ = saved_prompt;
		prompt_visible_len_ = saved_visible;
		if (cancel) { buffer_ = saved_buffer; cursor_ = saved_cursor; }
		last_cursor_row_ = 0;
		revsearch_active_ = false;
		emit("\r\x1b[J");
		emit(prompt_raw_);
		redraw();
		if (submit) {
			emit("\r\n");
			out = buffer_;
			done = true;
		}
	}

	static DWORD enterRawInputMode(HANDLE h_in) {
		DWORD saved = 0;
		GetConsoleMode(h_in, &saved);
		DWORD in_mode = saved;
		in_mode &= ~ENABLE_LINE_INPUT;
		in_mode &= ~ENABLE_ECHO_INPUT;
		in_mode &= ~ENABLE_PROCESSED_INPUT;
		in_mode &= ~ENABLE_MOUSE_INPUT;
		in_mode &= ~ENABLE_WINDOW_INPUT;
		SetConsoleMode(h_in, in_mode);
		return saved;
	}

	// AltGr arrives as LEFT_CTRL + RIGHT_ALT with a layout-resolved char;
	// without this check its LEFT_CTRL bit would read as Ctrl+<key>.
	static bool isAltGrActive(const KEY_EVENT_RECORD& k) {
		const DWORD ctrl = k.dwControlKeyState;
		return (ctrl & (LEFT_CTRL_PRESSED | RIGHT_ALT_PRESSED))
		           == (LEFT_CTRL_PRESSED | RIGHT_ALT_PRESSED)
		    && k.uChar.UnicodeChar != 0;
	}

	// Alt-code input (Alt + numpad digits) is delivered as a key-UP event
	// for VK_MENU carrying the resolved Unicode char.
	void LineEditor::handleAltKeyUp(const KEY_EVENT_RECORD& k) {
		if (k.wVirtualKeyCode != VK_MENU) return;
		if (k.uChar.UnicodeChar < 0x20) return;
		const WCHAR ach = k.uChar.UnicodeChar;
		char buf[8] = {};
		const int n = WideCharToMultiByte(CP_UTF8, 0, &ach, 1,
			buf, sizeof(buf), nullptr, nullptr);
		if (n > 0) {
			insertChars(std::string(buf, n));
			redraw();
		}
	}

	bool LineEditor::handleCtrlKey(const KEY_EVENT_RECORD& k,
	                               const std::string& prompt,
	                               std::string& out, bool& done, bool& eof) {
		switch (k.wVirtualKeyCode) {
		case 'C':
			emit("^C\r\n");
			buffer_.clear();
			cursor_ = 0;
			last_cursor_row_ = 0;
			emit(prompt_raw_);
			(void)prompt;
			return true;
		case 'D':
			if (buffer_.empty()) { eof = true; done = true; }
			else { handleDelete(); redraw(); }
			return true;
		case 'A': cursor_ = 0;              redraw(); return true;
		case 'E': cursor_ = buffer_.size(); redraw(); return true;
		case 'K': handleKillToEnd();        redraw(); return true;
		case 'U': handleKillToStart();      redraw(); return true;
		case 'W': handleKillWordBack();     redraw(); return true;
		case 'L': handleClearScreen();                return true;
		case 'B': if (cursor_ > 0)              { --cursor_; redraw(); } return true;
		case 'F': if (cursor_ < buffer_.size()) { ++cursor_; redraw(); } return true;
		case 'P': handleHistoryUp();        redraw(); return true;
		case 'N': handleHistoryDown();      redraw(); return true;
		case 'V': pasteFromClipboard();               return true;
		case 'R': runReverseSearch(out, done, eof);   return true;
		default:  return false;
		}
	}

	bool LineEditor::handleNavigationKey(const KEY_EVENT_RECORD& k,
	                                     std::string& out, bool& done, bool& was_tab) {
		switch (k.wVirtualKeyCode) {
		case VK_RETURN:  handleEnter(out, done);                                     return true;
		case VK_BACK:    handleBackspace(); redraw();                                 return true;
		case VK_DELETE:  handleDelete();    redraw();                                 return true;
		case VK_TAB:     handleTab();       redraw(); was_tab = true;                 return true;
		case VK_LEFT:    if (cursor_ > 0)              { --cursor_; redraw(); }      return true;
		case VK_RIGHT:   handleRightArrow();                                          return true;
		case VK_UP:      handleHistoryUp();   redraw();                              return true;
		case VK_DOWN:    handleHistoryDown(); redraw();                              return true;
		case VK_HOME:    cursor_ = 0;              redraw();                         return true;
		case VK_END:     cursor_ = buffer_.size(); redraw();                         return true;
		default:                                                                      return false;
		}
	}

	void LineEditor::insertReceivedChar(WCHAR ch) {
		if (ch == 0) return;
		if (ch >= 0x20 && ch < 0x7F) {
			insertChars(std::string(1, static_cast<char>(ch)));
			redraw();
			return;
		}

		if (ch >= 0x80) {
			insertWideCharFromConsole(ch);
		}
	}

	bool LineEditor::readLineRaw(const std::string& prompt, std::string& out) {
		HANDLE h_in  = GetStdHandle(STD_INPUT_HANDLE);
		HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
		if (h_in == INVALID_HANDLE_VALUE || h_out == INVALID_HANDLE_VALUE) {
			return readLineCooked(out);
		}

		const DWORD saved_in_mode = enterRawInputMode(h_in);

		emit(prompt);
		bool done = false;
		bool eof = false;
		while (!done) {
			INPUT_RECORD rec;
			DWORD nread = 0;
			if (!ReadConsoleInputW(h_in, &rec, 1, &nread) || nread == 0) {
				eof = true;
				break;
			}

			if (rec.EventType != KEY_EVENT) continue;
			KEY_EVENT_RECORD& k = rec.Event.KeyEvent;
			if (!k.bKeyDown) {
				handleAltKeyUp(k);
				continue;
			}

			const bool altgr = isAltGrActive(k);
			const bool is_ctrl = !altgr
				&& (k.dwControlKeyState
				    & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
			const WCHAR ch = k.uChar.UnicodeChar;
			bool was_tab = false;

			if (handleNavigationKey(k, out, done, was_tab)) {
				last_was_tab_ = was_tab;
				continue;
			}

			if (is_ctrl && handleCtrlKey(k, prompt, out, done, eof)) {
				last_was_tab_ = was_tab;
				continue;
			}

			insertReceivedChar(ch);
			last_was_tab_ = was_tab;
		}

		SetConsoleMode(h_in, saved_in_mode);
		return !eof || !out.empty();
	}

#else  // !_WIN32

	bool LineEditor::readLineRaw(const std::string& prompt, std::string& out) {
		std::fputs(prompt.c_str(), stdout);
		std::fflush(stdout);
		return readLineCooked(out);
	}

	void LineEditor::emit(const std::string&) {}
	void LineEditor::redraw() {}
	void LineEditor::handleEnter(std::string&, bool& d) { d = true; }
	void LineEditor::handleBackspace() {}
	void LineEditor::handleDelete() {}
	void LineEditor::handleTab() {}
	void LineEditor::handleHistoryUp() {}
	void LineEditor::handleHistoryDown() {}
	void LineEditor::handleKillToEnd() {}
	void LineEditor::handleKillToStart() {}
	void LineEditor::handleKillWordBack() {}
	void LineEditor::handleClearScreen() {}
	void LineEditor::insertChars(const std::string&) {}
	LineEditor::Tok LineEditor::currentToken() const { return {0, 0, true}; }
	std::vector<std::string> LineEditor::commandCompletions(const std::string&) { return {}; }
	std::vector<std::string> LineEditor::pathCompletions(const std::string&) { return {}; }
	std::vector<std::string> LineEditor::completionsFor(const std::string&, bool) { return {}; }
	std::string LineEditor::longestCommonPrefix(const std::vector<std::string>&) { return {}; }
	void LineEditor::printMatches(const std::vector<std::string>&) {}
	void LineEditor::applyCompletion(const Tok&, const std::vector<std::string>&) {}
	void LineEditor::pasteFromClipboard() {}
	void LineEditor::insertWideCharFromConsole(wchar_t) {}
	void LineEditor::runReverseSearch(std::string&, bool&, bool&) {}
	void LineEditor::revsearchRefresh(const std::string&, std::size_t) {}
	void LineEditor::refreshSuggestion() {}
	bool LineEditor::acceptInlineSuggestion() { return false; }
	void LineEditor::handleRightArrow() {}

#endif /* _WIN32 */

}  // namespace wbsh

#include "lineedit.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <io.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace wbsh {

	namespace {

		namespace fs = std::filesystem;

#ifdef _WIN32
		bool stdinIsTty() {
			return _isatty(_fileno(stdin)) != 0;
		}

		// Compute "visible length" of a prompt — strips ANSI CSI sequences
		// and the bash \[...\] non-printing markers (which our expandPrompt
		// already removes, but be paranoid).
		std::size_t visibleLen(const std::string& s) {
			std::size_t n = 0;
			std::size_t i = 0;
			while (i < s.size()) {
				if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
					i += 2;
					while (i < s.size()
					       && !((s[i] >= '@' && s[i] <= '~'))) ++i;
					if (i < s.size()) ++i;
					continue;
				}
				++n;
				++i;
			}
			return n;
		}
#endif

		bool isWordBreak(char c) {
			return std::isspace(static_cast<unsigned char>(c))
			    || c == '|' || c == '&' || c == ';'
			    || c == '<' || c == '>'
			    || c == '(' || c == ')';
		}

	}  // namespace

	LineEditor::LineEditor(Environment& env, Executor& exec)
		: env_(env), exec_(exec) {}

	bool LineEditor::readLine(const std::string& prompt, std::string& out) {
		buffer_.clear();
		cursor_ = 0;
		history_pos_ = 0;
		saved_partial_.clear();
		last_was_tab_ = false;
		last_tab_word_.clear();
		prompt_raw_ = prompt;
#ifdef _WIN32
		prompt_visible_len_ = visibleLen(prompt);
		if (stdinIsTty()) {
			return readLineRaw(prompt, out);
		}
#endif
		// Cooked / piped fallback: emit the prompt and read a line via fgetc.
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
		// Single-line redraw via VT escapes:
		//   \r              go to start of current row
		//   prompt + buffer
		//   \x1b[K          clear to end of line
		//   \x1b[<n>D       reposition cursor if not at end
		std::string out;
		out.push_back('\r');
		out += prompt_raw_;
		out += buffer_;
		out += "\x1b[K";
		if (cursor_ < buffer_.size()) {
			out += "\x1b[" + std::to_string(buffer_.size() - cursor_) + "D";
		}
		emit(out);
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
		// Move cursor home + clear screen + clear scrollback, then redraw.
		emit("\x1b[H\x1b[2J\x1b[3J");
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
		// History is recorded by the REPL caller (so raw and cooked paths
		// agree); we just hand back the line.
		done = true;
	}

	// ---- Completion ----------------------------------------------------------

	LineEditor::Tok LineEditor::currentToken() const {
		Tok t{ cursor_, cursor_, true };
		std::size_t i = cursor_;
		while (i > 0 && !isWordBreak(buffer_[i - 1])) --i;
		t.start = i;
		t.end = cursor_;
		// Determine whether the position is "command-position" — i.e. the
		// previous non-space char is a command separator (or BOL).
		std::size_t k = t.start;
		while (k > 0 && std::isspace(static_cast<unsigned char>(buffer_[k - 1]))) --k;
		if (k == 0) { t.first = true; return t; }
		char prev = buffer_[k - 1];
		t.first = (prev == '|' || prev == '&' || prev == ';'
		        || prev == '(' || prev == '{');
		return t;
	}

	std::vector<std::string> LineEditor::commandCompletions(const std::string& prefix) {
		std::set<std::string> set;
		for (const auto& n : exec_.builtinNames()) {
			if (n.compare(0, prefix.size(), prefix) == 0) set.insert(n);
		}
		for (const auto& n : exec_.functionNames()) {
			if (n.compare(0, prefix.size(), prefix) == 0) set.insert(n);
		}
		// Walk PATH for executables whose names start with `prefix`.
		std::string path = env_.get("PATH");
		std::vector<std::string> dirs;
		std::string cur;
		for (std::size_t i = 0; i < path.size(); ++i) {
			char c = path[i];
			if (c == ';') { dirs.push_back(cur); cur.clear(); }
			else if (c == ':') {
				if (cur.size() == 1 && std::isalpha((unsigned char)cur[0])) cur.push_back(c);
				else { dirs.push_back(cur); cur.clear(); }
			} else cur.push_back(c);
		}
		if (!cur.empty()) dirs.push_back(cur);

		auto stripExt = [](std::string s) -> std::string {
			static const char* exts[] = { ".exe", ".cmd", ".bat", ".com", nullptr };
			std::string lo = s;
			std::transform(lo.begin(), lo.end(), lo.begin(),
				[](char c) { return static_cast<char>(std::tolower((unsigned char)c)); });
			for (int i = 0; exts[i]; ++i) {
				std::size_t L = std::strlen(exts[i]);
				if (lo.size() > L && lo.compare(lo.size() - L, L, exts[i]) == 0) {
					s.resize(s.size() - L);
					break;
				}
			}
			return s;
		};

		for (const auto& d : dirs) {
			if (d.empty()) continue;
			std::error_code ec;
			fs::path base(exec_.pathConv().toWin32(d));
			fs::directory_iterator it(base, ec);
			if (ec) continue;
			for (const auto& de : it) {
				std::string name;
				try { name = de.path().filename().string(); }
				catch (...) { continue; }
				std::string nice = stripExt(name);
				if (nice.compare(0, prefix.size(), prefix) == 0) {
					set.insert(nice);
				}
			}
		}
		return std::vector<std::string>(set.begin(), set.end());
	}

	std::vector<std::string> LineEditor::pathCompletions(const std::string& prefix) {
		// Split prefix into dir + base.
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
		std::string native = exec_.pathConv().toWin32(dir);
		std::vector<std::string> matches;
		std::error_code ec;
		fs::directory_iterator it(native, ec);
		if (ec) return matches;
		for (const auto& de : it) {
			std::string name;
			try { name = de.path().filename().string(); }
			catch (...) { continue; }
			if (name.empty()) continue;
			if (name.compare(0, base.size(), base) != 0) continue;
			// Hide dotfiles unless the user is explicitly typing a leading dot.
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
		// Treat as a path if the prefix contains `/` (even at command pos —
		// matches bash's `./foo<TAB>` UX).
		if (cmd && prefix.find('/') == std::string::npos) {
			return commandCompletions(prefix);
		}
		// Argument-position completion: try tool-aware completions first.
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
		// Programmable completion (`complete -W ...` / `complete -F ...`)
		// takes precedence over the built-in tool tables.
		if (auto* spec = exec_.completionSpec(head)) {
			std::vector<std::string> matches;
			for (const auto& w : spec->words) {
				if (w.compare(0, prefix.size(), prefix) == 0) matches.push_back(w);
			}
			if (!spec->function.empty() && exec_.isFunction(spec->function)) {
				// Call the function with COMP_WORDS / COMP_CWORD set, then
				// read COMPREPLY into matches.
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
				try {
					std::vector<std::string> args = { head, prefix,
						prev.size() > 1 ? prev.back() : std::string() };
					exec_.callFunction(spec->function, args);
				} catch (...) { /* swallow */ }
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

	std::vector<std::string> LineEditor::gitBranches() {
		namespace fs = std::filesystem;
		std::vector<std::string> branches;
		std::error_code ec;
		fs::path cur = fs::current_path(ec);
		if (ec) return branches;
		fs::path gitdir;
		fs::path probe = cur;
		while (true) {
			fs::path candidate = probe / ".git";
			if (fs::exists(candidate, ec)) { gitdir = candidate; break; }
			fs::path parent = probe.parent_path();
			if (parent == probe) break;
			probe = parent;
		}
		if (gitdir.empty()) return branches;
		// `.git` may be a worktree pointer file.
		if (!fs::is_directory(gitdir, ec)) {
			std::ifstream f(gitdir);
			std::string line;
			if (std::getline(f, line)) {
				const std::string prefix = "gitdir: ";
				if (line.compare(0, prefix.size(), prefix) == 0) {
					fs::path g(line.substr(prefix.size()));
					if (!g.is_absolute()) g = (probe / g).lexically_normal();
					gitdir = g;
				}
			}
		}
		// Loose refs.
		fs::path heads = gitdir / "refs" / "heads";
		if (fs::is_directory(heads, ec)) {
			fs::recursive_directory_iterator it(heads, ec);
			if (!ec) {
				for (auto p = it; p != fs::recursive_directory_iterator(); p.increment(ec)) {
					if (ec) break;
					std::error_code fec;
					if (p->is_regular_file(fec)) {
						auto rel = fs::relative(p->path(), heads, fec).string();
						std::replace(rel.begin(), rel.end(), '\\', '/');
						branches.push_back(std::move(rel));
					}
				}
			}
		}
		// Packed refs.
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
		std::sort(branches.begin(), branches.end());
		branches.erase(std::unique(branches.begin(), branches.end()), branches.end());
		return branches;
	}

	namespace {
		std::vector<std::string> filterPrefix(const char* const* table,
		                                      const std::string& prefix) {
			std::vector<std::string> out;
			for (int i = 0; table[i]; ++i) {
				std::string s = table[i];
				if (s.compare(0, prefix.size(), prefix) == 0) out.push_back(std::move(s));
			}
			return out;
		}
	}

	std::vector<std::string> LineEditor::gitCompletions(const std::string& prefix,
	                                                    const std::vector<std::string>& prev) {
		// 2nd token = subcommand position.
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
		// Branch-aware subcommands.
		const std::string& sub = prev[1];
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
		std::vector<std::string> out;
		for (auto& b : gitBranches()) {
			if (b.compare(0, prefix.size(), prefix) == 0) out.push_back(b);
		}
		return out;
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
		// Subcommand-specific second-level completions.
		if (prev.size() == 2) {
			const std::string& sub = prev[1];
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
		}
		return {};
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
		// `npm run <TAB>`: read scripts from package.json in CWD.
		if (prev.size() == 2 && (prev[1] == "run" || prev[1] == "run-script")) {
			std::vector<std::string> scripts;
			std::error_code ec;
			std::filesystem::path pkg = std::filesystem::current_path(ec) / "package.json";
			std::ifstream f(pkg);
			if (f) {
				std::string content((std::istreambuf_iterator<char>(f)),
				                    std::istreambuf_iterator<char>());
				auto sp = content.find("\"scripts\"");
				if (sp != std::string::npos) {
					auto open = content.find('{', sp);
					auto close = content.find('}', open);
					if (open != std::string::npos && close != std::string::npos) {
						std::string body = content.substr(open + 1, close - open - 1);
						for (std::size_t i = 0; i < body.size(); ) {
							if (body[i] != '"') { ++i; continue; }
							std::size_t end = body.find('"', i + 1);
							if (end == std::string::npos) break;
							std::string name = body.substr(i + 1, end - i - 1);
							i = body.find(':', end);
							if (i == std::string::npos) break;
							std::size_t vstart = body.find('"', i);
							if (vstart == std::string::npos) break;
							std::size_t vend = body.find('"', vstart + 1);
							if (vend == std::string::npos) break;
							if (name.compare(0, prefix.size(), prefix) == 0)
								scripts.push_back(std::move(name));
							i = vend + 1;
						}
					}
				}
			}
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
		// Resource types after `kubectl get|describe|delete|edit|...`.
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
			// Add a trailing space when the completion isn't a directory.
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
		// No further common prefix — display the candidates.
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

	bool LineEditor::readLineRaw(const std::string& prompt, std::string& out) {
		HANDLE h_in  = GetStdHandle(STD_INPUT_HANDLE);
		HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
		if (h_in == INVALID_HANDLE_VALUE || h_out == INVALID_HANDLE_VALUE) {
			return readLineCooked(out);
		}
		DWORD saved_in_mode = 0, saved_out_mode = 0;
		GetConsoleMode(h_in,  &saved_in_mode);
		GetConsoleMode(h_out, &saved_out_mode);

		// Raw input: keep VT processing for input on (so escape sequences
		// from the OS are handled), but turn OFF line input + echo +
		// processed-input so we see every keystroke. Output mode keeps VT
		// processing (set up at REPL boot).
		DWORD in_mode = saved_in_mode;
		in_mode &= ~ENABLE_LINE_INPUT;
		in_mode &= ~ENABLE_ECHO_INPUT;
		in_mode &= ~ENABLE_PROCESSED_INPUT;
		// Mouse / window events would interleave with key events; suppress.
		in_mode &= ~ENABLE_MOUSE_INPUT;
		in_mode &= ~ENABLE_WINDOW_INPUT;
		SetConsoleMode(h_in, in_mode);

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
			if (!k.bKeyDown) continue;

			DWORD ctrl = k.dwControlKeyState;
			bool is_ctrl  = (ctrl & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
			bool is_alt   = (ctrl & (LEFT_ALT_PRESSED  | RIGHT_ALT_PRESSED))  != 0;
			(void)is_alt;
			WCHAR ch = k.uChar.UnicodeChar;
			bool was_tab = false;

			switch (k.wVirtualKeyCode) {
			case VK_RETURN:
				handleEnter(out, done);
				break;
			case VK_BACK:
				handleBackspace();
				redraw();
				break;
			case VK_DELETE:
				handleDelete();
				redraw();
				break;
			case VK_TAB:
				handleTab();
				redraw();
				was_tab = true;
				break;
			case VK_LEFT:
				if (cursor_ > 0) { --cursor_; redraw(); }
				break;
			case VK_RIGHT:
				if (cursor_ < buffer_.size()) { ++cursor_; redraw(); }
				break;
			case VK_UP:
				handleHistoryUp();
				redraw();
				break;
			case VK_DOWN:
				handleHistoryDown();
				redraw();
				break;
			case VK_HOME:
				cursor_ = 0;
				redraw();
				break;
			case VK_END:
				cursor_ = buffer_.size();
				redraw();
				break;
			default:
				if (is_ctrl && (k.wVirtualKeyCode == 'C')) {
					emit("^C\r\n");
					buffer_.clear();
					cursor_ = 0;
					emit(prompt_raw_);
					break;
				}
				if (is_ctrl && (k.wVirtualKeyCode == 'D')) {
					if (buffer_.empty()) { eof = true; done = true; }
					else { handleDelete(); redraw(); }
					break;
				}
				if (is_ctrl && (k.wVirtualKeyCode == 'A')) { cursor_ = 0; redraw(); break; }
				if (is_ctrl && (k.wVirtualKeyCode == 'E')) { cursor_ = buffer_.size(); redraw(); break; }
				if (is_ctrl && (k.wVirtualKeyCode == 'K')) { handleKillToEnd(); redraw(); break; }
				if (is_ctrl && (k.wVirtualKeyCode == 'U')) { handleKillToStart(); redraw(); break; }
				if (is_ctrl && (k.wVirtualKeyCode == 'W')) { handleKillWordBack(); redraw(); break; }
				if (is_ctrl && (k.wVirtualKeyCode == 'L')) { handleClearScreen(); break; }
				if (is_ctrl && (k.wVirtualKeyCode == 'B')) { if (cursor_>0) { --cursor_; redraw(); } break; }
				if (is_ctrl && (k.wVirtualKeyCode == 'F')) { if (cursor_<buffer_.size()) { ++cursor_; redraw(); } break; }
				if (is_ctrl && (k.wVirtualKeyCode == 'P')) { handleHistoryUp(); redraw(); break; }
				if (is_ctrl && (k.wVirtualKeyCode == 'N')) { handleHistoryDown(); redraw(); break; }

				if (ch == 0) break;   // pure modifier press
				if (ch >= 0x20 && ch < 0x7F) {
					char c = static_cast<char>(ch);
					insertChars(std::string(1, c));
					redraw();
				} else if (ch >= 0x80) {
					// Convert UTF-16 to UTF-8 (handle surrogate pairs).
					WCHAR pair[2] = { ch, 0 };
					int len = 1;
					if (ch >= 0xD800 && ch <= 0xDBFF) {
						// high surrogate — read next event for the low
						// surrogate.
						INPUT_RECORD r2;
						DWORD nr = 0;
						if (ReadConsoleInputW(h_in, &r2, 1, &nr) && nr == 1
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
				break;
			}
			last_was_tab_ = was_tab;
		}

		SetConsoleMode(h_in, saved_in_mode);
		(void)saved_out_mode;
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

#endif  // _WIN32

}  // namespace wbsh

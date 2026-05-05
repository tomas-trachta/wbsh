#include "ast.h"
#include "environment.h"
#include "executor.h"
#include "expander.h"
#include "lexer.h"
#include "lineedit.h"
#include "parser.h"
#include "pathconv.h"
#include "printer.h"

#include <atomic>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <shellapi.h>
#  include <dwmapi.h>
#  include <io.h>
#  pragma comment(lib, "shell32.lib")
#  pragma comment(lib, "dwmapi.lib")
// 20 since Win10 1903; defined in newer SDKs but we don't rely on the SDK version.
#  ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#    define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#  endif
// 19 was the pre-1903 value; attempted as a fallback on older builds.
#  ifndef DWMWA_USE_IMMERSIVE_DARK_MODE_OLD
#    define DWMWA_USE_IMMERSIVE_DARK_MODE_OLD 19
#  endif
#  ifndef DWMWA_CAPTION_COLOR
#    define DWMWA_CAPTION_COLOR 35
#  endif
#  ifndef DWMWA_BORDER_COLOR
#    define DWMWA_BORDER_COLOR 34
#  endif
#endif

#include <ctime>
#include <filesystem>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using namespace wbsh;

namespace {

	void printHelp() {
		std::cout <<
			"wbsh -- bash front-end + executor (Windows)\n"
			"\n"
			"usage:\n"
			"  wbsh                          interactive shell (TTY auto-detect)\n"
			"  wbsh [opts] -c <command>      run / dump the given string\n"
			"  wbsh [opts] <file>            run / dump the file (- for stdin)\n"
			"\n"
			"modes (default for files = dump AST):\n"
			"  -i, --interactive             force interactive REPL\n"
			"  -r, --run                     actually execute the script\n"
			"  -e, --expand                  walk the AST and dump expanded words\n"
			"  -t, --tokens                  dump the token stream\n"
			"  --no-ast                      suppress the AST dump\n"
			"  -h, --help                    show this help\n";
	}

	void walkAndExpand(const Node& n, Expander& exp, std::ostream& os, int depth);

	void indent(std::ostream& os, int n) {
		for (int i = 0; i < n; ++i) os << "  ";
	}

	void escape(std::ostream& os, const std::string& s) {
		os << '"';
		for (char c : s) {
			switch (c) {
			case '\n': os << "\\n"; break;
			case '\t': os << "\\t"; break;
			case '\r': os << "\\r"; break;
			case '\\': os << "\\\\"; break;
			case '"':  os << "\\\""; break;
			default:   os << c;
			}
		}
		os << '"';
	}

	void expandSimpleCommand(const SimpleCommand& sc, Expander& exp,
	                         Environment& env,
	                         std::ostream& os, int depth) {
		indent(os, depth);
		os << "SimpleCommand @ " << sc.loc.line << ":" << sc.loc.column << "\n";
		// Commit assignments to env when this is a "pure" assignment command
		// (no command words). Mirrors bash's `a=1` (set) vs `a=1 cmd` (per-call).
		bool commit = sc.words.empty();
		for (const auto& a : sc.assignments) {
			indent(os, depth + 1);
			std::string v;
			try { v = exp.expandStringValue(a.value); }
			catch (const ExpandError& e) { os << "<expand error: " << e.what() << ">"; }
			if (commit) env.set(a.name, v);
			os << "Assign " << a.name << "=";
			escape(os, v);
			os << "\n";
		}
		for (const auto& w : sc.words) {
			indent(os, depth + 1);
			os << "Word raw=";
			escape(os, w.raw);
			os << " ->";
			try {
				auto fields = exp.expandWord(w);
				if (fields.empty()) {
					os << " (vanished)";
				}
				for (const auto& f : fields) {
					os << ' ';
					escape(os, f);
				}
			} catch (const ExpandError& e) {
				os << " <expand error: " << e.what() << ">";
			}
			os << "\n";
		}
		for (const auto& r : sc.redirs) {
			indent(os, depth + 1);
			os << "Redir " << redirOpName(r.op);
			if (r.fd != -1) os << " fd=" << r.fd;
			os << " target=";
			try { escape(os, exp.expandStringValue(r.target)); }
			catch (const ExpandError& e) { os << "<expand error: " << e.what() << ">"; }
			if (r.op == RedirOp::DLess || r.op == RedirOp::DLessDash) {
				os << " body=";
				escape(os, exp.expandHeredoc(r.heredoc_body, r.heredoc_quoted));
			}
			os << "\n";
		}
	}

	void walkAndExpand(const Node& n, Expander& exp, Environment& env,
	                   std::ostream& os, int depth) {
		switch (n.kind) {
		case Node::Kind::SimpleCommand:
			expandSimpleCommand(static_cast<const SimpleCommand&>(n), exp, env, os, depth);
			break;
		case Node::Kind::Pipeline: {
			const auto& p = static_cast<const Pipeline&>(n);
			indent(os, depth); os << "Pipeline" << (p.bang ? " (bang)" : "") << "\n";
			for (const auto& c : p.commands) walkAndExpand(*c, exp, env, os, depth + 1);
			break;
		}
		case Node::Kind::AndOr: {
			const auto& a = static_cast<const AndOr&>(n);
			indent(os, depth); os << (a.op == AndOr::Op::AndIf ? "&&" : "||") << "\n";
			walkAndExpand(*a.left, exp, env, os, depth + 1);
			walkAndExpand(*a.right, exp, env, os, depth + 1);
			break;
		}
		case Node::Kind::List: {
			const auto& l = static_cast<const List&>(n);
			for (const auto& it : l.items) walkAndExpand(*it.command, exp, env, os, depth);
			break;
		}
		case Node::Kind::BraceGroup: {
			const auto& bg = static_cast<const BraceGroup&>(n);
			indent(os, depth); os << "BraceGroup\n";
			if (bg.body) walkAndExpand(*bg.body, exp, env, os, depth + 1);
			break;
		}
		case Node::Kind::Subshell: {
			const auto& ss = static_cast<const Subshell&>(n);
			indent(os, depth); os << "Subshell\n";
			if (ss.body) walkAndExpand(*ss.body, exp, env, os, depth + 1);
			break;
		}
		case Node::Kind::IfClause: {
			const auto& ic = static_cast<const IfClause&>(n);
			indent(os, depth); os << "If\n";
			for (std::size_t i = 0; i < ic.branches.size(); ++i) {
				indent(os, depth + 1);
				os << (i == 0 ? "if-cond:" : "elif-cond:") << "\n";
				if (ic.branches[i].cond) walkAndExpand(*ic.branches[i].cond, exp, env, os, depth + 2);
				indent(os, depth + 1); os << "then:\n";
				if (ic.branches[i].body) walkAndExpand(*ic.branches[i].body, exp, env, os, depth + 2);
			}
			if (ic.else_body) {
				indent(os, depth + 1); os << "else:\n";
				walkAndExpand(*ic.else_body, exp, env, os, depth + 2);
			}
			break;
		}
		case Node::Kind::WhileClause: {
			const auto& w = static_cast<const WhileClause&>(n);
			indent(os, depth); os << (w.until ? "Until\n" : "While\n");
			indent(os, depth + 1); os << "cond:\n";
			if (w.cond) walkAndExpand(*w.cond, exp, env, os, depth + 2);
			indent(os, depth + 1); os << "do:\n";
			if (w.body) walkAndExpand(*w.body, exp, env, os, depth + 2);
			break;
		}
		case Node::Kind::ForClause: {
			const auto& f = static_cast<const ForClause&>(n);
			indent(os, depth); os << "For " << f.var << "\n";
			if (f.has_in) {
				indent(os, depth + 1); os << "items:";
				for (const auto& w : f.items) {
					try {
						auto fields = exp.expandWord(w);
						for (const auto& s : fields) { os << ' '; escape(os, s); }
					} catch (const ExpandError& e) {
						os << " <error: " << e.what() << ">";
					}
				}
				os << "\n";
			}
			indent(os, depth + 1); os << "do:\n";
			if (f.body) walkAndExpand(*f.body, exp, env, os, depth + 2);
			break;
		}
		case Node::Kind::CaseClause: {
			const auto& c = static_cast<const CaseClause&>(n);
			indent(os, depth); os << "Case subject=";
			try { escape(os, exp.expandStringValue(c.subject)); }
			catch (const ExpandError& e) { os << "<error: " << e.what() << ">"; }
			os << "\n";
			for (const auto& it : c.items) {
				indent(os, depth + 1); os << "patterns:";
				for (const auto& p : it.patterns) {
					try { os << ' '; escape(os, exp.expandStringValue(p)); }
					catch (const ExpandError&) { os << " <err>"; }
				}
				os << "\n";
				if (it.body) {
					indent(os, depth + 2); os << "body:\n";
					walkAndExpand(*it.body, exp, env, os, depth + 3);
				}
			}
			break;
		}
		case Node::Kind::FunctionDef: {
			const auto& f = static_cast<const FunctionDef&>(n);
			indent(os, depth); os << "FunctionDef " << f.name << "\n";
			if (f.body) walkAndExpand(*f.body, exp, env, os, depth + 1);
			break;
		}
		}
	}

	// Probe well-known install locations for git.exe and return the
	// directories that contain it. wbsh ships its own coreutils, but git
	// itself is an external the user installs separately and absolutely
	// expects to find on PATH. When wbsh is launched from Explorer the
	// inherited PATH may miss it; this fills the gap.
	std::vector<std::string> findGitDirs() {
		namespace fs = std::filesystem;
		std::vector<std::string> candidates;
		auto add = [&](const char* var, const char* suffix) {
			if (const char* v = std::getenv(var)) {
				candidates.push_back(std::string(v) + suffix);
			}
		};
		add("ProgramFiles",      "\\Git\\cmd");
		add("ProgramFiles(x86)", "\\Git\\cmd");
		add("ProgramW6432",      "\\Git\\cmd");
		add("LOCALAPPDATA",      "\\Programs\\Git\\cmd");
		if (const char* up = std::getenv("USERPROFILE")) {
			candidates.push_back(std::string(up)
				+ "\\scoop\\apps\\git\\current\\cmd");
			candidates.push_back(std::string(up)
				+ "\\scoop\\shims");
		}
		if (const char* pd = std::getenv("ProgramData")) {
			candidates.push_back(std::string(pd)
				+ "\\chocolatey\\bin");
		}

		std::vector<std::string> hits;
		for (const auto& d : candidates) {
			std::error_code ec;
			fs::path exe = fs::path(d) / "git.exe";
			if (fs::exists(exe, ec) && !fs::is_directory(exe, ec)) {
				hits.push_back(d);
			}
		}
		return hits;
	}

	// Same idea as findGitDirs, for Docker. Docker Desktop installs its CLI
	// under `Program Files\Docker\Docker\resources\bin`, which isn't on the
	// PATH a non-shell parent (Explorer, VS debugger) inherits.
	std::vector<std::string> findDockerDirs() {
		namespace fs = std::filesystem;
		std::vector<std::string> candidates;
		auto add = [&](const char* var, const char* suffix) {
			if (const char* v = std::getenv(var)) {
				candidates.push_back(std::string(v) + suffix);
			}
		};
		add("ProgramFiles",      "\\Docker\\Docker\\resources\\bin");
		add("ProgramFiles(x86)", "\\Docker\\Docker\\resources\\bin");
		add("ProgramW6432",      "\\Docker\\Docker\\resources\\bin");
		if (const char* up = std::getenv("USERPROFILE")) {
			candidates.push_back(std::string(up)
				+ "\\scoop\\apps\\docker\\current");
			candidates.push_back(std::string(up)
				+ "\\scoop\\shims");
		}
		if (const char* pd = std::getenv("ProgramData")) {
			candidates.push_back(std::string(pd)
				+ "\\chocolatey\\bin");
		}

		std::vector<std::string> hits;
		for (const auto& d : candidates) {
			std::error_code ec;
			fs::path exe = fs::path(d) / "docker.exe";
			if (fs::exists(exe, ec) && !fs::is_directory(exe, ec)) {
				hits.push_back(d);
			}
		}
		return hits;
	}

	void prependDirsToPath(Environment& env, const PathConv& pc,
	                       const std::vector<std::string>& dirs) {
		if (dirs.empty()) return;
		std::string path = env.get("PATH");
		std::string prepend;
		for (const auto& d : dirs) {
			std::string posix = pc.toPosix(d);
			// Crude dedup: skip if PATH already contains this entry.
			if (!path.empty() && path.find(posix) != std::string::npos) continue;
			if (!prepend.empty()) prepend.push_back(':');
			prepend += posix;
		}
		if (prepend.empty()) return;
		env.set("PATH", path.empty() ? prepend : (prepend + ":" + path));
	}

	void prepareEnv(Environment& env) {
		env.loadFromProcessEnv();
		PathConv pc;
		std::string p = env.get("PATH");
		if (!p.empty()) env.set("PATH", pc.pathListWin32ToPosix(p));
		// Make `git` and `docker` discoverable when launched standalone (from
		// Explorer or via a non-shell parent). Doesn't affect users whose
		// system PATH already includes them — the dedup check is a no-op there.
		prependDirsToPath(env, pc, findGitDirs());
		prependDirsToPath(env, pc, findDockerDirs());
		std::string home = env.get("HOME");
		if (home.empty()) home = env.get("USERPROFILE");
		if (!home.empty()) {
			env.set("HOME", pc.toPosix(home));
			env.exportVar("HOME");
		}
		std::error_code ec;
		auto cwd = std::filesystem::current_path(ec);
		if (!ec) {
			env.set("PWD", pc.toPosix(pathToUtf8(cwd)));
			env.exportVar("PWD");
		}
	}

	void absorbInheritedState(Environment& env, Executor& exec) {
		// Drop any local-name marker from the parent (we already loaded all
		// values; we just need to fix isExported() before any new spawn).
		std::string locals_marker = env.get("WBSH_LOCAL_NAMES");
		if (!locals_marker.empty()) {
			env.unset("WBSH_LOCAL_NAMES");
			std::size_t i = 0;
			while (i < locals_marker.size()) {
				while (i < locals_marker.size() && locals_marker[i] == ' ') ++i;
				std::size_t start = i;
				while (i < locals_marker.size() && locals_marker[i] != ' ') ++i;
				if (i > start) env.unexportVar(locals_marker.substr(start, i - start));
			}
		}
		std::string inherited_fns = env.get("WBSH_FUNCTIONS");
		if (!inherited_fns.empty()) {
			env.unset("WBSH_FUNCTIONS");
			exec.executeText(inherited_fns, "<inherited functions>");
		}
		std::string inherited_aliases = env.get("WBSH_ALIASES");
		if (!inherited_aliases.empty()) {
			env.unset("WBSH_ALIASES");
			exec.executeText(inherited_aliases, "<inherited aliases>");
		}
	}

	// Resolve the current git branch, walking up from cwd looking for .git.
	// Returns the branch name (e.g. "main"), a short commit hash for detached
	// HEAD, or empty if not inside a repository. Cheap enough to call every
	// prompt redraw — bounded directory walk + one tiny file read.
	std::string detectGitBranch() {
		namespace fs = std::filesystem;
		std::error_code ec;
		fs::path cur = fs::current_path(ec);
		if (ec) return {};

		fs::path git_dir;
		fs::path probe = cur;
		while (true) {
			fs::path candidate = probe / ".git";
			if (fs::exists(candidate, ec)) {
				git_dir = candidate;
				break;
			}
			fs::path parent = probe.parent_path();
			if (parent == probe) break;
			probe = parent;
		}
		if (git_dir.empty()) return {};

		// `.git` may be a regular directory OR a file containing
		// `gitdir: <relative-or-absolute-path>` (worktrees, submodules).
		if (!fs::is_directory(git_dir, ec)) {
			std::ifstream f(git_dir);
			std::string line;
			if (!std::getline(f, line)) return {};
			const std::string prefix = "gitdir: ";
			if (line.compare(0, prefix.size(), prefix) != 0) return {};
			fs::path g(line.substr(prefix.size()));
			if (!g.is_absolute()) g = (probe / g).lexically_normal();
			git_dir = g;
		}

		std::ifstream head(git_dir / "HEAD");
		std::string line;
		if (!std::getline(head, line)) return {};
		// Trim trailing CR/LF.
		while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
			line.pop_back();

		const std::string ref_prefix = "ref: ";
		if (line.compare(0, ref_prefix.size(), ref_prefix) == 0) {
			std::string ref = line.substr(ref_prefix.size());
			const std::string heads = "refs/heads/";
			if (ref.compare(0, heads.size(), heads) == 0) {
				return ref.substr(heads.size());
			}
			return ref;
		}
		// Detached HEAD: line is a 40-char SHA. Show short form.
		if (line.size() >= 7) line.resize(7);
		return line + " (detached)";
	}

	std::string expandPrompt(const std::string& ps, Environment& env, const PathConv& pc) {
		auto cwdStr = []() -> std::string {
			std::error_code ec;
			auto p = std::filesystem::current_path(ec);
			return ec ? std::string(".") : pathToUtf8(p);
		};
		std::string out;
		for (std::size_t i = 0; i < ps.size(); ++i) {
			if (ps[i] != '\\' || i + 1 >= ps.size()) {
				out.push_back(ps[i]);
				continue;
			}
			char nx = ps[++i];
			switch (nx) {
			case 'n':  out.push_back('\n');     break;
			case 'r':  out.push_back('\r');     break;
			case 'a':  out.push_back('\a');     break;
			case 'e':  out.push_back('\x1b');   break;
			case '\\': out.push_back('\\');     break;
			case '$':  out.push_back('$');      break;
			case 's':  out += "wbsh";           break;
			// \[ and \] mark non-printing regions for line-editor width
			// accounting. We don't have a fancy editor, so just drop them.
			case '[':                           break;
			case ']':                           break;
			case 'u': {
				std::string u = env.get("USER");
				if (u.empty()) u = env.get("USERNAME");
				out += u;
				break;
			}
			case 'h':
			case 'H': {
				std::string h = env.get("HOSTNAME");
				if (h.empty()) h = env.get("COMPUTERNAME");
				if (nx == 'h') {
					auto dot = h.find('.');
					if (dot != std::string::npos) h.resize(dot);
				}
				out += h;
				break;
			}
			case 'w': {
				std::string posix = pc.toPosix(cwdStr());
				std::string home = env.get("HOME");
				if (!home.empty() && posix.size() >= home.size()
				    && posix.compare(0, home.size(), home) == 0
				    && (posix.size() == home.size() || posix[home.size()] == '/')) {
					posix = "~" + posix.substr(home.size());
				}
				out += posix;
				break;
			}
			case 'W': {
				std::string posix = pc.toPosix(cwdStr());
				auto slash = posix.rfind('/');
				out += (slash == std::string::npos) ? posix : posix.substr(slash + 1);
				break;
			}
			case 'g': {
				// `\g` — git branch (or empty when not inside a repo).
				// Renders as " (branch)" — yellow when stdout is a TTY,
				// plain otherwise so piped/redirected output stays clean.
				std::string br = detectGitBranch();
				if (br.empty()) break;
#ifdef _WIN32
				bool tty = _isatty(_fileno(stdout)) != 0;
#else
				bool tty = false;
#endif
				if (tty) {
					out += " \x1b[33;1m(";
					out += br;
					out += ")\x1b[0m";
				} else {
					out += " (";
					out += br;
					out += ")";
				}
				break;
			}
			case 't': {
				std::time_t t = std::time(nullptr);
				char buf[16];
				std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
				out += buf;
				break;
			}
			default:
				out.push_back('\\');
				out.push_back(nx);
				break;
			}
		}
		return out;
	}

#ifdef _WIN32
	// Set by the console handler when the user hits Ctrl-C. The REPL polls
	// this between commands to fire the INT trap (or just abandon a partly-
	// edited line). Externals are killed by the OS via the dispatched event,
	// so we don't need to forward; we just swallow at the shell level.
	std::atomic<bool> g_ctrlc_pending{ false };

	BOOL WINAPI ctrlCHandler(DWORD ctrl) {
		if (ctrl == CTRL_C_EVENT || ctrl == CTRL_BREAK_EVENT) {
			g_ctrlc_pending.store(true);
			return TRUE;
		}
		return FALSE;
	}
#endif

	bool isInteractiveStdin() {
#ifdef _WIN32
		return _isatty(_fileno(stdin)) != 0;
#else
		return false;
#endif
	}

	// Apply csh-style history expansion to `line` against `history`.
	//   !!         most recent entry
	//   !N         entry by 1-based number
	//   !-N        N-th most recent
	//   !str       most recent entry starting with str
	//   ^old^new   first occurrence of `old` in last entry replaced
	// Returns true when the line was rewritten; on output, `expanded` holds
	// the new text the caller should run.
	bool expandHistory(const std::string& line,
	                   const std::vector<std::string>& history,
	                   std::string& expanded) {
		expanded.clear();
		if (line.empty()) { expanded = line; return false; }
		// ^old^new(^) form.
		if (line[0] == '^') {
			if (history.empty()) { expanded = line; return false; }
			std::size_t p1 = line.find('^', 1);
			if (p1 == std::string::npos) { expanded = line; return false; }
			std::size_t p2 = line.find('^', p1 + 1);
			std::string oldp = line.substr(1, p1 - 1);
			std::string newp = (p2 == std::string::npos)
				? line.substr(p1 + 1)
				: line.substr(p1 + 1, p2 - p1 - 1);
			std::string base = history.back();
			auto pos = base.find(oldp);
			if (pos == std::string::npos) { expanded = line; return false; }
			expanded = base.substr(0, pos) + newp + base.substr(pos + oldp.size());
			return true;
		}
		bool any = false;
		for (std::size_t i = 0; i < line.size(); ++i) {
			char c = line[i];
			if (c != '!') { expanded.push_back(c); continue; }
			if (i + 1 >= line.size()) { expanded.push_back(c); continue; }
			char nx = line[i + 1];
			// Don't expand `!=`, `!"`, `!\`, end-of-line `!`, or `!{` (rare).
			if (nx == ' ' || nx == '\t' || nx == '\n' || nx == '='
			    || nx == '"' || nx == '\\') { expanded.push_back(c); continue; }
			std::string sub;
			std::size_t advance = 0;
			if (nx == '!') {
				if (history.empty()) { expanded.push_back(c); continue; }
				sub = history.back();
				advance = 2;
			} else if (nx == '-' && i + 2 < line.size() && std::isdigit((unsigned char)line[i + 2])) {
				std::size_t k = i + 2;
				while (k < line.size() && std::isdigit((unsigned char)line[k])) ++k;
				int n = std::stoi(line.substr(i + 2, k - i - 2));
				if (n <= 0 || static_cast<std::size_t>(n) > history.size()) {
					expanded.push_back(c); continue;
				}
				sub = history[history.size() - n];
				advance = k - i;
			} else if (std::isdigit((unsigned char)nx)) {
				std::size_t k = i + 1;
				while (k < line.size() && std::isdigit((unsigned char)line[k])) ++k;
				int n = std::stoi(line.substr(i + 1, k - i - 1));
				if (n <= 0 || static_cast<std::size_t>(n) > history.size()) {
					expanded.push_back(c); continue;
				}
				sub = history[n - 1];
				advance = k - i;
			} else if (std::isalpha((unsigned char)nx) || nx == '_') {
				std::size_t k = i + 1;
				while (k < line.size()
				    && (std::isalnum((unsigned char)line[k])
				    || line[k] == '_' || line[k] == '-')) ++k;
				std::string prefix = line.substr(i + 1, k - i - 1);
				const std::string* match = nullptr;
				for (auto rit = history.rbegin(); rit != history.rend(); ++rit) {
					if (rit->compare(0, prefix.size(), prefix) == 0) { match = &*rit; break; }
				}
				if (!match) { expanded.push_back(c); continue; }
				sub = *match;
				advance = k - i;
			} else {
				expanded.push_back(c);
				continue;
			}
			expanded += sub;
			i += advance - 1;
			any = true;
		}
		return any;
	}

	bool readInteractiveLine(std::string& out) {
		out.clear();
		int c;
		while ((c = std::fgetc(stdin)) != EOF) {
			if (c == '\n') return true;
			out.push_back(static_cast<char>(c));
		}
		return !out.empty();   // false on real EOF with nothing buffered
	}

#ifdef _WIN32
	// Apply title + dark mode title bar to the host console window.
	// No-op on Windows Terminal (which owns its own chrome) and harmless
	// on conhost when the OS doesn't recognize the DWM attribute.
	void setupConsoleWindow() {
		SetConsoleTitleW(L"wbsh");
		HWND hwnd = GetConsoleWindow();
		if (!hwnd) return;
		BOOL dark = TRUE;
		if (FAILED(DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
		                                 &dark, sizeof(dark)))) {
			DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_OLD,
			                      &dark, sizeof(dark));
		}
		// Win11-only: paint the caption + frame to match the shell's dark
		// theme. Older systems return E_INVALIDARG, which we silently ignore.
		COLORREF caption = RGB(0x14, 0x14, 0x18);
		DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
		COLORREF border = RGB(0x33, 0x33, 0x3a);
		DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
	}

	// Pick a clean monospace face for conhost. Windows Terminal ignores
	// SetCurrentConsoleFontEx (font is profile-driven) — so this is a
	// best-effort upgrade for the legacy host, never a regression.
	void setupConsoleFont(HANDLE h_out) {
		if (h_out == INVALID_HANDLE_VALUE) return;
		CONSOLE_FONT_INFOEX cfi{};
		cfi.cbSize = sizeof(cfi);
		cfi.nFont = 0;
		cfi.dwFontSize.X = 0;
		cfi.dwFontSize.Y = 16;
		cfi.FontFamily = FF_DONTCARE;
		cfi.FontWeight = FW_NORMAL;
		const wchar_t* faces[] = { L"Cascadia Mono", L"Cascadia Code", L"Consolas" };
		for (const wchar_t* face : faces) {
			wcsncpy_s(cfi.FaceName, face, _TRUNCATE);
			if (SetCurrentConsoleFontEx(h_out, FALSE, &cfi)) return;
		}
	}
#endif

	bool stderrIsTty() {
#ifdef _WIN32
		return _isatty(_fileno(stderr)) != 0;
#else
		return false;
#endif
	}

	bool stdoutIsTty() {
#ifdef _WIN32
		return _isatty(_fileno(stdout)) != 0;
#else
		return false;
#endif
	}

	bool looksIncomplete(const std::vector<LexError>& lex_errs,
	                     const std::vector<ParseError>& parse_errs) {
		for (const auto& e : lex_errs) {
			if (e.message.find("unterminated") != std::string::npos) return true;
		}
		for (const auto& e : parse_errs) {
			const auto& m = e.message;
			if (m.find("expected `")        != std::string::npos
			    || m.find("expected pipeline") != std::string::npos
			    || m.find("expected command")  != std::string::npos) {
				return true;
			}
		}
		return false;
	}

	int runInteractive() {
		Environment env;
		prepareEnv(env);
		Executor exec(env);
		absorbInheritedState(env, exec);

#ifdef _WIN32
		SetConsoleCtrlHandler(ctrlCHandler, TRUE);
		// UTF-8 console output so the prompt and our messages render right
		// on Windows Terminal / conhost. Input mode left as default (line
		// + echo + processed) so basic editing works.
		SetConsoleOutputCP(CP_UTF8);
		// Capture sensible "cooked" modes once and re-apply them every loop
		// iteration. Externals (git's pager, less, vim, etc.) often change
		// console state and don't always restore it cleanly — without this
		// the next prompt's escape codes print literally.
		HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
		HANDLE h_in  = GetStdHandle(STD_INPUT_HANDLE);
		DWORD good_out_mode = 0, good_in_mode = 0;
		bool color_ok = false;
		if (h_out != INVALID_HANDLE_VALUE && GetConsoleMode(h_out, &good_out_mode)) {
			good_out_mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
			good_out_mode |= ENABLE_PROCESSED_OUTPUT;
			good_out_mode |= ENABLE_WRAP_AT_EOL_OUTPUT;
			color_ok = SetConsoleMode(h_out, good_out_mode) != 0;
		}
		if (h_in != INVALID_HANDLE_VALUE && GetConsoleMode(h_in, &good_in_mode)) {
			good_in_mode |= ENABLE_LINE_INPUT;
			good_in_mode |= ENABLE_ECHO_INPUT;
			good_in_mode |= ENABLE_PROCESSED_INPUT;
			good_in_mode |= ENABLE_EXTENDED_FLAGS;
			SetConsoleMode(h_in, good_in_mode);
		}
		setupConsoleWindow();
		setupConsoleFont(h_out);
#else
		bool color_ok = true;
#endif

		if (env.get("PS1").empty()) {
			// Match Git Bash's default look when colour is available:
			// bright-green user@host, bright-cyan path, yellow git-branch
			// suffix, bold prompt char.
			if (color_ok) {
				env.set("PS1",
					"\\[\\e[32;1m\\]\\u@\\h\\[\\e[0m\\] "
					"\\[\\e[36;1m\\]\\w\\[\\e[0m\\]\\g\\$ ");
			} else {
				env.set("PS1", "\\u@\\h \\w\\g\\$ ");
			}
		}
		if (env.get("PS2").empty()) env.set("PS2", "> ");
		(void)color_ok;

		if (color_ok) {
			std::fputs(
				"\x1b[36;1m wbsh \x1b[0m"
				"\x1b[2m— bash front-end + executor (Windows)\x1b[0m\n"
				"\x1b[2m   type `\x1b[0;33mexit\x1b[2m` or press "
				"`\x1b[0;33mCtrl-D\x1b[2m` to quit\x1b[0m\n",
				stdout);
		} else {
			std::fputs(
				"wbsh -- bash front-end + executor (Windows)\n"
				"   type `exit` or press Ctrl-D to quit\n",
				stdout);
		}

		// Determine HISTFILE (defaults to $HOME/.wbsh_history) and load
		// previous-session history before showing any prompt.
		std::string home_dir = env.get("HOME");
		std::string histfile = env.get("HISTFILE");
		if (histfile.empty() && !home_dir.empty()) {
			histfile = home_dir + "/.wbsh_history";
			env.set("HISTFILE", histfile);
		}
		if (!histfile.empty()) {
			exec.loadHistoryFromFile(exec.pathConv().toWin32(histfile));
		}

		// Source ~/.wbshrc for user customisation (aliases, env, prompt).
		if (!home_dir.empty()) {
			std::string rcfile = home_dir + "/.wbshrc";
			std::filesystem::path native = utf8ToPath(exec.pathConv().toWin32(rcfile));
			std::error_code ec;
			if (std::filesystem::exists(native, ec)) {
				std::ifstream f(native, std::ios::binary);
				if (f) {
					std::stringstream ss;
					ss << f.rdbuf();
					try { exec.executeText(ss.str(), rcfile); }
					catch (ShellExit& e) {
						if (!histfile.empty())
							exec.saveHistoryToFile(exec.pathConv().toWin32(histfile));
						return e.status;
					}
				}
			}
		}

		// Save history to disk on any normal exit path.
		auto saveHistory = [&]() {
			if (!histfile.empty()) {
				exec.saveHistoryToFile(exec.pathConv().toWin32(histfile));
			}
		};
		auto onExit = [&]() {
			exec.fireExitTrap();
			saveHistory();
		};

		LineEditor editor(env, exec);
		std::string buffer;
		bool waiting_for_more = false;
		int last_cols = 0, last_lines = 0;
		while (true) {
#ifdef _WIN32
			// External processes (especially pagers — git log -> less, vim,
			// etc.) commonly mutate console mode and don't restore it on
			// exit. Re-apply our good modes before every prompt so the line
			// editor and ANSI escapes work consistently.
			if (h_out != INVALID_HANDLE_VALUE) SetConsoleMode(h_out, good_out_mode);
			if (h_in  != INVALID_HANDLE_VALUE) SetConsoleMode(h_in,  good_in_mode);
			// If a Ctrl-C arrived while no command was running, fire the
			// INT trap (if any) and discard any partly-typed multiline.
			if (g_ctrlc_pending.exchange(false)) {
				buffer.clear();
				waiting_for_more = false;
				std::fputc('\n', stdout);
				if (exec.hasTrap("INT")) {
					std::string action = exec.trapAction("INT");
					exec.executeText(action, "<trap INT>");
				}
				exec.setLastStatus(130);   // 128 + SIGINT
			}
			// Window resize → update LINES/COLUMNS and fire WINCH trap.
			if (h_out != INVALID_HANDLE_VALUE) {
				CONSOLE_SCREEN_BUFFER_INFO info{};
				if (GetConsoleScreenBufferInfo(h_out, &info)) {
					int cols = info.srWindow.Right - info.srWindow.Left + 1;
					int lines = info.srWindow.Bottom - info.srWindow.Top + 1;
					if (cols != last_cols || lines != last_lines) {
						env.set("COLUMNS", std::to_string(cols));
						env.set("LINES", std::to_string(lines));
						if (last_cols != 0 && exec.hasTrap("WINCH")) {
							std::string action = exec.trapAction("WINCH");
							exec.executeText(action, "<trap WINCH>");
						}
						last_cols = cols;
						last_lines = lines;
					}
				}
			}
#endif
			std::string ps_raw = waiting_for_more ? env.get("PS2") : env.get("PS1");
			if (ps_raw.empty()) ps_raw = waiting_for_more ? "> " : "$ ";
			std::string prompt = expandPrompt(ps_raw, env, exec.pathConv());

			std::string line;
			bool got_line = editor.readLine(prompt, line);
			if (!got_line) {
				// EOF on empty line (Ctrl-D or stdin closed). If we're in the
				// middle of a multi-line construct, just discard and re-prompt.
				if (!buffer.empty()) {
					buffer.clear();
					waiting_for_more = false;
					std::fputc('\n', stdout);
					continue;
				}
				std::fputc('\n', stdout);
				onExit();
				return exec.lastStatus();
			}

			// History expansion applies to each newly-entered line, before
			// we accumulate it into the multi-line buffer.
			if (!waiting_for_more && !line.empty() && line[0] != ' ') {
				std::string expanded;
				if (expandHistory(line, exec.history(), expanded)) {
					std::fprintf(stdout, "%s\n", expanded.c_str());
					std::fflush(stdout);
					line = expanded;
				}
			}

			if (!line.empty()) exec.addHistoryEntry(line);

			if (!buffer.empty()) buffer.push_back('\n');
			buffer += line;

			Lexer lex(buffer);
			auto tokens = lex.tokenize();
			Parser parser(std::move(tokens), buffer);
			auto root = parser.parseProgram();

			if (looksIncomplete(lex.errors(), parser.errors())) {
				waiting_for_more = true;
				continue;
			}

			waiting_for_more = false;

			bool err_color = stderrIsTty();
			const char* err_pre = err_color ? "\x1b[31;1m" : "";
			const char* err_loc = err_color ? "\x1b[33m"   : "";
			const char* err_msg = err_color ? "\x1b[0m"    : "";
			for (const auto& e : lex.errors()) {
				std::fprintf(stderr, "%swbsh: lex%s %s%zu:%zu:%s %s\n",
					err_pre, err_msg, err_loc, e.loc.line, e.loc.column, err_msg,
					e.message.c_str());
			}
			for (const auto& e : parser.errors()) {
				std::fprintf(stderr, "%swbsh: parse%s %s%zu:%zu:%s %s\n",
					err_pre, err_msg, err_loc, e.loc.line, e.loc.column, err_msg,
					e.message.c_str());
			}

			if (root && parser.errors().empty() && lex.errors().empty()) {
				Node* root_ptr = root.get();
				exec.adoptAst(std::move(root));
				exec.setSourceText(buffer);
				try {
					exec.execute(*root_ptr);
				} catch (ShellExit& e) {
					onExit();
					return e.status;
				}
			}
			buffer.clear();
		}
	}

	int runOnSource(const std::string& src, bool show_tokens, bool show_ast,
	                bool do_expand, bool do_run) {
		Lexer lex(src);
		auto tokens = lex.tokenize();

		bool out_color = stdoutIsTty();
		bool err_color = stderrIsTty();
		auto header = [&](const char* title) {
			if (out_color) std::cout << "\x1b[35;1m── " << title << " ──\x1b[0m\n";
			else           std::cout << "--- " << title << " ---\n";
		};
		const char* ep = err_color ? "\x1b[31;1m" : "";
		const char* el = err_color ? "\x1b[33m"   : "";
		const char* er = err_color ? "\x1b[0m"    : "";

		if (show_tokens) {
			header("TOKENS");
			dumpTokens(std::cout, tokens);
		}
		for (const auto& e : lex.errors()) {
			std::cerr << ep << "lex error" << er << " "
				<< el << e.loc.line << ":" << e.loc.column << er
				<< ": " << e.message << "\n";
		}

		Parser parser(std::move(tokens), src);
		auto root = parser.parseProgram();
		if (show_ast) {
			header("AST");
			if (root) dumpAst(std::cout, *root);
			else      std::cout << "(empty)\n";
		}
		for (const auto& e : parser.errors()) {
			std::cerr << ep << "parse error" << er << " "
				<< el << e.loc.line << ":" << e.loc.column << er
				<< ": " << e.message << "\n";
		}

		if (do_expand && root) {
			Environment env;
			prepareEnv(env);
			Expander exp(env, /*sub=*/nullptr);
			header("EXPANSIONS");
			walkAndExpand(*root, exp, env, std::cout, 0);
		}

		if (do_run && root) {
			Environment env;
			prepareEnv(env);
			Executor exec(env);
			exec.setSourceText(src);
			absorbInheritedState(env, exec);
			try {
				return exec.execute(*root);
			} catch (ShellExit& e) {
				return e.status;
			}
		}

		return (parser.errors().empty() && lex.errors().empty()) ? 0 : 1;
	}

	std::string readAll(std::istream& in) {
		std::stringstream ss;
		ss << in.rdbuf();
		return ss.str();
	}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
	// Replace the CRT-decoded argv with the wide command line decoded
	// via UTF-16 -> UTF-8. Without this, MSVC's narrow `argv` is decoded
	// through the C locale (default "C") and any non-ASCII argument like
	// "/c/Users/Tomáš" arrives as "Tom�" — every multi-byte sequence
	// collapses to a replacement char, breaking everything downstream.
	std::vector<std::string> argv_utf8_storage;
	std::vector<char*> argv_ptrs;
	{
		int wargc = 0;
		LPWSTR* wargv = ::CommandLineToArgvW(::GetCommandLineW(), &wargc);
		if (wargv) {
			argv_utf8_storage.reserve(static_cast<std::size_t>(wargc));
			for (int i = 0; i < wargc; ++i) {
				argv_utf8_storage.push_back(wbsh::wideToUtf8(wargv[i]));
			}
			::LocalFree(wargv);
			argv_ptrs.reserve(argv_utf8_storage.size() + 1);
			for (auto& s : argv_utf8_storage) argv_ptrs.push_back(s.data());
			argv_ptrs.push_back(nullptr);
			argc = static_cast<int>(argv_utf8_storage.size());
			argv = argv_ptrs.data();
		}
	}
#endif
	bool show_tokens = false;
	bool show_ast = true;
	bool do_expand = false;
	bool do_run = false;
	bool ast_explicit = false;
	std::string src;
	bool have_src = false;
	bool from_stdin = false;

	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		if (a == "-h" || a == "--help") {
			printHelp();
			return 0;
		}
		if (a == "-t" || a == "--tokens") { show_tokens = true; continue; }
		if (a == "-e" || a == "--expand") { do_expand = true; continue; }
		if (a == "-r" || a == "--run")    { do_run = true; continue; }
		if (a == "-i" || a == "--interactive") { return runInteractive(); }
		if (a == "--ast")    { show_ast = true;  ast_explicit = true; continue; }
		if (a == "--no-ast") { show_ast = false; ast_explicit = true; continue; }
		if (a == "-c") {
			if (i + 1 >= argc) {
				std::cerr << "wbsh: -c requires an argument\n";
				return 2;
			}
			src = argv[++i];
			have_src = true;
			continue;
		}
		if (a == "-") {
			from_stdin = true;
			continue;
		}
		std::ifstream f(a, std::ios::binary);
		if (!f) {
			std::cerr << "wbsh: cannot open file: " << a << "\n";
			return 2;
		}
		src = readAll(f);
		have_src = true;
	}

	if (!have_src) {
		if (from_stdin) {
			src = readAll(std::cin);
			have_src = true;
		} else if (isInteractiveStdin()) {
			// No script + a real TTY: drop into the interactive REPL.
			return runInteractive();
		} else if (!std::cin.eof()) {
			// Piped stdin (no TTY): read it as a script.
			src = readAll(std::cin);
			have_src = true;
		}
	}
	if (!have_src) {
		printHelp();
		return 0;
	}
	// In run mode, default to suppressing the AST dump so output is clean.
	if (do_run && !ast_explicit) show_ast = false;
	return runOnSource(src, show_tokens, show_ast, do_expand, do_run);
}

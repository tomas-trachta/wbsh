/**
 * @file fzf.cpp
 * @brief `fzf` — interactive fuzzy line picker, in the spirit of
 *        junegunn/fzf. Reads candidate lines from stdin (or, when stdin
 *        is a tty, walks the current directory), lets the user narrow
 *        them with an incremental fuzzy query, and prints the picked
 *        line to stdout.
 */

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>

#  include <io.h>
#  include <shellapi.h>
#  pragma comment(lib, "shell32.lib")
#endif /* _WIN32 */

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "consolekeys.h"
#include "coreutils_internal.h"
#include "executor.h"
#include "numparse.h"
#include "pathconv.h"
#include "termreq.h"
#include "textcells.h"

namespace wbsh {

	namespace fs = std::filesystem;

	struct FzfMatch {
		std::size_t index;
		int score;
		std::vector<std::size_t> positions;
	};

	static bool isWordBoundary(const std::string& text, std::size_t i) {
		if (i == 0) return true;
		const unsigned char prev = static_cast<unsigned char>(text[i - 1]);
		const unsigned char cur  = static_cast<unsigned char>(text[i]);
		if (prev == '/' || prev == '\\' || prev == '_'
		    || prev == '-' || prev == '.' || prev == ' ') return true;
		return std::islower(prev) != 0 && std::isupper(cur) != 0;
	}

	// Greedy subsequence match starting from a fixed anchor for the first
	// query char: walks `text` once from `start`, taking the first
	// occurrence of each remaining query char in order.
	static bool fuzzyMatchFrom(const std::string& query, const std::string& text,
	                          std::size_t start, int& score,
	                          std::vector<std::size_t>& positions) {
		positions.clear();
		std::size_t ti = start;
		int total = 0;
		bool prev_matched = false;
		std::size_t prev_pos = 0;

		for (char qc_raw : query) {
			const unsigned char qc = std::tolower(static_cast<unsigned char>(qc_raw));
			while (ti < text.size()
			       && std::tolower(static_cast<unsigned char>(text[ti])) != qc) ++ti;
			if (ti >= text.size()) return false;

			int char_score = 1;
			if (isWordBoundary(text, ti)) char_score += 8;
			if (prev_matched) {
				std::size_t gap = ti - prev_pos - 1;
				char_score += gap == 0 ? 5 : -static_cast<int>(gap);
			}

			total += char_score;

			positions.push_back(ti);
			prev_matched = true;
			prev_pos = ti;
			++ti;
		}

		score = total;
		return true;
	}

	// Anchoring on the very first occurrence of the first query char (as
	// fuzzyMatchFrom(...,  0, ...) would) picks a bad anchor whenever that
	// char also appears earlier outside the "real" match, e.g. query
	// "wbsh" against "software_projects/wbsh" would anchor on the 'w' in
	// "software" and then hunt for a scattered b/s/h far away, scoring
	// worse than an unrelated candidate whose letters happen to sit
	// closer together. So every occurrence of the first char is tried as
	// an anchor and the highest-scoring run wins — not a full DP (a real
	// fzf uses one), but it fixes the common "anchor trap" case cheaply.
	static bool fuzzyMatch(const std::string& query, const std::string& text,
	                       int& score, std::vector<std::size_t>& positions) {
		if (query.empty()) {
			score = 0;
			positions.clear();
			return true;
		}

		const unsigned char first = std::tolower(static_cast<unsigned char>(query[0]));
		bool found = false;

		for (std::size_t anchor = 0; anchor < text.size(); ++anchor) {
			if (std::tolower(static_cast<unsigned char>(text[anchor])) != first) continue;

			int cur_score;
			std::vector<std::size_t> cur_positions;
			if (!fuzzyMatchFrom(query, text, anchor, cur_score, cur_positions)) continue;
			if (found && cur_score <= score) continue;

			found = true;
			score = cur_score;
			positions = std::move(cur_positions);
		}

		return found;
	}

	static bool scoreCandidate(const std::string& query, const std::string& text,
	                           std::size_t index, std::vector<FzfMatch>& matches) {
		FzfMatch match;
		match.index = index;
		if (!fuzzyMatch(query, text, match.score, match.positions)) return false;

		matches.push_back(std::move(match));
		return true;
	}

	static bool byScore(const FzfMatch& left, const FzfMatch& right) {
		if (left.score != right.score) return left.score > right.score;

		return left.index < right.index;
	}

	static bool stdinIsTty() {
#ifdef _WIN32
		return _isatty(_fileno(stdin)) != 0;
#else
		return false;
#endif
	}

	// As fzf's own walker: version-control and dependency trees are not
	// worth offering, and descending into them can take minutes.
	static bool skippedByWalk(const fs::path& name) {
		return name == ".git" || name == "node_modules";
	}

	// `take` gets each entry under `dir` as a forward-slashed relative
	// path, and stops the walk by returning false.
	static void walkTree(const fs::path& dir, const std::function<bool(std::string)>& take) {
		std::error_code ec;
		fs::recursive_directory_iterator it(dir,
			fs::directory_options::skip_permission_denied, ec);
		if (ec) return;

		for (auto cur = it; cur != fs::recursive_directory_iterator(); cur.increment(ec)) {
			if (ec) break;
			if (skippedByWalk(cur->path().filename())) {
				cur.disable_recursion_pending();
				continue;
			}

			std::string rel = pathToUtf8(cur->path().lexically_relative(dir));
			std::replace(rel.begin(), rel.end(), '\\', '/');
			if (!take(std::move(rel))) return;
		}
	}

	static std::vector<std::string> collectCandidates(Executor& exec) {
		std::vector<std::string> lines;
		if (stdinIsTty()) {
			walkTree(fs::current_path(), [&lines](std::string entry) {
				lines.push_back(std::move(entry));
				return true;
			});
			return lines;
		}

		readAllLines(exec, "-", lines);
		return lines;
	}

	// The directory walk runs beside the picker, which shows and filters
	// what has been found so far.
	struct CandidateFeed {
		std::mutex lock;
		std::vector<std::string> pending;
		std::atomic<bool> done{ false };
		std::atomic<bool> stop{ false };
		std::thread worker;
	};

	static bool feedEntry(CandidateFeed& feed, std::string entry) {
		if (feed.stop.load()) return false;

		std::lock_guard<std::mutex> guard(feed.lock);
		feed.pending.push_back(std::move(entry));
		return true;
	}

	static void startTreeWalk(CandidateFeed& feed, const fs::path& dir) {
		feed.worker = std::thread([&feed, dir] {
			walkTree(dir, [&feed](std::string entry) { return feedEntry(feed, std::move(entry)); });
			feed.done.store(true);
		});
	}

	static void stopTreeWalk(CandidateFeed& feed) {
		feed.stop.store(true);
		if (feed.worker.joinable()) feed.worker.join();
	}

	static std::vector<std::string> takeFedEntries(CandidateFeed& feed) {
		std::vector<std::string> fresh;
		std::lock_guard<std::mutex> guard(feed.lock);
		fresh.swap(feed.pending);
		return fresh;
	}

#ifdef _WIN32

	static const std::size_t kMaxRows = 15;
	static const std::size_t kDefaultColumns = 80;
	static const DWORD kFeedPollMs = 100;
	static const char* const kMatchStyle = "\x1b[1;33m";
	static const char* const kEndMatchStyle = "\x1b[22;39m";
	static const char* const kSelectedStyle = "\x1b[7m";
	static const char* const kResetStyle = "\x1b[0m";

	// State for one interactive picker session. Owns the CONIN$/CONOUT$
	// handles opened directly against the console device (not the
	// process's stdin/stdout) so the picker still works when piped, e.g.
	// `ls | fzf | xargs cat` — exactly how a real terminal fzf behaves.
	// `headless` replays keys and keeps the output instead (`__fzf`).
	struct FzfSession {
		HANDLE h_in  = INVALID_HANDLE_VALUE;
		HANDLE h_out = INVALID_HANDLE_VALUE;
		DWORD saved_in_mode  = 0;
		DWORD saved_out_mode = 0;
		std::size_t max_rows = kMaxRows;
		std::size_t columns = kDefaultColumns;

		std::string query;
		std::vector<std::string> candidates;
		CandidateFeed* feed = nullptr;
		bool loading = false;

		// `matches` holds candidates[0, scored) that match `scored_query`.
		std::string scored_query;
		std::size_t scored = 0;
		std::vector<FzfMatch> matches;

		std::size_t selected = 0;
		std::size_t top = 0;
		std::size_t drawn_lines = 0;

		bool headless = false;
		std::deque<KEY_EVENT_RECORD> replay;
		std::string captured;
	};

	static void fzfMeasureConsole(FzfSession& session) {
		CONSOLE_SCREEN_BUFFER_INFO info{};
		if (!::GetConsoleScreenBufferInfo(session.h_out, &info)) return;

		const int width = info.srWindow.Right - info.srWindow.Left + 1;
		const int height = info.srWindow.Bottom - info.srWindow.Top + 1;
		if (width > 0) session.columns = static_cast<std::size_t>(width);
		if (height > 3) session.max_rows = std::min(kMaxRows, static_cast<std::size_t>(height - 3));
	}

	static bool fzfOpenConsole(FzfSession& session) {
		session.h_in  = ::CreateFileW(L"CONIN$",  GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
		session.h_out = ::CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
		if (session.h_in == INVALID_HANDLE_VALUE) return false;
		if (session.h_out == INVALID_HANDLE_VALUE) return false;

		::GetConsoleMode(session.h_in, &session.saved_in_mode);
		::GetConsoleMode(session.h_out, &session.saved_out_mode);

		DWORD in_mode = session.saved_in_mode;
		in_mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT
		           | ENABLE_MOUSE_INPUT | ENABLE_WINDOW_INPUT);
		::SetConsoleMode(session.h_in, in_mode);

		DWORD out_mode = session.saved_out_mode
			| ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT;
		::SetConsoleMode(session.h_out, out_mode);
		fzfMeasureConsole(session);
		return true;
	}

	static void fzfCloseConsole(FzfSession& session) {
		if (session.h_in != INVALID_HANDLE_VALUE) {
			::SetConsoleMode(session.h_in, session.saved_in_mode);
		}

		if (session.h_out != INVALID_HANDLE_VALUE) {
			::SetConsoleMode(session.h_out, session.saved_out_mode);
		}

		if (session.h_in  != INVALID_HANDLE_VALUE) ::CloseHandle(session.h_in);
		if (session.h_out != INVALID_HANDLE_VALUE) ::CloseHandle(session.h_out);
	}

	static void fzfEmit(FzfSession& session, const std::string& out) {
		if (session.headless) {
			session.captured += out;
			return;
		}

		DWORD wrote;
		::WriteFile(session.h_out, out.data(), static_cast<DWORD>(out.size()), &wrote, nullptr);
	}

	// True when what is shown changed: new entries, or the walk ended.
	static bool fzfAbsorbFeed(FzfSession& session) {
		if (session.feed == nullptr) return false;

		const bool was_loading = session.loading;
		session.loading = !session.feed->done.load();
		std::vector<std::string> fresh = takeFedEntries(*session.feed);
		for (std::string& entry : fresh) session.candidates.push_back(std::move(entry));

		return !fresh.empty() || was_loading != session.loading;
	}

	static void fzfScrollToSelection(FzfSession& session) {
		if (session.selected < session.top) session.top = session.selected;
		if (session.selected >= session.top + session.max_rows) {
			session.top = session.selected + 1 - session.max_rows;
		}
	}

	static void fzfClampSelection(FzfSession& session) {
		if (session.selected >= session.matches.size()) {
			session.selected = session.matches.empty() ? 0 : session.matches.size() - 1;
		}

		if (session.top > session.selected) session.top = session.selected;
		fzfScrollToSelection(session);
	}

	// A longer query can only match fewer lines, so only the last matches
	// are scored again.
	static void fzfNarrowMatches(FzfSession& session) {
		std::vector<FzfMatch> kept;
		for (const FzfMatch& match : session.matches) {
			scoreCandidate(session.query, session.candidates[match.index], match.index, kept);
		}

		session.matches = std::move(kept);
	}

	static void fzfScoreNewCandidates(FzfSession& session) {
		for (std::size_t i = session.scored; i < session.candidates.size(); ++i) {
			scoreCandidate(session.query, session.candidates[i], i, session.matches);
		}

		session.scored = session.candidates.size();
	}

	static void fzfRefilter(FzfSession& session) {
		const bool same_query = session.query == session.scored_query;
		const bool narrower = !same_query
			&& session.query.compare(0, session.scored_query.size(), session.scored_query) == 0;
		if (narrower) fzfNarrowMatches(session);
		if (!same_query && !narrower) {
			session.matches.clear();
			session.scored = 0;
		}

		session.scored_query = session.query;
		fzfScoreNewCandidates(session);
		std::sort(session.matches.begin(), session.matches.end(), byScore);
		if (!same_query) {
			session.selected = 0;
			session.top = 0;
		}

		fzfClampSelection(session);
	}

	// What fits in `cells` cells, with the matched characters bright and
	// control characters shown as spaces so they cannot move the cursor.
	static std::string fzfRowText(const std::string& text,
	                              const std::vector<std::size_t>& positions, std::size_t cells) {
		std::string out;
		std::size_t used = 0;
		std::size_t next = 0;
		std::size_t at = 0;
		while (at < text.size()) {
			const TextCluster cluster = clusterAt(text, at);
			const bool control = static_cast<unsigned char>(text[at]) < 0x20;
			const std::size_t width = control ? 1 : cluster.width;
			if (used + width > cells) break;

			bool hit = false;
			for (; next < positions.size() && positions[next] < cluster.end; ++next) hit = true;
			if (hit) out += kMatchStyle;
			out += control ? std::string(" ") : text.substr(at, cluster.end - at);
			if (hit) out += kEndMatchStyle;

			used += width;
			at = cluster.end;
		}

		return out;
	}

	// The end of a query too long for the line, where the typing happens.
	static std::string fzfQueryTail(const std::string& query, std::size_t cells) {
		const std::size_t total = displayWidth(query, query.size());
		std::size_t start = 0;
		while (start < query.size() && total - displayWidth(query, start) > cells) {
			start = nextClusterEnd(query, start);
		}

		return query.substr(start);
	}

	// Every row stays one cell short of the edge, so none wraps and the
	// next draw can move up over exactly the rows this one wrote.
	static std::size_t fzfTextCells(const FzfSession& session) {
		const std::size_t prefix = 2;
		return session.columns > prefix + 1 ? session.columns - prefix - 1 : 1;
	}

	static void fzfRenderRow(FzfSession& session, std::size_t row, std::string& out) {
		const FzfMatch& match = session.matches[row];
		const bool is_selected = row == session.selected;
		out += is_selected ? std::string(kSelectedStyle) + "> " : std::string("  ");
		out += fzfRowText(session.candidates[match.index], match.positions, fzfTextCells(session));
		out += kResetStyle;
		out += "\r\n";
	}

	static std::string fzfInfoLine(const FzfSession& session) {
		std::string info = "  " + std::to_string(session.matches.size()) + "/"
			+ std::to_string(session.candidates.size());
		if (session.loading) info += " ...";
		return info;
	}

	static void fzfRender(FzfSession& session) {
		std::string out;
		if (session.drawn_lines > 0) out += "\x1b[" + std::to_string(session.drawn_lines) + "A";
		out += "\r\x1b[J";

		out += "> " + fzfQueryTail(session.query, fzfTextCells(session)) + "\r\n";
		out += fzfInfoLine(session) + "\r\n";

		const std::size_t end = std::min(session.top + session.max_rows, session.matches.size());
		for (std::size_t row = session.top; row < end; ++row) fzfRenderRow(session, row, out);

		session.drawn_lines = 2 + end - session.top;
		fzfEmit(session, out);
	}

	static void fzfMoveSelection(FzfSession& session, long delta) {
		if (session.matches.empty()) return;

		const long last = static_cast<long>(session.matches.size()) - 1;
		const long next = static_cast<long>(session.selected) + delta;
		session.selected = static_cast<std::size_t>(std::clamp(next, 0L, last));
		fzfScrollToSelection(session);
	}

	static void fzfInsertChar(FzfSession& session, const std::string& utf8) {
		session.query += utf8;
	}

	static void fzfBackspace(FzfSession& session) {
		if (session.query.empty()) return;

		session.query.erase(previousClusterStart(session.query, session.query.size()));
	}

	static std::string fzfCharacter(FzfSession& session, WCHAR ch) {
		WCHAR units[2] = { ch, 0 };
		const bool high = ch >= 0xD800 && ch <= 0xDBFF;
		if (!session.headless) return readConsoleCharacter(session.h_in, ch);
		if (!high || session.replay.empty()) return wideToUtf8(std::wstring(units, 1));

		units[1] = session.replay.front().uChar.UnicodeChar;
		session.replay.pop_front();
		return wideToUtf8(std::wstring(units, 2));
	}

	static bool fzfMovesSelection(const KEY_EVENT_RECORD& k, bool ctrl, long& delta,
	                              std::size_t page) {
		const WORD vk = k.wVirtualKeyCode;
		const long rows = static_cast<long>(page);
		if (vk == VK_UP || (ctrl && (vk == 'P' || vk == 'K'))) delta = -1;
		else if (vk == VK_DOWN || vk == VK_TAB || (ctrl && (vk == 'N' || vk == 'J'))) delta = 1;
		else if (vk == VK_PRIOR) delta = -rows;
		else if (vk == VK_NEXT) delta = rows;
		else return false;

		return true;
	}

	// Handles one keystroke. Returns false when the modal loop should end;
	// `accepted` distinguishes Enter (true) from Esc / Ctrl-C / Ctrl-G (false).
	static bool fzfHandleKey(FzfSession& session, const KEY_EVENT_RECORD& k, bool& accepted) {
		const bool ctrl = (k.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
		const WORD vk = k.wVirtualKeyCode;

		if (vk == VK_RETURN) { accepted = !session.matches.empty(); return false; }
		if (vk == VK_ESCAPE
		    || (ctrl && (vk == 'C' || vk == 'G'))) { accepted = false; return false; }

		long delta = 0;
		if (fzfMovesSelection(k, ctrl, delta, session.max_rows)) {
			fzfMoveSelection(session, delta);
			return true;
		}

		if (vk == VK_BACK) { fzfBackspace(session); return true; }
		if (ctrl && vk == 'U') { session.query.clear(); return true; }

		const WCHAR ch = k.uChar.UnicodeChar;
		if (!ctrl && ch >= 0x20) fzfInsertChar(session, fzfCharacter(session, ch));
		return true;
	}

	static bool fzfInputQueued(const FzfSession& session) {
		if (session.headless) return false;

		DWORD count = 0;
		return ::GetNumberOfConsoleInputEvents(session.h_in, &count) && count > 0;
	}

	static bool fzfReadKeyDown(FzfSession& session, KEY_EVENT_RECORD& key) {
		if (session.headless) {
			if (session.replay.empty()) return false;

			key = session.replay.front();
			session.replay.pop_front();
			return true;
		}

		for (;;) {
			INPUT_RECORD record;
			DWORD read = 0;
			if (!::ReadConsoleInputW(session.h_in, &record, 1, &read) || read == 0) return false;
			if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) continue;

			key = record.Event.KeyEvent;
			return true;
		}
	}

	// Handles the next key and every key already queued behind it -- a
	// fast typist, or a paste -- before the list is drawn again. False
	// once a key ends the picker or the input is gone.
	static bool fzfHandleKeys(FzfSession& session, bool& accepted) {
		KEY_EVENT_RECORD key{};
		if (!fzfReadKeyDown(session, key)) return false;

		bool going = fzfHandleKey(session, key, accepted);
		while (going && fzfInputQueued(session)) {
			INPUT_RECORD record;
			DWORD read = 0;
			if (!::ReadConsoleInputW(session.h_in, &record, 1, &read) || read == 0) return false;
			if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) continue;

			going = fzfHandleKey(session, record.Event.KeyEvent, accepted);
		}

		return going;
	}

	// While the walk goes on, the wait for a key gives up now and then so
	// that new entries are shown.
	static bool fzfAwaitKey(const FzfSession& session) {
		if (session.headless || !session.loading) return true;

		return ::WaitForSingleObject(session.h_in, kFeedPollMs) == WAIT_OBJECT_0;
	}

	static void fzfRefresh(FzfSession& session) {
		fzfAbsorbFeed(session);
		fzfRefilter(session);
		fzfRender(session);
	}

	static bool fzfRunLoop(FzfSession& session, std::string& selected) {
		bool accepted = false;
		session.loading = session.feed != nullptr;
		fzfRefresh(session);

		bool going = true;
		while (going) {
			if (!fzfAwaitKey(session)) {
				if (fzfAbsorbFeed(session)) {
					fzfRefilter(session);
					fzfRender(session);
				}

				continue;
			}

			going = fzfHandleKeys(session, accepted);
			fzfRefresh(session);
		}

		fzfEmit(session, "\r\n");
		if (accepted) selected = session.candidates[session.matches[session.selected].index];
		return accepted;
	}

	static std::string fzfShellExecError(INT_PTR code) {
		switch (code) {
		case 0:                    return "not enough memory or resources";
		case ERROR_FILE_NOT_FOUND: return "file not found";
		case ERROR_PATH_NOT_FOUND: return "path not found";
		case ERROR_BAD_FORMAT:     return "invalid executable format";
		case SE_ERR_ACCESSDENIED:  return "access denied";
		case SE_ERR_SHARE:         return "sharing violation";
		default:                   return "launch failed (error " + std::to_string(code) + ")";
		}
	}

	// Launches `native` through its file association (the "open" verb,
	// same as double-clicking it in Explorer). If Windows has no
	// association for it, falls back to the classic "Open With" picker
	// (the "openas" verb). `native` may be a relative, forward-slashed
	// path (the default candidate walk emits POSIX-style relatives), so
	// it's resolved to an absolute, backslashed path with an explicit
	// working directory before handing it to ShellExecuteW — a bare
	// relative lpFile is not reliably resolved by the shell otherwise.
	static bool fzfOpenFile(const fs::path& native) {
		std::error_code ec;
		fs::path abs = fs::absolute(native, ec);
		if (ec) abs = native;
		abs.make_preferred();

		const HINSTANCE result = ::ShellExecuteW(nullptr, L"open", abs.c_str(),
			nullptr, abs.parent_path().c_str(), SW_SHOWNORMAL);
		const auto code = reinterpret_cast<INT_PTR>(result);
		if (code > 32) return true;

		if (code == SE_ERR_NOASSOC || code == SE_ERR_ASSOCINCOMPLETE) {
			const HINSTANCE retry = ::ShellExecuteW(nullptr, L"openas", abs.c_str(),
				nullptr, abs.parent_path().c_str(), SW_SHOWNORMAL);
			return reinterpret_cast<INT_PTR>(retry) > 32;
		}

		perr("fzf", fzfShellExecError(code));
		return false;
	}

	// A bare interactive `fzf` (its own stdout still the console, i.e. not
	// piped / captured by `$(...)`) acts on the picked entry instead of
	// just printing it, since as a builtin it runs in-process and can
	// reach into the shell / shell32 directly: a directory gets `cd`'d
	// into, a file gets opened via fzfOpenFile(). Piped/substituted uses
	// (`cd "$(fzf)"`, `fzf | ...`) keep printing the selection so they
	// stay composable. Sets `handled` when it took one of those actions;
	// the caller falls back to printing otherwise.
	static int fzfActOnSelection(Executor& exec, const std::string& selected, bool& handled) {
		handled = false;
		if (_isatty(_fileno(stdout)) == 0) return 0;

		std::error_code ec;
		fs::path native = toNative(exec, selected);
		if (fs::is_directory(native, ec)) {
			handled = true;
			std::string err;
			if (!changeDirectory(exec, selected, err)) {
				perr("fzf", err);
				return 1;
			}

			return 0;
		}

		if (fs::is_regular_file(native, ec)) {
			handled = true;
			return fzfOpenFile(native) ? 0 : 1;
		}

		return 0;
	}

	// A terminal that understands OSC 1337 draws the picker itself: the
	// shell hands over the list and waits for the line it chose. Anywhere
	// else this is skipped and the in-console picker runs as before.
	static bool terminalDrawsPicker() {
		const char* flag = std::getenv("WBSHTERM_PICKER");
		return flag != nullptr && *flag != '0';
	}

	// ConPTY forwards only a few kilobytes of OSC output before it starts
	// dropping the rest, so a long list sent item by item loses its tail --
	// and the "end" that opens the overlay with it. The list travels as a
	// file instead; only its path goes through the console.
	static fs::path pickListPath() {
		std::error_code ec;
		const fs::path dir = fs::temp_directory_path(ec);
		if (ec) return fs::path();

		const std::wstring name = L"wbsh-pick-"
			+ std::to_wstring(static_cast<unsigned long>(::GetCurrentProcessId())) + L".txt";
		return dir / name;
	}

	static bool writePickList(const fs::path& path, const std::vector<std::string>& candidates) {
		if (path.empty()) return false;

		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out) return false;

		for (const std::string& candidate : candidates) out << candidate << '\n';

		out.flush();
		return out.good();
	}

	static void appendPickLine(std::string& request, const std::string& verb,
			const std::string& value) {
		request += "\x1b]1337;pick;" + verb;
		if (!value.empty()) request += ";" + percentEncodeRequest(value);

		request += "\a";
	}

	// Writing the list can fail -- a full or read-only temp directory -- and
	// a short list still fits through the console, so the item-by-item form
	// stays as the fallback.
	static void sendPickRequest(const std::vector<std::string>& candidates,
			const fs::path& list_path, bool list_written) {
		std::string request;
		appendPickLine(request, "begin", "fzf");

		if (list_written) {
			appendPickLine(request, "list", pathToUtf8(list_path));
		} else {
			for (const std::string& candidate : candidates) {
				appendPickLine(request, "item", candidate);
			}
		}

		appendPickLine(request, "end", std::string());
		writeTerminalRequest(request);
	}

	// The terminal replies by typing the choice, so this reads a line of key
	// events; an empty one means the user backed out. A character past the
	// BMP comes as two key events, one per surrogate, so the line is put
	// together in UTF-16 first.
	static bool readPickReplyFrom(HANDLE input, std::string& selected) {
		std::wstring line;
		for (;;) {
			INPUT_RECORD record{};
			DWORD read = 0;
			if (!::ReadConsoleInputW(input, &record, 1, &read) || read == 0) return false;
			if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) continue;

			const wchar_t letter = record.Event.KeyEvent.uChar.UnicodeChar;
			if (letter == L'\r' || letter == L'\n') break;
			if (letter == 0x1B) return false;
			if (letter != 0) line.push_back(letter);
		}

		if (line.empty()) return false;

		selected = wideToUtf8(line);
		return true;
	}

	// Read the console device rather than stdin: in `ls | fzf` stdin is the
	// pipe, and reading that ends the pick before the user has chosen
	// anything -- the terminal answer then lands on the next prompt.
	static bool readPickReply(std::string& selected) {
		const HANDLE input = ::CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
		if (input == INVALID_HANDLE_VALUE) return false;

		DWORD saved_mode = 0;
		::GetConsoleMode(input, &saved_mode);
		::SetConsoleMode(input, saved_mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT
			| ENABLE_PROCESSED_INPUT | ENABLE_MOUSE_INPUT | ENABLE_WINDOW_INPUT));

		const bool answered = readPickReplyFrom(input, selected);

		::SetConsoleMode(input, saved_mode);
		::CloseHandle(input);
		return answered;
	}

	static int pickThroughTerminal(Executor& exec, const std::vector<std::string>& candidates) {
		const fs::path list_path = pickListPath();
		const bool list_written = writePickList(list_path, candidates);

		sendPickRequest(candidates, list_path, list_written);

		std::string selected;
		const bool answered = readPickReply(selected);

		if (list_written) {
			std::error_code ec;
			fs::remove(list_path, ec);
		}

		if (!answered) return 130;

		bool handled = false;
		const int status = fzfActOnSelection(exec, selected, handled);
		if (handled) return status;

		std::fputs(selected.c_str(), stdout);
		std::fputc('\n', stdout);
		return 0;
	}

	static int finishPick(Executor& exec, bool accepted, const std::string& selected) {
		if (!accepted) return 130;

		bool handled = false;
		const int status = fzfActOnSelection(exec, selected, handled);
		if (handled) return status;

		std::fputs(selected.c_str(), stdout);
		std::fputc('\n', stdout);
		return 0;
	}

	static int pickInConsole(Executor& exec) {
		FzfSession session;
		CandidateFeed feed;
		if (stdinIsTty()) {
			startTreeWalk(feed, fs::current_path());
			session.feed = &feed;
		} else {
			readAllLines(exec, "-", session.candidates);
			if (session.candidates.empty()) return 1;
		}

		if (!fzfOpenConsole(session)) {
			perr("fzf", "not attached to a console");
			stopTreeWalk(feed);
			fzfCloseConsole(session);
			return 2;
		}

		std::string selected;
		const bool accepted = fzfRunLoop(session, selected);
		stopTreeWalk(feed);
		fzfCloseConsole(session);
		return finishPick(exec, accepted, selected);
	}

	static int builtin_fzf(Executor& exec, const std::vector<std::string>&) {
		if (!terminalDrawsPicker()) return pickInConsole(exec);

		const std::vector<std::string> candidates = collectCandidates(exec);
		if (candidates.empty()) return 1;

		return pickThroughTerminal(exec, candidates);
	}

	namespace replay_detail {

		struct ReplayKey {
			const char* name;
			WORD vk;
			WCHAR ch;
		};

		static const ReplayKey kReplayKeys[] = {
			{ "UP", VK_UP, 0 },         { "DOWN", VK_DOWN, 0 },
			{ "PGUP", VK_PRIOR, 0 },    { "PGDN", VK_NEXT, 0 },
			{ "ENTER", VK_RETURN, 0x0D }, { "ESC", VK_ESCAPE, 0x1B },
			{ "BS", VK_BACK, 0x08 },     { "TAB", VK_TAB, 0x09 },
		};

		static KEY_EVENT_RECORD pressOf(WORD vk, WCHAR ch, DWORD modifiers) {
			KEY_EVENT_RECORD key{};
			key.bKeyDown = TRUE;
			key.wRepeatCount = 1;
			key.wVirtualKeyCode = vk;
			key.uChar.UnicodeChar = ch;
			key.dwControlKeyState = modifiers;
			return key;
		}

		static bool namedKey(const std::string& name, KEY_EVENT_RECORD& key) {
			for (const ReplayKey& entry : kReplayKeys) {
				if (name != entry.name) continue;

				key = pressOf(entry.vk, entry.ch, 0);
				return true;
			}

			const bool ctrl_letter = name.size() == 3 && name.compare(0, 2, "C-") == 0
				&& std::isalpha(static_cast<unsigned char>(name[2])) != 0;
			if (!ctrl_letter) return false;

			const auto byte = static_cast<unsigned char>(name[2]);
			const auto letter = static_cast<WORD>(std::toupper(byte));
			key = pressOf(letter, static_cast<WCHAR>(letter - 'A' + 1), LEFT_CTRL_PRESSED);
			return true;
		}

		static void queueToken(const std::string& token, std::deque<KEY_EVENT_RECORD>& queue) {
			KEY_EVENT_RECORD key{};
			const bool named = token.size() > 2 && token.front() == '<' && token.back() == '>'
				&& namedKey(token.substr(1, token.size() - 2), key);
			if (named) {
				queue.push_back(key);
				return;
			}

			for (const wchar_t unit : utf8ToWide(token)) queue.push_back(pressOf(0, unit, 0));
		}

		static std::string spelledOut(const std::string& output) {
			std::string text;
			for (const char letter : output) {
				if (letter == '\x1b') text += "\\e";
				else if (letter == '\r') text += "\\r";
				else if (letter == '\n') text += "\\n\n";
				else text.push_back(letter);
			}

			return text;
		}

		struct ReplayRequest {
			std::string walk_dir;
			bool show_output = false;
			std::size_t first_key = 0;
		};

		static std::size_t positiveCount(const std::string& text, std::size_t fallback) {
			long long value = 0;
			return parseLL(text, value) && value > 0 ? static_cast<std::size_t>(value) : fallback;
		}

		// -c COLUMNS, -r ROWS (list rows), -w DIR and -o come before the keys.
		static ReplayRequest takeOptions(const std::vector<std::string>& args,
		                                 FzfSession& session) {
			ReplayRequest request;
			std::size_t i = 0;
			while (i < args.size()) {
				const std::string& arg = args[i];
				const bool takes_value = arg == "-c" || arg == "-r" || arg == "-w";
				const bool valued = takes_value && i + 1 < args.size();
				if (arg == "--") ++i;
				if (!valued && arg != "-o") break;

				if (arg == "-c") session.columns = positiveCount(args[i + 1], session.columns);
				if (arg == "-r") session.max_rows = positiveCount(args[i + 1], session.max_rows);
				if (arg == "-w") request.walk_dir = args[i + 1];
				if (arg == "-o") request.show_output = true;
				i += valued ? 2 : 1;
			}

			request.first_key = i;
			return request;
		}

	}  // namespace replay_detail

	// `__fzf [-c COLUMNS] [-r ROWS] [-w DIR] [-o] KEY...`: the picker over
	// stdin's lines, or the entries under DIR, with keys typed from the
	// arguments as `__lineedit` takes them (<UP> <DOWN> <PGUP> <PGDN>
	// <ENTER> <ESC> <BS> <TAB> <C-x>, or text). Prints what was picked and
	// with -o what the picker drew; the walk is finished before any key.
	static int builtin_fzf_replay(Executor& exec, const std::vector<std::string>& args) {
		FzfSession session;
		session.headless = true;
		const replay_detail::ReplayRequest request = replay_detail::takeOptions(args, session);
		for (std::size_t i = request.first_key; i < args.size(); ++i) {
			replay_detail::queueToken(args[i], session.replay);
		}

		CandidateFeed feed;
		if (request.walk_dir.empty()) {
			readAllLines(exec, "-", session.candidates);
		} else {
			startTreeWalk(feed, toNative(exec, request.walk_dir));
			feed.worker.join();
			session.feed = &feed;
		}

		std::string selected;
		const bool accepted = fzfRunLoop(session, selected);
		stopTreeWalk(feed);
		std::printf("%s\n", accepted ? ("accept [" + selected + "]").c_str() : "abort");
		if (request.show_output) {
			std::printf("%s\n", replay_detail::spelledOut(session.captured).c_str());
		}

		return 0;
	}

#else  // !_WIN32

	static int builtin_fzf(Executor& exec, const std::vector<std::string>&) {
		collectCandidates(exec);
		perr("fzf", "interactive picker requires a Windows console");
		return 1;
	}

#endif /* _WIN32 */

	void registerFzfBuiltin(Executor& exec) {
		exec.registerBuiltin("fzf", builtin_fzf);
#ifdef _WIN32
		exec.registerBuiltin("__fzf", builtin_fzf_replay);
#endif /* _WIN32 */
	}

}  // namespace wbsh

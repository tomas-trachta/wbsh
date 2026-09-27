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
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "coreutils_internal.h"
#include "executor.h"
#include "pathconv.h"
#include "termreq.h"

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

	static std::vector<FzfMatch> filterCandidates(const std::vector<std::string>& candidates,
	                                              const std::string& query) {
		std::vector<FzfMatch> matches;
		for (std::size_t i = 0; i < candidates.size(); ++i) {
			FzfMatch match;
			match.index = i;
			if (!fuzzyMatch(query, candidates[i], match.score, match.positions)) continue;
			matches.push_back(std::move(match));
		}

		std::stable_sort(matches.begin(), matches.end(),
			[](const FzfMatch& a, const FzfMatch& b) { return a.score > b.score; });

		return matches;
	}

	static bool stdinIsTty() {
#ifdef _WIN32
		return _isatty(_fileno(stdin)) != 0;
#else
		return false;
#endif
	}

	static void collectDefaultCandidates(const fs::path& dir, std::vector<std::string>& out) {
		std::error_code ec;
		fs::recursive_directory_iterator it(dir,
			fs::directory_options::skip_permission_denied, ec);
		if (ec) return;

		for (auto cur = it; cur != fs::recursive_directory_iterator(); cur.increment(ec)) {
			if (ec) break;
			if (cur->path().filename() == ".git") {
				cur.disable_recursion_pending();
				continue;
			}

			std::string rel = pathToUtf8(cur->path().lexically_relative(dir));
			std::replace(rel.begin(), rel.end(), '\\', '/');
			out.push_back(std::move(rel));
		}
	}

	static std::vector<std::string> collectCandidates(Executor& exec) {
		std::vector<std::string> lines;
		if (stdinIsTty()) {
			collectDefaultCandidates(fs::current_path(), lines);
			return lines;
		}

		readAllLines(exec, "-", lines);
		return lines;
	}

#ifdef _WIN32

	// State for one interactive picker session. Owns the CONIN$/CONOUT$
	// handles opened directly against the console device (not the
	// process's stdin/stdout) so the picker still works when piped, e.g.
	// `ls | fzf | xargs cat` — exactly how a real terminal fzf behaves.
	struct FzfSession {
		HANDLE h_in  = INVALID_HANDLE_VALUE;
		HANDLE h_out = INVALID_HANDLE_VALUE;
		DWORD saved_in_mode  = 0;
		DWORD saved_out_mode = 0;
		std::size_t max_rows = 15;

		std::string query;
		std::vector<std::string> candidates;
		std::vector<FzfMatch> matches;
		std::size_t selected = 0;
		std::size_t drawn_lines = 0;
	};

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
		DWORD wrote;
		::WriteFile(session.h_out, out.data(), static_cast<DWORD>(out.size()), &wrote, nullptr);
	}

	static void fzfRefilter(FzfSession& session) {
		session.matches = filterCandidates(session.candidates, session.query);
		session.selected = 0;
	}

	static std::string fzfHighlightLine(const std::string& text,
	                                    const std::vector<std::size_t>& positions) {
		std::string out;
		std::size_t next_position = 0;
		for (std::size_t i = 0; i < text.size(); ++i) {
			const bool hit = next_position < positions.size() && positions[next_position] == i;
			if (hit) {
				out += "\x1b[1;33m";
				++next_position;
			}

			out += text[i];
			if (hit) out += "\x1b[0m";
		}

		return out;
	}

	static void fzfRenderRow(FzfSession& session, std::size_t row, std::string& out) {
		const FzfMatch& match = session.matches[row];
		const bool is_selected = row == session.selected;
		out += is_selected ? "\x1b[7m> " : "  ";
		out += fzfHighlightLine(session.candidates[match.index], match.positions);
		if (is_selected) out += "\x1b[0m";
		out += "\x1b[0m\r\n";
	}

	static void fzfRender(FzfSession& session) {
		std::string out;
		if (session.drawn_lines > 0) out += "\x1b[" + std::to_string(session.drawn_lines) + "A";
		out += "\r\x1b[J";

		out += "> " + session.query + "\r\n";
		out += "  " + std::to_string(session.matches.size()) + "/"
		     + std::to_string(session.candidates.size()) + "\r\n";

		const std::size_t shown = std::min(session.max_rows, session.matches.size());
		for (std::size_t row = 0; row < shown; ++row) fzfRenderRow(session, row, out);

		session.drawn_lines = 2 + shown;
		fzfEmit(session, out);
	}

	static void fzfMoveSelection(FzfSession& session, int delta) {
		if (session.matches.empty()) return;
		const std::size_t shown = std::min(session.max_rows, session.matches.size());
		long next = static_cast<long>(session.selected) + delta;
		if (next < 0) next = 0;
		if (next >= static_cast<long>(shown)) next = static_cast<long>(shown) - 1;
		session.selected = static_cast<std::size_t>(next);
	}

	static void fzfInsertChar(FzfSession& session, const std::string& utf8) {
		session.query += utf8;
		fzfRefilter(session);
	}

	static void fzfBackspace(FzfSession& session) {
		if (session.query.empty()) return;
		std::size_t i = session.query.size() - 1;
		while (i > 0 && (static_cast<unsigned char>(session.query[i]) & 0xC0) == 0x80) --i;
		session.query.erase(i);
		fzfRefilter(session);
	}

	static std::string fzfEncodeChar(wchar_t ch, HANDLE h_in) {
		WCHAR pair[2] = { ch, 0 };
		int len = 1;
		if (ch >= 0xD800 && ch <= 0xDBFF) {
			INPUT_RECORD record;
			DWORD read = 0;
			if (::ReadConsoleInputW(h_in, &record, 1, &read) && read == 1
			    && record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown) {
				pair[1] = record.Event.KeyEvent.uChar.UnicodeChar;
				len = 2;
			}
		}

		char bytes[8] = {};
		const int length = ::WideCharToMultiByte(CP_UTF8, 0, pair, len, bytes, sizeof(bytes),
			nullptr, nullptr);
		return length > 0 ? std::string(bytes, length) : std::string();
	}

	// Handles one keystroke. Returns false when the modal loop should end;
	// `accepted` distinguishes Enter (true) from Esc / Ctrl-C / Ctrl-G (false).
	static bool fzfHandleKey(FzfSession& session, const KEY_EVENT_RECORD& k, bool& accepted) {
		const bool ctrl = (k.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
		const WORD vk = k.wVirtualKeyCode;

		if (vk == VK_RETURN) { accepted = !session.matches.empty(); return false; }
		if (vk == VK_ESCAPE
		    || (ctrl && (vk == 'C' || vk == 'G'))) { accepted = false; return false; }

		if (vk == VK_UP || (ctrl && vk == 'P')) { fzfMoveSelection(session, -1); return true; }
		if (vk == VK_DOWN
		    || (ctrl && vk == 'N') || vk == VK_TAB) { fzfMoveSelection(session, 1); return true; }
		if (vk == VK_BACK) { fzfBackspace(session); return true; }
		if (ctrl && vk == 'U') { session.query.clear(); fzfRefilter(session); return true; }

		const WCHAR ch = k.uChar.UnicodeChar;
		if (!ctrl && ch >= 0x20) fzfInsertChar(session, fzfEncodeChar(ch, session.h_in));
		return true;
	}

	static bool fzfRunLoop(FzfSession& session, std::string& selected) {
		bool accepted = false;
		fzfRefilter(session);
		fzfRender(session);

		while (true) {
			INPUT_RECORD rec;
			DWORD nread = 0;
			if (!::ReadConsoleInputW(session.h_in, &rec, 1, &nread) || nread == 0) break;
			if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;

			const bool keep_going = fzfHandleKey(session, rec.Event.KeyEvent, accepted);
			fzfRender(session);
			if (!keep_going) break;
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
	// events; an empty one means the user backed out.
	static bool readPickReplyFrom(HANDLE input, std::string& selected) {
		std::string line;
		for (;;) {
			INPUT_RECORD record{};
			DWORD read = 0;
			if (!::ReadConsoleInputW(input, &record, 1, &read) || read == 0) return false;
			if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) continue;

			const wchar_t letter = record.Event.KeyEvent.uChar.UnicodeChar;
			if (letter == L'\r' || letter == L'\n') break;
			if (letter == 0x1B) return false;
			if (letter == 0) continue;

			char bytes[8] = {};
			const int length = ::WideCharToMultiByte(CP_UTF8, 0, &letter, 1, bytes,
				static_cast<int>(sizeof(bytes)), nullptr, nullptr);
			if (length > 0) line.append(bytes, static_cast<std::size_t>(length));
		}

		if (line.empty()) return false;

		selected = line;
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

	static int builtin_fzf(Executor& exec, const std::vector<std::string>&) {
		FzfSession session;
		session.candidates = collectCandidates(exec);
		if (session.candidates.empty()) return 1;

		if (terminalDrawsPicker()) return pickThroughTerminal(exec, session.candidates);

		if (!fzfOpenConsole(session)) {
			perr("fzf", "not attached to a console");
			fzfCloseConsole(session);
			return 2;
		}

		std::string selected;
		const bool accepted = fzfRunLoop(session, selected);
		fzfCloseConsole(session);
		if (!accepted) return 130;

		bool handled = false;
		const int status = fzfActOnSelection(exec, selected, handled);
		if (handled) return status;

		std::fputs(selected.c_str(), stdout);
		std::fputc('\n', stdout);
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
	}

}  // namespace wbsh

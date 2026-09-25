/**
 * @file pick.cpp
 * @brief A fuzzy picker in the spirit of fzf, written against the SDK's
 *        terminal helpers alone.
 *
 * Lines come in on stdin or as arguments, the user narrows them with a
 * query and moves with the arrows, and the chosen line goes to stdout,
 * so `cd "$(ls | pick)"` and `pick a b c | xargs echo` both compose.
 * The picker itself draws on the console device, not on stdout, which
 * is why it keeps working with a pipe on either side.
 *
 * Besides typing, it understands Up/Down, Ctrl+P/Ctrl+N, Tab, PageUp,
 * PageDown, Home, End, Backspace, Ctrl+U (clear), Ctrl+W (delete word),
 * Enter, Escape, Ctrl+C and Ctrl+G, and it follows the window when the
 * terminal is resized.
 */

#include "wbshsdk.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace pick {

	struct Match {
		std::size_t index = 0;
		int score = 0;
		std::vector<std::size_t> positions;
	};

	struct Picker {
		WbshTerminal* terminal = nullptr;
		std::vector<std::string> candidates;
		std::vector<Match> matches;
		std::string query;
		std::size_t selected = 0;
		std::size_t first_shown = 0;
		std::size_t visible_rows = 10;
		std::size_t drawn_lines = 0;
	};

	enum class Outcome { Accepted, Cancelled, Interrupted, KeepGoing };

	/* ---- matching ---------------------------------------------------- */

	static bool isSeparator(unsigned char letter) {
		return letter == '/' || letter == '\\' || letter == '_' || letter == '-'
			|| letter == '.' || letter == ' ';
	}

	static bool isWordStart(const std::string& text, std::size_t at) {
		if (at == 0) return true;

		const unsigned char previous = static_cast<unsigned char>(text[at - 1]);
		const unsigned char current  = static_cast<unsigned char>(text[at]);
		if (isSeparator(previous)) return true;

		return std::islower(previous) && std::isupper(current);
	}

	static bool sameLetter(char a, char b) {
		return std::tolower(static_cast<unsigned char>(a))
			== std::tolower(static_cast<unsigned char>(b));
	}

	static int scoreStep(const std::string& text, std::size_t at, bool adjacent) {
		int score = 1;
		if (isWordStart(text, at)) score += 8;
		if (adjacent) score += 5;
		return score;
	}

	static bool matchFrom(const std::string& query, const std::string& text,
			std::size_t start, Match& out_match) {
		out_match.positions.clear();
		out_match.score = 0;

		std::size_t at = start;
		for (std::size_t qi = 0; qi < query.size(); ++qi) {
			while (at < text.size() && !sameLetter(text[at], query[qi])) ++at;
			if (at >= text.size()) return false;

			const bool adjacent = qi > 0 && out_match.positions.back() + 1 == at;
			out_match.score += scoreStep(text, at, adjacent);
			out_match.positions.push_back(at);
			++at;
		}

		out_match.score -= static_cast<int>(text.size()) / 8;
		return true;
	}

	// Every place the first query letter occurs is tried as an anchor and
	// the best-scoring run wins, so "fb" prefers "foo_bar" over "fbx".
	static bool bestMatch(const std::string& query, const std::string& text, Match& out_best) {
		bool found = false;
		Match attempt;

		for (std::size_t start = 0; start < text.size(); ++start) {
			if (!sameLetter(text[start], query[0])) continue;
			if (!matchFrom(query, text, start, attempt)) break;

			if (!found || attempt.score > out_best.score) {
				out_best.score = attempt.score;
				out_best.positions = attempt.positions;
				found = true;
			}
		}

		return found;
	}

	static void refilter(Picker& picker) {
		picker.matches.clear();

		for (std::size_t index = 0; index < picker.candidates.size(); ++index) {
			Match match;
			match.index = index;
			if (picker.query.empty() || bestMatch(picker.query, picker.candidates[index], match)) {
				picker.matches.push_back(match);
			}
		}

		std::stable_sort(picker.matches.begin(), picker.matches.end(),
			[](const Match& a, const Match& b) { return a.score > b.score; });

		picker.selected = 0;
		picker.first_shown = 0;
	}

	/* ---- drawing ----------------------------------------------------- */

	static std::string highlighted(const std::string& text,
			const std::vector<std::size_t>& positions) {
		std::string out;
		std::size_t next = 0;

		for (std::size_t at = 0; at < text.size(); ++at) {
			const bool lit = next < positions.size() && positions[next] == at;
			if (lit) { out += "\x1b[1;33m"; ++next; }
			out += text[at];
			if (lit) out += "\x1b[0m";
		}

		return out;
	}

	// The cut lands on a character boundary so a clipped line never ends
	// in half a UTF-8 sequence.
	static std::string clipped(const std::string& line, std::size_t width) {
		if (line.size() <= width) return line;

		std::size_t cut = width;
		while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) --cut;
		return line.substr(0, cut);
	}

	static std::string headerLine(const Picker& picker) {
		return "> " + picker.query + "\x1b[90m  " + std::to_string(picker.matches.size())
			+ "/" + std::to_string(picker.candidates.size()) + "\x1b[0m\r\n";
	}

	static std::string matchLine(const Picker& picker, std::size_t row, std::size_t width) {
		const Match& match = picker.matches[row];
		const std::string text = clipped(picker.candidates[match.index], width);
		const bool is_selected = row == picker.selected;

		std::string line = is_selected ? "\x1b[7m> " : "  ";
		line += highlighted(text, match.positions);
		return line + "\x1b[0m\r\n";
	}

	static void keepSelectionVisible(Picker& picker) {
		if (picker.selected < picker.first_shown) picker.first_shown = picker.selected;

		const std::size_t last_visible = picker.first_shown + picker.visible_rows;
		if (picker.selected >= last_visible) {
			picker.first_shown = picker.selected - picker.visible_rows + 1;
		}
	}

	static void fitToWindow(Picker& picker) {
		int columns = 0;
		int rows = 0;
		if (wbshTerminalSize(picker.terminal, &columns, &rows) != WBSH_OK) return;

		const std::size_t room = rows > 3 ? static_cast<std::size_t>(rows) - 3 : 1;
		picker.visible_rows = std::min<std::size_t>(room, 15);
		keepSelectionVisible(picker);
	}

	static std::string erasePrevious(const Picker& picker) {
		std::string out;
		if (picker.drawn_lines > 0) out += "\x1b[" + std::to_string(picker.drawn_lines) + "A";
		return out + "\r\x1b[J";
	}

	static void render(Picker& picker) {
		int columns = 0;
		wbshTerminalSize(picker.terminal, &columns, nullptr);
		const std::size_t width = columns > 4 ? static_cast<std::size_t>(columns) - 3 : 1;

		keepSelectionVisible(picker);
		std::string frame = erasePrevious(picker) + headerLine(picker);

		const std::size_t end = std::min(picker.matches.size(),
			picker.first_shown + picker.visible_rows);
		for (std::size_t row = picker.first_shown; row < end; ++row) {
			frame += matchLine(picker, row, width);
		}

		picker.drawn_lines = 1 + (end - picker.first_shown);
		wbshTerminalWrite(picker.terminal, frame.data(), frame.size());
	}

	/* ---- editing ----------------------------------------------------- */

	static void moveSelection(Picker& picker, long delta) {
		if (picker.matches.empty()) return;

		const long last = static_cast<long>(picker.matches.size()) - 1;
		const long target = std::clamp(static_cast<long>(picker.selected) + delta, 0L, last);
		picker.selected = static_cast<std::size_t>(target);
	}

	static void eraseLastCharacter(std::string& query) {
		if (query.empty()) return;

		std::size_t at = query.size() - 1;
		while (at > 0 && (static_cast<unsigned char>(query[at]) & 0xC0) == 0x80) --at;
		query.erase(at);
	}

	static void eraseLastWord(std::string& query) {
		while (!query.empty() && query.back() == ' ') query.pop_back();
		while (!query.empty() && query.back() != ' ') query.pop_back();
	}

	static bool editQuery(Picker& picker, const WbshKey& key) {
		const bool ctrl = (key.modifiers & WBSH_MOD_CTRL) != 0;

		if (key.kind == WBSH_KEY_BACKSPACE) { eraseLastCharacter(picker.query); return true; }
		if (ctrl && std::strcmp(key.text, "u") == 0) { picker.query.clear(); return true; }
		if (ctrl && std::strcmp(key.text, "w") == 0) { eraseLastWord(picker.query); return true; }
		if (key.kind == WBSH_KEY_CHAR && key.modifiers == 0) {
			picker.query += key.text;
			return true;
		}

		return false;
	}

	static bool navigate(Picker& picker, const WbshKey& key) {
		const bool ctrl = (key.modifiers & WBSH_MOD_CTRL) != 0;
		const long page = static_cast<long>(picker.visible_rows);

		switch (key.kind) {
		case WBSH_KEY_UP:        moveSelection(picker, -1);    return true;
		case WBSH_KEY_DOWN:      moveSelection(picker, 1);     return true;
		case WBSH_KEY_TAB:       moveSelection(picker, 1);     return true;
		case WBSH_KEY_PAGE_UP:   moveSelection(picker, -page); return true;
		case WBSH_KEY_PAGE_DOWN: moveSelection(picker, page);  return true;
		case WBSH_KEY_HOME:      picker.selected = 0;          return true;
		case WBSH_KEY_END:       moveSelection(picker, 1L << 30); return true;
		case WBSH_KEY_CHAR:
			if (ctrl && std::strcmp(key.text, "p") == 0) { moveSelection(picker, -1); return true; }
			if (ctrl && std::strcmp(key.text, "n") == 0) { moveSelection(picker, 1); return true; }
			return false;
		default:
			return false;
		}
	}

	static Outcome decide(const Picker& picker, const WbshKey& key) {
		const bool ctrl = (key.modifiers & WBSH_MOD_CTRL) != 0;

		if (key.kind == WBSH_KEY_ENTER) {
			return picker.matches.empty() ? Outcome::Cancelled : Outcome::Accepted;
		}
		if (key.kind == WBSH_KEY_ESCAPE) return Outcome::Cancelled;
		if (ctrl && std::strcmp(key.text, "c") == 0) return Outcome::Interrupted;
		if (ctrl && std::strcmp(key.text, "g") == 0) return Outcome::Cancelled;

		return Outcome::KeepGoing;
	}

	static Outcome handleKey(Picker& picker, const WbshKey& key) {
		const Outcome verdict = decide(picker, key);
		if (verdict != Outcome::KeepGoing) return verdict;

		if (key.kind == WBSH_KEY_RESIZE) { fitToWindow(picker); return Outcome::KeepGoing; }
		if (navigate(picker, key)) return Outcome::KeepGoing;
		if (editQuery(picker, key)) refilter(picker);

		return Outcome::KeepGoing;
	}

	static Outcome runLoop(Picker& picker) {
		refilter(picker);
		fitToWindow(picker);
		render(picker);

		while (true) {
			WbshKey key;
			if (wbshTerminalReadKey(picker.terminal, &key, -1) <= 0) return Outcome::Cancelled;

			const Outcome verdict = handleKey(picker, key);
			render(picker);
			if (verdict != Outcome::KeepGoing) return verdict;
		}
	}

	/* ---- the command ------------------------------------------------- */

	static void readCandidatesFromStdin(std::vector<std::string>& out_lines) {
		std::string line;
		while (std::getline(std::cin, line)) {
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (!line.empty()) out_lines.push_back(line);
		}
	}

	static void gatherCandidates(int argc, const char* const* argv,
			std::vector<std::string>& out_lines) {
		for (int i = 1; i < argc; ++i) out_lines.push_back(argv[i]);

		if (out_lines.empty() && !wbshIsTerminal(0)) readCandidatesFromStdin(out_lines);
	}

	static int exitStatus(Outcome outcome) {
		switch (outcome) {
		case Outcome::Accepted:    return 0;
		case Outcome::Interrupted: return 130;
		case Outcome::Cancelled:   return 1;
		default:                   return 1;
		}
	}

	static int runPicker(std::vector<std::string> candidates) {
		Picker picker;
		picker.candidates = std::move(candidates);
		picker.terminal = wbshTerminalOpen();
		if (picker.terminal == nullptr) {
			wbshPrintError("pick: no terminal to draw on\n");
			return 2;
		}

		const Outcome outcome = runLoop(picker);
		const std::string clear = erasePrevious(picker);
		wbshTerminalWrite(picker.terminal, clear.data(), clear.size());
		wbshTerminalClose(picker.terminal);

		if (outcome == Outcome::Accepted) {
			wbshPrint(picker.candidates[picker.matches[picker.selected].index].c_str());
			wbshPrint("\n");
		}

		return exitStatus(outcome);
	}

	static int reportSize() {
		WbshTerminal* terminal = wbshTerminalOpen();
		if (terminal == nullptr) { wbshPrint("no terminal\n"); return 1; }

		int columns = 0;
		int rows = 0;
		const int status = wbshTerminalSize(terminal, &columns, &rows);
		wbshTerminalClose(terminal);
		if (status != WBSH_OK) { wbshPrint("no terminal\n"); return 1; }

		char line[64];
		std::snprintf(line, sizeof(line), "%dx%d\n", columns, rows);
		wbshPrint(line);
		return 0;
	}

	static int reportTty() {
		char line[64];
		std::snprintf(line, sizeof(line), "stdin=%d stdout=%d stderr=%d\n",
			wbshIsTerminal(0), wbshIsTerminal(1), wbshIsTerminal(2));
		wbshPrint(line);
		return 0;
	}

	static int usage() {
		wbshPrint("usage: pick [item...]     choose one of the items, or of the lines on stdin\n"
		          "       pick --size        print the terminal's columns and rows\n"
		          "       pick --tty         say which of stdin, stdout, stderr is a terminal\n");
		return 0;
	}

	static int command(void*, int argc, const char* const* argv) {
		if (argc > 1 && std::strcmp(argv[1], "--help") == 0) return usage();
		if (argc > 1 && std::strcmp(argv[1], "--size") == 0) return reportSize();
		if (argc > 1 && std::strcmp(argv[1], "--tty") == 0) return reportTty();

		std::vector<std::string> candidates;
		gatherCandidates(argc, argv, candidates);
		if (candidates.empty()) { wbshPrintError("pick: nothing to choose from\n"); return 1; }

		return runPicker(std::move(candidates));
	}

}  /* namespace pick */

static const WbshUtilInfo kInfo = {
	WBSH_SDK_ABI,
	"pick",
	"1.0.0",
	"A fuzzy picker built on the SDK's terminal helpers."
};

extern "C" {

WBSH_UTIL_API const WbshUtilInfo* wbshUtilDescribe(void) {
	return &kInfo;
}

WBSH_UTIL_API int wbshUtilLoad(WbshHostKind host) {
	if (host == WBSH_HOST_SHELL) wbshRegisterCommand("pick", pick::command, nullptr);

	return WBSH_OK;
}

WBSH_UTIL_API void wbshUtilUnload(void) {
}

}  /* extern "C" */

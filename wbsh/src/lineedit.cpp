/**
 * @file lineedit.cpp
 * @brief Raw-console line editor: keys, redraw, history, and Tab completion.
 */

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
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "consolekeys.h"
#include "strscan.h"
#include "termreq.h"
#include "textcells.h"
#include "utils.h"

namespace wbsh {

	namespace fs = std::filesystem;

	static const std::size_t kDefaultConsoleWidth = 80;
	static const std::size_t kNoHistoryIndex = static_cast<std::size_t>(-1);
	static const char* const kDimStyle = "\x1b[90m";
	static const char* const kResetStyle = "\x1b[0m";
	static const char* const kEraseToEnd = "\r\x1b[J";

#ifdef _WIN32
	static bool stdinIsTty() {
		return _isatty(_fileno(stdin)) != 0;
	}

	static std::size_t skipCsiSequence(const std::string& text, std::size_t i) {
		i += 2;
		while (i < text.size() && !(text[i] >= '@' && text[i] <= '~')) ++i;
		if (i < text.size()) ++i;
		return i;
	}

	static std::string withoutCsiSequences(const std::string& text) {
		std::string shown;
		std::size_t i = 0;
		while (i < text.size()) {
			if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
				i = skipCsiSequence(text, i);
				continue;
			}

			shown.push_back(text[i]);
			++i;
		}

		return shown;
	}

	static std::size_t visibleLen(const std::string& text) {
		const std::string shown = withoutCsiSequences(text);
		return displayWidth(shown, shown.size());
	}
#endif /* _WIN32 */

	static bool isSpaceByte(char letter) {
		return std::isspace(static_cast<unsigned char>(letter)) != 0;
	}

	static bool isWordBreak(char letter) {
		return isSpaceByte(letter)
		    || letter == '|' || letter == '&' || letter == ';'
		    || letter == '<' || letter == '>'
		    || letter == '(' || letter == ')';
	}

	static bool hasPrefix(const std::string& text, const std::string& prefix) {
		return text.compare(0, prefix.size(), prefix) == 0;
	}

	std::size_t findReverseSearchMatch(const std::vector<std::string>& history,
	                                   const std::string& query,
	                                   std::size_t start_index,
	                                   bool forward) {
		if (query.empty() || history.empty()) return history.size();

		const std::size_t start = std::min<std::size_t>(start_index, history.size() - 1);
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

	static bool extendsPrefix(const std::string& entry, const std::string& prefix) {
		return entry.size() > prefix.size() && hasPrefix(entry, prefix);
	}

	// Newest-first scan for the first history entry that begins with
	// `prefix` and is strictly longer than it, skipping entries whose exit
	// status was non-zero. Returns the matching entry's index (history.size()
	// when nothing matches) so callers can cache and incrementally extend
	// the match on subsequent keystrokes instead of rescanning.
	static std::size_t findInlinePredictionIndexed(const std::vector<std::string>& history,
	                                               const std::vector<int>& history_status,
	                                               const std::string& prefix) {
		if (prefix.empty()) return history.size();

		for (std::size_t i = history.size(); i > 0; --i) {
			const std::size_t index = i - 1;
			if (index < history_status.size() && history_status[index] != 0) continue;
			if (extendsPrefix(history[index], prefix)) return index;
		}

		return history.size();
	}

	std::string findInlinePrediction(const std::vector<std::string>& history,
	                                 const std::vector<int>& history_status,
	                                 const std::string& prefix) {
		const std::size_t index = findInlinePredictionIndexed(history, history_status, prefix);
		if (index >= history.size()) return {};

		return history[index].substr(prefix.size());
	}

	static void stripLineEnding(std::string& line) {
		while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
	}

	static fs::path findGitEntry(fs::path& probe) {
		std::error_code ec;
		for (;;) {
			const fs::path candidate = probe / ".git";
			if (fs::exists(candidate, ec)) return candidate;

			const fs::path parent = probe.parent_path();
			if (parent == probe) return {};

			probe = parent;
		}
	}

	static fs::path resolveGitDirFile(const fs::path& gitfile, const fs::path& worktree) {
		std::ifstream file(gitfile);
		std::string line;
		if (!std::getline(file, line)) return {};

		stripLineEnding(line);
		const std::string prefix = "gitdir: ";
		if (!hasPrefix(line, prefix)) return {};

		fs::path target = utf8ToPath(line.substr(prefix.size()));
		if (!target.is_absolute()) target = (worktree / target).lexically_normal();
		return target;
	}

	fs::path findGitDir() {
		std::error_code ec;
		fs::path probe = fs::current_path(ec);
		if (ec) return {};

		const fs::path gitdir = findGitEntry(probe);
		if (gitdir.empty()) return {};
		if (fs::is_directory(gitdir, ec)) return gitdir;

		return resolveGitDirFile(gitdir, probe);
	}

	fs::path findGitCommonDir(const fs::path& git_dir) {
		std::ifstream file(git_dir / "commondir");
		std::string line;
		if (!std::getline(file, line)) return git_dir;

		stripLineEnding(line);
		if (line.empty()) return git_dir;

		fs::path common = utf8ToPath(line);
		if (!common.is_absolute()) common = (git_dir / common).lexically_normal();
		return common;
	}

	bool usesRefTable(const fs::path& common_dir) {
		std::error_code ec;
		return fs::is_directory(common_dir / "reftable", ec);
	}

	LineEditor::LineEditor(Environment& env, Executor& exec)
		: env_(env), exec_(exec) {}

	LineStatus LineEditor::readLine(const std::string& prompt, std::string& out) {
		resetLine(prompt);

#ifdef _WIN32
		if (stdinIsTty()) return readLineRaw(prompt, out);
#endif /* _WIN32 */

		std::fputs(prompt.c_str(), stdout);
		std::fflush(stdout);
		return readLineCooked(out);
	}

	void LineEditor::resetLine(const std::string& prompt) {
		buffer_.clear();
		cursor_ = 0;
		history_pos_ = 0;
		saved_partial_.clear();
		last_was_tab_ = false;
		last_tab_word_.clear();
		last_cursor_row_ = 0;
		suggestion_.clear();
		suggestion_cache_idx_ = kNoHistoryIndex;
		revsearch_active_ = false;
		interrupted_ = false;
		done_ = false;
		eof_ = false;
		accepted_.clear();
		pending_keys_.clear();
		last_command_.clear();
		undo_.clear();
		insert_run_ = 0;
		prompt_raw_ = prompt;

#ifdef _WIN32
		prompt_visible_len_ = visibleLen(prompt);
#endif /* _WIN32 */
	}

	LineStatus LineEditor::readLineCooked(std::string& out) {
		out.clear();

		int byte;
		while ((byte = std::fgetc(stdin)) != EOF) {
			if (byte == '\n') return LineStatus::kLine;
			out.push_back(static_cast<char>(byte));
		}

		return out.empty() ? LineStatus::kEof : LineStatus::kLine;
	}

	LineStatus LineEditor::lineStatus() const {
		if (interrupted_) return LineStatus::kInterrupted;
		if (eof_ && accepted_.empty()) return LineStatus::kEof;

		return LineStatus::kLine;
	}

	std::string LineEditor::describeReplay(bool finished) const {
		if (!finished) {
			return "[" + buffer_.substr(0, cursor_) + "][" + buffer_.substr(cursor_) + "]";
		}

		switch (lineStatus()) {
		case LineStatus::kInterrupted: return "interrupt";
		case LineStatus::kEof:         return "eof";
		case LineStatus::kLine:        return "accept [" + accepted_ + "]";
		default:                       return {};
		}
	}

	std::vector<std::string> LineEditor::currentMatches(Tok& tok) {
		const std::string prefix = buffer_.substr(tok.start, tok.end - tok.start);
		return completionsFor(prefix, tok.first);
	}

#ifdef _WIN32

	static std::size_t consoleWidth() {
		const HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
		if (out == INVALID_HANDLE_VALUE) return kDefaultConsoleWidth;

		CONSOLE_SCREEN_BUFFER_INFO info{};
		if (!::GetConsoleScreenBufferInfo(out, &info)) return kDefaultConsoleWidth;

		const int width = info.srWindow.Right - info.srWindow.Left + 1;
		if (width <= 0) return kDefaultConsoleWidth;

		return static_cast<std::size_t>(width);
	}

	static std::string utf16ToUtf8(const WCHAR* units, int count) {
		const int length = ::WideCharToMultiByte(CP_UTF8, 0, units, count,
			nullptr, 0, nullptr, nullptr);
		if (length <= 0) return {};

		std::string utf8(static_cast<std::size_t>(length), '\0');
		::WideCharToMultiByte(CP_UTF8, 0, units, count, utf8.data(), length, nullptr, nullptr);
		return utf8;
	}

	static bool readInputRecord(HANDLE input, INPUT_RECORD& record) {
		if (input == INVALID_HANDLE_VALUE) return false;

		DWORD read = 0;
		return ::ReadConsoleInputW(input, &record, 1, &read) && read != 0;
	}

	static void appendCursorUp(std::string& out, std::size_t rows) {
		if (rows > 0) out += "\x1b[" + std::to_string(rows) + "A";
	}

	static void appendCursorRight(std::string& out, std::size_t columns) {
		if (columns > 0) out += "\x1b[" + std::to_string(columns) + "C";
	}

	void LineEditor::emit(const std::string& text) {
		if (headless_) return;

		const HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
		DWORD wrote;
		::WriteFile(out, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
	}

	struct ScreenPos {
		std::size_t row;
		std::size_t col;
	};

	// Where writing text[0, end) from `pos` leaves the cursor on a screen
	// `width` cells wide. A cluster that does not fit on the row moves
	// whole to the next one, as the console wraps it, and `col == width`
	// is the pending wrap after a row's last cell.
	static ScreenPos advanceOver(const std::string& text, std::size_t end, ScreenPos pos,
	                             std::size_t width) {
		std::size_t at = 0;
		while (at < end && at < text.size()) {
			if (text[at] == '\n') {
				pos = { pos.row + 1, 0 };
				++at;
				continue;
			}

			const TextCluster cluster = clusterAt(text, at);
			if (pos.col + cluster.width > width) pos = { pos.row + 1, 0 };
			pos.col += cluster.width;
			at = cluster.end;
		}

		return pos;
	}

	static ScreenPos settled(ScreenPos pos, std::size_t width) {
		if (pos.col < width) return pos;

		return { pos.row + 1, 0 };
	}

	static std::string withCrLf(const std::string& text) {
		std::string out;
		out.reserve(text.size());
		for (const char letter : text) {
			if (letter == '\n') out.push_back('\r');
			out.push_back(letter);
		}

		return out;
	}

	void LineEditor::appendLineText(std::string& out) const {
		out += withCrLf(buffer_);
		if (suggestion_.empty()) return;

		out += kDimStyle;
		out += withCrLf(suggestion_);
		out += kResetStyle;
	}

	// A line that ends exactly at the right edge leaves the console's
	// cursor waiting on that edge; the CRLF puts it on the row below,
	// where the layout counts it.
	void LineEditor::redraw() {
		const std::size_t width = consoleWidth();
		refreshSuggestion();

		const ScreenPos start = { prompt_visible_len_ / width, prompt_visible_len_ % width };
		const ScreenPos cursor = settled(advanceOver(buffer_, cursor_, start, width), width);
		const ScreenPos text_end = advanceOver(buffer_, buffer_.size(), start, width);
		const ScreenPos end = advanceOver(suggestion_, suggestion_.size(), text_end, width);

		std::string out;
		appendCursorUp(out, last_cursor_row_);
		out += kEraseToEnd;
		out += prompt_raw_;
		appendLineText(out);
		if (end.col >= width) out += "\r\n";

		out += "\r";
		appendCursorUp(out, settled(end, width).row - cursor.row);
		appendCursorRight(out, cursor.col);

		last_cursor_row_ = cursor.row;
		emit(out);
	}

	void LineEditor::moveToLineEnd() {
		cursor_ = buffer_.size();
		redraw();
		emit("\x1b[J");
	}

	void LineEditor::refreshSuggestion() {
		suggestion_.clear();
		if (revsearch_active_ || buffer_.empty() || cursor_ != buffer_.size()) {
			suggestion_cache_idx_ = kNoHistoryIndex;
			return;
		}

		// Fast path: if the cached match still begins with the (possibly
		// grown or shrunk) buffer, reuse it instead of rescanning the
		// whole history on every keystroke.
		const auto& history = exec_.history();
		if (suggestion_cache_idx_ < history.size()) {
			const std::string& entry = history[suggestion_cache_idx_];
			if (extendsPrefix(entry, buffer_)) {
				suggestion_ = entry.substr(buffer_.size());
				return;
			}
		}

		suggestion_cache_idx_ =
			findInlinePredictionIndexed(history, exec_.historyStatus(), buffer_);
		if (suggestion_cache_idx_ < history.size()) {
			suggestion_ = history[suggestion_cache_idx_].substr(buffer_.size());
		}
	}

	bool LineEditor::acceptInlineSuggestion() {
		if (suggestion_.empty()) return false;

		insertChars(suggestion_);
		suggestion_.clear();
		return true;
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

	}  // namespace complete_detail

	LineEditor::Tok LineEditor::currentToken() const {
		Tok token{ cursor_, cursor_, true };

		std::size_t start = cursor_;
		while (start > 0 && !isWordBreak(buffer_[start - 1])) --start;
		token.start = start;
		token.end = cursor_;

		std::size_t before = token.start;
		while (before > 0 && isSpaceByte(buffer_[before - 1])) --before;
		if (before == 0) return token;

		const char prev = buffer_[before - 1];
		token.first = prev == '|' || prev == '&' || prev == ';' || prev == '(' || prev == '{'
			|| complete_detail::commandWordsBefore(buffer_, token.start).words.empty();
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
			if (hasPrefix(name, prefix)) into.insert(name);
		}
	}

	std::vector<std::string> LineEditor::commandCompletions(const std::string& prefix) {
		std::set<std::string> names;
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
			if (!hasPrefix(name, base)) continue;
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

	std::vector<std::string> LineEditor::completionsFor(const std::string& prefix,
	                                                    bool command_pos) {
		if (command_pos && prefix.find('/') == std::string::npos) return commandCompletions(prefix);

		const Tok current = currentToken();
		const std::vector<std::string> tools = toolCompletions(prefix, current);
		if (!tools.empty()) return tools;

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

	static void runCompletionFunction(Executor& exec, const std::string& function,
	                                  const std::vector<std::string>& args) {
		exec.callFunction(function, args);

		// A completion function calling exit / break / return must
		// not tear down the interactive shell — drop any signal
		// (and stray expansion error) it left behind.
		exec.clearFlow();
		if (exec.expander().failed()) exec.expander().takeError();
	}

	static void appendCompReply(const Environment& env, const std::string& prefix,
	                            std::vector<std::string>& matches) {
		const auto* reply = env.getIndexedArray("COMPREPLY");
		if (reply == nullptr) return;

		for (const auto& entry : *reply) {
			if (hasPrefix(entry.second, prefix)) matches.push_back(entry.second);
		}
	}

	void LineEditor::appendFunctionCompletions(const std::string& function,
	                                           const std::string& prefix, const Tok& tok,
	                                           const std::vector<std::string>& prev,
	                                           std::vector<std::string>& matches) {
		Environment& env = exec_.env();

		const std::size_t start = complete_detail::commandWordsBefore(buffer_, tok.start).start;
		const std::size_t end = complete_detail::commandEndAfter(buffer_, tok.end);
		const std::size_t word_end = complete_detail::wordEndAfter(buffer_, tok.end);
		const std::vector<std::string> words =
			complete_detail::wordsOfCommand(buffer_, prev, tok.start, word_end, end);
		publishCompWords(env, words, prev.size());
		publishCompLine(env, buffer_.substr(start, end - start), tok.end - start);

		runCompletionFunction(exec_, function, { prev.front(), prefix, prev.back() });
		appendCompReply(env, prefix, matches);
	}

	std::vector<std::string> LineEditor::specCompletions(const Executor::CompletionSpec& spec,
	                                                     const std::string& prefix, const Tok& tok,
	                                                     const std::vector<std::string>& prev) {
		std::vector<std::string> matches;
		for (const std::string& word : spec.words) {
			if (hasPrefix(word, prefix)) matches.push_back(word);
		}

		if (!spec.function.empty() && exec_.isFunction(spec.function)) {
			appendFunctionCompletions(spec.function, prefix, tok, prev, matches);
		}

		if (!matches.empty()) return matches;
		if (spec.include_dirs || spec.include_files || spec.default_fallback) {
			return pathCompletions(prefix);
		}

		return {};
	}

	std::vector<std::string> LineEditor::toolCompletions(const std::string& prefix,
	                                                     const Tok& tok) {
		const std::vector<std::string> prev = prevTokensBefore(tok);
		if (prev.empty()) return {};

		const std::string& head = prev.front();
		const Executor::CompletionSpec* spec = exec_.completionSpec(head);
		if (spec != nullptr) return specCompletions(*spec, prefix, tok, prev);

		const PluginCommand* plugin = exec_.pluginCommand(head);
		if (plugin != nullptr && plugin->complete != nullptr) {
			return pluginCompletions(*plugin, prefix, prev);
		}

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
			if (candidate.compare(0, prefix.size(), prefix) == 0) matches.push_back(candidate);
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
			if (hasPrefix(word, prefix)) out.push_back(std::move(word));
		}

		return out;
	}

	static std::vector<std::string> filterPrefix(std::vector<std::string> words,
	                                             const std::string& prefix) {
		std::vector<std::string> out;
		for (std::string& word : words) {
			if (hasPrefix(word, prefix)) out.push_back(std::move(word));
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

		std::string prefix = words.front();
		for (std::size_t i = 1; i < words.size(); ++i) {
			std::size_t k = 0;
			while (k < prefix.size() && k < words[i].size() && prefix[k] == words[i][k]) ++k;
			prefix.resize(k);
			if (prefix.empty()) break;
		}

		return prefix;
	}

	static std::size_t longestLength(const std::vector<std::string>& matches) {
		std::size_t longest = 0;
		for (const std::string& match : matches) {
			longest = std::max<std::size_t>(longest, match.size());
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
			if (more_in_row) line.append(pad - matches[index].size(), ' ');
		}

		return line;
	}

	void LineEditor::printMatches(const std::vector<std::string>& matches) {
		emit("\r\n");

		const std::size_t pad = longestLength(matches) + 2;
		const std::size_t cols = std::max<std::size_t>(1, consoleWidth() / pad);
		const std::size_t rows = (matches.size() + cols - 1) / cols;
		for (std::size_t row = 0; row < rows; ++row) {
			emit(matchesRow(matches, row, rows, cols, pad) + "\r\n");
		}

		last_cursor_row_ = 0;
		redraw();
	}

	void LineEditor::applyCompletion(const Tok& tok,
	                                 const std::vector<std::string>& matches) {
		if (matches.empty()) {
			ding();
			last_was_tab_ = false;
			return;
		}

		const std::string current = buffer_.substr(tok.start, tok.end - tok.start);
		if (matches.size() == 1) {
			std::string replacement = matches[0];
			if (replacement.empty() || replacement.back() != '/') replacement.push_back(' ');

			buffer_.replace(tok.start, tok.end - tok.start, replacement);
			cursor_ = tok.start + replacement.size();
			last_was_tab_ = false;
			return;
		}

		const std::string common = longestCommonPrefix(matches);
		if (common.size() > current.size()) {
			buffer_.replace(tok.start, tok.end - tok.start, common);
			cursor_ = tok.start + common.size();
			last_was_tab_ = true;
			last_tab_word_ = common;
			return;
		}

		printMatches(matches);
		last_was_tab_ = true;
		last_tab_word_ = current;
	}

	static bool readClipboardText(std::wstring& text) {
		if (!::OpenClipboard(nullptr)) return false;

		const HANDLE clip = ::GetClipboardData(CF_UNICODETEXT);
		if (clip == nullptr) {
			::CloseClipboard();
			return false;
		}

		const auto* units = static_cast<const WCHAR*>(::GlobalLock(clip));
		if (units == nullptr) {
			::CloseClipboard();
			return false;
		}

		text = units;
		::GlobalUnlock(clip);
		::CloseClipboard();
		return true;
	}

	// Strip CR/LF: a multi-line paste would corrupt our single-line
	// redraw. The user can press Enter themselves between lines.
	static std::wstring withoutLineBreaks(const std::wstring& text) {
		std::wstring filtered;
		filtered.reserve(text.size());
		for (const WCHAR unit : text) {
			if (unit == L'\r' || unit == L'\n') continue;
			filtered.push_back(unit);
		}

		return filtered;
	}

	void LineEditor::insertClipboardText() {
		std::wstring text;
		if (!readClipboardText(text)) return;

		const std::wstring filtered = withoutLineBreaks(text);
		if (filtered.empty()) return;

		insertChars(utf16ToUtf8(filtered.data(), static_cast<int>(filtered.size())));
	}

	std::string LineEditor::replayCharacter(wchar_t ch) {
		WCHAR units[2] = { ch, 0 };
		const bool high = ch >= 0xD800 && ch <= 0xDBFF;
		const bool low_follows = !replay_keys_.empty()
			&& replay_keys_.front().uChar.UnicodeChar >= 0xDC00
			&& replay_keys_.front().uChar.UnicodeChar <= 0xDFFF;
		if (!high || !low_follows) return utf16ToUtf8(units, 1);

		units[1] = replay_keys_.front().uChar.UnicodeChar;
		replay_keys_.pop_front();
		return utf16ToUtf8(units, 2);
	}

	std::string LineEditor::wideCharacter(wchar_t ch) {
		if (headless_) return replayCharacter(ch);

		return readConsoleCharacter(::GetStdHandle(STD_INPUT_HANDLE), ch);
	}

	static std::size_t lastHistoryIndex(const std::vector<std::string>& history) {
		return history.empty() ? 0 : history.size() - 1;
	}

	void LineEditor::revsearchRefresh(const std::string& query,
	                                  std::size_t match_index) {
		prompt_raw_ = "(reverse-i-search)`" + query + "': ";
		prompt_visible_len_ = visibleLen(prompt_raw_);

		const auto& history = exec_.history();
		if (match_index < history.size()) {
			buffer_ = history[match_index];
			const std::size_t found = buffer_.find(query);
			cursor_ = found == std::string::npos ? buffer_.size() : found + query.size();
		} else {
			buffer_.clear();
			cursor_ = 0;
		}

		last_cursor_row_ = 0;
		emit(kEraseToEnd);
		redraw();
	}

	void LineEditor::revsearchStepOlder(const std::string& query,
	                                    std::size_t& match_index) {
		const auto& history = exec_.history();
		if (match_index == 0) {
			ding();
			return;
		}

		const std::size_t start = match_index < history.size()
			? match_index - 1 : lastHistoryIndex(history);
		const std::size_t next = findReverseSearchMatch(history, query, start, false);
		if (next == history.size()) {
			ding();
			return;
		}

		match_index = next;
		revsearchRefresh(query, match_index);
	}

	void LineEditor::revsearchStepNewer(const std::string& query,
	                                    std::size_t& match_index) {
		const auto& history = exec_.history();
		if (match_index >= history.size() || match_index + 1 >= history.size()) {
			ding();
			return;
		}

		const std::size_t next = findReverseSearchMatch(history, query, match_index + 1, true);
		if (next == history.size()) {
			ding();
			return;
		}

		match_index = next;
		revsearchRefresh(query, match_index);
	}

	void LineEditor::revsearchEraseQueryChar(std::string& query, std::size_t& match_index) {
		if (query.empty()) {
			ding();
			return;
		}

		query.pop_back();
		const auto& history = exec_.history();
		match_index = findReverseSearchMatch(history, query, lastHistoryIndex(history), false);
		revsearchRefresh(query, match_index);
	}

	void LineEditor::revsearchExtendQuery(std::string& query, char letter,
	                                      std::size_t& match_index) {
		query.push_back(letter);

		const auto& history = exec_.history();
		const std::size_t start = match_index < history.size()
			? match_index : lastHistoryIndex(history);
		match_index = findReverseSearchMatch(history, query, start, false);
		revsearchRefresh(query, match_index);
	}

	namespace keys_detail {

		static const DWORD kCtrlBits = LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED;
		static const DWORD kAltBits = LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED;
		static const char* const kMetaPrefix = "\x1b";

		static bool ctrlHeld(const KEY_EVENT_RECORD& key) {
			return (key.dwControlKeyState & kCtrlBits) != 0;
		}

		static bool altHeld(const KEY_EVENT_RECORD& key) {
			return (key.dwControlKeyState & kAltBits) != 0;
		}

		static bool shiftHeld(const KEY_EVENT_RECORD& key) {
			return (key.dwControlKeyState & SHIFT_PRESSED) != 0;
		}

		// Windows reports AltGr as Ctrl+Alt, so Ctrl+Alt with a character
		// is that character from the layout's third level (a Czech @ or {),
		// not a chord.
		static bool typesLayoutCharacter(const KEY_EVENT_RECORD& key) {
			return ctrlHeld(key) && altHeld(key) && key.uChar.UnicodeChar >= 0x20;
		}

		// AltGr on a key the layout gives nothing for.
		static bool isEmptyAltGr(const KEY_EVENT_RECORD& key) {
			const DWORD state = key.dwControlKeyState;
			return (state & RIGHT_ALT_PRESSED) != 0 && (state & LEFT_CTRL_PRESSED) != 0
			    && key.uChar.UnicodeChar == 0;
		}

		static bool isInterruptKey(const KEY_EVENT_RECORD& key) {
			if (typesLayoutCharacter(key)) return false;

			const bool ctrl_c = ctrlHeld(key) && !altHeld(key) && key.wVirtualKeyCode == 'C';
			return key.uChar.UnicodeChar == 0x03 || ctrl_c;
		}

		static int modifierCode(const KEY_EVENT_RECORD& key) {
			return 1 + (altHeld(key) ? 2 : 0) + (ctrlHeld(key) ? 4 : 0);
		}

		static std::string cursorKey(char final_byte, int modifier) {
			if (modifier == 1) return std::string("\x1b[") + final_byte;

			return "\x1b[1;" + std::to_string(modifier) + final_byte;
		}

		static std::string tildeKey(int number, int modifier) {
			const std::string code = "\x1b[" + std::to_string(number);
			if (modifier == 1) return code + "~";

			return code + ";" + std::to_string(modifier) + "~";
		}

		// Ctrl+Backspace erases a word as Alt+Backspace does; Shift alone
		// does not change a cursor key.
		static std::string editingKey(const KEY_EVENT_RECORD& key) {
			const bool meta = altHeld(key);
			switch (key.wVirtualKeyCode) {
			case VK_BACK:   return meta || ctrlHeld(key) ? "\x1b\x7f" : "\x7f";
			case VK_TAB:    return shiftHeld(key) ? "\x1b[Z" : (meta ? "\x1b\t" : "\t");
			case VK_RETURN: return meta ? "\x1b\r" : "\r";
			case VK_ESCAPE: return kMetaPrefix;
			default:        return {};
			}
		}

		// The keys with no character of their own, spelled as xterm sends them.
		static std::string specialKey(const KEY_EVENT_RECORD& key) {
			const int modifier = modifierCode(key);
			switch (key.wVirtualKeyCode) {
			case VK_UP:     return cursorKey('A', modifier);
			case VK_DOWN:   return cursorKey('B', modifier);
			case VK_RIGHT:  return cursorKey('C', modifier);
			case VK_LEFT:   return cursorKey('D', modifier);
			case VK_HOME:   return cursorKey('H', modifier);
			case VK_END:    return cursorKey('F', modifier);
			case VK_INSERT: return tildeKey(2, modifier);
			case VK_DELETE: return tildeKey(3, modifier);
			case VK_PRIOR:  return tildeKey(5, modifier);
			case VK_NEXT:   return tildeKey(6, modifier);
			default:        return editingKey(key);
			}
		}

		// Ctrl with a key Windows gives no character for.
		static std::string controlCharacter(const KEY_EVENT_RECORD& key) {
			const WORD vk = key.wVirtualKeyCode;
			if (vk >= 'A' && vk <= 'Z') return std::string(1, static_cast<char>(vk - 'A' + 1));
			if (vk == VK_SPACE || vk == '2') return std::string(1, '\0');
			if (vk == VK_OEM_MINUS || vk == VK_OEM_2) return "\x1f";

			return {};
		}

	}  // namespace keys_detail

	static bool isPrintableAscii(WCHAR unit) {
		return unit >= 0x20 && unit < 0x7F;
	}

	bool LineEditor::revsearchHandleKey(const KEY_EVENT_RECORD& key,
	                                    std::string& query,
	                                    std::size_t& match_index,
	                                    bool& cancel, bool& submit, bool& redispatch) {
		const bool is_ctrl = keys_detail::ctrlHeld(key) && !keys_detail::typesLayoutCharacter(key);
		const bool is_meta = keys_detail::altHeld(key) && !keys_detail::typesLayoutCharacter(key);
		const WORD vk = key.wVirtualKeyCode;
		const WCHAR ch = key.uChar.UnicodeChar;

		if (vk == VK_RETURN) { submit = true; return false; }
		if (vk == VK_ESCAPE) { cancel = true; return false; }
		if (vk == VK_BACK && !is_ctrl && !is_meta) {
			revsearchEraseQueryChar(query, match_index);
			return true;
		}

		if (keys_detail::isInterruptKey(key)) { cancel = true; interrupted_ = true; return false; }
		if (is_ctrl && vk == 'R') { revsearchStepOlder(query, match_index); return true; }
		if (is_ctrl && vk == 'S') { revsearchStepNewer(query, match_index); return true; }
		if (is_ctrl && vk == 'G') { cancel = true; return false; }

		if (is_ctrl || is_meta || !isPrintableAscii(ch)) {
			redispatch = true;
			return false;
		}

		revsearchExtendQuery(query, static_cast<char>(ch), match_index);
		return true;
	}

	void LineEditor::runReverseSearch() {
		const std::string saved_buffer = buffer_;
		const std::size_t saved_cursor = cursor_;
		const std::string saved_prompt = prompt_raw_;
		const std::size_t saved_visible = prompt_visible_len_;

		revsearch_active_ = true;
		std::string query;
		std::size_t match_index = exec_.history().size();
		revsearchRefresh(query, match_index);

		bool cancel = false;
		bool submit = false;
		bool redispatch = false;
		KEY_EVENT_RECORD key{};
		bool looping = true;
		while (looping) {
			if (!nextKeyDown(key)) {
				cancel = true;
				break;
			}

			looping = revsearchHandleKey(key, query, match_index, cancel, submit, redispatch);
		}

		prompt_raw_ = saved_prompt;
		prompt_visible_len_ = saved_visible;
		if (cancel) {
			buffer_ = saved_buffer;
			cursor_ = saved_cursor;
		}

		last_cursor_row_ = 0;
		revsearch_active_ = false;
		emit(kEraseToEnd);
		emit(prompt_raw_);
		redraw();
		if (interrupted_) interrupt();
		if (submit) acceptLine();
		if (redispatch) dispatchKeyDown(key);
	}

	static DWORD enterRawInputMode(HANDLE input) {
		DWORD saved = 0;
		::GetConsoleMode(input, &saved);

		DWORD raw_mode = saved;
		raw_mode &= ~ENABLE_LINE_INPUT;
		raw_mode &= ~ENABLE_ECHO_INPUT;
		raw_mode &= ~ENABLE_PROCESSED_INPUT;
		raw_mode &= ~ENABLE_MOUSE_INPUT;
		raw_mode &= ~ENABLE_WINDOW_INPUT;
		::SetConsoleMode(input, raw_mode);
		return saved;
	}

	void LineEditor::enterRawMode() {
		if (headless_) return;

		cooked_input_mode_ = enterRawInputMode(::GetStdHandle(STD_INPUT_HANDLE));
	}

	void LineEditor::leaveRawMode() {
		if (headless_) return;

		::SetConsoleMode(::GetStdHandle(STD_INPUT_HANDLE), cooked_input_mode_);
	}

	// Alt-code input (Alt + numpad digits) is delivered as a key-UP event
	// for VK_MENU carrying the resolved Unicode char.
	void LineEditor::handleAltKeyUp(const KEY_EVENT_RECORD& key) {
		if (key.wVirtualKeyCode != VK_MENU) return;
		if (key.uChar.UnicodeChar < 0x20) return;

		const WCHAR unit = key.uChar.UnicodeChar;
		const std::string utf8 = utf16ToUtf8(&unit, 1);
		if (!utf8.empty()) dispatchKeys(utf8);
	}

	// Every key becomes the bytes a terminal would send for it, so the
	// keymap sees Alt-b as ESC b and Up as ESC [ A, as readline does.
	void LineEditor::dispatchKeyDown(const KEY_EVENT_RECORD& key) {
		if (keys_detail::isInterruptKey(key)) {
			interrupt();
			return;
		}

		if (keys_detail::isEmptyAltGr(key)) return;

		const std::string special = keys_detail::specialKey(key);
		if (!special.empty()) {
			dispatchKeys(special);
			return;
		}

		const WCHAR ch = key.uChar.UnicodeChar;
		std::string text;
		if (ch >= 0x80) text = wideCharacter(ch);
		else if (ch != 0) text = std::string(1, static_cast<char>(ch));
		else if (keys_detail::ctrlHeld(key)) text = keys_detail::controlCharacter(key);
		if (text.empty()) return;

		const bool meta = keys_detail::altHeld(key) && !keys_detail::typesLayoutCharacter(key);
		dispatchKeys(meta ? keys_detail::kMetaPrefix + text : text);
	}

	// Key-ups only matter for Alt-code input; every other event is passed
	// over. While replaying, the keys come from the replay queue instead
	// of the console.
	bool LineEditor::nextKeyDown(KEY_EVENT_RECORD& key) {
		for (;;) {
			if (headless_) return takeReplayKey(key);

			INPUT_RECORD record;
			if (!readInputRecord(::GetStdHandle(STD_INPUT_HANDLE), record)) return false;
			if (record.EventType != KEY_EVENT) continue;

			key = record.Event.KeyEvent;
			if (key.bKeyDown) return true;

			handleAltKeyUp(key);
		}
	}

	bool LineEditor::takeReplayKey(KEY_EVENT_RECORD& key) {
		if (replay_keys_.empty()) return false;

		key = replay_keys_.front();
		replay_keys_.pop_front();
		return true;
	}

	bool LineEditor::editUntilDone() {
		while (!done_) {
			KEY_EVENT_RECORD key;
			if (!nextKeyDown(key)) {
				eof_ = true;
				return false;
			}

			dispatchKeyDown(key);
		}

		return true;
	}

	LineStatus LineEditor::readLineRaw(const std::string& prompt, std::string& out) {
		const HANDLE input  = ::GetStdHandle(STD_INPUT_HANDLE);
		const HANDLE output = ::GetStdHandle(STD_OUTPUT_HANDLE);
		if (input == INVALID_HANDLE_VALUE || output == INVALID_HANDLE_VALUE) {
			return readLineCooked(out);
		}

		enterRawMode();
		emit(prompt);
		editUntilDone();
		leaveRawMode();

		out = accepted_;
		return lineStatus();
	}

	namespace replay_detail {

		struct NamedKey {
			const char* name;
			WORD vk;
			WCHAR ch;
		};

		static const NamedKey kNamedKeys[] = {
			{ "LEFT",  VK_LEFT,   0 },
			{ "RIGHT", VK_RIGHT,  0 },
			{ "UP",    VK_UP,     0 },
			{ "DOWN",  VK_DOWN,   0 },
			{ "HOME",  VK_HOME,   0 },
			{ "END",   VK_END,    0 },
			{ "BS",    VK_BACK,   0x08 },
			{ "DEL",   VK_DELETE, 0 },
			{ "TAB",   VK_TAB,    0x09 },
			{ "ENTER", VK_RETURN, 0x0D },
			{ "ESC",   VK_ESCAPE, 0x1B },
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

		static bool namedKey(const std::string& name, DWORD modifiers, KEY_EVENT_RECORD& key) {
			for (const NamedKey& named : kNamedKeys) {
				if (name != named.name) continue;

				key = pressOf(named.vk, named.ch, modifiers);
				return true;
			}

			return false;
		}

		static WORD chordVk(char letter) {
			if (std::isalpha(static_cast<unsigned char>(letter)) != 0) {
				return static_cast<WORD>(std::toupper(static_cast<unsigned char>(letter)));
			}

			if (letter == '_' || letter == '-') return VK_OEM_MINUS;
			if (letter == '/') return VK_OEM_2;

			return 0;
		}

		// What the console puts in uChar for the chord: a control code
		// for Ctrl with a letter, `_` or `@`, nothing for Ctrl+/.
		static WCHAR chordChar(char letter, bool ctrl) {
			if (!ctrl) return static_cast<WCHAR>(static_cast<unsigned char>(letter));
			const auto byte = static_cast<unsigned char>(letter);
			if (std::isalpha(byte) != 0) return static_cast<WCHAR>(std::toupper(byte) - 'A' + 1);
			if (letter == '_') return 0x1F;
			if (letter == '/' || letter == '@' || letter == ' ') return 0;

			return static_cast<WCHAR>(letter);
		}

		static std::wstring widen(const std::string& text) {
			const int count = static_cast<int>(text.size());
			const int length = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), count, nullptr, 0);
			if (length <= 0) return {};

			std::wstring wide(static_cast<std::size_t>(length), L'\0');
			::MultiByteToWideChar(CP_UTF8, 0, text.data(), count, wide.data(), length);
			return wide;
		}

		static DWORD takeModifiers(std::string& name) {
			DWORD modifiers = 0;
			for (;;) {
				if (name.size() > 2 && name.compare(0, 2, "C-") == 0) {
					modifiers |= LEFT_CTRL_PRESSED;
				} else if (name.size() > 2 && name.compare(0, 2, "M-") == 0) {
					modifiers |= LEFT_ALT_PRESSED;
				} else {
					return modifiers;
				}

				name.erase(0, 2);
			}
		}

		// <AltGr-@>: the character as a Czech keyboard types it, with the
		// Ctrl and right Alt bits Windows reports for AltGr.
		static bool altGrKey(const std::string& name, KEY_EVENT_RECORD& key) {
			const std::string prefix = "AltGr-";
			if (name.size() <= prefix.size()) return false;
			if (name.compare(0, prefix.size(), prefix) != 0) return false;

			const std::wstring wide = widen(name.substr(prefix.size()));
			if (wide.size() != 1) return false;

			key = pressOf(0, wide[0], LEFT_CTRL_PRESSED | RIGHT_ALT_PRESSED);
			return true;
		}

		static bool chordKey(std::string name, KEY_EVENT_RECORD& key) {
			const DWORD modifiers = takeModifiers(name);
			if (modifiers == 0) return false;
			if (namedKey(name, modifiers, key)) return true;
			if (name.size() != 1) return false;

			const bool ctrl = (modifiers & LEFT_CTRL_PRESSED) != 0;
			key = pressOf(chordVk(name[0]), chordChar(name[0], ctrl), modifiers);
			return true;
		}

		static bool keyFor(const std::string& token, KEY_EVENT_RECORD& key) {
			if (token.size() < 3 || token.front() != '<' || token.back() != '>') return false;

			const std::string name = token.substr(1, token.size() - 2);
			return namedKey(name, 0, key) || altGrKey(name, key) || chordKey(name, key);
		}

		static void queueText(const std::string& text, std::deque<KEY_EVENT_RECORD>& queue) {
			for (const wchar_t unit : widen(text)) queue.push_back(pressOf(0, unit, 0));
		}

		static void queueToken(const std::string& token, std::deque<KEY_EVENT_RECORD>& queue) {
			KEY_EVENT_RECORD key{};
			if (keyFor(token, key)) {
				queue.push_back(key);
				return;
			}

			queueText(token, queue);
		}

	}  // namespace replay_detail

	std::string LineEditor::replayKeys(const std::vector<std::string>& keys) {
		resetLine("");
		replay_keys_.clear();
		for (const std::string& token : keys) replay_detail::queueToken(token, replay_keys_);

		headless_ = true;
		const bool finished = editUntilDone();
		headless_ = false;

		replay_keys_.clear();
		return describeReplay(finished);
	}

#else  /* !_WIN32 */

	LineStatus LineEditor::readLineRaw(const std::string& prompt, std::string& out) {
		std::fputs(prompt.c_str(), stdout);
		std::fflush(stdout);
		return readLineCooked(out);
	}

	void LineEditor::emit(const std::string&) {}
	void LineEditor::redraw() {}
	void LineEditor::moveToLineEnd() {}
	void LineEditor::appendLineText(std::string&) const {}
	bool LineEditor::editUntilDone() { return false; }
	void LineEditor::leaveRawMode() {}
	void LineEditor::enterRawMode() {}
	LineEditor::Tok LineEditor::currentToken() const { return {0, 0, true}; }
	std::vector<std::string> LineEditor::commandCompletions(const std::string&) { return {}; }
	std::vector<std::string> LineEditor::pathCompletions(const std::string&) { return {}; }
	std::vector<std::string> LineEditor::completionsFor(const std::string&, bool) { return {}; }
	std::string LineEditor::longestCommonPrefix(const std::vector<std::string>&) { return {}; }
	void LineEditor::printMatches(const std::vector<std::string>&) {}
	void LineEditor::applyCompletion(const Tok&, const std::vector<std::string>&) {}
	void LineEditor::insertClipboardText() {}
	void LineEditor::runReverseSearch() {}
	void LineEditor::revsearchRefresh(const std::string&, std::size_t) {}
	void LineEditor::refreshSuggestion() {}
	bool LineEditor::acceptInlineSuggestion() { return false; }
	std::string LineEditor::replayKeys(const std::vector<std::string>&) { return {}; }

#endif /* _WIN32 */

	int lineEditBuiltin(Executor& exec, const std::vector<std::string>& args) {
		LineEditor editor(exec.env(), exec);
		std::printf("%s\n", editor.replayKeys(args).c_str());
		return 0;
	}

}  // namespace wbsh

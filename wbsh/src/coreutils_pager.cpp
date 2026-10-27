/**
 * @file coreutils_pager.cpp
 * @brief less and more: a console pager for when no real `less` is on
 *        PATH (and for `more`, whose Windows namesake is far weaker).
 *
 * The keys come from CONIN$ and the screen goes to CONOUT$, so the pager
 * still works at the end of a pipeline. ANSI colour sequences pass
 * through (less -R behaviour) and count as zero width.
 */

#include "coreutils_internal.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif /* _WIN32 */

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "consolekeys.h"
#include "executor.h"
#include "numparse.h"

namespace wbsh {

#ifdef _WIN32

	namespace pager_detail {

		struct PagerOptions {
			bool line_numbers = false;
			bool chop = false;
			bool quit_if_one_screen = false;
			bool quit_at_eof = false;
			bool keep_screen = false;
			bool ignore_case = false;
			bool start_at_end = false;
			long start_line = 0;
			std::string start_pattern;
			std::vector<std::string> files;
		};

		struct DisplayRow {
			std::size_t line = 0;
			std::size_t begin = 0;
			std::size_t end = 0;
		};

		struct PagerConsole {
			HANDLE in = INVALID_HANDLE_VALUE;
			HANDLE out = INVALID_HANDLE_VALUE;
			DWORD saved_in = 0;
			DWORD saved_out = 0;
			int columns = 80;
			int rows = 24;
		};

		struct Pager {
			PagerOptions options;
			PagerConsole console;
			std::vector<std::string> lines;
			std::vector<DisplayRow> rows;
			std::size_t top = 0;
			std::string pattern;
			bool backward = false;
			std::string message;
			std::string title;
			bool done = false;
		};

	}  // namespace pager_detail

	using pager_detail::DisplayRow;
	using pager_detail::Pager;
	using pager_detail::PagerConsole;
	using pager_detail::PagerOptions;

	static const int kTabWidth = 8;
	static const int kNumberWidth = 8;
	static const int kWheelLines = 3;

	static bool isContinuationByte(unsigned char byte) {
		return (byte & 0xC0) == 0x80;
	}

	// An ANSI CSI sequence (ESC [ ... final byte) occupies no columns.
	static std::size_t escapeLength(const std::string& text, std::size_t at) {
		if (at + 1 >= text.size() || text[at + 1] != '[') return 1;
		std::size_t end = at + 2;
		while (end < text.size() && (text[end] < 0x40 || text[end] > 0x7E)) ++end;
		return end < text.size() ? end - at + 1 : text.size() - at;
	}

	// Appends what raw[at] looks like on screen and returns how many input
	// bytes that took: tabs become spaces, other control bytes ^X, and
	// escape sequences pass through whole.
	static std::size_t appendDisplayed(const std::string& raw, std::size_t at, std::string& out,
	                                   int& column) {
		const unsigned char byte = static_cast<unsigned char>(raw[at]);
		if (byte == 0x1B) {
			const std::size_t length = escapeLength(raw, at);
			out.append(raw, at, length);
			return length;
		}

		if (byte == '\t') {
			const int spaces = kTabWidth - column % kTabWidth;
			out.append(static_cast<std::size_t>(spaces), ' ');
			column += spaces;
			return 1;
		}

		if (byte < 0x20 || byte == 0x7F) {
			out.push_back('^');
			out.push_back(static_cast<char>(byte == 0x7F ? '?' : byte + '@'));
			column += 2;
			return 1;
		}

		out.push_back(raw[at]);
		if (!isContinuationByte(byte)) ++column;
		return 1;
	}

	static std::string expandControls(const std::string& raw) {
		std::string out;
		int column = 0;
		for (std::size_t at = 0; at < raw.size();) at += appendDisplayed(raw, at, out, column);
		return out;
	}

	static void splitIntoLines(const std::string& content, std::vector<std::string>& lines) {
		std::size_t start = 0;
		while (start < content.size()) {
			std::size_t end = content.find('\n', start);
			if (end == std::string::npos) end = content.size();
			std::string line = content.substr(start, end - start);
			if (!line.empty() && line.back() == '\r') line.pop_back();
			lines.push_back(expandControls(line));
			start = end + 1;
		}
	}

	static bool readStreamText(std::FILE* stream, std::string& content) {
		char chunk[65536];
		for (std::size_t got = 0; (got = std::fread(chunk, 1, sizeof(chunk), stream)) != 0;) {
			content.append(chunk, got);
		}

		return std::ferror(stream) == 0;
	}

	static bool loadPagerInput(Executor& exec, const PagerOptions& options, std::string& content) {
		std::vector<std::string> files = options.files;
		if (files.empty()) files.push_back("-");

		bool ok = true;
		for (const std::string& file : files) {
			std::FILE* stream = file == "-" ? stdin : fopenNative(exec, file, "rb");
			if (stream == nullptr) {
				perr("less", file + ": " + std::strerror(errno));
				ok = false;
				continue;
			}

			if (files.size() > 1) content += "::::::::::::::\n" + file + "\n::::::::::::::\n";
			readStreamText(stream, content);
			if (stream != stdin) std::fclose(stream);
		}

		return ok;
	}

	static void layoutRows(Pager& pager) {
		pager.rows.clear();
		const int number_width = pager.options.line_numbers ? kNumberWidth : 0;
		const int width = (std::max)(1, pager.console.columns - number_width);
		for (std::size_t index = 0; index < pager.lines.size(); ++index) {
			const std::string& line = pager.lines[index];
			std::size_t begin = 0;
			int used = 0;
			for (std::size_t at = 0; at < line.size();) {
				const std::size_t step = line[at] == '\x1B' ? escapeLength(line, at) : 1;
				const bool visible = line[at] != '\x1B'
					&& !isContinuationByte(static_cast<unsigned char>(line[at]));
				if (visible && used == width && !pager.options.chop) {
					pager.rows.push_back(DisplayRow{ index, begin, at });
					begin = at;
					used = 0;
				}

				if (visible) ++used;
				at += step;
			}

			pager.rows.push_back(DisplayRow{ index, begin, line.size() });
		}
	}

	static std::size_t pageHeight(const Pager& pager) {
		return static_cast<std::size_t>((std::max)(1, pager.console.rows - 1));
	}

	static std::size_t lastTop(const Pager& pager) {
		const std::size_t height = pageHeight(pager);
		return pager.rows.size() > height ? pager.rows.size() - height : 0;
	}

	static bool atEnd(const Pager& pager) {
		return pager.top >= lastTop(pager);
	}

	static bool sameText(const std::string& haystack, std::size_t at, const std::string& needle,
	                     bool ignore_case) {
		for (std::size_t k = 0; k < needle.size(); ++k) {
			char a = haystack[at + k];
			char b = needle[k];
			if (ignore_case) {
				a = static_cast<char>(std::tolower(static_cast<unsigned char>(a)));
				b = static_cast<char>(std::tolower(static_cast<unsigned char>(b)));
			}

			if (a != b) return false;
		}

		return true;
	}

	static std::size_t findText(const std::string& haystack, const std::string& needle,
	                            std::size_t from, bool ignore_case) {
		if (needle.empty() || haystack.size() < needle.size()) return std::string::npos;
		for (std::size_t at = from; at + needle.size() <= haystack.size(); ++at) {
			if (sameText(haystack, at, needle, ignore_case)) return at;
		}

		return std::string::npos;
	}

	static std::string highlighted(const Pager& pager, const std::string& segment) {
		if (pager.pattern.empty()) return segment;

		std::string out;
		std::size_t at = 0;
		for (;;) {
			const std::size_t hit = findText(segment, pager.pattern, at, pager.options.ignore_case);
			if (hit == std::string::npos) break;
			out.append(segment, at, hit - at);
			out += "\x1b[7m" + segment.substr(hit, pager.pattern.size()) + "\x1b[27m";
			at = hit + pager.pattern.size();
		}

		return out + segment.substr(at);
	}

	static std::string renderRow(const Pager& pager, std::size_t index) {
		if (index >= pager.rows.size()) return "\x1b[1m~\x1b[0m";

		const DisplayRow& row = pager.rows[index];
		std::string text;
		if (pager.options.line_numbers) {
			char number[16];
			const bool first_row = row.begin == 0;
			if (first_row) std::snprintf(number, sizeof(number), "%7zu ", row.line + 1);
			else           std::snprintf(number, sizeof(number), "%8s", "");
			text = number;
		}

		const std::string& line = pager.lines[row.line];
		return text + highlighted(pager, line.substr(row.begin, row.end - row.begin)) + "\x1b[0m";
	}

	static std::string statusText(const Pager& pager) {
		if (!pager.message.empty()) return pager.message;
		if (atEnd(pager)) return "(END)";
		if (pager.top == 0 && !pager.title.empty()) return pager.title;
		return ":";
	}

	static void writeConsole(const PagerConsole& console, const std::string& text) {
		DWORD written = 0;
		::WriteFile(console.out, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
	}

	static void renderPager(Pager& pager) {
		std::string frame = "\x1b[?25l\x1b[H";
		for (std::size_t k = 0; k < pageHeight(pager); ++k) {
			frame += renderRow(pager, pager.top + k);
			frame += "\x1b[K\r\n";
		}

		frame += "\x1b[7m" + statusText(pager) + "\x1b[0m\x1b[K\x1b[?25h";
		writeConsole(pager.console, frame);
	}

	static bool openPagerConsole(PagerConsole& console) {
		console.in = ::CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
		console.out = ::CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
		if (console.in == INVALID_HANDLE_VALUE || console.out == INVALID_HANDLE_VALUE) return false;

		::GetConsoleMode(console.in, &console.saved_in);
		::GetConsoleMode(console.out, &console.saved_out);
		const DWORD in_mode = (console.saved_in & ~static_cast<DWORD>(ENABLE_LINE_INPUT
			| ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_QUICK_EDIT_MODE))
			| ENABLE_MOUSE_INPUT | ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS;
		::SetConsoleMode(console.in, in_mode);
		::SetConsoleMode(console.out, console.saved_out | ENABLE_VIRTUAL_TERMINAL_PROCESSING
			| ENABLE_PROCESSED_OUTPUT);
		return true;
	}

	static void closePagerConsole(PagerConsole& console) {
		if (console.in != INVALID_HANDLE_VALUE) {
			::SetConsoleMode(console.in, console.saved_in);
			::CloseHandle(console.in);
		}

		if (console.out != INVALID_HANDLE_VALUE) {
			::SetConsoleMode(console.out, console.saved_out);
			::CloseHandle(console.out);
		}
	}

	static void refreshSize(Pager& pager) {
		CONSOLE_SCREEN_BUFFER_INFO info{};
		if (::GetConsoleScreenBufferInfo(pager.console.out, &info) == 0) return;

		const int columns = info.srWindow.Right - info.srWindow.Left + 1;
		const int rows = info.srWindow.Bottom - info.srWindow.Top + 1;
		if (columns == pager.console.columns && rows == pager.console.rows && !pager.rows.empty()) {
			return;
		}

		const std::size_t anchor = pager.rows.empty() ? 0 : pager.rows[(std::min)(pager.top,
			pager.rows.size() - 1)].line;
		pager.console.columns = columns;
		pager.console.rows = rows;
		layoutRows(pager);
		pager.top = 0;
		while (pager.top + 1 < pager.rows.size() && pager.rows[pager.top].line < anchor) {
			++pager.top;
		}
	}

	static void scrollBy(Pager& pager, long delta) {
		if (delta > 0 && atEnd(pager) && pager.options.quit_at_eof) {
			pager.done = true;
			return;
		}

		const long target = static_cast<long>(pager.top) + delta;
		const long limit = static_cast<long>(lastTop(pager));
		pager.top = static_cast<std::size_t>((std::max)(0L, (std::min)(target, limit)));
	}

	static bool searchFrom(Pager& pager, bool backward) {
		const std::size_t count = pager.rows.size();
		for (std::size_t step = 1; step <= count; ++step) {
			const std::size_t index = backward ? (pager.top + count - step) % count
				: (pager.top + step) % count;
			const DisplayRow& row = pager.rows[index];
			const std::string& line = pager.lines[row.line];
			const std::string segment = line.substr(row.begin, row.end - row.begin);
			const std::size_t hit = findText(segment, pager.pattern, 0, pager.options.ignore_case);
			if (hit == std::string::npos) {
				continue;
			}

			pager.top = (std::min)(index, lastTop(pager));
			return true;
		}

		pager.message = "Pattern not found";
		return false;
	}

	static bool readKey(const PagerConsole& console, KEY_EVENT_RECORD& key, int& wheel) {
		for (;;) {
			INPUT_RECORD record;
			DWORD read = 0;
			if (::ReadConsoleInputW(console.in, &record, 1, &read) == 0 || read == 0) return false;
			if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
				key = KEY_EVENT_RECORD{};
				return true;
			}

			const MOUSE_EVENT_RECORD& mouse = record.Event.MouseEvent;
			if (record.EventType == MOUSE_EVENT && mouse.dwEventFlags == MOUSE_WHEELED) {
				const short direction = static_cast<short>(HIWORD(mouse.dwButtonState));
				wheel = direction > 0 ? -kWheelLines : kWheelLines;
				key = KEY_EVENT_RECORD{};
				return true;
			}

			if (record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown) {
				key = record.Event.KeyEvent;
				return true;
			}
		}
	}

	static bool promptPattern(Pager& pager, char leader, std::string& entered) {
		entered.clear();
		for (;;) {
			pager.message = std::string(1, leader) + entered;
			renderPager(pager);

			KEY_EVENT_RECORD key{};
			int wheel = 0;
			if (!readKey(pager.console, key, wheel)) return false;
			if (key.wVirtualKeyCode == VK_RETURN) break;
			if (key.wVirtualKeyCode == VK_ESCAPE) {
				pager.message.clear();
				return false;
			}

			if (key.wVirtualKeyCode == VK_BACK) {
				if (entered.empty()) break;
				while (!entered.empty()
				       && isContinuationByte(static_cast<unsigned char>(entered.back()))) {
					entered.pop_back();
				}

				if (!entered.empty()) entered.pop_back();
				continue;
			}

			if (key.uChar.UnicodeChar >= 0x20) {
				entered += readConsoleCharacter(pager.console.in, key.uChar.UnicodeChar);
			}
		}

		pager.message.clear();
		return true;
	}

	static void startSearch(Pager& pager, bool backward) {
		std::string entered;
		if (!promptPattern(pager, backward ? '?' : '/', entered)) return;
		if (!entered.empty()) pager.pattern = entered;
		pager.backward = backward;
		if (!pager.pattern.empty()) searchFrom(pager, backward);
	}

	static bool handleMotionKey(Pager& pager, WORD virtual_key, wchar_t ch) {
		const long page = static_cast<long>(pageHeight(pager));
		const bool page_down = ch == ' ' || ch == 'f' || ch == 'z' || virtual_key == VK_NEXT;
		const bool page_up = ch == 'b' || ch == 'w' || virtual_key == VK_PRIOR;
		const bool line_down = ch == 'j' || ch == 'e' || ch == '\r' || virtual_key == VK_DOWN;
		const bool line_up = ch == 'k' || ch == 'y' || virtual_key == VK_UP;
		const bool to_top = ch == 'g' || ch == '<' || virtual_key == VK_HOME;
		const bool to_end = ch == 'G' || ch == '>' || virtual_key == VK_END;
		if (page_down)      scrollBy(pager, page);
		else if (page_up)   scrollBy(pager, -page);
		else if (line_down) scrollBy(pager, 1);
		else if (line_up)   scrollBy(pager, -1);
		else if (ch == 'd') scrollBy(pager, page / 2);
		else if (ch == 'u') scrollBy(pager, -page / 2);
		else if (to_top)    pager.top = 0;
		else if (to_end)    pager.top = lastTop(pager);
		else return false;
		return true;
	}

	static bool controlKeyMotion(Pager& pager, const KEY_EVENT_RECORD& key) {
		const bool ctrl = (key.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
		if (!ctrl) return false;

		const long page = static_cast<long>(pageHeight(pager));
		switch (key.wVirtualKeyCode) {
		case 'C': pager.done = true;           return true;
		case 'F':
		case 'V': scrollBy(pager, page);       return true;
		case 'B': scrollBy(pager, -page);      return true;
		case 'D': scrollBy(pager, page / 2);   return true;
		case 'U': scrollBy(pager, -page / 2);  return true;
		case 'N':
		case 'E': scrollBy(pager, 1);          return true;
		case 'P':
		case 'Y':
		case 'K': scrollBy(pager, -1);         return true;
		case 'L':
		case 'R': return true;
		default:  return false;
		}
	}

	static void handlePagerKey(Pager& pager, const KEY_EVENT_RECORD& key) {
		pager.message.clear();
		if (controlKeyMotion(pager, key)) return;

		const wchar_t ch = key.uChar.UnicodeChar;
		if (handleMotionKey(pager, key.wVirtualKeyCode, ch)) return;

		switch (ch) {
		case 'q':
		case 'Q': pager.done = true;                                    break;
		case '/': startSearch(pager, false);                             break;
		case '?': startSearch(pager, true);                              break;
		case 'n': if (!pager.pattern.empty()) searchFrom(pager, pager.backward);  break;
		case 'N': if (!pager.pattern.empty()) searchFrom(pager, !pager.backward); break;
		case 'h':
		case 'H':
			pager.message = "q quit  space/b page  j/k line  d/u half  g/G ends  / ? n N search";
			break;
		default:
			break;
		}
	}

	static void applyStartPosition(Pager& pager) {
		if (pager.options.start_at_end) pager.top = lastTop(pager);
		if (pager.options.start_line > 0) {
			const std::size_t wanted = static_cast<std::size_t>(pager.options.start_line - 1);
			while (pager.top < lastTop(pager) && pager.rows[pager.top].line < wanted) ++pager.top;
		}

		if (!pager.options.start_pattern.empty()) {
			pager.pattern = pager.options.start_pattern;
			searchFrom(pager, false);
		}
	}

	static bool isKeyPress(const KEY_EVENT_RECORD& key) {
		return key.wVirtualKeyCode != 0 || key.uChar.UnicodeChar != 0;
	}

	static void runPagerLoop(Pager& pager) {
		if (!pager.options.keep_screen) writeConsole(pager.console, "\x1b[?1049h");
		refreshSize(pager);
		applyStartPosition(pager);
		while (!pager.done) {
			refreshSize(pager);
			renderPager(pager);

			KEY_EVENT_RECORD key{};
			int wheel = 0;
			if (!readKey(pager.console, key, wheel)) break;
			if (wheel != 0) scrollBy(pager, wheel);
			else if (isKeyPress(key)) handlePagerKey(pager, key);
		}

		writeConsole(pager.console, pager.options.keep_screen ? "\r\n" : "\x1b[?1049l");
	}

	static bool applyPagerFlag(char flag, PagerOptions& options) {
		switch (flag) {
		case 'N': options.line_numbers = true;       return true;
		case 'S': options.chop = true;               return true;
		case 'F': options.quit_if_one_screen = true; return true;
		case 'E': options.quit_at_eof = true;        return true;
		case 'X': options.keep_screen = true;        return true;
		case 'i':
		case 'I': options.ignore_case = true;        return true;
		case 'R':
		case 'r':
		case 'M':
		case 'm':
		case 'K':
		case 'n':
		case 'e':
			return true;
		default:
			return false;
		}
	}

	static bool applyStartCommand(const std::string& command, PagerOptions& options) {
		if (command == "G") {
			options.start_at_end = true;
			return true;
		}

		if (!command.empty() && command[0] == '/') {
			options.start_pattern = command.substr(1);
			return true;
		}

		long long line = 0;
		if (!parseLL(command, line)) return false;
		options.start_line = static_cast<long>(line);
		return true;
	}

	static bool parsePagerArgs(const std::vector<std::string>& args, PagerOptions& options) {
		for (const std::string& arg : args) {
			if (arg.size() > 1 && arg[0] == '+') {
				if (applyStartCommand(arg.substr(1), options)) continue;
				return false;
			}

			if (arg.size() < 2 || arg[0] != '-') {
				options.files.push_back(arg);
				continue;
			}

			for (std::size_t k = 1; k < arg.size(); ++k) {
				if (!applyPagerFlag(arg[k], options)) return false;
			}
		}

		return true;
	}

	static void writeThrough(const std::string& content) {
		std::fwrite(content.data(), 1, content.size(), stdout);
		std::fflush(stdout);
	}

	static bool fitsOnOneScreen(Pager& pager) {
		pager.console.columns = 80;
		pager.console.rows = 24;
		if (!queryConsoleSize(pager.console.columns, pager.console.rows)) return false;
		layoutRows(pager);
		return pager.rows.size() <= pageHeight(pager);
	}

	static int runPager(Executor& exec, const std::vector<std::string>& args,
	                    PagerOptions options) {
		if (!parsePagerArgs(args, options)) {
			perr("less", "usage: less [-EFNSXi] [+NUM | +G | +/PATTERN] [FILE]...");
			return 1;
		}

		if (options.files.empty() && isConsoleStream(stdin)) {
			perr("less", "missing filename (\"less --help\" for help)");
			return 1;
		}

		std::string content;
		const bool loaded = loadPagerInput(exec, options, content);
		if (!isConsoleStream(stdout)) {
			writeThrough(content);
			return loaded ? 0 : 1;
		}

		Pager pager;
		pager.options = options;
		pager.title = options.files.size() == 1 ? options.files[0] : "";
		splitIntoLines(content, pager.lines);
		if (options.quit_if_one_screen && fitsOnOneScreen(pager)) {
			writeThrough(content);
			return loaded ? 0 : 1;
		}

		if (!openPagerConsole(pager.console)) {
			writeThrough(content);
			return loaded ? 0 : 1;
		}

		runPagerLoop(pager);
		closePagerConsole(pager.console);
		return loaded ? 0 : 1;
	}

	static bool preferExternalLess(Executor& exec) {
		if (exec.env().get("WBSH_PAGER") == "builtin" || !isConsoleStream(stdout)) return false;
		return !exec.findExecutable("less").empty();
	}

	static int builtin_less(Executor& exec, const std::vector<std::string>& args) {
		if (preferExternalLess(exec)) {
			std::vector<std::string> argv = { "less" };
			argv.insert(argv.end(), args.begin(), args.end());
			return exec.runExternal(argv, {});
		}

		return runPager(exec, args, PagerOptions());
	}

	static int builtin_more(Executor& exec, const std::vector<std::string>& args) {
		PagerOptions options;
		options.quit_if_one_screen = true;
		options.quit_at_eof = true;
		options.keep_screen = true;
		return runPager(exec, args, options);
	}

	void registerPagerBuiltins(Executor& exec) {
		exec.registerBuiltin("less", builtin_less);
		exec.registerBuiltin("more", builtin_more);
	}

#else

	void registerPagerBuiltins(Executor&) {}

#endif /* _WIN32 */

}  // namespace wbsh

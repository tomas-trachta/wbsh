#pragma once

/**
 * @file vi_internal.h
 * @brief The editor state and the entry points shared by the vi translation units.
 *
 * Text is a vector of UTF-8 lines without their line ends. Every change
 * to it goes through replaceLines(), which records undo, moves marks and
 * keeps the :g line flags in step. Keys are Unicode code points, with
 * the cursor and editing keys past the Unicode range.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "posixregex.h"

namespace wbsh {

	class Executor;

	namespace vi_detail {

		using Key = char32_t;

		inline constexpr Key kKeyEscape = 0x1B;
		inline constexpr Key kKeyEnter = '\r';
		inline constexpr Key kKeyBackspace = 0x08;
		inline constexpr Key kKeyTab = '\t';
		inline constexpr Key kKeyUp = 0x110001;
		inline constexpr Key kKeyDown = 0x110002;
		inline constexpr Key kKeyLeft = 0x110003;
		inline constexpr Key kKeyRight = 0x110004;
		inline constexpr Key kKeyHome = 0x110005;
		inline constexpr Key kKeyEnd = 0x110006;
		inline constexpr Key kKeyPageUp = 0x110007;
		inline constexpr Key kKeyPageDown = 0x110008;
		inline constexpr Key kKeyInsert = 0x110009;
		inline constexpr Key kKeyDelete = 0x11000A;
		/// A byte that is not UTF-8 arrives as this plus the byte.
		inline constexpr Key kKeyRawByte = 0x110100;

		constexpr Key ctrlKey(char letter) {
			return static_cast<Key>(letter & 0x1F);
		}

		inline constexpr std::size_t kNoMark = static_cast<std::size_t>(-1);

		struct Position {
			std::size_t row = 0;
			std::size_t col = 0;
		};

		enum class Mode { Normal, Insert, Replace, CommandLine };

		struct Options {
			std::size_t tabstop = 8;
			std::size_t shiftwidth = 8;
			bool autoindent = false;
			bool ignorecase = false;
			bool number = false;
			bool list = false;
			bool expandtab = false;
			bool wrapscan = true;
			bool readonly = false;
		};

		struct Register {
			std::string text;
			bool linewise = false;
		};

		struct UndoChange {
			std::size_t first = 0;
			std::vector<std::string> removed;
			std::vector<std::string> inserted;
		};

		struct UndoGroup {
			std::vector<UndoChange> changes;
			Position cursor_before;
			std::uint64_t state = 0;
			std::uint64_t previous_state = 0;
			bool no_lines_before = false;
		};

		struct Terminal {
			bool headless = true;
			void* in = nullptr;
			void* out = nullptr;
			unsigned long saved_in = 0;
			unsigned long saved_out = 0;
			bool screen_active = false;
			std::size_t columns = 80;
			std::size_t rows = 24;
			int lookahead = -1;
		};

		struct Substitution {
			std::string pattern;
			std::string replacement;
			bool global = false;
			bool valid = false;
		};

		struct Editor {
			Executor* exec = nullptr;
			Terminal term;
			Options options;
			Mode mode = Mode::Normal;

			std::vector<std::string> lines = { std::string() };
			bool no_lines = true;
			Position cursor;
			std::size_t want_column = 0;
			bool want_end = false;
			std::size_t top = 0;
			std::size_t top_skip = 0;

			std::vector<std::string> files;
			std::size_t file_index = 0;
			std::string filename;
			bool crlf = false;
			bool quit_warned = false;
			bool quit_warning_active = false;
			bool quit = false;
			int status = 0;

			std::string message;
			bool message_error = false;
			std::string command_line;
			bool command_active = false;

			std::array<Position, 26> marks{};
			std::array<bool, 26> mark_set{};
			Position context;
			bool context_set = false;
			std::array<Register, 128> registers{};

			std::vector<UndoGroup> undo;
			std::vector<UndoGroup> redo;
			UndoGroup group;
			bool group_open = false;
			bool replaying_undo = false;
			std::uint64_t state = 0;
			std::uint64_t saved_state = 0;
			std::uint64_t next_state = 0;

			std::vector<char> global_flags;
			bool in_global = false;

			std::deque<Key> injected;
			std::deque<Key> pending;
			bool input_ended = false;
			std::vector<Key> recording;
			bool record_on = false;
			std::vector<Key> dot_keys;
			bool text_changed = false;

			std::string last_search;
			bool search_backward = false;
			Key find_char = 0;
			char find_kind = 0;
			Substitution substitution;
			std::vector<std::string> history;
			std::string last_shell_command;
		};

		/* vi.cpp */
		bool nextKey(Editor& ed, Key& key);
		void setMessage(Editor& ed, const std::string& text);
		void setError(Editor& ed, const std::string& text);
		bool isModified(const Editor& ed);
		void appendMessageLine(Editor& ed, const std::string& text);
		void showLongMessage(Editor& ed);
		void runShell(Editor& ed, const std::string& command);
		std::string makeTempPath(Executor& exec);

		/* vi_terminal.cpp */
		bool openTerminal(Editor& ed);
		void closeTerminal(Editor& ed);
		void suspendTerminal(Editor& ed);
		void resumeTerminal(Editor& ed);
		bool readTerminalKey(Editor& ed, Key& key);
		bool terminalInputQueued(const Editor& ed);
		void refreshTerminalSize(Editor& ed);
		void writeTerminal(const Editor& ed, const std::string& text);
		void waitForKeyPress(Editor& ed);

		/* vi_render.cpp */
		std::size_t textRows(const Editor& ed);
		std::size_t displayColumn(const Editor& ed, std::size_t row, std::size_t col);
		std::size_t byteAtColumn(const Editor& ed, std::size_t row, std::size_t column);
		void scrollToCursor(Editor& ed);
		std::size_t lastVisibleLine(const Editor& ed);
		void renderScreen(Editor& ed);

		/* vi_buffer.cpp */
		void replaceLines(Editor& ed, std::size_t first, std::size_t count,
		                  const std::vector<std::string>& inserted);
		void setLine(Editor& ed, std::size_t row, const std::string& text);
		void insertLineTexts(Editor& ed, std::size_t at, const std::vector<std::string>& texts);
		void deleteLineRange(Editor& ed, std::size_t first, std::size_t count);
		void replaceBuffer(Editor& ed, std::vector<std::string> lines);
		std::string textBetween(const Editor& ed, Position start, Position end);
		void deleteBetween(Editor& ed, Position start, Position end);
		Position insertText(Editor& ed, Position at, const std::string& text);
		void beginUndoGroup(Editor& ed);
		void endUndoGroup(Editor& ed);
		bool undoChange(Editor& ed);
		bool redoChange(Editor& ed);
		void clearHistory(Editor& ed);
		void storeRegister(Editor& ed, char name, const Register& value, bool is_delete);
		bool fetchRegister(const Editor& ed, char name, Register& value);
		bool setMark(Editor& ed, Key name, Position at);
		bool markPosition(const Editor& ed, Key name, Position& at);
		void rememberContext(Editor& ed);

		/* vi_text.cpp */
		std::size_t nextCharStart(const std::string& line, std::size_t pos);
		std::size_t previousCharStart(const std::string& line, std::size_t pos);
		std::size_t lastCharStart(const std::string& line);
		std::size_t firstNonBlank(const std::string& line);
		bool isBlankChar(const std::string& line, std::size_t pos);
		int charClass(const std::string& line, std::size_t pos, bool bigword);
		std::string leadingIndent(const std::string& line);
		std::string indentOfWidth(const Editor& ed, std::size_t width);
		std::size_t indentWidth(const Editor& ed, const std::string& line);
		std::string keyText(Key key);
		bool isRawByteKey(Key key);
		void clampCursor(Editor& ed);
		void moveToFirstNonBlank(Editor& ed, std::size_t row);
		void updateWantColumn(Editor& ed);

		/* vi_motion.cpp */
		struct Motion {
			Position target;
			bool linewise = false;
			bool inclusive = false;
			bool jump = false;
			bool keep_column = false;
			bool to_end = false;
		};

		struct MotionRequest {
			Key key = 0;
			long count = 1;
			bool has_count = false;
			bool for_operator = false;
		};

		bool isMotionKey(Key key);
		bool resolveMotion(Editor& ed, const MotionRequest& request, Motion& motion);
		void applyMotion(Editor& ed, const Motion& motion);
		bool findPattern(Editor& ed, const std::string& pattern, bool backward, Position from,
		                 Position& found);
		bool compileSearch(Editor& ed, const std::string& pattern, PosixRegex& regex);
		bool changeWordMotion(const Editor& ed, long count, bool big, Motion& motion);

		/* vi_normal.cpp */
		void runNormalCommand(Editor& ed);
		void joinRows(Editor& ed, std::size_t row, std::size_t count, bool add_spaces);
		void shiftRows(Editor& ed, std::size_t first, std::size_t last, bool right,
		               std::size_t times);
		std::vector<std::string> registerLines(const std::string& text);

		/* vi_insert.cpp */
		enum class InsertKind {
			Insert,
			Append,
			InsertLine,
			AppendLine,
			OpenBelow,
			OpenAbove,
			Change,
			Replace,
		};
		void runInsertMode(Editor& ed, InsertKind kind, long count);
		void openLine(Editor& ed, bool below);

		/// Ex line numbers are 1-based; 0 stands before the first line.
		struct ExRange {
			std::size_t first = 0;
			std::size_t last = 0;
			std::size_t count = 0;
		};

		struct ExCommand {
			ExRange range;
			bool bang = false;
			std::string name;
			std::string args;
		};

		/* vi_ex.cpp */
		bool readCommandLine(Editor& ed, Key leader, std::string& text);
		void runExCommand(Editor& ed, const std::string& text);
		bool executeExLine(Editor& ed, const std::string& text);
		bool parseExAddress(Editor& ed, const std::string& text, std::size_t& pos,
		                    std::size_t& line, bool& have);

		/* vi_ex_files.cpp */
		bool exWrite(Editor& ed, const ExCommand& cmd);
		bool exWriteQuit(Editor& ed, const ExCommand& cmd);
		bool exExit(Editor& ed, const ExCommand& cmd);
		bool exQuit(Editor& ed, const ExCommand& cmd);
		bool exQuitAll(Editor& ed, const ExCommand& cmd);
		bool exEdit(Editor& ed, const ExCommand& cmd);
		bool exRead(Editor& ed, const ExCommand& cmd);
		bool exBang(Editor& ed, const ExCommand& cmd);
		bool exNext(Editor& ed, const ExCommand& cmd);
		bool exWriteNext(Editor& ed, const ExCommand& cmd);
		bool exPrevious(Editor& ed, const ExCommand& cmd);
		bool exRewind(Editor& ed, const ExCommand& cmd);
		bool exArgs(Editor& ed, const ExCommand& cmd);
		bool exFile(Editor& ed, const ExCommand& cmd);

		/* vi_options.cpp */
		bool exSet(Editor& ed, const ExCommand& cmd);

		/* vi_substitute.cpp */

		bool substituteCommand(Editor& ed, const ExRange& range, const std::string& args);
		bool globalCommand(Editor& ed, const ExRange& range, const std::string& args, bool invert);

		/* vi_file.cpp */
		bool loadFile(Editor& ed, const std::string& name);
		bool writeFile(Editor& ed, const std::string& name, const ExRange& range, bool append,
		               bool force);
		bool readFileText(Editor& ed, const std::string& name, std::string& text);
		void showFileInfo(Editor& ed);
		std::vector<std::string> splitLines(const std::string& text, bool& crlf);

	}  // namespace vi_detail

}  // namespace wbsh

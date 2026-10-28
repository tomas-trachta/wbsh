#pragma once

/**
 * @file ed_internal.h
 * @brief The editing state and the entry points shared by the ed translation units.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "posixregex.h"

namespace wbsh {

	class Executor;

	namespace ed_detail {

		inline constexpr int kPrintPlain = 1;
		inline constexpr int kPrintList = 2;
		inline constexpr int kPrintNumber = 4;

		struct EdLine {
			std::string text;
			std::uint64_t id = 0;
			bool global = false;
		};

		struct Snapshot {
			std::vector<EdLine> lines;
			std::size_t current = 0;
			bool modified = false;
		};

		struct Substitution {
			PosixRegex regex;
			std::string replacement;
			bool global = false;
			int print_flags = 0;
			long occurrence = 1;
		};

		/// One command line being parsed; a command that continues over
		/// several input lines has them joined by '\n'.
		struct CommandText {
			std::string text;
			std::size_t pos = 0;

			char peek() const { return pos < text.size() ? text[pos] : '\0'; }
			char take() { return pos < text.size() ? text[pos++] : '\0'; }
			bool atEnd() const { return pos >= text.size(); }
			std::string rest() const {
				return pos < text.size() ? text.substr(pos) : std::string();
			}
		};

		struct Range {
			std::size_t count = 0;
			std::size_t first = 0;
			std::size_t second = 0;
		};

		struct EdState {
			Executor* exec = nullptr;
			std::vector<EdLine> lines;
			std::size_t current = 0;
			std::uint64_t next_id = 1;
			std::array<std::uint64_t, 26> marks{};

			std::string filename;
			bool crlf = false;
			bool modified = false;
			bool changed = false;
			bool warned = false;
			bool warning_active = false;

			bool silent = false;
			bool loose = false;
			bool verbose = false;
			bool prompt_on = false;
			std::string prompt = "*";

			std::string error;
			int status = 0;
			bool quit = false;
			bool input_ended = false;

			PosixRegex regex;
			bool has_regex = false;
			Substitution substitution;
			bool has_substitution = false;
			std::string shell_command;

			Snapshot undo;
			bool has_undo = false;

			bool in_global = false;
			std::vector<std::string> global_input;
			std::size_t global_next = 0;
		};

		std::size_t lastLine(const EdState& st);

		bool fail(EdState& st, const std::string& message);

		/// The next line of input: from the running global command list
		/// when there is one, otherwise from stdin.
		bool readInputLine(EdState& st, std::string& line);
		bool readUserLine(EdState& st, std::string& line);
		void skipBlanks(CommandText& cmd);
		void printByteCount(const EdState& st, std::size_t bytes);
		bool editFile(EdState& st, const std::string& name);

		bool executeCommand(EdState& st, CommandText& cmd);

		void insertLines(EdState& st, std::size_t after, const std::vector<std::string>& texts);
		void deleteLines(EdState& st, std::size_t first, std::size_t last);
		void replaceLine(EdState& st, std::size_t line, const std::string& text);
		void moveLines(EdState& st, std::size_t first, std::size_t last, std::size_t after);

		Snapshot takeSnapshot(const EdState& st);
		void restoreSnapshot(EdState& st, const Snapshot& snapshot);

		bool lineOfMark(const EdState& st, char mark, std::size_t& line);
		bool setMark(EdState& st, char mark, std::size_t line);

		std::vector<std::string> splitText(const std::string& text, bool& crlf, bool& complete);
		bool readFileText(EdState& st, const std::string& name, std::string& text);
		bool writeLines(EdState& st, const std::string& name, const Range& range, bool append);
		bool readSource(EdState& st, const std::string& name, std::string& text);
		bool writeToCommand(EdState& st, const std::string& command, const Range& range);
		bool runShellCommand(EdState& st, const std::string& command);
		bool expandShellCommand(EdState& st, const std::string& raw, std::string& command);

		void printLine(const EdState& st, std::size_t line, int flags);

		bool parsePattern(CommandText& cmd, char delimiter, std::string& pattern);
		bool compilePattern(EdState& st, const std::string& pattern);
		bool lineMatches(const EdState& st, std::size_t line);
		bool parseRange(EdState& st, CommandText& cmd, Range& range);
		bool parseAddress(EdState& st, CommandText& cmd, std::size_t& address, bool& have);

		bool substituteCommand(EdState& st, CommandText& cmd, const Range& range);
		bool globalCommand(EdState& st, CommandText& cmd, const Range& range, char kind);

		bool parsePrintSuffix(CommandText& cmd, int& flags);
		bool finishCommand(EdState& st, CommandText& cmd);

	}  // namespace ed_detail

}  // namespace wbsh

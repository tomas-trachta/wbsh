/**
 * @file coreutils_sed.cpp
 * @brief sed: the POSIX command set with the usual GNU extensions.
 *
 * Commands: { } = a b c d D g G h H i l n N p P q Q r t T w x y z : #,
 * and s with g, p, N, I and w flags. Addresses: N, $, /re/ (with I),
 * \cREc, first~step, addr1,addr2, addr1,+N, addr1,~N, 0,/re/ and !.
 * Options: -n -e -f -E/-r -s -i[SUFFIX] -z. Patterns go through
 * posixregex.h, so \n, \t, \w, \< \> and back-references work in both
 * basic and extended mode; replacements understand & \1-\9 \n \t and
 * the GNU case conversions \U \L \u \l \E.
 *
 * -i never truncates the original: the result goes to a temporary file in
 * the same directory, which is renamed over the target only once every
 * write succeeded (the original is renamed to the backup name first).
 */

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif /* _WIN32 */

#include "coreutils_internal.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <regex>
#include <string>
#include <system_error>
#include <vector>

#include "executor.h"
#include "numparse.h"
#include "pathconv.h"
#include "posixregex.h"
#include "regexutil.h"

namespace wbsh {

	namespace fs = std::filesystem;

	namespace sed_detail {

		enum class AddressKind {
			None,
			Line,
			Last,
			Regex,
			Step,
			Zero,
		};

		struct Address {
			AddressKind kind = AddressKind::None;
			long line = 0;
			long step = 0;
			int regex = -1;
		};

		enum class RangeEnd {
			Address,
			Relative,
			Multiple,
		};

		enum class PieceKind {
			Literal,
			Whole,
			Group,
			UpperAll,
			LowerAll,
			UpperNext,
			LowerNext,
			CaseEnd,
		};

		struct ReplacementPiece {
			PieceKind kind = PieceKind::Literal;
			std::string text;
			int group = 0;
		};

		struct Substitution {
			int regex = -1;
			std::vector<ReplacementPiece> replacement;
			bool global = false;
			long nth = 1;
			bool print = false;
			std::string write_file;
		};

		struct Command {
			Address first;
			Address second;
			RangeEnd end_kind = RangeEnd::Address;
			long end_count = 0;
			bool negate = false;
			char name = 0;
			std::string text;
			std::size_t target = std::string::npos;
			int exit_code = 0;
			Substitution subst;
			std::vector<unsigned char> table;
			bool range_active = false;
			long range_end = 0;
		};

		struct Program {
			std::vector<Command> commands;
			std::vector<std::regex> regexes;
			bool quiet = false;
		};

		struct SedOptions {
			bool quiet = false;
			bool extended = false;
			bool separate = false;
			bool in_place = false;
			bool null_data = false;
			std::string backup_suffix;
			std::string script;
			bool have_script = false;
			std::vector<std::string> files;
		};

		struct ScriptParser {
			const std::string& text;
			Program& program;
			bool extended = false;
			std::size_t at = 0;
			std::vector<std::size_t> open_blocks;
			std::string error;
		};

		struct LineSource {
			Executor* exec = nullptr;
			std::vector<std::string> files;
			std::size_t next_file = 0;
			std::FILE* current = nullptr;
			char delimiter = '\n';
			bool dash_is_stdin = true;
			bool has_ahead = false;
			std::string ahead;
			bool ahead_terminated = true;
			long line_number = 0;
			bool failed = false;
		};

		struct Output {
			std::FILE* stream = stdout;
			std::string* buffer = nullptr;
			char delimiter = '\n';
			bool missing_newline = false;
		};

		enum class Flow {
			Next,
			Jump,
			EndCycle,
			EndCycleQuiet,
			Restart,
		};

		struct RunState {
			std::string pattern;
			std::string hold;
			bool terminated = true;
			bool hold_terminated = true;
			bool substituted = false;
			bool quit = false;
			int exit_code = 0;
			const std::regex* last_regex = nullptr;
			std::vector<std::string> appends;
			std::map<std::string, std::FILE*> write_files;
			Output out;
		};

	}  // namespace sed_detail

	using sed_detail::Address;
	using sed_detail::AddressKind;
	using sed_detail::Command;
	using sed_detail::Flow;
	using sed_detail::LineSource;
	using sed_detail::Output;
	using sed_detail::PieceKind;
	using sed_detail::Program;
	using sed_detail::RangeEnd;
	using sed_detail::ReplacementPiece;
	using sed_detail::RunState;
	using sed_detail::ScriptParser;
	using sed_detail::SedOptions;
	using sed_detail::Substitution;

	static const int kSedSyntaxStatus = 1;
	static const int kSedInputStatus = 2;
	static const int kSedPanicStatus = 4;
	static const std::size_t kListWidth = 70;

	static bool atEnd(const ScriptParser& p) {
		return p.at >= p.text.size();
	}

	static char peek(const ScriptParser& p) {
		return atEnd(p) ? '\0' : p.text[p.at];
	}

	static void skipBlanks(ScriptParser& p) {
		while (!atEnd(p) && (peek(p) == ' ' || peek(p) == '\t')) ++p.at;
	}

	static void skipSeparators(ScriptParser& p) {
		while (!atEnd(p) && std::strchr(" \t\n;", peek(p)) != nullptr) ++p.at;
	}

	static bool fail(ScriptParser& p, const std::string& message) {
		if (p.error.empty()) p.error = "char " + std::to_string(p.at + 1) + ": " + message;
		return false;
	}

	static bool parseNumber(ScriptParser& p, long& value) {
		const std::size_t start = p.at;
		while (!atEnd(p) && std::isdigit(static_cast<unsigned char>(peek(p))) != 0) ++p.at;
		if (p.at == start) return false;

		long long parsed = 0;
		parseLL(p.text.substr(start, p.at - start), parsed);
		value = static_cast<long>(parsed);
		return true;
	}

	// Reads up to an unescaped delimiter. "\DELIM" stands for the delimiter
	// itself; "\n" stays an escape for the regex translator.
	static bool readDelimited(ScriptParser& p, char delimiter, std::string& out) {
		while (!atEnd(p) && peek(p) != delimiter) {
			const char c = p.text[p.at++];
			if (c == '\n' && delimiter != '\n') return fail(p, "unterminated address regex");
			if (c != '\\' || atEnd(p)) {
				out.push_back(c);
				continue;
			}

			const char next = p.text[p.at++];
			if (next == delimiter && delimiter != 'n') {
				out.push_back(next);
				continue;
			}

			out.push_back('\\');
			out.push_back(next);
		}

		if (atEnd(p)) return fail(p, "unterminated `s' command");
		++p.at;
		return true;
	}

	static bool addRegex(ScriptParser& p, const std::string& pattern, bool ignore_case,
	                     int& index) {
		if (pattern.empty()) {
			index = -1;
			return true;
		}

		std::regex compiled;
		if (!compilePosixRegex(compiled, pattern, p.extended, ignore_case)) {
			return fail(p, "invalid regular expression: " + pattern);
		}

		p.program.regexes.push_back(compiled);
		index = static_cast<int>(p.program.regexes.size()) - 1;
		return true;
	}

	static bool parseRegexAddress(ScriptParser& p, Address& address) {
		char delimiter = peek(p);
		++p.at;
		if (delimiter == '\\') {
			if (atEnd(p)) return fail(p, "unexpected end of script");
			delimiter = p.text[p.at++];
		}

		std::string pattern;
		if (!readDelimited(p, delimiter, pattern)) return false;

		bool ignore_case = false;
		while (peek(p) == 'I' || peek(p) == 'M') {
			ignore_case = ignore_case || peek(p) == 'I';
			++p.at;
		}

		address.kind = AddressKind::Regex;
		return addRegex(p, pattern, ignore_case, address.regex);
	}

	static bool parseAddress(ScriptParser& p, Address& address) {
		const char c = peek(p);
		if (c == '$') {
			++p.at;
			address.kind = AddressKind::Last;
			return true;
		}

		if (c == '/' || c == '\\') return parseRegexAddress(p, address);
		if (!parseNumber(p, address.line)) return true;

		address.kind = address.line == 0 ? AddressKind::Zero : AddressKind::Line;
		if (peek(p) != '~') return true;

		++p.at;
		address.kind = AddressKind::Step;
		if (!parseNumber(p, address.step)) address.step = 0;
		return true;
	}

	static bool parseRangeEnd(ScriptParser& p, Command& command) {
		skipBlanks(p);
		if (peek(p) == '+' || peek(p) == '~') {
			command.end_kind = peek(p) == '+' ? RangeEnd::Relative : RangeEnd::Multiple;
			++p.at;
			if (!parseNumber(p, command.end_count)) return fail(p, "expected a number");
			command.second.kind = AddressKind::Line;
			return true;
		}

		if (!parseAddress(p, command.second)) return false;
		if (command.second.kind == AddressKind::None) return fail(p, "unexpected `,'");
		return true;
	}

	static bool parseAddresses(ScriptParser& p, Command& command) {
		if (!parseAddress(p, command.first)) return false;
		if (command.first.kind == AddressKind::None) return true;

		skipBlanks(p);
		if (peek(p) == ',') {
			++p.at;
			if (!parseRangeEnd(p, command)) return false;
		} else if (command.first.kind == AddressKind::Zero) {
			return fail(p, "invalid usage of line address 0");
		}

		return true;
	}

	static void appendTextChar(ScriptParser& p, std::string& text) {
		const char c = p.text[p.at++];
		if (c != '\\' || atEnd(p)) {
			text.push_back(c);
			return;
		}

		text.push_back(p.text[p.at++]);
	}

	// a, i, c: "a\" + newline + text (POSIX), or "a text" (GNU one-liner).
	// A backslash before a newline continues the text on the next line.
	static void parseText(ScriptParser& p, std::string& text) {
		skipBlanks(p);
		if (peek(p) == '\\') {
			++p.at;
			if (peek(p) == '\n') ++p.at;
		}

		while (!atEnd(p) && peek(p) != '\n') appendTextChar(p, text);
		if (!atEnd(p)) ++p.at;
	}

	static std::string restOfLine(ScriptParser& p, const char* stops) {
		skipBlanks(p);
		const std::size_t start = p.at;
		while (!atEnd(p) && std::strchr(stops, peek(p)) == nullptr) ++p.at;

		std::string value = p.text.substr(start, p.at - start);
		while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
		return value;
	}

	static void addLiteral(std::vector<ReplacementPiece>& pieces, char c) {
		if (pieces.empty() || pieces.back().kind != PieceKind::Literal) {
			pieces.push_back(ReplacementPiece());
		}

		pieces.back().text.push_back(c);
	}

	static void addPiece(std::vector<ReplacementPiece>& pieces, PieceKind kind, int group) {
		ReplacementPiece piece;
		piece.kind = kind;
		piece.group = group;
		pieces.push_back(piece);
	}

	static void addReplacementEscape(std::vector<ReplacementPiece>& pieces, char c) {
		switch (c) {
		case 'n':  addLiteral(pieces, '\n');                  break;
		case 't':  addLiteral(pieces, '\t');                  break;
		case 'U':  addPiece(pieces, PieceKind::UpperAll, 0);  break;
		case 'L':  addPiece(pieces, PieceKind::LowerAll, 0);  break;
		case 'u':  addPiece(pieces, PieceKind::UpperNext, 0); break;
		case 'l':  addPiece(pieces, PieceKind::LowerNext, 0); break;
		case 'E':  addPiece(pieces, PieceKind::CaseEnd, 0);   break;
		default:
			if (c >= '0' && c <= '9') addPiece(pieces, PieceKind::Group, c - '0');
			else                      addLiteral(pieces, c);
			break;
		}
	}

	static bool parseReplacement(ScriptParser& p, char delimiter,
	                             std::vector<ReplacementPiece>& pieces) {
		while (!atEnd(p) && peek(p) != delimiter) {
			const char c = p.text[p.at++];
			if (c == '&') {
				addPiece(pieces, PieceKind::Whole, 0);
				continue;
			}

			if (c != '\\' || atEnd(p)) {
				addLiteral(pieces, c);
				continue;
			}

			const char next = p.text[p.at++];
			const bool literal = next == delimiter || next == '\n' || next == '&' || next == '\\';
			if (literal) addLiteral(pieces, next);
			else         addReplacementEscape(pieces, next);
		}

		if (atEnd(p)) return fail(p, "unterminated `s' command");
		++p.at;
		return true;
	}

	static bool applySubstFlag(char c, Substitution& subst, bool& ignore_case) {
		if (c == 'g')                  subst.global = true;
		else if (c == 'p')             subst.print = true;
		else if (c == 'i' || c == 'I') ignore_case = true;
		else return c == 'm' || c == 'M' || c == 'e';
		return true;
	}

	static bool parseSubstFlags(ScriptParser& p, Substitution& subst, bool& ignore_case) {
		for (;;) {
			const char c = peek(p);
			if (c == 'w') {
				++p.at;
				subst.write_file = restOfLine(p, "\n");
				return !subst.write_file.empty() || fail(p, "missing filename in w flag");
			}

			if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
				parseNumber(p, subst.nth);
				if (subst.nth == 0) return fail(p, "number option to `s' command may not be zero");
				continue;
			}

			if (!applySubstFlag(c, subst, ignore_case)) return true;
			++p.at;
		}
	}

	static bool parseSubstitution(ScriptParser& p, Command& command) {
		const char delimiter = peek(p);
		const bool bad_delimiter = atEnd(p) || delimiter == '\n' || delimiter == '\\';
		if (bad_delimiter) return fail(p, "unterminated `s' command");
		++p.at;

		std::string pattern;
		if (!readDelimited(p, delimiter, pattern)) return false;
		if (!parseReplacement(p, delimiter, command.subst.replacement)) return false;

		bool ignore_case = false;
		if (!parseSubstFlags(p, command.subst, ignore_case)) return false;
		return addRegex(p, pattern, ignore_case, command.subst.regex);
	}

	static bool readTransliterationSide(ScriptParser& p, char delimiter, std::string& out) {
		while (!atEnd(p) && peek(p) != delimiter) {
			const char c = p.text[p.at++];
			if (c != '\\' || atEnd(p)) {
				out.push_back(c);
				continue;
			}

			const char next = p.text[p.at++];
			out.push_back(next == 'n' ? '\n' : (next == 't' ? '\t' : next));
		}

		if (atEnd(p)) return fail(p, "unterminated `y' command");
		++p.at;
		return true;
	}

	static bool parseTransliteration(ScriptParser& p, Command& command) {
		const char delimiter = peek(p);
		++p.at;
		std::string from;
		std::string to;
		if (!readTransliterationSide(p, delimiter, from)
		    || !readTransliterationSide(p, delimiter, to)) {
			return false;
		}

		if (from.size() != to.size()) {
			return fail(p, "strings for `y' command are different lengths");
		}

		command.table.resize(256);
		for (int k = 0; k < 256; ++k) command.table[k] = static_cast<unsigned char>(k);
		for (std::size_t k = 0; k < from.size(); ++k) {
			command.table[static_cast<unsigned char>(from[k])] = static_cast<unsigned char>(to[k]);
		}

		return true;
	}

	static bool parseExitCode(ScriptParser& p, Command& command) {
		skipBlanks(p);
		long code = 0;
		if (parseNumber(p, code)) command.exit_code = static_cast<int>(code);
		return true;
	}

	static void skipOptionalNumber(ScriptParser& p) {
		skipBlanks(p);
		long ignored = 0;
		parseNumber(p, ignored);
	}

	static bool parseCommandArguments(ScriptParser& p, Command& command) {
		switch (command.name) {
		case 'a':
		case 'i':
		case 'c': parseText(p, command.text);                     return true;
		case ':':
			if (command.first.kind != AddressKind::None) {
				return fail(p, ": doesn't want any addresses");
			}

			command.text = restOfLine(p, "\n;");
			return !command.text.empty() || fail(p, "\":\" lacks a label");
		case 'b':
		case 't':
		case 'T': command.text = restOfLine(p, "\n;}");            return true;
		case 'r':
		case 'R':
		case 'w':
		case 'W':
			command.text = restOfLine(p, "\n");
			return !command.text.empty() || fail(p, "missing filename in r/R/w/W commands");
		case 'q':
		case 'Q': return parseExitCode(p, command);
		case 'l':
		case 'L': skipOptionalNumber(p); return true;
		case 's': return parseSubstitution(p, command);
		case 'y': return parseTransliteration(p, command);
		default:  return std::strchr("{=dDgGhHnNpPxzF", command.name) != nullptr
			|| fail(p, std::string("unknown command: `") + command.name + "'");
		}
	}

	static bool finishCommand(ScriptParser& p) {
		skipBlanks(p);
		const char c = peek(p);
		if (atEnd(p) || c == ';' || c == '\n' || c == '}' || c == '#') return true;
		return fail(p, std::string("extra characters after command"));
	}

	static bool parseCloseBlock(ScriptParser& p) {
		++p.at;
		if (p.open_blocks.empty()) return fail(p, "unexpected `}'");

		Command close;
		close.name = '}';
		p.program.commands[p.open_blocks.back()].target = p.program.commands.size();
		p.open_blocks.pop_back();
		p.program.commands.push_back(close);
		return true;
	}

	static bool parseOneCommand(ScriptParser& p) {
		if (peek(p) == '#') {
			while (!atEnd(p) && peek(p) != '\n') ++p.at;
			return true;
		}

		if (peek(p) == '}') return parseCloseBlock(p);

		Command command;
		if (!parseAddresses(p, command)) return false;
		skipBlanks(p);
		while (peek(p) == '!') {
			command.negate = true;
			++p.at;
			skipBlanks(p);
		}

		if (atEnd(p)) return fail(p, "missing command");
		command.name = p.text[p.at++];
		if (!parseCommandArguments(p, command)) return false;
		if (command.name == '{') p.open_blocks.push_back(p.program.commands.size());
		p.program.commands.push_back(command);
		return std::strchr("aic{", command.name) != nullptr || finishCommand(p);
	}

	static bool resolveLabels(ScriptParser& p) {
		std::map<std::string, std::size_t> labels;
		for (std::size_t k = 0; k < p.program.commands.size(); ++k) {
			if (p.program.commands[k].name == ':') labels[p.program.commands[k].text] = k;
		}

		for (Command& command : p.program.commands) {
			if (std::strchr("btT", command.name) == nullptr || command.name == '\0') continue;
			if (command.text.empty()) {
				command.target = p.program.commands.size();
				continue;
			}

			const auto found = labels.find(command.text);
			if (found == labels.end()) {
				return fail(p, "can't find label for jump to `" + command.text + "'");
			}

			command.target = found->second;
		}

		return true;
	}

	static bool parseScript(const std::string& text, bool extended, Program& program,
	                        std::string& error) {
		ScriptParser p{ text, program, extended };
		if (text.compare(0, 2, "#n") == 0 && (text.size() == 2 || text[2] == '\n')) {
			program.quiet = true;
		}

		for (;;) {
			skipSeparators(p);
			if (atEnd(p)) break;
			if (!parseOneCommand(p)) break;
		}

		if (p.error.empty() && !p.open_blocks.empty()) fail(p, "unmatched `{'");
		if (p.error.empty()) resolveLabels(p);
		error = p.error;
		return error.empty();
	}

	static bool openNextFile(LineSource& source) {
		while (source.next_file < source.files.size()) {
			const std::string& name = source.files[source.next_file++];
			const bool from_stdin = name == "-" && source.dash_is_stdin;
			source.current = from_stdin ? stdin : fopenNative(*source.exec, name, "rb");
			if (source.current != nullptr) return true;

			std::fprintf(stderr, "sed: can't read %s: %s\n", name.c_str(), std::strerror(errno));
			source.failed = true;
		}

		return false;
	}

	static bool readRawLine(LineSource& source, std::string& line, bool& terminated) {
		line.clear();
		for (;;) {
			if (source.current == nullptr && !openNextFile(source)) return false;

			int c = 0;
			while ((c = std::fgetc(source.current)) != EOF && c != source.delimiter) {
				line.push_back(static_cast<char>(c));
			}

			terminated = c != EOF;
			if (terminated || !line.empty()) return true;
			if (source.current != stdin) std::fclose(source.current);
			source.current = nullptr;
		}
	}

	static bool fillAhead(LineSource& source) {
		if (source.has_ahead) return true;
		source.has_ahead = readRawLine(source, source.ahead, source.ahead_terminated);
		return source.has_ahead;
	}

	static bool nextLine(LineSource& source, std::string& line, bool& terminated) {
		if (!fillAhead(source)) return false;
		line.swap(source.ahead);
		terminated = source.ahead_terminated;
		source.has_ahead = false;
		++source.line_number;
		return true;
	}

	static bool isLastLine(LineSource& source) {
		return !fillAhead(source);
	}

	static void writeRaw(Output& out, const std::string& text) {
		if (out.buffer != nullptr) out.buffer->append(text);
		else                       std::fwrite(text.data(), 1, text.size(), out.stream);
	}

	// Like GNU sed: an input line that had no newline is printed without
	// one, unless something else is printed after it.
	static void emitLine(Output& out, const std::string& text, bool terminated) {
		if (out.missing_newline) writeRaw(out, std::string(1, out.delimiter));
		writeRaw(out, text);
		out.missing_newline = !terminated;
		if (terminated) writeRaw(out, std::string(1, out.delimiter));
	}

	static const std::regex* resolveRegex(const Program& program, RunState& state, int index) {
		if (index >= 0) state.last_regex = &program.regexes[static_cast<std::size_t>(index)];
		return state.last_regex;
	}

	static bool searchFrom(const std::string& text, std::size_t from, const std::regex& re,
	                       std::smatch& match) {
		const auto flags = from == 0 ? std::regex_constants::match_default
			: std::regex_constants::match_prev_avail;
		return searchRegexFlags(text.begin() + static_cast<std::ptrdiff_t>(from), text.end(), re,
			match, flags);
	}

	static bool patternMatches(const Program& program, RunState& state, int regex) {
		const std::regex* re = resolveRegex(program, state, regex);
		std::smatch match;
		return re != nullptr && searchFrom(state.pattern, 0, *re, match);
	}

	static bool matchSingle(const Program& program, RunState& state, LineSource& source,
	                        const Address& address) {
		const long line = source.line_number;
		switch (address.kind) {
		case AddressKind::Line: return line == address.line;
		case AddressKind::Last: return isLastLine(source);
		case AddressKind::Step:
			if (address.step <= 0) return line == address.line;
			return line >= address.line && (line - address.line) % address.step == 0;
		case AddressKind::Regex: return patternMatches(program, state, address.regex);
		case AddressKind::Zero: return false;
		case AddressKind::None:
		default:
			return true;
		}
	}

	static void startRange(const Program& program, RunState& state, LineSource& source,
	                       Command& command) {
		const long line = source.line_number;
		command.range_active = true;
		if (command.end_kind == RangeEnd::Relative) {
			command.range_end = line + command.end_count;
			command.range_active = command.end_count > 0;
			return;
		}

		if (command.end_kind == RangeEnd::Multiple) {
			const long step = command.end_count;
			command.range_active = step > 0 && line % step != 0;
			command.range_end = step > 0 ? (line / step + 1) * step : line;
			return;
		}

		const Address& end = command.second;
		if (end.kind == AddressKind::Line && end.line <= line) command.range_active = false;
		if (end.kind == AddressKind::Last && isLastLine(source)) command.range_active = false;
		const bool zero_start = command.first.kind == AddressKind::Zero;
		const bool regex_end = end.kind == AddressKind::Regex;
		if (zero_start && regex_end && matchSingle(program, state, source, end)) {
			command.range_active = false;
		}
	}

	static void continueRange(const Program& program, RunState& state, LineSource& source,
	                          Command& command) {
		const long line = source.line_number;
		if (command.end_kind != RangeEnd::Address) {
			if (line >= command.range_end) command.range_active = false;
			return;
		}

		const Address& end = command.second;
		const bool line_end = end.kind == AddressKind::Line && line >= end.line;
		const bool other_end = end.kind != AddressKind::Line
			&& matchSingle(program, state, source, end);
		if (line_end || other_end) command.range_active = false;
	}

	static bool addressMatches(const Program& program, RunState& state, LineSource& source,
	                           Command& command) {
		if (command.first.kind == AddressKind::None) return true;
		if (command.second.kind == AddressKind::None) {
			return matchSingle(program, state, source, command.first);
		}

		if (command.range_active) {
			continueRange(program, state, source, command);
			return true;
		}

		const bool zero_start = command.first.kind == AddressKind::Zero && source.line_number == 1;
		if (!zero_start && !matchSingle(program, state, source, command.first)) return false;
		startRange(program, state, source, command);
		return true;
	}

	static char convertCase(char c, PieceKind mode) {
		const unsigned char byte = static_cast<unsigned char>(c);
		if (mode == PieceKind::UpperAll || mode == PieceKind::UpperNext) {
			return static_cast<char>(std::toupper(byte));
		}

		if (mode == PieceKind::LowerAll || mode == PieceKind::LowerNext) {
			return static_cast<char>(std::tolower(byte));
		}

		return c;
	}

	static void appendConverted(std::string& out, const std::string& text, PieceKind& mode,
	                            PieceKind& once) {
		for (const char c : text) {
			if (once != PieceKind::CaseEnd) {
				out.push_back(convertCase(c, once));
				once = PieceKind::CaseEnd;
				continue;
			}

			out.push_back(convertCase(c, mode));
		}
	}

	static std::string expandReplacement(const std::vector<ReplacementPiece>& pieces,
	                                     const std::smatch& match) {
		std::string out;
		PieceKind mode = PieceKind::CaseEnd;
		PieceKind once = PieceKind::CaseEnd;
		for (const ReplacementPiece& piece : pieces) {
			switch (piece.kind) {
			case PieceKind::Literal: appendConverted(out, piece.text, mode, once);          break;
			case PieceKind::Whole:   appendConverted(out, match.str(0), mode, once);        break;
			case PieceKind::Group:
				appendConverted(out, match.str(piece.group), mode, once);
				break;
			case PieceKind::UpperAll:
			case PieceKind::LowerAll:
			case PieceKind::CaseEnd:  mode = piece.kind;                                    break;
			case PieceKind::UpperNext:
			case PieceKind::LowerNext: once = piece.kind;                                   break;
			default:                                                                        break;
			}
		}

		return out;
	}

	// An empty match right where the previous match ended is skipped, as in
	// GNU sed: s/x*/-/g turns "xy" into "-y-", not "--y-".
	static bool substitute(const Substitution& subst, const std::regex& re, std::string& pattern) {
		std::string result;
		std::size_t from = 0;
		std::size_t previous_end = std::string::npos;
		long count = 0;
		bool replaced = false;
		std::smatch match;
		while (from <= pattern.size() && searchFrom(pattern, from, re, match)) {
			const std::size_t start = from + static_cast<std::size_t>(match.position(0));
			const std::size_t length = static_cast<std::size_t>(match.length(0));
			result.append(pattern, from, start - from);
			if (length == 0 && start == previous_end) {
				if (start < pattern.size()) result.push_back(pattern[start]);
				from = start + 1;
				continue;
			}

			previous_end = start + length;
			++count;
			const bool chosen = count == subst.nth || (subst.global && count > subst.nth);
			result += chosen ? expandReplacement(subst.replacement, match) : match.str(0);
			replaced = replaced || chosen;
			from = start + length;
			if (length == 0) {
				if (from < pattern.size()) result.push_back(pattern[from]);
				++from;
			}

			if (replaced && !subst.global) break;
		}

		if (!replaced) return false;
		if (from < pattern.size()) result.append(pattern, from, std::string::npos);
		pattern.swap(result);
		return true;
	}

	static std::FILE* writeFile(RunState& state, Executor& exec, const std::string& name) {
		if (name == "/dev/stdout") return stdout;
		if (name == "/dev/stderr") return stderr;

		const auto found = state.write_files.find(name);
		if (found != state.write_files.end()) return found->second;

		std::FILE* file = fopenNative(exec, name, "wb");
		state.write_files[name] = file;
		return file;
	}

	static void writeToFile(RunState& state, Executor& exec, const std::string& name,
	                        const std::string& text) {
		std::FILE* file = writeFile(state, exec, name);
		if (file == nullptr) return;
		std::fwrite(text.data(), 1, text.size(), file);
		std::fputc(state.out.delimiter, file);
	}

	static void runSubstitution(const Program& program, RunState& state, Executor& exec,
	                            const Command& command) {
		const std::regex* re = resolveRegex(program, state, command.subst.regex);
		if (re == nullptr) {
			std::fprintf(stderr, "sed: no previous regular expression\n");
			state.quit = true;
			state.exit_code = kSedSyntaxStatus;
			return;
		}

		if (!substitute(command.subst, *re, state.pattern)) return;
		state.substituted = true;
		if (command.subst.print) emitLine(state.out, state.pattern, true);
		if (!command.subst.write_file.empty()) {
			writeToFile(state, exec, command.subst.write_file, state.pattern);
		}
	}

	static std::string listedText(const std::string& text) {
		static const char kEscapes[] = "\\\a\b\f\n\r\t\v";
		static const char kLetters[] = "\\abfnrtv";
		std::string out;
		for (const char c : text) {
			const char* escape = c == '\0' ? nullptr : std::strchr(kEscapes, c);
			const unsigned char byte = static_cast<unsigned char>(c);
			char octal[8];
			if (escape != nullptr) out += std::string("\\") + kLetters[escape - kEscapes];
			else if (byte < 32 || byte >= 127) {
				std::snprintf(octal, sizeof(octal), "\\%03o", byte);
				out += octal;
			}
			else out.push_back(c);
		}

		return out;
	}

	static void listPattern(RunState& state) {
		const std::string text = listedText(state.pattern);
		std::string wrapped;
		for (std::size_t k = 0; k < text.size(); k += kListWidth - 1) {
			wrapped += text.substr(k, kListWidth - 1);
			if (k + kListWidth - 1 < text.size()) wrapped += "\\\n";
		}

		emitLine(state.out, wrapped + "$", true);
	}

	static void flushAppends(RunState& state) {
		if (state.appends.empty()) return;
		if (state.out.missing_newline) writeRaw(state.out, std::string(1, state.out.delimiter));
		state.out.missing_newline = false;
		for (const std::string& text : state.appends) writeRaw(state.out, text);
		state.appends.clear();
	}

	static void queueFile(RunState& state, Executor& exec, const std::string& name) {
		std::FILE* in = fopenNative(exec, name, "rb");
		if (in == nullptr) return;

		std::string content;
		char chunk[8192];
		for (std::size_t got = 0; (got = std::fread(chunk, 1, sizeof(chunk), in)) != 0;) {
			content.append(chunk, got);
		}

		std::fclose(in);
		state.appends.push_back(content);
	}

	static void queueText(RunState& state, const std::string& text) {
		state.appends.push_back(text + std::string(1, state.out.delimiter));
	}

	// The cycle-control commands. They return false when the cycle ends.
	static bool readNextInto(RunState& state, LineSource& source, bool append, bool quiet) {
		if (isLastLine(source)) {
			state.quit = true;
			return false;
		}

		if (!append && !quiet) emitLine(state.out, state.pattern, state.terminated);
		flushAppends(state);

		std::string line;
		nextLine(source, line, state.terminated);
		state.pattern = append ? state.pattern + "\n" + line : line;
		state.substituted = append && state.substituted;
		return true;
	}

	static std::string firstLine(const RunState& state) {
		return state.pattern.substr(0, state.pattern.find('\n'));
	}

	static void printFirstLine(RunState& state) {
		const bool multiline = state.pattern.find('\n') != std::string::npos;
		emitLine(state.out, firstLine(state), multiline || state.terminated);
	}

	// GNU sed keeps "did this line end in a newline" with each buffer, so a
	// final unterminated line stays unterminated only while it is in the
	// pattern space; the hold space starts out terminated.
	static Flow runHoldCommand(RunState& state, char name) {
		switch (name) {
		case 'g':
			state.pattern = state.hold;
			state.terminated = state.hold_terminated;
			break;
		case 'G':
			state.pattern += "\n" + state.hold;
			state.terminated = state.hold_terminated;
			break;
		case 'h':
			state.hold = state.pattern;
			state.hold_terminated = state.terminated;
			break;
		case 'H':
			state.hold += "\n" + state.pattern;
			state.hold_terminated = state.terminated;
			break;
		case 'x':
			state.pattern.swap(state.hold);
			std::swap(state.terminated, state.hold_terminated);
			break;
		case 'z':
			state.pattern.clear();
			break;
		default:
			break;
		}

		return Flow::Next;
	}

	static Flow runDeleteFirst(RunState& state) {
		const std::size_t newline = state.pattern.find('\n');
		if (newline == std::string::npos) return Flow::EndCycleQuiet;
		state.pattern.erase(0, newline + 1);
		return Flow::Restart;
	}

	static Flow runChange(RunState& state, const Command& command) {
		const bool range_done = command.second.kind == AddressKind::None || !command.range_active;
		if (range_done || command.negate) emitLine(state.out, command.text, true);
		return Flow::EndCycleQuiet;
	}

	static Flow runBranch(RunState& state, char name) {
		if (name == 'b') return Flow::Jump;

		const bool taken = name == 't' ? state.substituted : !state.substituted;
		state.substituted = false;
		return taken ? Flow::Jump : Flow::Next;
	}

	static Flow runOutputCommand(RunState& state, LineSource& source, Executor& exec,
	                             const Command& command) {
		switch (command.name) {
		case '=':
			writeRaw(state.out, std::to_string(source.line_number) + "\n");
			return Flow::Next;
		case 'p': emitLine(state.out, state.pattern, state.terminated); return Flow::Next;
		case 'P': printFirstLine(state);                           return Flow::Next;
		case 'l':
		case 'L': listPattern(state);                              return Flow::Next;
		case 'i': emitLine(state.out, command.text, true);          return Flow::Next;
		case 'a': queueText(state, command.text);                   return Flow::Next;
		case 'r': queueFile(state, exec, command.text);             return Flow::Next;
		case 'w': writeToFile(state, exec, command.text, state.pattern); return Flow::Next;
		case 'W': writeToFile(state, exec, command.text, firstLine(state)); return Flow::Next;
		case 'F': writeRaw(state.out, "-\n");                       return Flow::Next;
		default:  return Flow::Next;
		}
	}

	static void transliterate(RunState& state, const Command& command) {
		for (char& c : state.pattern) {
			c = static_cast<char>(command.table[static_cast<unsigned char>(c)]);
		}
	}

	static Flow runCommand(const Program& program, RunState& state, LineSource& source,
	                       Executor& exec, const Command& command, bool quiet) {
		switch (command.name) {
		case '{':
		case '}':
		case ':': return Flow::Next;
		case 'd': return Flow::EndCycleQuiet;
		case 'D': return runDeleteFirst(state);
		case 'c': return runChange(state, command);
		case 'n': return readNextInto(state, source, false, quiet) ? Flow::Next : Flow::EndCycle;
		case 'N': return readNextInto(state, source, true, quiet) ? Flow::Next : Flow::EndCycle;
		case 'q':
			state.quit = true;
			state.exit_code = command.exit_code;
			return Flow::EndCycle;
		case 'Q':
			state.quit = true;
			state.exit_code = command.exit_code;
			return Flow::EndCycleQuiet;
		case 'b':
		case 't':
		case 'T': return runBranch(state, command.name);
		case 's': runSubstitution(program, state, exec, command); return Flow::Next;
		case 'y': transliterate(state, command); return Flow::Next;
		default:
			if (std::strchr("gGhHxz", command.name) != nullptr) {
				return runHoldCommand(state, command.name);
			}

			return runOutputCommand(state, source, exec, command);
		}
	}

	static std::size_t skipTarget(const Command& command, std::size_t pc) {
		return command.name == '{' ? command.target + 1 : pc + 1;
	}

	// Runs the script over the current pattern space. Returns true when the
	// next cycle should reuse the pattern space instead of reading (D).
	static bool runCycle(Program& program, RunState& state, LineSource& source, Executor& exec,
	                     bool quiet) {
		std::size_t pc = 0;
		while (pc < program.commands.size() && !state.quit) {
			Command& command = program.commands[pc];
			const bool selected = addressMatches(program, state, source, command) != command.negate;
			if (!selected) {
				pc = skipTarget(command, pc);
				continue;
			}

			const Flow flow = runCommand(program, state, source, exec, command, quiet);
			if (flow == Flow::Next) {
				++pc;
				continue;
			}

			if (flow == Flow::Jump) {
				pc = command.target;
				continue;
			}

			if (flow == Flow::EndCycle) break;

			flushAppends(state);
			return flow == Flow::Restart;
		}

		if (!quiet) emitLine(state.out, state.pattern, state.terminated);
		flushAppends(state);
		return false;
	}

	static void runProgram(Program& program, RunState& state, LineSource& source, Executor& exec,
	                       bool quiet) {
		bool reuse = false;
		while (!state.quit) {
			if (!reuse) {
				if (!nextLine(source, state.pattern, state.terminated)) break;
				state.substituted = false;
			}

			reuse = runCycle(program, state, source, exec, quiet);
		}
	}

	static void closeWriteFiles(RunState& state) {
		for (auto& entry : state.write_files) {
			if (entry.second != nullptr && entry.second != stdout && entry.second != stderr) {
				std::fclose(entry.second);
			}
		}

		state.write_files.clear();
	}

	static bool appendScriptFile(Executor& exec, const std::string& path, SedOptions& options) {
		std::vector<std::string> lines;
		if (!readAllLines(exec, path, lines)) {
			std::fprintf(stderr, "sed: couldn't open file %s: %s\n", path.c_str(),
				std::strerror(errno));
			return false;
		}

		for (const std::string& line : lines) {
			if (options.have_script) options.script.push_back('\n');
			options.script += line;
			options.have_script = true;
		}

		return true;
	}

	static void appendScript(SedOptions& options, const std::string& part) {
		if (options.have_script) options.script.push_back('\n');
		options.script += part;
		options.have_script = true;
	}

	static bool applyInPlaceFlag(const std::string& arg, SedOptions& options) {
		if (arg.compare(0, 10, "--in-place") == 0) {
			options.in_place = true;
			if (arg.size() > 11) options.backup_suffix = arg.substr(11);
			return true;
		}

		if (arg.compare(0, 2, "-i") != 0) return false;
		options.in_place = true;
		options.backup_suffix = arg.substr(2);
		return true;
	}

	static bool applySedFlag(const std::string& arg, SedOptions& options) {
		const bool quiet = arg == "-n" || arg == "--quiet" || arg == "--silent";
		const bool extended = arg == "-E" || arg == "-r" || arg == "--regexp-extended";
		const bool ignored = arg == "-u" || arg == "--unbuffered" || arg == "--posix";
		if (quiet)                                    options.quiet = true;
		else if (extended)                            options.extended = true;
		else if (arg == "-s" || arg == "--separate")  options.separate = true;
		else if (arg == "-z" || arg == "--null-data") options.null_data = true;
		else if (!ignored) return applyInPlaceFlag(arg, options);
		return true;
	}

	static bool applyScriptOption(Executor& exec, const std::vector<std::string>& args,
	                              std::size_t& i, SedOptions& options, bool& valid) {
		const std::string& arg = args[i];
		const bool has_value = i + 1 < args.size();
		if ((arg == "-e" || arg == "--expression") && has_value) {
			appendScript(options, args[++i]);
			return true;
		}

		if (arg.compare(0, 2, "-e") == 0 && arg.size() > 2) {
			appendScript(options, arg.substr(2));
			return true;
		}

		if ((arg != "-f" && arg != "--file") || !has_value) return false;
		valid = appendScriptFile(exec, args[++i], options);
		return true;
	}

	static bool parseSedArgs(Executor& exec, const std::vector<std::string>& args,
	                         SedOptions& options) {
		std::vector<std::string> operands;
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg == "--") {
				const auto rest = args.begin() + static_cast<std::ptrdiff_t>(i) + 1;
				operands.insert(operands.end(), rest, args.end());
				break;
			}

			bool valid = true;
			if (applyScriptOption(exec, args, i, options, valid)) {
				if (!valid) return false;
				continue;
			}

			if (arg.size() < 2 || arg[0] != '-') {
				operands.push_back(arg);
				continue;
			}

			if (applySedFlag(arg, options)) continue;
			std::fprintf(stderr, "sed: unknown option -- '%s'\n", arg.c_str());
			return false;
		}

		if (!options.have_script && !operands.empty()) {
			options.script = operands.front();
			options.have_script = true;
			operands.erase(operands.begin());
		}

		options.files = operands;
		if (options.files.empty() && !options.in_place) options.files.push_back("-");
		return options.have_script;
	}

	static void resetRanges(Program& program) {
		for (Command& command : program.commands) command.range_active = false;
	}

	static int runOnFiles(Executor& exec, Program& program, const SedOptions& options,
	                      const std::vector<std::string>& files, RunState& state) {
		LineSource source;
		source.exec = &exec;
		source.files = files;
		source.delimiter = options.null_data ? '\0' : '\n';
		source.dash_is_stdin = !options.in_place;
		state.out.delimiter = source.delimiter;
		runProgram(program, state, source, exec, options.quiet || program.quiet);
		if (source.current != nullptr && source.current != stdin) std::fclose(source.current);
		return source.failed ? kSedInputStatus : 0;
	}

	static const DWORD kReplaceBlockingAttributes =
		FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
	static const DWORD kSettableAttributes = kReplaceBlockingAttributes | FILE_ATTRIBUTE_ARCHIVE
		| FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;

	static std::string lastErrorText() {
		return posixErrorText(std::error_code(static_cast<int>(::GetLastError()),
			std::system_category()));
	}

	static std::string shownSibling(const std::string& file, const fs::path& sibling) {
		const std::size_t slash = file.find_last_of("/\\");
		const std::string directory = slash == std::string::npos ? "." : file.substr(0, slash);
		return directory + "/" + pathToUtf8(sibling.filename());
	}

	static fs::path backupPath(const fs::path& target, const std::string& suffix) {
		const std::string base = pathToUtf8(target.filename());
		if (suffix.find('*') == std::string::npos) {
			return target.parent_path() / utf8ToPath(base + suffix);
		}

		std::string name;
		for (char c : suffix) {
			if (c == '*') name += base;
			else name.push_back(c);
		}

		return target.parent_path() / utf8ToPath(name);
	}

	static bool isEditableFile(Executor& exec, const std::string& file) {
		std::error_code ec;
		const fs::file_status status = fs::status(toNative(exec, file), ec);
		const bool missing = ec || status.type() == fs::file_type::not_found;
		if (missing || fs::is_regular_file(status)) return true;

		std::fprintf(stderr, "sed: couldn't edit %s: not a regular file\n", file.c_str());
		return false;
	}

	static bool writeAndClose(std::FILE* out, const std::string& content,
	                          const std::string& shown) {
		const std::size_t written = std::fwrite(content.data(), 1, content.size(), out);
		if (written != content.size() || std::fflush(out) != 0) {
			std::fprintf(stderr, "sed: couldn't write %zu items to %s: %s\n",
				content.size() - written, shown.c_str(), std::strerror(errno));
			std::fclose(out);
			return false;
		}

		if (std::fclose(out) == 0) return true;

		std::fprintf(stderr, "sed: couldn't close %s: %s\n", shown.c_str(), std::strerror(errno));
		return false;
	}

	static bool writeTempCopy(const fs::path& target, const std::string& file,
	                          const std::string& content, fs::path& temp) {
		std::FILE* out = createSiblingTempFile(target, "sed", temp);
		if (out == nullptr) {
			std::fprintf(stderr, "sed: couldn't open temporary file %s: %s\n",
				shownSibling(file, temp).c_str(), std::strerror(errno));
			return false;
		}

		if (writeAndClose(out, content, shownSibling(file, temp))) return true;

		std::error_code ec;
		fs::remove(temp, ec);
		return false;
	}

	static bool moveBackup(const fs::path& target, const fs::path& backup,
	                       const std::string& file) {
		if (::MoveFileExW(target.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE) {
			return true;
		}

		std::fprintf(stderr, "sed: cannot rename %s: %s\n", file.c_str(), lastErrorText().c_str());
		return false;
	}

	static bool replaceTarget(const fs::path& temp, const fs::path& target, const std::string& file,
	                          DWORD attributes) {
		const bool blocking = attributes != INVALID_FILE_ATTRIBUTES
			&& (attributes & kReplaceBlockingAttributes) != 0;
		if (blocking) ::SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL);

		const DWORD flags = MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH;
		const bool moved = ::MoveFileExW(temp.c_str(), target.c_str(), flags) != FALSE;
		const std::string error = moved ? std::string() : lastErrorText();
		if (blocking) ::SetFileAttributesW(target.c_str(), attributes & kSettableAttributes);
		if (moved) return true;

		std::fprintf(stderr, "sed: cannot rename %s: %s\n",
			shownSibling(file, temp).c_str(), error.c_str());
		return false;
	}

	static bool writeInPlace(Executor& exec, const std::string& file, const std::string& content,
	                         const std::string& suffix) {
		const fs::path target = toNative(exec, file);
		const DWORD attributes = ::GetFileAttributesW(target.c_str());
		fs::path temp;
		if (!writeTempCopy(target, file, content, temp)) return false;

		const bool backed_up = suffix.empty()
			|| moveBackup(target, backupPath(target, suffix), file);
		if (backed_up && replaceTarget(temp, target, file, attributes)) return true;

		std::error_code ec;
		fs::remove(temp, ec);
		return false;
	}

	static int editInPlace(Executor& exec, Program& program, const SedOptions& options) {
		if (options.files.empty()) {
			std::fprintf(stderr, "sed: no input files\n");
			return kSedPanicStatus;
		}

		int status = 0;
		for (const std::string& file : options.files) {
			if (!isEditableFile(exec, file)) {
				status = kSedPanicStatus;
				continue;
			}

			RunState state;
			std::string buffer;
			state.out.buffer = &buffer;
			resetRanges(program);
			const int file_status = runOnFiles(exec, program, options, { file }, state);
			closeWriteFiles(state);
			if (file_status != 0) {
				status = file_status;
				continue;
			}

			if (!writeInPlace(exec, file, buffer, options.backup_suffix)) status = kSedPanicStatus;
			if (state.quit) return state.exit_code != 0 ? state.exit_code : status;
		}

		return status;
	}

	static int runSeparately(Executor& exec, Program& program, const SedOptions& options) {
		RunState state;
		int status = 0;
		for (const std::string& file : options.files) {
			resetRanges(program);
			status = (std::max)(status, runOnFiles(exec, program, options, { file }, state));
			if (state.quit) break;
		}

		closeWriteFiles(state);
		std::fflush(stdout);
		return state.exit_code != 0 ? state.exit_code : status;
	}

	static int builtin_sed(Executor& exec, const std::vector<std::string>& args) {
		SedOptions options;
		if (!parseSedArgs(exec, args, options)) {
			if (!options.have_script) std::fprintf(stderr, "sed: no script specified\n");
			return kSedSyntaxStatus;
		}

		Program program;
		std::string error;
		if (!parseScript(options.script, options.extended, program, error)) {
			std::fprintf(stderr, "sed: -e expression #1, %s\n", error.c_str());
			return kSedSyntaxStatus;
		}

		if (options.in_place) return editInPlace(exec, program, options);
		if (options.separate) return runSeparately(exec, program, options);

		RunState state;
		const int status = runOnFiles(exec, program, options, options.files, state);
		closeWriteFiles(state);
		std::fflush(stdout);
		return state.exit_code != 0 ? state.exit_code : status;
	}

	void registerSedBuiltin(Executor& exec) {
		exec.registerBuiltin("sed", builtin_sed);
	}

}  // namespace wbsh

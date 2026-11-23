/**
 * @file script.cpp
 * @brief Non-interactive (single-source) execution entry point.
 */

#include "script.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>

#  include <io.h>
#endif /* _WIN32 */

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <ostream>
#include <string>
#include <vector>

#include "ast.h"
#include "diagnostics.h"
#include "environment.h"
#include "executor.h"
#include "expander.h"
#include "funcprint.h"
#include "history.h"
#include "lexer.h"
#include "parser.h"
#include "printer.h"
#include "setup.h"
#include "startup.h"

namespace wbsh {

	namespace script_detail {

		struct ExpandDump {
			Expander& expander;
			Environment& env;
			std::ostream& os;
		};

		struct ErrorColors {
			const char* kind;
			const char* location;
			const char* reset;
		};

		// A script read from standard input, as bash reads it: a pipe one
		// byte at a time, a file a buffer at a time with the descriptor put
		// back just past each command before it runs. Either way a command
		// that reads standard input gets the script's next line.
		struct StdinScript {
			bool        seekable = false;
			bool        at_start = true;
			long long   base = 0;
			std::string buffer;
			std::size_t consumed = 0;
		};

		enum class Completeness {
			Incomplete,
			Complete,
			Fatal,
			EndOfInput,
		};

	}  // namespace script_detail

	using script_detail::Completeness;
	using script_detail::ErrorColors;
	using script_detail::ExpandDump;
	using script_detail::StdinScript;

	static bool stdoutIsTty() {
#ifdef _WIN32
		return _isatty(_fileno(stdout)) != 0;
#else
		return false;
#endif /* _WIN32 */
	}

	static bool stderrIsTty() {
#ifdef _WIN32
		return _isatty(_fileno(stderr)) != 0;
#else
		return false;
#endif /* _WIN32 */
	}

	static void dumpExpanded(ExpandDump& dump, const Node& node, int depth);

	static void indent(std::ostream& os, int depth) {
		for (int i = 0; i < depth; ++i) os << "  ";
	}

	static void escape(std::ostream& os, const std::string& text) {
		os << '"';
		for (char c : text) {
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

	static void writeLine(std::ostream& os, int depth, const std::string& text) {
		indent(os, depth);
		os << text << "\n";
	}

	static void writeQuotedFields(std::ostream& os, const std::vector<std::string>& fields) {
		for (const auto& field : fields) {
			os << ' ';
			escape(os, field);
		}
	}

	static void writeExpandedValue(ExpandDump& dump, const Word& word, const char* error_label) {
		const std::string value = dump.expander.expandStringValue(word);
		if (dump.expander.failed()) {
			dump.os << error_label << dump.expander.takeError() << ">";
			return;
		}

		escape(dump.os, value);
	}

	static void dumpExpandedBody(ExpandDump& dump, const char* label, const Node* body, int depth) {
		writeLine(dump.os, depth, label);
		if (body != nullptr) dumpExpanded(dump, *body, depth + 1);
	}

	static void dumpExpandedAssignments(ExpandDump& dump, const SimpleCommand& command, int depth) {
		const bool bare_assignments = command.words.empty();
		for (const auto& assignment : command.assignments) {
			indent(dump.os, depth + 1);
			const std::string value = dump.expander.expandAssignmentValue(assignment.value);
			if (dump.expander.failed()) {
				dump.os << "<expand error: " << dump.expander.takeError() << ">";
			}

			if (bare_assignments) dump.env.set(assignment.name, value);
			dump.os << "Assign " << assignment.name << "=";
			escape(dump.os, value);
			dump.os << "\n";
		}
	}

	static void dumpExpandedWords(ExpandDump& dump, const SimpleCommand& command, int depth) {
		for (const auto& word : command.words) {
			indent(dump.os, depth + 1);
			dump.os << "Word raw=";
			escape(dump.os, word.raw);
			dump.os << " ->";

			const auto fields = dump.expander.expandWord(word);
			if (dump.expander.failed()) {
				dump.os << " <expand error: " << dump.expander.takeError() << ">";
			} else if (fields.empty()) {
				dump.os << " (vanished)";
			}

			writeQuotedFields(dump.os, fields);
			dump.os << "\n";
		}
	}

	static void dumpExpandedRedirs(ExpandDump& dump, const SimpleCommand& command, int depth) {
		for (const auto& redir : command.redirs) {
			indent(dump.os, depth + 1);
			dump.os << "Redir " << redirOpName(redir.op);
			if (redir.fd != -1) dump.os << " fd=" << redir.fd;
			dump.os << " target=";
			writeExpandedValue(dump, redir.target, "<expand error: ");

			if (redir.op == RedirOp::DLess || redir.op == RedirOp::DLessDash) {
				dump.os << " body=";
				const std::string body =
					dump.expander.expandHeredoc(redir.heredoc_body, redir.heredoc_quoted);
				escape(dump.os, body);
				if (dump.expander.failed()) dump.expander.takeError();
			}

			dump.os << "\n";
		}
	}

	static void dumpExpandedSimpleCommand(ExpandDump& dump, const SimpleCommand& command,
	                                      int depth) {
		indent(dump.os, depth);
		dump.os << "SimpleCommand @ " << command.loc.line << ":" << command.loc.column << "\n";
		dumpExpandedAssignments(dump, command, depth);
		dumpExpandedWords(dump, command, depth);
		dumpExpandedRedirs(dump, command, depth);
	}

	static void dumpExpandedPipeline(ExpandDump& dump, const Pipeline& pipeline, int depth) {
		writeLine(dump.os, depth, pipeline.bang ? "Pipeline (bang)" : "Pipeline");
		for (const auto& command : pipeline.commands) dumpExpanded(dump, *command, depth + 1);
	}

	static void dumpExpandedAndOr(ExpandDump& dump, const AndOr& andor, int depth) {
		writeLine(dump.os, depth, andor.op == AndOr::Op::AndIf ? "&&" : "||");
		dumpExpanded(dump, *andor.left, depth + 1);
		dumpExpanded(dump, *andor.right, depth + 1);
	}

	static void dumpExpandedList(ExpandDump& dump, const List& list, int depth) {
		for (const auto& item : list.items) dumpExpanded(dump, *item.command, depth);
	}

	static void dumpExpandedIf(ExpandDump& dump, const IfClause& clause, int depth) {
		writeLine(dump.os, depth, "If");
		for (std::size_t i = 0; i < clause.branches.size(); ++i) {
			const char* label = (i == 0) ? "if-cond:" : "elif-cond:";
			dumpExpandedBody(dump, label, clause.branches[i].cond, depth + 1);
			dumpExpandedBody(dump, "then:", clause.branches[i].body, depth + 1);
		}

		if (clause.else_body == nullptr) return;

		dumpExpandedBody(dump, "else:", clause.else_body, depth + 1);
	}

	static void dumpExpandedWhile(ExpandDump& dump, const WhileClause& loop, int depth) {
		writeLine(dump.os, depth, loop.until ? "Until" : "While");
		dumpExpandedBody(dump, "cond:", loop.cond, depth + 1);
		dumpExpandedBody(dump, "do:", loop.body, depth + 1);
	}

	static void dumpExpandedFunctionDef(ExpandDump& dump, const FunctionDef& function, int depth) {
		writeLine(dump.os, depth, "FunctionDef " + function.name);
		if (function.body != nullptr) dumpExpanded(dump, *function.body, depth + 1);
	}

	static void dumpExpandedForItems(ExpandDump& dump, const ForClause& loop, int depth) {
		indent(dump.os, depth + 1);
		dump.os << "items:";
		for (const auto& word : loop.items) {
			const auto fields = dump.expander.expandWord(word);
			if (dump.expander.failed()) {
				dump.os << " <error: " << dump.expander.takeError() << ">";
				continue;
			}

			writeQuotedFields(dump.os, fields);
		}

		dump.os << "\n";
	}

	static void dumpExpandedFor(ExpandDump& dump, const ForClause& loop, int depth) {
		writeLine(dump.os, depth, "For " + loop.var);
		if (loop.has_in) dumpExpandedForItems(dump, loop, depth);
		dumpExpandedBody(dump, "do:", loop.body, depth + 1);
	}

	static void dumpExpandedCasePatterns(ExpandDump& dump, const CaseClause::Item& item,
	                                     int depth) {
		indent(dump.os, depth + 1);
		dump.os << "patterns:";
		for (const auto& pattern : item.patterns) {
			dump.os << ' ';
			const std::string text = dump.expander.expandStringValue(pattern);
			if (dump.expander.failed()) {
				dump.expander.takeError();
				dump.os << "<err>";
				continue;
			}

			escape(dump.os, text);
		}

		dump.os << "\n";
	}

	static void dumpExpandedCase(ExpandDump& dump, const CaseClause& clause, int depth) {
		indent(dump.os, depth);
		dump.os << "Case subject=";
		writeExpandedValue(dump, clause.subject, "<error: ");
		dump.os << "\n";

		for (const auto& item : clause.items) {
			dumpExpandedCasePatterns(dump, item, depth);
			if (item.body != nullptr) dumpExpandedBody(dump, "body:", item.body, depth + 2);
		}
	}

	static void dumpExpanded(ExpandDump& dump, const Node& node, int depth) {
		switch (node.kind) {
		case Node::Kind::SimpleCommand:
			dumpExpandedSimpleCommand(dump, static_cast<const SimpleCommand&>(node), depth);
			return;
		case Node::Kind::Pipeline:
			dumpExpandedPipeline(dump, static_cast<const Pipeline&>(node), depth);
			return;
		case Node::Kind::AndOr:
			dumpExpandedAndOr(dump, static_cast<const AndOr&>(node), depth);
			return;
		case Node::Kind::List:
			dumpExpandedList(dump, static_cast<const List&>(node), depth);
			return;
		case Node::Kind::BraceGroup:
			dumpExpandedBody(dump, "BraceGroup", static_cast<const BraceGroup&>(node).body, depth);
			return;
		case Node::Kind::Subshell:
			dumpExpandedBody(dump, "Subshell", static_cast<const Subshell&>(node).body, depth);
			return;
		case Node::Kind::IfClause:
			dumpExpandedIf(dump, static_cast<const IfClause&>(node), depth);
			return;
		case Node::Kind::WhileClause:
			dumpExpandedWhile(dump, static_cast<const WhileClause&>(node), depth);
			return;
		case Node::Kind::ForClause:
			dumpExpandedFor(dump, static_cast<const ForClause&>(node), depth);
			return;
		case Node::Kind::CaseClause:
			dumpExpandedCase(dump, static_cast<const CaseClause&>(node), depth);
			return;
		case Node::Kind::FunctionDef:
			dumpExpandedFunctionDef(dump, static_cast<const FunctionDef&>(node), depth);
			return;
		case Node::Kind::DBracket:
			writeLine(dump.os, depth, "DBracket");
			return;
		case Node::Kind::ArithCommand: {
			const auto& command = static_cast<const ArithCommand&>(node);
			writeLine(dump.os, depth, "ArithCommand " + command.expr);
			return;
		}
		case Node::Kind::Coproc: {
			const auto& coproc = static_cast<const CoprocCommand&>(node);
			const std::string label = "Coproc " + coproc.name;
			dumpExpandedBody(dump, label.c_str(), coproc.command, depth);
			return;
		}
		}
	}

	static void printHeader(bool color, const char* title) {
		if (color) std::cout << "\x1b[35;1m── " << title << " ──\x1b[0m\n";
		else       std::cout << "--- " << title << " ---\n";
	}

	static ErrorColors errorColors(bool color) {
		if (!color) return { "", "", "" };
		return { "\x1b[31;1m", "\x1b[33m", "\x1b[0m" };
	}

	static void printDiagnostic(const ErrorColors& colors, const char* kind,
	                            const SourceLoc& loc, const std::string& message) {
		std::cerr << colors.kind << kind << colors.reset << " "
			<< colors.location << loc.line << ":" << loc.column << colors.reset
			<< ": " << message << "\n";
	}

	static void applyScriptArguments(Environment& env, const ScriptRun& run) {
		if (!run.script_name.empty()) env.setShellName(run.script_name);
		env.setPositional(run.positional);
	}

	static void dumpExpansions(NodePtr root, bool header_color, const ScriptRun& run) {
		Environment env;
		prepareEnv(env);
		applyScriptArguments(env, run);
		Expander expander(env, /*sub=*/nullptr);
		printHeader(header_color, "EXPANSIONS");

		ExpandDump dump{ expander, env, std::cout };
		dumpExpanded(dump, *root, 0);
	}

	static void printSyntaxErrors(const Lexer& lex, const Parser& parser) {
		const ErrorColors colors = errorColors(stderrIsTty());
		for (const auto& e : lex.errors()) printDiagnostic(colors, "lex error", e.loc, e.message);
		for (const auto& e : parser.errors()) printDiagnostic(colors, "parse error", e.loc, e.message);
	}

	// A script that runs reports its first syntax error as bash does, by
	// script name and line; the dump modes keep line:column and every error
	// for whoever reads them.
	static void printRunSyntaxErrors(const Lexer& lex, const Parser& parser,
	                                 const ScriptRun& run) {
		ParseError error;
		if (!firstSyntaxError(lex.errors(), parser.errors(), error)) return;

		const std::string input = run.command_string ? "-c" : std::string();
		for (const LexError& warning : lex.warnings()) {
			if (warning.loc.line > error.loc.line) continue;
			std::cerr << syntaxWarningReport(input, warning.loc.line, warning.message);
		}

		std::cerr << syntaxErrorReport(input, run.source, run.first_line, error.loc.line,
			error.message);

		ParseError follow_up;
		if (!followUpSyntaxError(parser.errors(), error, follow_up)) return;

		std::cerr << syntaxErrorReport(input, run.source, run.first_line, follow_up.loc.line,
			follow_up.message);
	}

	static bool hasSyntaxErrors(const Lexer& lex, const Parser& parser) {
		return !lex.errors().empty() || !parser.errors().empty();
	}

	// False when a startup file already ended the shell with `exit`.
	static bool runStartup(Executor& exec, const ScriptRun& run, bool child_shell,
			int& out_status) {
		applyStartupOptions(exec, run.startup);
		sourceLoginProfile(exec, run.startup);
		if (run.read_rc) sourceRcFile(exec, run.startup);
		else if (!child_shell) sourceBashEnv(exec);

		return !exec.consumeFlow(FlowSignal::Kind::Exit, &out_status);
	}

	// bash reads on after an error that only discards the command it sits
	// in; that command fails.
	static std::string scriptOrigin(const ScriptRun& run) {
		return run.command_string ? "-c" : std::string();
	}

	static int resumeScript(Executor& exec, const ScriptRun& run, std::size_t resume) {
		exec.setLastStatus(1);
		if (resume >= run.source.size()) return 1;

		const auto skipped_lines = std::count(run.source.begin(),
			run.source.begin() + static_cast<std::ptrdiff_t>(resume), '\n');
		const std::size_t line = run.first_line + static_cast<std::size_t>(skipped_lines);
		return exec.executeInputText(run.source.substr(resume), scriptOrigin(run), line);
	}

	// Like bash, the commands on the lines before a syntax error run
	// first; the error is reported, and the script ends with status 2,
	// only when execution reaches it.
	static int runParsedScript(Executor& exec, NodePtr root, const ScriptRun& run,
	                           const Lexer& lex, const Parser& parser) {
		truncateAtSyntaxError(*root, parser, firstSyntaxErrorOffset(lex.errors(), parser.errors()));
		const int status = exec.executeInput(*root, run.source);
		if (exec.relexPending()) {
			return exec.relexRestOfInput(run.source,
				{ scriptOrigin(run), run.first_line, lex.source(), lex.aliasSplices() }, status);
		}

		if (exec.flowPending() || !hasSyntaxErrors(lex, parser)) return status;

		printRunSyntaxErrors(lex, parser, run);
		const std::size_t resume = discardResumeOffset(run.source, lex.errors(), parser.errors());
		if (resume == std::string::npos || exec.env().options().posix) return kSyntaxErrorStatus;

		return resumeScript(exec, run, resume);
	}

	static bool stdinIsSeekableFile() {
		const HANDLE input = ::GetStdHandle(STD_INPUT_HANDLE);
		if (input == INVALID_HANDLE_VALUE || ::GetFileType(input) != FILE_TYPE_DISK) return false;

		return _lseeki64(0, 0, SEEK_CUR) >= 0;
	}

	static void refillFromStdinPosition(StdinScript& script) {
		script.base = _lseeki64(0, 0, SEEK_CUR);
		script.buffer.clear();
		script.consumed = 0;

		char chunk[65536];
		int got = 0;
		while ((got = _read(0, chunk, sizeof chunk)) > 0) {
			script.buffer.append(chunk, static_cast<std::size_t>(got));
		}
	}

	static StdinScript openStdinScript() {
		StdinScript script;
		script.seekable = stdinIsSeekableFile();
		if (script.seekable) refillFromStdinPosition(script);
		return script;
	}

	static bool readBufferedLine(StdinScript& script, std::string& out_line) {
		if (script.consumed >= script.buffer.size()) return false;

		const std::size_t newline = script.buffer.find('\n', script.consumed);
		const std::size_t end = newline == std::string::npos ? script.buffer.size() : newline + 1;
		out_line.assign(script.buffer, script.consumed, end - script.consumed);
		script.consumed = end;
		return true;
	}

	static bool readPipeLine(std::string& out_line) {
		out_line.clear();
		char byte = 0;
		while (_read(0, &byte, 1) == 1) {
			out_line.push_back(byte);
			if (byte == '\n') return true;
		}

		return !out_line.empty();
	}

	static bool readScriptLine(StdinScript& script, std::string& out_line) {
		const bool got = script.seekable
			? readBufferedLine(script, out_line)
			: readPipeLine(out_line);
		if (!got) return false;

		if (script.at_start) stripUtf8Bom(out_line);
		script.at_start = false;
		normalizeCrlf(out_line);
		return true;
	}

	static bool endsInLineContinuation(const std::string& text) {
		std::size_t end = text.size();
		if (end > 0 && text[end - 1] == '\n') --end;

		std::size_t backslashes = 0;
		while (backslashes < end && text[end - 1 - backslashes] == '\\') ++backslashes;
		return backslashes % 2 == 1;
	}

	static bool lexerWantsMoreInput(const Lexer& lexer) {
		for (const LexError& error : lexer.errors()) {
			if (error.message.find("unexpected EOF") != std::string::npos) return true;
			if (error.message.find("unterminated") != std::string::npos) return true;
		}

		return false;
	}

	// More input can mend only an error at the very end of the text; any
	// other one ends the script once the commands before it have run.
	static Completeness judgeCommandText(const std::string& text) {
		if (endsInLineContinuation(text)) return Completeness::Incomplete;

		Lexer lexer(text);
		std::vector<Token> tokens = lexer.tokenize();
		if (lexerWantsMoreInput(lexer)) return Completeness::Incomplete;

		Parser parser(std::move(tokens), text);
		parser.parseProgram();
		ParseError error;
		const bool has_error = firstSyntaxError(lexer.errors(), parser.errors(), error);
		if (!has_error) return Completeness::Complete;
		if (error.loc.offset >= text.size()) return Completeness::Incomplete;

		return error.discards_command ? Completeness::Complete : Completeness::Fatal;
	}

	static bool takeTextLine(const std::string& text, std::size_t& pos, std::string& line) {
		if (pos >= text.size()) return false;

		const std::size_t newline = text.find('\n', pos);
		const std::size_t end = newline == std::string::npos ? text.size() : newline + 1;
		line = text.substr(pos, end - pos);
		pos = end;
		return true;
	}

	static std::string withoutNewline(const std::string& line) {
		return !line.empty() && line.back() == '\n' ? line.substr(0, line.size() - 1) : line;
	}

	// As bash, the line is history-expanded as it is read while the history
	// option is on: the result goes to stderr, and a failed expansion drops
	// the line.
	static void recordScriptHistory(Executor& exec, const std::string& entry);

	static bool expandScriptLine(Executor& exec, std::string& line, HistorySubstitution& last) {
		const SetOptions& options = exec.env().options();
		if (!options.history || !options.histexpand) return true;

		const bool newline = !line.empty() && line.back() == '\n';
		const HistoryExpansion expansion =
			expandHistoryLine(withoutNewline(line), exec.history(), '\0', last, options.posix);
		switch (expansion.kind) {
			case HistoryExpansion::Kind::kExpanded:
				std::fprintf(stderr, "%s\n", expansion.text.c_str());
				line = expansion.text + (newline ? "\n" : "");
				return true;
			case HistoryExpansion::Kind::kPrintOnly:
				std::fprintf(stderr, "%s\n", expansion.text.c_str());
				recordScriptHistory(exec, expansion.text);
				return false;
			case HistoryExpansion::Kind::kFailed:
				shellErrorf("%s\n", expansion.text.c_str());
				return false;
			default:
				return true;
		}
	}

	static bool historyTakes(Executor& exec, const std::string& entry) {
		const Environment& env = exec.env();
		if (!env.options().history || entry.empty()) return false;

		const HistoryFilter filter = { env.get("HISTCONTROL"), env.get("HISTIGNORE") };
		const auto& history = exec.history();
		return historyAccepts(filter, entry, history.empty() ? std::string() : history.back());
	}

	// fc and history leave out the command that runs them when it is the
	// newest entry.
	static void recordScriptHistory(Executor& exec, const std::string& entry) {
		const bool recorded = historyTakes(exec, entry);
		exec.setCurrentCommandInHistory(recorded);
		if (!recorded) return;

		const HistoryFilter filter = { exec.env().get("HISTCONTROL"), std::string() };
		if (historyErasesDuplicates(filter)) exec.eraseHistoryEntries(entry);

		exec.addHistoryEntry(entry);
	}

	// bash's entry for a command with a here-document ends in a newline.
	static bool opensHeredoc(const std::string& text) {
		Lexer lexer(text);
		for (const Token& token : lexer.tokenize()) {
			if (token.kind == TokKind::DLess || token.kind == TokKind::DLessDash) return true;
		}

		return false;
	}

	struct ScriptCommand {
		std::string text;
		std::string entry;
		std::size_t lines = 0;
		bool dropped = false;
	};

	static Completeness judgeCommandText(const std::string& text);

	// The text ends inside a here-document's body.
	static bool heredocPending(const std::string& text) {
		if (text.empty()) return false;

		Lexer lexer(text);
		lexer.tokenize();
		const auto mentions = [](const LexError& error) {
			return error.message.find("delimited by end-of-file") != std::string::npos
				|| error.message.find("unterminated heredoc") != std::string::npos;
		};
		return std::any_of(lexer.warnings().begin(), lexer.warnings().end(), mentions)
			|| std::any_of(lexer.errors().begin(), lexer.errors().end(), mentions);
	}

	// The lines of one command, each expanded and joined into its history
	// entry as bash reads them; a here-document's body is not expanded.
	static Completeness readScriptCommand(Executor& exec, const std::string& text, std::size_t& pos,
			std::size_t first_line, HistorySubstitution& last, ScriptCommand& command) {
		std::string pending;
		std::string line;
		Completeness completeness = Completeness::Incomplete;
		while (completeness == Completeness::Incomplete && takeTextLine(text, pos, line)) {
			exec.env().setCurrentLineno(static_cast<int>(first_line + command.lines));
			++command.lines;
			const bool body = heredocPending(command.text);
			if (!body && !expandScriptLine(exec, line, last)) {
				command.dropped = true;
				return Completeness::Complete;
			}

			appendHistoryLine(command.entry, pending, withoutNewline(line));
			pending += (pending.empty() ? "" : "\n") + withoutNewline(line);
			command.text += line;
			completeness = heredocPending(command.text) ? Completeness::Incomplete
				: judgeCommandText(command.text);
		}

		return completeness;
	}

	// Once a script turns the history option on it is read a command at a
	// time, so each line can be recorded and expanded before it runs.
	static int runScriptLines(Executor& exec, const std::string& text, const std::string& origin,
			std::size_t first_line) {
		std::size_t pos = 0;
		std::size_t line = first_line;
		int status = exec.lastStatus();
		HistorySubstitution last;
		while (pos < text.size()) {
			ScriptCommand command;
			const Completeness completeness = readScriptCommand(exec, text, pos, line, last, command);
			if (!command.dropped) {
				if (opensHeredoc(command.text)) command.entry += '\n';
				recordScriptHistory(exec, command.entry);
				status = exec.executeInputText(command.text, origin, line);
			}

			line += command.lines;
			if (exec.flowPending() || completeness == Completeness::Fatal) return status;
		}

		return status;
	}

	static const std::size_t kLineByLineLimit = 64;

	// Past kLineByLineLimit lines a command is judged at doubling lengths
	// only, so a long function costs a few parses rather than one per line.
	static Completeness readCommandText(StdinScript& script, std::string& out_text) {
		out_text.clear();
		std::size_t lines = 0;
		std::size_t next_judgement = 1;
		std::string line;
		while (readScriptLine(script, line)) {
			out_text += line;
			if (++lines < next_judgement) continue;

			const Completeness completeness = judgeCommandText(out_text);
			if (completeness != Completeness::Incomplete) return completeness;
			next_judgement = lines < kLineByLineLimit ? lines + 1 : lines * 2;
		}

		return out_text.empty() ? Completeness::EndOfInput : Completeness::Fatal;
	}

	static long long stdinScriptPosition(const StdinScript& script) {
		return script.base + static_cast<long long>(script.consumed);
	}

	static void syncStdinBeforeCommand(const StdinScript& script) {
		if (script.seekable) _lseeki64(0, stdinScriptPosition(script), SEEK_SET);
	}

	// A command that read standard input moved the descriptor; the script
	// goes on from wherever it left it.
	static void followStdinAfterCommand(StdinScript& script) {
		if (!script.seekable) return;

		const bool moved = _lseeki64(0, 0, SEEK_CUR) != stdinScriptPosition(script);
		if (moved) refillFromStdinPosition(script);
	}

	static int runStdinScript(Executor& exec, const ScriptRun& run) {
		StdinScript script = openStdinScript();
		std::size_t line = run.first_line;
		int status = 0;
		std::string text;
		for (;;) {
			const Completeness completeness = readCommandText(script, text);
			if (completeness == Completeness::EndOfInput) return status;

			syncStdinBeforeCommand(script);
			status = exec.executeInputText(text, std::string(), line);
			if (exec.flowPending() || completeness == Completeness::Fatal) return status;

			line += static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
			followStdinAfterCommand(script);
		}
	}

	// read_rc is `-i`, which makes even a script or `-c` shell interactive
	// as in bash: `$-` holds `i` and aliases expand, while the commands still
	// come from the script. bash leaves history expansion off for a script.
	static void applyInteractiveFlag(Environment& env, const ScriptRun& run) {
		env.setInteractive(run.read_rc);
		if (run.from_file) env.options().histexpand = false;
	}

	static int executeProgram(NodePtr root, const ScriptRun& run, const Lexer& lex,
	                          const Parser& parser) {
		Environment env;
		prepareEnv(env);
		applyScriptArguments(env, run);
		applyInteractiveFlag(env, run);
		const DiagnosticsBinding diagnostics(env);

		Executor exec(env);
		exec.setSourceText(run.source);
		const bool child_shell = isChildShell(env);
		if (!child_shell) exec.setLineRunner(runScriptLines);
		const bool inherited_stage = !env.get("WBSH_SUBSHELL_LEVEL").empty();
		absorbInheritedState(env, exec);
		if (inherited_stage) exec.enterInheritedStage(*root);
		if (!child_shell) env.setInvocationLetter(run.invocation_letter);

		int status = 0;
		if (!runStartup(exec, run, child_shell, status)) return exec.fireExitTrap(status);

		if (run.from_file) exec.beginScriptFrame(run.script_name);

		status = run.stream_stdin ? runStdinScript(exec, run)
			: runParsedScript(exec, root, run, lex, parser);
		exec.consumeFlow(FlowSignal::Kind::Exit, &status);

		exec.endStoppedJobs();
		return exec.fireExitTrap(status);
	}

	static void showDumps(const ScriptRun& run, const std::vector<Token>& tokens, NodePtr root) {
		const bool out_color = stdoutIsTty();
		if (run.show_tokens) {
			printHeader(out_color, "TOKENS");
			dumpTokens(std::cout, tokens);
		}

		if (run.show_ast) {
			printHeader(out_color, "AST");
			if (root != nullptr) dumpAst(std::cout, *root);
			else std::cout << "(empty)\n";
		}

		if (run.show_expansions && root != nullptr) dumpExpansions(root, out_color, run);
		if (run.pretty_print && root != nullptr) std::cout << prettyPrintedProgram(*root, run.source);
	}

	// The last `-o posix` / `+o posix` of the invocation wins over the
	// environment, as it is applied after POSIXLY_CORRECT is imported.
	static bool startsInPosixMode(const ShellStartup& startup) {
		bool posix = std::getenv("POSIXLY_CORRECT") != nullptr;
		const std::vector<std::string>& words = startup.set_words;
		for (std::size_t k = 0; k + 1 < words.size(); ++k) {
			if (words[k + 1] != "posix") continue;
			if (words[k] == "-o") posix = true;
			if (words[k] == "+o") posix = false;
		}

		return posix;
	}

	int runScript(const ScriptRun& run) {
		Lexer lex(run.source);
		lex.startAtLine(run.first_line);
		lex.lexInPosixMode(startsInPosixMode(run.startup));
		lex.acceptHeredocAtEof();
		auto tokens = lex.tokenize();
		const std::vector<Token> dumped_tokens = run.show_tokens ? tokens : std::vector<Token>{};

		Parser parser(std::move(tokens), run.source);
		auto root = parser.parseProgram();
		showDumps(run, dumped_tokens, root);

		if (run.execute && root != nullptr) return executeProgram(root, run, lex, parser);

		printSyntaxErrors(lex, parser);
		return hasSyntaxErrors(lex, parser) ? 1 : 0;
	}

}  // namespace wbsh

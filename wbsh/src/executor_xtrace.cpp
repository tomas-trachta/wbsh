/**
 * @file executor_xtrace.cpp
 * @brief `set -x` lines for assignments, `for` / `select` / `case` headers
 *        and `(( ))`, and the BASH_XTRACEFD stream they go to.
 */

#include "executor.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <io.h>
#else
#  include <fcntl.h>
#  include <unistd.h>
#endif /* _WIN32 */

#include "diagnostics.h"
#include "lexer.h"
#include "numparse.h"
#include "prompt.h"
#include "utf8.h"

namespace wbsh {

	namespace xtrace_detail {

		static bool parseTraceFd(const std::string& text, int& fd) {
			std::size_t consumed = 0;
			if (!parseInt(text, fd, 10, &consumed)) return false;
			return consumed == text.size() && fd >= 0;
		}

		static bool isOpenFd(int fd) {
#ifdef _WIN32
			return ::_get_osfhandle(fd) != -1;
#else
			return ::fcntl(fd, F_GETFD) != -1;
#endif /* _WIN32 */
		}

		static void writeToFd(int fd, const std::string& line) {
			std::fflush(stdout);
			std::fflush(stderr);
#ifdef _WIN32
			(void)::_write(fd, line.data(), static_cast<unsigned>(line.size()));
#else
			(void)::write(fd, line.data(), line.size());
#endif /* _WIN32 */
		}

		static void writeTraceLine(const Environment& env, const std::string& line) {
			int fd = -1;
			if (parseTraceFd(env.get("BASH_XTRACEFD"), fd) && fd != 2 && isOpenFd(fd)) {
				writeToFd(fd, line);
				return;
			}

			std::fputs(line.c_str(), stderr);
			std::fflush(stderr);
		}

		static std::size_t ansiCEnd(const std::string& raw, std::size_t body) {
			std::size_t i = body;
			while (i < raw.size() && raw[i] != '\'') i += raw[i] == '\\' ? 2 : 1;
			return (std::min)(i, raw.size());
		}

		static std::size_t quotedEnd(const std::string& raw, std::size_t open) {
			const char quote = raw[open];
			std::size_t i = open + 1;
			while (i < raw.size() && raw[i] != quote) i += (quote == '"' && raw[i] == '\\') ? 2 : 1;
			return (std::min)(i + 1, raw.size());
		}

		// bash turns `$'...'` into a plain quoted string as it reads the
		// word, so the trace of an array literal shows it that way.
		static std::string withAnsiCDecoded(const std::string& raw, Expander& expander) {
			std::string out;
			std::size_t i = 0;
			while (i < raw.size()) {
				const char c = raw[i];
				if (c == '$' && i + 1 < raw.size() && raw[i + 1] == '\'') {
					const std::size_t end = ansiCEnd(raw, i + 2);
					out += "'" + expander.ansiCText(raw.substr(i + 2, end - i - 2)) + "'";
					i = end + 1;
					continue;
				}

				const std::size_t next = (c == '\'' || c == '"') ? quotedEnd(raw, i)
					: (std::min)(i + (c == '\\' ? 2 : 1), raw.size());
				out.append(raw, i, next - i);
				i = next;
			}

			return out;
		}

		static std::string arrayItemsText(const Assignment& assignment, Expander& expander) {
			std::string text;
			for (const Assignment::Keyed& item : assignment.keyed_items) {
				if (!text.empty()) text.push_back(' ');
				text += withAnsiCDecoded(item.value.raw, expander);
			}

			return text;
		}

		static std::string traceValue(const std::string& value) {
			return value.empty() ? value : shellQuoteForReuse(value);
		}

		static bool assignedValue(const Executor::SimpleCmdAssigns& expanded, std::string& value) {
			if (!expanded.scalar.empty()) value = expanded.scalar.front().value;
			else if (!expanded.elem.empty()) value = expanded.elem.front().value;
			else return false;
			return true;
		}

		static std::string loopWordsText(const ForClause& clause) {
			if (!clause.has_in) return "\"$@\"";

			std::string text;
			for (const Word& word : clause.items) {
				if (!text.empty()) text.push_back(' ');
				text += word.raw;
			}

			return text;
		}

		static std::string withoutLeadingBlanks(const std::string& text) {
			const std::size_t first = text.find_first_not_of(" \t\n");
			return first == std::string::npos ? std::string() : text.substr(first);
		}

	}  // namespace xtrace_detail

	std::string Executor::expandedPs4() {
		const std::string ps4 = env_.get("PS4");
		if (ps4.empty()) return ps4;

		env_.setXtrace(false);
		const std::string expanded = expandPromptString(*this, ps4, false);
		env_.setXtrace(true);
		return expanded;
	}

	// bash repeats PS4's first character once per level of eval, source,
	// trap action and command substitution the command runs in.
	std::string Executor::traceIndent() {
		const std::string ps4 = expandedPs4();
		if (ps4.empty()) return ps4;

		const std::size_t first = env_.utf8Locale() ? utf8CharLength(ps4, 0) : 1;
		std::string indent;
		for (int level = 0; level < xtrace_level_; ++level) indent += ps4.substr(0, first);
		return indent + ps4.substr(first);
	}

	void Executor::traceText(const std::string& text) {
		if (!env_.xtrace()) return;

		xtrace_detail::writeTraceLine(env_, traceIndent() + text + "\n");
	}

	void Executor::traceAssignment(const Assignment& assignment,
			const SimpleCmdAssigns& expanded) {
		if (!env_.xtrace()) return;

		const std::string op = assignment.append ? "+=" : "=";
		if (assignment.is_array) {
			traceText(assignment.name + op + "("
				+ xtrace_detail::arrayItemsText(assignment, expander_) + ")");
			return;
		}

		std::string value;
		if (!xtrace_detail::assignedValue(expanded, value)) return;

		const std::string subscript = assignment.has_subscript
			? "[" + assignment.subscript.raw + "]" : std::string();
		traceText(assignment.name + subscript + op + xtrace_detail::traceValue(value));
	}

	void Executor::traceConditional(const std::vector<std::string>& parts) {
		if (!env_.xtrace()) return;

		std::string text = "[[";
		for (const std::string& part : parts) text += " " + (part.empty() ? "''" : part);
		traceText(text + " ]]");
	}

	void Executor::traceLoopHeader(const char* keyword, const ForClause& clause) {
		if (!env_.xtrace()) return;

		traceText(std::string(keyword) + " " + clause.var + " in "
			+ xtrace_detail::loopWordsText(clause));
	}

	void Executor::traceCaseHeader(const CaseClause& clause) {
		if (!env_.xtrace()) return;

		traceText("case " + clause.subject.raw + " in");
	}

	// The parts of `for (( ; ; ))` lose their leading blanks, an empty part
	// counts as 1, and `(( ))` keeps its text as written.
	bool Executor::evalTracedArith(const std::string& expr, bool loop_part, long long& value) {
		if (expr.empty() && loop_part) {
			traceText("(( 1 ))");
			value = 1;
			return true;
		}

		value = expander_.evalArith(Lexer::withoutArithmeticDoubleQuotes(expr));
		if (env_.xtrace()) {
			const std::string& text = expander_.lastArithText();
			const std::string shown = loop_part ? xtrace_detail::withoutLeadingBlanks(text) : text;
			traceText("(( " + shown + " ))");
		}

		if (!expander_.failed()) return true;

		const char* label = expander_.errorLabel("((: ");
		shellErrorf("%s%s\n", label, expander_.takeError().c_str());
		return false;
	}

	void Executor::checkXtraceFdAssignment(const SimpleCmdAssigns& expanded) {
		for (const ScalarAssign& scalar : expanded.scalar) {
			if (scalar.name != "BASH_XTRACEFD" || scalar.value.empty()) continue;

			int fd = -1;
			const bool valid = xtrace_detail::parseTraceFd(scalar.value, fd)
				&& xtrace_detail::isOpenFd(fd);
			if (valid) continue;

			shellErrorf("BASH_XTRACEFD: %s: invalid value for trace file descriptor\n",
				scalar.value.c_str());
		}
	}

}  // namespace wbsh

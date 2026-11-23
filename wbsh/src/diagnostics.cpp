/**
 * @file diagnostics.cpp
 * @brief The prefix of the shell's own error messages, as bash writes it:
 *        `NAME: line N: ` in a script or `-c` string, `NAME: ` at the prompt.
 */

#include "diagnostics.h"

#include <cstdarg>
#include <cstdio>
#include <string>

#include "environment.h"

namespace wbsh {

	static const char* const kDefaultName = "wbsh";

	static const Environment* g_diagnostic_env = nullptr;

	void bindDiagnostics(const Environment* env) {
		g_diagnostic_env = env;
	}

	static std::string runningFile(const Environment& env) {
		const Environment::IndexedArray* sources = env.getIndexedArray("BASH_SOURCE");
		if (sources == nullptr) return std::string();

		const auto first = sources->find(0);
		return first == sources->end() ? std::string() : first->second;
	}

	static std::string errorName() {
		if (g_diagnostic_env == nullptr) return kDefaultName;

		const Environment& env = *g_diagnostic_env;
		if (env.interactive()) return env.shellName();

		const std::string file = runningFile(env);
		return file.empty() ? env.shellName() : file;
	}

	static bool reportsLines() {
		return g_diagnostic_env != nullptr && !g_diagnostic_env->interactive();
	}

	static int errorLine() {
		return reportsLines() ? g_diagnostic_env->currentLineno() : 0;
	}

	static std::string prefixAt(int line) {
		const std::string name = errorName();
		if (line <= 0) return name + ": ";

		return name + ": line " + std::to_string(line) + ": ";
	}

	std::string errorPrefix() {
		return prefixAt(errorLine());
	}

	std::string scriptNamePrefix() {
		if (g_diagnostic_env == nullptr) return std::string(kDefaultName) + ": ";

		return g_diagnostic_env->shellName() + ": ";
	}

	std::string syntaxErrorPrefix(const std::string& input, std::size_t line) {
		const std::string name = errorName();
		const std::string where = "line " + std::to_string(line) + ": ";
		if (input.empty() || input == name) return name + ": " + where;

		return name + ": " + input + ": " + where;
	}

	static std::string sourceLine(const std::string& source, std::size_t index) {
		std::size_t start = 0;
		for (std::size_t i = 0; i < index; ++i) {
			const std::size_t newline = source.find('\n', start);
			if (newline == std::string::npos) return std::string();

			start = newline + 1;
		}

		const std::size_t end = source.find('\n', start);
		return source.substr(start, end == std::string::npos ? std::string::npos : end - start);
	}

	static std::string prefixedLines(const std::string& prefix, const std::string& message) {
		std::string report;
		std::size_t start = 0;
		while (true) {
			const std::size_t end = message.find('\n', start);
			report += prefix + message.substr(start, end - start) + "\n";
			if (end == std::string::npos) return report;

			start = end + 1;
		}
	}

	// bash quotes the line back after "syntax error near ...", the last
	// line of a report.
	static bool echoesSourceLine(const std::string& message) {
		const std::size_t last_newline = message.rfind('\n');
		const std::size_t last_line = last_newline == std::string::npos ? 0 : last_newline + 1;
		return message.compare(last_line, 18, "syntax error near ") == 0;
	}

	std::string syntaxErrorReport(const std::string& input, const std::string& source,
	                              std::size_t first_line, std::size_t line,
	                              const std::string& message) {
		const std::string prefix = syntaxErrorPrefix(input, line);
		std::string report = prefixedLines(prefix, message);
		if (!echoesSourceLine(message) || line < first_line) return report;

		report += prefix + "`" + sourceLine(source, line - first_line) + "'\n";
		return report;
	}

	std::string syntaxWarningReport(const std::string& input, std::size_t line,
	                                const std::string& message) {
		return syntaxErrorPrefix(input, line) + "warning: " + message + "\n";
	}

	// What a builtin printed before goes out first, as in bash, where both
	// often share one file.
	static void writeStderr(const std::string& text) {
		std::fflush(stdout);
		std::fwrite(text.data(), 1, text.size(), stderr);
	}

	void shellWarningAt(std::size_t line, const std::string& message) {
		const int shown = reportsLines() ? static_cast<int>(line) : 0;
		writeStderr(prefixAt(shown) + "warning: " + message + "\n");
	}

	void shellError(const std::string& message) {
		writeStderr(errorPrefix() + message + "\n");
	}

	void shellErrorWithoutLine(const std::string& message) {
		writeStderr(errorName() + ": " + message + "\n");
	}

	static std::string formatMessage(const char* format, std::va_list args) {
		std::va_list measure;
		va_copy(measure, args);
		const int size = std::vsnprintf(nullptr, 0, format, measure);
		va_end(measure);
		if (size <= 0) return std::string();

		std::string message(static_cast<std::size_t>(size), '\0');
		std::vsnprintf(&message[0], message.size() + 1, format, args);
		return message;
	}

	// The format carries its own trailing newline, as the fprintf calls
	// it replaces did.
	void shellErrorf(const char* format, ...) {
		std::va_list args;
		va_start(args, format);
		const std::string message = formatMessage(format, args);
		va_end(args);

		writeStderr(errorPrefix() + message);
	}

}  // namespace wbsh

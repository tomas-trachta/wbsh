#pragma once

/**
 * @file diagnostics.h
 * @brief The prefix of the shell's own error messages, as bash writes it:
 *        `NAME: line N: ` in a script or `-c` string, `NAME: ` at the prompt.
 */

#include <string>

namespace wbsh {

	class Environment;

	/// The Environment whose `$0`, BASH_SOURCE and LINENO say where an error
	/// happened; nullptr leaves the plain `wbsh: ` prefix.
	void bindDiagnostics(const Environment* env);

	/// Binds @p env for as long as the scope lives.
	class DiagnosticsBinding {
	public:
		explicit DiagnosticsBinding(const Environment& env) {
			bindDiagnostics(&env);
		}

		~DiagnosticsBinding() {
			bindDiagnostics(nullptr);
		}

		DiagnosticsBinding(const DiagnosticsBinding&) = delete;
		DiagnosticsBinding& operator=(const DiagnosticsBinding&) = delete;
	};

	/// NAME is the file running (BASH_SOURCE[0], else `$0`); an interactive
	/// shell, or one that has not started a line yet, leaves out `line N: `.
	std::string errorPrefix();

	/// `$0: `, which getopts puts before its messages instead.
	std::string scriptNamePrefix();

	/// The prefix for a syntax error at @p line of @p input: bash names the
	/// input (`eval`, `-c`) too when it is not the file the name already says.
	std::string syntaxErrorPrefix(const std::string& input, std::size_t line);

	/// A syntax error the way bash reports it, one or two lines: the
	/// message, then for an unexpected token the line of @p source it is on.
	/// @p first_line is the number of the first line of @p source.
	std::string syntaxErrorReport(const std::string& input, const std::string& source,
	                              std::size_t first_line, std::size_t line,
	                              const std::string& message);

	/// `NAME: line N: warning: MESSAGE` for @p line.
	void shellWarningAt(std::size_t line, const std::string& message);

	void shellError(const std::string& message);

	void shellErrorf(const char* format, ...);

}  // namespace wbsh

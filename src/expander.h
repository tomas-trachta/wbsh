#pragma once

#include "ast.h"
#include "environment.h"
#include "pathconv.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace wbsh {

	// Pluggable command-substitution callback. The executor implements this;
	// tests can stub it to return canned strings. When null, $(...) and
	// `...` expand to empty.
	struct CommandSubstitutor {
		virtual ~CommandSubstitutor() = default;
		// Default `run` is for $(...) — trailing newlines stripped.
		virtual std::string run(const std::string& body) = 0;
		// `runRaw` is for <(...) — preserves the inner command's stdout
		// byte-for-byte. Defaults to `run` for substitutors that don't
		// distinguish.
		virtual std::string runRaw(const std::string& body) { return run(body); }
	};

	struct ExpandError : std::runtime_error {
		using std::runtime_error::runtime_error;
	};

	class Expander {
	public:
		Expander(Environment& env, CommandSubstitutor* sub = nullptr);

		// For command words. Performs brace, tilde, parameter, command,
		// arithmetic, ANSI-C, word splitting, pathname expansion, and
		// quote removal.
		std::vector<std::string> expandWord(const Word& w);

		// Same pipeline minus brace expansion — used internally by the
		// brace-expansion driver for each generated alternative.
		std::vector<std::string> expandWordPostBrace(const Word& w);

		// For assignment values, redirection targets, case subjects, etc.
		// Same pipeline minus word splitting and pathname expansion.
		std::string expandStringValue(const Word& w);

		// Heredoc body expansion. If quoted, returned verbatim. Otherwise
		// $-substitutions and backquotes are processed inline.
		std::string expandHeredoc(const std::string& body, bool quoted);

		// Arithmetic expression evaluation.
		long long evalArith(const std::string& body);

		// Pending temp files created by `<(...)` substitutions. We hand out
		// scoped views so that nested SimpleCommands (driven by command
		// substitution / process substitution) only see files they
		// themselves produced — without this they'd delete the outer
		// caller's files on the way out.
		std::size_t pendingTempFileWatermark() const {
			return pending_temp_files_.size();
		}
		std::vector<std::string> drainTempFilesSince(std::size_t watermark) {
			std::vector<std::string> out;
			if (watermark < pending_temp_files_.size()) {
				out.assign(pending_temp_files_.begin() + watermark,
					pending_temp_files_.end());
				pending_temp_files_.resize(watermark);
			}
			return out;
		}

	private:
		// Tagged text used internally during expansion. Each character
		// carries flags describing its provenance.
		static constexpr std::uint8_t F_QUOTED = 1u << 0;

		struct Tagged {
			std::string text;
			std::vector<std::uint8_t> flags;
			bool had_quote = false;     // ever entered a quoted segment in source
			std::size_t size() const { return text.size(); }
			void push(char c, std::uint8_t f) {
				text.push_back(c);
				flags.push_back(f);
			}
		};

		Environment& env_;
		CommandSubstitutor* sub_;
		PathConv path_conv_;
		std::vector<std::string> pending_temp_files_;

		// ---- Rendering ----
		Tagged renderWord(const Word& w);
		void   renderSegment(const WordSegment& s, Tagged& out, bool inside_dq);

		// ---- Parameter / command / arith ----
		std::string lookupParam(const std::string& name);
		// Look up name[subscript] as a scalar string. Supports numeric
		// subscripts (indexed arrays, where the subscript is an arithmetic
		// expression), `@`/`*` (joined elements), and assoc-array string
		// keys. Returns empty for unknown names/keys.
		std::string lookupSubscripted(const std::string& name,
		                              const std::string& subscript,
		                              bool star_join_ifs);
		// Count of elements: ${#name[@]} / ${#name[*]}.
		std::size_t arrayLength(const std::string& name) const;
		// Indices / keys: ${!name[@]}.
		std::vector<std::string> arrayKeys(const std::string& name) const;
		// All values, in iteration order. Used by renderSegment for
		// field-aware emission of `${name[@]}`.
		std::vector<std::string> arrayValues(const std::string& name) const;
		std::string expandParam(const std::string& body, bool quoted_ctx);
		std::string runCmdSubst(const std::string& body);
		std::string interpretAnsiC(const std::string& body);

		bool        isSpecialParam1(char c) const;

		// ---- Tilde ----
		Word applyTildeExpansion(const Word& w);

		// ---- Splitting / globbing / quote removal ----
		std::vector<Tagged> splitWords(const Tagged& t);
		std::vector<std::string> globExpand(const Tagged& t);
		std::string quoteRemove(const Tagged& t);

		// ---- Helpers for ${...} ----
		std::string substringExpand(const std::string& val, const std::string& args);
		std::string stripPrefix(const std::string& val, const std::string& pat, bool greedy);
		std::string stripSuffix(const std::string& val, const std::string& pat, bool greedy);
		std::string replacePattern(const std::string& val, const std::string& pat,
		                           const std::string& rep, bool all,
		                           bool anchor_start, bool anchor_end);
	};

}  // namespace wbsh

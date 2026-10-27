#pragma once

/**
 * @file expander.h
 * @brief Word-expansion pipeline.
 *
 * Implements the POSIX expansion sequence — brace, tilde, parameter,
 * command, arithmetic, and ANSI-C expansion, then word splitting,
 * pathname expansion, and quote removal — used by the executor every
 * time a Word is materialised into one or more argv strings.
 *
 * Command and process substitution are dispatched through the
 * pluggable CommandSubstitutor callback so tests can stub them.
 */

#include "ast.h"
#include "environment.h"
#include "pathconv.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace wbsh {

	/**
	 * @brief Pluggable command-substitution callback.
	 *
	 * The executor implements this; tests can stub it to return
	 * canned strings. When the Expander is constructed with a null
	 * substitutor, `$(...)` and backquotes expand to the empty string.
	 */
	struct CommandSubstitutor {
		virtual ~CommandSubstitutor() = default;
		/**
		 * @brief Run @p body and return its captured stdout.
		 *
		 * Used for `$(...)` — implementations strip trailing newlines.
		 */
		virtual std::string run(const std::string& body) = 0;
		/**
		 * @brief Like run() but preserves stdout byte-for-byte.
		 *
		 * Used for `<(...)`. Defaults to run() for substitutors that
		 * don't distinguish raw and trimmed output.
		 */
		virtual std::string runRaw(const std::string& body) { return run(body); }
		/**
		 * @brief Did a substitution raise shell control flow?
		 *
		 * The Executor reports a pending `exit` / `return` / `break` /
		 * `continue` signal here so the Expander stops expanding the
		 * rest of the word.
		 */
		virtual bool interrupted() const { return false; }
	};

	/**
	 * @brief Word-expansion engine.
	 *
	 * Constructed with the active Environment (variables, options) and
	 * an optional CommandSubstitutor. Methods correspond to the
	 * different contexts in which a Word can be expanded.
	 *
	 * Errors are reported as values: an unrecoverable expansion error
	 * (unbound variable under `set -u`, a failed `${name:?msg}`) sets a
	 * pending error message instead of throwing. Callers check failed()
	 * after any expansion call and consume the message with takeError();
	 * the error sticks until consumed so it survives intermediate
	 * helper frames.
	 */
	class Expander {
	public:
		Expander(Environment& env, CommandSubstitutor* sub = nullptr);

		std::vector<std::string> expandWord(const Word& w);

		/**
		 * @brief Same pipeline as expandWord() minus brace expansion.
		 *
		 * Used internally by the brace-expansion driver for each
		 * generated alternative.
		 */
		std::vector<std::string> expandWordPostBrace(const Word& w);

		/**
		 * @brief Expansion for non-splitting contexts.
		 *
		 * Used for assignment values, redirection targets, case
		 * subjects, etc. Same pipeline minus word splitting and
		 * pathname expansion.
		 */
		std::string expandStringValue(const Word& w);

		/**
		 * @brief Expand a heredoc body.
		 *
		 * @param body   The captured heredoc text.
		 * @param quoted If true, the delimiter was quoted, so the
		 *               body is returned verbatim. Otherwise
		 *               `$`-substitutions and backquotes are
		 *               processed inline.
		 */
		std::string expandHeredoc(const std::string& body, bool quoted);

		long long evalArith(const std::string& body);

		/**
		 * @brief Is a fatal expansion error pending?
		 *
		 * Set by `set -u` violations and failed `${name:?msg}` forms.
		 * The flag sticks until takeError() consumes it, so every
		 * caller that detects a failure must consume the message —
		 * even when it intends to ignore it.
		 */
		bool failed() const { return !pending_error_.empty(); }

		std::string takeError() {
			std::string message = std::move(pending_error_);
			pending_error_.clear();
			return message;
		}

		/**
		 * @brief evalArith() for contexts that swallow arithmetic errors.
		 *
		 * Used for array subscripts, slice bounds, and similar places
		 * where a bad expression falls back to a default instead of
		 * failing the command. Returns false on failure, consuming the
		 * error iff THIS evaluation raised it; with an error already
		 * pending it refuses to evaluate and leaves that error alone.
		 */
		bool tryEvalArith(const std::string& body, long long& out) {
			if (failed()) return false;

			const long long value = evalArith(body);
			if (failed()) {
				takeError();
				return false;
			}

			out = value;
			return true;
		}

		/**
		 * @brief Watermark of pending `<(...)` temp files.
		 *
		 * Used by the executor to scope temp-file cleanup so nested
		 * SimpleCommands only delete the files they themselves
		 * produced — without this they'd clobber outer files on the
		 * way out.
		 */
		std::size_t pendingTempFileWatermark() const {
			return pending_temp_files_.size();
		}
		/// Drain temp files produced after @p watermark; caller now owns them.
		std::vector<std::string> drainTempFilesSince(std::size_t watermark) {
			std::vector<std::string> out;
			if (watermark >= pending_temp_files_.size()) return out;

			out.assign(pending_temp_files_.begin() + watermark, pending_temp_files_.end());
			pending_temp_files_.resize(watermark);
			return out;
		}

		// Tagged text used internally during expansion. Each character
		// carries flags describing its provenance.
		// Public because file-static helpers in expander.cpp consume it as
		// an opaque "string + per-char flags" buffer.
		static constexpr std::uint8_t F_QUOTED = 1u << 0;
		// A space that separates two elements of `$@` / `${a[@]}`. In
		// non-splitting contexts it stays a plain space; word splitting
		// treats it as a field boundary that IFS cannot merge away.
		// With F_QUOTED it comes from `"$@"`, so both neighbouring
		// fields survive even when empty.
		static constexpr std::uint8_t F_FIELD_END = 1u << 1;
		// On an F_FIELD_END marker: the field that ends here had a
		// quoted part, so it survives even when empty.
		static constexpr std::uint8_t F_QUOTED_FIELD = 1u << 2;

		struct Tagged {
			std::string text;
			std::vector<std::uint8_t> flags;
			// The field after the last F_FIELD_END marker has a quoted
			// part (`''`, `"$x"`, ...), so it survives even when empty.
			bool had_quote = false;
			std::size_t size() const { return text.size(); }
			void push(char c, std::uint8_t f) {
				text.push_back(c);
				flags.push_back(f);
			}
			// Close the current field with an F_FIELD_END marker.
			void endField(bool quoted) {
				std::uint8_t f = F_FIELD_END;
				if (quoted) f |= F_QUOTED;
				if (had_quote || quoted) f |= F_QUOTED_FIELD;
				push(' ', f);
				had_quote = quoted;
			}
			// Pre-size both parallel buffers; rendered output is usually
			// at least as long as the source spelling, so reserving the
			// raw length avoids most mid-render reallocations.
			void reserve(std::size_t n) {
				text.reserve(n);
				flags.reserve(n);
			}
		};

	private:

		// Record an expansion error. First error wins so the diagnostic
		// matches what the first failure point saw.
		void fail(std::string msg) {
			if (pending_error_.empty()) pending_error_ = std::move(msg);
		}
		// True when expansion should stop early: an error is pending or
		// a command substitution raised shell control flow.
		bool aborting() const {
			return failed() || (sub_ != nullptr && sub_->interrupted());
		}

		Environment& env_;
		CommandSubstitutor* sub_;
		PathConv path_conv_;
		std::vector<std::string> pending_temp_files_;
		std::string pending_error_;

		Tagged renderWord(const Word& w);
		// renderSegment, renderArrayFields and renderParamExpSegment
		// return true when they rendered an `@` list with no elements,
		// which removes the quoted null of the enclosing `"..."`.
		bool   renderSegment(const WordSegment& s, Tagged& out, bool inside_dq);
		bool   renderArrayFields(const std::vector<std::string>& elems,
		                         bool star, bool inside_dq, Tagged& out);
		void   renderDoubleQuoted(const WordSegment& s, Tagged& out);
		bool   renderParamExpSegment(const WordSegment& s, Tagged& out, bool inside_dq);
		void   renderProcSubstSegment(const WordSegment& s, Tagged& out);

		std::string lookupParam(const std::string& name, bool suppress_nounset = false);
		std::string lookupPositional(std::size_t index) const;
		bool tryExpandIndirectParam(const std::string& body, std::string& out);
		std::string joinPositionals(char form) const;
		bool        lookupSpecialChar(char c, std::string& out);
		bool        lookupDynamicSpecial(const std::string& name, std::string& out) const;
		// Look up name[subscript] as a scalar string. Supports numeric
		// subscripts (indexed arrays, where the subscript is an arithmetic
		// expression), `@`/`*` (joined elements), and assoc-array string
		// keys. Returns empty for unknown names/keys.
		std::string lookupSubscripted(const std::string& name,
		                              const std::string& subscript,
		                              bool star_join_ifs);
		std::string starSeparator(bool star_join_ifs) const;
		std::string resolveSubscript(const std::string& subscript);
		std::string joinWholeArray(const std::string& name, const std::string& sep);
		std::size_t arrayLength(const std::string& name) const;
		std::vector<std::string> arrayKeys(const std::string& name) const;
		std::vector<std::string> arrayValues(const std::string& name) const;
		std::string expandParam(const std::string& body, bool quoted_ctx);
		std::string expandParamLengthForm(const std::string& body);
		std::string expandParamIndicesForm(const std::string& body);
		std::string evalParamDefaultOp(char op, bool colon,
		                               const std::string& name,
		                               const std::string& cur,
		                               const std::string& arg);
		// A `@`, `*`, `name[@]` or `name[*]` subject with an operator
		// after it. Bash applies the operator to each element, and the
		// `@` forms keep the elements as separate fields.
		struct ListParam {
			std::string name;        // the array name; empty for `@` / `*`
			bool star = false;       // a `*` form, joined with IFS[0]
			std::size_t op_pos = 0;  // index of the operator in the body
		};
		using KeyedList = std::vector<std::pair<long long, std::string>>;
		bool parseListParam(const std::string& body, ListParam& out) const;
		std::vector<std::string> listParamValues(const ListParam& p) const;
		KeyedList sliceCandidates(const ListParam& p) const;
		std::vector<std::string> expandListParam(const ListParam& p, const std::string& body);
		std::vector<std::string> sliceListParam(const ListParam& p, const std::string& args);
		std::vector<std::string> evalListDefaultOp(char op, bool colon,
		                                           const std::string& subject,
		                                           std::vector<std::string> elems,
		                                           const std::string& arg);
		std::vector<std::string> mapListOp(char op, bool doubled, const std::string& arg,
		                                   std::vector<std::string> elems);
		std::string evalParamReplace(const std::string& cur,
		                             const std::string& args, bool all);
		struct ParamOpCtx {
			const std::string& name;
			const std::string& subscript;
			bool has_subscript;
			const std::string& body;       // for ${name:OFF:LEN} / pattern args
			std::string named_value;       // resolved scalar value
		};
		std::string expandParamApplyOp(char op, bool colon, std::size_t op_pos,
		                               ParamOpCtx& ctx);

		std::string runCmdSubst(const std::string& body);
		std::string interpretAnsiC(const std::string& body);

		// Helpers for expandHeredoc — one per `$X` / backquote shape.
		// Each takes the body, the cursor `i` (advanced through the
		// matched span), and the output buffer.
		void expandHeredocDollar     (const std::string& body, std::size_t& i,
		                              std::string& out);
		void expandHeredocParamBraces(const std::string& body, std::size_t& i,
		                              std::string& out);
		void expandHeredocCmdSubst   (const std::string& body, std::size_t& i,
		                              std::string& out);
		void expandHeredocArith      (const std::string& body, std::size_t& i,
		                              std::string& out);
		void expandHeredocArithBracket(const std::string& body, std::size_t& i,
		                              std::string& out);
		void expandHeredocSimpleParam(const std::string& body, std::size_t& i,
		                              std::string& out);
		void expandHeredocBackquote  (const std::string& body, std::size_t& i,
		                              std::string& out);

	public:
		bool        isSpecialParam1(char c) const;
	private:

		Word applyTildeExpansion(const Word& w);

		std::vector<Tagged> splitWords(const Tagged& t);
		std::vector<std::string> globExpand(const Tagged& t);
		std::string quoteRemove(const Tagged& t);

		std::string substringExpand(const std::string& val, const std::string& args);
		std::string stripPrefix(const std::string& val, const std::string& pat, bool greedy);
		std::string stripSuffix(const std::string& val, const std::string& pat, bool greedy);
		std::string applyCaseConv(const std::string& val, char op, bool all,
		                          const std::string& pat);
		std::string expandParamWordArg(const std::string& body);
		std::string replacePattern(const std::string& val, const std::string& pat,
		                           const std::string& rep, bool all,
		                           bool anchor_start, bool anchor_end);
	};

}  // namespace wbsh

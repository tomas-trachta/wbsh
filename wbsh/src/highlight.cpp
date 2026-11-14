/**
 * @file highlight.cpp
 * @brief Syntax colours for the command line being edited: the scan and the painting.
 */

#include "highlight.h"

#include <cctype>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "lexer.h"

namespace wbsh {

	namespace highlight_detail {

		static const char* const kKeywords[] = {
			"!", "[[", "case", "coproc", "do", "done", "elif", "else", "esac", "fi", "for",
			"function", "if", "select", "then", "time", "until", "while", "{", "}", nullptr,
		};

		// The word after one of these is a command again.
		static const char* const kPrecommands[] = {
			"builtin", "command", "env", "exec", "nice", "nohup", "sudo", nullptr,
		};

		static const char* const kRoleNames[kHighlightRoleCount] = {
			"plain", "command", "unknown", "keyword", "string", "variable", "comment",
			"operator", "option", "path",
		};

		enum class Effect {
			kSeparator,   // the next word is a command
			kCaseBreak,   // ;; ;& ;;&
			kClose,       // )
			kRedirect,
			kDuplicate,   // <& >&: the target may be a descriptor
			kHeredoc,
		};

		struct Operator {
			const char* text;
			Effect effect;
		};

		// Longest first where one spelling starts another.
		static const Operator kOperators[] = {
			{ ";;&", Effect::kCaseBreak }, { ";;", Effect::kCaseBreak },
			{ ";&", Effect::kCaseBreak },  { "&&", Effect::kSeparator },
			{ "&>>", Effect::kRedirect },  { "&>", Effect::kRedirect },
			{ "||", Effect::kSeparator },  { "|&", Effect::kSeparator },
			{ "<<<", Effect::kRedirect },  { "<<-", Effect::kHeredoc },
			{ "<<", Effect::kHeredoc },    { "<>", Effect::kRedirect },
			{ "<&", Effect::kDuplicate },  { ">>", Effect::kRedirect },
			{ ">|", Effect::kRedirect },   { ">&", Effect::kDuplicate },
			{ ";", Effect::kSeparator },   { "&", Effect::kSeparator },
			{ "|", Effect::kSeparator },   { "(", Effect::kSeparator },
			{ ")", Effect::kClose },       { "<", Effect::kRedirect },
			{ ">", Effect::kRedirect },
		};

		enum class Next {
			kCommand,
			kArgument,
			kLoopName,       // after `for` or `select`
			kCaseWord,       // after `case`
			kFunctionName,   // after `function`
			kIn,             // after `for NAME` or `case WORD`
			kPattern,        // a case pattern, up to its `)`
		};

		struct Heredoc {
			std::string delimiter;
			bool strip_tabs;
		};

		struct State {
			Next next = Next::kCommand;
			bool in_case = false;
			int case_depth = 0;
			bool test = false;
			bool after_precommand = false;
			bool redirect_target = false;
			bool dup_target = false;
			bool heredoc_delimiter = false;
			bool heredoc_strip = false;
			std::vector<Heredoc> heredocs;
		};

		struct Word {
			std::size_t start = 0;
			std::size_t end = 0;
			std::string value;        // quotes and escapes taken away
			bool quoted = false;
			bool expanded = false;    // holds an expansion, so its value is not known
			std::vector<std::pair<std::size_t, std::size_t>> literal;   // where its role colours
		};

		static bool isOneOf(const std::string& word, const char* const* names) {
			for (int i = 0; names[i] != nullptr; ++i) {
				if (word == names[i]) return true;
			}

			return false;
		}

		static bool isNameStart(char letter) {
			return std::isalpha(static_cast<unsigned char>(letter)) != 0 || letter == '_';
		}

		static bool isNameChar(char letter) {
			return isNameStart(letter) || std::isdigit(static_cast<unsigned char>(letter)) != 0;
		}

		static bool isBlank(char letter) {
			return letter == ' ' || letter == '\t' || letter == '\r';
		}

		static bool isOperatorChar(char letter) {
			return std::strchr(";&|()<>", letter) != nullptr;
		}

		static bool endsWord(char letter) {
			return isBlank(letter) || letter == '\n' || isOperatorChar(letter);
		}

		static bool isBare(const Word& word) {
			return !word.quoted && !word.expanded;
		}

		static bool isOption(const std::string& value) {
			return value.size() > 1 && value[0] == '-';
		}

		static bool mayBePath(const std::string& value) {
			return !value.empty() && value.find_first_of("*?[") == std::string::npos;
		}

		static bool isDigit(char letter) {
			return std::isdigit(static_cast<unsigned char>(letter)) != 0;
		}

		static bool isDescriptorWord(const std::string& value) {
			if (value == "-") return true;

			std::size_t digits = 0;
			while (digits < value.size() && isDigit(value[digits])) ++digits;

			const bool moved = digits + 1 == value.size() && value.back() == '-';
			return digits > 0 && (digits == value.size() || moved);
		}

		static void addLiteral(Word& word, std::size_t from, std::size_t to) {
			if (!word.literal.empty() && word.literal.back().second == from) {
				word.literal.back().second = to;
				return;
			}

			word.literal.emplace_back(from, to);
		}

		class Scanner {
		public:
			Scanner(const std::string& text, const HighlightLookup& lookup,
			        std::vector<HighlightRole>& roles)
				: text_(text), lookup_(lookup), roles_(roles) {}

			void scanRange(std::size_t from, std::size_t to);

		private:
			std::size_t scanItem(State& state, std::size_t at, std::size_t to);
			std::size_t scanNewline(State& state, std::size_t at, std::size_t to);
			std::size_t scanHeredocBody(const Heredoc& heredoc, std::size_t at, std::size_t to);
			std::size_t scanComment(std::size_t at, std::size_t to);
			bool opensArithmeticCommand(const State& state, std::size_t at, std::size_t to) const;
			std::size_t scanArithmeticCommand(State& state, std::size_t at, std::size_t to);
			std::size_t ioNumberEnd(std::size_t at, std::size_t to) const;
			bool startsOperator(std::size_t at, std::size_t to) const;
			const Operator& operatorAt(std::size_t at, std::size_t to) const;
			std::size_t scanOperator(State& state, std::size_t at, std::size_t to);
			std::size_t applySeparator(State& state, const Operator& op, std::size_t end,
			                           std::size_t to);
			std::size_t emptyParensEnd(std::size_t at, std::size_t to) const;

			std::size_t scanWord(Word& word, std::size_t at, std::size_t to);
			bool scanWordPart(Word& word, std::size_t& at, std::size_t to, int& parens);
			bool opensWordParens(const Word& word, std::size_t at) const;
			std::size_t assignmentEquals(std::size_t start, std::size_t end) const;
			std::size_t scanQuotedPart(Word& word, std::size_t at, std::size_t to);
			std::size_t scanEscape(Word& word, std::size_t at, std::size_t to);
			std::size_t scanSingleQuoted(Word& word, std::size_t at, std::size_t to);
			std::size_t scanDoubleQuoted(Word& word, std::size_t at, std::size_t to);
			std::size_t scanAnsiQuoted(Word& word, std::size_t at, std::size_t to);
			std::size_t scanDollar(std::size_t at, std::size_t to);
			std::size_t scanDollarParen(std::size_t at, std::size_t to);
			std::size_t braceEnd(std::size_t at, std::size_t to) const;
			std::size_t scanSubstitution(std::size_t at, std::size_t open, std::size_t to);
			std::size_t scanBackquote(std::size_t at, std::size_t to);

			void classify(State& state, const Word& word);
			void classifyCommandWord(State& state, const Word& word);
			void classifyKeyword(State& state, const Word& word);
			void classifyIn(State& state, const Word& word);
			void classifyPattern(State& state, const Word& word);
			void classifyArgument(State& state, const Word& word);
			void classifyRedirectTarget(State& state, const Word& word);
			void classifyHeredocDelimiter(State& state, const Word& word);
			bool definesFunction(std::size_t at) const;
			void colourWord(const Word& word, HighlightRole role);
			void paint(std::size_t from, std::size_t to, HighlightRole role);

			const std::string& text_;
			const HighlightLookup& lookup_;
			std::vector<HighlightRole>& roles_;
		};

		void Scanner::paint(std::size_t from, std::size_t to, HighlightRole role) {
			for (std::size_t i = from; i < to && i < roles_.size(); ++i) roles_[i] = role;
		}

		void Scanner::colourWord(const Word& word, HighlightRole role) {
			for (const auto& span : word.literal) paint(span.first, span.second, role);
		}

		void Scanner::scanRange(std::size_t from, std::size_t to) {
			State state;
			std::size_t at = from;
			while (at < to) at = scanItem(state, at, to);
		}

		std::size_t Scanner::scanItem(State& state, std::size_t at, std::size_t to) {
			const char letter = text_[at];
			if (isBlank(letter)) return at + 1;
			if (letter == '\\' && at + 1 < to && text_[at + 1] == '\n') return at + 2;
			if (letter == '\n') return scanNewline(state, at + 1, to);
			if (letter == '#') return scanComment(at, to);
			if (opensArithmeticCommand(state, at, to)) return scanArithmeticCommand(state, at, to);

			const std::size_t io_end = ioNumberEnd(at, to);
			if (io_end != at) {
				paint(at, io_end, HighlightRole::kOperator);
				return scanOperator(state, io_end, to);
			}

			if (startsOperator(at, to)) return scanOperator(state, at, to);

			Word word;
			const std::size_t end = scanWord(word, at, to);
			classify(state, word);
			return end > at ? end : at + 1;
		}

		std::size_t Scanner::scanNewline(State& state, std::size_t at, std::size_t to) {
			for (const Heredoc& heredoc : state.heredocs) at = scanHeredocBody(heredoc, at, to);

			state.heredocs.clear();
			state.redirect_target = false;
			state.dup_target = false;
			if (state.next == Next::kArgument) state.next = Next::kCommand;
			return at;
		}

		std::size_t Scanner::scanHeredocBody(const Heredoc& heredoc, std::size_t at, std::size_t to) {
			while (at < to) {
				std::size_t line_end = text_.find('\n', at);
				if (line_end == std::string::npos || line_end > to) line_end = to;

				std::size_t body = at;
				while (heredoc.strip_tabs && body < line_end && text_[body] == '\t') ++body;

				const bool last = text_.compare(body, line_end - body, heredoc.delimiter) == 0;
				if (!last) paint(at, line_end, HighlightRole::kString);

				at = line_end < to ? line_end + 1 : to;
				if (last) break;
			}

			return at;
		}

		std::size_t Scanner::scanComment(std::size_t at, std::size_t to) {
			std::size_t end = text_.find('\n', at);
			if (end == std::string::npos || end > to) end = to;

			paint(at, end, HighlightRole::kComment);
			return end;
		}

		bool Scanner::opensArithmeticCommand(const State& state, std::size_t at, std::size_t to) const {
			const bool position = state.next == Next::kCommand || state.next == Next::kLoopName;
			return position && !state.redirect_target && at + 1 < to
			    && text_[at] == '(' && text_[at + 1] == '(';
		}

		std::size_t Scanner::scanArithmeticCommand(State& state, std::size_t at, std::size_t to) {
			std::size_t close = Lexer::arithmeticEnd(text_, at + 2);
			if (close == std::string::npos || close + 2 > to) close = to;

			paint(at, at + 2, HighlightRole::kOperator);
			for (std::size_t i = at + 2; i < close;) {
				const std::size_t end = text_[i] == '$' ? scanDollar(i, close) : i;
				i = end > i ? end : i + 1;
			}

			paint(close, close + 2, HighlightRole::kOperator);
			state.next = Next::kArgument;
			return close < to ? close + 2 : to;
		}

		// The digits of `2>` or `10<&`: the descriptor a redirection is for.
		std::size_t Scanner::ioNumberEnd(std::size_t at, std::size_t to) const {
			std::size_t i = at;
			while (i < to && isDigit(text_[i])) ++i;
			if (i == at || i >= to || (text_[i] != '<' && text_[i] != '>')) return at;
			if (i + 1 < to && text_[i + 1] == '(') return at;

			return i;
		}

		// `<(` and `>(` start a process substitution, which is a word.
		bool Scanner::startsOperator(std::size_t at, std::size_t to) const {
			const char letter = text_[at];
			if (!isOperatorChar(letter)) return false;

			const bool redirect = letter == '<' || letter == '>';
			return !(redirect && at + 1 < to && text_[at + 1] == '(');
		}

		const Operator& Scanner::operatorAt(std::size_t at, std::size_t to) const {
			for (const Operator& op : kOperators) {
				const std::size_t length = std::strlen(op.text);
				if (at + length <= to && text_.compare(at, length, op.text) == 0) return op;
			}

			return kOperators[sizeof(kOperators) / sizeof(kOperators[0]) - 1];
		}

		// Inside [[ ]], `<`, `>`, `&&`, `(` and the rest compare and group.
		std::size_t Scanner::scanOperator(State& state, std::size_t at, std::size_t to) {
			const Operator& op = operatorAt(at, to);
			const std::size_t end = at + std::strlen(op.text);
			paint(at, end, HighlightRole::kOperator);
			state.after_precommand = false;
			if (state.test) {
				state.next = Next::kArgument;
				return end;
			}

			switch (op.effect) {
			case Effect::kSeparator:
				return applySeparator(state, op, end, to);
			case Effect::kCaseBreak:
				state.next = state.case_depth > 0 ? Next::kPattern : Next::kCommand;
				return end;
			case Effect::kClose:
				state.next = state.next == Next::kPattern ? Next::kCommand : Next::kArgument;
				return end;
			case Effect::kHeredoc:
				state.heredoc_delimiter = true;
				state.heredoc_strip = std::strcmp(op.text, "<<-") == 0;
				return end;
			default:
				state.redirect_target = true;
				state.dup_target = op.effect == Effect::kDuplicate;
				return end;
			}
		}

		// `()` after a function's name: its body, a command, comes next.
		std::size_t Scanner::applySeparator(State& state, const Operator& op, std::size_t end,
		                                    std::size_t to) {
			const bool pattern = state.next == Next::kPattern;
			const bool grouping = std::strcmp(op.text, "|") == 0 || std::strcmp(op.text, "(") == 0;
			if (pattern && grouping) return end;

			state.next = Next::kCommand;
			if (std::strcmp(op.text, "(") != 0) return end;

			const std::size_t close = emptyParensEnd(end, to);
			paint(end, close, HighlightRole::kOperator);
			return close;
		}

		std::size_t Scanner::emptyParensEnd(std::size_t at, std::size_t to) const {
			std::size_t i = at;
			while (i < to && isBlank(text_[i])) ++i;

			return i < to && text_[i] == ')' ? i + 1 : at;
		}

		std::size_t Scanner::scanWord(Word& word, std::size_t at, std::size_t to) {
			word.start = at;
			int parens = 0;
			while (at < to && scanWordPart(word, at, to, parens)) {}

			word.end = at;
			return at;
		}

		// An array assignment's `(...)` and an extglob's `@(...)` belong to
		// the word, blanks and all.
		bool Scanner::scanWordPart(Word& word, std::size_t& at, std::size_t to, int& parens) {
			const char letter = text_[at];
			const bool opens = letter == '(' && (parens > 0 || opensWordParens(word, at));
			if (opens || (letter == ')' && parens > 0)) {
				parens += letter == '(' ? 1 : -1;
				addLiteral(word, at, at + 1);
				++at;
				return true;
			}

			const bool process = at == word.start && (letter == '<' || letter == '>')
				&& at + 1 < to && text_[at + 1] == '(';
			if (process) {
				word.expanded = true;
				at = scanSubstitution(at, at + 1, to);
				return true;
			}

			if (parens == 0 && endsWord(letter)) return false;

			const std::size_t end = scanQuotedPart(word, at, to);
			if (end == at) {
				word.value.push_back(letter);
				addLiteral(word, at, at + 1);
			}

			at = end > at ? end : at + 1;
			return true;
		}

		// A quoted string, an escape or an expansion starting at text[at];
		// its end, or `at` when the character is a plain one.
		std::size_t Scanner::scanQuotedPart(Word& word, std::size_t at, std::size_t to) {
			const char letter = text_[at];
			const char next = at + 1 < to ? text_[at + 1] : '\0';
			if (letter == '\\') return scanEscape(word, at, to);
			if (letter == '\'') return scanSingleQuoted(word, at, to);
			if (letter == '"') return scanDoubleQuoted(word, at, to);
			if (letter == '$' && next == '\'') return scanAnsiQuoted(word, at, to);
			if (letter == '$' && next == '"') {
				paint(at, at + 1, HighlightRole::kString);
				return scanDoubleQuoted(word, at + 1, to);
			}

			if (letter != '$' && letter != '`') return at;

			const std::size_t end = letter == '$' ? scanDollar(at, to) : scanBackquote(at, to);
			if (end > at) word.expanded = true;
			return end;
		}

		bool Scanner::opensWordParens(const Word& word, std::size_t at) const {
			if (at == word.start) return false;

			const char before = text_[at - 1];
			if (std::strchr("?*+@!", before) != nullptr) return true;

			return before == '=' && assignmentEquals(word.start, at) == at - 1;
		}

		// Where the `=` of `NAME=` or `NAME+=` at text[start, end) is; npos
		// when the word is no assignment.
		std::size_t Scanner::assignmentEquals(std::size_t start, std::size_t end) const {
			if (start >= end || !isNameStart(text_[start])) return std::string::npos;

			std::size_t i = start + 1;
			while (i < end && isNameChar(text_[i])) ++i;
			if (i < end && text_[i] == '+') ++i;

			return i < end && text_[i] == '=' ? i : std::string::npos;
		}

		std::size_t Scanner::scanEscape(Word& word, std::size_t at, std::size_t to) {
			word.quoted = true;
			const std::size_t end = at + 2 <= to ? at + 2 : to;
			if (end == at + 2 && text_[at + 1] != '\n') word.value.push_back(text_[at + 1]);

			addLiteral(word, at, end);
			return end;
		}

		std::size_t Scanner::scanSingleQuoted(Word& word, std::size_t at, std::size_t to) {
			word.quoted = true;
			const std::size_t close = text_.find('\'', at + 1);
			const bool closed = close != std::string::npos && close < to;
			const std::size_t body_end = closed ? close : to;
			const std::size_t end = closed ? close + 1 : to;

			word.value += text_.substr(at + 1, body_end - (at + 1));
			paint(at, end, HighlightRole::kString);
			return end;
		}

		std::size_t Scanner::scanDoubleQuoted(Word& word, std::size_t at, std::size_t to) {
			word.quoted = true;
			paint(at, at + 1, HighlightRole::kString);
			std::size_t i = at + 1;
			while (i < to && text_[i] != '"') {
				const char letter = text_[i];
				const std::size_t end = letter == '$' ? scanDollar(i, to)
					: letter == '`' ? scanBackquote(i, to) : i;
				if (end > i) {
					word.expanded = true;
					i = end;
					continue;
				}

				const std::size_t step = letter == '\\' && i + 1 < to ? 2 : 1;
				word.value.push_back(text_[i + step - 1]);
				paint(i, i + step, HighlightRole::kString);
				i += step;
			}

			paint(i, i + 1, HighlightRole::kString);
			return i < to ? i + 1 : to;
		}

		std::size_t Scanner::scanAnsiQuoted(Word& word, std::size_t at, std::size_t to) {
			word.quoted = true;
			word.expanded = true;
			std::size_t i = at + 2;
			while (i < to && text_[i] != '\'') i += text_[i] == '\\' ? 2 : 1;

			const std::size_t end = i < to ? i + 1 : to;
			paint(at, end, HighlightRole::kString);
			return end;
		}

		static bool isSpecialParameter(char letter) {
			return isDigit(letter) || std::strchr("?$!#@*-", letter) != nullptr;
		}

		// The end of the expansion at text[at] == '$', or `at` itself when
		// the `$` starts none and is a plain character.
		std::size_t Scanner::scanDollar(std::size_t at, std::size_t to) {
			if (at + 1 >= to) return at;

			const char next = text_[at + 1];
			if (next == '(') return scanDollarParen(at, to);

			std::size_t end = at;
			if (next == '{') end = braceEnd(at + 2, to);
			if (isSpecialParameter(next)) end = at + 2;
			if (isNameStart(next)) {
				end = at + 2;
				while (end < to && isNameChar(text_[end])) ++end;
			}

			paint(at, end, HighlightRole::kVariable);
			return end;
		}

		// A `$((` still being typed is arithmetic until it turns out to be
		// a command substitution starting with a subshell.
		std::size_t Scanner::scanDollarParen(std::size_t at, std::size_t to) {
			const bool arithmetic = at + 2 < to && text_[at + 2] == '(';
			if (!arithmetic) return scanSubstitution(at, at + 1, to);

			const std::size_t close = Lexer::arithmeticEnd(text_, at + 3);
			if (close != std::string::npos && close + 2 <= to) {
				paint(at, close + 2, HighlightRole::kVariable);
				return close + 2;
			}

			if (Lexer::commandSubstitutionEnd(text_, at + 2) != std::string::npos) {
				return scanSubstitution(at, at + 1, to);
			}

			paint(at, to, HighlightRole::kVariable);
			return to;
		}

		std::size_t Scanner::braceEnd(std::size_t at, std::size_t to) const {
			int depth = 1;
			for (std::size_t i = at; i < to; ++i) {
				if (text_[i] == '\\') {
					++i;
				} else if (text_[i] == '{') {
					++depth;
				} else if (text_[i] == '}' && --depth == 0) {
					return i + 1;
				}
			}

			return to;
		}

		// `$(...)`, `<(...)` or `>(...)` from `at`, its `(` at `open`; the
		// commands inside are coloured as commands. A substitution not
		// closed yet runs to the end.
		std::size_t Scanner::scanSubstitution(std::size_t at, std::size_t open, std::size_t to) {
			std::size_t close = Lexer::commandSubstitutionEnd(text_, open + 1);
			if (close == std::string::npos || close >= to) close = to;

			paint(at, open + 1, HighlightRole::kVariable);
			scanRange(open + 1, close);
			paint(close, close + 1, HighlightRole::kVariable);
			return close < to ? close + 1 : to;
		}

		std::size_t Scanner::scanBackquote(std::size_t at, std::size_t to) {
			std::size_t close = at + 1;
			while (close < to && text_[close] != '`') close += text_[close] == '\\' ? 2 : 1;
			if (close > to) close = to;

			paint(at, at + 1, HighlightRole::kVariable);
			scanRange(at + 1, close);
			paint(close, close + 1, HighlightRole::kVariable);
			return close < to ? close + 1 : to;
		}

		void Scanner::classify(State& state, const Word& word) {
			if (state.heredoc_delimiter) {
				classifyHeredocDelimiter(state, word);
				return;
			}

			if (state.redirect_target) {
				classifyRedirectTarget(state, word);
				return;
			}

			switch (state.next) {
			case Next::kCommand:
				classifyCommandWord(state, word);
				break;
			case Next::kLoopName:
				colourWord(word, HighlightRole::kVariable);
				state.next = Next::kIn;
				state.in_case = false;
				break;
			case Next::kCaseWord:
				state.next = Next::kIn;
				state.in_case = true;
				break;
			case Next::kFunctionName:
				colourWord(word, HighlightRole::kCommand);
				state.next = Next::kCommand;
				break;
			case Next::kIn:
				classifyIn(state, word);
				break;
			case Next::kPattern:
				classifyPattern(state, word);
				break;
			default:
				classifyArgument(state, word);
				break;
			}
		}

		void Scanner::classifyCommandWord(State& state, const Word& word) {
			const std::size_t equals = assignmentEquals(word.start, word.end);
			if (equals != std::string::npos) {
				const bool appends = text_[equals - 1] == '+';
				paint(word.start, appends ? equals - 1 : equals, HighlightRole::kVariable);
				return;
			}

			if (isBare(word) && isOneOf(word.value, kKeywords)) {
				classifyKeyword(state, word);
				return;
			}

			if (isBare(word) && state.after_precommand && isOption(word.value)) {
				colourWord(word, HighlightRole::kOption);
				return;
			}

			state.next = Next::kArgument;
			state.after_precommand = false;
			if (word.expanded) return;

			const bool defined = definesFunction(word.end);
			colourWord(word, defined ? HighlightRole::kCommand : lookup_.command(word.value));
			if (!defined && isOneOf(word.value, kPrecommands)) {
				state.next = Next::kCommand;
				state.after_precommand = true;
			}
		}

		void Scanner::classifyKeyword(State& state, const Word& word) {
			colourWord(word, HighlightRole::kKeyword);
			const std::string& name = word.value;
			if (name == "for" || name == "select") {
				state.next = Next::kLoopName;
			} else if (name == "case") {
				state.next = Next::kCaseWord;
			} else if (name == "function") {
				state.next = Next::kFunctionName;
			} else if (name == "[[") {
				state.test = true;
				state.next = Next::kArgument;
			} else if (name == "esac" || name == "fi" || name == "done" || name == "}") {
				if (name == "esac" && state.case_depth > 0) --state.case_depth;
				state.next = Next::kArgument;
			}
		}

		void Scanner::classifyIn(State& state, const Word& word) {
			if (isBare(word) && word.value == "in") {
				colourWord(word, HighlightRole::kKeyword);
				if (state.in_case) ++state.case_depth;
				state.next = state.in_case ? Next::kPattern : Next::kArgument;
				return;
			}

			if (isBare(word) && word.value == "do") {
				colourWord(word, HighlightRole::kKeyword);
				state.next = Next::kCommand;
				return;
			}

			state.next = Next::kArgument;
			classifyArgument(state, word);
		}

		void Scanner::classifyPattern(State& state, const Word& word) {
			if (!isBare(word) || word.value != "esac") return;

			colourWord(word, HighlightRole::kKeyword);
			if (state.case_depth > 0) --state.case_depth;
			state.next = Next::kArgument;
		}

		void Scanner::classifyArgument(State& state, const Word& word) {
			if (state.test && isBare(word) && word.value == "]]") {
				colourWord(word, HighlightRole::kKeyword);
				state.test = false;
				return;
			}

			if (isBare(word) && isOption(word.value)) {
				colourWord(word, HighlightRole::kOption);
				return;
			}

			if (!word.expanded && mayBePath(word.value) && lookup_.existing_path(word.value)) {
				colourWord(word, HighlightRole::kPath);
			}
		}

		void Scanner::classifyRedirectTarget(State& state, const Word& word) {
			const bool duplicate = state.dup_target;
			state.redirect_target = false;
			state.dup_target = false;
			if (duplicate && isBare(word) && isDescriptorWord(word.value)) {
				colourWord(word, HighlightRole::kOperator);
				return;
			}

			if (!word.expanded && mayBePath(word.value) && lookup_.existing_path(word.value)) {
				colourWord(word, HighlightRole::kPath);
			}
		}

		void Scanner::classifyHeredocDelimiter(State& state, const Word& word) {
			state.heredoc_delimiter = false;
			state.heredocs.push_back({ word.value, state.heredoc_strip });
			colourWord(word, HighlightRole::kString);
		}

		bool Scanner::definesFunction(std::size_t at) const {
			while (at < text_.size() && isBlank(text_[at])) ++at;
			if (at >= text_.size() || text_[at] != '(') return false;

			return emptyParensEnd(at + 1, text_.size()) != at + 1;
		}

		static void appendStyle(std::string& out, const std::string& sgr) {
			out += sgr.empty() ? "\x1b[0m" : "\x1b[0;" + sgr + "m";
		}

		static bool isSgrValue(const std::string& value) {
			return value.find_first_not_of("0123456789;") == std::string::npos;
		}

		static void applySpecEntry(const std::string& entry, HighlightPalette& palette) {
			const std::size_t equals = entry.find('=');
			if (equals == std::string::npos) return;

			const std::string name = entry.substr(0, equals);
			std::string value = entry.substr(equals + 1);
			if (!isSgrValue(value)) return;
			if (value == "0") value.clear();

			for (std::size_t role = 0; role < kHighlightRoleCount; ++role) {
				if (name == kRoleNames[role]) palette.sgr[role] = value;
			}
		}

	}  // namespace highlight_detail

	std::vector<HighlightRole> highlightRoles(const std::string& text, const HighlightLookup& lookup) {
		std::vector<HighlightRole> roles(text.size(), HighlightRole::kPlain);
		highlight_detail::Scanner scanner(text, lookup, roles);
		scanner.scanRange(0, text.size());
		return roles;
	}

	HighlightPalette defaultHighlightPalette() {
		HighlightPalette palette;
		palette.sgr[static_cast<std::size_t>(HighlightRole::kCommand)] = "32";
		palette.sgr[static_cast<std::size_t>(HighlightRole::kUnknown)] = "31";
		palette.sgr[static_cast<std::size_t>(HighlightRole::kKeyword)] = "35";
		palette.sgr[static_cast<std::size_t>(HighlightRole::kString)] = "33";
		palette.sgr[static_cast<std::size_t>(HighlightRole::kVariable)] = "36";
		palette.sgr[static_cast<std::size_t>(HighlightRole::kComment)] = "90";
		palette.sgr[static_cast<std::size_t>(HighlightRole::kOption)] = "94";
		palette.sgr[static_cast<std::size_t>(HighlightRole::kPath)] = "4";
		return palette;
	}

	void applyHighlightSpec(const std::string& spec, HighlightPalette& palette) {
		std::size_t start = 0;
		while (start <= spec.size()) {
			std::size_t end = spec.find(':', start);
			if (end == std::string::npos) end = spec.size();

			highlight_detail::applySpecEntry(spec.substr(start, end - start), palette);
			start = end + 1;
		}
	}

	std::string paintHighlighted(const std::string& text, const std::vector<HighlightRole>& roles,
	                             std::size_t from, const HighlightPalette& palette) {
		static const std::string kDefaultStyle;

		std::string out;
		out.reserve(text.size() * 2);

		const std::string* current = &kDefaultStyle;
		for (std::size_t i = from; i < text.size(); ++i) {
			const std::string& wanted = palette.sgr[static_cast<std::size_t>(roles[i])];
			if (wanted != *current) highlight_detail::appendStyle(out, wanted);

			current = &wanted;
			out.push_back(text[i]);
		}

		if (!current->empty()) out += "\x1b[0m";
		return out;
	}

}  // namespace wbsh

/**
 * @file collsyms.cpp
 * @brief The POSIX collating-symbol names of the portable character set.
 */

#include "collsyms.h"

#include "utf8.h"

namespace wbsh {

	namespace collsyms_detail {

		struct CollatingName {
			const char* name;
			char code;
		};

		static const CollatingName kNames[] = {
			{ "NUL", '\0' }, { "alert", '\a' }, { "backspace", '\b' }, { "tab", '\t' },
			{ "newline", '\n' }, { "vertical-tab", '\v' }, { "form-feed", '\f' },
			{ "carriage-return", '\r' }, { "space", ' ' }, { "exclamation-mark", '!' },
			{ "quotation-mark", '"' }, { "number-sign", '#' }, { "dollar-sign", '$' },
			{ "percent-sign", '%' }, { "ampersand", '&' }, { "apostrophe", '\'' },
			{ "left-parenthesis", '(' }, { "right-parenthesis", ')' }, { "asterisk", '*' },
			{ "plus-sign", '+' }, { "comma", ',' }, { "hyphen", '-' }, { "hyphen-minus", '-' },
			{ "period", '.' }, { "full-stop", '.' }, { "slash", '/' }, { "solidus", '/' },
			{ "zero", '0' }, { "one", '1' }, { "two", '2' }, { "three", '3' }, { "four", '4' },
			{ "five", '5' }, { "six", '6' }, { "seven", '7' }, { "eight", '8' }, { "nine", '9' },
			{ "colon", ':' }, { "semicolon", ';' }, { "less-than-sign", '<' },
			{ "equals-sign", '=' }, { "greater-than-sign", '>' }, { "question-mark", '?' },
			{ "commercial-at", '@' }, { "left-square-bracket", '[' }, { "backslash", '\\' },
			{ "reverse-solidus", '\\' }, { "right-square-bracket", ']' },
			{ "circumflex", '^' }, { "circumflex-accent", '^' }, { "underscore", '_' },
			{ "low-line", '_' }, { "grave-accent", '`' }, { "left-brace", '{' },
			{ "left-curly-bracket", '{' }, { "vertical-line", '|' }, { "right-brace", '}' },
			{ "right-curly-bracket", '}' }, { "tilde", '~' }, { "DEL", '\x7f' },
		};

	}  // namespace collsyms_detail

	bool collatingSymbolChar(const std::string& name, char32_t& out) {
		if (name.empty()) return false;

		const Utf8Char single = decodeUtf8(name, 0);
		if (single.length == name.size()) {
			out = single.code;
			return true;
		}

		for (const collsyms_detail::CollatingName& entry : collsyms_detail::kNames) {
			if (name != entry.name) continue;

			out = static_cast<unsigned char>(entry.code);
			return true;
		}

		return false;
	}

}  // namespace wbsh

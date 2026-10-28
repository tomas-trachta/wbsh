#pragma once

/**
 * @file posixregex.h
 * @brief POSIX basic / extended regular expressions (with the GNU escapes
 *        \w \s \b \< \> \` \' and back-references) and the awk dialect.
 *
 * Matching follows POSIX: the leftmost match wins, and among those the
 * longest. Among the ways to make that match, groups take the earliest
 * alternative and the greediest repetition (not POSIX's longest-group
 * rule). Patterns and subjects are UTF-8; `.` and bracket
 * expressions match whole characters, and a byte that is not valid UTF-8
 * is a character of its own. Nothing recurses on the call stack, so long
 * subjects and heavily repeated groups cannot overflow it: patterns
 * without back-references run on a lazily built DFA and a Pike VM, in
 * time linear in the subject, patterns with them on a backtracker with a
 * heap-allocated stack.
 */

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace wbsh {

	namespace posixregex_detail {

		struct Program;

	}  // namespace posixregex_detail

	enum class RegexSyntax {
		Basic,
		Extended,
		/// ERE plus awk's string escapes (\/ \" \ddd, also inside brackets) and gawk's \y.
		Awk,
	};

	/// Extra context every match must satisfy (grep -w / -x).
	enum class RegexWrap {
		None,
		Word,
		Line,
	};

	struct RegexOptions {
		RegexSyntax syntax = RegexSyntax::Basic;
		bool ignore_case = false;
		/// ^ and $ also match next to an embedded newline (sed's M flag).
		bool multiline = false;
		RegexWrap wrap = RegexWrap::None;
	};

	struct RegexMatch {
		/// Start and end offset of each group, group 0 being the whole
		/// match; std::string::npos for a group that did not take part.
		std::vector<std::size_t> spans;

		std::size_t groups() const { return spans.size() / 2; }
		bool matched(std::size_t group) const;
		std::size_t start(std::size_t group) const { return spans[group * 2]; }
		std::size_t end(std::size_t group) const { return spans[group * 2 + 1]; }
		std::size_t length(std::size_t group) const;
		std::string str(const std::string& subject, std::size_t group) const;
	};

	class PosixRegex {
	public:
		/// False (see error()) when @p pattern is not a valid expression.
		bool compile(const std::string& pattern, const RegexOptions& options);

		/// One expression matching wherever any of @p patterns matches; the
		/// groups of each pattern are numbered after those of the previous ones.
		bool compile(const std::vector<std::string>& patterns, const RegexOptions& options);

		/// GNU's wording of why the last compile failed.
		const std::string& error() const { return error_; }

		/// Number of groups, counting the whole match as group 0.
		std::size_t groups() const;

		/// The leftmost-longest match starting at or after @p from. Text
		/// before @p from still counts for ^ and word boundaries.
		bool search(const std::string& subject, std::size_t from, RegexMatch& match) const;

		bool test(const std::string& subject) const;

		/// Whether the whole of @p subject matches.
		bool matchesWhole(const std::string& subject) const;

	private:
		std::shared_ptr<const posixregex_detail::Program> program_;
		std::string error_;
	};

}  // namespace wbsh

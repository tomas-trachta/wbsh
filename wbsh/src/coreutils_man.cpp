/**
 * @file coreutils_man.cpp
 * @brief man, apropos, whatis: manual pages for bundled commands.
 *
 * wbsh ships no roff pages. Every command it carries has a page built
 * from man_pages.h (name, whatis line, synopsis), with the command's own
 * `--help` text when it has one. A name wbsh does not know goes to an
 * external `man` when one is on PATH (MSYS2 has one, Git for Windows
 * does not). On a console the page goes through $MANPAGER, $PAGER or the
 * built-in less, as man-db does; exit status 16 means no page was found.
 */

#include "coreutils_internal.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "executor.h"
#include "man_pages.h"
#include "numparse.h"
#include "optscan.h"
#include "pathconv.h"
#include "posixregex.h"

namespace wbsh {

	namespace fs = std::filesystem;

	namespace man_detail {

		enum OptId { kOptApropos, kOptWhatis, kOptAll, kOptSection, kOptPager, kOptIgnored };

		enum class ManMode { Show, Apropos, Whatis };

		struct ManOptions {
			ManMode mode = ManMode::Show;
			std::string section;
			std::string pager;
			std::vector<std::string> names;
		};

	}  // namespace man_detail

	using man_detail::ManMode;
	using man_detail::ManOptions;

	static const int kNotFoundStatus = 16;
	static const int kDefaultWidth = 80;
	static const int kNarrowestWidth = 40;
	static const std::size_t kIndent = 7;
	static const std::size_t kWhatisColumn = 20;

	static const std::vector<OptDef>& manOptionDefs() {
		static const std::vector<OptDef> defs = {
			{ man_detail::kOptApropos, 'k', "apropos",  OptArg::None },
			{ man_detail::kOptWhatis,  'f', "whatis",   OptArg::None },
			{ man_detail::kOptAll,     'a', "all",      OptArg::None },
			{ man_detail::kOptSection, 's', "sections", OptArg::Required },
			{ man_detail::kOptSection, 'S', nullptr,    OptArg::Required },
			{ man_detail::kOptPager,   'P', "pager",    OptArg::Required },
			{ man_detail::kOptIgnored, 'E', "encoding", OptArg::Required },
		};
		return defs;
	}

	static bool isSectionName(const std::string& text) {
		if (text.empty()) return false;
		if (std::isdigit(static_cast<unsigned char>(text[0])) != 0) return true;
		return text == "n" || text == "l";
	}

	static bool parseManArgs(const std::vector<std::string>& args, ManMode mode,
	                         ManOptions& options) {
		OptScanResult scanned;
		if (!scanOptions("man", args, manOptionDefs(), scanned)) return false;

		options.mode = mode;
		for (const OptHit& hit : scanned.hits) {
			if (hit.id == man_detail::kOptApropos) options.mode = ManMode::Apropos;
			else if (hit.id == man_detail::kOptWhatis) options.mode = ManMode::Whatis;
			else if (hit.id == man_detail::kOptSection) options.section = hit.value;
			else if (hit.id == man_detail::kOptPager) options.pager = hit.value;
		}

		options.names = scanned.operands;
		const bool leading_section = options.mode == ManMode::Show && options.names.size() > 1
			&& isSectionName(options.names[0]);
		if (leading_section) {
			options.section = options.names[0];
			options.names.erase(options.names.begin());
		}

		return true;
	}

	static bool pageAvailable(Executor& exec, const ManPage& page) {
		return page.kind == ManKind::Program || exec.isBuiltin(page.name);
	}

	static bool sectionMatches(const ManOptions& options) {
		return options.section.empty() || options.section[0] == '1';
	}

	static std::string upper(const std::string& text) {
		std::string out = text;
		for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		return out;
	}

	static int pageWidth(Executor& exec) {
		int width = 0;
		if (parseInt(exec.env().get("MANWIDTH"), width) && width >= kNarrowestWidth) return width;

		int rows = 0;
		if (queryConsoleSize(width, rows) && width >= kNarrowestWidth) return width;
		return kDefaultWidth;
	}

	static std::string spreadLine(const std::string& left, const std::string& middle,
	                              const std::string& right, std::size_t width) {
		const std::size_t used = left.size() + middle.size() + right.size();
		const std::size_t room = width > used ? width - used : 2;
		const std::size_t before = room / 2;
		return left + std::string(before, ' ') + middle + std::string(room - before, ' ') + right;
	}

	static void appendIndented(std::string& out, const std::string& text) {
		std::size_t at = 0;
		while (at <= text.size()) {
			const std::size_t end = (std::min)(text.find('\n', at), text.size());
			const std::string line = text.substr(at, end - at);
			out += line.empty() ? "\n" : std::string(kIndent, ' ') + line + "\n";
			if (end == text.size()) break;
			at = end + 1;
		}
	}

	static void appendWrapped(std::string& out, const std::string& text, std::size_t width) {
		const std::size_t limit = width > kIndent + 20 ? width - kIndent : 20;
		std::string line;
		std::size_t at = 0;
		while ((at = text.find_first_not_of(' ', at)) != std::string::npos) {
			const std::size_t end = (std::min)(text.find(' ', at), text.size());
			const std::string word = text.substr(at, end - at);
			if (!line.empty() && line.size() + 1 + word.size() > limit) {
				appendIndented(out, line);
				line.clear();
			}

			line += (line.empty() ? "" : " ") + word;
			at = end;
		}

		if (!line.empty()) appendIndented(out, line);
	}

	static std::string descriptionText(const ManPage& page) {
		const std::string name = page.name;
		switch (page.kind) {
			case ManKind::Shell:
				return name + " is a shell builtin. It behaves as the bash builtin of the same"
					" name, which bash(1) documents in full.";
			case ManKind::Program:
				return "wbsh is a bash-compatible shell for Windows with the common POSIX"
					" utilities built in. `help` lists them and `man -k KEYWORD` searches"
					" their descriptions.";
			default:
				return name + " is built into wbsh and runs without an external program.";
		}
	}

	static std::string selfHelpText(Executor& exec, const ManPage& page) {
		if (!page.self_help) return std::string();
		return exec.run(quoteShellArgv({ page.name, "--help" }) + " 2>&1");
	}

	static std::string renderPage(Executor& exec, const ManPage& page, std::size_t width) {
		const std::string title = upper(page.name) + "(1)";
		std::string out = spreadLine(title, "wbsh Manual", title, width) + "\n\nNAME\n";
		appendIndented(out, std::string(page.name) + " - " + page.summary);
		out += "\nSYNOPSIS\n";
		appendIndented(out, page.synopsis);
		out += "\nDESCRIPTION\n";
		appendWrapped(out, descriptionText(page), width);

		const std::string help = selfHelpText(exec, page);
		if (!help.empty()) {
			out += "\n";
			appendIndented(out, help);
		}

		out += "\nSEE ALSO\n";
		const bool shell = page.kind == ManKind::Shell;
		appendIndented(out, shell ? "bash(1), help(1)" : "wbsh(1), apropos(1)");
		return out + "\n" + spreadLine("wbsh", "", title, width) + "\n";
	}

	static std::string pagerCommand(Executor& exec, const ManOptions& options) {
		if (!options.pager.empty()) return options.pager;

		const std::string manpager = exec.env().get("MANPAGER");
		if (!manpager.empty()) return manpager;

		const std::string pager = exec.env().get("PAGER");
		return pager.empty() ? "less" : pager;
	}

	static bool writeTextFile(const fs::path& path, const std::string& text) {
		std::FILE* out = openUtf8(pathToUtf8(path), "wb");
		if (out == nullptr) return false;

		const bool ok = std::fwrite(text.data(), 1, text.size(), out) == text.size();
		return std::fclose(out) == 0 && ok;
	}

	static void pageText(Executor& exec, const ManOptions& options, const std::string& text) {
		const fs::path directory = shellTempDirectory(exec);
		fs::path temp;
		std::FILE* reserved = directory.empty() ? nullptr
			: createSiblingTempFile(directory / "wbsh-man", "wbsh-man-", temp);
		if (reserved == nullptr) {
			std::fputs(text.c_str(), stdout);
			return;
		}

		std::fclose(reserved);
		if (writeTextFile(temp, text)) {
			const std::string shown = exec.pathConv().toPosix(pathToUtf8(temp));
			const std::string input = " < " + quoteShellArgv({ shown });
			exec.executeText(pagerCommand(exec, options) + input, "man");
		}

		std::error_code ec;
		fs::remove(temp, ec);
	}

	static void showText(Executor& exec, const ManOptions& options, const std::string& text) {
		if (text.empty()) return;
		if (isConsoleStream(stdout)) pageText(exec, options, text);
		else std::fputs(text.c_str(), stdout);
		std::fflush(stdout);
	}

	static bool delegateToExternal(Executor& exec, const ManOptions& options,
	                               const std::string& name, int& status) {
		if (exec.findExecutable("man").empty()) return false;

		std::vector<std::string> argv = { "man" };
		if (!options.section.empty()) argv.push_back(options.section);
		argv.push_back(name);
		status = exec.runExternal(argv, {});
		return true;
	}

	static void reportMissing(const ManOptions& options, const std::string& name) {
		if (options.section.empty()) perr("man", "No manual entry for " + name);
		else perr("man", "No manual entry for " + name + " in section " + options.section);
	}

	static int showPages(Executor& exec, const ManOptions& options) {
		std::string text;
		int status = 0;
		const std::size_t width = static_cast<std::size_t>(pageWidth(exec));
		for (const std::string& name : options.names) {
			const ManPage* page = findManPage(name);
			if (page != nullptr && pageAvailable(exec, *page) && sectionMatches(options)) {
				text += (text.empty() ? "" : "\n") + renderPage(exec, *page, width);
				continue;
			}

			int external = 0;
			if (delegateToExternal(exec, options, name, external)) {
				if (external != 0) status = external;
				continue;
			}

			reportMissing(options, name);
			status = kNotFoundStatus;
		}

		showText(exec, options, text);
		return status;
	}

	static std::string whatisLine(const ManPage& page) {
		std::string head = std::string(page.name) + " (1)";
		if (head.size() < kWhatisColumn) head.resize(kWhatisColumn, ' ');
		return head + " - " + page.summary + "\n";
	}

	static bool sameNameIgnoringCase(const std::string& a, const char* b) {
		const std::string other = b;
		return a.size() == other.size() && upper(a) == upper(other);
	}

	static bool keywordMatches(const PosixRegex& regex, const ManPage& page) {
		return regex.test(page.name) || regex.test(page.summary);
	}

	static bool compileKeyword(const std::string& keyword, PosixRegex& regex) {
		RegexOptions regex_options;
		regex_options.syntax = RegexSyntax::Extended;
		regex_options.ignore_case = true;
		if (regex.compile(keyword, regex_options)) return true;

		perr("apropos", keyword + ": " + regex.error());
		return false;
	}

	static bool searchKeyword(Executor& exec, const ManOptions& options, const std::string& keyword,
	                          const PosixRegex& regex, std::string& out) {
		bool found = false;
		for (const ManPage* page = manPagesBegin(); page != manPagesEnd(); ++page) {
			if (!pageAvailable(exec, *page)) continue;

			const bool hit = options.mode == ManMode::Whatis
				? sameNameIgnoringCase(keyword, page->name)
				: keywordMatches(regex, *page);
			if (!hit) continue;
			out += whatisLine(*page);
			found = true;
		}

		return found;
	}

	static int searchPages(Executor& exec, const ManOptions& options) {
		const char* tool = options.mode == ManMode::Whatis ? "whatis" : "apropos";
		if (options.names.empty()) {
			perr(tool, "what?");
			return 1;
		}

		std::string out;
		int status = 0;
		for (const std::string& keyword : options.names) {
			PosixRegex regex;
			if (options.mode == ManMode::Apropos && !compileKeyword(keyword, regex)) {
				status = 1;
				continue;
			}

			if (searchKeyword(exec, options, keyword, regex, out)) continue;

			std::fprintf(stderr, "%s: nothing appropriate.\n", keyword.c_str());
			status = kNotFoundStatus;
		}

		std::fputs(out.c_str(), stdout);
		std::fflush(stdout);
		return status;
	}

	static int runMan(Executor& exec, const std::vector<std::string>& args, ManMode mode) {
		ManOptions options;
		if (!parseManArgs(args, mode, options)) return 1;
		if (options.mode != ManMode::Show) return searchPages(exec, options);
		if (options.names.empty()) {
			std::fprintf(stderr, "What manual page do you want?\nFor example, try 'man man'.\n");
			return 1;
		}

		return showPages(exec, options);
	}

	static int builtin_man(Executor& exec, const std::vector<std::string>& args) {
		return runMan(exec, args, ManMode::Show);
	}

	static int builtin_apropos(Executor& exec, const std::vector<std::string>& args) {
		return runMan(exec, args, ManMode::Apropos);
	}

	static int builtin_whatis(Executor& exec, const std::vector<std::string>& args) {
		return runMan(exec, args, ManMode::Whatis);
	}

	void registerManBuiltin(Executor& exec) {
		exec.registerBuiltin("man",     builtin_man);
		exec.registerBuiltin("apropos", builtin_apropos);
		exec.registerBuiltin("whatis",  builtin_whatis);
	}

}  // namespace wbsh

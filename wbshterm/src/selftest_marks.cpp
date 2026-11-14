/**
 * @file selftest_marks.cpp
 * @brief Self-test checks for what the shell's marks feed: a block's command
 *        line and timing, Ctrl+click paths, and the saved pane layout.
 */

#include "selftest_internal.h"

#include "file_links.h"
#include "layout_store.h"

#include <string>
#include <vector>

namespace wbshterm {

	namespace test {

		static std::vector<std::string> cellsOf(const std::string& ascii) {
			std::vector<std::string> cells;
			for (const char letter : ascii) cells.push_back(std::string(1, letter));
			return cells;
		}

		static FileLink linkIn(const std::string& row, int column) {
			FileLink link;
			if (!findFileLink(cellsOf(row), column, link)) link.path = "<none>";
			return link;
		}

		static void checkMarksCarryCommandLine(Report& report) {
			Screen screen(40, 6);
			feedToScreen(screen, "\x1b]133;A\a$ \x1b]133;B\a"
				"\x1b]633;E;echo a\\x3bb \\\\x\a\x1b]133;C\aa\r\n\x1b]133;D;2\a");

			const std::vector<CommandBlock>& blocks = screen.commandBlocks();
			const bool one = blocks.size() == 1;
			report.check("OSC 633;E gives a block its command line",
				one && blocks[0].command == "echo a;b \\x", one ? blocks[0].command : "");
			report.check("OSC 133;B marks where the command is typed",
				one && blocks[0].command_column == 2 && blocks[0].command_offset == 0, "");
			report.check("a finished block knows when it started and ended",
				one && blocks[0].started_ms != 0 && blocks[0].ended_ms >= blocks[0].started_ms
				&& blocks[0].exit_status == 2, "");
			report.check("a row of the prompt finds its block",
				screen.blockAt(blocks.empty() ? 0 : blocks[0].prompt_row) == &blocks[0], "");
		}

		static void checkFileLinksAreFound(Report& report) {
			const FileLink grep = linkIn("src/main.cpp:42:7: error: x", 3);
			const FileLink msvc = linkIn("x main.cpp(12,4): warning", 4);
			const FileLink plain = linkIn("see (docs/readme.md).", 8);
			const FileLink word = linkIn("hello world", 2);

			report.check("a path:line:column is one link",
				grep.path == "src/main.cpp" && grep.line == 42 && grep.column == 7, grep.path);
			report.check("MSVC's file(line,column) is one link",
				msvc.path == "main.cpp" && msvc.line == 12 && msvc.column == 4, msvc.path);
			report.check("brackets and full stops around a path are not part of it",
				plain.path == "docs/readme.md" && plain.line == 0, plain.path);
			report.check("a plain word is no link", word.path == "<none>", word.path);
		}

		static void checkLinkPathsResolve(Report& report) {
			report.check("a relative link is read from the shell's directory",
				resolveLinkPath("src/a.c", "C:/proj", "") == "C:/proj/src/a.c", "");
			report.check("an MSYS /c/ path is a drive path",
				resolveLinkPath("/c/Users/me/x.txt", "C:/proj", "") == "C:/Users/me/x.txt", "");
			report.check("~ is the user's home",
				resolveLinkPath("~/x.txt", "C:/proj", "C:/Users/me") == "C:/Users/me/x.txt", "");

			FileLink link;
			link.line = 9;
			report.check("the editor command gets the file and line",
				editorCommandFor("code -g \"{file}:{line}:{column}\"", "C:/a.c", link)
					== "code -g \"C:/a.c:9:1\"", "");
			report.check("programs and scripts are not opened by a click",
				linkRunsSomething("C:/x/setup.EXE") && linkRunsSomething("run.cmd")
				&& !linkRunsSomething("notes.txt"), "");
		}

		static void checkSavedLayoutReadsBack(Report& report) {
			const std::string text = "wbshterm-layout 1\n"
				"split columns 0.300\n"
				"pane C:/a dir\n"
				"split rows 0.500\n"
				"pane C:/b\n"
				"pane\n";

			SavedPane root;
			const bool parsed = parseLayout(text, root);
			const bool shape = parsed && !root.leaf && root.axis == SplitAxis::Columns
				&& root.first && root.first->leaf && root.second && !root.second->leaf
				&& root.second->axis == SplitAxis::Rows;
			report.check("a saved layout reads back as its tree", shape, "");
			report.check("a saved pane keeps a directory with spaces",
				shape && root.first->directory == "C:/a dir"
				&& firstSavedPane(*root.second).directory == "C:/b", "");

			SavedPane cut;
			report.check("a layout cut short is refused",
				!parseLayout("wbshterm-layout 1\nsplit rows 0.5\npane C:/x\n", cut), "");
		}

	} /* namespace test */

	void runMarkChecks(test::Report& report) {
		test::checkMarksCarryCommandLine(report);
		test::checkFileLinksAreFound(report);
		test::checkLinkPathsResolve(report);
		test::checkSavedLayoutReadsBack(report);
	}

} /* namespace wbshterm */

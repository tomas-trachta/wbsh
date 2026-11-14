/**
 * @file window_blocks.cpp
 * @brief What the window does with the shell's marks and its directory:
 *        copying or re-running one command, opening a printed path with
 *        Ctrl+click, and keeping the panes for the next start.
 */

#include "window.h"

#include "file_links.h"
#include "layout_store.h"
#include "utf8.h"

#include <shellapi.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace wbshterm {

	namespace blocks_detail {

		static const char kLineDiscard = '\x15';

		static std::vector<std::string> rowCells(const Screen& screen, int absolute_row) {
			std::vector<std::string> cells;
			cells.reserve(static_cast<std::size_t>(screen.columns()));
			for (int column = 0; column < screen.columns(); ++column) {
				const Cell& cell = screen.cellAt(absolute_row, column);
				std::string text;
				if ((cell.attributes & kAttrWideTail) == 0) screen.appendCellText(text, cell);
				cells.push_back(text);
			}

			return cells;
		}

		static bool pathExists(const std::string& path, bool& out_directory) {
			const DWORD attributes = ::GetFileAttributesW(widenUtf8(path).c_str());
			if (attributes == INVALID_FILE_ATTRIBUTES) return false;

			out_directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
			return true;
		}

		static std::wstring backslashed(const std::string& path) {
			std::wstring wide = widenUtf8(path);
			for (wchar_t& letter : wide) {
				if (letter == L'/') letter = L'\\';
			}

			return wide;
		}

		static std::string homeDirectory() {
			wchar_t* profile = nullptr;
			std::size_t length = 0;
			if (_wdupenv_s(&profile, &length, L"USERPROFILE") != 0 || profile == nullptr) {
				return std::string();
			}

			std::string home = narrowUtf16(profile);
			std::free(profile);
			for (char& letter : home) {
				if (letter == '\\') letter = '/';
			}

			return home;
		}

		// The editor runs through cmd, as typing it would: `code` is a
		// batch file, and CreateProcess alone cannot start one.
		static void runEditor(const std::string& command) {
			std::wstring line = L"cmd.exe /d /c " + widenUtf8(command);
			STARTUPINFOW startup{};
			startup.cb = sizeof(startup);
			PROCESS_INFORMATION process{};
			if (!::CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE,
					CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
				return;
			}

			::CloseHandle(process.hThread);
			::CloseHandle(process.hProcess);
		}

		// A program or script is shown in Explorer rather than run: a click
		// on text a command printed must never start anything.
		static void showFile(const std::string& path, bool directory) {
			const std::wstring native = backslashed(path);
			if (directory || !linkRunsSomething(path)) {
				::ShellExecuteW(nullptr, L"open", native.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
				return;
			}

			const std::wstring select = L"/select,\"" + native + L"\"";
			::ShellExecuteW(nullptr, L"open", L"explorer.exe", select.c_str(), nullptr,
				SW_SHOWNORMAL);
		}

	}  // namespace blocks_detail

	// Scrolled back, the command whose prompt is at the top of the view is
	// the one being read; at the bottom, the last one that ran.
	const CommandBlock* TerminalWindow::currentBlock() const {
		const Screen& screen = focused().screen();
		const TerminalView& view = focused().view();
		const int bottom_top = screen.totalRows() - screen.rows();
		if (view.topRow(screen) < bottom_top) return screen.blockAt(view.topRow(screen));

		const std::vector<CommandBlock>& blocks = screen.commandBlocks();
		for (auto block = blocks.rbegin(); block != blocks.rend(); ++block) {
			if (block->finished && block->output_row >= 0) return &*block;
		}

		return nullptr;
	}

	void TerminalWindow::copyBlockOutput(const CommandBlock& block) {
		if (block.output_row < 0) return;

		focused().view().selectBlockOutput(block, focused().screen());
		copySelection();
		::InvalidateRect(window_, nullptr, FALSE);
	}

	void TerminalWindow::selectBlock(const CommandBlock& block) {
		focused().view().selectBlockOutput(block, focused().screen());
		::InvalidateRect(window_, nullptr, FALSE);
	}

	// The line being typed is cleared first, and the command goes in the
	// way a paste does, so a multi-line one arrives whole.
	void TerminalWindow::rerunBlock(const CommandBlock& block) {
		if (block.command.empty() || !focused().screen().atPrompt()) {
			::MessageBeep(MB_OK);
			return;
		}

		const std::string command = block.command;
		const std::string bytes = std::string(1, blocks_detail::kLineDiscard)
			+ encodePaste(command, currentModes()) + "\r";
		focused().view().scrollToBottom(focused().screen());
		sendBytes(bytes.data(), bytes.size());
	}

	void TerminalWindow::copyCommandOutput() {
		const CommandBlock* block = currentBlock();
		if (block != nullptr) copyBlockOutput(*block);
	}

	void TerminalWindow::rerunCommand() {
		const CommandBlock* block = currentBlock();
		if (block != nullptr) rerunBlock(*block);
	}

	// Ctrl+click on a path the shell or a tool printed: the file, in the
	// configured editor at its line when there is one, else as Windows
	// opens it. Relative paths are read from the shell's directory.
	bool TerminalWindow::openPathAt(const PaneNode& leaf, const GridPoint& point) {
		if ((::GetKeyState(VK_CONTROL) & 0x8000) == 0) return false;

		const Screen& screen = leaf.pane()->screen();
		FileLink link;
		if (!findFileLink(blocks_detail::rowCells(screen, point.row), point.column, link)) {
			return false;
		}

		const std::string path = resolveLinkPath(link.path, screen.workingDirectory(),
			blocks_detail::homeDirectory());
		bool directory = false;
		if (!blocks_detail::pathExists(path, directory)) return false;

		if (!directory && !config_.shell.editor.empty()) {
			blocks_detail::runEditor(editorCommandFor(config_.shell.editor, path, link));
		} else {
			blocks_detail::showFile(path, directory);
		}

		return true;
	}

	void TerminalWindow::saveLayout() const {
		if (!config_.shell.restore_session || panes_.focused() == nullptr) return;

		const std::wstring path = layoutFilePath(config_path_);
		if (path.empty()) return;

		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file << layoutText(*panes_.focused());
	}

	bool TerminalWindow::loadLayout(SavedPane& out_root) const {
		if (!config_.shell.restore_session) return false;

		const std::wstring path = layoutFilePath(config_path_);
		std::ifstream file(path, std::ios::binary);
		if (path.empty() || !file) return false;

		std::ostringstream text;
		text << file.rdbuf();
		return parseLayout(text.str(), out_root);
	}

	bool TerminalWindow::splitIntoDirectory(SplitAxis axis, const std::string& directory) {
		directory_hint_ = nativeDirectory(directory);

		auto added = std::make_unique<Pane>();
		Pane* fresh = added.get();
		panes_.splitFocused(axis, std::move(added));
		layoutPanes();

		std::string error;
		if (startPane(*fresh, error)) return true;

		panes_.close(panes_.focused());
		layoutPanes();
		return false;
	}

	// A split keeps the old pane first, so a saved branch is rebuilt by
	// splitting off its second half, then filling in each half on its own.
	void TerminalWindow::rebuildSplits(const SavedPane& node, PaneNode* leaf) {
		if (node.leaf || leaf == nullptr || !node.first || !node.second) return;

		panes_.focusOn(leaf);
		const std::string& directory = firstSavedPane(*node.second).directory;
		if (!splitIntoDirectory(node.axis, directory)) return;

		PaneNode* second = panes_.focused();
		PaneNode* branch = second->parent();
		branch->setRatio(node.ratio);
		rebuildSplits(*node.second, second);
		rebuildSplits(*node.first, branch->first());
	}

	// Splits only exist in pane mode, so a restored layout of more than
	// one pane comes back with it on.
	void TerminalWindow::restoreSplits(const SavedPane& root) {
		if (root.leaf) return;

		const Pane* original = panes_.focused()->pane();
		rebuildSplits(root, panes_.focused());
		for (PaneNode* leaf : panes_.leaves()) {
			if (leaf->pane() == original) panes_.focusOn(leaf);
		}

		tmuxAttach();
		commitGridSizes();
		syncTitle();
	}

} /* namespace wbshterm */

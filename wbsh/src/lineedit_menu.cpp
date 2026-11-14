/**
 * @file lineedit_menu.cpp
 * @brief Menu selection: the matches of an ambiguous completion as a grid
 *        below the line, walked with Tab, Shift-Tab and the arrow keys.
 *
 * The selected match stands in the line while the grid is open, as with
 * menu-complete. Enter takes it as a finished word, Esc puts back the word
 * as it was typed, and any other key keeps the match and is then handled
 * as usual. The grid lives in the redraw: it is drawn below the line every
 * time the line is, and erased with it.
 */

#include "lineedit.h"

#include <algorithm>
#include <string>
#include <vector>

#include "keymap.h"
#include "textcells.h"

namespace wbsh {

	namespace menu_detail {

		static const std::size_t kNone = static_cast<std::size_t>(-1);
		static const std::size_t kGap = 2;
		static const char* const kSelectedStyle = "\x1b[7m";
		static const char* const kDescriptionStyle = "\x1b[2m";
		static const char* const kPlainStyle = "\x1b[0m";

		static std::size_t cellsOf(const std::string& text) {
			return displayWidth(text, text.size());
		}

		// As much of the text as fits in `cells`; a wide character that
		// would straddle the edge is left out whole.
		static std::string fitted(const std::string& text, std::size_t cells) {
			std::size_t used = 0;
			std::size_t at = 0;
			while (at < text.size()) {
				const TextCluster cluster = clusterAt(text, at);
				if (used + cluster.width > cells) break;

				used += cluster.width;
				at = cluster.end;
			}

			return text.substr(0, at);
		}

		static std::string padded(const std::string& text, std::size_t cells) {
			const std::string shown = fitted(text, cells);
			return shown + std::string(cells - cellsOf(shown), ' ');
		}

		static std::size_t longestCells(const std::vector<std::string>& texts) {
			std::size_t longest = 0;
			for (const std::string& text : texts) longest = (std::max)(longest, cellsOf(text));
			return longest;
		}

		static bool anyText(const std::vector<std::string>& texts) {
			return std::any_of(texts.begin(), texts.end(),
				[](const std::string& text) { return !text.empty(); });
		}

		// The last cell of a row is never written, so a row that fills the
		// screen cannot wrap on a console that wraps eagerly.
		static std::size_t usableWidth(std::size_t width) {
			return width > 1 ? width - 1 : 1;
		}

	}  // namespace menu_detail

	bool LineEditor::menuSelectWanted() const {
		return shellKeymap().isOn("menu-select");
	}

	void LineEditor::runMenuSelect(const Tok& tok, const std::vector<std::string>& matches,
	                               const UndoPoint& origin) {
		fillMenuGrid(matches, origin);
		menu_tok_ = tok;
		menu_end_ = cursor_;
		if (!shellKeymap().isOn("menu-complete-display-prefix")) selectMenuItem(0);

		runMenuGrid();
	}

	void LineEditor::fillMenuGrid(const std::vector<std::string>& matches,
	                              const UndoPoint& origin) {
		menu_grid_ = MenuGrid();
		menu_grid_.matches = matches;
		menu_grid_.labels = listedNames(matches);
		for (const std::string& match : matches) {
			menu_grid_.descriptions.push_back(describeMatch(match));
		}

		if (!menu_detail::anyText(menu_grid_.descriptions)) menu_grid_.descriptions.clear();
		menu_grid_.origin = origin;
	}

	// Enter finishes the word as a lone match would be; Esc gives the
	// typed word back; any other way out leaves the match as it stands.
	void LineEditor::finishMenuGrid(MenuOutcome outcome) {
		const std::size_t selected = menu_grid_.selected;
		if (outcome == MenuOutcome::kCancel) {
			buffer_ = menu_grid_.origin.buffer;
			cursor_ = menu_grid_.origin.cursor;
			return;
		}

		if (outcome != MenuOutcome::kAccept || selected == menu_detail::kNone) return;

		Tok tok = menu_tok_;
		tok.end = menu_end_;
		insertMatch(tok, menu_grid_.matches[selected], true);
	}

	void LineEditor::selectMenuItem(std::size_t index) {
		menu_grid_.selected = index;

		Tok tok = menu_tok_;
		tok.end = menu_end_;
		insertMatch(tok, menu_grid_.matches[index], false);
		menu_end_ = cursor_;
	}

	// Tab and Down go on to the next match, which in a grid filled column
	// by column is the one below; past the last comes the first.
	void LineEditor::stepMenuSelection(bool forward) {
		const std::size_t count = menu_grid_.matches.size();
		const std::size_t selected = menu_grid_.selected;
		if (selected == menu_detail::kNone) {
			selectMenuItem(forward ? 0 : count - 1);
			return;
		}

		selectMenuItem(forward ? (selected + 1) % count : (selected + count - 1) % count);
	}

	// Right and Left move a whole column, wrapping to the next or previous
	// row at either edge.
	void LineEditor::stepMenuColumn(bool forward) {
		const std::size_t count = menu_grid_.matches.size();
		const std::size_t rows = std::max<std::size_t>(1, menu_grid_.layout.rows);
		const std::size_t selected = menu_grid_.selected == menu_detail::kNone
			? 0 : menu_grid_.selected;
		if (forward) {
			const std::size_t next = selected + rows;
			selectMenuItem(next < count ? next : (selected % rows + 1) % rows);
			return;
		}

		if (selected >= rows) {
			selectMenuItem(selected - rows);
			return;
		}

		const std::size_t row = (selected % rows + rows - 1) % rows;
		selectMenuItem((count - 1 - row) / rows * rows + row);
	}

	void LineEditor::stepMenuPage(bool forward) {
		const std::size_t count = menu_grid_.matches.size();
		const std::size_t page = std::max<std::size_t>(1, menu_grid_.visible);
		const std::size_t selected = menu_grid_.selected == menu_detail::kNone
			? 0 : menu_grid_.selected;
		if (forward) {
			selectMenuItem((std::min)(selected + page, count - 1));
			return;
		}

		selectMenuItem(selected > page ? selected - page : 0);
	}

	// Filled column by column as the classic listing is, unless a match
	// has a description: then one match a row, its description beside it.
	LineEditor::MenuLayout LineEditor::menuLayout(std::size_t width) const {
		const std::size_t usable = menu_detail::usableWidth(width);
		const std::size_t count = menu_grid_.matches.size();

		MenuLayout layout;
		layout.label = (std::min)(menu_detail::longestCells(menu_grid_.labels), usable);
		layout.described = !menu_grid_.descriptions.empty();
		layout.cell = (std::min)(layout.label + menu_detail::kGap, usable);
		layout.columns = layout.described ? 1 : std::max<std::size_t>(1, usable / layout.cell);
		layout.rows = (count + layout.columns - 1) / layout.columns;
		return layout;
	}

	// Rows the screen has room for below the line; a grid taller than
	// that scrolls, with a line that says which rows are shown.
	std::vector<std::string> LineEditor::menuLines(std::size_t budget) {
		const std::size_t width = screenColumns();
		menu_grid_.layout = menuLayout(width);

		const std::size_t rows = menu_grid_.layout.rows;
		const bool scrolls = rows > budget;
		const std::size_t visible = scrolls ? std::max<std::size_t>(1, budget - 1) : rows;
		menu_grid_.visible = visible;
		scrollMenuToSelection(visible);

		std::vector<std::string> lines;
		for (std::size_t row = menu_grid_.top_row; row < menu_grid_.top_row + visible; ++row) {
			lines.push_back(menuRow(row, width));
		}

		if (scrolls && budget > 1) lines.push_back(menuStatusLine());
		return lines;
	}

	void LineEditor::scrollMenuToSelection(std::size_t visible) {
		const std::size_t rows = menu_grid_.layout.rows;
		std::size_t& top = menu_grid_.top_row;
		top = (std::min)(top, rows > visible ? rows - visible : 0);
		if (menu_grid_.selected == menu_detail::kNone) return;

		const std::size_t row = menu_grid_.selected % std::max<std::size_t>(1, rows);
		if (row < top) top = row;
		if (row >= top + visible) top = row + 1 - visible;
	}

	std::string LineEditor::menuRow(std::size_t row, std::size_t width) const {
		const MenuLayout& layout = menu_grid_.layout;
		std::string line;
		for (std::size_t column = 0; column < layout.columns; ++column) {
			const std::size_t index = column * layout.rows + row;
			if (index >= menu_grid_.matches.size()) break;

			line += menuCell(index, width);
		}

		return line;
	}

	std::string LineEditor::menuCell(std::size_t index, std::size_t width) const {
		const MenuLayout& layout = menu_grid_.layout;
		const bool selected = index == menu_grid_.selected;
		const std::string label = menu_detail::padded(menu_grid_.labels[index], layout.label);
		std::string cell = selected
			? menu_detail::kSelectedStyle + label + menu_detail::kPlainStyle : label;
		const std::size_t gap = layout.cell - menu_detail::cellsOf(label);
		cell.append(gap, ' ');
		if (!layout.described) return cell;

		const std::size_t room = menu_detail::usableWidth(width) - layout.cell;
		const std::string description = menu_detail::fitted(menu_grid_.descriptions[index], room);
		if (description.empty()) return cell;

		return cell + menu_detail::kDescriptionStyle + description + menu_detail::kPlainStyle;
	}

	std::string LineEditor::menuStatusLine() const {
		const std::size_t first = menu_grid_.top_row + 1;
		const std::size_t last = menu_grid_.top_row + menu_grid_.visible;
		const std::string text = "rows " + std::to_string(first) + "-" + std::to_string(last)
			+ " of " + std::to_string(menu_grid_.layout.rows);
		return menu_detail::kDescriptionStyle
			+ menu_detail::fitted(text, menu_detail::usableWidth(screenColumns()))
			+ menu_detail::kPlainStyle;
	}

	// Called by redraw with the line drawn; `used_rows` is how many rows
	// the line takes. Returns the rows added below the line's last one.
	std::size_t LineEditor::appendMenuRows(std::string& out, bool at_row_start,
	                                       std::size_t used_rows) {
		if (!menu_grid_.active) return 0;

		const std::size_t screen = screenRows();
		if (used_rows >= screen) return 0;

		const std::vector<std::string> lines = menuLines(screen - used_rows);
		if (lines.empty()) return 0;

		if (!at_row_start) out += "\r\n";
		for (std::size_t i = 0; i < lines.size(); ++i) {
			if (i > 0) out += "\r\n";
			out += lines[i];
		}

		return at_row_start ? lines.size() - 1 : lines.size();
	}

#ifdef _WIN32

	namespace menu_detail {

		static bool held(const KEY_EVENT_RECORD& key, DWORD flags) {
			return (key.dwControlKeyState & flags) != 0;
		}

		static bool isCancelKey(const KEY_EVENT_RECORD& key) {
			const bool ctrl = held(key, LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED);
			const bool alt = held(key, LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED);
			return key.wVirtualKeyCode == VK_ESCAPE || (ctrl && !alt && key.wVirtualKeyCode == 'G');
		}

		static bool isInterrupt(const KEY_EVENT_RECORD& key) {
			const bool ctrl = held(key, LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED);
			const bool alt = held(key, LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED);
			return ctrl && !alt && key.wVirtualKeyCode == 'C';
		}

	}  // namespace menu_detail

	bool LineEditor::menuMoveKey(const KEY_EVENT_RECORD& key) {
		const bool shift = menu_detail::held(key, SHIFT_PRESSED);
		switch (key.wVirtualKeyCode) {
		case VK_TAB:
			stepMenuSelection(!shift);
			return true;
		case VK_DOWN:
		case VK_UP:
			stepMenuSelection(key.wVirtualKeyCode == VK_DOWN);
			return true;
		case VK_RIGHT:
		case VK_LEFT:
			stepMenuColumn(key.wVirtualKeyCode == VK_RIGHT);
			return true;
		case VK_NEXT:
		case VK_PRIOR:
			stepMenuPage(key.wVirtualKeyCode == VK_NEXT);
			return true;
		default:
			return false;
		}
	}

	// Ctrl-C gives the word back and then interrupts the line as usual.
	LineEditor::MenuOutcome LineEditor::menuHandleKey(const KEY_EVENT_RECORD& key,
	                                                  bool& redispatch) {
		redispatch = false;
		if (isModifierOnlyKey(key)) return MenuOutcome::kStay;
		if (menuMoveKey(key)) return MenuOutcome::kStay;
		if (menu_detail::isCancelKey(key)) return MenuOutcome::kCancel;

		const bool plain = !menu_detail::held(key, LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED
			| LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED);
		if (key.wVirtualKeyCode == VK_RETURN && plain) return MenuOutcome::kAccept;

		redispatch = true;
		return menu_detail::isInterrupt(key) ? MenuOutcome::kCancel : MenuOutcome::kKeep;
	}

	void LineEditor::runMenuGrid() {
		menu_grid_.active = true;
		redraw();

		KEY_EVENT_RECORD key{};
		bool redispatch = false;
		MenuOutcome outcome = MenuOutcome::kStay;
		while (outcome == MenuOutcome::kStay) {
			if (!nextKeyDown(key)) {
				outcome = MenuOutcome::kKeep;
				break;
			}

			outcome = menuHandleKey(key, redispatch);
			if (outcome == MenuOutcome::kStay) redraw();
		}

		menu_grid_.active = false;
		finishMenuGrid(outcome);
		redraw();
		if (redispatch) dispatchKeyDown(key);
	}

#else  /* !_WIN32 */

	void LineEditor::runMenuGrid() {
		finishMenuGrid(MenuOutcome::kKeep);
	}

#endif /* _WIN32 */

}  // namespace wbsh

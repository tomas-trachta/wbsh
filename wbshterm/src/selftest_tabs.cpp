/**
 * @file selftest_tabs.cpp
 * @brief The tab checks: order and selection, the strip's geometry and
 *        hit-testing, the session file with tabs in it, and [tabs].
 */

#include "selftest_internal.h"

#include "config.h"
#include "layout_store.h"
#include "tab_strip.h"
#include "tabs.h"

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace wbshterm {

	namespace test {

		static TabList listOf(std::size_t count) {
			TabList tabs;
			for (std::size_t index = 0; index < count; ++index) {
				tabs.insert(tabs.count(), std::make_unique<Tab>());
			}

			return tabs;
		}

		static void checkTabsOpenAndStep(Report& report) {
			TabList tabs = listOf(3);
			report.check("a tab that opens is the one shown",
				tabs.count() == 3 && tabs.activeIndex() == 2, "");

			const Tab* placed = &tabs.insert(1, std::make_unique<Tab>());
			report.check("a tab opened beside another lands there and is shown",
				tabs.activeIndex() == 1 && tabs.indexOf(placed) == 1, "");

			tabs.insert(99, std::make_unique<Tab>());
			report.check("a place past the end is the end", tabs.activeIndex() == 4, "");

			tabs.step(1);
			report.check("stepping past the last tab wraps to the first",
				tabs.activeIndex() == 0, "");
			tabs.step(-1);
			report.check("stepping back past the first wraps to the last",
				tabs.activeIndex() == 4, "");
			tabs.step(-7);
			report.check("a long step wraps as often as it needs",
				tabs.activeIndex() == 2, std::to_string(tabs.activeIndex()));
		}

		static void checkTabsClose(Report& report) {
			TabList tabs = listOf(4);
			tabs.activate(1);
			const Tab* second = &tabs.at(2);

			tabs.remove(1);
			report.check("closing the shown tab shows the one that takes its place",
				tabs.activeIndex() == 1 && &tabs.active() == second, "");

			const Tab* shown = &tabs.active();
			tabs.remove(0);
			report.check("closing a tab before the shown one keeps it shown",
				&tabs.active() == shown && tabs.activeIndex() == 0, "");

			tabs.activate(tabs.count() - 1);
			tabs.remove(tabs.count() - 1);
			report.check("closing the last tab, shown, shows the one before it",
				tabs.count() == 1 && tabs.activeIndex() == 0, "");

			report.check("closing a tab that is not there hands nothing back",
				tabs.remove(5) == nullptr && tabs.count() == 1, "");
		}

		static void checkTabsMove(Report& report) {
			TabList tabs = listOf(4);
			tabs.activate(1);
			const Tab* shown = &tabs.active();
			const Tab* first = &tabs.at(0);

			tabs.move(1, 3);
			report.check("a moved tab lands where it was put",
				tabs.indexOf(shown) == 3 && tabs.indexOf(first) == 0, "");
			report.check("the shown tab stays shown when it moves", &tabs.active() == shown, "");

			tabs.move(0, 2);
			report.check("moving another tab past the shown one keeps it shown",
				&tabs.active() == shown && tabs.indexOf(first) == 2, "");

			tabs.move(0, 9);
			report.check("a move past the end does nothing", tabs.indexOf(first) == 2, "");
		}

		static TabStrip stripOf(int count, int active) {
			TabStrip strip;
			strip.applyMetrics(TabStripMetrics());
			strip.setBounds(D2D1::RectF(0.0f, 0.0f, 600.0f, 38.0f));
			strip.setTabs(count, active);
			return strip;
		}

		static D2D1_POINT_2F middleOf(const D2D1_RECT_F& rect) {
			return D2D1::Point2F((rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f);
		}

		// 600 wide: 8 before the first tab, then 32 + 4 for the new-tab
		// button and 46 left to drag the window by, which leaves 510.
		static void checkTabStripGeometry(Report& report) {
			const TabStrip two = stripOf(2, 0);
			report.check("a few tabs are as wide as a tab gets",
				two.tabWidth() == 220.0f, std::to_string(two.tabWidth()));
			report.check("tabs sit side by side from the strip's start",
				two.tabRect(0).left == 8.0f && two.tabRect(1).left == 228.0f, "");
			report.check("tabs leave room above them and reach the bottom",
				two.tabRect(0).top == 7.0f && two.tabRect(0).bottom == 38.0f, "");
			report.check("the new-tab button follows the last tab",
				two.newRect().left == 452.0f, std::to_string(two.newRect().left));

			const TabStrip ten = stripOf(10, 0);
			report.check("many tabs share the room",
				ten.tabWidth() == 51.0f, std::to_string(ten.tabWidth()));

			const TabStrip crowd = stripOf(40, 0);
			report.check("a crowd of tabs stops shrinking at the minimum",
				crowd.tabWidth() == 28.0f, std::to_string(crowd.tabWidth()));

			const TabStrip none = stripOf(0, 0);
			report.check("with no tabs the new-tab button starts the strip",
				none.newRect().left == 8.0f && none.tabWidth() == 0.0f, "");
		}

		static void checkTabStripHitTesting(Report& report) {
			TabStrip strip = stripOf(2, 0);

			const D2D1_POINT_2F close = middleOf(strip.closeRect(1));
			const D2D1_POINT_2F plus = middleOf(strip.newRect());
			report.check("a tab's label is the tab",
				strip.hitAt(40.0f, 20.0f) == TabHit{ TabPart::Tab, 0 }, "");
			report.check("a tab's close mark is its own part",
				strip.hitAt(close.x, close.y) == TabHit{ TabPart::Close, 1 }, "");
			report.check("the new-tab button answers for itself",
				strip.hitAt(plus.x, plus.y).part == TabPart::New, "");
			report.check("above the tabs is the caption, for dragging",
				strip.hitAt(40.0f, 3.0f).part == TabPart::None, "");
			report.check("past the new-tab button is the caption too",
				strip.hitAt(560.0f, 20.0f).part == TabPart::None, "");
			report.check("outside the strip is not the strip",
				!strip.holdsPoint(40.0f, 50.0f)
					&& strip.hitAt(40.0f, 50.0f).part == TabPart::None, "");

			report.check("a new hover is a change worth repainting",
				strip.setHovered(TabHit{ TabPart::Tab, 1 }), "");
			report.check("the same hover twice is not",
				!strip.setHovered(TabHit{ TabPart::Tab, 1 }), "");
			report.check("the shown tab and the hovered one show their close marks",
				strip.closeShown(0) && strip.closeShown(1), "");
			strip.setHovered(TabHit());
			report.check("a tab neither shown nor hovered hides its close mark",
				!strip.closeShown(1), "");
		}

		// Narrow tabs keep a close mark only on the shown one, where a
		// press on it is meant; on the rest that spot just picks the tab.
		static void checkNarrowTabsCloseOnlyWhenShown(Report& report) {
			const TabStrip strip = stripOf(10, 3);
			const D2D1_POINT_2F shown = middleOf(strip.closeRect(3));
			const D2D1_POINT_2F other = middleOf(strip.closeRect(5));

			report.check("a narrow shown tab still has its close mark",
				strip.hitAt(shown.x, shown.y) == TabHit{ TabPart::Close, 3 }, "");
			report.check("a narrow tab in the background has none",
				strip.hitAt(other.x, other.y) == TabHit{ TabPart::Tab, 5 }
					&& !strip.closeFits(5), "");
		}

		static void checkTabDragSlots(Report& report) {
			const TabStrip strip = stripOf(4, 0);
			report.check("a drag left of the strip is the first place",
				strip.slotAt(-100.0f) == 0, "");
			report.check("a drag past the last tab is the last place",
				strip.slotAt(5000.0f) == 3, "");
			report.check("a drag over a tab is that tab's place",
				strip.slotAt(8.0f + 2.0f * strip.tabWidth() + 1.0f) == 2, "");
		}

		static void checkEmptyStripHoldsNothing(Report& report) {
			TabStrip strip;
			strip.setTabs(3, 1);
			report.check("a strip with no room holds nothing",
				!strip.holdsPoint(0.0f, 0.0f) && strip.hitAt(10.0f, 10.0f).part == TabPart::None,
				"");
		}

		static void checkSessionWithTabsReadsBack(Report& report) {
			PaneTree split;
			split.adopt(std::make_unique<Pane>());
			split.splitFocused(SplitAxis::Rows, std::make_unique<Pane>());
			PaneTree lone;
			lone.adopt(std::make_unique<Pane>());

			const std::vector<const PaneNode*> leaves = { split.focused(), lone.focused() };
			const std::string text = sessionText(leaves, 1);

			SavedSession session;
			const bool parsed = parseSession(text, session);
			report.check("a session of tabs reads back as its tabs",
				parsed && session.tabs.size() == 2, text);
			report.check("each tab keeps its own tree",
				parsed && session.tabs.size() == 2 && !session.tabs[0].leaf
					&& session.tabs[0].axis == SplitAxis::Rows && session.tabs[1].leaf, text);
			report.check("the tab that was shown is remembered", session.active == 1, text);
		}

		static void checkOlderLayoutsStillRead(Report& report) {
			SavedSession session;
			const bool parsed = parseSession(
				"wbshterm-layout 1\nsplit columns 0.500\npane C:/a\npane C:/b\n", session);
			report.check("a layout from before tabs is one tab",
				parsed && session.tabs.size() == 1 && !session.tabs[0].leaf
					&& session.active == 0, "");

			SavedSession refused;
			report.check("a tab with no panes under it is refused",
				!parseSession("wbshterm-layout 2\ntab\n", refused), "");
			report.check("a session with no tabs is refused",
				!parseSession("wbshterm-layout 2\n", refused), "");
			report.check("a file this version did not write is refused",
				!parseSession("wbshterm-layout 9\ntab\npane\n", refused), "");
		}

		static void writeSettings(const std::wstring& path, const std::string& text) {
			std::ofstream file(path, std::ios::binary | std::ios::trunc);
			file << text;
		}

		static void checkTabSettings(Report& report, const std::wstring& directory) {
			const std::wstring path = directory + L"wbshterm-tabs.conf";
			writeSettings(path, "[tabs]\nenabled = false\nclassic_keys = false\n");

			Config edited;
			std::string error;
			loadConfig(path, edited, error);

			const Config defaults;
			report.check("tabs and their classic keys are on by default",
				defaults.tabs.enabled && defaults.tabs.classic_keys, "");
			report.check("[tabs] enabled is read", !edited.tabs.enabled, error);
			report.check("[tabs] classic_keys is read", !edited.tabs.classic_keys, error);
			_wremove(path.c_str());
		}

	} /* namespace test */

	void runTabChecks(test::Report& report, const std::wstring& directory) {
		test::checkTabsOpenAndStep(report);
		test::checkTabsClose(report);
		test::checkTabsMove(report);
		test::checkTabStripGeometry(report);
		test::checkTabStripHitTesting(report);
		test::checkNarrowTabsCloseOnlyWhenShown(report);
		test::checkTabDragSlots(report);
		test::checkEmptyStripHoldsNothing(report);
		test::checkSessionWithTabsReadsBack(report);
		test::checkOlderLayoutsStillRead(report);
		test::checkTabSettings(report, directory);
	}

} /* namespace wbshterm */

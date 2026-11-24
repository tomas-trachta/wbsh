/**
 * @file layout_store.cpp
 * @brief Writing a split tree out as lines and reading it back.
 */

#include "layout_store.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace wbshterm {

	namespace layout_detail {

		static const char* const kHeader = "wbshterm-layout 1";
		static const char* const kSessionHeader = "wbshterm-layout 2";
		static const char* const kTabWord = "tab";
		static const char* const kActiveTabWord = "tab active";
		static const char* const kSplitWord = "split ";
		static const char* const kPaneWord = "pane";
		static const std::size_t kMaxDepth = 64;

		static const PaneNode& rootOf(const PaneNode& leaf) {
			const PaneNode* node = &leaf;
			while (node->parent() != nullptr) node = node->parent();
			return *node;
		}

		static std::string ratioText(float ratio) {
			char text[16] = {};
			std::snprintf(text, sizeof(text), "%.3f", static_cast<double>(ratio));
			return text;
		}

		static void writeNode(const PaneNode& node, std::string& out) {
			if (node.isLeaf()) {
				const std::string& directory = node.pane()->screen().workingDirectory();
				out += std::string(kPaneWord) + (directory.empty() ? "" : " " + directory) + "\n";
				return;
			}

			const char* axis = node.axis() == SplitAxis::Columns ? "columns" : "rows";
			out += std::string(kSplitWord) + axis + " " + ratioText(node.ratio()) + "\n";
			writeNode(*node.first(), out);
			writeNode(*node.second(), out);
		}

		static std::vector<std::string> linesOf(const std::string& text) {
			std::vector<std::string> lines;
			std::istringstream in(text);
			std::string line;
			while (std::getline(in, line)) {
				if (!line.empty() && line.back() == '\r') line.pop_back();
				lines.push_back(line);
			}

			return lines;
		}

		struct Reader {
			const std::vector<std::string>& lines;
			std::size_t next = 1;
		};

		static bool readNode(Reader& reader, SavedPane& out, std::size_t depth);

		static bool readPane(const std::string& line, SavedPane& out) {
			out.leaf = true;
			out.directory = line.size() > 5 ? line.substr(5) : std::string();
			return true;
		}

		static bool readSplit(Reader& reader, const std::string& line, SavedPane& out,
				std::size_t depth) {
			std::istringstream in(line.substr(6));
			std::string axis;
			float ratio = 0.5f;
			if (!(in >> axis >> ratio) || (axis != "columns" && axis != "rows")) return false;

			out.leaf  = false;
			out.axis  = axis == "columns" ? SplitAxis::Columns : SplitAxis::Rows;
			out.ratio = ratio;
			out.first = std::make_unique<SavedPane>();
			out.second = std::make_unique<SavedPane>();
			return readNode(reader, *out.first, depth + 1)
				&& readNode(reader, *out.second, depth + 1);
		}

		static bool readNode(Reader& reader, SavedPane& out, std::size_t depth) {
			if (depth > kMaxDepth || reader.next >= reader.lines.size()) return false;

			const std::string& line = reader.lines[reader.next++];
			if (line == kPaneWord || line.compare(0, 5, "pane ") == 0) return readPane(line, out);
			if (line.compare(0, 6, kSplitWord) == 0) return readSplit(reader, line, out, depth);

			return false;
		}

		static bool readTabs(Reader& reader, SavedSession& out_session) {
			while (reader.next < reader.lines.size()) {
				const std::string& line = reader.lines[reader.next++];
				if (line.empty()) continue;
				if (line != kTabWord && line != kActiveTabWord) return false;

				if (line == kActiveTabWord) out_session.active = out_session.tabs.size();
				out_session.tabs.emplace_back();
				if (!readNode(reader, out_session.tabs.back(), 0)) return false;
			}

			return !out_session.tabs.empty();
		}

	}  // namespace layout_detail

	bool parseLayout(const std::string& text, SavedPane& out_root) {
		const std::vector<std::string> lines = layout_detail::linesOf(text);
		if (lines.empty() || lines.front() != layout_detail::kHeader) return false;

		layout_detail::Reader reader{ lines };
		return layout_detail::readNode(reader, out_root, 0);
	}

	std::string sessionText(const std::vector<const PaneNode*>& tab_leaves, std::size_t active) {
		std::string text = std::string(layout_detail::kSessionHeader) + "\n";

		for (std::size_t index = 0; index < tab_leaves.size(); ++index) {
			text += index == active ? layout_detail::kActiveTabWord : layout_detail::kTabWord;
			text += "\n";
			layout_detail::writeNode(layout_detail::rootOf(*tab_leaves[index]), text);
		}

		return text;
	}

	bool parseSession(const std::string& text, SavedSession& out_session) {
		out_session = SavedSession();

		const std::vector<std::string> lines = layout_detail::linesOf(text);
		if (lines.empty()) return false;

		if (lines.front() == layout_detail::kHeader) {
			out_session.tabs.emplace_back();
			return parseLayout(text, out_session.tabs.back());
		}

		if (lines.front() != layout_detail::kSessionHeader) return false;

		layout_detail::Reader reader{ lines };
		return layout_detail::readTabs(reader, out_session);
	}

	const SavedPane& firstSavedPane(const SavedPane& node) {
		const SavedPane* at = &node;
		while (!at->leaf && at->first) at = at->first.get();
		return *at;
	}

	std::wstring layoutFilePath(const std::wstring& config_path) {
		const std::size_t slash = config_path.find_last_of(L"\\/");
		if (slash == std::wstring::npos) return std::wstring();

		return config_path.substr(0, slash + 1) + L"session.txt";
	}

} /* namespace wbshterm */

/**
 * @file vi_options.cpp
 * @brief vi's :set: tabstop, shiftwidth, autoindent, ignorecase, number,
 *        list, expandtab, wrapscan, readonly and fileformat, with the
 *        no / inv / ! / ? forms.
 */

#include "vi_internal.h"

#include <string>
#include <vector>

#include "numparse.h"

namespace wbsh {

	namespace vi_detail {

		enum class OptionId {
			TabStop,
			ShiftWidth,
			AutoIndent,
			IgnoreCase,
			Number,
			List,
			ExpandTab,
			WrapScan,
			ReadOnly,
			FileFormat,
		};

		enum class OptionKind { Flag, Count, Text };

		static const unsigned long long kMaxWidth = 64;

		struct OptionEntry {
			const char* name;
			const char* abbreviation;
			OptionId id;
			OptionKind kind;
		};

		static const OptionEntry kOptions[] = {
			{ "autoindent", "ai", OptionId::AutoIndent, OptionKind::Flag },
			{ "expandtab", "et", OptionId::ExpandTab, OptionKind::Flag },
			{ "fileformat", "ff", OptionId::FileFormat, OptionKind::Text },
			{ "ignorecase", "ic", OptionId::IgnoreCase, OptionKind::Flag },
			{ "list", "list", OptionId::List, OptionKind::Flag },
			{ "number", "nu", OptionId::Number, OptionKind::Flag },
			{ "readonly", "ro", OptionId::ReadOnly, OptionKind::Flag },
			{ "shiftwidth", "sw", OptionId::ShiftWidth, OptionKind::Count },
			{ "tabstop", "ts", OptionId::TabStop, OptionKind::Count },
			{ "wrapscan", "ws", OptionId::WrapScan, OptionKind::Flag },
		};

		static bool* flagOf(Editor& ed, OptionId id) {
			switch (id) {
				case OptionId::AutoIndent:
					return &ed.options.autoindent;
				case OptionId::IgnoreCase:
					return &ed.options.ignorecase;
				case OptionId::Number:
					return &ed.options.number;
				case OptionId::List:
					return &ed.options.list;
				case OptionId::ExpandTab:
					return &ed.options.expandtab;
				case OptionId::WrapScan:
					return &ed.options.wrapscan;
				case OptionId::ReadOnly:
					return &ed.options.readonly;
				default:
					return nullptr;
			}
		}

		static std::size_t* countOf(Editor& ed, OptionId id) {
			if (id == OptionId::TabStop) return &ed.options.tabstop;
			if (id == OptionId::ShiftWidth) return &ed.options.shiftwidth;
			return nullptr;
		}

		static std::string valueText(Editor& ed, const OptionEntry& entry) {
			const std::string name = entry.name;
			if (entry.kind == OptionKind::Count) {
				return name + "=" + std::to_string(*countOf(ed, entry.id));
			}

			if (entry.kind == OptionKind::Text) return name + "=" + (ed.crlf ? "dos" : "unix");
			return (*flagOf(ed, entry.id) ? "" : "no") + name;
		}

		static bool isDefault(Editor& ed, const OptionEntry& entry) {
			const Options defaults;
			switch (entry.id) {
				case OptionId::TabStop:
					return ed.options.tabstop == defaults.tabstop;
				case OptionId::ShiftWidth:
					return ed.options.shiftwidth == defaults.shiftwidth;
				case OptionId::WrapScan:
					return ed.options.wrapscan;
				case OptionId::FileFormat:
					return !ed.crlf;
				default:
					return !*flagOf(ed, entry.id);
			}
		}

		static void listOptions(Editor& ed, bool all) {
			std::string text;
			for (const OptionEntry& entry : kOptions) {
				if (!all && isDefault(ed, entry)) continue;
				if (!text.empty()) text += "  ";
				text += valueText(ed, entry);
			}

			setMessage(ed, text.empty() ? "--- Options ---" : text);
		}

		static const OptionEntry* findOption(const std::string& name) {
			for (const OptionEntry& entry : kOptions) {
				if (name == entry.name || name == entry.abbreviation) return &entry;
			}

			return nullptr;
		}

		static bool setValue(Editor& ed, const OptionEntry& entry, const std::string& value) {
			if (entry.kind == OptionKind::Text) {
				if (value != "dos" && value != "unix") return false;
				ed.crlf = value == "dos";
				return true;
			}

			unsigned long long number = 0;
			if (entry.kind != OptionKind::Count || !parseULL(value, number)) return false;
			if (number == 0 || number > kMaxWidth) return false;

			*countOf(ed, entry.id) = static_cast<std::size_t>(number);
			return true;
		}

		struct OptionWord {
			std::string name;
			std::string value;
			bool has_value = false;
			bool negate = false;
			bool invert = false;
			bool query = false;
		};

		static OptionWord parseOptionWord(std::string word) {
			OptionWord parsed;
			const std::size_t equals = word.find_first_of("=:");
			if (equals != std::string::npos) {
				parsed.value = word.substr(equals + 1);
				parsed.has_value = true;
				word.resize(equals);
			}

			if (!word.empty() && word.back() == '?') parsed.query = true;
			if (!word.empty() && word.back() == '!') parsed.invert = true;
			if (parsed.query || parsed.invert) word.pop_back();

			const bool known = findOption(word) != nullptr;
			if (!known && word.compare(0, 2, "no") == 0) parsed.negate = true;
			if (!known && word.compare(0, 3, "inv") == 0) parsed.invert = true;
			parsed.name = parsed.negate
				? word.substr(2)
				: (!known && parsed.invert ? word.substr(3) : word);
			return parsed;
		}

		static bool applyFlag(Editor& ed, const OptionEntry& entry, const OptionWord& word) {
			bool* flag = flagOf(ed, entry.id);
			if (word.has_value) return false;

			if (word.invert) *flag = !*flag;
			else *flag = !word.negate;
			return true;
		}

		static bool applyOptionWord(Editor& ed, const std::string& text) {
			const OptionWord word = parseOptionWord(text);
			const OptionEntry* entry = findOption(word.name);
			if (entry == nullptr) {
				setError(ed, "Unknown option: " + text);
				return false;
			}

			const bool shows = word.query || (entry->kind != OptionKind::Flag && !word.has_value);
			if (shows) {
				setMessage(ed, valueText(ed, *entry));
				return true;
			}

			const bool applied = entry->kind == OptionKind::Flag ? applyFlag(ed, *entry, word)
				: !word.negate && !word.invert && setValue(ed, *entry, word.value);
			if (!applied) setError(ed, "Invalid argument: " + text);
			return applied;
		}

		static std::vector<std::string> optionWords(const std::string& text) {
			std::vector<std::string> words;
			std::size_t at = 0;
			while (at < text.size()) {
				while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) ++at;
				const std::size_t start = at;
				while (at < text.size() && text[at] != ' ' && text[at] != '\t') ++at;
				if (at > start) words.push_back(text.substr(start, at - start));
			}

			return words;
		}

		bool exSet(Editor& ed, const ExCommand& cmd) {
			const std::vector<std::string> words = optionWords(cmd.args);
			if (words.empty() || words[0] == "all") {
				listOptions(ed, !words.empty());
				return true;
			}

			for (const std::string& word : words) {
				if (!applyOptionWord(ed, word)) return false;
			}

			return true;
		}

	}  // namespace vi_detail

}  // namespace wbsh

/**
 * @file screen_vt.cpp
 * @brief Tab stops, REP, cursor shape, hyperlinks, the clipboard and colour queries.
 */

#include "screen.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace wbshterm {

	static const int kTabWidth = 8;
	static const std::size_t kMaxLinks = 0xFFFF;

	void Screen::resetTabStops() {
		tab_stops_.assign(static_cast<std::size_t>(columns_), false);
		for (int column = kTabWidth; column < columns_; column += kTabWidth) {
			tab_stops_[static_cast<std::size_t>(column)] = true;
		}
	}

	// Stops a program set survive a resize; columns that are new get the
	// default every eight.
	void Screen::resizeTabStops(int new_columns) {
		const std::size_t kept = tab_stops_.size();
		tab_stops_.resize(static_cast<std::size_t>(new_columns), false);
		for (std::size_t column = kept; column < tab_stops_.size(); ++column) {
			tab_stops_[column] = column > 0 && column % kTabWidth == 0;
		}
	}

	void Screen::tab() {
		forwardTab(1);
	}

	void Screen::forwardTab(int count) {
		wrap_pending_ = false;
		for (int step = 0; step < std::max(1, count) && cursor_.column < columns_ - 1; ++step) {
			++cursor_.column;
			while (cursor_.column < columns_ - 1
				&& !tab_stops_[static_cast<std::size_t>(cursor_.column)]) {
				++cursor_.column;
			}
		}
	}

	void Screen::backwardTab(int count) {
		wrap_pending_ = false;
		for (int step = 0; step < std::max(1, count) && cursor_.column > 0; ++step) {
			--cursor_.column;
			while (cursor_.column > 0 && !tab_stops_[static_cast<std::size_t>(cursor_.column)]) {
				--cursor_.column;
			}
		}
	}

	void Screen::clearTabStops(int mode) {
		if (mode == 0) tab_stops_[static_cast<std::size_t>(cursor_.column)] = false;
		if (mode == 3) tab_stops_.assign(tab_stops_.size(), false);
	}

	void Screen::repeatLastCharacter(int count) {
		if (last_printed_ == 0) return;

		const int times = std::min(std::max(1, count), columns_ * rows_);
		for (int step = 0; step < times; ++step) writeChar(last_printed_);
	}

	void Screen::setCursorShape(int shape) {
		switch (shape) {
		case 0:  cursor_shape_ = CursorShape::kDefault; break;
		case 1:  cursor_shape_ = CursorShape::kBlinkingBlock; break;
		case 2:  cursor_shape_ = CursorShape::kSteadyBlock; break;
		case 3:  cursor_shape_ = CursorShape::kBlinkingUnderline; break;
		case 4:  cursor_shape_ = CursorShape::kSteadyUnderline; break;
		case 5:  cursor_shape_ = CursorShape::kBlinkingBar; break;
		case 6:  cursor_shape_ = CursorShape::kSteadyBar; break;
		default: break;
		}
	}

	void Screen::resetVtExtras() {
		resetTabStops();
		autowrap_     = true;
		last_printed_ = 0;
		cursor_shape_ = CursorShape::kDefault;
	}

	// A link's number stays valid for as long as any history can hold it,
	// so the table only grows; once it is full, new links show as text.
	std::uint16_t Screen::internLink(const std::string& target) {
		const auto found = link_ids_.find(target);
		if (found != link_ids_.end()) return found->second;
		if (links_.size() >= kMaxLinks) return 0;

		links_.push_back(target);
		const auto id = static_cast<std::uint16_t>(links_.size());
		link_ids_.emplace(target, id);
		return id;
	}

	// OSC 8 ; params ; URI -- an empty URI ends the link.
	void Screen::noteHyperlink(const std::string& body) {
		const std::size_t separator = body.find(';');
		if (separator == std::string::npos) return;

		const std::string target = body.substr(separator + 1);
		pen_.link = target.empty() ? 0 : internLink(target);
	}

	const std::string* Screen::linkAt(int absolute_row, int column) const {
		if (absolute_row < 0 || absolute_row >= totalRows()) return nullptr;
		if (column < 0 || column >= columns_) return nullptr;

		const std::uint16_t id = cellAt(absolute_row, column).link;
		if (id == 0 || id > links_.size()) return nullptr;
		return &links_[id - 1u];
	}

	static bool startsWithIgnoringCase(const std::string& text, const char* prefix) {
		for (std::size_t index = 0; prefix[index] != '\0'; ++index) {
			if (index >= text.size()) return false;

			const char lowered = text[index] >= 'A' && text[index] <= 'Z'
				? static_cast<char>(text[index] - 'A' + 'a')
				: text[index];
			if (lowered != prefix[index]) return false;
		}

		return true;
	}

	bool linkIsSafeToOpen(const std::string& target) {
		static const char* const kSchemes[] = {
			"http://", "https://", "ftp://", "mailto:",
		};

		for (const char* scheme : kSchemes) {
			if (startsWithIgnoringCase(target, scheme)) return true;
		}

		return false;
	}

	static int base64Value(char symbol) {
		if (symbol >= 'A' && symbol <= 'Z') return symbol - 'A';
		if (symbol >= 'a' && symbol <= 'z') return symbol - 'a' + 26;
		if (symbol >= '0' && symbol <= '9') return symbol - '0' + 52;
		if (symbol == '+') return 62;
		if (symbol == '/') return 63;
		return -1;
	}

	static std::string decodeBase64(const std::string& text) {
		std::string out;
		unsigned int bits = 0;
		int pending = 0;

		for (const char symbol : text) {
			const int value = base64Value(symbol);
			if (value < 0) continue;

			bits = (bits << 6) | static_cast<unsigned int>(value);
			pending += 6;
			if (pending < 8) continue;

			pending -= 8;
			out.push_back(static_cast<char>((bits >> pending) & 0xFF));
		}

		return out;
	}

	// OSC 52 ; selection ; base64 -- a "?" asks for the clipboard back,
	// which is not answered: any program could read what was copied.
	void Screen::noteClipboardWrite(const std::string& body) {
		const std::size_t separator = body.find(';');
		if (separator == std::string::npos || clipboard_handler_ == nullptr) return;

		const std::string data = body.substr(separator + 1);
		if (data == "?") return;

		clipboard_handler_->clipboardWrite(decodeBase64(data));
	}

	const Palette& Screen::palette() const {
		static const Palette kBuiltIn;
		return palette_ != nullptr ? *palette_ : kBuiltIn;
	}

	static std::string xParseColor(std::uint32_t rgb) {
		char text[32];
		const unsigned red   = (rgb >> 16) & 0xFF;
		const unsigned green = (rgb >> 8) & 0xFF;
		const unsigned blue  = rgb & 0xFF;
		std::snprintf(text, sizeof(text), "rgb:%02x%02x/%02x%02x/%02x%02x",
			red, red, green, green, blue, blue);
		return text;
	}

	void Screen::replyColor(const std::string& prefix, std::uint32_t rgb) {
		if (responder_ == nullptr) return;
		responder_->vtRespond("\x1b]" + prefix + xParseColor(rgb) + "\x1b\\");
	}

	static std::uint32_t dynamicColor(const Palette& palette, int code) {
		switch (code) {
		case 10: return palette.foreground;
		case 11: return palette.background;
		default: return palette.cursor;
		}
	}

	// OSC 10 ; ? asks for the text colour; each further "?" moves on to
	// the next dynamic colour, as in "OSC 10 ; ? ; ?" for text and back.
	void Screen::answerColorQuery(int code, const std::string& body) {
		std::size_t start = 0;
		for (int current = code; current <= 12; ++current) {
			const std::size_t end = body.find(';', start);
			if (body.substr(start, end - start) == "?") {
				replyColor(std::to_string(current) + ";", dynamicColor(palette(), current));
			}

			if (end == std::string::npos) return;
			start = end + 1;
		}
	}

	static std::uint32_t paletteEntry(const Palette& palette, int index) {
		const std::uint32_t color = indexedColor(index);
		if ((color & 0xFF000000u) != kPaletteColor) return color;
		return palette.ansi[color & 0x0Fu];
	}

	// OSC 4 ; index ; spec pairs; only the "?" ones are answered.
	void Screen::answerPaletteQuery(const std::string& body) {
		std::vector<std::string> fields;
		std::size_t start = 0;
		for (;;) {
			const std::size_t end = body.find(';', start);
			fields.push_back(body.substr(start, end - start));
			if (end == std::string::npos) break;
			start = end + 1;
		}

		for (std::size_t pair = 0; pair + 1 < fields.size(); pair += 2) {
			if (fields[pair + 1] != "?") continue;

			const int index = std::atoi(fields[pair].c_str());
			if (index < 0 || index > 255) continue;
			replyColor("4;" + fields[pair] + ";", paletteEntry(palette(), index));
		}
	}

} /* namespace wbshterm */

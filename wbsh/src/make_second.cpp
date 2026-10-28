/**
 * @file make_second.cpp
 * @brief .SECONDEXPANSION: prerequisite lists kept unexpanded at reading
 *        time and expanded once more, with the automatic variables of the
 *        rule, when the target is about to be considered.
 */

#include "make_internal.h"

#include <string>
#include <vector>

namespace wbsh::make_detail {

	// A deferred list rides in the prerequisite vector behind a marker byte
	// no file name contains; the static form gets the stem substituted for
	// `%` after the expansion.
	static const char kDeferred = '\x01';
	static const char kDeferredStatic = '\x02';

	bool isDeferredPrereq(const std::string& entry) {
		return !entry.empty() && (entry[0] == kDeferred || entry[0] == kDeferredStatic);
	}

	bool hasDeferredPrereqs(const std::vector<std::string>& entries) {
		for (const std::string& entry : entries) {
			if (isDeferredPrereq(entry)) return true;
		}

		return false;
	}

	std::string deferPrereqs(const std::string& text) {
		return std::string(1, kDeferred) + text;
	}

	std::string deferStaticPrereqs(const std::string& entry) {
		return std::string(1, kDeferredStatic) + entry.substr(1);
	}

	std::string deferredText(const std::string& entry) {
		return entry.substr(1);
	}

	static void appendNames(Make& make, const std::string& text, bool substitute,
	                        const std::string& stem, std::vector<std::string>& names) {
		for (std::string word : splitWords(text)) {
			if (word == ".WAIT") continue;
			if (substitute) word = substituteStem(word, stem);

			const std::vector<std::string> matches = hasGlobMeta(word)
				? globWord(make, word)
				: std::vector<std::string>();
			if (matches.empty()) names.push_back(word);
			else names.insert(names.end(), matches.begin(), matches.end());
		}
	}

	static void expandOne(Make& make, const SecondContext& context, const std::string& entry,
	                      std::vector<std::string>& resolved, std::vector<std::string>& order_only) {
		AutoVars autos;
		autos.target = context.target;
		autos.stem = context.stem;
		autos.all_prereqs = resolved;
		autos.prereqs = uniqueWords(resolved);
		const ExpandContext ctx{ context.scope, context.file, &autos };

		const std::string text = expand(make, deferredText(entry), ctx);
		const bool substitute = entry[0] == kDeferredStatic;
		const std::size_t bar = text.find('|');
		appendNames(make, text.substr(0, bar), substitute, context.stem, resolved);
		if (bar != std::string::npos) {
			appendNames(make, text.substr(bar + 1), substitute, context.stem, order_only);
		}
	}

	void expandDeferredPrereqs(Make& make, const SecondContext& context,
	                           std::vector<std::string>& prereqs,
	                           std::vector<std::string>& order_only) {
		std::vector<std::string> resolved;
		std::vector<std::string> extra_order;
		for (const std::string& entry : prereqs) {
			if (isDeferredPrereq(entry)) expandOne(make, context, entry, resolved, extra_order);
			else resolved.push_back(entry);
		}

		std::vector<std::string> order;
		for (const std::string& entry : order_only) {
			if (isDeferredPrereq(entry)) expandOne(make, context, entry, order, order);
			else order.push_back(entry);
		}

		order.insert(order.end(), extra_order.begin(), extra_order.end());
		prereqs = resolved;
		order_only = order;
	}

}  // namespace wbsh::make_detail

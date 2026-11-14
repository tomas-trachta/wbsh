#include "globsort.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "filetimes.h"

namespace wbsh {

	namespace globsort_detail {
		enum class SortKey {
			Name,
			Size,
			Mtime,
			Atime,
			Ctime,
			Blocks,
			Numeric,
			NoSort,
		};

		struct SortSpec {
			SortKey key = SortKey::Name;
			bool reverse = false;
		};

		struct KeyName {
			const char* name;
			SortKey key;
		};

		static const KeyName kKeyNames[] = {
			{ "name", SortKey::Name },     { "size", SortKey::Size },
			{ "mtime", SortKey::Mtime },   { "atime", SortKey::Atime },
			{ "ctime", SortKey::Ctime },   { "blocks", SortKey::Blocks },
			{ "numeric", SortKey::Numeric }, { "nosort", SortKey::NoSort },
		};

		struct Entry {
			std::string name;
			long long size = -1;
			long long blocks = -1;
			DateValue mtime{ -1, -1 };
			DateValue atime{ -1, -1 };
			DateValue ctime{ -1, -1 };
		};

		static std::size_t skipBlanks(const std::string& text) {
			std::size_t i = 0;
			while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n')) ++i;
			return i;
		}

		// An unknown specifier means the historical ascending sort by name,
		// whatever sign came before it.
		static SortSpec parseSpec(const std::string& value) {
			SortSpec spec;
			std::size_t i = skipBlanks(value);
			if (i < value.size() && value[i] == '+') ++i;
			else if (i < value.size() && value[i] == '-') {
				spec.reverse = true;
				++i;
			}

			const std::string name = value.substr(i);
			if (name.empty()) return spec;

			for (const KeyName& known : kKeyNames) {
				if (name == known.name) {
					spec.key = known.key;
					return spec;
				}
			}

			return SortSpec();
		}

		static const long long kBlockBytes = 512;
		static const long long kClusterBytes = 4096;

		static long long allocatedBlocks(long long size) {
			const long long clusters = (size + kClusterBytes - 1) / kClusterBytes;
			return clusters * (kClusterBytes / kBlockBytes);
		}

		static void statEntry(Entry& entry, const PathConv& pc) {
			const std::filesystem::path native = utf8ToPath(pc.toWin32(entry.name));
			std::error_code ec;
			if (!std::filesystem::exists(native, ec)) return;

			const bool directory = std::filesystem::is_directory(native, ec);
			const auto size = directory ? 0 : std::filesystem::file_size(native, ec);
			entry.size = ec ? 0 : static_cast<long long>(size);
			entry.blocks = allocatedBlocks(entry.size);

			FileTimes times;
			if (!readFileTimes(native, true, times)) return;

			entry.mtime = times.modify;
			entry.atime = times.access;
			entry.ctime = times.change;
		}

		static std::vector<Entry> buildEntries(const std::vector<std::string>& matches,
				const PathConv& pc) {
			std::vector<Entry> entries;
			entries.reserve(matches.size());
			for (const std::string& match : matches) {
				Entry entry;
				entry.name = match;
				statEntry(entry, pc);
				entries.push_back(std::move(entry));
			}

			return entries;
		}

		template <typename T>
		static int compareValues(const T& left, const T& right) {
			return (left > right) - (left < right);
		}

		static int compareTimes(const DateValue& left, const DateValue& right) {
			const int seconds = compareValues(left.seconds, right.seconds);
			return seconds != 0 ? seconds : compareValues(left.nanos, right.nanos);
		}

		static bool allDigits(const std::string& text, long long& value) {
			if (text.empty()) return false;

			for (const char c : text) {
				if (!std::isdigit(static_cast<unsigned char>(c))) return false;
			}

			value = std::stoll(text.size() > 18 ? text.substr(0, 18) : text);
			return true;
		}

		// Numbers come before other names; names that are both numbers need no
		// tie-break by name.
		static int compareNumeric(const Entry& left, const Entry& right, bool& decided) {
			long long left_value = 0;
			long long right_value = 0;
			const bool left_number = allDigits(left.name, left_value);
			const bool right_number = allDigits(right.name, right_value);
			decided = left_number || right_number;
			if (left_number && right_number) return compareValues(left_value, right_value);
			if (left_number) return -1;
			if (right_number) return 1;
			return 0;
		}

		static int compareKey(SortKey key, const Entry& left, const Entry& right) {
			switch (key) {
			case SortKey::Size:
				return compareValues(left.size, right.size);
			case SortKey::Blocks:
				return compareValues(left.blocks, right.blocks);
			case SortKey::Mtime:
				return compareTimes(left.mtime, right.mtime);
			case SortKey::Atime:
				return compareTimes(left.atime, right.atime);
			case SortKey::Ctime:
				return compareTimes(left.ctime, right.ctime);
			default:
				return 0;
			}
		}

		static int compareEntries(const SortSpec& spec, const Entry& left, const Entry& right) {
			if (spec.key == SortKey::Numeric) {
				bool decided = false;
				const int numeric = compareNumeric(left, right, decided);
				if (decided) return numeric;
			}

			const int by_key = compareKey(spec.key, left, right);
			return by_key != 0 ? by_key : left.name.compare(right.name);
		}

		static void sortByName(std::vector<std::string>& matches, bool reverse) {
			std::sort(matches.begin(), matches.end());
			if (reverse) std::reverse(matches.begin(), matches.end());
		}

		static void sortByAttributes(std::vector<std::string>& matches, const SortSpec& spec,
				const PathConv& pc) {
			std::vector<Entry> entries = buildEntries(matches, pc);
			std::stable_sort(entries.begin(), entries.end(), [&](const Entry& a, const Entry& b) {
				const int order = compareEntries(spec, a, b);
				return spec.reverse ? order > 0 : order < 0;
			});

			for (std::size_t i = 0; i < entries.size(); ++i) {
				matches[i] = std::move(entries[i].name);
			}
		}
	}

	void sortGlobMatches(std::vector<std::string>& matches, const std::string& globsort,
	                     const PathConv& pc) {
		const globsort_detail::SortSpec spec = globsort_detail::parseSpec(globsort);
		switch (spec.key) {
		case globsort_detail::SortKey::NoSort:
			return;
		case globsort_detail::SortKey::Name:
			globsort_detail::sortByName(matches, spec.reverse);
			return;
		default:
			globsort_detail::sortByAttributes(matches, spec, pc);
			return;
		}
	}

}  // namespace wbsh

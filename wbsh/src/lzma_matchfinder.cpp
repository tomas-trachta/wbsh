/**
 * @file lzma_matchfinder.cpp
 * @brief Match finder for the LZMA encoder, shaped like xz's bt4: an exact
 *        two-byte head and a hashed three-byte head catch the short close
 *        matches, a four-byte hash roots a binary tree of earlier positions
 *        sorted by what follows them. Each search also re-roots the tree at
 *        the position searched, so finding and inserting are one walk.
 */

#include "lzma_encoder_internal.h"

#include <algorithm>

namespace wbsh::lzma_encoder_detail {

	static const int kHash3Bits = 16;
	static const std::uint32_t kHashMinLength = 4;
	static const std::uint32_t kTreeMinLength = 3;

	struct Candidates {
		std::uint32_t two;
		std::uint32_t three;
		std::uint32_t four;
	};

	/// found is null when the walk only inserts.
	struct Search {
		const std::uint8_t* here;
		std::uint32_t position;
		std::uint32_t limit;
		std::uint32_t best;
		Matches* found;
	};

	static std::uint32_t readLe32(const std::uint8_t* bytes) {
		return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8)
			| (static_cast<std::uint32_t>(bytes[2]) << 16)
			| (static_cast<std::uint32_t>(bytes[3]) << 24);
	}

	static std::uint32_t hash3(const std::uint8_t* bytes) {
		return ((readLe32(bytes) & 0x00FFFFFFu) * 2654435761u) >> (32 - kHash3Bits);
	}

	static std::uint32_t hash4(const MatchFinder& finder, const std::uint8_t* bytes) {
		return (readLe32(bytes) * 2654435761u) >> finder.hash4_shift;
	}

	static Search startSearch(const MatchFinder& finder, Matches* found) {
		const std::size_t available = availableFrom(finder, finder.inserted);
		Search search;
		search.here = finder.window.data() + finder.inserted;
		search.position = static_cast<std::uint32_t>(finder.inserted) + finder.cyclic_size;
		const std::size_t nice = finder.nice_length;
		search.limit = static_cast<std::uint32_t>((std::min)(available, nice));
		search.best = 1;
		search.found = found;
		return search;
	}

	static Candidates updateHeads(MatchFinder& finder, const Search& search) {
		const std::uint8_t* here = search.here;
		std::uint32_t& head2 = finder.head2[here[0] | (static_cast<std::uint32_t>(here[1]) << 8)];
		std::uint32_t& head3 = finder.head3[hash3(here)];
		std::uint32_t& head4 = finder.head4[hash4(finder, here)];
		const Candidates candidates = { head2, head3, head4 };

		head2 = search.position;
		head3 = search.position;
		head4 = search.position;
		return candidates;
	}

	static bool withinDictionary(const MatchFinder& finder, std::uint32_t delta) {
		return delta != 0 && delta < finder.cyclic_size;
	}

	static void recordMatch(Search& search, std::uint32_t length, std::uint32_t delta) {
		if (search.found == nullptr || length <= search.best) return;

		Matches& found = *search.found;
		found.list[found.count].length = length;
		found.list[found.count].distance = delta - 1;
		++found.count;
		search.best = length;
	}

	static void tryCandidate(const MatchFinder& finder, Search& search, std::uint32_t candidate) {
		const std::uint32_t delta = search.position - candidate;
		if (!withinDictionary(finder, delta)) return;
		recordMatch(search, matchLength(search.here - delta, search.here, 0, search.limit), delta);
	}

	/// The two subtrees still being rebuilt under the searched position:
	/// earlier positions whose continuation sorts below it go left.
	struct TreeCursor {
		std::uint32_t* left;
		std::uint32_t* right;
		std::uint32_t left_length;
		std::uint32_t right_length;
	};

	static void closeTree(TreeCursor& cursor) {
		*cursor.left = 0;
		*cursor.right = 0;
	}

	static std::uint32_t* treeNode(MatchFinder& finder, std::uint32_t position) {
		return &finder.tree[2 * static_cast<std::size_t>(position % finder.cyclic_size)];
	}

	// Every position below a subtree's cursor shares at least that side's
	// length of leading bytes with the searched one, so comparisons start
	// past them.
	static void walkTree(MatchFinder& finder, Search& search, std::uint32_t candidate) {
		std::uint32_t* root = treeNode(finder, search.position);
		TreeCursor cursor = { root + 1, root, 0, 0 };
		for (std::uint32_t depth = finder.depth; depth > 0; --depth) {
			const std::uint32_t delta = search.position - candidate;
			if (!withinDictionary(finder, delta)) break;

			std::uint32_t* pair = treeNode(finder, candidate);
			const std::uint8_t* earlier = search.here - delta;
			std::uint32_t length = (std::min)(cursor.left_length, cursor.right_length);
			if (earlier[length] == search.here[length]) {
				length = matchLength(earlier, search.here, length + 1, search.limit);
				recordMatch(search, length, delta);
				if (length == search.limit) {
					*cursor.right = pair[0];
					*cursor.left = pair[1];
					return;
				}
			}

			if (earlier[length] < search.here[length]) {
				*cursor.right = candidate;
				cursor.right = pair + 1;
				cursor.right_length = length;
				candidate = pair[1];
			} else {
				*cursor.left = candidate;
				cursor.left = pair;
				cursor.left_length = length;
				candidate = pair[0];
			}
		}

		closeTree(cursor);
	}

	// A match cut short at nice_length is still worth its full length.
	static void extendLongest(const MatchFinder& finder, const Search& search,
	                          std::size_t available, Matches& found) {
		Match& longest = found.list[found.count - 1];
		if (longest.length != finder.nice_length) return;

		const std::size_t most = kMatchMaxLength;
		const auto limit = static_cast<std::uint32_t>((std::min)(available, most));
		const std::uint8_t* earlier = search.here - longest.distance - 1;
		longest.length = matchLength(earlier, search.here, longest.length, limit);
	}

	void findMatches(MatchFinder& finder, Matches& found) {
		found.count = 0;
		found.longest = 0;
		const std::size_t available = availableFrom(finder, finder.inserted);
		if (available < kHashMinLength) {
			++finder.inserted;
			return;
		}

		Search search = startSearch(finder, &found);
		const Candidates candidates = updateHeads(finder, search);
		tryCandidate(finder, search, candidates.two);
		tryCandidate(finder, search, candidates.three);
		if (search.best == search.limit) search.found = nullptr;

		search.best = (std::max)(search.best, kTreeMinLength);
		walkTree(finder, search, candidates.four);
		++finder.inserted;
		if (found.count == 0) return;

		extendLongest(finder, search, available, found);
		found.longest = found.list[found.count - 1].length;
	}

	void skipPositions(MatchFinder& finder, std::size_t target) {
		while (finder.inserted < target) {
			if (availableFrom(finder, finder.inserted) >= kHashMinLength) {
				Search search = startSearch(finder, nullptr);
				walkTree(finder, search, updateHeads(finder, search).four);
			}

			++finder.inserted;
		}
	}

	static int hash4Bits(std::uint32_t dictionary) {
		int bits = 0;
		while (bits < 31 && (1u << (bits + 1)) <= dictionary) ++bits;
		return (std::min)((std::max)(bits - 1, 16), 24);
	}

	void setUpFinder(MatchFinder& finder, const LzmaPreset& preset) {
		finder.cyclic_size = preset.dictionary;
		finder.depth = preset.depth;
		finder.nice_length = preset.nice_length;

		const int bits = hash4Bits(preset.dictionary);
		finder.hash4_shift = 32 - bits;
		finder.head2.assign(static_cast<std::size_t>(1) << 16, 0);
		finder.head3.assign(static_cast<std::size_t>(1) << kHash3Bits, 0);
		finder.head4.assign(static_cast<std::size_t>(1) << bits, 0);
		finder.tree.assign(2 * static_cast<std::size_t>(preset.dictionary), 0);
	}

	static void rebase(std::vector<std::uint32_t>& table, std::uint32_t shift) {
		for (std::uint32_t& entry : table) entry = entry > shift ? entry - shift : 0;
	}

	// Drops input more than a dictionary behind @p keep_from, in whole
	// dictionaries so the tree stays indexed by position modulo its size.
	std::size_t slideWindow(Core& core, std::size_t keep_from) {
		MatchFinder& finder = core.finder;
		const std::size_t cyclic = finder.cyclic_size;
		if (keep_from < 2 * cyclic) return 0;

		const std::size_t shift = (keep_from - cyclic) - (keep_from - cyclic) % cyclic;
		const auto dropped = finder.window.begin() + static_cast<std::ptrdiff_t>(shift);
		finder.window.erase(finder.window.begin(), dropped);
		finder.inserted -= shift;
		core.cursor -= shift;

		const auto amount = static_cast<std::uint32_t>(shift);
		rebase(finder.head2, amount);
		rebase(finder.head3, amount);
		rebase(finder.head4, amount);
		rebase(finder.tree, amount);
		return shift;
	}

}  // namespace wbsh::lzma_encoder_detail

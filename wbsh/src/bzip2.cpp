/**
 * @file bzip2.cpp
 * @brief bzip2 codec. Bits are most significant first throughout; the
 *        48-bit block and end-of-stream magics are the BCD digits of pi
 *        and sqrt(pi).
 */

#include "bzip2.h"

#include <algorithm>
#include <cstring>

#include "bytestream.h"
#include "huffman.h"

namespace wbsh {

	namespace bzip2_detail {

		struct BitReader {
			InputBuffer* in = nullptr;
			std::uint64_t buffer = 0;
			int count = 0;
			bool overrun = false;

			std::uint32_t bits(int width) {
				while (count < width) {
					int byte = in->get();
					if (byte < 0) {
						overrun = true;
						byte = 0;
					}

					buffer = (buffer << 8) | static_cast<std::uint8_t>(byte);
					count += 8;
				}

				count -= width;
				return static_cast<std::uint32_t>((buffer >> count) & ((1ULL << width) - 1));
			}

			void alignToByte() {
				count -= count % 8;
			}
		};

		struct BitWriter {
			std::vector<std::uint8_t>* out = nullptr;
			std::uint64_t buffer = 0;
			int count = 0;

			void put(std::uint32_t value, int width) {
				buffer = (buffer << width) | (value & ((1ULL << width) - 1));
				count += width;
				while (count >= 8) {
					count -= 8;
					out->push_back(static_cast<std::uint8_t>((buffer >> count) & 0xFF));
				}
			}

			void flush() {
				if (count > 0) put(0, 8 - count);
			}
		};

		struct DecodeTable {
			std::int32_t limit[22] = {};
			std::int32_t base[22] = {};
			std::vector<std::uint16_t> permute;
			int min_length = 0;
			int max_length = 0;
		};

		struct BlockState {
			std::vector<std::uint8_t> symbol_map;
			std::vector<std::uint8_t> selectors;
			std::vector<DecodeTable> tables;
			int alpha_size = 0;
		};

		struct EncodedTables {
			int group_count = 0;
			std::vector<std::vector<std::uint8_t>> lengths;
			std::vector<std::vector<std::uint32_t>> codes;
			std::vector<std::uint8_t> selectors;
		};

	}  // namespace bzip2_detail

	using bzip2_detail::BitReader;
	using bzip2_detail::BitWriter;
	using bzip2_detail::BlockState;
	using bzip2_detail::DecodeTable;
	using bzip2_detail::EncodedTables;

	static const std::uint64_t kBlockMagic  = 0x314159265359ULL;
	static const std::uint64_t kStreamMagic = 0x177245385090ULL;
	static const int kGroupSize     = 50;
	static const int kMaxGroups     = 6;
	static const int kMaxCodeLength = 20;
	static const int kEncodeLength  = 17;
	static const int kRunA          = 0;
	static const int kRunB          = 1;
	static const int kMaxSelectors  = 18002;

	static std::uint32_t crcTableEntry(std::uint32_t index) {
		std::uint32_t entry = index << 24;
		for (int k = 0; k < 8; ++k) {
			entry = (entry & 0x80000000u) != 0 ? (entry << 1) ^ 0x04C11DB7u : entry << 1;
		}

		return entry;
	}

	static std::uint32_t crcUpdate(std::uint32_t crc, std::uint8_t byte) {
		static std::uint32_t table[256];
		static bool built = false;
		if (!built) {
			for (std::uint32_t k = 0; k < 256; ++k) table[k] = crcTableEntry(k);
			built = true;
		}

		return (crc << 8) ^ table[((crc >> 24) ^ byte) & 0xFF];
	}

	static std::uint32_t blockCrc(const std::uint8_t* data, std::size_t size) {
		std::uint32_t crc = 0xFFFFFFFFu;
		for (std::size_t k = 0; k < size; ++k) crc = crcUpdate(crc, data[k]);
		return ~crc;
	}

	static std::uint32_t combineCrc(std::uint32_t combined, std::uint32_t block) {
		return ((combined << 1) | (combined >> 31)) ^ block;
	}

	static std::uint64_t readMagic(BitReader& reader) {
		const std::uint64_t high = reader.bits(24);
		return (high << 24) | reader.bits(24);
	}

	static bool readSymbolMap(BitReader& reader, BlockState& state) {
		const std::uint32_t ranges = reader.bits(16);
		for (int range = 0; range < 16; ++range) {
			if ((ranges & (0x8000u >> range)) == 0) continue;

			const std::uint32_t used = reader.bits(16);
			for (int k = 0; k < 16; ++k) {
				if ((used & (0x8000u >> k)) != 0) {
					state.symbol_map.push_back(static_cast<std::uint8_t>(range * 16 + k));
				}
			}
		}

		state.alpha_size = static_cast<int>(state.symbol_map.size()) + 2;
		return !state.symbol_map.empty();
	}

	static bool readSelectors(BitReader& reader, int groups, BlockState& state) {
		const std::uint32_t count = reader.bits(15);
		if (count == 0) return false;

		std::uint8_t order[kMaxGroups] = { 0, 1, 2, 3, 4, 5 };
		for (std::uint32_t k = 0; k < count; ++k) {
			int index = 0;
			while (reader.bits(1) == 1) {
				if (++index >= groups || reader.overrun) return false;
			}

			const std::uint8_t chosen = order[index];
			for (int shift = index; shift > 0; --shift) order[shift] = order[shift - 1];
			order[0] = chosen;
			if (k < static_cast<std::uint32_t>(kMaxSelectors)) state.selectors.push_back(chosen);
		}

		return true;
	}

	static bool readCodeLengths(BitReader& reader, int alpha_size,
	                            std::vector<std::uint8_t>& lengths) {
		int current = static_cast<int>(reader.bits(5));
		lengths.assign(static_cast<std::size_t>(alpha_size), 0);
		for (int symbol = 0; symbol < alpha_size; ++symbol) {
			for (;;) {
				if (current < 1 || current > kMaxCodeLength || reader.overrun) return false;
				if (reader.bits(1) == 0) break;
				current += reader.bits(1) == 0 ? 1 : -1;
			}

			lengths[static_cast<std::size_t>(symbol)] = static_cast<std::uint8_t>(current);
		}

		return true;
	}

	static DecodeTable buildDecodeTable(const std::vector<std::uint8_t>& lengths) {
		DecodeTable table;
		table.min_length = *std::min_element(lengths.begin(), lengths.end());
		table.max_length = *std::max_element(lengths.begin(), lengths.end());
		for (int length = table.min_length; length <= table.max_length; ++length) {
			for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
				if (lengths[symbol] != length) continue;
				table.permute.push_back(static_cast<std::uint16_t>(symbol));
			}
		}

		std::int32_t code = 0;
		std::int32_t index = 0;
		for (int length = table.min_length; length <= table.max_length; ++length) {
			const std::int32_t count = static_cast<std::int32_t>(std::count(lengths.begin(),
				lengths.end(), static_cast<std::uint8_t>(length)));
			table.base[length] = index - code;
			table.limit[length] = code + count - 1;
			code = (code + count) << 1;
			index += count;
		}

		return table;
	}

	static bool readTables(BitReader& reader, BlockState& state) {
		const int groups = static_cast<int>(reader.bits(3));
		if (groups < 2 || groups > kMaxGroups) return false;
		if (!readSelectors(reader, groups, state)) return false;

		for (int group = 0; group < groups; ++group) {
			std::vector<std::uint8_t> lengths;
			if (!readCodeLengths(reader, state.alpha_size, lengths)) return false;
			state.tables.push_back(buildDecodeTable(lengths));
		}

		return true;
	}

	static bool decodeSymbol(BitReader& reader, const DecodeTable& table, int& symbol) {
		int length = table.min_length;
		std::int32_t code = static_cast<std::int32_t>(reader.bits(length));
		while (length <= table.max_length && code > table.limit[length]) {
			++length;
			code = (code << 1) | static_cast<std::int32_t>(reader.bits(1));
		}

		if (length > table.max_length || reader.overrun) return false;
		const std::int32_t index = code + table.base[length];
		if (index < 0 || index >= static_cast<std::int32_t>(table.permute.size())) return false;
		symbol = table.permute[static_cast<std::size_t>(index)];
		return true;
	}

	static bool emitRun(std::vector<std::uint32_t>& block, std::uint8_t byte, std::uint32_t run,
	                    std::size_t limit) {
		if (block.size() + run > limit) return false;
		block.insert(block.end(), run, byte);
		return true;
	}

	static bool decodeMtfSymbols(BitReader& reader, const BlockState& state, std::size_t limit,
	                             std::vector<std::uint32_t>& block) {
		std::uint8_t order[256];
		for (int k = 0; k < 256; ++k) order[k] = static_cast<std::uint8_t>(k);

		const int end_of_block = state.alpha_size - 1;
		std::uint32_t run = 0;
		std::uint32_t run_weight = 1;
		for (std::size_t decoded = 0;; ++decoded) {
			const std::size_t group = decoded / kGroupSize;
			if (group >= state.selectors.size()) return false;

			int symbol = 0;
			if (!decodeSymbol(reader, state.tables[state.selectors[group]], symbol)) return false;
			if (symbol == kRunA || symbol == kRunB) {
				run += run_weight << (symbol == kRunB ? 1 : 0);
				run_weight <<= 1;
				if (run > limit) return false;
				continue;
			}

			if (run > 0 && !emitRun(block, state.symbol_map[order[0]], run, limit)) return false;
			run = 0;
			run_weight = 1;
			if (symbol == end_of_block) return true;

			const int index = symbol - 1;
			const std::uint8_t chosen = order[index];
			std::memmove(order + 1, order, static_cast<std::size_t>(index));
			order[0] = chosen;
			if (!emitRun(block, state.symbol_map[chosen], 1, limit)) return false;
		}
	}

	static bool inverseBwt(std::vector<std::uint32_t>& block, std::uint32_t origin,
	                       std::vector<std::uint8_t>& out) {
		if (origin >= block.size()) return false;

		std::uint32_t starts[256] = {};
		for (const std::uint32_t entry : block) ++starts[entry & 0xFF];
		std::uint32_t total = 0;
		for (std::uint32_t& start : starts) {
			const std::uint32_t count = start;
			start = total;
			total += count;
		}

		for (std::size_t k = 0; k < block.size(); ++k) {
			const std::uint8_t byte = static_cast<std::uint8_t>(block[k] & 0xFF);
			block[starts[byte]++] |= static_cast<std::uint32_t>(k) << 8;
		}

		std::uint32_t position = block[origin] >> 8;
		out.reserve(block.size());
		for (std::size_t k = 0; k < block.size(); ++k) {
			position = block[position];
			out.push_back(static_cast<std::uint8_t>(position & 0xFF));
			position >>= 8;
		}

		return true;
	}

	static void undoInitialRle(const std::vector<std::uint8_t>& in,
	                           std::vector<std::uint8_t>& out) {
		int repeats = 0;
		int previous = -1;
		for (std::size_t k = 0; k < in.size(); ++k) {
			const std::uint8_t byte = in[k];
			if (repeats == 4) {
				out.insert(out.end(), byte, static_cast<std::uint8_t>(previous));
				repeats = 0;
				previous = -1;
				continue;
			}

			repeats = byte == previous ? repeats + 1 : 1;
			previous = byte;
			out.push_back(byte);
		}
	}

	static bool decodeBlock(BitReader& reader, std::size_t limit, ByteSink& out,
	                        std::uint32_t& crc, std::string& error) {
		crc = reader.bits(32);
		if (reader.bits(1) != 0) {
			error = "randomized blocks are not supported";
			return false;
		}

		const std::uint32_t origin = reader.bits(24);
		BlockState state;
		std::vector<std::uint32_t> block;
		std::vector<std::uint8_t> transformed;
		if (!readSymbolMap(reader, state) || !readTables(reader, state)
		    || !decodeMtfSymbols(reader, state, limit, block)
		    || !inverseBwt(block, origin, transformed)) {
			error = "data is corrupt";
			return false;
		}

		std::vector<std::uint8_t> plain;
		undoInitialRle(transformed, plain);
		if (blockCrc(plain.data(), plain.size()) != crc) {
			error = "block checksum mismatch";
			return false;
		}

		return out.write(plain.data(), plain.size());
	}

	static bool decodeStream(BitReader& reader, ByteSink& out, std::string& error) {
		if (reader.bits(8) != 'B' || reader.bits(8) != 'Z' || reader.bits(8) != 'h') {
			error = "not in bzip2 format";
			return false;
		}

		const std::uint32_t level = reader.bits(8);
		if (level < '1' || level > '9') {
			error = "not in bzip2 format";
			return false;
		}

		const std::size_t limit = static_cast<std::size_t>(level - '0') * 100000;
		std::uint32_t combined = 0;
		for (;;) {
			const std::uint64_t magic = readMagic(reader);
			if (magic == kStreamMagic) break;
			if (magic != kBlockMagic || reader.overrun) {
				error = "data is corrupt";
				return false;
			}

			std::uint32_t crc = 0;
			if (!decodeBlock(reader, limit, out, crc, error)) return false;
			combined = combineCrc(combined, crc);
		}

		if (reader.bits(32) == combined) return true;
		error = "stream checksum mismatch";
		return false;
	}

	static bool anotherStreamFollows(InputBuffer& in) {
		const std::uint8_t* next = nullptr;
		return in.peek(next, 3) == 3 && std::memcmp(next, "BZh", 3) == 0;
	}

	bool bunzip2Stream(InputBuffer& in, ByteSink& out, std::string& error) {
		BitReader reader;
		reader.in = &in;
		do {
			if (!decodeStream(reader, out, error)) return false;
			reader.count = 0;
			reader.buffer = 0;
		} while (anotherStreamFollows(in));

		return true;
	}

	bool bunzip2Data(const std::uint8_t* in, std::size_t size, std::vector<std::uint8_t>& out,
	                 std::string& error) {
		MemorySource source(in, size);
		InputBuffer input(source);
		VectorSink sink(out);
		return bunzip2Stream(input, sink, error);
	}

	static std::size_t appendRun(std::vector<std::uint8_t>& block, std::uint8_t byte,
	                             std::size_t run) {
		const std::size_t literal = (std::min)(run, static_cast<std::size_t>(4));
		block.insert(block.end(), literal, byte);
		if (run >= 4) block.push_back(static_cast<std::uint8_t>(run - 4));
		return run;
	}

	static void countingSortByClass(const std::vector<int>& classes, int class_count,
	                                const std::vector<int>& order, std::vector<int>& sorted) {
		std::vector<int> starts(static_cast<std::size_t>(class_count) + 1, 0);
		for (const int index : order) ++starts[static_cast<std::size_t>(classes[index]) + 1];
		for (std::size_t k = 1; k < starts.size(); ++k) starts[k] += starts[k - 1];
		for (const int index : order) sorted[starts[classes[index]]++] = index;
	}

	static int reclassify(const std::vector<int>& sorted, const std::vector<int>& classes,
	                      int shift, std::vector<int>& next) {
		const int n = static_cast<int>(sorted.size());
		int count = 1;
		next[sorted[0]] = 0;
		for (int k = 1; k < n; ++k) {
			const int current = sorted[k];
			const int previous = sorted[k - 1];
			const bool differs = classes[current] != classes[previous]
				|| classes[(current + shift) % n] != classes[(previous + shift) % n];
			if (differs) ++count;
			next[current] = count - 1;
		}

		return count;
	}

	// Sorted cyclic rotations by prefix doubling: O(n log n), and simple
	// enough that its correctness is easy to see.
	static std::vector<int> sortRotations(const std::vector<std::uint8_t>& block) {
		const int n = static_cast<int>(block.size());
		std::vector<int> classes(block.begin(), block.end());
		std::vector<int> order(static_cast<std::size_t>(n));
		for (int k = 0; k < n; ++k) order[k] = k;

		std::vector<int> sorted(static_cast<std::size_t>(n));
		countingSortByClass(classes, 256, order, sorted);
		std::vector<int> next(static_cast<std::size_t>(n));
		int class_count = reclassify(sorted, classes, 0, next);
		classes.swap(next);

		for (int shift = 1; shift < n && class_count < n; shift <<= 1) {
			for (int k = 0; k < n; ++k) order[k] = (sorted[k] - shift + n) % n;
			countingSortByClass(classes, class_count, order, sorted);
			class_count = reclassify(sorted, classes, shift, next);
			classes.swap(next);
		}

		return sorted;
	}

	static void appendZeroRun(std::uint32_t run, std::vector<std::uint16_t>& symbols) {
		std::uint32_t pending = run - 1;
		for (;;) {
			symbols.push_back(static_cast<std::uint16_t>((pending & 1) != 0 ? kRunB : kRunA));
			if (pending < 2) return;
			pending = (pending - 2) / 2;
		}
	}

	static std::vector<std::uint16_t> mtfEncode(const std::vector<std::uint8_t>& last_column,
	                                            const std::vector<std::uint8_t>& used,
	                                            int& alpha_size) {
		std::uint8_t to_sequence[256] = {};
		std::vector<std::uint8_t> order;
		for (std::size_t k = 0; k < used.size(); ++k) {
			to_sequence[used[k]] = static_cast<std::uint8_t>(k);
			order.push_back(static_cast<std::uint8_t>(k));
		}

		alpha_size = static_cast<int>(used.size()) + 2;
		std::vector<std::uint16_t> symbols;
		std::uint32_t zeros = 0;
		for (const std::uint8_t byte : last_column) {
			const std::uint8_t sequence = to_sequence[byte];
			const std::size_t index = static_cast<std::size_t>(
				std::find(order.begin(), order.end(), sequence) - order.begin());
			if (index == 0) {
				++zeros;
				continue;
			}

			if (zeros > 0) appendZeroRun(zeros, symbols);
			zeros = 0;
			order.erase(order.begin() + static_cast<std::ptrdiff_t>(index));
			order.insert(order.begin(), sequence);
			symbols.push_back(static_cast<std::uint16_t>(index + 1));
		}

		if (zeros > 0) appendZeroRun(zeros, symbols);
		symbols.push_back(static_cast<std::uint16_t>(alpha_size - 1));
		return symbols;
	}

	static int groupCountFor(std::size_t symbols) {
		if (symbols < 200)  return 2;
		if (symbols < 600)  return 3;
		if (symbols < 1200) return 4;
		if (symbols < 2400) return 5;
		return 6;
	}

	static void seedTables(const std::vector<std::uint16_t>& symbols, int alpha_size,
	                       EncodedTables& tables) {
		std::vector<std::uint32_t> frequency(static_cast<std::size_t>(alpha_size), 0);
		for (const std::uint16_t symbol : symbols) ++frequency[symbol];

		std::size_t remaining = symbols.size();
		int start = 0;
		for (int group = tables.group_count; group > 0; --group) {
			const std::size_t target = remaining / static_cast<std::size_t>(group);
			std::size_t taken = 0;
			int end = start - 1;
			while (taken < target && end < alpha_size - 1) taken += frequency[++end];

			std::vector<std::uint8_t> lengths(static_cast<std::size_t>(alpha_size), 15);
			for (int k = start; k <= end; ++k) lengths[k] = 0;
			tables.lengths[tables.group_count - group] = lengths;
			start = end + 1;
			remaining -= taken;
		}
	}

	static std::size_t groupCost(const std::vector<std::uint16_t>& symbols, std::size_t from,
	                             std::size_t to, const std::vector<std::uint8_t>& lengths) {
		std::size_t cost = 0;
		for (std::size_t k = from; k < to; ++k) cost += lengths[symbols[k]];
		return cost;
	}

	static std::uint8_t cheapestTable(const std::vector<std::uint16_t>& symbols, std::size_t from,
	                                  std::size_t to, const EncodedTables& tables) {
		std::uint8_t best = 0;
		std::size_t best_cost = ~static_cast<std::size_t>(0);
		for (int group = 0; group < tables.group_count; ++group) {
			const std::size_t cost = groupCost(symbols, from, to, tables.lengths[group]);
			if (cost >= best_cost) continue;
			best = static_cast<std::uint8_t>(group);
			best_cost = cost;
		}

		return best;
	}

	static void refineTables(const std::vector<std::uint16_t>& symbols, int alpha_size,
	                         EncodedTables& tables) {
		const std::size_t groups = static_cast<std::size_t>(tables.group_count);
		std::vector<std::vector<std::uint32_t>> frequency(groups,
			std::vector<std::uint32_t>(static_cast<std::size_t>(alpha_size), 0));
		tables.selectors.clear();
		for (std::size_t from = 0; from < symbols.size(); from += kGroupSize) {
			const std::size_t to = (std::min)(from + kGroupSize, symbols.size());
			const std::uint8_t table = cheapestTable(symbols, from, to, tables);
			tables.selectors.push_back(table);
			for (std::size_t k = from; k < to; ++k) ++frequency[table][symbols[k]];
		}

		for (int group = 0; group < tables.group_count; ++group) {
			for (std::uint32_t& count : frequency[group]) ++count;
			tables.lengths[group] = limitedCodeLengths(frequency[group], kEncodeLength);
		}
	}

	static EncodedTables buildTables(const std::vector<std::uint16_t>& symbols, int alpha_size) {
		EncodedTables tables;
		tables.group_count = groupCountFor(symbols.size());
		tables.lengths.resize(static_cast<std::size_t>(tables.group_count));
		seedTables(symbols, alpha_size, tables);
		for (int pass = 0; pass < 4; ++pass) refineTables(symbols, alpha_size, tables);

		for (const std::vector<std::uint8_t>& lengths : tables.lengths) {
			tables.codes.push_back(canonicalCodes(lengths));
		}

		return tables;
	}

	static void writeSymbolMap(BitWriter& writer, const std::vector<std::uint8_t>& used) {
		bool present[256] = {};
		for (const std::uint8_t byte : used) present[byte] = true;

		std::uint32_t ranges = 0;
		for (int range = 0; range < 16; ++range) {
			for (int k = 0; k < 16; ++k) {
				if (present[range * 16 + k]) ranges |= 0x8000u >> range;
			}
		}

		writer.put(ranges, 16);
		for (int range = 0; range < 16; ++range) {
			if ((ranges & (0x8000u >> range)) == 0) continue;
			std::uint32_t bits = 0;
			for (int k = 0; k < 16; ++k) {
				if (present[range * 16 + k]) bits |= 0x8000u >> k;
			}

			writer.put(bits, 16);
		}
	}

	static void writeSelectors(BitWriter& writer, const EncodedTables& tables) {
		writer.put(static_cast<std::uint32_t>(tables.group_count), 3);
		writer.put(static_cast<std::uint32_t>(tables.selectors.size()), 15);

		std::uint8_t order[kMaxGroups] = { 0, 1, 2, 3, 4, 5 };
		for (const std::uint8_t selector : tables.selectors) {
			int index = 0;
			while (order[index] != selector) ++index;
			for (int k = 0; k < index; ++k) writer.put(1, 1);
			writer.put(0, 1);

			for (int shift = index; shift > 0; --shift) order[shift] = order[shift - 1];
			order[0] = selector;
		}
	}

	static void writeCodeLengths(BitWriter& writer, const std::vector<std::uint8_t>& lengths) {
		int current = lengths[0];
		writer.put(static_cast<std::uint32_t>(current), 5);
		for (const std::uint8_t length : lengths) {
			while (current < length) {
				writer.put(2, 2);
				++current;
			}

			while (current > length) {
				writer.put(3, 2);
				--current;
			}

			writer.put(0, 1);
		}
	}

	static void writeSymbols(BitWriter& writer, const std::vector<std::uint16_t>& symbols,
	                         const EncodedTables& tables) {
		for (std::size_t k = 0; k < symbols.size(); ++k) {
			const std::uint8_t table = tables.selectors[k / kGroupSize];
			const std::uint16_t symbol = symbols[k];
			writer.put(tables.codes[table][symbol], tables.lengths[table][symbol]);
		}
	}

	static std::vector<std::uint8_t> usedBytes(const std::vector<std::uint8_t>& block) {
		bool present[256] = {};
		for (const std::uint8_t byte : block) present[byte] = true;

		std::vector<std::uint8_t> used;
		for (int byte = 0; byte < 256; ++byte) {
			if (present[byte]) used.push_back(static_cast<std::uint8_t>(byte));
		}

		return used;
	}

	static void writeBlock(BitWriter& writer, const std::vector<std::uint8_t>& block,
	                       std::uint32_t crc) {
		const std::vector<int> rotations = sortRotations(block);
		const std::size_t n = block.size();
		std::vector<std::uint8_t> last_column(n);
		std::uint32_t origin = 0;
		for (std::size_t k = 0; k < n; ++k) {
			const std::size_t start = static_cast<std::size_t>(rotations[k]);
			if (start == 0) origin = static_cast<std::uint32_t>(k);
			last_column[k] = block[(start + n - 1) % n];
		}

		const std::vector<std::uint8_t> used = usedBytes(block);
		int alpha_size = 0;
		const std::vector<std::uint16_t> symbols = mtfEncode(last_column, used, alpha_size);
		const EncodedTables tables = buildTables(symbols, alpha_size);

		writer.put(static_cast<std::uint32_t>(kBlockMagic >> 24), 24);
		writer.put(static_cast<std::uint32_t>(kBlockMagic & 0xFFFFFF), 24);
		writer.put(crc, 32);
		writer.put(0, 1);
		writer.put(origin, 24);
		writeSymbolMap(writer, used);
		writeSelectors(writer, tables);
		for (const std::vector<std::uint8_t>& lengths : tables.lengths) {
			writeCodeLengths(writer, lengths);
		}
		writeSymbols(writer, symbols, tables);
	}

	// The initial run-length encoding happens as bytes arrive, so only the
	// block being filled is ever held: four equal bytes are followed by a
	// count (0-251) of further copies.
	struct Bzip2Encoder::State {
		std::vector<std::uint8_t> bytes;
		BitWriter writer;
		std::vector<std::uint8_t> block;
		std::size_t capacity = 0;
		std::uint32_t block_crc = 0xFFFFFFFFu;
		std::uint32_t combined = 0;
		std::uint8_t run_byte = 0;
		std::size_t run_length = 0;
	};

	Bzip2Encoder::Bzip2Encoder(ByteSink& out, int level) : out_(out), state_(std::make_unique<State>()) {
		level = (std::min)((std::max)(level, 1), 9);
		state_->writer.out = &state_->bytes;
		state_->capacity = static_cast<std::size_t>(level) * 100000 - 19;
		state_->writer.put('B', 8);
		state_->writer.put('Z', 8);
		state_->writer.put('h', 8);
		state_->writer.put(static_cast<std::uint32_t>('0' + level), 8);
	}

	Bzip2Encoder::~Bzip2Encoder() = default;

	bool Bzip2Encoder::drainBytes() {
		std::vector<std::uint8_t>& bytes = state_->bytes;
		const bool ok = bytes.empty() || out_.write(bytes.data(), bytes.size());
		bytes.clear();
		return ok;
	}

	bool Bzip2Encoder::emitBlock() {
		State& state = *state_;
		if (state.block.empty()) return true;

		const std::uint32_t crc = ~state.block_crc;
		writeBlock(state.writer, state.block, crc);
		state.combined = combineCrc(state.combined, crc);
		state.block.clear();
		state.block_crc = 0xFFFFFFFFu;
		return drainBytes();
	}

	// A run that no longer fits closes the block, exactly where a one-shot
	// fill of the whole input would have closed it.
	bool Bzip2Encoder::flushRun() {
		State& state = *state_;
		if (state.run_length == 0) return true;

		const std::size_t encoded = state.run_length >= 4 ? 5 : state.run_length;
		if (state.block.size() + encoded > state.capacity && !emitBlock()) return false;

		appendRun(state.block, state.run_byte, state.run_length);
		for (std::size_t k = 0; k < state.run_length; ++k) state.block_crc = crcUpdate(state.block_crc, state.run_byte);
		state.run_length = 0;
		return true;
	}

	bool Bzip2Encoder::write(const std::uint8_t* data, std::size_t size) {
		State& state = *state_;
		for (std::size_t k = 0; k < size; ++k) {
			if (state.run_length > 0 && data[k] == state.run_byte && state.run_length < 255) {
				++state.run_length;
				continue;
			}

			if (!flushRun()) return false;
			state.run_byte = data[k];
			state.run_length = 1;
		}

		return true;
	}

	bool Bzip2Encoder::finish() {
		State& state = *state_;
		if (!flushRun() || !emitBlock()) return false;

		state.writer.put(static_cast<std::uint32_t>(kStreamMagic >> 24), 24);
		state.writer.put(static_cast<std::uint32_t>(kStreamMagic & 0xFFFFFF), 24);
		state.writer.put(state.combined, 32);
		state.writer.flush();
		return drainBytes();
	}

	void bzip2Data(const std::uint8_t* data, std::size_t size, int level,
	               std::vector<std::uint8_t>& out) {
		VectorSink sink(out);
		Bzip2Encoder encoder(sink, level);
		encoder.write(data, size);
		encoder.finish();
	}

}  // namespace wbsh

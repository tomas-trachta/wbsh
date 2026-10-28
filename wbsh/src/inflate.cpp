/**
 * @file inflate.cpp
 * @brief Puff-style DEFLATE decoder: bit reader, canonical Huffman tables, the three block kinds.
 */

#include "inflate.h"

#include <cstring>

#include "bytestream.h"

namespace wbsh {

	static const int kMaxBits          = 15;
	static const int kMaxLengthCodes   = 286;
	static const int kMaxDistCodes     = 30;
	static const int kMaxCodes         = kMaxLengthCodes + kMaxDistCodes;
	static const int kFixedLengthCodes = 288;
	static const int kCodeLengthCodes  = 19;
	static const int kEndOfBlock       = 256;
	static const int kFirstLengthCode  = 257;
	static const int kLengthSymbols    = 29;
	static const int kDistSymbols      = 30;

	static const short kLengthBase[kLengthSymbols] = {
		3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
		67, 83, 99, 115, 131, 163, 195, 227, 258 };
	static const short kLengthExtra[kLengthSymbols] = {
		0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
		4, 4, 4, 4, 5, 5, 5, 5, 0 };
	static const short kDistBase[kDistSymbols] = {
		1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
		1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
	static const short kDistExtra[kDistSymbols] = {
		0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
		9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
	static const short kCodeLengthOrder[kCodeLengthCodes] = {
		16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

	namespace inflate_detail {
		struct BitStream {
			InputBuffer& in;
			int bit_buffer = 0;
			int bit_count = 0;
			LzWindow& out;
			bool failed = false;

			BitStream(InputBuffer& input, LzWindow& window) : in(input), out(window) {}

			int bits(int need) {
				long value = bit_buffer;
				while (bit_count < need) {
					const int byte = in.get();
					if (byte < 0) {
						failed = true;
						return 0;
					}

					value |= static_cast<long>(byte) << bit_count;
					bit_count += 8;
				}

				bit_buffer = static_cast<int>(value >> need);
				bit_count -= need;
				return static_cast<int>(value) & ((1 << need) - 1);
			}
		};

		struct Huffman {
			short count[kMaxBits + 1] {};
			short symbol[kMaxCodes] {};
		};
	}  // namespace inflate_detail

	using inflate_detail::BitStream;
	using inflate_detail::Huffman;

	static int decodeSymbol(BitStream& stream, const Huffman& table) {
		int code = 0;
		int first = 0;
		int index = 0;
		for (int len = 1; len <= kMaxBits; ++len) {
			code |= stream.bits(1);
			const int count = table.count[len];
			if (code - count < first) return table.symbol[index + (code - first)];

			index += count;
			first += count;
			first <<= 1;
			code <<= 1;
		}

		stream.failed = true;
		return -1;
	}

	// Returns the number of unused codes: 0 for a complete code, positive
	// for an incomplete one, negative when over-subscribed.
	static int countUnusedCodes(const Huffman& table) {
		int left = 1;
		for (int len = 1; len <= kMaxBits; ++len) {
			left <<= 1;
			left -= table.count[len];
			if (left < 0) return left;
		}

		return left;
	}

	static void placeSymbols(Huffman& table, const short* lengths, int symbol_count) {
		short offsets[kMaxBits + 1];
		offsets[1] = 0;
		for (int len = 1; len < kMaxBits; ++len) offsets[len + 1] = offsets[len] + table.count[len];

		for (int sym = 0; sym < symbol_count; ++sym) {
			if (lengths[sym] != 0) table.symbol[offsets[lengths[sym]]++] = static_cast<short>(sym);
		}
	}

	static int buildHuffman(Huffman& table, const short* lengths, int symbol_count) {
		for (int len = 0; len <= kMaxBits; ++len) table.count[len] = 0;
		for (int sym = 0; sym < symbol_count; ++sym) table.count[lengths[sym]]++;
		if (table.count[0] == symbol_count) return 0;

		const int left = countUnusedCodes(table);
		if (left < 0) return left;

		placeSymbols(table, lengths, symbol_count);
		return left;
	}

	static bool copyBackReference(BitStream& stream, int length_symbol, const Huffman& dist_table) {
		if (length_symbol >= kLengthSymbols) return false;
		const int length = kLengthBase[length_symbol] + stream.bits(kLengthExtra[length_symbol]);

		const int dist_symbol = decodeSymbol(stream, dist_table);
		if (stream.failed || dist_symbol >= kDistSymbols) return false;
		const int distance = kDistBase[dist_symbol] + stream.bits(kDistExtra[dist_symbol]);
		if (stream.failed) return false;
		return stream.out.copy(static_cast<std::size_t>(distance), static_cast<std::size_t>(length));
	}

	static int decodeBlockCodes(BitStream& stream, const Huffman& length_table,
			const Huffman& dist_table) {
		while (true) {
			const int symbol = decodeSymbol(stream, length_table);
			if (stream.failed || !stream.out.ok()) return -1;
			if (symbol == kEndOfBlock) return 0;
			if (symbol < kEndOfBlock) {
				stream.out.put(static_cast<std::uint8_t>(symbol));
				continue;
			}

			if (!copyBackReference(stream, symbol - kFirstLengthCode, dist_table)) return -1;
		}
	}

	static int inflateStoredBlock(BitStream& stream) {
		stream.bit_buffer = 0;
		stream.bit_count = 0;

		std::uint8_t header[4];
		if (!stream.in.readExact(header, sizeof(header))) return -1;

		const int length = header[0] | (header[1] << 8);
		const int complement = header[2] | (header[3] << 8);
		if ((length & 0xFFFF) != ((~complement) & 0xFFFF)) return -1;

		for (int k = 0; k < length; ++k) {
			const int byte = stream.in.get();
			if (byte < 0) return -1;
			stream.out.put(static_cast<std::uint8_t>(byte));
		}

		return stream.out.ok() ? 0 : -1;
	}

	static void buildFixedTables(Huffman& length_table, Huffman& dist_table) {
		short lengths[kFixedLengthCodes];
		int sym = 0;
		for (; sym < 144; ++sym) lengths[sym] = 8;
		for (; sym < 256; ++sym) lengths[sym] = 9;
		for (; sym < 280; ++sym) lengths[sym] = 7;
		for (; sym < kFixedLengthCodes; ++sym) lengths[sym] = 8;
		buildHuffman(length_table, lengths, kFixedLengthCodes);

		for (sym = 0; sym < kMaxDistCodes; ++sym) lengths[sym] = 5;
		buildHuffman(dist_table, lengths, kMaxDistCodes);
	}

	static int inflateFixedBlock(BitStream& stream) {
		static Huffman length_table;
		static Huffman dist_table;
		static bool built = false;
		if (!built) {
			buildFixedTables(length_table, dist_table);
			built = true;
		}

		return decodeBlockCodes(stream, length_table, dist_table);
	}

	static bool repeatLength(short* lengths, int& index, int total, short value, int times) {
		if (index + times > total) return false;
		for (int k = 0; k < times; ++k) lengths[index++] = value;
		return true;
	}

	static bool readDynamicLengths(BitStream& stream, const Huffman& code_table,
			short* lengths, int total) {
		int index = 0;
		while (index < total) {
			const int symbol = decodeSymbol(stream, code_table);
			if (stream.failed) return false;

			if (symbol < 16) {
				lengths[index++] = static_cast<short>(symbol);
				continue;
			}

			if (symbol == 16 && index == 0) return false;

			const short value = symbol == 16 ? lengths[index - 1] : 0;
			const int extra_bits = symbol == 16 ? 2 : (symbol == 17 ? 3 : 7);
			const int base = symbol == 18 ? 11 : 3;
			if (!repeatLength(lengths, index, total, value, stream.bits(extra_bits) + base)) {
				return false;
			}
		}

		return true;
	}

	// RFC 1951 allows exactly one incomplete code: a single symbol of
	// length one (as puff puts it, "incomplete code ok only for single
	// length 1 code").
	static bool acceptableCode(const Huffman& table, int unused, int symbol_count) {
		if (unused == 0) return true;
		if (unused < 0) return false;
		return symbol_count == table.count[0] + table.count[1];
	}

	static int inflateDynamicBlock(BitStream& stream) {
		const int length_count = stream.bits(5) + kFirstLengthCode;
		const int dist_count = stream.bits(5) + 1;
		const int code_count = stream.bits(4) + 4;
		if (length_count > kMaxLengthCodes || dist_count > kMaxDistCodes) return -1;

		short lengths[kMaxCodes] = { 0 };
		for (int i = 0; i < code_count; ++i) {
			lengths[kCodeLengthOrder[i]] = static_cast<short>(stream.bits(3));
		}

		Huffman code_table;
		if (buildHuffman(code_table, lengths, kCodeLengthCodes) != 0) return -1;
		if (!readDynamicLengths(stream, code_table, lengths, length_count + dist_count)) return -1;
		if (lengths[kEndOfBlock] == 0) return -1;

		Huffman length_table;
		const int length_unused = buildHuffman(length_table, lengths, length_count);
		if (!acceptableCode(length_table, length_unused, length_count)) return -1;

		Huffman dist_table;
		const int dist_unused = buildHuffman(dist_table, lengths + length_count, dist_count);
		if (!acceptableCode(dist_table, dist_unused, dist_count)) return -1;

		return decodeBlockCodes(stream, length_table, dist_table);
	}

	static int inflateBlock(BitStream& stream, int type) {
		switch (type) {
		case 0:  return inflateStoredBlock(stream);
		case 1:  return inflateFixedBlock(stream);
		case 2:  return inflateDynamicBlock(stream);
		default: return -1;
		}
	}

	bool inflateStream(InputBuffer& in, LzWindow& out) {
		BitStream stream(in, out);
		bool last = false;
		while (!last) {
			last = stream.bits(1) != 0;
			const int type = stream.bits(2);
			if (stream.failed) return false;
			if (inflateBlock(stream, type) != 0 || stream.failed) return false;
		}

		return out.flush();
	}

	bool inflateRawConsumed(const std::uint8_t* in, std::size_t in_len,
	                        std::vector<std::uint8_t>& out, std::size_t& consumed) {
		MemorySource source(in, in_len);
		InputBuffer input(source);
		VectorSink sink(out);
		LzWindow window(sink, kDeflateWindow);
		if (!inflateStream(input, window)) return false;

		consumed += static_cast<std::size_t>(input.consumed());
		return true;
	}

	bool inflateRaw(const std::uint8_t* in, std::size_t in_len, std::vector<std::uint8_t>& out) {
		std::size_t consumed = 0;
		return inflateRawConsumed(in, in_len, out, consumed);
	}

}  // namespace wbsh

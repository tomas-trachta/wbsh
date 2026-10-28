/**
 * @file lzw.cpp
 * @brief .Z decoding. compress reads its codes in groups of eight; when
 *        the code width grows or a CLEAR code arrives, whatever is left
 *        of the current group is padding and gets skipped, which is the
 *        detail every reimplementation has to reproduce.
 */

#include "lzw.h"

#include <algorithm>

#include "bytestream.h"

namespace wbsh {

	namespace lzw_detail {

		// Eight codes of `width` bits fill exactly `width` bytes, so the input
		// is read one group at a time.
		struct CodeReader {
			InputBuffer* in = nullptr;
			std::uint8_t group[16] = {};
			std::size_t bit_count = 0;
			std::size_t position = 0;
			int width = 9;
			std::size_t codes_in_group = 0;

			bool loadGroup() {
				bit_count = 0;
				position = 0;
				while (bit_count < static_cast<std::size_t>(width) * 8) {
					const int byte = in->get();
					if (byte < 0) break;
					group[bit_count / 8] = static_cast<std::uint8_t>(byte);
					bit_count += 8;
				}

				return bit_count != 0;
			}

			bool next(std::uint32_t& code) {
				if (codes_in_group == 0 && !loadGroup()) return false;
				if (position + static_cast<std::size_t>(width) > bit_count) return false;

				const std::size_t byte = position >> 3;
				std::uint32_t window = group[byte];
				for (std::size_t extra = 1; extra <= 2 && (byte + extra) * 8 < bit_count; ++extra) {
					window |= static_cast<std::uint32_t>(group[byte + extra]) << (8 * extra);
				}

				code = (window >> (position & 7)) & ((1u << width) - 1);
				position += static_cast<std::size_t>(width);
				codes_in_group = (codes_in_group + 1) % 8;
				return true;
			}

			void skipRestOfGroup() {
				codes_in_group = 0;
			}
		};

		struct Dictionary {
			std::vector<std::uint16_t> prefix;
			std::vector<std::uint8_t> suffix;
			std::uint32_t next_free = 256;
			std::uint32_t limit = 0;
		};

	}  // namespace lzw_detail

	using lzw_detail::CodeReader;
	using lzw_detail::Dictionary;

	static const std::uint32_t kClearCode = 256;
	static const int kInitialWidth = 9;

	static std::uint32_t widthLimit(int width, int max_width, std::uint32_t max_code) {
		return width == max_width ? max_code : (1u << width) - 1;
	}

	static bool expandCode(const Dictionary& dictionary, std::uint32_t code, std::uint32_t previous,
	                       std::uint8_t& first, std::vector<std::uint8_t>& stack) {
		stack.clear();
		if (code >= dictionary.next_free) {
			if (code > dictionary.next_free) return false;
			stack.push_back(first);
			code = previous;
		}

		while (code >= 256) {
			if (code >= dictionary.next_free) return false;
			stack.push_back(dictionary.suffix[code]);
			code = dictionary.prefix[code];
		}

		first = static_cast<std::uint8_t>(code);
		stack.push_back(first);
		return true;
	}

	static std::uint32_t initialLimit() {
		return (1u << kInitialWidth) - 1;
	}

	static bool readHeader(InputBuffer& in, int& max_width, bool& block_mode, std::string& error) {
		std::uint8_t header[3];
		if (!in.readExact(header, sizeof(header)) || header[0] != 0x1F || header[1] != 0x9D) {
			error = "not in compress format";
			return false;
		}

		max_width = header[2] & 0x1F;
		block_mode = (header[2] & 0x80) != 0;
		if (max_width >= kInitialWidth && max_width <= 16) return true;

		error = "unsupported code width";
		return false;
	}

	// Like compress and gzip's unlzw, the first limit is 511 whatever the
	// maximum width, so a -b9 file still steps up to 10-bit codes.
	static void growWidthIfFull(CodeReader& reader, Dictionary& dictionary, int max_width,
	                            std::uint32_t max_code) {
		if (dictionary.next_free <= dictionary.limit) return;

		reader.skipRestOfGroup();
		++reader.width;
		dictionary.limit = widthLimit(reader.width, max_width, max_code);
	}

	static void clearDictionary(CodeReader& reader, Dictionary& dictionary) {
		reader.skipRestOfGroup();
		reader.width = kInitialWidth;
		dictionary.limit = initialLimit();
		dictionary.next_free = 256;
	}

	static void addEntry(Dictionary& dictionary, std::uint32_t previous, std::uint8_t first,
	                     std::uint32_t max_code) {
		if (dictionary.next_free >= max_code) return;
		dictionary.prefix[dictionary.next_free] = static_cast<std::uint16_t>(previous);
		dictionary.suffix[dictionary.next_free] = first;
		++dictionary.next_free;
	}

	static const std::size_t kOutputBatch = 64 * 1024;

	static bool emitString(const std::vector<std::uint8_t>& stack, std::vector<std::uint8_t>& batch,
	                       ByteSink& out) {
		batch.insert(batch.end(), stack.rbegin(), stack.rend());
		if (batch.size() < kOutputBatch) return true;

		const bool ok = out.write(batch.data(), batch.size());
		batch.clear();
		return ok;
	}

	static bool decodeCodes(CodeReader& reader, Dictionary& dictionary, int max_width, bool block_mode,
	                        ByteSink& out, std::string& error) {
		const std::uint32_t max_code = 1u << max_width;
		std::uint32_t previous = 0;
		if (!reader.next(previous)) return true;
		if (previous >= 256) {
			error = "data is corrupt";
			return false;
		}

		std::uint8_t first = static_cast<std::uint8_t>(previous);
		std::vector<std::uint8_t> batch(1, first);
		std::vector<std::uint8_t> stack;
		for (std::uint32_t code = 0;;) {
			growWidthIfFull(reader, dictionary, max_width, max_code);
			if (!reader.next(code)) return batch.empty() || out.write(batch.data(), batch.size());
			if (code == kClearCode && block_mode) {
				clearDictionary(reader, dictionary);
				continue;
			}

			if (!expandCode(dictionary, code, previous, first, stack)) {
				error = "data is corrupt";
				return false;
			}

			if (!emitString(stack, batch, out)) return false;
			addEntry(dictionary, previous, first, max_code);
			previous = code;
		}
	}

	bool uncompressStream(InputBuffer& in, ByteSink& out, std::string& error) {
		int max_width = 0;
		bool block_mode = false;
		if (!readHeader(in, max_width, block_mode, error)) return false;

		const std::uint32_t max_code = 1u << max_width;
		Dictionary dictionary;
		dictionary.prefix.assign(max_code, 0);
		dictionary.suffix.assign(max_code, 0);
		dictionary.next_free = block_mode ? 257 : 256;
		dictionary.limit = initialLimit();

		CodeReader reader;
		reader.in = &in;
		return decodeCodes(reader, dictionary, max_width, block_mode, out, error);
	}

	bool uncompressData(const std::uint8_t* in, std::size_t size, std::vector<std::uint8_t>& out,
	                    std::string& error) {
		MemorySource source(in, size);
		InputBuffer input(source);
		VectorSink sink(out);
		return uncompressStream(input, sink, error);
	}

}  // namespace wbsh

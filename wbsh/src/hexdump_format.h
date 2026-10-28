#pragma once

/**
 * @file hexdump_format.h
 * @brief The hexdump format language of BSD and util-linux: `-e` / `-f`
 *        format strings parsed into a program that renders one input block
 *        at a time.
 *
 * A format string is a list of units, `[COUNT][/BYTES] "FORMAT"`. Each
 * format string consumes the block from its start; the block is as long as
 * the longest of them, and a last unit without an iteration count repeats
 * until it fills the block.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wbsh {

	enum class HexdumpConversion {
		Text,
		Address,
		Character,
		Escaped,
		Printable,
		Named,
		Signed,
		Unsigned,
		Float,
		String,
	};

	/// One conversion with the literal text printed in front of it; text after
	/// a unit's last conversion is a Text print of its own.
	struct HexdumpPrint {
		HexdumpConversion conversion = HexdumpConversion::Text;
		std::string text;
		std::string spec;
		char letter = 0;
		std::size_t byte_count = 0;
		std::size_t width = 0;
	};

	struct HexdumpUnit {
		std::size_t repeat = 1;
		bool repeat_given = false;
		std::size_t byte_count = 0;
		bool byte_count_given = false;
		bool end_only = false;
		bool trims_last_round = false;
		std::vector<HexdumpPrint> prints;
	};

	struct HexdumpFormat {
		std::vector<HexdumpUnit> units;
		std::size_t byte_count = 0;
	};

	struct HexdumpProgram {
		std::vector<HexdumpFormat> formats;
		std::size_t block_size = 0;
	};

	/// A block of block_size bytes. A short final block is zero-padded and
	/// marked final; conversions at or past data_end then print as blanks.
	struct HexdumpBlock {
		const unsigned char* data = nullptr;
		std::uint64_t address = 0;
		std::uint64_t data_end = 0;
		bool final = false;
	};

	/// Adds one format string (one `-e` argument or one `-f` line); false with
	/// util-linux's wording in @p error when it does not parse.
	bool addHexdumpFormat(HexdumpProgram& program, const std::string& text, std::string& error);

	/// Settles the block size once every format string has been added.
	void finishHexdumpProgram(HexdumpProgram& program);

	void renderHexdumpBlock(const HexdumpProgram& program, const HexdumpBlock& block,
	                        std::string& out);

	/// The `%_A` units: printed once, after the last block, with the end offset.
	void renderHexdumpEnd(const HexdumpProgram& program, std::uint64_t end_address,
	                      std::string& out);

}  // namespace wbsh
